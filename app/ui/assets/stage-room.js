/* Headroom — Room view: your rig, set up in a room the way you'd play it  (stage-room.js → #view-room, tab "Room")
 *
 *   The current rig laid out physically: stompboxes on a pedalboard on the floor (in chain order, patched together),
 *   wahs at the front of the board, amps standing on their cabinets behind it (combos on a tilt-back stand, two amps
 *   side by side for a parallel split), studio processors in a rack case, instruments as keyboards on a stand, and
 *   cables running between them. Click any piece of gear and it lifts up toward you as the real illustrated face
 *   with working controls; Esc or a click on the room puts it back.
 *
 *   What the room does that the Signal Chain can't:
 *     • Scenes — pick where you're playing (studio, living room, concert stage, outdoors). Purely visual: the rig and
 *       the engine are never touched by a scene.
 *     • Cab & mics — on a Cabinet unit (900100) pick the cabinet, drag mic A / B onto cap / cap edge / cone, slide mic B
 *       back (alignB, 0–51 cm), set the room mic, and pick the microphone from the locker (/api/cabinet).
 *       Legacy cabinets convert in place first (/api/cabinet/convert).
 *     • Stomp — click a pedal's footswitch to bypass it.
 *     • Build the board — drag pedals along the board to reorder, off the board to remove, and in from the gear bag
 *       (rack insert / move / remove, the same routes the chain uses).
 *
 *   Topology → room:
 *     pedal / wah (any lane)        → the pedalboard, chain order left→right (back row first); wahs at the front-left
 *     split                         → an A/B·Y box on the board (click it for the lane balance)
 *     amp / NAM (+ the cab after it) → a stack: head on its cabinet; combo on a stand (its cab unit = the mic in front)
 *     cab on its own                → a standalone speaker cabinet with a mic
 *     MIDI: instrument / poly synth → keyboards on an X stand; Pitch→MIDI / MIDI FX are small desktop modules on top
 *     everything else (rack / studio gear, plugins) → one or more rack cases, at the right
 *
 *   Scale: real-world proportions. A 4×12 cabinet (≈ 75 cm) is AMP_W px wide at the backline (4 px/cm); the board
 *   is nearer the eye (BOARD_MAG ×). Every stompbox is sized in centimetres from its drawing's aspect ratio
 *   (a standard box ≈ 7 × 12 cm, a round Fuzz Face ≈ 15 cm, a wah ≈ 10 × 25 cm), and the board is at most two rows
 *   and ROW_MAX wide (it shrinks uniformly beyond that), so no rig can blow it up.
 *
 *   Plumbing (all call-time globals from index.html): units render through the chain's renderers (pedalHTML, ampHTML,
 *   cabHTML, rackHTML, midiUnitHTML, namHTML; illustrated faces via assets/unit-art.js), sized with CSS zoom. Their
 *   controls are ordinary `.ctl[data-slot][data-pid]`, so indexControls() keeps them painted and setValue /
 *   onPointerDown / onWheel / resetValue / ctlMenu drive them in the focused card.
 *   Hooks: HRRoom.sync() from applyRackState / toggleBypass, HRRoom.onTelemetry() from onTelemetry,
 *   HRRoom.chainUnitClick() from the Signal Chain's click handler (the same focus card lifts chain units).
 */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const reduced = () => !!(window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches);
  const typingIn = (a) => a && (a.tagName === "INPUT" && a.type !== "range" || a.tagName === "SELECT" || a.tagName === "TEXTAREA" || a.isContentEditable);
  const PP = (s, id) => (s && s.params || []).find((p) => p.id === id) || null;
  const now = () => performance.now();

  /* ---------- scene geometry (natural units; the whole scene is scaled to the pane) ---------- */
  const SH = 820;            // scene height
  const SEAM = 446;          // where the back wall meets the floor
  const FLOOR = 512;         // where the backline's feet touch the floor (a little in front of the wall)
  const BOARD_FRONT = 796;   // front edge of the pedalboard (its lip below that edge is LIP tall)
  const LIP = 13;
  const TILT = 36;           // pedalboard tilt (deg) — seen from where you stand
  const PERSP = 1150;        // its perspective, and where the eye is (above the board)
  const ORY = -340;
  const AMP_W = 300, RACK_W = 226, KEYS_W = 350, GAP = 62;
  const NAT = { stack: 860, rack: 900 };   // natural widths of the faces as the chain lays them out
  const PX_CM = AMP_W / 75;  // backline scale: a 4×12 cabinet is ≈ 75 cm wide
  const BOARD_MAG = 2.1;     // the board is nearer the eye than the backline
  const BPX = PX_CM * BOARD_MAG;   // px per cm on the board
  const ROW_MAX = 860;       // widest a row of pedals may get before the board shrinks it
  const PGAP = 16;           // gap between pedals on the board
  const DEPTH_MAX = 36 * BPX; // deepest the rows of pedals may get (a large two-row board)

  const R = {
    root: null, wrap: null, cam: null, scene: null, content: null, hud: null, tag: null, focusEl: null,
    built: false, visible: false, stale: true, key: "", sw: 1280, scale: 1, ox: 0, oy: 0,
    focus: null,          // { slot | split, card, src, host, mode }
    lvl: 0, lvlShown: -1, live: false, lit: [], cones: [], mods: [], tubes: [], meters: [], jewels: [],
    bagOpen: false,
  };

  /* =====================================================================
     SCENES — where you're playing. Purely visual: a scene never touches the rig or the engine.
       Each scene is one illustrated SVG backdrop (wall / sky, its floor or ground drawn in the room's own perspective,
       props and the light fixtures) that is painted once, plus a few small animated overlays (flicker, haze, beams,
       grass) that only animate transform / opacity. A grade layer over the gear tints it with the scene's light.
       The choice is remembered across rigs (localStorage "hr.room.scene").
     ===================================================================== */
  const SCENES = [
    { id: "studio", name: "Studio", sub: "Live room, late session", blurb: "A tracking room: slatted wood, track spots over the gear, wall sconces, the control-room window glowing." },
    { id: "living", name: "Living room", sub: "Fireplace, sofa, books", blurb: "Home. A fire going, the sofa under the window, shelves of records and books, warm lamps." },
    { id: "stage", name: "Concert stage", sub: "Truss, drum riser, LED wall", blurb: "On the deck of a big stage: the lighting rig overhead, drums on the riser, side fills and the video wall." },
    { id: "outdoors", name: "Outdoors", sub: "Mountain meadow, golden hour", blurb: "A wooden riser on a meadow above a lake, mountains going gold as the sun drops behind them." },
  ];
  const sceneById = (id) => SCENES.find((s) => s.id === id) || SCENES[0];
  const LS_SCENE = "hr.room.scene";
  const lsGet = (k, d) => { try { const v = localStorage.getItem(k); return v == null ? d : v; } catch (e) { return d; } };
  const lsSet = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* private mode */ } };
  try { localStorage.removeItem("hr.room.sound"); } catch (e) { /* the old room-sound switch is gone */ }
  R.sceneId = sceneById(lsGet(LS_SCENE, "studio")).id;
  function setScene(id) {
    id = sceneById(id).id;
    if (id !== R.sceneId) { lsSet(LS_SCENE, id); sceneTransition(id); }
    closeSpaces();
    return Promise.resolve();
  }

  /* ---------- drawing helpers: the floor in the room's perspective ----------
     The eye is at amp-head height: horizon HZ. A floor point X px to the side of centre (in backline px, where a 4×12
     is 300 px) at screen height y sits at x = cx + X·(y − HZ)/(FLOOR − HZ). Depth Z (backline = 1) is at y = HZ + FB/Z. */
  const HZ = 140, FB = FLOOR - HZ;
  const gx = (cx, X, y) => cx + X * (y - HZ) / FB;
  const zy = (Z) => HZ + FB / Z;
  const f1 = (v) => (Math.round(v * 10) / 10).toString();
  function rng(seed) { let k = seed >>> 0 || 1; return () => { k = (k * 1664525 + 1013904223) >>> 0; return k / 4294967296; }; }
  const quad = (cx, X1, X2, y1, y2) => `${f1(gx(cx, X1, y1))},${f1(y1)} ${f1(gx(cx, X2, y1))},${f1(y1)} ${f1(gx(cx, X2, y2))},${f1(y2)} ${f1(gx(cx, X1, y2))},${f1(y2)}`;
  const mix = (a, b, t) => { const p = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16)); const A = p(a), B = p(b); return "#" + A.map((v, i) => Math.round(v + (B[i] - v) * t).toString(16).padStart(2, "0")).join(""); };
  /* A rug lying on the floor: border bands, a field of small lozenges and a medallion, fringe at both ends. */
  function rugSVG(cx, X, Z1, Z2, c) {
    const y1 = zy(Z1), y2 = zy(Z2);
    const band = (iX, iZ, fill) => { const a = zy(Z1 - iZ), b = zy(Z2 + iZ * 0.5); return `<polygon points="${quad(cx, -X + iX, X - iX, a, b)}" fill="${fill}"/>`; };
    let o = `<polygon points="${quad(cx, -X - 4, X + 4, y1 - 2, y2 + 4)}" fill="rgba(0,0,0,.45)" filter="url(#P-soft)"/>`;
    o += band(0, 0, c.edge) + band(10, 0.006, c.b1) + band(22, 0.013, c.edge) + band(28, 0.017, c.b2) + band(46, 0.03, c.edge) + band(52, 0.034, c.field);
    // lozenges in the field
    const r = rng(77);
    for (let i = -8; i <= 8; i++) for (let j = 1; j <= 9; j++) {
      const Xc = i * (X - 70) / 8.5, Z = Z1 - 0.045 - (j - 0.5) * (Z1 - Z2 - 0.1) / 9.2, y = zy(Z), s = (y - HZ) / FB;
      if (Math.abs(i) < 3 && j > 2 && j < 8) continue;
      const w = 6 * s, h = 6 * s * 0.55;
      o += `<path d="M${f1(gx(cx, Xc, y))} ${f1(y - h)} l${f1(w)} ${f1(h)} l${f1(-w)} ${f1(h)} l${f1(-w)} ${f1(-h)}z" fill="${r() > 0.5 ? c.m1 : c.m2}" opacity=".85"/>`;
    }
    // medallion
    const ym = zy((Z1 + Z2) / 2), sm = (ym - HZ) / FB;
    o += `<ellipse cx="${f1(cx)}" cy="${f1(ym)}" rx="${f1(92 * sm)}" ry="${f1(40 * sm)}" fill="${c.b2}"/><ellipse cx="${f1(cx)}" cy="${f1(ym)}" rx="${f1(70 * sm)}" ry="${f1(30 * sm)}" fill="${c.edge}"/>
      <ellipse cx="${f1(cx)}" cy="${f1(ym)}" rx="${f1(46 * sm)}" ry="${f1(20 * sm)}" fill="${c.m1}"/><ellipse cx="${f1(cx)}" cy="${f1(ym)}" rx="${f1(18 * sm)}" ry="${f1(8 * sm)}" fill="${c.b1}"/>`;
    // fringe
    for (let k = -X + 6; k <= X - 6; k += 9) { const a = gx(cx, k, y1), b = gx(cx, k, y2); o += `<path d="M${f1(a)} ${f1(y1)} v${f1(-5)}M${f1(b)} ${f1(y2)} v${f1(10)}" stroke="${c.fringe}" stroke-width="1.6" opacity=".8"/>`; }
    o += `<polygon points="${quad(cx, -X, X, y1, y2)}" fill="url(#P-rugshade)"/>`;
    return o;
  }
  const DEFS = (P, extra = "") => `<defs>
      <filter id="${P}-soft" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="6"/></filter>
      <filter id="${P}-blur2" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="2"/></filter>
      <filter id="${P}-blur12" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="12"/></filter>
      <filter id="${P}-blur30" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="30"/></filter>
      <filter id="${P}-grain" x="0" y="0" width="100%" height="100%"><feTurbulence type="fractalNoise" baseFrequency=".9" numOctaves="2" seed="4" result="n"/><feColorMatrix in="n" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 .55 -.18"/><feComposite in2="SourceGraphic" operator="in"/></filter>
      <filter id="${P}-fabric" x="0" y="0" width="100%" height="100%"><feTurbulence type="fractalNoise" baseFrequency=".55 1.6" numOctaves="2" seed="9"/><feColorMatrix values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 .5 -.15"/><feComposite in2="SourceGraphic" operator="in"/></filter>
      <linearGradient id="${P}-rugshade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#000" stop-opacity=".35"/><stop offset=".4" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity=".15"/></linearGradient>
      ${extra}</defs>`;
  /* Every backdrop is drawn at one fixed size (ART_W wide, its centre on the room's centre) and becomes a cached image. */
  const ART_W = 1240, AX0 = -480, AY0 = -150, AVW = 2200, AVH = 1150;
  const wrapSVG = (P, W, body, defs) => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${AX0} ${AY0} ${AVW} ${AVH}" width="${AVW}" height="${AVH}">${DEFS(P, defs).replace(/P-/g, P + "-")}${body.replace(/#P-/g, `#${P}-`)}</svg>`;
  /* A guitar on a stand (body, neck, headstock), standing on the floor at (x, y), scale k. */
  function guitarSVG(x, y, k, body, flip) {
    const s = (v) => f1(v * k);
    return `<g transform="translate(${f1(x)} ${f1(y)})${flip ? " scale(-1 1)" : ""}">
      <ellipse cx="0" cy="2" rx="${s(46)}" ry="${s(6)}" fill="rgba(0,0,0,.5)" filter="url(#P-blur2)"/>
      <path d="M${s(-30)} 0 L${s(-6)} ${s(-74)} M${s(30)} 0 L${s(6)} ${s(-74)} M0 ${s(-74)} L0 ${s(-150)}" stroke="#1a1a1c" stroke-width="${s(3)}" stroke-linecap="round"/>
      <g transform="rotate(-6)">
        <path d="M${s(-34)} ${s(-60)} C${s(-44)} ${s(-92)} ${s(-30)} ${s(-118)} ${s(-20)} ${s(-122)} C${s(-14)} ${s(-138)} ${s(-30)} ${s(-150)} ${s(-24)} ${s(-170)} C${s(-14)} ${s(-196)} ${s(14)} ${s(-196)} ${s(24)} ${s(-170)} C${s(30)} ${s(-150)} ${s(14)} ${s(-138)} ${s(20)} ${s(-122)} C${s(30)} ${s(-118)} ${s(44)} ${s(-92)} ${s(34)} ${s(-60)} C${s(26)} ${s(-38)} ${s(-26)} ${s(-38)} ${s(-34)} ${s(-60)}Z" fill="${body}"/>
        <path d="M${s(-34)} ${s(-60)} C${s(-44)} ${s(-92)} ${s(-30)} ${s(-118)} ${s(-20)} ${s(-122)} C${s(-14)} ${s(-138)} ${s(-30)} ${s(-150)} ${s(-24)} ${s(-170)} C${s(-14)} ${s(-196)} ${s(14)} ${s(-196)} ${s(24)} ${s(-170)} C${s(30)} ${s(-150)} ${s(14)} ${s(-138)} ${s(20)} ${s(-122)} C${s(30)} ${s(-118)} ${s(44)} ${s(-92)} ${s(34)} ${s(-60)} C${s(26)} ${s(-38)} ${s(-26)} ${s(-38)} ${s(-34)} ${s(-60)}Z" fill="url(#P-burst)" opacity=".9"/>
        <rect x="${s(-4)}" y="${s(-330)}" width="${s(8)}" height="${s(190)}" rx="${s(2)}" fill="#3a2414"/><rect x="${s(-3)}" y="${s(-330)}" width="${s(2)}" height="${s(190)}" fill="rgba(255,230,190,.25)"/>
        <path d="M${s(-8)} ${s(-330)} l${s(-2)} ${s(-30)} c${s(4)} ${s(-6)} ${s(16)} ${s(-6)} ${s(20)} 0 l${s(-2)} ${s(30)}z" fill="#1d1410"/>
        <rect x="${s(-14)} " y="${s(-104)}" width="${s(28)}" height="${s(5)}" rx="${s(1)}" fill="#c9ccd1"/><rect x="${s(-12)}" y="${s(-88)}" width="${s(24)}" height="${s(7)}" rx="${s(2)}" fill="#1a1a1a"/>
        <circle cx="${s(18)}" cy="${s(-72)}" r="${s(3)}" fill="#d9c08a"/><circle cx="${s(24)}" cy="${s(-62)}" r="${s(3)}" fill="#d9c08a"/>
        <path d="M${s(-26)} ${s(-160)} C${s(-20)} ${s(-180)} ${s(4)} ${s(-188)} ${s(16)} ${s(-176)}" stroke="rgba(255,240,210,.35)" stroke-width="${s(2)}" fill="none"/>
      </g></g>`;
  }

  /* ---------- Studio: a live room late in a session ---------- */
  function studioSVG(W, P) {
    const cx = W / 2, r = rng(11);
    let b = `<rect x="-900" y="-700" width="${W + 1800}" height="2000" fill="#0d0907"/>`;
    // ceiling: dark acoustic baffles, the soffit edge, the lighting track
    b += `<rect x="-900" y="-700" width="${W + 1800}" height="742" fill="url(#P-ceil)"/>`;
    for (let x = -880; x < W + 900; x += 86) b += `<rect x="${x}" y="-700" width="44" height="736" fill="#0a0807"/><rect x="${x + 44}" y="-700" width="1.2" height="736" fill="rgba(255,220,180,.05)"/>`;
    b += `<rect x="-900" y="36" width="${W + 1800}" height="10" fill="#1c1510"/><rect x="-900" y="36" width="${W + 1800}" height="1.4" fill="rgba(255,214,170,.18)"/>`;
    b += `<rect x="-900" y="2" width="${W + 1800}" height="7" rx="2" fill="#2e2924"/><rect x="-900" y="2" width="${W + 1800}" height="1.5" fill="rgba(255,255,255,.14)"/>`;
    // the wall: plaster at the ends, a slatted oak wall in the middle (each slat its own tone), felt behind
    b += `<rect x="-900" y="46" width="${W + 1800}" height="400" fill="url(#P-plaster)"/><rect x="-900" y="46" width="${W + 1800}" height="400" fill="#fff" filter="url(#P-grain)" opacity=".5"/>`;
    const sx0 = 30, sx1 = W - 30;
    b += `<rect x="${sx0}" y="58" width="${sx1 - sx0}" height="376" fill="#0b0806"/>`;
    for (let x = sx0 + 2; x < sx1 - 12; x += 21) {
      const t = r(), c1 = mix("#3a2718", "#6b4a2e", t), c2 = mix("#24180f", "#46301e", t);
      b += `<rect x="${f1(x)}" y="58" width="15" height="376" fill="url(#P-slat)"/><rect x="${f1(x)}" y="58" width="15" height="376" fill="${c1}" opacity=".55"/>
        <path d="M${f1(x + 3 + r() * 3)} 58 C${f1(x + 2 + r() * 8)} 170 ${f1(x + 4 + r() * 6)} 300 ${f1(x + 3 + r() * 6)} 434" stroke="${c2}" stroke-width=".8" fill="none" opacity=".7"/>
        <rect x="${f1(x)}" y="58" width="1.2" height="376" fill="rgba(255,226,190,.16)"/>`;
    }
    b += `<rect x="${sx0}" y="58" width="${sx1 - sx0}" height="376" fill="url(#P-slatshade)"/>`;
    b += `<rect x="${sx0 - 6}" y="50" width="${sx1 - sx0 + 12}" height="9" fill="#4a3220"/><rect x="${sx0 - 6}" y="433" width="${sx1 - sx0 + 12}" height="6" fill="#2c1e13"/>`;
    // fabric absorbers outside the slats, a bass trap in each corner
    [[-560, -40], [W + 40, W + 560]].forEach(([a, c]) => { b += `<rect x="${a}" y="70" width="${c - a}" height="360" rx="3" fill="#221d1a"/><rect x="${a}" y="70" width="${c - a}" height="360" fill="#fff" filter="url(#P-fabric)" opacity=".35"/>`; });
    // control-room window (right): dark glass, the console's meters and screen glowing through it
    const wx = cx + W * 0.2, wy = 96, ww = Math.min(250, W * 0.2), wh = 150;
    b += `<g><rect x="${f1(wx - 12)}" y="${wy - 12}" width="${f1(ww + 24)}" height="${wh + 24}" rx="4" fill="#3a2a1c"/><rect x="${f1(wx - 4)}" y="${wy - 4}" width="${f1(ww + 8)}" height="${wh + 8}" rx="2" fill="#120d09"/>
      <rect x="${f1(wx)}" y="${wy}" width="${f1(ww)}" height="${wh}" fill="url(#P-glass)"/>
      <rect x="${f1(wx + 10)}" y="${wy + 92}" width="${f1(ww - 20)}" height="40" fill="#0b0a0c"/>
      ${Array.from({ length: 30 }, (_, i) => `<circle cx="${f1(wx + 16 + (i % 15) * (ww - 30) / 14)}" cy="${wy + 104 + Math.floor(i / 15) * 10}" r="1.4" fill="${["#4be07a", "#f2c94c", "#ff6b4a", "#7fb6ff"][i % 4]}" opacity="${f1(0.35 + r() * 0.6)}"/>`).join("")}
      <rect x="${f1(wx + ww * 0.32)}" y="${wy + 34}" width="${f1(ww * 0.36)}" height="48" rx="2" fill="#2a4a7a" opacity=".55"/><rect x="${f1(wx + ww * 0.32)}" y="${wy + 34}" width="${f1(ww * 0.36)}" height="48" fill="url(#P-screen)"/>
      <rect x="${f1(wx + 12)}" y="${wy + 52}" width="34" height="22" rx="2" fill="#e8b860" opacity=".75"/><rect x="${f1(wx + ww - 46)}" y="${wy + 52}" width="34" height="22" rx="2" fill="#e8b860" opacity=".75"/>
      <path d="M${f1(wx)} ${wy + wh} L${f1(wx + ww * 0.45)} ${wy} L${f1(wx + ww * 0.6)} ${wy} L${f1(wx + ww * 0.15)} ${wy + wh}Z" fill="rgba(255,255,255,.05)"/>
      <rect x="${f1(wx - 12)}" y="${wy + wh + 8}" width="${f1(ww + 24)}" height="6" fill="#4a3424"/></g>`;
    // gold records
    [[sx0 + 150, 92], [sx0 + 240, 92]].forEach(([x, y]) => { b += `<g transform="translate(${f1(x)} ${y})"><rect width="70" height="90" fill="#151515"/><rect x="2" y="2" width="66" height="86" fill="none" stroke="#b08d4a" stroke-width="3"/>
      <circle cx="35" cy="36" r="24" fill="url(#P-gold)"/><circle cx="35" cy="36" r="6" fill="#1b1b1b"/><circle cx="35" cy="36" r="1.6" fill="#f7e3a3"/><rect x="16" y="70" width="38" height="8" fill="#d8d2c2" opacity=".85"/></g>`; });
    // sconces: a visible lamp with a fabric shade, its light up and down the wall
    const sconce = (x) => `<g><ellipse cx="${f1(x)}" cy="150" rx="70" ry="130" fill="url(#P-sconceglow)"/>
      <rect x="${f1(x - 3)}" y="150" width="6" height="26" fill="#2c2620"/><rect x="${f1(x - 9)}" y="170" width="18" height="10" rx="2" fill="#3b342b"/>
      <path d="M${f1(x - 16)} 128 L${f1(x + 16)} 128 L${f1(x + 21)} 154 L${f1(x - 21)} 154Z" fill="url(#P-shade)"/><ellipse cx="${f1(x)}" cy="154" rx="21" ry="3" fill="#fff2cc"/></g>`;
    b += sconce(sx0 + 70) + sconce(sx1 - 40);
    // a guitar on its stand by the wall, a coil of cable on a hook
    b += guitarSVG(sx0 + 60, 472, 0.8, "#4a120e");
    b += `<g transform="translate(${f1(sx1 - 60)} 200)"><rect x="-2" y="-6" width="4" height="12" fill="#888"/><ellipse cx="0" cy="34" rx="20" ry="32" fill="none" stroke="#141416" stroke-width="5"/><ellipse cx="0" cy="38" rx="16" ry="30" fill="none" stroke="#1d1e21" stroke-width="4"/><path d="M14 60 q8 30 2 70" stroke="#141416" stroke-width="4" fill="none"/></g>`;
    // skirting
    b += `<rect x="-900" y="434" width="${W + 1800}" height="14" fill="#1f1610"/><rect x="-900" y="434" width="${W + 1800}" height="1.5" fill="rgba(255,220,180,.18)"/>`;
    // the floor: oak planks running away from you, each its own tone, butt joints, the varnish catching the spots
    b += `<rect x="-900" y="446" width="${W + 1800}" height="854" fill="#241710"/>`;
    for (let X = -2400; X < 2400; X += 64) {
      const t = r(), c = mix("#2c1c12", "#5a3c26", t);
      b += `<polygon points="${quad(cx, X, X + 62, 446, 1300)}" fill="${c}"/>`;
      let Z = 1.22;
      while (Z > 0.38) { Z -= 0.12 + r() * 0.35; const y = zy(Z); if (y > 1300) break; b += `<line x1="${f1(gx(cx, X, y))}" y1="${f1(y)}" x2="${f1(gx(cx, X + 62, y))}" y2="${f1(y)}" stroke="#140c07" stroke-width="${f1((y - HZ) / FB * 1.2)}"/>`; }
      b += `<line x1="${f1(gx(cx, X + 62, 446))}" y1="446" x2="${f1(gx(cx, X + 62, 1300))}" y2="1300" stroke="#120a06" stroke-width="1.6"/>`;
    }
    b += `<rect x="-900" y="446" width="${W + 1800}" height="854" fill="#fff" filter="url(#P-grain)" opacity=".35"/>`;
    b += `<rect x="-900" y="446" width="${W + 1800}" height="854" fill="url(#P-floorshade)"/>`;
    b += rugSVG(cx, 300, 0.88, 0.5, { edge: "#2a100c", b1: "#a8803e", b2: "#1d3048", field: "#5a1c16", m1: "#c99a52", m2: "#2a4a66", fringe: "#d8c6a0" });
    const defs = `<linearGradient id="P-ceil" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#050403"/><stop offset="1" stop-color="#17110c"/></linearGradient>
      <linearGradient id="P-plaster" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2a1f17"/><stop offset=".6" stop-color="#1f1711"/><stop offset="1" stop-color="#140e0a"/></linearGradient>
      <linearGradient id="P-slat" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#5b3e27"/><stop offset=".5" stop-color="#3e2a1a"/><stop offset="1" stop-color="#2a1c11"/></linearGradient>
      <linearGradient id="P-slatshade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#000" stop-opacity=".55"/><stop offset=".35" stop-color="#000" stop-opacity=".05"/><stop offset=".8" stop-color="#000" stop-opacity=".1"/><stop offset="1" stop-color="#000" stop-opacity=".5"/></linearGradient>
      <linearGradient id="P-glass" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0d1218"/><stop offset="1" stop-color="#151b22"/></linearGradient>
      <linearGradient id="P-screen" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#9cc8ff" stop-opacity=".35"/><stop offset="1" stop-color="#9cc8ff" stop-opacity="0"/></linearGradient>
      <radialGradient id="P-gold" cx=".4" cy=".35" r=".7"><stop offset="0" stop-color="#fff0b8"/><stop offset=".5" stop-color="#d4a94c"/><stop offset="1" stop-color="#8a6420"/></radialGradient>
      <radialGradient id="P-sconceglow" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#ffcf8a" stop-opacity=".55"/><stop offset=".45" stop-color="#ffb060" stop-opacity=".16"/><stop offset="1" stop-color="#ffb060" stop-opacity="0"/></radialGradient>
      <linearGradient id="P-shade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#e8c890"/><stop offset="1" stop-color="#ffdca0"/></linearGradient>
      <linearGradient id="P-burst" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f2b24a" stop-opacity=".9"/><stop offset=".5" stop-color="#b8521c" stop-opacity=".4"/><stop offset="1" stop-color="#2a0c06" stop-opacity=".7"/></linearGradient>
      <linearGradient id="P-floorshade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#000" stop-opacity=".55"/><stop offset=".22" stop-color="#000" stop-opacity=".1"/><stop offset=".6" stop-color="#000" stop-opacity=".05"/><stop offset="1" stop-color="#000" stop-opacity=".55"/></linearGradient>`;
    return wrapSVG(P, W, b, defs);
  }

  /* ---------- Living room: home, an evening with the fire going ---------- */
  function livingSVG(W, P) {
    const cx = W / 2, r = rng(23);
    let b = `<rect x="-900" y="-700" width="${W + 1800}" height="2000" fill="#1b1512"/>`;
    // ceiling, crown moulding, walls in a deep green, picture rail, panelled dado and a white skirting
    b += `<rect x="-900" y="-700" width="${W + 1800}" height="724" fill="url(#P-lceil)"/>`;
    b += `<rect x="-900" y="22" width="${W + 1800}" height="424" fill="url(#P-lwall)"/><rect x="-900" y="22" width="${W + 1800}" height="424" fill="#fff" filter="url(#P-grain)" opacity=".35"/>`;
    b += `<rect x="-900" y="14" width="${W + 1800}" height="12" fill="#d9d2c2"/><rect x="-900" y="24" width="${W + 1800}" height="4" fill="#b9b1a0"/><rect x="-900" y="28" width="${W + 1800}" height="3" fill="rgba(0,0,0,.35)"/>`;
    b += `<rect x="-900" y="300" width="${W + 1800}" height="6" fill="#cfc7b6"/><rect x="-900" y="306" width="${W + 1800}" height="128" fill="#2f3f38"/>`;
    for (let x = -880; x < W + 900; x += 140) b += `<rect x="${x + 10}" y="318" width="120" height="104" fill="none" stroke="#25332d" stroke-width="3"/><rect x="${x + 12}" y="320" width="116" height="1.5" fill="rgba(255,255,255,.08)"/>`;
    b += `<rect x="-900" y="420" width="${W + 1800}" height="26" fill="#e3dccb"/><rect x="-900" y="420" width="${W + 1800}" height="2" fill="#fffaf0"/>`;
    // bookshelves (left): built-in, books of every colour, a few lying flat, a plant trailing over
    const fx = cx - W * 0.21, fw = 210;
    const bw = 300, bx = Math.min(-40, fx - fw / 2 - 20 - bw);
    b += `<rect x="${f1(bx)}" y="40" width="${bw}" height="406" fill="#e6dfcf"/><rect x="${f1(bx + 12)}" y="52" width="${bw - 24}" height="382" fill="#3a2e24"/>`;
    for (let row = 0; row < 5; row++) {
      const y0 = 52 + row * 76, base = y0 + 70;
      b += `<rect x="${f1(bx + 12)}" y="${base}" width="${bw - 24}" height="8" fill="#ece5d6"/>`;
      let x = bx + 18;
      while (x < bx + bw - 30) {
        if (r() < 0.12) { const h = 6 + r() * 4; for (let k = 0; k < 3; k++) b += `<rect x="${f1(x)}" y="${f1(base - (k + 1) * h)}" width="38" height="${f1(h - 1)}" fill="${["#8a3a2a", "#2c4a5a", "#c9a35c", "#4a5a3a"][Math.floor(r() * 4)]}"/>`; x += 44; continue; }
        const w = 6 + r() * 9, h = 42 + r() * 22, c = ["#7a2f24", "#1d3a4a", "#c9a35c", "#2d4a2f", "#6b4a7a", "#b5462e", "#d8c8a4", "#3a5a6a", "#8a5a2a", "#22252c", "#e3d7b8"][Math.floor(r() * 11)];
        const lean = r() < 0.08 ? 8 : 0;
        b += `<g transform="rotate(${lean} ${f1(x)} ${base})"><rect x="${f1(x)}" y="${f1(base - h)}" width="${f1(w)}" height="${f1(h)}" fill="${c}"/><rect x="${f1(x + 1)}" y="${f1(base - h + 6)}" width="${f1(w - 2)}" height="2" fill="rgba(255,235,190,.35)"/></g>`;
        x += w + 0.6 + lean * 0.6;
      }
    }
    b += `<path d="M${f1(bx + bw - 70)} 128 c-10 40 10 60 -6 110 M${f1(bx + bw - 60)} 128 c6 30 -8 70 10 120" stroke="#2f5a32" stroke-width="3" fill="none"/>${Array.from({ length: 16 }, (_, i) => `<ellipse cx="${f1(bx + bw - 70 + (i % 2 ? 12 : -6) + r() * 6)}" cy="${f1(132 + i * 8)}" rx="6" ry="4" fill="${i % 3 ? "#3f7a42" : "#5a9a52"}" transform="rotate(${f1(r() * 60 - 30)} ${f1(bx + bw - 66)} ${f1(132 + i * 8)})"/>`).join("")}`;
    // the fireplace: a stone surround and mantel, a round mirror above, candles; the fire itself is animated
    b += `<g><ellipse cx="${f1(fx)}" cy="168" rx="50" ry="50" fill="#b08d4a"/><ellipse cx="${f1(fx)}" cy="168" rx="45" ry="45" fill="url(#P-mirror)"/><path d="M${f1(fx - 24)} 138 l30 -8 l-38 46z" fill="rgba(255,255,255,.12)"/>
      <rect x="${f1(fx - fw / 2 - 16)}" y="236" width="${fw + 32}" height="16" rx="2" fill="#e8e0cd"/><rect x="${f1(fx - fw / 2 - 16)}" y="250" width="${fw + 32}" height="4" fill="rgba(0,0,0,.3)"/>
      <rect x="${f1(fx - fw / 2)}" y="252" width="${fw}" height="194" fill="#d6ccb6"/><rect x="${f1(fx - fw / 2)}" y="252" width="${fw}" height="194" fill="#fff" filter="url(#P-grain)" opacity=".45"/>
      <path d="M${f1(fx - 72)} 446 L${f1(fx - 72)} 330 Q${f1(fx)} 290 ${f1(fx + 72)} 330 L${f1(fx + 72)} 446Z" fill="#120a07"/>
      <path d="M${f1(fx - 72)} 446 L${f1(fx - 72)} 330 Q${f1(fx)} 290 ${f1(fx + 72)} 330 L${f1(fx + 72)} 446Z" fill="url(#P-firebox)"/>
      <path d="M${f1(fx - 50)} 436 l100 0 l-8 -12 l-84 0z" fill="#2a1c14"/><path d="M${f1(fx - 46)} 428 l40 -12 l46 10 l-4 6 l-40 -8 l-40 10z" fill="#4a2a16"/>
      ${[-28, 0, 28].map((d) => `<rect x="${f1(fx + d - 2)}" y="${222 - (d === 0 ? 8 : 0)}" width="5" height="${14 + (d === 0 ? 8 : 0)}" rx="1" fill="#efe6d2"/><ellipse cx="${f1(fx + d + 0.5)}" cy="${218 - (d === 0 ? 8 : 0)}" rx="2.2" ry="4" fill="#ffd27a"/>`).join("")}
      <rect x="${f1(fx - fw / 2 - 30)}" y="440" width="${fw + 60}" height="14" fill="#cfc4ac"/></g>`;
    // the window bay (right): night garden and the moon through tall panes, heavy curtains
    const wx = cx + W * 0.22, ww = 300;
    b += `<g><rect x="${f1(wx - 14)}" y="44" width="${ww + 28}" height="264" fill="#e3dccb"/><rect x="${f1(wx)}" y="56" width="${ww}" height="240" fill="url(#P-night)"/>
      <circle cx="${f1(wx + ww * 0.72)}" cy="98" r="15" fill="#f4f0dc"/><circle cx="${f1(wx + ww * 0.72)}" cy="98" r="40" fill="#cfe0ff" opacity=".12" filter="url(#P-blur12)"/>
      <path d="M${f1(wx)} 296 L${f1(wx)} 220 Q${f1(wx + 40)} 180 ${f1(wx + 90)} 214 Q${f1(wx + 140)} 160 ${f1(wx + 200)} 206 Q${f1(wx + 250)} 176 ${f1(wx + ww)} 214 L${f1(wx + ww)} 296Z" fill="#0d1a1a"/>
      ${Array.from({ length: 9 }, () => `<circle cx="${f1(wx + 10 + r() * (ww - 20))}" cy="${f1(240 + r() * 40)}" r="1.4" fill="#ffd38a" opacity=".8"/>`).join("")}
      <path d="M${f1(wx + ww / 3)} 56 v240 M${f1(wx + 2 * ww / 3)} 56 v240 M${f1(wx)} 140 h${ww} M${f1(wx)} 220 h${ww}" stroke="#e3dccb" stroke-width="5"/>
      <rect x="${f1(wx - 40)}" y="34" width="${ww + 80}" height="7" rx="3" fill="#6b4a2a"/>
      <path d="M${f1(wx - 46)} 38 C${f1(wx - 30)} 160 ${f1(wx - 52)} 300 ${f1(wx - 28)} 446 L${f1(wx + 30)} 446 C${f1(wx + 10)} 300 ${f1(wx + 26)} 160 ${f1(wx + 6)} 38Z" fill="url(#P-curtain)"/>
      <path d="M${f1(wx + ww + 46)} 38 C${f1(wx + ww + 30)} 160 ${f1(wx + ww + 52)} 300 ${f1(wx + ww + 28)} 446 L${f1(wx + ww - 30)} 446 C${f1(wx + ww - 10)} 300 ${f1(wx + ww - 26)} 160 ${f1(wx + ww - 6)} 38Z" fill="url(#P-curtain)"/></g>`;
    // the sofa under the window: mustard velvet, cushions, a throw; a side table with a lamp; an arc lamp over it
    const sx = wx + ww / 2, sy = 446;
    b += `<g><ellipse cx="${f1(sx)}" cy="${sy + 10}" rx="230" ry="16" fill="rgba(0,0,0,.55)" filter="url(#P-soft)"/>
      <rect x="${f1(sx - 210)}" y="${sy - 128}" width="420" height="80" rx="26" fill="#9a6a1e"/><rect x="${f1(sx - 210)}" y="${sy - 128}" width="420" height="80" rx="26" fill="url(#P-velvet)"/>
      <rect x="${f1(sx - 222)}" y="${sy - 82}" width="444" height="66" rx="18" fill="#b07a24"/><rect x="${f1(sx - 222)}" y="${sy - 82}" width="444" height="66" rx="18" fill="url(#P-velvet)"/>
      <rect x="${f1(sx - 240)}" y="${sy - 104}" width="44" height="90" rx="20" fill="#a37020"/><rect x="${f1(sx + 196)}" y="${sy - 104}" width="44" height="90" rx="20" fill="#a37020"/>
      <rect x="${f1(sx - 160)}" y="${sy - 120}" width="86" height="56" rx="16" fill="#2c4a5a" transform="rotate(-6 ${f1(sx - 117)} ${sy - 92})"/><rect x="${f1(sx + 70)}" y="${sy - 118}" width="80" height="52" rx="16" fill="#c96a3a" transform="rotate(7 ${f1(sx + 110)} ${sy - 92})"/>
      <path d="M${f1(sx + 40)} ${sy - 84} q40 6 70 -2 l14 64 q-40 8 -76 0z" fill="#e8dcc4" opacity=".9"/>
      <rect x="${f1(sx - 200)}" y="${sy - 16}" width="8" height="16" fill="#2a1a10"/><rect x="${f1(sx + 192)}" y="${sy - 16}" width="8" height="16" fill="#2a1a10"/></g>`;
    const tx = sx - 300;
    b += `<g><ellipse cx="${f1(tx)}" cy="${sy + 4}" rx="44" ry="8" fill="rgba(0,0,0,.5)" filter="url(#P-blur2)"/><rect x="${f1(tx - 34)}" y="${sy - 70}" width="68" height="8" rx="2" fill="#6b4224"/><path d="M${f1(tx - 26)} ${sy - 62} l-6 62 M${f1(tx + 26)} ${sy - 62} l6 62" stroke="#4a2c16" stroke-width="4"/>
      <ellipse cx="${f1(tx)}" cy="${sy - 160}" rx="90" ry="110" fill="url(#P-lampglow)"/>
      <path d="M${f1(tx - 8)} ${sy - 72} q-10 -20 0 -40 q8 -12 16 0 q10 20 0 40z" fill="#c9b79a"/><rect x="${f1(tx - 2)}" y="${sy - 132}" width="4" height="22" fill="#8a7a5a"/>
      <path d="M${f1(tx - 26)} ${sy - 168} L${f1(tx + 26)} ${sy - 168} L${f1(tx + 34)} ${sy - 128} L${f1(tx - 34)} ${sy - 128}Z" fill="url(#P-lshade)"/><ellipse cx="${f1(tx)}" cy="${sy - 128}" rx="34" ry="4" fill="#fff0c8"/></g>`;
    const ax = sx + 260;
    b += `<g><ellipse cx="${f1(ax)}" cy="${sy + 4}" rx="30" ry="6" fill="rgba(0,0,0,.5)" filter="url(#P-blur2)"/><ellipse cx="${f1(ax)}" cy="${sy}" rx="22" ry="5" fill="#d0d2d4"/>
      <path d="M${f1(ax)} ${sy} L${f1(ax)} ${sy - 250} Q${f1(ax - 10)} ${sy - 330} ${f1(ax - 150)} ${sy - 300}" stroke="#c8cacc" stroke-width="4" fill="none"/>
      <ellipse cx="${f1(ax - 150)}" cy="${sy - 220}" rx="100" ry="120" fill="url(#P-lampglow)"/>
      <path d="M${f1(ax - 176)} ${sy - 300} Q${f1(ax - 150)} ${sy - 326} ${f1(ax - 124)} ${sy - 300}Z" fill="#cfd2d4"/><ellipse cx="${f1(ax - 150)}" cy="${sy - 300}" rx="26" ry="4" fill="#fff0c8"/></g>`;
    // a big fiddle-leaf fig in a basket by the fireplace
    const px = bx + bw - 10;
    b += `<g><ellipse cx="${f1(px)}" cy="${sy + 6}" rx="50" ry="9" fill="rgba(0,0,0,.5)" filter="url(#P-blur2)"/><path d="M${f1(px - 36)} ${sy - 60} l72 0 l-8 62 l-56 0z" fill="#b58a52"/><path d="M${f1(px - 36)} ${sy - 50} l72 0 M${f1(px - 34)} ${sy - 36} l68 0 M${f1(px - 32)} ${sy - 22} l64 0" stroke="#8a6436" stroke-width="2"/>
      <path d="M${f1(px)} ${sy - 60} C${f1(px - 4)} ${sy - 160} ${f1(px + 10)} ${sy - 220} ${f1(px + 2)} ${sy - 300}" stroke="#4a3420" stroke-width="5" fill="none"/>
      ${Array.from({ length: 15 }, (_, i) => { const y = sy - 80 - i * 15, side = i % 2 ? 1 : -1, a = side * (30 + r() * 40), cxl = px + side * (18 + r() * 14); return `<g transform="rotate(${f1(a)} ${f1(px)} ${f1(y)})"><ellipse cx="${f1(cxl)}" cy="${f1(y)}" rx="${f1(20 + r() * 8)}" ry="${f1(12 + r() * 5)}" fill="${i % 3 ? "#2f6a36" : "#3f8446"}"/><path d="M${f1(px)} ${f1(y)} L${f1(cxl + side * 18)} ${f1(y)}" stroke="#7fb27a" stroke-width=".8" opacity=".6"/></g>`; }).join("")}</g>`;
    // the floor: honey oak boards running across the room, joints staggered, the firelight on it
    b += `<rect x="-900" y="446" width="${W + 1800}" height="854" fill="#6a4426"/>`;
    let Z = 1.22, row = 0;
    while (true) {
      const Zn = Z - 0.06 * Z * Z * 1.15, y1 = zy(Z), y2 = zy(Zn);
      if (y1 > 1300) break;
      const t = r(); b += `<rect x="-900" y="${f1(y1)}" width="${W + 1800}" height="${f1(y2 - y1)}" fill="${mix("#5a381e", "#8a5a32", t)}"/><rect x="-900" y="${f1(y1)}" width="${W + 1800}" height="${f1(Math.max(0.8, (y1 - HZ) / FB * 1.4))}" fill="#2a170a" opacity=".8"/>`;
      for (let X = -2600 + (row % 3) * 140 + r() * 80; X < 2600; X += 360 + r() * 200) { const yy = (y1 + y2) / 2; b += `<line x1="${f1(gx(cx, X, y1))}" y1="${f1(y1)}" x2="${f1(gx(cx, X, y2))}" y2="${f1(y2)}" stroke="#2a170a" stroke-width="${f1((yy - HZ) / FB * 1.2)}"/>`; }
      Z = Zn; row++;
    }
    b += `<rect x="-900" y="446" width="${W + 1800}" height="854" fill="#fff" filter="url(#P-grain)" opacity=".3"/>`;
    b += `<rect x="-900" y="446" width="${W + 1800}" height="854" fill="url(#P-floorshade)"/>`;
    // moonlight from the window lying on the floor, firelight pooling in front of the hearth
    b += `<polygon points="${f1(wx + 20)},450 ${f1(wx + ww - 20)},450 ${f1(wx + ww + 150)},640 ${f1(wx + 120)},640" fill="#9cb8ff" opacity=".06" filter="url(#P-blur12)"/>`;
    b += `<ellipse cx="${f1(fx)}" cy="520" rx="300" ry="90" fill="#ff9a40" opacity=".18" filter="url(#P-blur30)"/>`;
    b += rugSVG(cx, 330, 0.9, 0.46, { edge: "#e9ddc3", b1: "#c45a2c", b2: "#2e5a5a", field: "#e3d2b0", m1: "#b84a26", m2: "#2e5a5a", fringe: "#efe4cc" });
    const defs = `<linearGradient id="P-lceil" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0e0b0a"/><stop offset="1" stop-color="#2a221d"/></linearGradient>
      <linearGradient id="P-lwall" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1f2c28"/><stop offset=".55" stop-color="#2c3d36"/><stop offset="1" stop-color="#25332d"/></linearGradient>
      <radialGradient id="P-mirror" cx=".45" cy=".4" r=".7"><stop offset="0" stop-color="#6a5240"/><stop offset=".6" stop-color="#2c2420"/><stop offset="1" stop-color="#1a1512"/></radialGradient>
      <radialGradient id="P-firebox" cx=".5" cy=".85" r=".7"><stop offset="0" stop-color="#ff8a2a" stop-opacity=".55"/><stop offset=".5" stop-color="#7a2a08" stop-opacity=".35"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>
      <linearGradient id="P-night" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0d1430"/><stop offset=".6" stop-color="#1b2a4a"/><stop offset="1" stop-color="#22324a"/></linearGradient>
      <linearGradient id="P-curtain" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#5a1e12"/><stop offset=".2" stop-color="#8a3a22"/><stop offset=".4" stop-color="#5a1e12"/><stop offset=".6" stop-color="#7a3018"/><stop offset=".8" stop-color="#4a180e"/><stop offset="1" stop-color="#6a2614"/></linearGradient>
      <linearGradient id="P-velvet" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".18"/><stop offset=".5" stop-color="#fff" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity=".35"/></linearGradient>
      <radialGradient id="P-lampglow" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#ffcf8a" stop-opacity=".5"/><stop offset=".5" stop-color="#ffb060" stop-opacity=".12"/><stop offset="1" stop-color="#ffb060" stop-opacity="0"/></radialGradient>
      <linearGradient id="P-lshade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f3dcae"/><stop offset="1" stop-color="#ffe7b8"/></linearGradient>
      <linearGradient id="P-floorshade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#000" stop-opacity=".45"/><stop offset=".25" stop-color="#000" stop-opacity=".05"/><stop offset="1" stop-color="#000" stop-opacity=".5"/></linearGradient>`;
    return wrapSVG(P, W, b, defs);
  }

  /* ---------- Concert stage: standing on the deck, looking upstage ---------- */
  function stageSVG(W, P) {
    const cx = W / 2, r = rng(5);
    let b = `<rect x="-900" y="-700" width="${W + 1800}" height="2000" fill="#050407"/>`;
    // the house: black masking, the upstage cyc glowing behind the LED wall
    b += `<rect x="-900" y="-700" width="${W + 1800}" height="1146" fill="url(#P-cyc)"/>`;
    // the LED video wall, with the crowd on it (IMAG) under a light show
    const lw = Math.min(900, W * 0.64), lx = cx - lw / 2, ly = 70, lh = 290;
    b += `<rect x="${f1(lx - 10)}" y="${ly - 10}" width="${f1(lw + 20)}" height="${lh + 20}" fill="#0b0b0e"/><rect x="${f1(lx)}" y="${ly}" width="${f1(lw)}" height="${lh}" fill="url(#P-led)"/>
      <ellipse cx="${f1(cx)}" cy="${ly + lh * 0.35}" rx="${f1(lw * 0.3)}" ry="${f1(lh * 0.4)}" fill="#ff6a3a" opacity=".35" filter="url(#P-blur30)"/>`;
    let crowd = "";
    for (let x = lx; x < lx + lw; x += 13 + r() * 9) { const h = 24 + r() * 26; crowd += `<ellipse cx="${f1(x)}" cy="${f1(ly + lh - h)}" rx="7" ry="9"/><rect x="${f1(x - 12)}" y="${f1(ly + lh - h + 6)}" width="24" height="${f1(h)}" rx="9"/>${r() > 0.82 ? `<path d="M${f1(x + 6)} ${f1(ly + lh - h)} l${f1(r() * 10 - 5)} -34" stroke-width="5" stroke-linecap="round"/>` : ""}`; }
    void crowd;   // the crowd itself is an overlay (stageCrowd), so it can move with the music; the loop above keeps the seed sequence
    b += `<rect x="${f1(lx)}" y="${ly}" width="${f1(lw)}" height="${lh}" fill="url(#P-pix)"/>`;
    // ground-support towers and the front truss, fixtures hung on it (the beams over the gear come from these)
    const tower = (x) => `<g><rect x="${f1(x - 13)}" y="20" width="26" height="430" fill="url(#P-trussV)"/><rect x="${f1(x - 13)}" y="20" width="2.5" height="430" fill="#8a8d92"/><rect x="${f1(x + 10.5)}" y="20" width="2.5" height="430" fill="#8a8d92"/>
      <rect x="${f1(x - 30)}" y="436" width="60" height="12" fill="#2a2b2e"/></g>`;
    b += tower(cx - lw / 2 - 70) + tower(cx + lw / 2 + 70);
    b += `<rect x="-900" y="6" width="${W + 1800}" height="30" fill="url(#P-trussH)"/><rect x="-900" y="6" width="${W + 1800}" height="2.5" fill="#9a9da2"/><rect x="-900" y="33" width="${W + 1800}" height="2.5" fill="#6a6d72"/>`;
    // upstage truss above the LED wall with blinders
    b += `<rect x="${f1(lx - 60)}" y="44" width="${f1(lw + 120)}" height="16" fill="url(#P-trussH)" opacity=".8"/>`;
    for (let i = 0; i < 6; i++) { const x = lx + (i + 0.5) * lw / 6; b += `<rect x="${f1(x - 18)}" y="60" width="36" height="16" rx="2" fill="#1a1b1e"/>${[0, 1, 2, 3].map((k) => `<circle cx="${f1(x - 12 + k * 8)}" cy="68" r="3" fill="#ffcf8a" opacity=".85"/>`).join("")}`; }
    // the drum riser: skirted platform with a light strip, the kit on it
    const rw = Math.min(680, W * 0.56);
    b += `<rect x="${f1(cx - rw / 2)}" y="384" width="${f1(rw)}" height="12" fill="#1f2024"/><rect x="${f1(cx - rw / 2)}" y="396" width="${f1(rw)}" height="50" fill="url(#P-skirt)"/><rect x="${f1(cx - rw / 2)}" y="396" width="${f1(rw)}" height="2.5" fill="#7fa0ff" opacity=".9"/>`;
    const kit = (x, y) => `<g transform="translate(${f1(x)} ${y})">
      <path d="M-230 -30 L-226 -210 M230 -30 L224 -230 M-170 -40 L-178 -170 M180 -40 L186 -180" stroke="#9a9da2" stroke-width="2.4"/>
      <ellipse cx="-226" cy="-210" rx="52" ry="9" fill="url(#P-cym)"/><ellipse cx="224" cy="-230" rx="58" ry="10" fill="url(#P-cym)"/><ellipse cx="-178" cy="-170" rx="38" ry="7" fill="url(#P-cym)"/><ellipse cx="186" cy="-180" rx="44" ry="8" fill="url(#P-cym)"/>
      <path d="M-150 -30 L-150 -110 M150 -30 L150 -100" stroke="#9a9da2" stroke-width="2"/><ellipse cx="-150" cy="-110" rx="26" ry="5" fill="url(#P-cym)"/><ellipse cx="-150" cy="-104" rx="26" ry="5" fill="url(#P-cym)"/>
      <rect x="128" y="-74" width="56" height="64" rx="7" fill="#4a0f14"/><ellipse cx="156" cy="-74" rx="28" ry="6" fill="#e9e4da"/>
      <rect x="-56" y="-108" width="44" height="34" rx="6" fill="#4a0f14"/><rect x="12" y="-112" width="48" height="38" rx="6" fill="#4a0f14"/><ellipse cx="-34" cy="-108" rx="22" ry="5" fill="#e9e4da"/><ellipse cx="36" cy="-112" rx="24" ry="5" fill="#e9e4da"/>
      <circle cx="0" cy="-40" r="42" fill="#5a1218"/><circle cx="0" cy="-40" r="36" fill="#141012"/><circle cx="0" cy="-40" r="22" fill="none" stroke="#e3a04b" stroke-width="3"/><text x="0" y="-35" font-size="13" font-weight="800" text-anchor="middle" fill="#e3a04b" font-family="system-ui, sans-serif">HR</text>
      <rect x="66" y="-60" width="48" height="56" rx="6" fill="#4a0f14"/><ellipse cx="90" cy="-60" rx="24" ry="5" fill="#e9e4da"/>
      <rect x="-118" y="-70" width="40" height="22" rx="5" fill="#8d9097"/><ellipse cx="-98" cy="-70" rx="20" ry="4" fill="#e9e4da"/></g>`;
    b += kit(cx, 384);
    // side fills on the wings: tall stacks of boxes with grilles
    const fill = (x) => `<g>${[0, 1, 2].map((k) => `<rect x="${f1(x - 60)}" y="${f1(226 + k * 74)}" width="120" height="70" rx="3" fill="#1d1e22"/><rect x="${f1(x - 52)}" y="${f1(232 + k * 74)}" width="104" height="58" fill="url(#P-grille)"/><rect x="${f1(x - 60)}" y="${f1(226 + k * 74)}" width="120" height="1.5" fill="rgba(255,170,200,.35)"/>`).join("")}
      <rect x="${f1(x - 60)}" y="${226 - 82}" width="120" height="78" rx="3" fill="#16171a"/><circle cx="${f1(x)}" cy="${226 - 43}" r="26" fill="#0b0b0d"/><circle cx="${f1(x)}" cy="${226 - 43}" r="10" fill="#1d1e22"/></g>`;
    b += fill(Math.max(70, cx - lw / 2 - 170)) + fill(Math.min(W - 70, cx + lw / 2 + 170));
    // the deck: black ply in 8×4 sheets running at you, scuffed; spike tape, a cable run, a set list taped down
    b += `<rect x="-900" y="446" width="${W + 1800}" height="854" fill="#0f0e10"/>`;
    for (let X = -2440; X < 2440; X += 122) {
      b += `<polygon points="${quad(cx, X, X + 121, 446, 1300)}" fill="${mix("#0e0d10", "#19171b", r())}"/><line x1="${f1(gx(cx, X, 446))}" y1="446" x2="${f1(gx(cx, X, 1300))}" y2="1300" stroke="#050506" stroke-width="1.5"/>`;
      for (let Z = 1.1 - r() * 0.3; Z > 0.38; Z -= 0.42) { const y = zy(Z); b += `<line x1="${f1(gx(cx, X, y))}" y1="${f1(y)}" x2="${f1(gx(cx, X + 121, y))}" y2="${f1(y)}" stroke="#050506" stroke-width="${f1((y - HZ) / FB)}"/>`; }
    }
    b += `<rect x="-900" y="446" width="${W + 1800}" height="854" fill="#fff" filter="url(#P-grain)" opacity=".28"/>`;
    for (let i = 0; i < 7; i++) { const X = (r() - 0.5) * 1500, Z = 0.55 + r() * 0.55, y = zy(Z), s = (y - HZ) / FB; b += `<path d="M${f1(gx(cx, X, y) - 6 * s)} ${f1(y)} h${f1(12 * s)} M${f1(gx(cx, X, y))} ${f1(y - 2.5 * s)} v${f1(5 * s)}" stroke="${r() > 0.5 ? "#c9b45a" : "#bdbdbd"}" stroke-width="${f1(2 * s)}" opacity=".45"/>`; }
    // floor wedges either side of where you stand
    [-1, 1].forEach((side) => { const Z = 0.6, y = zy(Z), s = (y - HZ) / FB, x = gx(cx, side * 640, y), w = 150 * s, h = 62 * s;
      b += `<ellipse cx="${f1(x)}" cy="${f1(y + 4 * s)}" rx="${f1(w * 0.6)}" ry="${f1(10 * s)}" fill="#000" opacity=".7" filter="url(#P-blur2)"/>
        <path d="M${f1(x - w / 2)} ${f1(y)} L${f1(x - w * 0.42)} ${f1(y - h)} L${f1(x + w * 0.42)} ${f1(y - h)} L${f1(x + w / 2)} ${f1(y)}Z" fill="#17181b"/>
        <path d="M${f1(x - w * 0.36)} ${f1(y - h * 0.9)} L${f1(x + w * 0.36)} ${f1(y - h * 0.9)} L${f1(x + w * 0.4)} ${f1(y - h * 0.12)} L${f1(x - w * 0.4)} ${f1(y - h * 0.12)}Z" fill="url(#P-grille)"/>
        <path d="M${f1(x - w * 0.42)} ${f1(y - h)} L${f1(x + w * 0.42)} ${f1(y - h)}" stroke="rgba(255,150,180,.45)" stroke-width="${f1(1.4 * s)}"/>`; });
    b += `<path d="M${f1(cx - W * 0.5)} 600 C${f1(cx - W * 0.3)} 560 ${f1(cx - W * 0.2)} 640 ${f1(cx - 40)} 610 S ${f1(cx + W * 0.2)} 560 ${f1(cx + W * 0.55)} 600" stroke="#050506" stroke-width="5" fill="none" opacity=".9"/>`;
    b += `<g transform="translate(${f1(cx + W * 0.36)} 700) rotate(-8)"><path d="M0 0 l60 -6 l10 50 l-62 8z" fill="#e8e4da"/>${[0, 1, 2, 3, 4].map((k) => `<path d="M8 ${8 + k * 8} l44 -4" stroke="#333" stroke-width="2"/>`).join("")}<path d="M-4 -2 l18 -2 M50 -8 l18 -2" stroke="#d9c88a" stroke-width="6" opacity=".8"/></g>`;
    b += `<rect x="-900" y="446" width="${W + 1800}" height="854" fill="url(#P-deckglow)"/>`;
    const defs = `<linearGradient id="P-cyc" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#020103"/><stop offset=".6" stop-color="#0a0610"/><stop offset=".88" stop-color="#1c0a1a"/><stop offset="1" stop-color="#0a0508"/></linearGradient>
      <linearGradient id="P-led" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2a0d3a"/><stop offset=".45" stop-color="#7a1e3a"/><stop offset=".7" stop-color="#e2582e"/><stop offset="1" stop-color="#3a1240"/></linearGradient>
      <pattern id="P-pix" width="5" height="5" patternUnits="userSpaceOnUse"><rect width="5" height="5" fill="#000" opacity=".35"/><rect width="3.6" height="3.6" fill="#000" opacity="0"/><rect x="3.6" width="1.4" height="5" fill="#000" opacity=".55"/><rect y="3.6" width="5" height="1.4" fill="#000" opacity=".55"/></pattern>
      <pattern id="P-trussH" width="30" height="30" patternUnits="userSpaceOnUse"><path d="M0 0 L15 30 L30 0" stroke="#5d6065" stroke-width="2" fill="none"/></pattern>
      <pattern id="P-trussV" width="26" height="30" patternUnits="userSpaceOnUse"><path d="M0 0 L26 15 L0 30" stroke="#5d6065" stroke-width="2" fill="none"/></pattern>
      <linearGradient id="P-skirt" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#0b0b0d"/><stop offset=".5" stop-color="#151518"/><stop offset="1" stop-color="#0b0b0d"/></linearGradient>
      <linearGradient id="P-cym" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#7a5a20"/><stop offset=".5" stop-color="#e9c46a"/><stop offset="1" stop-color="#7a5a20"/></linearGradient>
      <pattern id="P-grille" width="5" height="5" patternUnits="userSpaceOnUse"><rect width="5" height="5" fill="#101114"/><circle cx="2.5" cy="2.5" r="1.3" fill="#34363c"/></pattern>
      <radialGradient id="P-deckglow" cx=".5" cy=".0" r=".75"><stop offset="0" stop-color="#ff6a5a" stop-opacity=".12"/><stop offset=".35" stop-color="#000" stop-opacity=".1"/><stop offset="1" stop-color="#000" stop-opacity=".6"/></radialGradient>`;
    return wrapSVG(P, W, b, defs);
  }

  /* ---------- Outdoors: a mountain meadow at golden hour ---------- */
  /* A mountain ridge by midpoint displacement: natural peaks and saddles. */
  function ridgePts(W, base, amp, seed) {
    const r = rng(seed), n = 128, pts = new Array(n + 1);
    pts[0] = base - amp * r(); pts[n] = base - amp * r();
    for (let step = n; step > 1; step /= 2) {
      for (let i = step / 2; i < n; i += step) pts[i] = (pts[i - step / 2] + pts[i + step / 2]) / 2 + (r() - 0.5) * amp * (step / n) * 2.4;
    }
    return pts.map((y, i) => [-900 + i * (W + 1800) / n, Math.min(base + 30, y)]);
  }
  const ridgeD = (pts) => `M${pts.map(([x, y]) => `${f1(x)} ${f1(y)}`).join(" L")} L${f1(pts[pts.length - 1][0])} 1300 L-900 1300Z`;
  /* A pine silhouette: tiers of jagged boughs narrowing to the tip (seeded). */
  function pine(x, base, h, w, c, seed) {
    const r = rng(seed), tiers = 7 + Math.round(r() * 3);
    const L = [], Rr = [];
    for (let i = 0; i <= tiers; i++) {
      const t = i / tiers, y = base - h * (0.08 + 0.92 * t), half = (w / 2) * Math.pow(1 - t, 0.9) * (0.75 + r() * 0.3), droop = h * 0.035 * (1 - t);
      L.push(`${f1(x - half)} ${f1(y + droop)}`, `${f1(x - half * 0.38)} ${f1(y - h * 0.02)}`);
      Rr.unshift(`${f1(x + half * 0.38)} ${f1(y - h * 0.02)}`, `${f1(x + half * (0.9 + r() * 0.2))} ${f1(y + droop)}`);
    }
    return `<path d="M${f1(x - w * 0.04)} ${base} L${L.join(" L")} L${f1(x)} ${f1(base - h)} L${Rr.join(" L")} L${f1(x + w * 0.04)} ${base}Z" fill="${c}"/>`;
  }
  function outdoorsSVG(W, P) {
    const cx = W / 2, r = rng(31), sunX = cx + W * 0.3, sunY = 250;
    let b = `<rect x="-900" y="-700" width="${W + 1800}" height="2000" fill="url(#P-sky)"/>`;
    b += `<circle cx="${f1(sunX)}" cy="${sunY}" r="260" fill="url(#P-sunbloom)"/>`;
    b += `<g opacity=".5">${Array.from({ length: 22 }, (_, i) => { const a1 = (i / 22) * Math.PI * 2 + 0.05, a2 = a1 + 0.06; return `<path d="M${f1(sunX)} ${sunY} L${f1(sunX + Math.cos(a1) * 700)} ${f1(sunY + Math.sin(a1) * 700)} L${f1(sunX + Math.cos(a2) * 700)} ${f1(sunY + Math.sin(a2) * 700)}Z" fill="url(#P-ray)"/>`; }).join("")}</g>`;
    // clouds: soft banks lit from below by the low sun
    const cloud = (x, y, w, h, op) => `<g opacity="${op}">${Array.from({ length: 9 }, (_, i) => `<ellipse cx="${f1(x + (i - 4) * w / 9 + r() * 12)}" cy="${f1(y + r() * h * 0.4)}" rx="${f1(w / 7 + r() * w / 10)}" ry="${f1(h * (0.5 + r() * 0.4))}" fill="url(#P-cloud)"/>`).join("")}</g>`;
    b += `<g filter="url(#P-blur2)">${cloud(cx - W * 0.3, 40, 520, 26, 0.85)}${cloud(cx + W * 0.1, -10, 620, 30, 0.7)}${cloud(cx + W * 0.45, 110, 420, 18, 0.9)}${cloud(cx - W * 0.55, 150, 380, 16, 0.8)}</g>`;
    b += `<circle cx="${f1(sunX)}" cy="${sunY}" r="26" fill="#fffbe8"/><circle cx="${f1(sunX)}" cy="${sunY}" r="60" fill="#ffe2a0" opacity=".5" filter="url(#P-blur12)"/>`;
    // mountains: a far snowy range (snow above a ragged snowline, sunlit faces warm), nearer ranges fading into haze
    const far = ridgePts(W, 310, 260, 7);
    let snowline = `M-900 -700 L${W + 900} -700`;
    for (let x = W + 900; x >= -900; x -= 40) snowline += ` L${x} ${f1(226 + Math.sin(x / 47) * 12 + Math.sin(x / 13) * 7)}`;
    b += `<clipPath id="P-mclip"><path d="${ridgeD(far)}"/></clipPath>`;
    b += `<path d="${ridgeD(far)}" fill="#8676a2"/><g clip-path="url(#P-mclip)"><path d="${snowline}Z" fill="url(#P-snow)"/><rect x="-900" y="-700" width="${W + 1800}" height="1000" fill="url(#P-sunside)"/></g>`;
    b += `<rect x="-900" y="260" width="${W + 1800}" height="170" fill="url(#P-haze)" opacity=".75"/>`;
    b += `<path d="${ridgeD(ridgePts(W, 350, 110, 19))}" fill="#6a5a86"/><rect x="-900" y="300" width="${W + 1800}" height="150" fill="url(#P-haze)" opacity=".8"/>`;
    // the lake in the valley, the sun's path glittering on it
    b += `<rect x="-900" y="372" width="${W + 1800}" height="60" fill="url(#P-lake)"/>`;
    for (let i = 0; i < 26; i++) { const y = 376 + r() * 50, w = 6 + r() * 40 * (1 - (y - 376) / 70); b += `<rect x="${f1(sunX - w / 2 + (r() - 0.5) * 50)}" y="${f1(y)}" width="${f1(w)}" height="1.4" fill="#fff2c8" opacity="${f1(0.4 + r() * 0.5)}"/>`; }
    b += `<path d="${ridgeD(ridgePts(W, 410, 40, 41))}" fill="#3c3a52"/>`;
    // a forested near ridge
    let trees = "";
    for (let x = -880; x < W + 880; x += 9 + r() * 10) { const h = 18 + r() * 26, y = 412 + Math.sin(x / 140) * 8 + r() * 6; trees += `<path d="M${f1(x)} ${f1(y - h)} L${f1(x - h * 0.22)} ${f1(y)} L${f1(x + h * 0.22)} ${f1(y)}Z"/>`; }
    b += `<g fill="#2a3a36">${trees}</g><rect x="-900" y="380" width="${W + 1800}" height="70" fill="url(#P-hazelow)"/>`;
    // the meadow: golden grass far away, deep green near, a texture of blades
    b += `<rect x="-900" y="440" width="${W + 1800}" height="860" fill="url(#P-meadow)"/><rect x="-900" y="440" width="${W + 1800}" height="860" fill="#fff" filter="url(#P-grassnoise)" opacity=".5"/>`;
    let blades = "";
    for (let i = 0; i < 1400; i++) {
      const y = 446 + Math.pow(r(), 0.7) * 860, s = (y - HZ) / FB, x = -900 + r() * (W + 1800), h = (6 + r() * 10) * s, lean = (r() - 0.4) * 5 * s;
      const lit = r() < 0.5;
      blades += `<path d="M${f1(x)} ${f1(y)} q${f1(lean * 0.4)} ${f1(-h * 0.6)} ${f1(lean)} ${f1(-h)}" stroke="${lit ? "#d9c26a" : "#4f6a2e"}" stroke-width="${f1(Math.max(0.6, 1.1 * s))}" opacity="${lit ? 0.55 : 0.6}"/>`;
    }
    b += `<g fill="none" stroke-linecap="round">${blades}</g>`;
    for (let i = 0; i < 90; i++) { const y = 470 + r() * 800, s = (y - HZ) / FB; b += `<circle cx="${f1(-900 + r() * (W + 1800))}" cy="${f1(y)}" r="${f1(1.6 * s)}" fill="${["#f2e8d0", "#e6c84a", "#c45ab8", "#f0f0ff"][Math.floor(r() * 4)]}" opacity=".85"/>`; }
    // rocks
    [[-0.42, 0.9, 70], [0.46, 0.75, 54], [-0.6, 0.6, 90]].forEach(([fxp, Z, w]) => { const y = zy(Z), s = (y - HZ) / FB, x = cx + fxp * W * 1.1; b += `<ellipse cx="${f1(x + 10 * s)}" cy="${f1(y + 4 * s)}" rx="${f1(w * s * 0.7)}" ry="${f1(10 * s)}" fill="rgba(20,30,10,.45)" filter="url(#P-blur2)"/><path d="M${f1(x - w * s / 2)} ${f1(y)} q${f1(w * s * 0.1)} ${f1(-26 * s)} ${f1(w * s * 0.45)} ${f1(-30 * s)} q${f1(w * s * 0.4)} ${f1(4 * s)} ${f1(w * s * 0.55)} ${f1(30 * s)}z" fill="#6a6658"/><path d="M${f1(x)} ${f1(y - 28 * s)} q${f1(w * s * 0.3)} ${f1(2 * s)} ${f1(w * s * 0.48)} ${f1(26 * s)}" stroke="#e9b36a" stroke-width="${f1(2.5 * s)}" fill="none" opacity=".7"/>`; });
    // a low wooden riser the amps stand on, then the meadow, and a rug on the grass for the pedalboard
    const RX = Math.min(W * 0.62, 760), Zb = 1.13, Zf = 0.93, yb = zy(Zb), yf = zy(Zf), lip = 14;
    b += `<polygon points="${quad(cx, -RX - 20, RX + 20, yb + 4, yf + lip + 10)}" fill="rgba(20,26,10,.55)" filter="url(#P-soft)"/>`;
    for (let X = -RX; X < RX; X += 48) {
      b += `<polygon points="${quad(cx, X, X + 47, yb, yf)}" fill="${mix("#7a5636", "#b08860", r())}"/><line x1="${f1(gx(cx, X + 47, yb))}" y1="${f1(yb)}" x2="${f1(gx(cx, X + 47, yf))}" y2="${f1(yf)}" stroke="#3a2614" stroke-width="1.2"/>`;
    }
    b += `<polygon points="${quad(cx, -RX, RX, yb, yf)}" fill="#fff" filter="url(#P-grain)" opacity=".4"/><polygon points="${quad(cx, -RX, RX, yb, yf)}" fill="url(#P-deckwarm)"/>`;
    b += `<rect x="${f1(gx(cx, -RX, yf))}" y="${f1(yf)}" width="${f1(gx(cx, RX, yf) - gx(cx, -RX, yf))}" height="${lip}" fill="url(#P-riserface)"/><rect x="${f1(gx(cx, -RX, yf))}" y="${f1(yf)}" width="${f1(gx(cx, RX, yf) - gx(cx, -RX, yf))}" height="2" fill="#ffd08a" opacity=".55"/>`;
    b += `<polygon points="${f1(gx(cx, RX, yb))},${f1(yb)} ${f1(gx(cx, RX, yf))},${f1(yf)} ${f1(gx(cx, RX, yf))},${f1(yf + lip)} ${f1(gx(cx, RX, yb))},${f1(yb + lip * 0.8)}" fill="#c08a52"/>`;
    b += `<polygon points="${f1(gx(cx, -RX, yb))},${f1(yb)} ${f1(gx(cx, -RX, yf))},${f1(yf)} ${f1(gx(cx, -RX, yf))},${f1(yf + lip)} ${f1(gx(cx, -RX, yb))},${f1(yb + lip * 0.8)}" fill="#3a2614"/>`;
    b += rugSVG(cx, 250, 0.8, 0.45, { edge: "#2a1d14", b1: "#d9a24a", b2: "#2a5560", field: "#a8442a", m1: "#ecd7a8", m2: "#2a5560", fringe: "#efe0c0" });
    // framing pines at the edges, rim-lit by the low sun
    b += `<g filter="url(#P-rim)">${pine(-60, 560, 520, 210, "#0c130e", 3)}${pine(70, 520, 400, 160, "#101a13", 7)}${pine(W + 80, 580, 560, 220, "#0c130e", 13)}${pine(W - 40, 520, 380, 150, "#121c15", 17)}</g>`;
    const defs = `<linearGradient id="P-sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1e2a5a"/><stop offset=".3" stop-color="#3a4a8a"/><stop offset=".42" stop-color="#9a76a6"/><stop offset=".48" stop-color="#f09a7a"/><stop offset=".52" stop-color="#ffcf8a"/><stop offset="1" stop-color="#ffd89a"/></linearGradient>
      <radialGradient id="P-ray" cx="${f1(cx + W * 0.3)}" cy="250" r="700" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#ffe2a8" stop-opacity=".45"/><stop offset=".5" stop-color="#ffd090" stop-opacity=".08"/><stop offset="1" stop-color="#ffd090" stop-opacity="0"/></radialGradient>
      <radialGradient id="P-sunbloom" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#fff2c0" stop-opacity=".9"/><stop offset=".25" stop-color="#ffc070" stop-opacity=".45"/><stop offset="1" stop-color="#ff9a5a" stop-opacity="0"/></radialGradient>
      <radialGradient id="P-cloud" cx=".5" cy=".7" r=".6"><stop offset="0" stop-color="#ffd2b0"/><stop offset=".6" stop-color="#e4a0a8"/><stop offset="1" stop-color="#8a7aa8" stop-opacity="0"/></radialGradient>
      <linearGradient id="P-haze" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffc89a" stop-opacity="0"/><stop offset="1" stop-color="#ffcf9a" stop-opacity=".5"/></linearGradient>
      <linearGradient id="P-hazelow" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffd8a0" stop-opacity=".0"/><stop offset=".6" stop-color="#ffd8a0" stop-opacity=".35"/><stop offset="1" stop-color="#ffd8a0" stop-opacity="0"/></linearGradient>
      <linearGradient id="P-lake" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#e8a68a"/><stop offset=".5" stop-color="#9a7aa0"/><stop offset="1" stop-color="#4a4a70"/></linearGradient>
      <linearGradient id="P-meadow" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#c8a84a"/><stop offset=".15" stop-color="#8a8a32"/><stop offset=".45" stop-color="#4f6a26"/><stop offset="1" stop-color="#22361a"/></linearGradient>
      <filter id="P-grassnoise" x="0" y="0" width="100%" height="100%"><feTurbulence type="fractalNoise" baseFrequency=".02 .35" numOctaves="3" seed="12"/><feColorMatrix values="0 0 0 0 0.1  0 0 0 0 0.15  0 0 0 0 0  0 0 0 .7 -.25"/><feComposite in2="SourceGraphic" operator="in"/></filter>
      <linearGradient id="P-snow" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff2ea"/><stop offset="1" stop-color="#e8c8d4"/></linearGradient>
      <linearGradient id="P-sunside" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#3a3060" stop-opacity=".35"/><stop offset=".55" stop-color="#3a3060" stop-opacity="0"/><stop offset="1" stop-color="#ff9a5a" stop-opacity=".35"/></linearGradient>
      <linearGradient id="P-riserface" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#6b4a2c"/><stop offset="1" stop-color="#2a1a0e"/></linearGradient>
      <filter id="P-rim" x="-10%" y="-10%" width="120%" height="120%"><feOffset in="SourceAlpha" dx="-3" dy="2" result="o"/><feComposite in="SourceAlpha" in2="o" operator="out" result="edge"/><feGaussianBlur in="edge" stdDeviation=".7" result="eb"/><feFlood flood-color="#ffc070" flood-opacity=".85"/><feComposite in2="eb" operator="in" result="rim"/><feMerge><feMergeNode in="SourceGraphic"/><feMergeNode in="rim"/></feMerge></filter>
      <linearGradient id="P-deckwarm" x1="1" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffb060" stop-opacity=".35"/><stop offset=".6" stop-color="#ffb060" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity=".25"/></linearGradient>`;
    return wrapSVG(P, W, b, defs);
  }
  const ART = { studio: studioSVG, living: livingSVG, stage: stageSVG, outdoors: outdoorsSVG };

  /* ---------- the scene layers: a cached image of the backdrop + a few animated overlays ----------
     Each scene's SVG is generated once, turned into a blob-URL image and decoded once; switching scenes crossfades two
     layers with opacity only. Overlays animate only transform / opacity (so the compositor runs them), and nothing in
     the Room animates or draws while it isn't on screen. */
  const sceneCache = new Map();   // id -> { url, canvas (the backdrop as a bitmap) | null, rasterP, thumb (small PNG data URL) }
  const RASTER_K = Math.min(1.25, Math.max(1, window.devicePixelRatio || 1));   // backdrop bitmap scale (it sits behind the gear)
  function sceneArt(id) {
    let c = sceneCache.get(id);
    if (c) return c;
    c = { url: URL.createObjectURL(new Blob([ART[id](ART_W, "sre")], { type: "image/svg+xml" })), canvas: null, rasterP: null, thumb: "" };
    sceneCache.set(id, c);
    return c;
  }
  /* Paint the scene's SVG into a bitmap once (the SVG's filters are only ever evaluated here), and cut the picker's
     miniature from it. Afterwards the backdrop is a plain bitmap: moving, showing or compositing it never re-renders it. */
  function rasterize(id) {
    const c = sceneArt(id);
    if (c.canvas) return Promise.resolve(c.canvas);
    if (c.rasterP) return c.rasterP;
    c.rasterP = new Promise((resolve) => {
      const img = new Image(AVW, AVH);
      img.decoding = "async";
      img.onload = () => resolve(img); img.onerror = () => resolve(null);
      img.src = c.url;
    }).then((img) => {
      const cv = document.createElement("canvas");
      cv.className = "sr-bgimg";
      cv.width = Math.round(AVW * RASTER_K); cv.height = Math.round(AVH * RASTER_K);
      if (img) cv.getContext("2d").drawImage(img, 0, 0, cv.width, cv.height);
      if (!c.thumb && img) {
        try {
          const t = document.createElement("canvas"); t.width = 208; t.height = 140;
          t.getContext("2d").drawImage(cv, (90 - AX0) * RASTER_K, (0 - AY0) * RASTER_K, 1060 * RASTER_K, 715 * RASTER_K, 0, 0, 208, 140);
          c.thumb = t.toDataURL("image/png");
        } catch (e) { c.thumb = c.url; }   // a tainted canvas (shouldn't happen): fall back to the SVG itself
        const ti = R.root && R.root.querySelector(`.sr-sp-card[data-space="${id}"] img`);
        if (ti) ti.src = c.thumb;
      }
      c.canvas = cv;
      return cv;
    });
    return c.rasterP;
  }
  /* While the Room is visible all four bitmaps stay ready (instant switches); when it's hidden only the current one is kept. */
  function trimRasters(keep) {
    sceneCache.forEach((c, id) => { if (!keep.includes(id) && c.canvas && !c.canvas.isConnected) { c.canvas.width = c.canvas.height = 0; c.canvas = null; c.rasterP = null; } });
  }
  /* Composited motes: a few specks of dust (or fireflies) drifting in a light, CSS-animated. */
  function motes(n, x, y, w, h, seed, fly) {
    const r = rng(seed); let o = "";
    for (let i = 0; i < n; i++) o += `<i class="sr-mote${fly ? " fly" : ""} m${i % 3}" style="left:${f1(x + r() * w)}px;top:${f1(y + r() * h)}px;--dur:${f1(7 + r() * 7)}s;--del:${f1(-r() * 12)}s"></i>`;
    return o;
  }
  /* The crowd on the LED wall (IMAG), as two interleaved groups that can sway against each other. Same seed as stageSVG. */
  function stageCrowd(W) {
    const cx = W / 2, r = rng(5), lw = Math.min(900, W * 0.64), lx = cx - lw / 2, ly = 70, lh = 290;
    const g = ["", ""];
    let k = 0;
    for (let x = lx; x < lx + lw; x += 13 + r() * 9) {
      const h = 24 + r() * 26, X = x - lx + 20, Y = lh - h;
      g[k++ % 2] += `<ellipse cx="${f1(X)}" cy="${f1(Y)}" rx="7" ry="9"/><rect x="${f1(X - 12)}" y="${f1(Y + 6)}" width="24" height="${f1(h + 16)}" rx="9"/>${r() > 0.82 ? `<path d="M${f1(X + 6)} ${f1(Y)} l${f1(r() * 10 - 5)} -34" stroke-width="5" stroke-linecap="round"/>` : ""}`;
    }
    const svg = (body, i) => `<svg class="sr-p sr-crowd" data-p="crowd" data-i="${i}" width="${f1(lw + 40)}" height="${lh}" viewBox="0 0 ${f1(lw + 40)} ${lh}" aria-hidden="true"><g fill="#0a0610" stroke="#0a0610" opacity=".9">${body}</g></svg>`;
    return `<div class="sr-crowdbox" style="left:${f1(lx - 20)}px;top:${ly}px;width:${f1(lw + 40)}px;height:${lh}px"><div class="sr-p sr-crowdj" data-p="jump">${svg(g[0], 0)}${svg(g[1], 1)}</div></div>`;
  }
  /* Outdoors: festoon lights strung between the pines (wire and bulbs in the art layer, their glow a separate layer that breathes). */
  function festoon(W) {
    const runs = [[-60, 26, W * 0.44, 58, 70], [W * 0.56, 52, W + 60, 22, 66]];
    let wire = "", bulbs = "";
    const glow = ["", "", ""];
    let bi = 0;
    runs.forEach(([x0, y0, x1, y1, sag]) => {
      const pt = (t) => ({ x: x0 + (x1 - x0) * t, y: y0 + (y1 - y0) * t + sag * 4 * t * (1 - t) });
      const m = pt(0.5);
      wire += `<path d="M${f1(x0)} ${f1(y0)} Q${f1(2 * m.x - (x0 + x1) / 2)} ${f1(2 * m.y - (y0 + y1) / 2)} ${f1(x1)} ${f1(y1)}" stroke="#1a140e" stroke-width="1.4" fill="none"/>`;
      const n = Math.round((x1 - x0) / 40);
      for (let i = 1; i < n; i++) {
        const p = pt(i / n);
        bulbs += `<path d="M${f1(p.x)} ${f1(p.y)} v5" stroke="#1a140e" stroke-width="1.6"/><ellipse cx="${f1(p.x)}" cy="${f1(p.y + 10)}" rx="3.4" ry="5" fill="#ffe2a0"/><ellipse cx="${f1(p.x - 1)}" cy="${f1(p.y + 8.6)}" rx="1.2" ry="2" fill="#fffbe8"/>`;
        glow[bi++ % 3] += `<circle cx="${f1(p.x)}" cy="${f1(p.y + 10)}" r="26" fill="url(#srbulb)"/>`;
      }
    });
    return `<svg class="sr-festoon" width="${W}" height="200" viewBox="0 0 ${W} 200" aria-hidden="true">${wire}${bulbs}</svg>
      <svg width="0" height="0" style="position:absolute" aria-hidden="true"><defs><radialGradient id="srbulb"><stop offset="0" stop-color="#fff0c0" stop-opacity=".95"/><stop offset=".3" stop-color="#ffc870" stop-opacity=".4"/><stop offset="1" stop-color="#ffb860" stop-opacity="0"/></radialGradient></defs></svg>
      ${glow.map((g, i) => `<svg class="sr-p sr-festglow" data-p="lights" data-i="${i}" width="${W}" height="200" viewBox="0 0 ${W} 200" aria-hidden="true">${g}</svg>`).join("")}`;
  }
  /* Each scene's animated overlays. [data-p] ones follow the music (see "the room listens"); the rest are CSS-animated. */
  /* Each scene's light, all of it IN the scene and behind the gear (the back layer): visible sources (truss heads,
     sconces, LED strips, the fire, lamps, string lights, the sun) and the surfaces their light lands on (back wall,
     haze, floor, ceiling, sky, grass). [data-p] parts follow the music (see "the room listens"); the rest are
     CSS-animated. Nothing is laid over the gear: the gear only gets a thin rim from the scene's key light, a cast
     shadow on the floor, and its own tubes, lamps and meters. */
  function fxBackHTML(id, W) {
    const cx = W / 2, P = (p, cls, st, i = 0) => `<i class="sr-p ${cls}" data-p="${p}" data-i="${i}" style="${st}"></i>`;
    if (id === "studio") {
      // practicals: the two sconces and their scallops on the wall, an LED strip along the top of the slat wall
      // (and one along the skirting), the studio's real meters behind the glass
      const wx = cx + W * 0.2, wy = 96, ww = Math.min(250, W * 0.2), n = 15;
      let m = "";
      for (let i = 0; i < n; i++) m += `<i class="sr-p sr-mbar" data-p="meter" data-i="${i}" style="left:${f1(wx + 14 + i * (ww - 28) / n)}px;top:${wy + 96}px;width:${f1((ww - 28) / n - 3)}px"></i>`;
      return [100, W - 70].map((x) => `<i class="sr-flick" style="left:${f1(x)}px;top:150px"></i>`).join("") +
        [100, W - 70].map((x, i) => P("sconce", "sr-scallop", `left:${f1(x)}px;top:150px`, i)).join("") +
        P("strip", "sr-ledstrip top", `left:30px;top:58px;width:${W - 60}px`) + P("strip", "sr-ledstrip low", `left:30px;top:433px;width:${W - 60}px`, 1) + m +
        [wx + 29, wx + ww - 29].map((x, i) => P("vu", "sr-pglow vu", `left:${f1(x)}px;top:${wy + 63}px;width:70px;height:50px`, i) + P("needle", "sr-needle", `left:${f1(x)}px;top:${wy + 72}px`, i)).join("") +
        motes(6, 40, 120, 140, 220, 3) + motes(6, W - 140, 120, 140, 220, 4);
    }
    if (id === "living") {
      // the fire lights its own chimney breast, the ceiling over it and the floor in front of the hearth (it flickers);
      // the lamps light the wall around their shades
      const fx = cx - W * 0.21, wx = cx + W * 0.22 + 150;
      return `${P("firewall", "sr-firewall", `left:${f1(fx)}px;top:360px`)}${P("fireceil", "sr-fireceil", `left:${f1(fx)}px;top:0`)}
        <div class="sr-fire sr-p" data-p="fire" style="left:${f1(fx)}px"><b class="embers"></b>${[0, 1, 2, 3, 4, 5, 6].map((i) => `<i style="--fi:${i}"></i>`).join("")}</div><i class="sr-firelight" style="left:${f1(fx)}px"></i>
        ${P("flare", "sr-pglow fire", `left:${f1(fx)}px;top:400px;width:300px;height:150px`)}${P("hearth", "sr-firefloor", `left:${f1(fx)}px;top:452px`)}
        ${[-28, 0, 28].map((d, i) => P("candle", "sr-candle", `left:${f1(fx + d + 0.5)}px;top:${222 - (d === 0 ? 8 : 0)}px`, i)).join("")}
        <i class="sr-flick lamp" style="left:${f1(wx - 300)}px;top:286px"></i>
        ${P("lamp", "sr-scallop lamp", `left:${f1(wx - 300)}px;top:286px`)}${P("lamp", "sr-scallop lamp arc", `left:${f1(wx + 110)}px;top:226px`, 1)}
        <div class="sr-p sr-pmotes" data-p="motes">${motes(8, fx - 120, 300, 240, 200, 5)}${motes(6, wx - 60, 160, 240, 260, 6)}</div>`;
    }
    if (id === "stage") {
      // the show: moving heads on the upstage truss (behind the backline) firing beams down through the haze, landing
      // as pools on the deck; washes on the back wall; the towers' beams; blinders on the big hits; the video wall
      const lw = Math.min(900, W * 0.64), lx = cx - lw / 2, fl = [Math.max(70, cx - lw / 2 - 170), Math.min(W - 70, cx + lw / 2 + 170)];
      const wy = zy(0.6), ws = (wy - HZ) / FB, hx = (i) => lx + (i + 0.5) * lw / 6;
      return `<div class="sr-stagefx" style="--ca:#ffb45e;--cb:#ff4f9c">
        ${[0, 1, 2].map((k) => P("wash", `sr-wash w${k}`, `left:${f1(cx)}px;width:${f1(lw + 360)}px`, k)).join("")}
        ${stageCrowd(W)}
        ${P("led", "sr-ledfx", `left:${f1(lx)}px;width:${f1(lw)}px`)}
        ${P("haze", "sr-phaze", "")}
        <div class="sr-p sr-pbeams" data-p="beams">${[-0.42, -0.26, 0.26, 0.42].map((p, i) => `<span class="sr-p sr-beamw" data-p="sweep" data-i="${i}" style="left:${f1(cx + p * W)}px"><i class="sr-sweep" style="--r:${p < 0 ? 18 : -18}deg;--d:${i * 1.7}s;--c:${["#9fb4ff", "#ff6aa0", "#ff6aa0", "#9fb4ff"][i]}"></i></span>`).join("")}</div>
        ${Array.from({ length: 6 }, (_, i) => P("hpool", `sr-hpool ${i % 2 ? "cb" : "ca"}`, "", i)).join("")}
        ${Array.from({ length: 6 }, (_, i) => P("hbeam", `sr-hbeam ${i % 2 ? "cb" : "ca"}`, `left:${f1(hx(i))}px`, i)).join("")}
        ${Array.from({ length: 6 }, (_, i) => P("lens", `sr-lens ${i % 2 ? "cb" : "ca"}`, `left:${f1(hx(i))}px`, i)).join("")}
        ${Array.from({ length: 6 }, (_, i) => P("blinder", "sr-pglow blind", `left:${f1(hx(i))}px;top:68px;width:120px;height:80px`, i)).join("")}
        ${fl.map((x, i) => P("cone", "sr-cone", `left:${f1(x)}px;top:183px`, i)).join("")}
        ${[-1, 1].map((sd, i) => P("wedge", "sr-pglow pink", `left:${f1(gx(cx, sd * 640, wy))}px;top:${f1(wy - 30 * ws)}px;width:${f1(260 * ws)}px;height:${f1(120 * ws)}px`, i)).join("")}
        <i class="sr-haze h1"></i>${motes(14, cx - W * 0.45, 80, W * 0.9, 420, 7)}</div>`;
    }
    if (id === "outdoors") {
      // the low sun and the sky around it, the festoon lights and the warm light they put on the riser and the grass
      const sunX = cx + W * 0.3, sunY = 250;
      return `<i class="sr-cloudfx" style="left:${f1(cx - W * 0.2)}px"></i>
        ${P("sky", "sr-skyglow", `left:${f1(sunX)}px;top:${sunY}px`)}
        ${P("rays", "sr-rays", `left:${f1(sunX)}px;top:${sunY}px`)}${P("sun", "sr-pglow sun", `left:${f1(sunX)}px;top:${sunY}px;width:420px;height:420px`)}
        ${P("glitter", "sr-glitter", `left:${f1(sunX)}px;top:400px`)}
        ${[W * 0.2, W * 0.8].map((x, i) => P("festpool", "sr-festpool", `left:${f1(x)}px;top:470px`, i)).join("")}
        ${festoon(W)}`;
    }
    return "";
  }
  /* In front of the gear: only real objects (grass, fireflies outdoors). No overlays, grades or washes. */
  function fxFrontHTML(id, W) {
    const cx = W / 2;
    let g = "";
    if (id === "outdoors") {
      const tuft = (x, s, fl, d, i) => `<span class="sr-p sr-psway" data-p="sway" data-i="${i}" style="left:${f1(x)}px;width:${f1(160 * s)}px;height:${f1(120 * s)}px"><span class="sr-tuftw" style="left:0;--s:${s};--d:${d}s${fl ? ";--fl:-1" : ""}"><svg class="sr-tuft" viewBox="0 0 160 120" aria-hidden="true">${Array.from({ length: 22 }, (_, i) => { const bx = 10 + i * 6.5, h = 50 + ((i * 37) % 60); return `<path d="M${bx} 120 q${(i % 3) * 4 - 4} ${-h * 0.6} ${(i % 4) * 6 - 8} ${-h}" stroke="${i % 3 ? "#2c4a1c" : "#b8a24a"}" stroke-width="3" fill="none" stroke-linecap="round"/>`; }).join("")}</svg></span></span>`;
      g += tuft(-40, 1.2, false, 0, 0) + tuft(W - 150, 1.1, true, -2, 1);
      g += `<div class="sr-p sr-pflies" data-p="flies" data-i="0">${motes(9, cx - W * 0.5, 470, W * 0.3, 160, 8, true)}</div><div class="sr-p sr-pflies" data-p="flies" data-i="1">${motes(9, cx + W * 0.2, 470, W * 0.3, 160, 9, true)}</div>`;
    }
    return g;
  }

  /* =====================================================================
     THE ROOM LISTENS: each scene's lights and life follow the music
     =====================================================================
     Inputs, with no new polling: the output level (telemetry), the output unit's 28-band spectrum (the same
     telemetry poll: while the Room shows, its last unit is added to HRUnitArt's per-unit query), the chord
     (only if the Listen pane is already polling /api/listen) and the rig's tempo, phase-locked to the onsets.
     Telemetry arrives at ~12.5 Hz; one requestAnimationFrame turns it into smooth envelopes (fast attack, slow
     release) and writes only opacity / transform on a few composited overlays. It runs only while the Room is the
     active, awake view with nothing lifted, and only while there is sound: silence settles to the calm idle look
     and the loop stops. prefers-reduced-motion: lights follow the level slowly; nothing moves or hits. */
  const AU = { tl: 0, tlow: 0, tmid: 0, thigh: 0, thue: 0.08, avg: 0, hitIn: 0, hitN: 0, lastHit: 0, tT: 0, b0: 0,
    lvl: 0, slow: 0, sus: 0, hit: 0, hitS: 0, low: 0, mid: 0, high: 0, hue: 0.08, ph: 0, rayA: 0,
    bands: null, prevB: null, raf: 0, last: 0, els: [], motes: [], rate: 1, rateT: 0, slot: null, patched: false, redT: 0 };
  const pulseOn = () => R.visible && R.built && !document.hidden && !R.focus && !R.drag && (!window.HRActivity || HRActivity.isActive("room"));
  /* The slot whose output is the rig's output: the last unit of the chain (of lane A after a closing split). */
  function outSlot() {
    let L = rackLayout(), it = L[L.length - 1];
    for (let g = 0; it && it.type === "split" && g < 4; g++) { const ln = (it.lanes || []).find((l) => l && l.length); it = ln ? ln[ln.length - 1] : null; }
    return it && it.type === "unit" ? it.slot : null;
  }
  /* Ask for that slot's spectrum on the regular telemetry poll while the Room is showing (HRUnitArt builds the query). */
  function patchQuery() {
    if (AU.patched || !window.HRUnitArt || typeof HRUnitArt.query !== "function") return;
    AU.patched = true;
    const q0 = HRUnitArt.query;
    HRUnitArt.query = function () {
      const q = q0.apply(this, arguments) || "";
      if (AU.slot == null || !R.visible || reduced() || (window.HRActivity && !HRActivity.isActive("room"))) return q;
      const parts = new Map(q.replace(/^\?/, "").split("&").filter(Boolean).map((kv) => { const i = kv.indexOf("="); return [kv.slice(0, i), kv.slice(i + 1)]; }));
      ["units", "spec"].forEach((k) => { const v = (parts.get(k) || "").split(",").filter(Boolean); if (!v.includes(String(AU.slot))) v.push(String(AU.slot)); parts.set(k, v.join(",")); });
      return "?" + [...parts].map(([k, v]) => `${k}=${v}`).join("&");
    };
  }
  /* One telemetry reading in: targets for the envelopes, onsets, the band shape, the chord colour. */
  function auIn(d, db) {
    const tn = now();
    const t = clamp((db + 50) / 44, 0, 1);
    AU.tl = t;
    const u = AU.slot != null && Array.isArray(d.units) ? d.units.find((x) => x && x.s === AU.slot) : null;
    let flux = 0;
    if (u && Array.isArray(u.sp) && u.sp.length >= 28) {
      const b = u.sp.map((v) => clamp((v + 80) / 62, 0, 1));
      const avg = (a, z) => { let s = 0; for (let i = a; i < z; i++) s += b[i]; return s / (z - a); };
      AU.tlow = avg(0, 7); AU.tmid = avg(7, 17); AU.thigh = avg(17, 28);
      if (AU.prevB) for (let i = 0; i < 28; i++) flux += Math.max(0, b[i] - AU.prevB[i]);
      flux /= 7;
      AU.prevB = b; AU.bands = b;
    } else {
      // no spectrum: shape it from the level (highs from how often the output waveform crosses zero)
      const w = d.waveform_out || [];
      let zc = 0;
      for (let i = 1; i < w.length; i++) if ((w[i] >= 0) !== (w[i - 1] >= 0)) zc++;
      AU.bands = null; AU.prevB = null;
      AU.tlow = t * 0.9; AU.tmid = t; AU.thigh = t * clamp(zc / 20, 0.25, 1);
    }
    // an onset: the level jumping over its recent average, or the spectrum lighting up (at most ~8 a second)
    const rise = t - AU.avg;
    AU.avg += (t - AU.avg) * 0.3;
    const k = Math.max(rise * 2.6, flux * 1.5);
    if (t > 0.2 && k > 0.24 && tn - AU.lastHit > 120) {
      AU.hitIn = clamp(k, 0.35, 1); AU.lastHit = tn; AU.hitN++;
      // pull the beat clock toward the playing (a third of the way each time)
      const per = 60000 / clamp((window.HRTempo && HRTempo.get()) || 120, 40, 240);
      let e = (((tn - AU.b0) / per) % 1 + 1) % 1;
      if (e > 0.5) e -= 1;
      AU.b0 += e * per * 0.33;
    }
    const L = window.ListenPane && ListenPane.fresh && ListenPane.fresh() ? ListenPane.latest() : null;
    if (L && L.chord && typeof L.chord.root === "number") AU.thue = ((L.chord.root * 7) % 12) / 12 + (L.chord.quality && /^m|dim/.test(L.chord.quality) ? 0.04 : 0);   // circle of fifths: related chords, related colours
    else if (AU.bands) AU.thue = (AU.thue * 0.97 + clamp(AU.thigh / Math.max(0.05, AU.tlow + AU.thigh), 0, 1) * 0.6 * 0.03);   // no chord data: drift with the brightness
    AU.tT = tn;
    if (reduced()) { pulseGentle(); return; }
    if (!AU.raf && t > 0.02 && pulseOn()) { AU.last = 0; AU.raf = requestAnimationFrame(pulseFrame); if (R.wrap) R.wrap.classList.add("sr-pulsing"); }
  }
  /* Reduced motion: no frames; lights follow a slow level a few times a second, nothing moves. */
  function pulseGentle() {
    const tn = now();
    if (tn - AU.redT < 240 || !pulseOn()) return;
    AU.redT = tn;
    AU.slow += (AU.tl - AU.slow) * 0.25; AU.lvl = AU.slow;
    AU.sus = AU.low = AU.mid = AU.high = AU.hit = AU.hitS = 0;
    paintPulse(tn, 0.25, true);
  }
  function pulseFrame(ts) {
    AU.raf = 0;
    if (!pulseOn() || reduced()) { pulseRest(); return; }
    const dt = AU.last ? Math.min(0.1, (ts - AU.last) / 1000) : 1 / 60;
    AU.last = ts;
    if (now() - AU.tT > 450) { AU.tl = AU.tlow = AU.tmid = AU.thigh = 0; }   // the readings stopped: let it all settle
    const env = (cur, tgt, a, r) => cur + (tgt - cur) * (1 - Math.exp(-dt / (tgt > cur ? a : r)));
    AU.lvl = env(AU.lvl, AU.tl, 0.06, 0.55);
    AU.slow = env(AU.slow, AU.tl, 0.9, 1.8);
    AU.sus = env(AU.sus, AU.tl > 0.5 ? AU.tl : 0, 2.4, 2.2);    // sustained loud playing
    AU.low = env(AU.low, AU.tlow, 0.05, 0.4);
    AU.mid = env(AU.mid, AU.tmid, 0.06, 0.45);
    AU.high = env(AU.high, AU.thigh, 0.04, 0.3);
    if (AU.hitIn) { AU.hit = Math.max(AU.hit, AU.hitIn); AU.hitIn = 0; } else AU.hit *= Math.exp(-dt / 0.3);
    AU.hitS = env(AU.hitS, AU.hit, 0.045, 0.32);                  // a hit swells in over ~50 ms: no hard flashes
    let dh = AU.thue - AU.hue; dh -= Math.round(dh);
    AU.hue = (AU.hue + dh * (1 - Math.exp(-dt / 1.4)) + 1) % 1;
    const per = 60000 / clamp((window.HRTempo && HRTempo.get()) || 120, 40, 240);
    AU.bp = (ts - AU.b0) / per;
    AU.ph = ((AU.bp % 1) + 1) % 1;
    AU.rayA = (AU.rayA + dt * (0.6 + 4 * AU.high)) % 360;
    // envelopes run every frame; the overlays are written at ~30 Hz (each write is a main-thread commit)
    AU.dtAcc = (AU.dtAcc || 0) + dt;
    if ((AU.fr = (AU.fr + 1 || 1) % 2) === 0) { paintPulse(ts, AU.dtAcc, false); AU.dtAcc = 0; }
    if (AU.tl < 0.01 && AU.lvl < 0.01 && AU.slow < 0.03 && AU.sus < 0.03 && AU.hitS < 0.01) { pulseRest(); return; }   // silence: calm idle, no frames
    AU.raf = requestAnimationFrame(pulseFrame);
  }
  /* Back to the idle look (everything at rest) and stop. */
  function pulseRest() {
    if (AU.raf) cancelAnimationFrame(AU.raf);
    AU.raf = 0; AU.last = 0;
    AU.lvl = AU.slow = AU.sus = AU.low = AU.mid = AU.high = AU.hit = AU.hitS = 0;
    paintPulse(now(), 1, true);
    if (R.wrap) R.wrap.classList.remove("sr-pulsing");
    R.lvlShown = -1;   // the 10 Hz telemetry path takes the gear lights back
  }
  /* Overlays of the scene on screen (called when a scene's layers go in). */
  function collectPulse(back, front) {
    AU.els = [...back.querySelectorAll("[data-p]"), ...front.querySelectorAll("[data-p]")].map((el) => ({ el, p: el.dataset.p, i: +el.dataset.i || 0, o: -1, t: null, v: 0 }));
    AU.motes = [...back.querySelectorAll(".sr-pmotes .sr-mote")];
    AU.cue = -1;
    AU.rate = 1;
    paintPulse(now(), 1, true);
  }
  const wob = (t, i) => 0.5 + 0.25 * Math.sin(t * 0.0131 + i * 1.7) + 0.25 * Math.sin(t * 0.0297 + i * 4.1);   // cheap flame noise, 0..1
  /* Stage colour cues: the lighting desk changes palette on chord changes (circle of fifths order). Two colours per cue. */
  const CUES = [["#ffb45e", "#ff4f9c"], ["#ffd27a", "#ff7a3a"], ["#ff5f5f", "#ffb45e"], ["#7fb2ff", "#c46bff"], ["#5ee0ff", "#7f8cff"], ["#ff6ad5", "#7fb2ff"]];
  const HEAD_BASE = [-30, -17, -6, 6, 17, 30];   // the upstage heads' resting fan (degrees, 0 = straight down)
  function paintPulse(ts, dt, still) {
    const E = AU, L = E.lvl, S = E.slow, H = E.hitS, lo = E.low, sp = R.sceneId;
    // the groove: a lift on every beat (rig BPM, pulled onto the playing), scaled by how loud it is
    const beat = still ? 0 : Math.pow(1 - E.ph, 2.2) * Math.min(1, L * 1.5);
    const alt = still ? 0.5 : 0.5 + 0.5 * Math.cos(Math.PI * (E.bp || 0));   // 1 on even beats, 0 on odd ones
    // fire: noise that the music feeds (more and faster when it's loud)
    const flick = 0.78 + 0.22 * wob(ts * (1 + S), 7) + 0.12 * H;
    // stage: how hard the show is running, and where the heads point
    const show = clamp(0.08 + 0.8 * S + 0.45 * H, 0, 1);
    const swing = still ? 0 : 16 * Math.min(1, S * 1.4), ph2 = Math.PI * (E.bp || 0) / 2;
    const headA = (i) => HEAD_BASE[i] + Math.sin(ph2 + (i < 3 ? 0 : Math.PI) + (i % 3) * 0.5) * swing + (i < 3 ? -1 : 1) * 5 * H;
    const headO = (i) => 0.05 + show * (0.5 + 0.5 * (i % 2 ? 1 - alt : alt));
    if (sp === "stage" && !still) {
      const cue = Math.round(E.hue * 6) % 6;
      if (cue !== E.cue && E.els.length) { E.cue = cue; const fx = E.els[0].el.closest(".sr-stagefx"); if (fx) { fx.style.setProperty("--ca", CUES[cue][0]); fx.style.setProperty("--cb", CUES[cue][1]); } }
    }
    const w = (k) => Math.pow(Math.max(0, Math.cos(2 * Math.PI * (E.hue - k / 3))), 1.6);
    const set = (it, o, t) => {
      if (o != null) { o = Math.round(clamp(o, 0, 1) * 64) / 64; if (o !== it.o) { it.o = o; it.el.style.opacity = o; } }
      if (t != null && t !== it.t) { it.t = t; it.el.style.transform = t === "none" ? "" : t; }
    };
    for (const it of E.els) {
      let o = null, t = null;
      switch (it.p) {
        // studio: practicals and real meters
        case "sconce": o = 0.18 + 0.7 * S + 0.12 * beat; break;
        case "strip": {   // an LED strip in music mode: lit from the middle outwards with the level, a lift on the beat
          o = (it.i ? 0.25 : 0.3) + 0.45 * S + 0.25 * beat;
          t = `scaleX(${(Math.round((still ? 0.22 : clamp(0.22 + 0.85 * (0.6 * L + 0.4 * S) + 0.15 * H, 0.22, 1)) * 60) / 60).toFixed(3)})`;
          break;
        }
        case "vu": o = 0.05 + 0.8 * L + 0.2 * H; break;
        case "needle": {
          const v = still ? 0 : clamp(L * 1.05 + 0.35 * H, 0, 1.1);
          it.v = v > it.v ? it.v + (v - it.v) * Math.min(1, dt * 18) : it.v + (v - it.v) * Math.min(1, dt * 3.2);   // VU ballistics
          t = `rotate(${(-38 + 80 * it.v).toFixed(1)}deg)`;
          break;
        }
        case "meter": {
          const b = E.bands, n = 15;
          let v = b ? (() => { let s = 0, c = 0; for (let j = Math.floor(it.i * 28 / n); j < Math.max(Math.floor(it.i * 28 / n) + 1, Math.floor((it.i + 1) * 28 / n)); j++) { s += b[j]; c++; } return s / c; })() : L * (0.6 + 0.4 * Math.sin(it.i * 0.9 + ts * 0.004));
          v = still ? 0.05 : clamp(Math.max(v * Math.min(1, L * 2.2) + 0.25 * H, 0.05), 0.05, 1);
          it.v = v > it.v ? v : Math.max(v, it.v - dt * 1.4);
          t = `scaleY(${(Math.round(it.v * 40) / 40).toFixed(3)})`; o = still ? 0 : 0.35 + 0.6 * Math.min(1, L * 2);
          break;
        }
        // living room: the fire and the lamps
        case "fire": t = still ? "none" : `scale(${(1 + 0.12 * L + 0.3 * H).toFixed(3)}, ${(1 + 0.35 * L + 0.8 * H).toFixed(3)})`; break;
        case "firewall": o = (0.22 + 0.65 * S + 0.35 * H) * (still ? 1 : flick); break;
        case "fireceil": o = (0.12 + 0.4 * S + 0.25 * H) * (still ? 1 : flick); break;
        case "hearth": o = (0.25 + 0.65 * S + 0.35 * H) * (still ? 1 : flick); break;
        case "flare": o = 0.05 + 0.6 * H + 0.3 * lo; break;
        case "candle": { const n = wob(ts, it.i); o = 0.35 + 0.65 * E.high * (0.55 + 0.45 * n); t = still ? "none" : `scaleY(${(1 + 0.45 * E.high * (n - 0.3) + 0.2 * H).toFixed(3)})`; break; }
        case "lamp": o = 0.25 + 0.6 * (0.6 * L + 0.4 * S); break;
        case "motes": o = 0.6 + 0.4 * Math.min(1, S * 1.5); break;
        // stage: the show
        case "wash": o = (0.05 + 0.4 * S + 0.2 * H + 0.12 * beat) * w(it.i); break;
        case "led": o = 0.3 + 0.7 * Math.min(1, 0.8 * L + H); break;
        case "haze": o = 0.12 + 0.45 * S + 0.2 * H + 0.25 * E.sus; break;
        case "hbeam": o = headO(it.i); t = `rotate(${headA(it.i).toFixed(2)}deg)`; break;
        case "hpool": {
          const a = headA(it.i) * Math.PI / 180, Lb = 560, x = HX(it.i) - Lb * Math.sin(a), y = 74 + Lb * Math.cos(a);
          o = headO(it.i) * 0.8; t = `translate(${x.toFixed(1)}px, ${y.toFixed(1)}px)`;
          break;
        }
        case "lens": o = 0.25 + 0.75 * headO(it.i); break;
        case "blinder": o = H * (it.i % 2 === E.hitN % 2 ? 0.7 : 0.25); break;
        case "beams": o = 0.3 + 0.7 * Math.min(1, 0.8 * L + H); break;
        case "sweep": {
          const a = still ? 0 : Math.min(1, S * 1.5);
          t = `rotate(${(Math.sin(Math.PI * (E.bp || 0) + (it.i % 2) * Math.PI) * 13 * a + (it.i < 2 ? -1 : 1) * 6 * H).toFixed(2)}deg)`;
          break;
        }
        case "jump": t = still ? "none" : `translateY(${(-(10 * H + 3.5 * beat)).toFixed(2)}px)`; break;
        case "crowd": {
          const an = it.an || (it.an = it.el.getAnimations()[0]);
          if (!an) break;
          const on = !still && S > 0.16, per = 60000 / clamp((window.HRTempo && HRTempo.get()) || 120, 40, 240);
          if (on && it.per !== per) { it.per = per; it.el.style.animationDuration = `${Math.round(per * 2)}ms`; it.sync = 0; }
          if (on !== it.on) { it.on = on; if (on) { an.play(); it.sync = 0; } else { an.pause(); an.currentTime = 0; } }
          if (on && (ts - (it.sync || 0) > 3000 || it.hn !== E.hitN && ts - it.sync > 600)) { it.sync = ts; it.hn = E.hitN; an.currentTime = ((ts - E.b0) % (per * 2) + per * 2) % (per * 2); }
          break;
        }
        case "cone": o = 0.12 + 0.6 * lo + 0.2 * H; t = still ? "none" : `scale(${(Math.round((1 + 0.1 * lo + 0.08 * H) * 160) / 160).toFixed(4)})`; break;
        case "wedge": o = 0.05 + 0.4 * lo + 0.12 * beat; break;
        // outdoors: the festoon, the sky, the sun
        case "lights": { const c = still ? 0.5 : 0.5 + 0.5 * Math.cos(2 * Math.PI * ((E.bp || 0) / 3 - it.i / 3)); o = 0.25 + S * (0.25 + 0.6 * c) + 0.2 * H; break; }
        case "festpool": o = 0.15 + 0.6 * S + 0.15 * beat; break;
        case "sky": o = 0.12 + 0.6 * S + 0.1 * beat; break;
        case "sun": o = 0.12 + 0.25 * S + 0.12 * E.high; break;
        case "rays": o = 0.08 + 0.4 * E.high * (0.7 + 0.3 * wob(ts, 3)) + 0.12 * S; t = still ? null : `rotate(${E.rayA.toFixed(2)}deg)`; break;
        case "glitter": o = 0.15 + 0.7 * E.high * wob(ts * 2, 5); break;
        case "flies": {
          o = 0.5 + 0.5 * Math.min(1, L * 1.3);
          const k = (it.i ? -1 : 1), x = k * 18 * S + Math.sin(ts * 0.0021 + it.i * 2) * 12 * S, y = -(10 * H + 9 * S * Math.abs(Math.sin(ts * 0.0033 + it.i)));
          t = still ? "none" : `translate(${x.toFixed(1)}px, ${y.toFixed(1)}px)`;
          break;
        }
        case "sway": t = still ? "none" : `rotate(${(Math.sin(ts * 0.0029 + it.i * 2) * 3.2 * Math.min(1, S * 1.2 + lo * 0.6) * (it.i ? -1 : 1)).toFixed(2)}deg)`; break;
      }
      set(it, o, t);
    }
    // the gear, lit by the scene: a rim of its key light, a shadow cast on the floor, its own spot pool and tube glow
    const avgSwing = sp === "stage" ? (headA(2) + headA(3)) / 2 : 0;
    for (const g of R.rims || []) {
      let o = 0;
      if (sp === "stage") o = (g.side ? 0.55 : 1) * (0.06 + 0.9 * show * (0.75 + 0.25 * alt));   // backlight: top and both sides
      else if (sp === "studio" && !g.side) o = 0.15 + 0.55 * L + 0.3 * H;                       // the track spot above
      else if (sp === "living" && g.side === "l") o = (0.2 + 0.55 * S + 0.3 * H) * (still ? 1 : flick);   // the fire, to the left
      else if (sp === "outdoors" && g.side === "r") o = 0.4 + 0.25 * S;                          // the low sun, to the right
      set(g, o, null);
    }
    for (const g of R.casts || []) {
      if (sp === "living") set(g, (0.45 + 0.45 * S) * (still ? 1 : flick), `skewX(${(-52 - (still ? 0 : (flick - 0.8) * 40)).toFixed(1)}deg)`);
      else if (sp === "outdoors") set(g, 0.7, null);
      else set(g, 0, null);
    }
    if (R.built && still) {
      // at rest: the gear's own lights go back to the telemetry path (≤10 Hz, gentle)
      for (const g of R.lit || []) { g.v = -1; g.o = -1; if (g.t) { g.t = null; g.el.style.transform = ""; } }
      for (const g of [...(R.pools || []), ...(R.beamsL || [])]) if (g.o !== -1) { g.o = -1; g.el.style.opacity = ""; }
      for (const c of R.cones || []) { c.v = -1; c.el.style.transform = ""; }
      for (const g of R.tubes || []) g.v = -1;
    } else if (R.built) {
      for (const g of R.lit || []) {
        const sl = g.slot != null ? getSlot(g.slot) : null, on = !sl || !sl.bypassed;
        // pools: the station's spot on the floor; glows: the stage backlight behind the backline (other scenes hide it)
        const o = !on ? (g.pool ? 0.18 : 0.06) : g.pool ? 0.3 + 0.45 * L + 0.35 * H + 0.1 * beat : 0.2 + 0.55 * show + 0.15 * beat;
        set(g, o, null);
      }
      for (const g of R.pools || []) set(g, 0.4 + 0.35 * L + 0.35 * H, null);
      for (const g of R.beamsL || []) set(g, 0.55 + 0.45 * Math.min(1, 0.8 * L + H), null);
      for (const c of R.cones || []) {
        const sl = getSlot(c.slot);
        const v = !sl || sl.bypassed ? 1 : Math.round((1 + 0.075 * lo + 0.06 * H) * 150) / 150;
        if (v !== c.v) { c.v = v; c.el.style.transform = v === 1 ? "" : `scale(${v})`; }
      }
      for (const g of R.tubes || []) {
        const sl = getSlot(g.slot);
        const v = sl && !sl.bypassed ? Math.round(clamp(0.45 + 0.35 * L + 0.55 * H, 0, 1) * 20) / 20 : 0.1;
        if (v !== g.v) { g.v = v; g.el.style.setProperty("--tube", v); }
      }
    }
    // dust drifts faster when it's loud (a few times a second at most)
    const rate = still ? 1 : Math.round((1 + 1.6 * Math.min(1, S * 1.3)) * 5) / 5;
    if (rate !== E.rate && (still || ts - E.rateT > 350)) {
      E.rate = rate; E.rateT = ts;
      E.motes.forEach((m) => m.getAnimations().forEach((a) => { a.playbackRate = rate; }));
    }
  }
  /* x of the i-th moving head on the upstage truss (art coordinates; matches stageSVG's blinders) */
  function HX(i) { const W = ART_W, lw = Math.min(900, W * 0.64), lx = W / 2 - lw / 2; return lx + (i + 0.5) * lw / 6; }
  if (window.HRActivity) HRActivity.on(() => { if (!pulseOn() && (AU.raf || AU.lvl)) pulseRest(); });

  /* Show the scene: put its layers in (once its image is decoded) and crossfade from the old ones. */
  function showScene(id, animate) {
    if (!R.built) return;
    const env = R.scene.querySelector(".sr-env"), front = R.scene.querySelector(".sr-front");
    const top = env.lastElementChild;
    if (top && top.dataset.space === id && !top.dataset.leaving) return;
    const token = (R.sceneToken = (R.sceneToken || 0) + 1);
    const go = (img) => {
      if (token !== R.sceneToken || !R.built) return;
      const back = document.createElement("div");
      back.className = "sr-layer"; back.dataset.space = id;
      back.innerHTML = `<div class="sr-envfx">${fxBackHTML(id, ART_W)}</div>`;
      back.insertBefore(img, back.firstChild);
      const fr = document.createElement("div");
      fr.className = "sr-flayer"; fr.dataset.space = id; fr.innerHTML = fxFrontHTML(id, ART_W);
      const olds = [...env.children], oldf = [...front.children];
      env.appendChild(back); front.appendChild(fr);
      R.wrap.dataset.space = id;
      collectPulse(back, fr);
      const done = () => { olds.forEach((e) => e.remove()); oldf.forEach((e) => e.remove()); };
      if (!animate || reduced() || !olds.length) { done(); return; }
      olds.forEach((e) => { e.dataset.leaving = "1"; });
      const k = { duration: 420, easing: "cubic-bezier(.3,.1,.3,1)" };
      back.animate([{ opacity: 0 }, { opacity: 1 }], k).onfinish = done;
      fr.animate([{ opacity: 0 }, { opacity: 1 }], k);
      oldf.forEach((e) => e.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 300, fill: "forwards" }));
    };
    const c = sceneArt(id);
    if (c.canvas) go(c.canvas); else setTimeout(() => rasterize(id).then(go), 0);   // let the gear paint first
  }
  function paintSpace() {
    if (!R.built) return;
    const id = R.sceneId;
    showScene(id, false);
    const sc = sceneById(id);
    const b = R.root.querySelector(".sr-spacebtn");
    if (b) { b.querySelector("b").textContent = sc.name; b.querySelector("small").textContent = sc.sub; }
    paintHUD(R.model || { board: [], stations: [], split: false });
    paintSpacesOn();
  }
  /* Crossfade to another scene; starts the next frame (the image is usually decoded already, see prewarm). */
  function sceneTransition(id) {
    R.sceneId = id;
    if (!R.visible) { if (R.built) paintSpace(); return; }
    showScene(id, true);
    const sc = sceneById(id), b = R.root.querySelector(".sr-spacebtn");
    if (b) { b.querySelector("b").textContent = sc.name; b.querySelector("small").textContent = sc.sub; }
    paintHUD(R.model || { board: [], stations: [], split: false });
    paintSpacesOn();
  }
  /* While the Room is on screen and idle, get the other scenes' images ready (one per idle slot; stops when hidden). */
  function prewarm() {
    clearTimeout(R.warmT);
    if (!R.visible || document.hidden) return;
    const next = SCENES.find((s) => !(sceneCache.get(s.id) || {}).thumb);
    if (!next) { const p = R.root.querySelector(".sr-spaces"); if (!p.dataset.built) buildSpaces(p); return; }
    R.warmT = setTimeout(() => { if (!R.visible || document.hidden) return; rasterize(next.id).then(() => prewarm()); }, 600);
  }
  /* The picker is built once; opening it only shows it. */
  function buildSpaces(p) {
    p.innerHTML = `<div class="sr-sp-h">Where are you playing?<small>Pick the scene for your rig. It only changes the picture.</small></div>
      <div class="sr-sp-list" role="menu">${SCENES.map((sc) => `<button class="sr-sp-card" data-space="${sc.id}" role="menuitemradio">
        <span class="sr-sp-art sp-${sc.id}" aria-hidden="true">${sceneArt(sc.id).thumb ? `<img src="${sceneArt(sc.id).thumb}" alt="" width="104" height="70" draggable="false">` : `<img alt="" width="104" height="70" draggable="false">`}</span>
        <span class="sr-sp-t"><b>${esc(sc.name)}</b><small>${esc(sc.blurb)}</small></span></button>`).join("")}</div>`;
    p.dataset.built = "1";
  }
  function paintSpacesOn() {
    const p = R.root && R.root.querySelector(".sr-spaces");
    if (!p || !p.dataset.built) return;
    p.querySelectorAll(".sr-sp-card").forEach((c) => { const on = c.dataset.space === R.sceneId; c.classList.toggle("on", on); c.setAttribute("aria-checked", String(on)); });
  }
  function openSpaces() {
    const p = R.root.querySelector(".sr-spaces");
    if (!p.dataset.built) buildSpaces(p);
    paintSpacesOn();
    p.hidden = false;
    SCENES.forEach((sc, i) => { if (!sceneArt(sc.id).thumb) setTimeout(() => rasterize(sc.id), 60 + i * 120); });
    if (!reduced()) p.animate([{ opacity: 0, transform: "translateY(-6px) scale(.98)" }, { opacity: 1, transform: "none" }], { duration: 180, easing: "cubic-bezier(.2,.8,.2,1)" });
    R.root.querySelector(".sr-spacebtn").setAttribute("aria-expanded", "true");
  }
  function closeSpaces() { const p = R.root && R.root.querySelector(".sr-spaces"); if (p && !p.hidden) { p.hidden = true; R.root.querySelector(".sr-spacebtn").setAttribute("aria-expanded", "false"); } }

  /* =====================================================================
     MODEL — walk the layout into a pedalboard, backline stations and cables
     ===================================================================== */
  const UK = (s) => (R.ukeys && R.ukeys.get(s.slot_idx)) || `u:${s.table_id}`;
  function buildModel() {
    const seen = {};
    R.ukeys = new Map((rackState.slots || []).map((s) => { const n = (seen[s.table_id] = (seen[s.table_id] || 0) + 1); return [s.slot_idx, `u:${s.table_id}#${n}`]; }));
    const board = [];                 // { t: "pedal" | "aby", s, d, lane, sp }
    const stations = [];              // { id, t: "stack" | "keys" | "rack", lane, ... }
    let rack = null;
    const plane = [], floor = [];
    let lastAmp = null;
    const link = (prev, to) => {
      prev.forEach((p) => {
        if (p.node === to.node && p.node !== "board") return;
        if (p.node === "board" && to.node === "board") { if (p.entry !== to.entry) plane.push([p.entry, to.entry, p.port || 0]); return; }
        floor.push({ a: p.node, b: to.node, ea: p.entry, eb: to.entry, port: p.port || 0 });
      });
    };
    const walk = (items, lane, prev) => {
      let open = null, prevForm = "";
      items.forEach((it) => {
        if (it.type === "split") {
          const e = board.length;
          board.push({ t: "aby", sp: it, lane });
          link(prev, { node: "board", entry: e });
          const ends = [];
          (it.lanes && it.lanes.length ? it.lanes : [[], []]).forEach((ln, li) => ends.push(...walk(ln || [], (lane ? lane + "." : "") + (li ? "B" : "A"), [{ node: "board", entry: e, port: li }])));
          prev = ends; open = null; prevForm = "split";
          return;
        }
        const s = getSlot(it.slot);
        if (!s) return;
        const d = layoutDesc(s);
        let to;
        if (d.form === "pedal" || d.form === "wah") {
          const e = board.length;
          board.push({ t: "pedal", s, d, lane, wah: d.form === "wah" });
          to = { node: "board", entry: e };
          open = null;
        } else if (d.form === "amp" || d.form === "nam") {
          open = { id: "st:" + UK(s), t: "stack", amp: s, cab: null, lane };
          stations.push(open); lastAmp = s;
          to = { node: open.id };
        } else if (d.form === "cab") {
          if (open && open.t === "stack" && open.amp && !open.cab && (prevForm === "amp" || prevForm === "nam")) { open.cab = s; to = { node: open.id }; }
          else { const st = { id: "st:" + UK(s), t: "stack", amp: null, cab: s, inherit: lastAmp, lane }; stations.push(st); to = { node: st.id }; }
          open = null;
        } else if (d.form === "midi") {
          if (!open || open.t !== "keys") { open = { id: "k:" + UK(s), t: "keys", mods: [], insts: [], lane }; stations.push(open); }
          (d.kind === "instrument" || d.kind === "synth" ? open.insts : open.mods).push(s);
          to = { node: open.id };
        } else {
          if (!rack) rack = { id: "rack", t: "rack", units: [] };
          rack.units.push({ s, lane });
          to = { node: "rack" };
          open = null;
        }
        prevForm = d.form;
        link(prev, to);
        prev = [to];
      });
      return prev;
    };
    walk(rackLayout(), "", [{ node: "in" }]);
    if (rack) stations.push(rack);
    const dup = new Set();
    const fl = floor.filter((c) => { const k = `${c.a}|${c.b}|${c.ea}|${c.eb}`; if (dup.has(k)) return false; dup.add(k); return true; });
    return { board, stations, plane, floor: fl, split: board.some((e) => e.t === "aby") };
  }
  /* Structure signature: what's in the room and where (bypass and knob values update in place). */
  function structKey() {
    const sl = (rackState.slots || []).map((s) => `${s.slot_idx}:${s.table_id}:${cabTxt(s)}${window.HRCabinet ? HRCabinet.sig(s) : ""}`).join(",");
    return `${sl}|${typeof layoutSig === "function" ? layoutSig() : ""}|${ui.everConnected ? 1 : 0}|${R.bagOpen ? 1 : 0}`;
  }

  /* =====================================================================
     FACES — the chain's own renderers, at their natural size (CSS zoom shrinks them)
     ===================================================================== */
  function withoutDrawer(s, fn) {
    const k = expKey(s), had = ui.expanded.has(k), prevC = RENDER.compact;
    ui.expanded.delete(k); RENDER.compact = false;
    try { return fn(); } finally { if (had) ui.expanded.add(k); RENDER.compact = prevC; }
  }
  function ampDescOf(src) {
    if (!src) return null;
    const sd = describe(src);
    return sd.form === "nam" ? { form: "amp", tolex: "#1b1c20", grille: "g-dark", pipe: "#454852", _name: src.reference_name } : { ...sd, _name: src.reference_name };
  }
  function faceHTML(s, ampSrc) {
    return withoutDrawer(s, () => {
      const d = describe(s), place = layoutDesc(s).form;
      let f;
      if (d.form === "midi") f = midiUnitHTML(s, 0);
      else if (d.form === "nam") f = namHTML(s, 0);
      else if (d.form === "pedal" || d.form === "wah") f = pedalHTML(s, d, 0);
      else if (d.form === "amp") f = ampHTML(s, { ...d, _name: s.reference_name }, 0);
      else if (d.form === "cab") f = cabHTML(s, d, 0, ampDescOf(ampSrc));
      else f = rackHTML(s, d, 0);
      if (place === "pedal" || place === "wah") return { html: f, w: 0 };
      if (place === "midi") { const w = d.kind === "detector" || d.kind === "midifx" ? 560 : 760; return { html: `<div style="width:${w}px">${f}</div>`, w }; }
      if (place === "rack") return { html: `<div class="rack" style="width:${NAT.rack}px"><div class="rack-units">${f}</div></div>`, w: NAT.rack };
      return { html: `<div class="stack" style="width:${NAT.stack}px">${f}</div>`, w: NAT.stack };
    });
  }
  const isCombo = (s) => {
    const a = window.HRUnitArt && HRUnitArt.artFor(s);
    if (a) return /combo/i.test(a.name || "");
    return !!describe(s).combo;
  };

  /* ---------- real-world sizes: faces are measured once at their natural size, then scaled ---------- */
  const natCache = new Map();          // "p:tid" (pedal face) / "r:tid" (rack face) -> { w, h } natural px as the chain draws it
  function measureFaces(slots, kind) {
    const todo = slots.filter((s) => !natCache.has(`${kind}:${s.table_id}`));
    if (!todo.length || !R.wrap) return;
    const host = document.createElement("div");
    host.className = "sr-content sr-measure";
    host.innerHTML = todo.map((s) => `<div class="sr-fit" data-tid="${s.table_id}">${faceHTML(s).html}</div>`).join("");
    R.wrap.appendChild(host);
    host.querySelectorAll(".sr-fit").forEach((f) => {
      const u = kind === "r" ? f.querySelector(".rack-units > *") : f.firstElementChild;
      const w = u ? u.offsetWidth : 0, h = u ? u.offsetHeight : 0;
      if (w > 10 && h > 10) natCache.set(`${kind}:${f.dataset.tid}`, { w, h });
    });
    host.remove();
  }
  const measurePedals = (slots) => measureFaces(slots, "p");
  /* A stompbox's footprint in centimetres, from its drawing's aspect ratio. */
  function pedalCm(r, wah) {
    if (wah) { const h = 25; return { w: clamp(h * r, 9, 13), h }; }
    if (r < 0.85) { const h = 12; return { w: clamp(h * r, 5.5, 10.5), h }; }           // portrait stompbox (MXR, Boss, Ibanez…)
    if (r <= 1.3) { const m = 14.5; return r >= 1 ? { w: m, h: m / r } : { w: m * r, h: m }; }   // round / squarish (Fuzz Face, Big Muff)
    const w = clamp(10.5 * r, 14, 24); return { w, h: w / r };                         // wide multi-switch boxes
  }
  function pedalBox(s, wah) {
    const n = natCache.get(`p:${s.table_id}`) || { w: 200, h: 320 };
    const cm = pedalCm(n.w / n.h, wah);
    const w = cm.w * BPX;
    return { w, h: w * n.h / n.w, zoom: w / n.w, cm };
  }

  /* ---------- cabinet drawing ---------- */
  function cabTxt(s) {
    if (!s) return "";
    const d = describe(s);
    if (d.form !== "cab") return "";
    let t = (s.cabinet && (s.cabinet.speakers || s.cabinet.label || s.cabinet.name)) || "";
    (s.params || []).forEach((p) => { if (hasLabels(p) && p.labels.some((l) => /\dx1[025]\b/i.test(l))) t += " " + stepLabel(p, stepIndex(p)); });
    return t.trim();
  }
  function cabSpec(cab, amp) {
    const txt = cabTxt(cab) || "";
    const m = txt.match(/\b([1248])\s*x\s*(10|12|15)\b/i);
    const bass = amp && /bass|svt|heritage/i.test(`${amp.reference_name} ${amp.display_name}`);
    const n = m ? +m[1] : 4, sz = m ? +m[2] : bass ? 10 : 12;
    const cols = n === 1 ? 1 : n === 8 ? 2 : 2, rows = Math.max(1, Math.round(n / cols));
    const w = n === 1 ? 236 : n === 2 ? 290 : AMP_W;
    let sd = sz === 10 ? 104 : sz === 15 ? 146 : 120;
    if (rows >= 4) sd = 70;
    if (n === 1) sd = 160;
    const h = Math.round(rows * (sd + 14) + 40);
    return { n, sz, cols, rows, w, h, sd, txt };
  }
  function cabArt(spec, ad, label) {
    const tolex = (ad && ad.tolex) || "#18181a";
    const grille = (ad && (ad.cabGrille || ad.grille)) || "g-dark";
    const pipe = (ad && ad.pipe) || "rgba(255,255,255,0.18)";
    let spk = "";
    for (let k = 0; k < spec.cols * spec.rows; k++) spk += `<span class="spk sr-spk"><i class="cone"></i><i class="cap"></i></span>`;
    return `<div class="sr-cab" style="width:${spec.w}px;height:${spec.h}px;--tolex:${tolex};--pipe:${pipe}">
      <div class="sr-cab-top"></div>
      <div class="sr-cab-in"><div class="sr-spks" style="--cols:${spec.cols};--sd:${spec.sd}px">${spk}</div><div class="cloth ${grille}"></div><div class="sr-weave"></div><div class="sr-cab-vign"></div>
      <span class="sr-cab-badge">${esc(spec.n)}×${esc(spec.sz)}</span></div>
      <i class="sr-cnr l"></i><i class="sr-cnr r"></i><i class="sr-cnr bl"></i><i class="sr-cnr br"></i><i class="sr-cab-jack"></i>
      <span class="sr-sr">${esc(label)}</span>
    </div>`;
  }

  /* ---------- microphones ---------- */
  function micKind(m) {
    const t = `${(m && m.id) || ""} ${(m && m.label) || ""}`.toLowerCase(), fam = ((m && m.family) || "").toLowerCase();
    if (/57/.test(t)) return "sm57";
    if (/421/.test(t)) return "md421";
    if (/re ?20/.test(t)) return "re20";
    if (/d6|d112|kick/.test(t)) return "kick";
    if (/121/.test(t)) return "r121";
    if (/160/.test(t)) return "m160";
    if (/qtc|pencil/.test(t)) return "pencil";
    if (/47|67|87|149|condenser/.test(t) || fam === "condenser") return "ldc";
    if (fam === "ribbon") return "r121";
    return "sm57";
  }
  /* Side-on silhouettes (pointing right), 60 × 22 box. */
  const MIC_ART = {
    sm57: `<path d="M2 8.5h30l6-1.2h9v7.4h-9l-6-1.2H2z" fill="#2b2d31"/><rect x="38" y="6.6" width="15" height="8.8" rx="4.2" fill="#9ea2a8"/><path d="M41 7v8M44 6.8v8.4M47 6.8v8.4M50 7.2v7.6" stroke="#5d6066" stroke-width=".7"/><rect x="2" y="9" width="30" height="1.4" fill="rgba(255,255,255,.18)"/>`,
    md421: `<path d="M2 7h24l4-2.5h22a3 3 0 0 1 3 3v7a3 3 0 0 1-3 3H30L26 15H2z" fill="#1f2023"/><path d="M33 6v10M36 6v10M39 6v10M42 6v10M45 6v10M48 6v10" stroke="#55585e" stroke-width="1.1"/><rect x="2" y="7.6" width="24" height="1.2" fill="rgba(255,255,255,.16)"/>`,
    re20: `<rect x="2" y="5.5" width="52" height="11" rx="3" fill="#a3a59f"/><path d="M8 5.5v11M12 5.5v11M16 5.5v11M20 5.5v11" stroke="#7d7f79" stroke-width="1"/><rect x="34" y="5" width="20" height="12" rx="4" fill="#8d908a"/><path d="M37 6v10M40 6v10M43 6v10M46 6v10M49 6v10" stroke="#5f625c" stroke-width=".8"/>`,
    kick: `<path d="M2 9h22l3-3h12c8 0 14 2.4 14 5s-6 5-14 5H27l-3-3H2z" fill="#26282c"/><ellipse cx="44" cy="11" rx="8.5" ry="4.6" fill="#3c3f45"/><path d="M38 8.2v5.6M41 7.4v7.2M44 7.2v7.6M47 7.4v7.2M50 8.4v5.2" stroke="#6b6e75" stroke-width=".7"/>`,
    r121: `<rect x="2" y="6" width="34" height="10" rx="2" fill="#babdc2"/><rect x="30" y="3" width="24" height="16" rx="3" fill="#c9ccd1"/><rect x="33" y="5.5" width="18" height="11" rx="1.5" fill="#55585e"/><path d="M35 6v10M37.5 6v10M40 6v10M42.5 6v10M45 6v10M47.5 6v10" stroke="#9ea1a6" stroke-width=".7"/>`,
    m160: `<rect x="2" y="7.5" width="36" height="7" rx="3" fill="#2a2b2e"/><rect x="36" y="6" width="17" height="10" rx="2.5" fill="#717479"/><path d="M39 6.5v9M42 6.5v9M45 6.5v9M48 6.5v9M51 7v8" stroke="#3a3c40" stroke-width=".8"/>`,
    pencil: `<rect x="2" y="8.2" width="44" height="5.6" rx="2.6" fill="#b9bcc1"/><rect x="44" y="7.6" width="10" height="6.8" rx="3" fill="#8e9196"/><path d="M46 8v6M48.5 8v6M51 8v6" stroke="#5c5f64" stroke-width=".6"/>`,
    ldc: `<rect x="2" y="7" width="30" height="8" rx="3" fill="#c3c0b4"/><rect x="28" y="2" width="26" height="18" rx="9" fill="#d3d0c4"/><path d="M33 3.5v15M36 2.6v16.8M39 2.2v17.6M42 2.2v17.6M45 2.6v16.8M48 3.5v15" stroke="#8f8c80" stroke-width=".7"/><circle cx="20" cy="11" r="1.6" fill="#7a7a72"/>`,
  };
  const micSVG = (k, cls = "") => `<svg class="sr-micsil ${cls}" viewBox="0 0 56 22" aria-hidden="true">${MIC_ART[k] || MIC_ART.sm57}</svg>`;
  function micStandSVG(kind) {
    const head = MIC_ART[kind] || MIC_ART.sm57;
    return `<svg class="sr-mic" viewBox="0 0 90 160" aria-hidden="true">
    <ellipse cx="52" cy="155" rx="32" ry="4.5" fill="rgba(0,0,0,.38)"/><ellipse cx="52" cy="155.5" rx="14" ry="2.2" fill="rgba(0,0,0,.4)"/>
    <path d="M52 140 L26 156 M52 140 L78 156 M52 140 L52 157" stroke="#26272b" stroke-width="3.4" stroke-linecap="round"/>
    <path d="M52 140 L26 156 M52 140 L78 156" stroke="rgba(255,255,255,.22)" stroke-width="1"/>
    <path d="M52 142 V64" stroke="#2c2d31" stroke-width="3.6" stroke-linecap="round"/><path d="M51 142 V64" stroke="rgba(255,255,255,.34)" stroke-width="1.1"/>
    <rect x="48.5" y="96" width="7" height="9" rx="2" fill="#4a4c51"/>
    <path d="M52 66 L20 40" stroke="#2c2d31" stroke-width="3" stroke-linecap="round"/><path d="M52 65 L20 39" stroke="rgba(255,255,255,.3)" stroke-width=".9"/>
    <circle cx="52" cy="65" r="4.2" fill="#55575c"/>
    <g transform="translate(24 41) rotate(-140) translate(-4 -11) scale(.72)">${head}</g>
    <path d="M24 44 C 30 70, 46 90, 50 104" stroke="#111214" stroke-width="1.6" fill="none"/>
  </svg>`;
  }
  const COMBO_STAND = `<svg class="sr-tilt" viewBox="0 0 300 70" preserveAspectRatio="none" aria-hidden="true">
    <path d="M40 4 L18 68 M260 4 L282 68 M40 4 L70 68 M260 4 L230 68" stroke="#26272b" stroke-width="5" stroke-linecap="round"/>
    <path d="M40 4 L18 68 M260 4 L282 68" stroke="rgba(255,255,255,.12)" stroke-width="1.4"/>
    <path d="M28 36 H272" stroke="#1f2023" stroke-width="4"/></svg>`;
  const X_STAND = `<svg class="sr-xstand" viewBox="0 0 300 190" preserveAspectRatio="none" aria-hidden="true">
    <path d="M58 6 L232 186 M242 6 L68 186" stroke="#1f2023" stroke-width="8" stroke-linecap="round"/>
    <path d="M58 6 L232 186 M242 6 L68 186" stroke="rgba(255,255,255,.10)" stroke-width="2"/>
    <path d="M40 6 H82 M218 6 H260" stroke="#2b2c30" stroke-width="7" stroke-linecap="round"/>
    <circle cx="150" cy="96" r="7" fill="#2b2c30"/><circle cx="150" cy="96" r="2.6" fill="#55575c"/>
    <path d="M50 186 H90 M210 186 H250" stroke="#141416" stroke-width="7" stroke-linecap="round"/></svg>`;
  const ROAD_CASE = `<div class="sr-case"><i class="c tl"></i><i class="c tr"></i><i class="c bl"></i><i class="c br"></i><span class="latch"></span></div>`;
  /* The head's top, seen from just above: tolex, a vent, and the power tubes glowing through it. */
  const ampTop = (n) => `<div class="sr-amptop" aria-hidden="true"><i class="vent">${Array.from({ length: n }, () => "<b></b>").join("")}</i></div>`;

  /* ---------- keyboards (instrument / poly synth) and MIDI modules ---------- */
  const KB_FACE = { lacquer: ["#141416", "#2a2a2e"], walnut: ["#3b2416", "#5a3a24"], organ: ["#5b1d17", "#7a2c22"], spruce: ["#b99b6d", "#d8c095"],
    graphite: ["#26282c", "#3a3d43"], brass: ["#5d4720", "#8a6d32"], pearl: ["#d9d6cf", "#efece6"], synth: ["#1b1830", "#2e2850"] };
  function kbInfo(s) {
    const k = midiKind(s);
    if (k === "synth") return { name: (typeof presetLabel === "function" && (s.factory_presets || []).length ? presetLabel(s).name : "Poly Synth"), fam: "Poly synth", face: "synth", synth: true };
    const snd = P(s, "sound") || (s.params || []).find((p) => hasLabels(p) && p.labels.length > 20);
    if (!snd) return { name: unitTitle(s), fam: "Instrument", face: "lacquer" };
    const i = soundInfo(snd, stepIndex(snd));
    return { name: i.name, fam: i.theme.t, face: i.theme.face };
  }
  function keyboardHTML(s, small) {
    const inf = kbInfo(s), c = KB_FACE[inf.face] || KB_FACE.lacquer;
    const light = inf.face === "spruce" || inf.face === "pearl";
    return `<div class="sr-hit sr-kb${inf.synth ? " synth" : ""}${small ? " small" : ""}${s.bypassed ? " sr-off" : ""}" data-sr-slot="${s.slot_idx}" data-sr-key="${UK(s)}" style="--kb1:${c[0]};--kb2:${c[1]};--kbink:${light ? "#2a2218" : "#e9e6f2"}">
      <div class="sr-kb-panel"><span class="sr-kb-led"></span><span class="sr-kb-scr"><b data-kb-name>${esc(inf.name)}</b><small data-kb-fam>${esc(inf.fam)}</small></span>
        <span class="sr-kb-knobs"><i></i><i></i><i></i>${inf.synth ? "<i></i><i></i>" : ""}</span></div>
      <div class="sr-kb-bed"><div class="piano sr-piano" data-range="" data-feed="${s.slot_idx}">${pianoHTML()}</div></div>
      <i class="sr-kb-cheek l"></i><i class="sr-kb-cheek r"></i>
    </div>`;
  }
  function moduleHTML(s) {
    const k = midiKind(s);
    const t = k === "detector" ? "Pitch → MIDI" : k === "midifx" ? "MIDI FX" : unitTitle(s);
    return `<div class="sr-hit sr-mod${s.bypassed ? " sr-off" : ""}" data-sr-slot="${s.slot_idx}" data-sr-key="${UK(s)}" data-sr-mod="${k}">
      <span class="sr-mod-led"></span><span class="sr-mod-scr" data-mod-note>${k === "detector" || k === "bloom" ? "—" : "ARP"}</span><span class="sr-mod-t">${esc(t)}</span></div>`;
  }

  /* =====================================================================
     RENDER
     ===================================================================== */
  const laneTag = (lane) => (lane ? `<span class="sr-lane" title="Lane ${esc(lane)} of the split">${esc(lane.split(".").pop())}</span>` : "");
  function fitHTML(s, zoom, ampSrc, cls = "") {
    const f = faceHTML(s, ampSrc);
    return `<div class="sr-fit ${cls}" data-sr-face="${s.slot_idx}" style="zoom:${zoom.toFixed(4)}">${f.html}</div>`;
  }
  function stationHTML(st) {
    if (st.t === "stack") return stackHTML(st);
    if (st.t === "keys") return keysHTML(st);
    return rackCaseHTML(st);
  }
  /* Where the mic stand stands: in front of the first speaker, nearer its centre for a cap position. */
  function micOffset(cab) {
    const m = cab && cab.cabinet && cab.cabinet.mic_a;
    const pos = m ? m.position : "capedge";
    return pos === "cap" ? 0 : pos === "cone" ? 1 : pos === "room" ? 1.6 : 0.5;
  }
  function stackHTML(st) {
    const amp = st.amp, cab = st.cab, src = amp || st.inherit;
    const ad = ampDescOf(src);
    const combo = amp && isCombo(amp);
    const z = AMP_W / NAT.stack;
    const tubes = amp && describe(amp).form !== "nam" ? (/5150|rectifier|svt|heritage|100/i.test(amp.reference_name) ? 4 : 2) : 0;
    let h = `<div class="sr-glow" aria-hidden="true"><i></i></div>`;
    let w = AMP_W, extra = "";
    const micKindA = cab && cab.cabinet && cab.cabinet.mic_a ? micKind(cab.cabinet.mic_a) : "sm57";
    const micTitle = cab && window.HRCabinet && HRCabinet.isCabinet(cab) ? "MIC" : "CAB";
    if (amp && combo) {
      h += `<div class="sr-hit sr-amp combo${amp.bypassed ? " sr-off" : ""}" data-sr-slot="${amp.slot_idx}" data-sr-key="${UK(amp)}">${tubes ? ampTop(tubes) : ""}${fitHTML(amp, z)}</div>${COMBO_STAND}`;
      if (cab) extra = `<div class="sr-hit sr-micd${cab.bypassed ? " sr-off" : ""}" data-sr-slot="${cab.slot_idx}" data-sr-key="${UK(cab)}" data-sr-mic="1">${micStandSVG(micKindA)}<span class="sr-mic-tag">${micTitle}</span></div>`;
    } else {
      if (amp) h += `<div class="sr-hit sr-amp${amp.bypassed ? " sr-off" : ""}" data-sr-slot="${amp.slot_idx}" data-sr-key="${UK(amp)}">${tubes ? ampTop(tubes) : ""}${fitHTML(amp, z)}</div>`;
      if (cab) {
        const spec = cabSpec(cab, src);
        w = Math.max(w, spec.w);
        h += `<div class="sr-hit sr-cabx${cab.bypassed ? " sr-off" : ""}" data-sr-slot="${cab.slot_idx}" data-sr-key="${UK(cab)}">${cabArt(spec, ad, cab.reference_name)}</div><div class="sr-casters"><i></i><i></i></div>`;
        const off = micOffset(cab);
        extra = `<div class="sr-hit sr-micdeco" data-sr-slot="${cab.slot_idx}" data-sr-mic="1" style="--mo:${off}" title="Cab &amp; mics">${micStandSVG(micKindA)}<span class="sr-mic-tag">${micTitle}</span></div>`;
      } else if (amp) h += ROAD_CASE;
    }
    return { cls: `sr-stack${combo ? " is-combo" : ""}${!cab && amp && !combo ? " on-case" : ""}${combo && cab ? " has-mic" : ""}`, inner: `${laneTag(st.lane)}<div class="sr-st-col">${h}</div>${extra}`, w: w + (combo && cab ? 64 : 0) };
  }
  function keysHTML(st) {
    const insts = st.insts.slice(0, 3), mods = st.mods;
    let h = "";
    if (mods.length) h += `<div class="sr-mods">${mods.map(moduleHTML).join("")}</div>`;
    if (insts.length > 1) h += `<div class="sr-tier">${insts.slice(1).reverse().map((s) => keyboardHTML(s, true)).join("")}</div>`;
    if (insts.length) h += keyboardHTML(insts[0], false);
    else h += `<div class="sr-shelf"></div>`;
    return { cls: "sr-keys", inner: `${laneTag(st.lane)}<div class="sr-st-col">${h}${X_STAND}</div>`, w: KEYS_W };
  }
  /* Rack gear: every unit is full 19" width (its face scaled to the bay) and a whole number of rack units tall
     (1U = 44.45 mm on a 482.6 mm panel); any slack under a face is a vent blank, like a real rack. A power
     conditioner with a level meter sits on top; the gear splits over as many cases as it takes. */
  const RACK_BAY = RACK_W - 26;                       // inner width: the 19" panels
  const U_PX = RACK_BAY * 44.45 / 482.6;              // one rack unit
  function rackCaseHTML(st) {
    measureFaces(st.units.map((u) => u.s), "r");
    const items = st.units.map((u) => {
      const n = natCache.get(`r:${u.s.table_id}`) || { w: NAT.rack, h: NAT.rack * 0.16 };
      const z = RACK_BAY / n.w, h = n.h * z;
      const U = Math.max(1, Math.ceil(h / U_PX - 0.08));
      return { u, n, z, h, U };
    });
    const cols = [];
    let col = [], us = 1;
    items.forEach((it) => {
      if (col.length && us + it.U > 16) { cols.push(col); col = []; us = 1; }
      col.push(it); us += it.U;
    });
    if (col.length) cols.push(col);
    const unitHTML = (it) => {
      const s = it.u.s, f = faceHTML(s).html.replace(/^<div class="rack" style="width:\d+px">/, `<div class="rack" style="width:${it.n.w}px">`);
      const slack = it.U * U_PX - it.h;
      return `<div class="sr-hit sr-ru${s.bypassed ? " sr-off" : ""}" data-sr-slot="${s.slot_idx}" data-sr-key="${UK(s)}" data-u="${it.U}" style="height:${(it.U * U_PX).toFixed(2)}px">${laneTag(it.u.lane)}
        <div class="sr-fit" data-sr-face="${s.slot_idx}" style="zoom:${it.z.toFixed(4)}">${f}</div>${slack > U_PX * 0.3 ? `<i class="sr-blank" style="height:${slack.toFixed(1)}px"></i>` : ""}</div>`;
    };
    const html = cols.map((c, ci) => `<div class="sr-rackcase" data-sr-key="rack:${ci}">
        <div class="sr-rack-lid"></div>
        <div class="sr-rack-bay"><div class="sr-pwrcond" aria-hidden="true" style="height:${U_PX.toFixed(2)}px"><i class="ear"></i><b class="volt">120</b><span class="sr-rmeter">${"<i></i>".repeat(10)}</span><i class="sw"></i><i class="ear"></i></div>
          ${c.map(unitHTML).join("")}</div>
        <div class="sr-rack-foot"><i></i><i></i></div></div>`).join("");
    const w = cols.length * RACK_W + (cols.length - 1) * 14;
    return { cls: "sr-rack", inner: `<div class="sr-rack-row">${html}</div>`, w };
  }

  /* ---------- the pedalboard ---------- */
  function rowsOf(items) {          // items: [{w}] → [[i…], [i…]] (at most two rows, back row first)
    const total = items.reduce((a, it) => a + it.w, 0) + Math.max(0, items.length - 1) * PGAP;
    if (total <= ROW_MAX || items.length < 2) return { rows: [items.map((_, i) => i)], k: Math.min(1, ROW_MAX / Math.max(1, total)) };
    let acc = 0, cut = 1;
    for (let i = 0; i < items.length; i++) { acc += items[i].w + PGAP; if (acc >= total / 2) { cut = Math.max(1, Math.min(items.length - 1, i + (acc - total / 2 > items[i].w / 2 ? 0 : 1))); break; } }
    const r = [items.slice(0, cut).map((_, i) => i), items.slice(cut).map((_, i) => i + cut)];
    const rw = r.map((ix) => ix.reduce((a, i) => a + items[i].w, 0) + (ix.length - 1) * PGAP);
    const depth = r.reduce((a, ix) => a + Math.max(...ix.map((i) => items[i].h)), 0) + 18;   // two rows of big boxes: keep the board ≤ ~36 cm deep
    return { rows: r, k: Math.min(1, ROW_MAX / Math.max(...rw), DEPTH_MAX / depth) };
  }
  function boardHTML(M) {
    // no pedals: no board, except an empty one to drop onto while the gear bag is open
    if (!M.board.length && !R.bagOpen) return "";
    measurePedals(M.board.filter((e) => e.t === "pedal").map((e) => e.s));
    const ABY = { w: 6 * BPX, h: 11 * BPX };
    const box = M.board.map((e) => (e.t === "aby" ? { w: ABY.w, h: ABY.h, zoom: 1 } : pedalBox(e.s, e.wah)));
    const ent = (e, i, k) => {
      const b = box[i];
      if (e.t === "aby") {
        const m = splitModel(e.sp.id);
        return `<div class="sr-pd sr-aby sr-hit" data-sr-split="${e.sp.id}" data-sr-key="split:${e.sp.id}" data-e="${i}" style="width:${(b.w * k).toFixed(1)}px;--k:${k.toFixed(3)}">
          <div class="sr-aby-body"><span class="sr-aby-t">A/B·Y</span><span class="sr-aby-leds"><i class="a"></i><i class="b"></i></span>
          <span class="sr-aby-bal"><i style="--v:${m ? m.norm.toFixed(3) : 0.5}"></i></span><span class="sr-aby-s">${esc((SPLIT_KIND[e.sp.kind] || "Split").replace(/ split$/i, ""))}</span><span class="sr-aby-fs"></span></div></div>`;
      }
      const s = e.s;
      return `<div class="sr-pd sr-hit${e.wah ? " sr-wah" : ""}${s.bypassed ? " sr-off" : ""}" data-sr-slot="${s.slot_idx}" data-sr-key="${UK(s)}" data-e="${i}" style="width:${(b.w * k).toFixed(1)}px">${laneTag(e.lane)}${fitHTML(s, b.zoom * k)}<i class="sr-pd-shadow"></i></div>`;
    };
    const wahI = [], restI = [];
    M.board.forEach((e, i) => (e.wah ? wahI : restI).push(i));
    const lay = rowsOf(restI.map((i) => box[i]));
    const k = lay.k;
    // a real board reads right to left: the guitar plugs in on the right, the last pedal on the left feeds the amp
    const rows = lay.rows.map((r) => `<div class="sr-prow">${r.slice().reverse().map((j) => ent(M.board[restI[j]], restI[j], k)).join("")}</div>`).join("");
    return `<div class="sr-boardwrap" data-sr-key="board"><div class="sr-bcol" style="perspective:${PERSP}px;perspective-origin:50% ${ORY}px">
      <div class="sr-board" style="transform:rotateX(${TILT}deg)">
        <svg class="sr-under" aria-hidden="true"></svg><svg class="sr-patch" aria-hidden="true"></svg>
        ${restI.length ? `<div class="sr-pedals">${rows}</div>` : M.board.length ? "" : `<div class="sr-pedals sr-bempty"><span>Drop pedals here</span></div>`}
        ${wahI.length ? `<div class="sr-wahs">${wahI.slice().reverse().map((i) => ent(M.board[i], i, Math.min(1, k * 1.1))).join("")}</div>` : ""}
        <i class="sr-caret" aria-hidden="true"></i>
      </div>
      <div class="sr-board-lip"><i class="sr-psu"></i></div><div class="sr-board-shadow"></div>
    </div></div>`;
  }

  /* Build the whole scene content (everything above the wall and floor). */
  function contentHTML(M) {
    const parts = M.stations.map((st) => ({ st, ...stationHTML(st) }));
    const total = parts.reduce((a, p) => a + p.w, 0) + Math.max(0, parts.length - 1) * GAP;
    const sw = Math.max(1080, total + 210);
    let x = (sw - total) / 2;
    const place = parts.map((p) => { const cx = x + p.w / 2; x += p.w + GAP; p.st.cx = cx; p.st.w = p.w; return p; });
    // each station: a thin rim of the scene's key light along the gear's edge and a cast shadow on the floor (both follow the music)
    const RIMS = `<i class="sr-rim t" aria-hidden="true"></i><i class="sr-rim s l" aria-hidden="true"></i><i class="sr-rim s r" aria-hidden="true"></i>`;
    const rimmed = (h) => h.replace(/class="sr-(st-col|rack-row)">/, (m) => m + RIMS);
    const back = place.map((p) => `<div class="sr-st ${p.cls}" data-sr-key="${p.st.id}" style="left:${(p.st.cx - p.w / 2).toFixed(1)}px;width:${p.w}px">${rimmed(p.inner)}<div class="sr-floorshadow"></div><i class="sr-cast"></i></div>`).join("");
    // a spot over every station, hung where its beam starts (a track can in the studio, a fixture on the truss on stage)
    const beams = place.map((p) => `<i class="sr-beam" data-sr-beam="${p.st.id}" style="left:${p.st.cx.toFixed(0)}px;--bw:${Math.max(260, p.w + 120)}px"></i>`).join("");
    const fixes = place.map((p) => `<i class="sr-fix" style="left:${p.st.cx.toFixed(0)}px"><b></b></i>`).join("");
    const pools = place.map((p) => `<i class="sr-pool${p.st.t === "stack" ? " warm" : ""}" data-sr-pool="${p.st.id}" style="left:${p.st.cx.toFixed(0)}px;--pw:${p.w + 160}px"></i>`).join("");
    const empty = !(rackState.slots || []).length && !M.split;
    const specks = place.map((p, i) => motes(2, p.st.cx - 50, 120, 100, 330, 11 + i)).join("");   // dust in each spot's light
    return {
      sw,
      html: `<div class="sr-lights" aria-hidden="true">${beams}${pools}${specks}</div>
        <svg class="sr-cables" aria-hidden="true" width="${sw}" height="${SH}" viewBox="0 0 ${sw} ${SH}"></svg>
        <div class="sr-backline">${back}</div>
        <div class="sr-fixes" aria-hidden="true">${fixes}</div>
        <svg class="sr-hang" aria-hidden="true" width="${sw}" height="${SH}" viewBox="0 0 ${sw} ${SH}"></svg>
        ${boardHTML(M)}
        ${empty ? `<div class="sr-empty">${ui.everConnected ? `<b>The room is empty</b>This rig has nothing in it yet. Open the gear bag and drag a pedal onto the board, or start from a template.
          <span class="sr-empty-btns"><button class="btn" data-sr-new="amp">Amp + cabinet</button><button class="btn" data-sr-new="pedalboard">Full pedalboard</button><button class="btn" data-sr-new="guitar_synth">Guitar + instrument</button></span>`
          : `<b>Waiting for the audio engine</b>Your gear appears here as soon as it connects.`}</div>` : ""}`,
    };
  }

  /* ---------- the room shell (built once) ---------- */
  const ICON = {
    space: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"><path d="M2 13.5V6l6-3.5L14 6v7.5z"/><path d="M6 13.5V9.5h4v4"/></svg>`,
    bag: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"><rect x="2" y="5" width="12" height="9" rx="1.6"/><path d="M5.5 5V3.6A1.1 1.1 0 0 1 6.6 2.5h2.8a1.1 1.1 0 0 1 1.1 1.1V5M2 9h12"/></svg>`,
    chev: `<svg viewBox="0 0 10 10" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M2 3.5 5 6.5 8 3.5"/></svg>`,
    close: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M4 4l8 8M12 4l-8 8"/></svg>`,
  };
  function build() {
    const root = $("view-room");
    if (!root) return false;
    R.root = root;
    root.innerHTML = `<div class="sr-wrap" id="sr-wrap" data-space="${R.sceneId}">
      <div class="sr-cam" id="sr-cam"><div class="sr-scene" id="sr-scene"><div class="sr-env"></div><div class="sr-content" id="sr-content"></div><div class="sr-front" aria-hidden="true"></div></div></div>
      <div class="sr-vignette" aria-hidden="true"></div>
      <div class="sr-hud" id="sr-hud"></div>
      <div class="sr-tools">
        <button class="sr-spacebtn" data-sr-act="spaces" aria-haspopup="menu" aria-expanded="false" title="Where are you playing? (changes what you hear)">${ICON.space}<span><b>Studio</b><small>Close-miked</small></span>${ICON.chev}</button>
        <div class="sr-spaces" hidden></div>
      </div>
      <button class="sr-bagbtn" data-sr-act="bag" aria-expanded="false" title="Gear bag: drag pedals onto the board">${ICON.bag}<span>Gear bag</span></button>
      <div class="sr-bag" hidden></div>
      <div class="sr-foot" aria-hidden="true"><i></i><span>Stomp</span></div>
      <div class="sr-tag" id="sr-tag" aria-hidden="true"></div>
      <div class="sr-ghost" aria-hidden="true" hidden></div>
      <div class="sr-focus" id="sr-focus" hidden><div class="sr-dim"></div></div>
    </div>`;
    R.wrap = $("sr-wrap"); R.cam = $("sr-cam"); R.scene = $("sr-scene"); R.content = $("sr-content"); R.hud = $("sr-hud"); R.tag = $("sr-tag"); R.focusEl = $("sr-focus");
    wire();
    R.ro = new ResizeObserver(() => fit());
    R.ro.observe(root);
    R.built = true;
    return true;
  }

  /* Fit the scene to the pane (a transform: no layout). */
  function fit() {
    if (!R.root || !R.visible) return;
    const W = R.root.clientWidth, H = R.root.clientHeight;
    if (!W || !H) return;
    const s = Math.min(W / R.sw, (H - 8) / SH, 1.35);
    R.scale = s;
    R.ox = (W - R.sw * s) / 2;
    R.oy = Math.max(0, (H - SH * s) * 0.62);
    R.scene.style.width = R.sw + "px";
    R.scene.style.setProperty("--sw", R.sw + "px");
    R.scene.style.setProperty("--sx", (R.sw / 2 - ART_W / 2) + "px");   // where the scene art's left edge sits
    R.scene.style.transform = `translate(${R.ox.toFixed(1)}px, ${R.oy.toFixed(1)}px) scale(${s.toFixed(4)})`;
  }

  /* Rebuild the room's contents. Items that stay glide to their new places (FLIP); new gear is set down;
     removed gear fades away; nothing flashes. */
  function render(mode) {
    AU.slot = outSlot();
    if (!R.built && !build()) return;
    const M = buildModel();
    R.model = M;
    const old = R.content;
    const oldPos = new Map();
    // A new rig crossfades in as one layer; per-item FLIP (glide / set down / fade away) is only for edits to the same rig.
    // (FLIP on a whole new rig clones and animates dozens of illustrated faces: seconds of raster work in WebKit.)
    const newRig = R.psig != null && `${rackState.preset_idx}|${rackState.preset_name}` !== R.psig;
    const anim = mode !== "instant" && !newRig && !reduced() && R.visible && old && old.childElementCount;
    const fadeIn = newRig && mode !== "instant" && !reduced() && R.visible;
    if (anim) old.querySelectorAll("[data-sr-key]").forEach((el) => { const r = el.getBoundingClientRect(); if (r.width) oldPos.set(el.dataset.srKey, r); });
    const C = contentHTML(M);
    hoverEl = null; hideTag(); hideFoot();
    const fresh = document.createElement("div");
    fresh.className = "sr-content";
    fresh.innerHTML = C.html;
    fresh.querySelectorAll("[tabindex]").forEach((n) => { n.tabIndex = -1; });
    fresh.querySelectorAll(".sr-fit").forEach((n) => n.setAttribute("aria-hidden", "true"));
    fresh.querySelectorAll(".sr-hit").forEach((n) => { n.tabIndex = 0; n.setAttribute("role", "button"); const inf = hitInfo(n); if (inf) n.setAttribute("aria-label", `${inf.name} — pick it up`); });
    R.sw = C.sw;
    if (old) old.replaceWith(fresh); else R.scene.appendChild(fresh);
    R.content = fresh;
    fit();
    paintSpace();
    layoutCables(M);
    collectLive();
    indexControls();
    if (typeof indexPianos === "function") indexPianos();
    paintHUD(M);
    paintState();
    R.key = structKey();
    R.stale = false;
    // the lifted unit keeps its card (following the unit, whose slot index may have moved); a new preset puts it away
    const psig = `${rackState.preset_idx}|${rackState.preset_name}`;
    if (R.focus && R.focus.room) {
      const f = R.focus;
      if (R.psig && psig !== R.psig) closeFocus(true);
      else {
        if (f.slot != null) {
          let hit = [...R.ukeys].find(([, k]) => k === f.ukey);
          if (!hit && adoptSwapped(f)) { f.ukey = R.ukeys.get(f.slot); hit = [f.slot, f.ukey]; fillCard(f); indexControls(); }   // a classic cab swapped for the Cabinet unit
          if (!hit) f.slot = -1;
          else if (hit[0] !== f.slot || f.mode === "mic") { f.slot = hit[0]; f.station = stationOf(f.slot); fillCard(f); fitCard(f); indexControls(); }
        }
        const src = focusSrc(f);
        if (!src) closeFocus(true);
        else { f.src = src; src.classList.add("sr-lifted"); }
      }
    }
    R.psig = psig;
    if (fadeIn) { fresh.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 260, easing: "ease-out" }); return; }
    if (!anim) { if (mode === "enter" && !reduced()) dropIn(fresh, true); return; }
    // FLIP
    const sc = R.scale || 1;
    const log = (R.flipLog = []);
    const leaving = [];
    old.querySelectorAll(":scope [data-sr-key]").forEach((el) => { if (!fresh.querySelector(`[data-sr-key="${CSS.escape(el.dataset.srKey)}"]`)) leaving.push(el); });
    fresh.querySelectorAll("[data-sr-key]").forEach((el, i) => {
      const k = el.dataset.srKey, a = oldPos.get(k);
      if (!a) {
        const par = el.parentElement.closest("[data-sr-key]");
        if (!par || oldPos.has(par.dataset.srKey)) {
          log.push(`in ${k}`);
          const onBoard = !!el.closest(".sr-board");
          el.animate(onBoard ? [{ opacity: 0, transform: "translateY(-34px) scale(1.08)" }, { opacity: 1, offset: 0.6, transform: "translateY(2px) scale(.99)" }, { opacity: 1, transform: "none" }]
            : [{ opacity: 0, transform: "translateY(-26px) scale(.97)" }, { opacity: 1, transform: "none" }], { duration: onBoard ? 520 : 420, delay: 60 + Math.min(i, 12) * 26, easing: "cubic-bezier(.2,.8,.2,1)", fill: "backwards" });
        }
        return;
      }
      const b = el.getBoundingClientRect();
      const inBoard = !!el.closest(".sr-board");
      const k2 = inBoard ? 1 / Math.cos(TILT * Math.PI / 180) : 1;
      const dx = (a.left - b.left) / sc, dy = (a.top - b.top) / sc * k2;
      const par = el.parentElement.closest("[data-sr-key]");
      if (par && oldPos.has(par.dataset.srKey)) { const pa = oldPos.get(par.dataset.srKey), pb = par.getBoundingClientRect(); if (Math.abs((pa.left - pb.left) - (a.left - b.left)) < 1 && Math.abs((pa.top - pb.top) - (a.top - b.top)) < 1) return; }
      if (Math.abs(dx) < 0.5 && Math.abs(dy) < 0.5) return;
      log.push(`move ${k} ${Math.round(dx)},${Math.round(dy)}`);
      el.animate([{ transform: `translate(${dx}px, ${dy}px)` }, { transform: "none" }], { duration: 460, easing: "cubic-bezier(.25,.8,.25,1)" });
    });
    if (leaving.length) {
      const ghost = document.createElement("div");
      ghost.className = "sr-ghosts";
      const sr = R.scene.getBoundingClientRect();
      leaving.forEach((el) => {
        const a = oldPos.get(el.dataset.srKey);
        if (!a || el.parentElement.closest("[data-sr-key]") && leaving.includes(el.parentElement.closest("[data-sr-key]"))) return;
        if (el.closest(".sr-board") && R.dropOut) return;     // dragged off the board: it already left with the hand
        const c = el.cloneNode(true);
        c.querySelectorAll("[id]").forEach((n) => n.removeAttribute("id"));
        c.querySelectorAll(".ctl").forEach((n) => n.classList.remove("ctl"));
        c.style.cssText += `;position:absolute;left:${(a.left - sr.left) / sc}px;top:${(a.top - sr.top) / sc}px;width:${a.width / sc}px;height:${a.height / sc}px;margin:0;transform:none;zoom:1`;
        if (c.querySelector(".sr-fit") && el.closest(".sr-board")) c.querySelector(".sr-fit").style.zoom = (a.width / sc) / ((c.querySelector(".sr-fit").firstElementChild || {}).offsetWidth || 1);
        c.classList.add("sr-ghost-out");
        log.push(`out ${el.dataset.srKey}`);
        ghost.appendChild(c);
      });
      R.dropOut = false;
      fresh.appendChild(ghost);
      ghost.animate([{ opacity: 1 }, { opacity: 0, transform: "translateY(-14px)" }], { duration: 300, easing: "ease-in", fill: "forwards" }).onfinish = () => ghost.remove();
    }
  }
  /* The room appears: one fade of the whole contents (one layer; no per-item animations to rasterize). */
  function dropIn(root) { root.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 300, easing: "ease-out" }); }

  /* ---------- jacks: where each unit's cables really plug in ----------
     From the illustrated face's own data (assets/units/index.json "jacks", auto-detected or hand-placed), as fractions of
     the face; real-world defaults otherwise: a stompbox takes the guitar on its right side and sends out on its left; an
     amp takes it at the front-panel INPUT and sends the speaker cable out of the back; rack gear is cabled at the back. */
  const DIR = { left: [-1, 0], right: [1, 0], top: [0, -1], bottom: [0, 1], front: [0, 1], rear: [0, 0] };
  function jacksOf(s, form) {
    const a = s && window.HRUnitArt && HRUnitArt.artFor(s);
    if (a && Array.isArray(a.jacks) && a.jacks.length) {
      const [x0, y0, W, H] = a.vb;
      return a.jacks.map((j) => ({ k: j.k, fx: (j.x - x0) / W, fy: (j.y - y0) / H, edge: j.edge, label: j.label || "" }));
    }
    form = form || (s ? layoutDesc(s).form : "rack");
    if (form === "pedal" || form === "wah") return [{ k: "in", fx: 1, fy: 0.22, edge: "right" }, { k: "out", fx: 0, fy: 0.22, edge: "left" }];
    if (form === "amp" || form === "nam") return [{ k: "in", fx: 0.07, fy: 0.72, edge: "front" }, { k: "out", fx: 0.92, fy: 0.5, edge: "rear" }];
    if (form === "cab") return [{ k: "in", fx: 0.97, fy: 0.5, edge: "rear" }];
    return [{ k: "in", fx: 0.85, fy: 0.5, edge: "rear" }, { k: "out", fx: 0.15, fy: 0.5, edge: "rear" }];
  }
  const jackOf = (js, k, i = 0) => js.filter((j) => j.k === k)[i] || js.find((j) => j.k === k) || null;
  const ABY_JACKS = [{ k: "in", fx: 1, fy: 0.24, edge: "right" }, { k: "out", fx: 0, fy: 0.2, edge: "left" }, { k: "out", fx: 0, fy: 0.42, edge: "left" }];
  /* The real neighbours of a unit in the signal path (what its input cable comes from, what its output goes to). */
  function neighbours(slot) {
    let res = null;
    const nm = (it, side) => {
      if (!it) return "";
      if (it.type === "split") return side === "in" ? `${(SPLIT_KIND[it.kind] || "Split")} (A/B·Y box)` : "the split’s mix (A + B)";
      const s = getSlot(it.slot); return s ? (s.cabinet && s.cabinet.label) || unitTitle(s) || s.reference_name : "unit";
    };
    const walk = (items, before, after) => items.forEach((it, i) => {
      const prev = i > 0 ? nm(items[i - 1], "out") : before, next = i < items.length - 1 ? nm(items[i + 1], "in") : after;
      if (it.type === "unit" && it.slot === slot) res = { prev, next };
      else if (it.type === "split") (it.lanes || []).forEach((ln, li) => walk(ln || [], `${nm(it, "in")}, lane ${li ? "B" : "A"}`, `the split’s mix → ${next}`));
    });
    walk(rackLayout(), "your guitar", "the output");
    return res || { prev: "", next: "" };
  }

  /* ---------- cables: sagging beziers with jack plugs ---------- */
  function boardGeom() {
    const wrap = R.content.querySelector(".sr-boardwrap"), b = wrap && wrap.querySelector(".sr-board");
    if (!b) return null;
    const BW = b.offsetWidth, BH = b.offsetHeight;
    const T = TILT * Math.PI / 180;
    const sw = R.sw, top = BOARD_FRONT - LIP - BH, bx0 = (sw - BW) / 2;
    const proj = (u, v) => {
      const X = bx0 + u - sw / 2, Y = v - BH;
      const Yp = Y * Math.cos(T), Zp = Y * Math.sin(T);
      const f = PERSP / (PERSP - Zp);
      return { x: sw / 2 + X * f, y: top + ORY + (BH + Yp - ORY) * f };
    };
    return { wrap, b, BW, BH, proj };
  }
  /* A plug at (x, y) whose cable leaves at angle a (radians). Barrel + sleeve + strain relief. */
  /* A plug is a metal tip, a body and a boot in the cable's own colour that narrows to exactly the cable's width
     (4 × scale) at 19.4. Its cable is one path that starts under the boot (at 14) and runs all the way to the other
     end: no separate end piece, no seam, no step in colour or width. */
  const PLUG_CABLE = 14;
  function plugSVG(x, y, a, s = 1) {
    const d = (a * 180 / Math.PI).toFixed(1);
    return `<g class="sr-plug" transform="translate(${x.toFixed(1)} ${y.toFixed(1)}) rotate(${d}) scale(${s})">
      <rect x="-1" y="-2.6" width="4" height="5.2" rx="1" fill="#c9ccd1"/>
      <path d="M9 -3.3 C13.5 -3.3 15 -2 19.4 -2 L19.4 2 C15 2 13.5 3.3 9 3.3Z" style="fill:var(--sr-cable,#0d0e10)"/>
      <rect x="3" y="-3.3" width="9" height="6.6" rx="1.8" fill="#26282c"/><rect x="3.6" y="-2.8" width="7.6" height="1.6" rx=".8" fill="rgba(255,255,255,.22)"/></g>`;
  }
  const cablePath = (d, k, extra = "") => `<path class="sr-cab-sh" d="${d}"/><path class="sr-cab-c" d="${d}"/><path class="sr-cab-h" d="${d}"/><path class="sr-cab-sig" style="--o:${(k % 5) * 7}" d="${d}"/>${extra}`;
  function layoutCables(M) {
    const svg = R.content.querySelector(".sr-cables"), hang = R.content.querySelector(".sr-hang");
    const stById = new Map(M.stations.map((q) => [q.id, q]));
    const G = boardGeom();
    const ents = new Map();
    if (G) {
      G.b.querySelectorAll(".sr-pd[data-e]").forEach((el) => {
        let x = 0, y = 0, n = el;
        while (n && n !== G.b) { x += n.offsetLeft; y += n.offsetTop; n = n.offsetParent; }
        const e = M.board[+el.dataset.e];
        const js = e && e.t === "aby" ? ABY_JACKS : jacksOf(e && e.s, e && (e.wah ? "wah" : "pedal"));
        ents.set(+el.dataset.e, { x, y, w: el.offsetWidth, h: el.offsetHeight, js });
      });
      R.boardBackY = G.proj(G.BW / 2, 0).y;
    }
    // a jack on the board, in board-plane px, with the way its cable leaves (top/front jacks: towards the back of the board)
    const bj = (e, k, i) => {
      const j = jackOf(e.js, k, i) || (k === "in" ? { fx: 1, fy: 0.22, edge: "right" } : { fx: 0, fy: 0.22, edge: "left" });
      const d = j.edge === "front" || j.edge === "rear" || j.edge === "bottom" ? [0, -1] : DIR[j.edge] || [k === "in" ? 1 : -1, 0];
      return { x: e.x + j.fx * e.w, y: e.y + j.fy * e.h, d };
    };
    const PL = 0.8, PLEN = PLUG_CABLE * PL;   // plug scale; where its cable starts
    const plugAt = (J) => plugSVG(J.x, J.y, Math.atan2(J.d[1], J.d[0]), PL);
    const tip = (J, extra = 0) => ({ x: J.x + J.d[0] * (PLEN + extra), y: J.y + J.d[1] * (PLEN + extra) });
    const plane = [], under = [], plugs = [];
    /* Routing on the board. A cable may run over the board only where it crosses no pedal (except at its own jack);
       otherwise it goes the way real patch cables do: out of the jack into the gap beside it, along the back of the
       board and under the pedals, drawn in a layer beneath them (so it shows only in the gaps). Plugs sit on top. */
    const rects = [...ents.values()].map((e) => ({ x: e.x + 1, y: e.y + 1, w: e.w - 2, h: e.h - 2 }));
    const YB = 9;   // the channel along the back of the board, behind the back row
    const bez = (c, n = 40) => { const o = []; for (let i = 0; i <= n; i++) { const t = i / n, u = 1 - t; o.push({ x: u * u * u * c[0].x + 3 * u * u * t * c[1].x + 3 * u * t * t * c[2].x + t * t * t * c[3].x, y: u * u * u * c[0].y + 3 * u * u * t * c[1].y + 3 * u * t * t * c[2].y + t * t * t * c[3].y }); } return o; };
    const pathD = (cs) => cs.map((c, i) => `${i ? "" : `M${f1(c[0].x)} ${f1(c[0].y)} `}C${f1(c[1].x)} ${f1(c[1].y)} ${f1(c[2].x)} ${f1(c[2].y)} ${f1(c[3].x)} ${f1(c[3].y)}`).join(" ");
    const crosses = (pts, ends) => pts.some((p) => !ends.some((q) => Math.hypot(p.x - q.x, p.y - q.y) < 5) && rects.some((r) => p.x > r.x && p.x < r.x + r.w && p.y > r.y && p.y < r.y + r.h));
    R.cableTop = []; R.cableRects = rects;
    const lay = (direct, alt, k, plug) => {
      const pts = direct.flatMap((c) => bez(c)), ends = [direct[0][0], direct[direct.length - 1][3]];
      if (!crosses(pts, ends)) { plane.push(cablePath(pathD(direct), k)); R.cableTop.push(pts); }
      else under.push(cablePath(pathD(alt()), k));
      if (plug) plugs.push(plug);
    };
    const pt = (p, d, l) => ({ x: p.x + d[0] * l, y: p.y + d[1] * l });
    // patch cables: short right-angle-plug cables from one pedal's output to the next one's input
    M.plane.forEach(([a, b, port], k) => {
      const A = ents.get(a), B = ents.get(b);
      if (!A || !B) return;
      const J1 = bj(A, "out", port), J2 = bj(B, "in");
      const p1 = tip(J1), p2 = tip(J2);
      const L = clamp(Math.hypot(p2.x - p1.x, p2.y - p1.y) * 0.45, 12, 90);
      const direct = [[p1, { x: p1.x + J1.d[0] * L, y: p1.y + J1.d[1] * L + 6 }, { x: p2.x + J2.d[0] * L, y: p2.y + J2.d[1] * L + 6 }, p2]];
      lay(direct, () => {
        const a1 = pt(p1, J1.d, 10), a2 = pt(p2, J2.d, 10), m = { x: (a1.x + a2.x) / 2, y: YB };
        return [[p1, a1, { x: a1.x, y: YB }, m], [m, { x: a2.x, y: YB }, a2, p2]];
      }, k, plugAt(J1) + plugAt(J2));
    });
    const floor = [], hung = [];
    // a jack on a backline unit (flat, facing you), in scene coordinates, from the face actually drawn
    const sceneRect = R.scene.getBoundingClientRect(), sc = R.scale || 1;
    const faceJack = (el, s, k) => {
      const face = el && (el.querySelector(".ua-face") || el.querySelector(".hw-body") || el);
      if (!face) return null;
      const r = face.getBoundingClientRect(), j = jackOf(jacksOf(s), k);
      if (!j || !r.width) return null;
      return { x: (r.left - sceneRect.left) / sc + j.fx * r.width / sc, y: (r.top - sceneRect.top) / sc + j.fy * r.height / sc, edge: j.edge, face: r };
    };
    const anchor = (node, entry, out, other, port) => {
      if (node === "in") return { x: R.sw / 2 + (G ? G.BW / 2 + 230 : 260), y: SH + 900, inp: true };   // from the player, well below the view
      if (node === "board") {
        if (!G) return null;
        const e = ents.get(entry);
        const st = stById.get(other) || (other === "rack" ? M.stations.find((q) => q.t === "rack") : null);
        const bx0 = (R.sw - G.BW) / 2;
        let u = e ? e.x + e.w / 2 : G.BW / 2;
        if (st && st.cx != null) u = clamp(st.cx - bx0, 30, G.BW - 30) * 0.4 + u * 0.6;
        u = clamp(u, 24, G.BW - 24);
        if (e && other !== "in") {   // from the jack to where the cable drops off the back of the board (under the pedals if it must)
          const J = bj(e, out ? "out" : "in", out ? port : 0), p = tip(J), end = { x: u, y: 2 };
          lay([[p, pt(p, J.d, 30), { x: u, y: Math.min(p.y - 30, 40) }, end]], () => {
            const a = pt(p, J.d, 10), m = { x: (a.x + u) / 2, y: YB };
            return [[p, a, { x: a.x, y: YB }, m], [m, { x: u, y: YB }, { x: u, y: YB }, end]];
          }, floor.length + 3, plugAt(J));
        }
        const p = G.proj(u, 2);
        return { x: p.x, y: p.y + 2 };
      }
      const st = stById.get(node);
      if (!st) return null;
      const stEl = R.content.querySelector(`.sr-st[data-sr-key="${CSS.escape(st.id)}"]`);
      // into an amp: its real front-panel INPUT; the cable hangs from the plug down the front to the floor
      if (!out && st.t === "stack" && st.amp && stEl) {
        const J = faceJack(stEl.querySelector(".sr-amp"), st.amp, "in");
        if (J) return { x: J.x, y: J.y, jack: true, foot: { x: J.x + (J.x > st.cx ? 30 : -30), y: FLOOR + 12 } };
      }
      // rack gear and the rest are cabled at the back: the cable runs under and behind the unit
      // (it comes out from behind on the side facing the gear at the other end)
      const ot = stById.get(other) || (other === "rack" ? M.stations.find((q) => q.t === "rack") : null);
      const side = ot && ot.cx != null && ot !== st ? Math.sign(ot.cx - st.cx) || 1 : other === "board" || other === "in" ? 0 : out ? 1 : -1;
      return { x: st.cx + side * st.w * 0.3, y: FLOOR - 6, behind: true };
    };
    M.floor.forEach((c, k) => {
      const A = anchor(c.a, c.ea, true, c.b, c.port), B = anchor(c.b, c.eb, false, c.a, 0);
      if (!A || !B) return;
      let d, end = B;
      if (B.jack) {
        // a soft catenary from the plug down the front of the cab to the floor
        const f = B.foot, top = { x: B.x, y: B.y };
        const hd = `M${f1(top.x)} ${f1(top.y)} C${f1(top.x)} ${f1(top.y + 46)} ${f1(f.x)} ${f1(f.y - 80)} ${f1(f.x)} ${f1(f.y)}`;
        hung.push(cablePath(hd, k) + `<g class="sr-jackplug" transform="translate(${f1(B.x)} ${f1(B.y)}) scale(${f1(clamp(R.scale ? 1 : 1, 0.8, 1))})"><circle r="5.2" fill="#1a1b1e"/><circle r="4" fill="#c4c7cc"/><circle r="2.4" fill="#2a2b2f"/><circle cx="-1.2" cy="-1.3" r="1" fill="rgba(255,255,255,.7)"/></g>`);
        end = f;
      }
      if (A.inp) {
        if (c.b === "board" && G) {
          // the guitar cable comes up from you, over the right end of the board, into the first pedal's input
          const e = ents.get(c.eb);
          if (e) {
            // over the board's right edge, then in; if that would cross a pedal, round the back of the board instead
            const J = bj(e, "in"), p = tip(J);
            let S = { x: G.BW + 4, y: p.y + 30 };
            const direct = [[S, { x: G.BW - 20, y: p.y + 20 }, pt(p, J.d, 40), p]];
            if (crosses(bez(direct[0]), [S, p])) S = { x: G.BW + 4, y: YB + 6 };
            lay(direct, () => { const a = pt(p, J.d, 10); return [[S, { x: G.BW - 14, y: YB }, { x: a.x, y: YB }, a], [a, a, p, p]]; }, 9, plugAt(J));
            const edge = G.proj(S.x, S.y);
            d = `M${f1(A.x)} ${f1(A.y)} C${f1(A.x - 10)} ${f1(A.y - 560)} ${f1(edge.x + 120)} ${f1(edge.y + 80)} ${f1(edge.x)} ${f1(edge.y)}`;
          }
        } else d = `M${f1(A.x)} ${f1(A.y)} C${f1(A.x - 30)} ${f1(A.y - 600)} ${f1(end.x + 80)} ${f1(end.y + 160)} ${f1(end.x)} ${f1(end.y)}`;
      } else {
        const sag = 22 + Math.min(60, Math.abs(end.x - A.x) * 0.08);
        const yb = Math.max(A.y, end.y);
        d = `M${f1(A.x)} ${f1(A.y)} C${f1(A.x + (end.x - A.x) * 0.25)} ${f1(yb + sag)} ${f1(A.x + (end.x - A.x) * 0.75)} ${f1(yb + sag * 0.6)} ${f1(end.x)} ${f1(end.y)}`;
      }
      if (d) floor.push(cablePath(d, k));
    });
    // speaker cable: out of the back of the head (a loop at its side) into the cab's rear jack plate
    M.stations.forEach((st, k) => {
      if (st.t !== "stack" || !st.amp || !st.cab || isCombo(st.amp)) return;
      const stEl = R.content.querySelector(`.sr-st[data-sr-key="${CSS.escape(st.id)}"]`), amp = stEl && stEl.querySelector(".sr-amp"), cab = stEl && stEl.querySelector(".sr-cab");
      if (!amp || !cab) return;
      const ar = amp.getBoundingClientRect(), cr = cab.getBoundingClientRect();
      const x = (Math.max(ar.right, cr.right) - sceneRect.left) / sc - 4, y1 = (ar.top - sceneRect.top) / sc + ar.height / sc * 0.55, y2 = (cr.top - sceneRect.top) / sc + cr.height / sc * 0.5;
      hung.push(cablePath(`M${f1(x - 8)} ${f1(y1)} C${f1(x + 24)} ${f1(y1 + 10)} ${f1(x + 30)} ${f1(y2 - 36)} ${f1(x - 6)} ${f1(y2)}`, k + 2));
    });
    if (svg) svg.innerHTML = `<g class="sr-cg">${floor.join("")}</g>`;
    if (hang) hang.innerHTML = `<g class="sr-cg">${hung.join("")}</g>`;
    if (G) {
      const ps = G.b.querySelector(".sr-patch");
      ps.setAttribute("viewBox", `0 0 ${G.BW} ${G.BH}`);
      ps.setAttribute("width", G.BW); ps.setAttribute("height", G.BH);
      ps.innerHTML = `<g class="sr-cg">${plane.join("")}${plugs.join("")}</g>`;
      const us = G.b.querySelector(".sr-under");
      if (us) { us.setAttribute("viewBox", `0 0 ${G.BW} ${G.BH}`); us.setAttribute("width", G.BW); us.setAttribute("height", G.BH); us.innerHTML = `<g class="sr-cg">${under.join("")}</g>`; }
    }
  }

  /* ---------- live bits: glows, speaker cones, tubes, jewels, rack meters, MIDI modules ---------- */
  function collectLive() {
    R.lit = [...R.content.querySelectorAll(".sr-glow, .sr-pool.warm")].map((el) => {
      const st = el.closest(".sr-st");
      const amp = st && st.querySelector(".sr-amp[data-sr-slot]");
      const id = el.dataset.srPool;
      let slot = amp ? +amp.dataset.srSlot : null;
      if (id && slot == null) { const s = R.content.querySelector(`.sr-st[data-sr-key="${CSS.escape(id)}"] .sr-amp[data-sr-slot]`); if (s) slot = +s.dataset.srSlot; }
      return { el, slot, pool: !!id, v: -1 };
    });
    R.pools = [...R.content.querySelectorAll(".sr-pool:not(.warm)")].map((el) => ({ el, o: -1, t: null }));
    R.rims = [...R.content.querySelectorAll(".sr-rim")].map((el) => ({ el, side: el.classList.contains("s") ? (el.classList.contains("r") ? "r" : "l") : "", o: -1, t: null }));
    R.casts = [...R.content.querySelectorAll(".sr-cast")].map((el) => ({ el, o: -1, t: null }));
    R.beamsL = [...R.content.querySelectorAll(".sr-beam")].map((el) => ({ el, o: -1, t: null }));
    R.cones = [...R.content.querySelectorAll(".sr-cabx .sr-spk")].map((el) => ({ el, slot: +el.closest("[data-sr-slot]").dataset.srSlot, v: -1 }));
    R.tubes = [...R.content.querySelectorAll(".sr-amp .sr-amptop")].map((el) => ({ el, slot: +el.closest("[data-sr-slot]").dataset.srSlot, v: -1 }));
    R.jewels = [...R.content.querySelectorAll(".sr-amp[data-sr-slot]")].map((el) => {
      const s = getSlot(+el.dataset.srSlot);
      const g = s && (s.params || []).find((p) => /^(gain|drive|pre ?amp|volume|lead|crunch)/i.test(p.name || p.id) && p.kind === "continuous");
      return { el, slot: +el.dataset.srSlot, pid: g && g.id, v: -1 };
    });
    R.meters = [...R.content.querySelectorAll(".sr-rmeter")].map((el) => ({ el, segs: [...el.children], n: -1 }));
    R.mods = [...R.content.querySelectorAll('.sr-mod[data-sr-mod="detector"], .sr-mod[data-sr-mod="bloom"]')].map((el) => ({ el, slot: +el.dataset.srSlot, scr: el.querySelector("[data-mod-note]"), bloom: el.dataset.srMod === "bloom" }));
    R.lvlShown = -1;
  }
  function onTelemetry(d) {
    if (!R.visible || !R.built || !d || document.hidden) return;
    const o = Math.sqrt(((d.out_rms_l || 0) ** 2 + (d.out_rms_r || 0) ** 2) / 2);
    const db = 20 * Math.log10(Math.max(1e-6, o));
    const t = clamp((db + 54) / 46, 0, 1);
    R.lvl += (t - R.lvl) * (t > R.lvl ? 0.65 : 0.22);
    R.lvlDb = db;
    auIn(d, db);   // the scene listens (see "the room listens")
    const live = R.live ? R.lvl > 0.04 : R.lvl > 0.08;
    if (live !== R.live) { R.live = live; R.wrap.classList.toggle("sr-sounding", live); }
    if (R.focus || R.drag) return;   // the room is behind the card (static under its blur) or under the hand
    const tn = now();
    if (tn - (R.telT || 0) < 95) return;   // DOM writes at most ~10 times a second
    R.telT = tn;
    if (!AU.raf && Math.abs(R.lvl - R.lvlShown) > 0.02) {   // (while the scene listens, its loop drives these)
      R.lvlShown = R.lvl;
      const L = R.lvl, rm = reduced();
      R.lit.forEach((g) => {
        const s = g.slot != null ? getSlot(g.slot) : null;
        const on = !s || !s.bypassed;
        const v = on ? (g.pool ? 0.35 + L * 0.65 : 0.38 + L * 0.62) : (g.pool ? 0.18 : 0.06);
        const r = Math.round(v * 100) / 100;
        if (r !== g.v) { g.v = r; g.el.style.opacity = r; }
      });
      R.tubes.forEach((g) => {
        const s = getSlot(g.slot);
        const v = s && !s.bypassed ? Math.round((0.55 + L * 0.45) * 20) / 20 : 0.1;
        if (v !== g.v) { g.v = v; g.el.style.setProperty("--tube", v); }
      });
      if (!rm) R.cones.forEach((c) => {
        const s = getSlot(c.slot);
        const v = s && !s.bypassed ? Math.round((1 + L * 0.035) * 1000) / 1000 : 1;
        if (v !== c.v) { c.v = v; c.el.style.transform = `scale(${v})`; }
      });
      const segs = Math.round(clamp((db + 48) / 48, 0, 1) * 10);
      R.meters.forEach((m) => {
        if (m.n === segs) return;
        m.n = segs;
        m.segs.forEach((el, i) => { const on = i < segs; if (el.classList.contains("on") !== on) el.classList.toggle("on", on); });
      });
    }
    if (R.mods.length && d.midi) {
      R.mods.forEach((m) => {
        if (m.bloom) {   // Bloom: the chord it is arpeggiating; the lamp follows the arpeggio
          const b = d.midi.bloom && (d.midi.bloom[m.slot] || d.midi.bloom[String(m.slot)]);
          setText(m.scr, b && b.chord ? blChord(b.chord) : "—");
          const on = !!(b && b.active);
          if (m.el.classList.contains("sr-note") !== on) m.el.classList.toggle("sr-note", on);
          return;
        }
        const u = unitMidi(d.midi, m.slot), ns = noteList(u.notes);
        const pitch = typeof u.pitch === "number" && u.pitch > 0 ? u.pitch : 0;
        const txt = pitch ? noteName(Math.round(69 + 12 * Math.log2(pitch / 440))) : ns.length ? noteName(ns[0]) : "—";
        setText(m.scr, txt);
        const on = !!(pitch || ns.length);
        if (m.el.classList.contains("sr-note") !== on) m.el.classList.toggle("sr-note", on);
      });
    }
  }

  /* ---------- in-place state (bypass, sound names, the split balance, scenes) ---------- */
  function paintState() {
    if (!R.built) return;
    const scope = R.root;
    (rackState.slots || []).forEach((s) => {
      scope.querySelectorAll(`[data-sr-slot="${s.slot_idx}"]`).forEach((el) => { if (el.classList.contains("sr-off") !== !!s.bypassed) el.classList.toggle("sr-off", !!s.bypassed); });
      scope.querySelectorAll(`.unit[data-unit="${s.slot_idx}"]`).forEach((u) => { if (u.classList.contains("bypassed") !== !!s.bypassed) u.classList.toggle("bypassed", !!s.bypassed); });
      if (midiKind(s) === "instrument" || midiKind(s) === "synth") {
        const kb = scope.querySelector(`.sr-kb[data-sr-slot="${s.slot_idx}"]`);
        if (kb) { const inf = kbInfo(s); setText(kb.querySelector("[data-kb-name]"), inf.name); setText(kb.querySelector("[data-kb-fam]"), inf.fam); }
      }
    });
    scope.querySelectorAll(".sr-aby[data-sr-split]").forEach((el) => {
      const m = splitModel(+el.dataset.srSplit), i = el.querySelector(".sr-aby-bal i");
      if (m && i) i.style.setProperty("--v", m.norm.toFixed(3));
    });
    // the jewel follows the gain knob (not the level: no filter re-evaluation on every telemetry frame)
    (R.jewels || []).forEach((j) => {
      const s = getSlot(j.slot), p = j.pid && PP(s, j.pid);
      const v = s && !s.bypassed ? Math.round((0.4 + 0.6 * (p ? p.norm : 0.5)) * 10) / 10 : 0;
      if (v !== j.v) { j.v = v; j.el.style.setProperty("--jewel", v); }
    });
    R.lvlShown = -1;
    if (R.focus) paintCardHead(R.focus);
  }

  function paintHUD(M) {
    if (!R.hud) return;
    const n = (rackState.slots || []).length;
    const ped = M.board.filter((e) => e.t === "pedal").length;
    const bits = [];
    if (ped) bits.push(`${ped} on the board`);
    const amps = M.stations.filter((s) => s.t === "stack" && s.amp).length;
    if (amps) bits.push(amps > 1 ? `${amps} amps` : "1 amp");
    const keys = M.stations.filter((s) => s.t === "keys").reduce((a, s) => a + s.insts.length, 0);
    if (keys) bits.push(keys > 1 ? `${keys} keyboards` : "1 keyboard");
    const rk = (M.stations.find((s) => s.t === "rack") || { units: [] }).units.length;
    if (rk) bits.push(`${rk} in the rack`);
    const sp = sceneById(R.sceneId);
    R.hud.innerHTML = n ? `<div class="sr-eyebrow">Room<span> · ${esc(sp.name)}</span></div><div class="sr-title">${esc(rackState.preset_name || "Current rig")}</div>
      <div class="sr-sub">${esc(bits.join(" · "))}${M.split ? ` · <span class="sr-ab">parallel A / B</span>` : ""}</div>` : "";
  }

  /* =====================================================================
     HOVER — highlight + name tag; the footswitch zone of a pedal
     ===================================================================== */
  let hoverEl = null;
  function hitInfo(el) {
    if (el.dataset.srSplit != null) {
      const sp = findSplit(+el.dataset.srSplit), m = splitModel(+el.dataset.srSplit);
      return { name: SPLIT_KIND[sp && sp.kind] || "Parallel split", sub: m ? m.fmt : "" };
    }
    const s = getSlot(+el.dataset.srSlot);
    if (!s) return null;
    if (el.dataset.srMic) {
      const cb = s.cabinet;
      return { name: "Cab & mics", sub: cb && cb.mic_a ? `${cb.mic_a.label} · ${cb.mic_a.position_label}${cb.mic_b ? ` + ${cb.mic_b.label}` : ""}` : (s.reference_name || "Cabinet") };
    }
    const d = layoutDesc(s);   // the form it is drawn in (a combo named "...Top Boost" is not a stompbox)
    const sub = [s.display_name !== s.reference_name ? s.display_name : "", d.form === "midi" ? (midiKind(s) === "detector" ? "Pitch → MIDI" : midiKind(s) === "midifx" ? "MIDI FX" : midiKind(s) === "bloom" ? "Arpeggiator" : "Instrument") : FORM_LABEL[d.form]].filter(Boolean).join(" · ");
    return { name: unitTitle(s) || s.reference_name, sub, off: s.bypassed };
  }
  function showTag(el, extra) {
    const inf = hitInfo(el);
    if (!inf) return hideTag();
    R.tag.innerHTML = `<b>${esc(inf.name)}</b>${inf.sub ? `<span>${esc(inf.sub)}</span>` : ""}${inf.off ? `<em>Bypassed</em>` : ""}${extra ? `<i>${esc(extra)}</i>` : ""}`;
    const r = el.getBoundingClientRect(), w = R.wrap.getBoundingClientRect();
    const x = clamp(r.left + r.width / 2 - w.left, 90, w.width - 90), y = Math.max(46, r.top - w.top - 8);
    R.tag.style.transform = `translate(${x.toFixed(0)}px, ${y.toFixed(0)}px) translate(-50%, -100%)`;
    R.tag.classList.add("show");
  }
  function hideTag() { R.tag && R.tag.classList.remove("show"); }
  /* The footswitch of a pedal on the board (screen rect), inflated so it's easy to hit. */
  function footRect(pd) {
    const f = pd.querySelector(".ua-foot, .footswitch, .wah-sw");
    let r;
    if (f) { const q = f.getBoundingClientRect(); if (q.width > 1) r = q; }
    if (!r) { const q = pd.getBoundingClientRect(); r = { left: q.left + q.width * 0.25, right: q.right - q.width * 0.25, top: q.bottom - q.height * 0.3, bottom: q.bottom - q.height * 0.06, width: q.width * 0.5, height: q.height * 0.24 }; }
    const pdr = pd.getBoundingClientRect();
    const m = Math.max(10, Math.min(pdr.width, pdr.height) * 0.12);
    return { left: r.left - m, right: r.right + m, top: r.top - m, bottom: r.bottom + m * 0.7, cx: (r.left + r.right) / 2, cy: (r.top + r.bottom) / 2, rad: Math.max(r.width, r.height) / 2 + m * 0.6 };
  }
  const inRect = (r, x, y) => x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
  function showFoot(fr) {
    const el = R.wrap.querySelector(".sr-foot"), w = R.wrap.getBoundingClientRect();
    el.style.transform = `translate(${(fr.cx - w.left).toFixed(1)}px, ${(fr.cy - w.top).toFixed(1)}px)`;
    el.style.setProperty("--fr", `${Math.max(14, fr.rad).toFixed(1)}px`);
    el.classList.add("show");
  }
  function hideFoot() { const el = R.wrap && R.wrap.querySelector(".sr-foot"); if (el) el.classList.remove("show"); }
  function stomp(pd, slot) {
    const s = getSlot(slot);
    if (!s) return;
    toggleBypass(slot);
    if (reduced()) return;
    const fit = pd.querySelector(".sr-fit");
    if (fit) fit.animate([{ transform: "none" }, { transform: "translateY(1.5px) scale(.985)", offset: 0.3 }, { transform: "none" }], { duration: 190, easing: "cubic-bezier(.3,.7,.3,1)" });
    const fr = footRect(pd), w = R.wrap.getBoundingClientRect();
    const ring = document.createElement("i");
    ring.className = `sr-stomp${s.bypassed ? " off" : ""}`;
    ring.style.left = `${(fr.cx - w.left).toFixed(1)}px`; ring.style.top = `${(fr.cy - w.top).toFixed(1)}px`;
    ring.innerHTML = `<b>${s.bypassed ? "OFF" : "ON"}</b>`;
    R.wrap.appendChild(ring);
    ring.animate([{ opacity: 0.9, transform: "translate(-50%,-50%) scale(.6)" }, { opacity: 0, transform: "translate(-50%,-50%) scale(1.6)" }], { duration: 520, easing: "cubic-bezier(.2,.8,.2,1)" }).onfinish = () => ring.remove();
    const lbl = ring.firstChild;
    lbl.animate([{ opacity: 0, transform: "translate(-50%, 0)" }, { opacity: 1, offset: 0.2 }, { opacity: 0, transform: "translate(-50%, -26px)" }], { duration: 520, easing: "ease-out", fill: "forwards" });
  }

  /* =====================================================================
     BOARD EDITING — drag along the board to reorder, off it to remove; drag in from the gear bag
     ===================================================================== */
  function locate(slot) {
    let r = null;
    const walk = (items, ctx) => items.forEach((it, i) => {
      if (r) return;
      if (it.type === "unit" && it.slot === slot) r = ctx ? { ...ctx, index: i } : { top: true };
      else if (it.type === "split") (it.lanes || []).forEach((l, li) => walk(l || [], { split: it.id, lane: li ? "B" : "A" }));
    });
    walk(rackLayout(), null);
    return r;
  }
  function chainTarget(slot, after) {
    const loc = locate(slot);
    if (!loc) return null;
    if (loc.split != null) return { split: loc.split, lane: loc.lane, index: loc.index + (after ? 1 : 0) };
    const pos = (rackState.slots || []).findIndex((s) => s.slot_idx === slot);
    return { pos: pos + (after ? 1 : 0) };
  }
  function boardRect() { const b = R.content && R.content.querySelector(".sr-board"); return b ? b.getBoundingClientRect() : null; }
  /* Nearest pedal on the board to (x, y) → where a drop lands (before / after it) and where the caret goes. */
  function dropTarget(x, y, exceptSlot) {
    const br = boardRect();
    if (!br) return null;
    const over = x >= br.left - 30 && x <= br.right + 30 && y >= br.top - 40 && y <= br.bottom + 30;
    if (!over) return { off: true };
    const all = [...R.content.querySelectorAll(".sr-board .sr-pd[data-sr-slot]")];
    const pds = all.filter((el) => +el.dataset.srSlot !== exceptSlot);
    // still nearest its own spot: it stays where it is
    const self = all.find((el) => +el.dataset.srSlot === exceptSlot);
    if (self) {
      const r = self.getBoundingClientRect();
      const dSelf = Math.hypot(x < r.left ? r.left - x : x > r.right ? x - r.right : 0, (y < r.top ? r.top - y : y > r.bottom ? y - r.bottom : 0) * 1.4);
      const dOther = pds.reduce((m, el) => { const q = el.getBoundingClientRect(); return Math.min(m, Math.hypot(x < q.left ? q.left - x : x > q.right ? x - q.right : 0, (y < q.top ? q.top - y : y > q.bottom ? y - q.bottom : 0) * 1.4)); }, Infinity);
      if (dSelf <= dOther) return { target: null, caret: null, stay: true };
    }
    if (!pds.length) {
      const first = rackLayout()[0];
      return { target: { pos: 0 }, caret: null, first };
    }
    let best = null, bd = Infinity;
    pds.forEach((el) => {
      const r = el.getBoundingClientRect();
      const dx = x < r.left ? r.left - x : x > r.right ? x - r.right : 0, dy = y < r.top ? r.top - y : y > r.bottom ? y - r.bottom : 0;
      const dd = Math.hypot(dx, dy * 1.4);
      if (dd < bd) { bd = dd; best = { el, r }; }
    });
    const after = x < best.r.left + best.r.width / 2;   // the board reads right to left (the guitar comes in on the right)
    const slot = +best.el.dataset.srSlot;
    return { target: chainTarget(slot, after), caret: { el: best.el, after }, slot, after };
  }
  function placeCaret(dt) {
    const c = R.content.querySelector(".sr-caret");
    if (!c) return;
    if (!dt || !dt.caret) { c.classList.remove("show"); return; }
    const el = dt.caret.el, b = el.offsetParent;
    let x = 0, y = 0, n = el;
    const board = R.content.querySelector(".sr-board");
    while (n && n !== board) { x += n.offsetLeft; y += n.offsetTop; n = n.offsetParent; }
    c.style.transform = `translate(${(dt.caret.after ? x - PGAP / 2 : x + el.offsetWidth + PGAP / 2).toFixed(1)}px, ${y.toFixed(1)}px)`;
    c.style.height = `${el.offsetHeight}px`;
    c.classList.add("show");
    void b;
  }
  function startDrag(e, info) {
    R.drag = { ...info, sx: e.clientX, sy: e.clientY, started: false, id: e.pointerId };
  }
  function dragMove(e) {
    const D = R.drag;
    if (!D) return;
    if (!D.started) {
      if (Math.hypot(e.clientX - D.sx, e.clientY - D.sy) < 6) return;
      D.started = true;
      hideTag(); hideFoot();
      if (hoverEl) { hoverEl.classList.remove("sr-hover"); hoverEl = null; }
      const g = R.wrap.querySelector(".sr-ghost");
      g.getAnimations().forEach((a) => a.cancel());
      g.innerHTML = D.ghostHTML;
      g.hidden = false;
      g.style.width = `${D.gw}px`;
      if (D.src) D.src.classList.add("sr-dragsrc");
      R.wrap.classList.add("sr-dragging");
      if (D.kind === "move") R.wrap.classList.add("sr-dragging-move");
    }
    const w = R.wrap.getBoundingClientRect();
    const dt = dropTarget(e.clientX, e.clientY, D.kind === "move" ? D.slot : -1);
    D.dt = dt;
    const off = !dt || dt.off;
    const g = R.wrap.querySelector(".sr-ghost");
    g.classList.toggle("remove", off && D.kind === "move");
    g.classList.toggle("nodrop", off && D.kind === "new");
    g.style.transform = `translate(${(e.clientX - w.left - D.gw / 2).toFixed(1)}px, ${(e.clientY - w.top - D.gh * 0.55).toFixed(1)}px) rotate(${off ? -6 : -2}deg) scale(${off ? 0.94 : 1.04})`;
    placeCaret(off ? null : dt);
    R.wrap.classList.toggle("sr-drop-off", off && D.kind === "move");
  }
  function dragEnd(e, cancel) {
    const D = R.drag;
    R.drag = null;
    if (!D || !D.started) return false;
    const g = R.wrap.querySelector(".sr-ghost");
    R.wrap.classList.remove("sr-dragging", "sr-dragging-move", "sr-drop-off");
    placeCaret(null);
    if (D.src) D.src.classList.remove("sr-dragsrc");
    const dt = D.dt;
    const fade = () => { g.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 160, fill: "forwards" }).onfinish = () => { g.hidden = true; g.innerHTML = ""; g.getAnimations().forEach((a) => a.cancel()); }; };
    if (cancel || !dt) { fade(); return true; }
    if (D.kind === "move") {
      if (dt.off) {
        const s = getSlot(D.slot);
        R.dropOut = true;
        g.animate([{ opacity: 1 }, { opacity: 0, transform: g.style.transform + " translateY(30px) rotate(-14deg) scale(.7)" }], { duration: 300, easing: "ease-in", fill: "forwards" }).onfinish = () => { g.hidden = true; g.innerHTML = ""; g.getAnimations().forEach((a) => a.cancel()); };
        removeModuleFromRack(D.slot);
        if (s) toast(`${unitTitle(s) || s.reference_name} is off the board`);
        return true;
      }
      fade();
      if (dt.target && !(dt.slot === D.slot)) moveUnit(D.slot, dt.target);
      return true;
    }
    fade();
    if (dt.off) return true;
    insertComponent(D.tid, dt.target);
    return true;
  }
  function pedalGhostHTML(pd) {
    const fit = pd.querySelector(".sr-fit");
    const r = pd.getBoundingClientRect();
    const nat = fit && fit.firstElementChild ? fit.firstElementChild.offsetWidth : r.width;
    const w = r.width * 1.08;
    return { html: `<div class="sr-fit" style="zoom:${(w / Math.max(1, nat)).toFixed(4)}">${fit ? fit.innerHTML : ""}</div>`, w, h: r.height / Math.cos(TILT * Math.PI / 180) * 1.08 };
  }

  /* ---------- the gear bag: pedals to drag onto the board ---------- */
  const BAG_CATS = ["Drive & Fuzz", "Dynamics", "EQ & Filter", "Modulation", "Delay", "Reverb", "Pitch & Synth", "Creative", "Utility"];
  function bagItems() {
    const list = (typeof comps !== "undefined" && comps.list) || [];
    return list.filter((c) => c.table_id !== "split" && !c.ucid && typeof c.table_id === "number").filter((c) => {
      try { const f = layoutDesc(compSlot(c)).form; return f === "pedal" || f === "wah"; } catch (e) { return false; }
    });
  }
  function openBag() {
    const bag = R.root.querySelector(".sr-bag");
    const items = bagItems();
    if (!items.length) { toast("The gear bag fills up once the engine’s components load."); return; }
    const cats = BAG_CATS.filter((c) => items.some((x) => x.category === c));
    items.forEach((x) => { if (!cats.includes(x.category)) cats.push(x.category); });
    R.bagCat = R.bagCat && cats.includes(R.bagCat) ? R.bagCat : cats[0];
    const grid = () => items.filter((x) => x.category === R.bagCat).map((c) => `<div class="sr-bag-it" data-tid="${c.table_id}" title="${esc(c.ref || c.name)} — ${esc(c.name)}\nDrag onto the board · click to add at the end">
        <div class="sr-bag-th" data-thumb="${c.table_id}"></div><span>${esc(c.ref || c.name)}</span></div>`).join("");
    bag.innerHTML = `<div class="sr-bag-h"><b>Gear bag</b><small>Drag a pedal onto the board</small><button class="icon-btn" data-sr-act="bag" aria-label="Close the gear bag">${ICON.close}</button></div>
      <div class="sr-bag-cats">${cats.map((c) => `<button class="sr-bag-cat${c === R.bagCat ? " on" : ""}" data-bag-cat="${esc(c)}">${esc(c.replace(/ & /, " & "))}</button>`).join("")}</div>
      <div class="sr-bag-grid">${grid()}</div>`;
    bag.hidden = false;
    R.root.querySelector(".sr-bagbtn").setAttribute("aria-expanded", "true");
    R.wrap.classList.add("sr-bag-open");
    if (!R.bagOpen) { R.bagOpen = true; if (R.model && !R.model.board.length) render("change"); }
    fillBagThumbs(bag);
    if (!reduced()) bag.animate([{ opacity: 0, transform: "translateX(-14px)" }, { opacity: 1, transform: "none" }], { duration: 220, easing: "cubic-bezier(.2,.8,.2,1)" });
  }
  function fillBagThumbs(bag) {
    bag.querySelectorAll(".sr-bag-th").forEach((box) => {
      const tid = +box.dataset.thumb;
      if (window.HRUnitArt && HRUnitArt.thumb(box, tid)) return;
      const c = comps.byTid && comps.byTid.get(tid);
      if (!c) return;
      box.innerHTML = `<div class="ti" inert>${unitPreviewHTML(compSlot(c))}</div>`;
      const ti = box.firstChild, w = ti.offsetWidth || 1, h = ti.offsetHeight || 1;
      ti.style.transform = `scale(${Math.min((box.clientWidth - 6) / w, (box.clientHeight - 6) / h, 0.5)}) translate(-50%, -50%)`;
    });
  }
  function closeBag() {
    const bag = R.root && R.root.querySelector(".sr-bag");
    if (!bag || bag.hidden) return;
    bag.hidden = true;
    R.root.querySelector(".sr-bagbtn").setAttribute("aria-expanded", "false");
    R.wrap.classList.remove("sr-bag-open");
    if (R.bagOpen) { R.bagOpen = false; if (R.model && !R.model.board.length) render("change"); }
  }
  function addToBoardEnd(tid) {
    const pds = [...R.content.querySelectorAll(".sr-board .sr-pd[data-sr-slot]")];
    const last = pds[pds.length - 1];
    insertComponent(tid, last ? chainTarget(+last.dataset.srSlot, true) : { pos: 0 });
  }

  /* =====================================================================
     FOCUS — pick a unit up, edit it, put it back (shared with the Signal Chain)
     ===================================================================== */
  function focusSrc(f) {
    if (!f.room) {   // the chain: its unit's body, wherever the chain has drawn it now
      const u = document.querySelector(`#rack-bay .unit[data-unit="${f.slot}"]`);
      return u ? (u.querySelector(":scope > .hw-body") || u) : null;
    }
    if (!R.content) return null;
    if (f.split != null) return R.content.querySelector(`[data-sr-split="${f.split}"]`);
    if (f.mode === "mic") return R.content.querySelector(`[data-sr-mic][data-sr-slot="${f.slot}"]`) || R.content.querySelector(`[data-sr-slot="${f.slot}"]`);
    return R.content.querySelector(`.sr-hit[data-sr-slot="${f.slot}"]:not([data-sr-mic])`) || R.content.querySelector(`[data-sr-slot="${f.slot}"]`);
  }
  const XSVG = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M4 4l8 8M12 4l-8 8"/></svg>`;
  function cardHeadHTML(f) {
    if (f.split != null) {
      const sp = findSplit(f.split);
      return `<div class="sr-ch-t"><div class="sr-ch-name">${esc(SPLIT_KIND[sp && sp.kind] || "Parallel split")}</div><div class="sr-ch-sub">A/B · Y box — the signal divides into two lanes and mixes back together</div></div>
        <span class="sr-ch-sp"></span><button class="icon-btn sr-x" data-sr-act="close" title="Put it back (Esc)" aria-label="Close">${XSVG}</button>`;
    }
    const s = getSlot(f.slot);
    if (!s) return "";
    const d = describe(s);
    if (f.mode === "mic") {
      const cb = s.cabinet;
      return `<div class="sr-ch-t"><div class="sr-ch-name">Cab &amp; mics</div><div class="sr-ch-sub">${esc(cb ? `${cb.label || s.reference_name}${cb.speakers ? ` · ${cb.speakers}` : ""}` : s.reference_name)}</div></div>
        <span class="sr-ch-sp"></span><button class="text-btn" data-sr-act="face" title="The unit's faceplate: tone, level, room &amp; stereo">Faceplate</button>
        <button class="icon-btn sr-x" data-sr-act="close" title="Put it back (Esc)" aria-label="Close">${XSVG}</button>`;
    }
    const lane = (() => { let r = ""; const walk = (items, tag) => items.forEach((it) => { if (it.type === "unit" && it.slot === s.slot_idx) r = tag; else if (it.type === "split") (it.lanes || []).forEach((l, li) => walk(l, `Lane ${li ? "B" : "A"}`)); }); walk(rackLayout(), ""); return r; })();
    const kind = d.form === "midi" ? (midiKind(s) === "detector" ? "Pitch → MIDI" : midiKind(s) === "midifx" ? "MIDI FX" : midiKind(s) === "bloom" ? "Arpeggiator" : "Instrument") : FORM_LABEL[layoutDesc(s).form] || "";
    const det = window.ComponentDetail && typeof comps !== "undefined" && comps.byTid && comps.byTid.has(s.table_id);
    const micable = d.form === "cab";
    const inRoom = !!f.room;
    return `<div class="sr-ch-t"><div class="sr-ch-name">${esc(unitTitle(s) || s.reference_name)}</div><div class="sr-ch-sub">${esc([s.display_name !== s.reference_name ? s.display_name : "", kind, lane].filter(Boolean).join(" · "))}</div></div>
      <span class="sr-ch-sp"></span>
      <button class="sr-pwr${s.bypassed ? "" : " on"}" data-sr-act="bypass" title="${s.bypassed ? "Switch it on" : "Bypass it"}" aria-pressed="${!s.bypassed}">${PWR_SVG}<span class="sr-pwr-l"><span${s.bypassed ? "" : ' class="ghost" aria-hidden="true"'}>Bypassed</span><span${s.bypassed ? ' class="ghost" aria-hidden="true"' : ""}>On</span></span></button>
      ${micable ? `<button class="text-btn" data-sr-act="mic" title="Choose the cabinet and place the microphones">Cab &amp; mics</button>` : ""}
      ${det ? `<button class="text-btn" data-sr-act="details" title="How this unit is modelled">Details</button>` : ""}
      ${inRoom ? `<button class="text-btn" data-sr-act="chain" title="Show it in the Signal Chain">In chain</button>` : `<button class="text-btn" data-sr-act="room" title="See it in the Room">In the room</button>`}
      <div class="sr-zoom" role="group" aria-label="Zoom"><button data-sr-zoom="-1" title="Zoom out (−)" aria-label="Zoom out">−</button><span>100%</span><button data-sr-zoom="1" title="Zoom in (+ · pinch · ⌃-scroll)" aria-label="Zoom in">+</button><button data-sr-zoom="0" title="Fit (0)">Fit</button></div>
      <button class="icon-btn sr-x" data-sr-act="close" title="Put it back (Esc)" aria-label="Close">${XSVG}</button>`;
  }
  function paintCardHead(f) {
    if (!f.card) return;
    const hd = f.card.querySelector(".sr-ch");
    if (f.split == null && f.mode !== "mic") {
      const s = getSlot(f.slot);
      if (!s) return;
      const b = hd.querySelector(".sr-pwr");
      if (b && b.classList.contains("on") === !!s.bypassed) hd.innerHTML = cardHeadHTML(f);
      f.card.classList.toggle("sr-c-off", !!s.bypassed);
    }
  }
  function drawerFor(s) {
    const k = expKey(s), had = ui.expanded.has(k), prevC = RENDER.compact;
    ui.expanded.add(k); RENDER.compact = false;
    try { return drawerHTML(s); } finally { if (!had) ui.expanded.delete(k); RENDER.compact = prevC; }
  }
  function splitCardHTML(id) {
    const sp = findSplit(id), xm = splitModel(id);
    const lane = (L, items) => {
      const ks = flatSlots(items || []).map(getSlot).filter(Boolean);
      return `<div class="sr-ln"><span class="lane2-badge">${L}</span><div><b>${esc(laneLabel(items || []))}</b><small>${ks.length ? ks.map((s) => esc(unitTitle(s))).join(" → ") : "passes the signal through"}</small></div></div>`;
    };
    return `<div class="sr-split-card">
      <div class="sr-lns">${lane("A", sp && sp.lanes && sp.lanes[0])}${lane("B", sp && sp.lanes && sp.lanes[1])}</div>
      ${xm ? `<div class="split-mix sr-split-mix"><span class="sm-t">Mix</span><span class="sm-l">A</span>
        <div class="ctl xfade sm-xf" data-kind="xfade" data-slot="X${id}" data-pid="xfade" role="slider" tabindex="0" aria-label="Lane balance" style="--v:${xm.norm.toFixed(3)}"><div class="xf-track"><div class="xf-thumb"></div></div><span class="ctl-val">${esc(xm.fmt)}</span></div>
        <span class="sm-l">B</span></div>` : ""}
    </div>`;
  }
  function cardBodyHTML(f) {
    if (f.split != null) return { html: splitCardHTML(f.split), tall: false, nw: 560 };
    if (f.mode === "mic") return { html: micCardHTML(getSlot(f.slot)), tall: false, nw: 760, mic: true };
    const s = getSlot(f.slot);
    const st = f.station;
    const face = faceHTML(s, st && (st.amp || st.inherit));
    const place = layoutDesc(s).form;
    const dr = extraParams(s).length && !(window.HRCabinet && HRCabinet.isCabinet(s)) ? drawerFor(s) : "";   // the Cabinet's plate already has every control
    const tall = place === "pedal" || place === "wah";
    return { html: `<div class="sr-cface"><div class="sr-fit sr-cfit" data-sr-face="${s.slot_idx}">${face.html}</div></div>${dr ? `<div class="sr-cmore${f.moreOff ? " off" : ""}">${dr}</div>` : ""}`, tall, nw: face.w };
  }
  function stationOf(slot) {
    const M = R.model || buildModel();
    return M.stations.find((st) => st.amp && st.amp.slot_idx === slot || st.cab && st.cab.slot_idx === slot) || null;
  }
  /* Where the card lives: the room's own overlay, or one over the Signal Chain. */
  /* The card's overlay: over the whole window (all three panes, the top bar dimmed), shared by the Room and the Chain. */
  function chainHost() {
    let h = $("sr-fhost");
    if (!h) {
      h = document.createElement("div");
      h.id = "sr-fhost";
      h.className = "sr-focus sr-fhost";
      h.hidden = true;
      h.innerHTML = `<div class="sr-dim"></div>`;
      document.body.appendChild(h);
      wireHost(h);
      if (window.HRUnitArt && HRUnitArt.bindRoot) HRUnitArt.bindRoot(h);   // lever clicks + hover readouts on the card's face
    }
    return h;
  }


  function openFocus(target, opt = {}) {
    const room = !opt.host;
    if (room && !R.built) return;
    const host = chainHost();
    if (!host) return;
    if (R.focus) {
      const same = target.split != null ? R.focus.split === target.split : R.focus.slot === target.slot && (R.focus.mode || "") === (target.mode || "");
      if (same && !!R.focus.room === room) return;
      closeFocus(true);
    }
    const f = { ...target, host, room, station: target.slot != null && room ? stationOf(target.slot) : null, moreOff: false };
    if (f.slot != null) { f.ukey = R.ukeys && R.ukeys.get(f.slot); f.tid = (getSlot(f.slot) || {}).table_id; }
    f.psig = `${rackState.preset_idx}|${rackState.preset_name}`;
    const src = opt.src || focusSrc(f);
    if (!src) return;
    f.src = src;
    hideTag(); hideFoot(); closeSpaces();
    if (f.slot != null && ui.selected !== f.slot && f.mode !== "mic") selectUnit(f.slot, false);
    host.hidden = false;
    const card = document.createElement("div");
    card.className = "sr-card";
    card.setAttribute("role", "dialog");
    card.setAttribute("aria-modal", "true");
    card.tabIndex = -1;
    f.card = card;
    fillCard(f);
    host.appendChild(card);
    R.focus = f;
    if (room) R.wrap.classList.add("sr-focused");
    else document.body.classList.add("sr-chain-focused");
    fitCard(f);
    indexControls();
    if (typeof indexPianos === "function") indexPianos();
    if (f.slot != null) refreshDisplays(f.slot);
    if (f.mode === "mic") paintMic(f);
    const a = src.getBoundingClientRect();
    src.classList.add("sr-lifted");
    card.focus({ preventScroll: true });
    const dim = host.querySelector(".sr-dim");
    if (reduced()) { dim.style.opacity = 1; f.landed = true; requestAnimationFrame(() => crispFace(f)); return; }
    dim.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 320, easing: "ease-out", fill: "both" });
    const fx = card.querySelector(".sr-cface, .sr-split-card, .sr-mk");
    const b = fx.getBoundingClientRect(), c = card.getBoundingClientRect();
    const s = clamp(a.width / Math.max(1, b.width), 0.05, 1.5);
    const ox = b.left + b.width / 2 - c.left, oy = b.top + b.height / 2 - c.top;
    card.style.transformOrigin = `${ox}px ${oy}px`;
    const dx = a.left + a.width / 2 - (b.left + b.width / 2), dy = a.top + a.height / 2 - (b.top + b.height / 2);
    const tilt = src.closest(".sr-board") ? TILT : 0;
    card.style.willChange = "transform";   // only for the flight: a layer kept for transforms may keep its first raster scale
    card.animate([
      { transform: `translate(${dx}px, ${dy}px) scale(${s}) perspective(900px) rotateX(${tilt}deg)`, opacity: f.mode === "mic" ? 0 : 1 },
      { offset: 0.55, opacity: 1 },
      { transform: "translate(0px, 0px) scale(1) perspective(900px) rotateX(0deg)", opacity: 1 },
    ], { duration: 540, easing: "cubic-bezier(.18,.84,.24,1)" }).onfinish = () => {
      // landed: no transform left on it; the face's drawing is re-rendered at its final pixel size
      card.style.willChange = "";
      f.landed = true;
      if (R.focus === f) crispFace(f);
    };
    card.querySelectorAll(".sr-ch, .sr-cmore, .sr-cft, .sr-mk-side, .sr-ccab").forEach((el) => el.animate([{ opacity: 0, transform: "translateY(6px)" }, { opacity: 1, transform: "none" }], { duration: 260, delay: 300, easing: "ease-out", fill: "backwards" }));
  }
  function fillCard(f) {
    const body = cardBodyHTML(f);
    f.tall = body.tall; f.nw = body.nw;
    f.card.className = `sr-card${body.tall ? " tall" : ""}${f.split != null ? " sr-c-split" : ""}${body.mic ? " sr-c-mic" : ""}`;
    f.card.setAttribute("aria-label", f.mode === "mic" ? "Cab and mics" : f.split != null ? "Parallel split" : (getSlot(f.slot) || {}).reference_name || "Unit");
    f.card.innerHTML = `<div class="sr-ch">${cardHeadHTML(f)}</div><div class="sr-cbody">${body.html}</div>
      <div class="sr-cft">${body.mic ? `<span><kbd>drag</kbd> a mic onto Cap · Cap edge · Cone · pick its model in the locker · <kbd>drag</kbd> mic B back from the grille</span>` : `<span><kbd>drag</kbd> knobs · <kbd>⇧</kbd> fine · <kbd>double-click</kbd> resets · <kbd>right-click</kbd> macros</span>`}<span><kbd>Esc</kbd> puts it back</span></div>`;
    paintCardHead(f);
  }
  /* Size the face to the pane: big enough to play with, never bigger than the pane. */
  function fitCard(f) {
    const card = f.card, fx = card && card.querySelector(".sr-cfit");
    if (!fx) { cardCables(f); return; }
    // the card covers the whole window: the unit gets most of it (aspect-fit), leaving gutters for its cables and their labels
    const W = window.innerWidth, H = window.innerHeight;
    card.classList.add("sr-c-cabled");
    // jacks on top (Fuzz Face, many boutique boxes): leave room above the unit for the plugs and their cables
    const sl = f.slot != null ? getSlot(f.slot) : null;
    const topJ = !!sl && f.mode !== "mic" && jacksOf(sl).some((j) => j.edge === "top" && (j.k === "in" || j.k === "out"));
    card.classList.toggle("sr-c-topjacks", topJ);
    fx.style.zoom = 1;
    const nat = fx.firstElementChild;
    const nw = nat ? nat.offsetWidth : 300, nh = nat ? nat.offsetHeight : 200;
    const more = card.querySelector(".sr-cmore:not(.off)");
    const head = 44 + 22 + 30 + 12 + (topJ ? 70 : 0);
    const gut = clamp(W * 0.05, 64, 170);   // labels move under the unit when the gutter is narrow
    let availW = W - 2 * gut, availH = H - head - 56 - (f.slot != null && f.mode !== "mic" ? 44 : 0);   // a band at the bottom for the cable labels
    if (more) {
      if (f.tall) availW -= Math.min(340, more.offsetWidth || 300) + 28;
      else availH -= Math.min(220, more.scrollHeight || 0) + 14;
    }
    const k = clamp(Math.min(4, availW / Math.max(1, nw), availH / Math.max(1, nh)), 0.3, 4);
    const uz = f.uz || 1;   // the user's own zoom on top of the fit (pinch, ⌃-scroll, the +/− buttons)
    fx.style.zoom = (k * uz).toFixed(4);
    f.k = k;
    card.classList.toggle("sr-zoomed", uz > 1.001);
    card.style.setProperty("--cw", `${Math.round(nw * k)}px`);
    if (uz > 1.001) card.querySelectorAll(".sr-ccab").forEach((el) => el.remove());   // cables come back at the fit
    else cardCables(f);
    paintZoom(f);
    if (f.landed) crispFace(f);
  }
  /* Zoom past the fit: up to 8× the fitted size, around the pointer (or the middle). The face then scrolls inside
     the card (scroll, or drag the background); Fit / 0 goes back. Same card in the Room and over the Signal Chain. */
  const UZ_MAX = 8;
  function zoomBy(factor, cx, cy) {
    const f = R.focus;
    if (!f || !f.card || f.mode === "mic" || f.split != null) return;
    const box = f.card.querySelector(".sr-cface");
    if (!box || !f.card.querySelector(".sr-cfit")) return;
    const old = f.uz || 1, uz = factor === 0 ? 1 : clamp(old * factor, 1, UZ_MAX);
    if (Math.abs(uz - old) < 1e-3) return;
    const r0 = box.getBoundingClientRect();
    const px = cx != null ? cx - r0.left : r0.width / 2, py = cy != null ? cy - r0.top : r0.height / 2;
    const fx = f.card.querySelector(".sr-cfit"), fr = fx.getBoundingClientRect();
    const ux = (cx != null ? cx : r0.left + r0.width / 2) - fr.left, uy = (cy != null ? cy : r0.top + r0.height / 2) - fr.top;   // the face point under the pointer
    f.uz = uz;
    fitCard(f);
    if (uz > 1.001) {
      const fr2 = fx.getBoundingClientRect(), r1 = box.getBoundingClientRect(), q = uz / old;
      box.scrollLeft += (fr2.left + ux * q) - (r1.left + px);
      box.scrollTop += (fr2.top + uy * q) - (r1.top + py);
    }
  }
  function paintZoom(f) {
    const z = f.card && f.card.querySelector(".sr-zoom");
    if (!z) return;
    const uz = f.uz || 1;
    z.querySelector("span").textContent = `${Math.round(uz * 100)}%`;
    z.querySelector('[data-sr-zoom="-1"]').disabled = uz <= 1.001;
    z.querySelector('[data-sr-zoom="1"]').disabled = uz >= UZ_MAX - 1e-3;
    z.querySelector('[data-sr-zoom="0"]').disabled = uz <= 1.001;
  }
  /* Once landed (and after every re-fit): make WebKit redraw the face's SVG filters at the face's real size.
     Measured in a real 2× WKWebView: the filters on the moving-parts layer (unlit LEDs and legends, dimmed buttons,
     drop shadows) are first rendered mid-flight, at the card's small starting scale, and WebKit keeps reusing those
     bitmaps after landing, stretched (soft, blocky legends). A no-op change to each filter's region rebuilds them.
     The static drawing stays a plain SVG <img> (it re-rasterises at its final size by itself; a canvas copy was a
     resampled bitmap and came out softer); only its filtered groups, which WebKit draws low-res inside an image,
     need the crisp version of the drawing (HRUnitArt.crispBg). */
  function crispFace(f) {
    const card = f.card;
    if (!card) return;
    const fl = [...card.querySelectorAll(".sr-cfit svg.ua-fg filter"), ...document.querySelectorAll("#ua-filters filter")];
    fl.forEach((el) => {
      const w0 = el.dataset.w0 || (el.dataset.w0 = el.getAttribute("width") || "120%");
      el.setAttribute("width", el.getAttribute("width") === w0 ? w0.replace(/[\d.]+/, (n) => String(+n + 0.001)) : w0);
    });
    if (window.HRUnitArt && HRUnitArt.crispBg) card.querySelectorAll(".sr-cfit img.ua-bg").forEach((img) => HRUnitArt.crispBg(img));
  }
  /* The unit's own cables on the card: plugged into its real jacks, running off to the window edges,
     labelled with where they come from and go to (its real neighbours in the chain). */
  function cardCables(f) {
    const card = f.card;
    card.querySelectorAll(".sr-ccab").forEach((el) => el.remove());
    if (f.split != null || f.mode === "mic" || f.slot == null) return;
    const s = getSlot(f.slot);
    const face = s && (card.querySelector(".sr-cfit .ua-face") || card.querySelector(".sr-cfit .hw-body") || card.querySelector(".sr-cfit > *"));
    if (!face) return;
    const nb = neighbours(f.slot);
    if (!nb.prev && !nb.next) return;
    const W = window.innerWidth, H = window.innerHeight;
    const c = card.getBoundingClientRect(), r = face.getBoundingClientRect();
    if (!r.width) return;
    const form = layoutDesc(s).form, js = jacksOf(s, form), pedal = form === "pedal" || form === "wah";
    const ps = clamp(Math.sqrt(r.width * r.height) / 120, 1.2, 3), sw = ps * 4, PLEN = PLUG_CABLE * ps;
    const back = [], front = [], labels = [];
    const roomy = (side) => (side === "right" ? W - r.right : r.left) > 168;   // room for a label beside the unit
    const ft = card.querySelector(".sr-cft"), fb = ft ? ft.getBoundingClientRect().bottom : r.bottom + 30;
    const low = clamp(Math.max(r.bottom + 44, fb + 46), 90, H - 14);   // under the unit and its footer
    const cab = (d, k) => `<path class="sr-cab-sh" d="${d}" style="stroke-width:${f1(sw + 3)}px"/><path class="sr-cab-c" d="${d}" style="stroke-width:${f1(sw)}px"/><path class="sr-cab-h" d="${d}" style="stroke-width:${f1(ps * 0.9)}px"/>`;
    const lab = (k, side, y, name) => labels.push(`<div class="sr-clab ${side}" style="top:${f1(clamp(y, 70, H - 40))}px"><small>${k === "in" ? "from" : form === "cab" ? "mic’d · to" : "to"}</small><b>${esc(name)}</b></div>`);
    ["in", "out"].forEach((k) => {
      const name = k === "in" ? nb.prev : nb.next;
      // a cab's way on is its microphone: that cable comes from behind it too
      const j = form === "cab" && k === "out" ? { k, fx: 1, fy: 0.62, edge: "rear" } : jackOf(js, k);
      if (!name || !j) return;
      const P = { x: r.left + j.fx * r.width, y: r.top + j.fy * r.height };
      let side = j.edge === "left" || j.edge === "right" ? j.edge : pedal ? (j.fx > 0.5 ? "right" : "left") : k === "in" ? "left" : "right";
      const sx = side === "right" ? 1 : -1, ex = side === "right" ? W + 40 : -40;
      if (j.edge === "left" || j.edge === "right" || j.edge === "top") {
        const d = j.edge === "top" ? [0, -1] : [sx, 0];
        const t = { x: P.x + d[0] * PLEN, y: P.y + d[1] * PLEN };
        const ey = !roomy(side) ? low : j.edge === "top" ? clamp(Math.max(t.y + 110, r.top + r.height * 0.3), 110, H - 60) : clamp(t.y + 30, 90, H - 60);
        const L = Math.abs(ex - t.x);
        const path = `M${f1(t.x)} ${f1(t.y)} C${f1(t.x + (d[0] ? d[0] * Math.min(140, L * 0.5) : sx * 10))} ${f1(t.y + d[1] * 72 + (d[1] ? 0 : 18))} ${f1(ex - sx * L * 0.45)} ${f1(ey + 34)} ${f1(ex)} ${f1(ey)}`;
        back.push(cab(path) + plugSVG(P.x, P.y, Math.atan2(d[1], d[0]), ps));
        lab(k, side, ey - 12, name);
      } else if (j.edge === "front") {
        // end-on into a front-panel jack: the cable hangs from the plug and runs off to the side
        const top = { x: P.x, y: P.y };   // the cable starts under the plug's face
        const ey = Math.max(low, clamp(top.y + 110, 90, H - 14));
        const path = `M${f1(top.x)} ${f1(top.y)} C${f1(top.x)} ${f1(top.y + (ey - top.y) * 0.8)} ${f1(ex - sx * Math.abs(ex - top.x) * 0.55)} ${f1(ey + 10)} ${f1(ex)} ${f1(ey)}`;
        front.push(cab(path) + `<g transform="translate(${f1(P.x)} ${f1(P.y)}) scale(${f1(ps)})"><circle r="5.4" fill="#1a1b1e"/><circle r="4.1" fill="#c4c7cc"/><circle r="2.5" fill="#2a2b2f"/><circle cx="-1.2" cy="-1.3" r="1" fill="rgba(255,255,255,.7)"/></g>`);
        lab(k, side, ey - 12, name);
      } else {
        // rear jacks: the cable comes out from behind the unit
        const y = clamp(r.top + j.fy * r.height, r.top + 20, r.bottom - 20);
        const x0 = side === "right" ? r.right - 40 : r.left + 40;
        const ey = roomy(side) ? clamp(y + 40, 90, H - 60) : low;
        const path = `M${f1(x0)} ${f1(y)} C${f1(x0 + sx * 120)} ${f1(y + 6)} ${f1(ex - sx * Math.abs(ex - x0) * 0.4)} ${f1(ey + 30)} ${f1(ex)} ${f1(ey)}`;
        back.push(cab(path));
        lab(k, side, ey - 12, name);
      }
    });
    const ox = -(c.left + card.clientLeft), oy = -(c.top + card.clientTop);
    const lay = (cls, body, extra = "") => `<div class="sr-ccab ${cls}" style="left:${f1(ox)}px;top:${f1(oy)}px;width:${W}px;height:${H}px">${body ? `<svg width="${W}" height="${H}" viewBox="0 0 ${W} ${H}"><g class="sr-cg">${body}</g></svg>` : ""}${extra}</div>`;
    card.insertAdjacentHTML("afterbegin", lay("back", back.join("")));
    card.insertAdjacentHTML("beforeend", lay("front", front.join(""), labels.join("")));
  }
  function closeFocus(instant) {
    const f = R.focus;
    if (!f) return;
    R.focus = null;
    const host = f.host, card = f.card, dim = host.querySelector(".sr-dim");
    const src = focusSrc(f) || (document.contains(f.src) ? f.src : null);
    const done = () => {
      card.remove();
      if (src) src.classList.remove("sr-lifted");
      if (f.src && f.src !== src) f.src.classList.remove("sr-lifted");
      if (!R.focus || R.focus.host !== host) {
        host.hidden = true;
        dim.getAnimations().forEach((a) => a.cancel()); dim.style.opacity = "";
        if (f.room) R.wrap.classList.remove("sr-focused"); else document.body.classList.remove("sr-chain-focused");
      }
      indexControls();
      if (typeof indexPianos === "function") indexPianos();
      hideTipSoon(0);
      if (f.room && R.visible) R.lvlShown = -1;
    };
    if (instant || reduced() || !src || !document.contains(card)) {
      if (!src && !instant && !reduced()) { card.animate([{ opacity: 1 }, { opacity: 0, transform: "scale(.96)" }], { duration: 200, fill: "forwards" }).onfinish = done; dim.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 220, fill: "forwards" }); return; }
      done(); return;
    }
    const fx = card.querySelector(".sr-cface, .sr-split-card, .sr-mk");
    const a = src.getBoundingClientRect(), b = fx.getBoundingClientRect(), c = card.getBoundingClientRect();
    const s = clamp(a.width / Math.max(1, b.width), 0.05, 1.5);
    card.style.transformOrigin = `${b.left + b.width / 2 - c.left}px ${b.top + b.height / 2 - c.top}px`;
    const dx = a.left + a.width / 2 - (b.left + b.width / 2), dy = a.top + a.height / 2 - (b.top + b.height / 2);
    const tilt = src.closest(".sr-board") ? TILT : 0;
    // only the unit flies home: the card's panel, shadow, header, drawer and footer go at once
    card.classList.add("sr-closing");
    card.querySelectorAll(".sr-ch, .sr-cmore, .sr-cft, .sr-mk-side, .sr-ccab").forEach((el) => el.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 90, fill: "forwards" }));
    card.style.pointerEvents = "none";
    dim.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 380, easing: "ease-in", fill: "forwards" });
    card.animate([
      { transform: "translate(0px, 0px) scale(1) perspective(900px) rotateX(0deg)", opacity: 1 },
      { transform: `translate(${dx}px, ${dy}px) scale(${s}) perspective(900px) rotateX(${tilt}deg)`, opacity: f.mode === "mic" ? 0 : 1 },
    ], { duration: 420, easing: "cubic-bezier(.45,.05,.3,1)", fill: "forwards" }).onfinish = done;
    if (host.contains(document.activeElement)) { if (f.room) R.root.focus({ preventScroll: true }); else document.activeElement.blur(); }
  }

  /* ---------- clicks inside the card (mirrors the chain's own click handling) ---------- */
  function cardClick(e) {
    const f = R.focus;
    if (!f) return;
    const zb = e.target.closest("[data-sr-zoom]");
    if (zb) { const d = +zb.dataset.srZoom; zoomBy(d === 0 ? 0 : d > 0 ? 1.5 : 1 / 1.5); return; }
    const sa = e.target.closest("[data-sr-act]");
    if (sa) {
      const a = sa.dataset.srAct;
      if (a === "close") closeFocus();
      else if (a === "bypass" && f.slot != null) toggleBypass(f.slot);
      else if (a === "details" && window.ComponentDetail) { const s = getSlot(f.slot); closeFocus(true); if (s) ComponentDetail.open(s.table_id); }
      else if (a === "chain") { const slot = f.slot; closeFocus(true); HRViews.show("chain"); selectUnit(slot, true); }
      else if (a === "room") { const slot = f.slot; closeFocus(true); HRViews.show("room"); setTimeout(() => { if (R.visible) openFocus({ slot }); }, 380); }
      else if (a === "mic") { const slot = f.slot; f.mode = "mic"; f.moreOff = false; fillCard(f); paintMic(f); indexControls(); void slot; }
      else if (a === "face") { f.mode = ""; fillCard(f); fitCard(f); indexControls(); refreshDisplays(f.slot); }
      else if (a === "mic-convert") convertCab(f);
      return;
    }
    if (f.mode === "mic") { micClick(e, f); return; }
    if (typeof ui !== "undefined" && ui.suppressClick) return;
    const nb = e.target.closest("[data-nam]");
    if (nb) { namBridge(nb.dataset.nam === "import" ? "importNam" : "revealNamFolder"); return; }
    const psb = e.target.closest("[data-ps]");
    if (psb) { const u = psb.closest("[data-unit]"); if (u) synthPreset(+u.dataset.unit, psb.dataset.ps, psb); return; }
    const act = e.target.closest("[data-act]");
    const unit = e.target.closest("[data-unit]");
    if (act && (unit || f.slot != null)) {
      const idx = unit ? +unit.dataset.unit : f.slot;
      if (act.dataset.act === "bypass") toggleBypass(idx);
      else if (act.dataset.act === "more") { f.moreOff = !f.moreOff; const m = f.card.querySelector(".sr-cmore"); if (m) { m.classList.toggle("off", f.moreOff); fitCard(f); } }
      else if (act.dataset.act === "edit") openSheet(idx);
      return;
    }
    const ctl = e.target.closest(".ctl");
    if (!ctl) return;
    const k = ctl.dataset.kind;
    const step = (b) => { const p = modelOf(ctl); if (p) { setValue(ctl, clamp01(p.norm + (+b.dataset.d) / Math.max(1, stepCount(p) - 1))); hideTipSoon(700); scheduleRackRefresh(300); } };
    if (k === "tog") cycleTog(ctl);
    else if (k === "btnrow") { const b = e.target.closest("button[data-i]"); if (b) { setValue(ctl, +b.dataset.i / Math.max(1, +ctl.dataset.steps - 1)); hideTipSoon(700); scheduleRackRefresh(300); } }
    else if (k === "sel") { const b = e.target.closest("button[data-d]"); if (b) step(b); else if (e.target.closest(".sel-v")) selMenu(ctl); }
    else if (k === "sound") { const b = e.target.closest("button[data-d]"); if (b) step(b); else soundBrowser(ctl); }
    else if (k === "capture") { const b = e.target.closest("button[data-d]"); if (b) step(b); else soundBrowser(ctl, { nam: true }); }
  }

  /* =====================================================================
     CAB & MICS — the one place to choose a cabinet and mic it. Opened from the Room (the mic stand, a cab's card),
     from the Signal Chain (the Cabinet faceplate's grille or its "Cab & mics" button) and from a classic cab unit
     (one click makes it mikeable). Front view: the cabinet, and its speaker with mic A and mic B on it (drag a mic
     onto Cap / Cap edge / Cone, or use the position buttons); side view: how far mic B sits behind A; the mic locker
     for the selected mic. Engine: /api/cabinets (catalogue), /api/cabinet (cab + mics + positions), /api/param
     (blend, alignB, phaseB, room). Positions are the ones each mic was recorded at: a mic dropped between rings
     lands on the nearest one, and stays there.
     ===================================================================== */
  const POS_R = { cap: 0, capedge: 0.44, cone: 0.8, fixed: 0.3, room: 1.25 };
  const POS_SHORT = { cap: "Cap", capedge: "Cap edge", cone: "Cone · off-axis", room: "Room", fixed: "Fixed" };
  const cabMics = () => R.cabs && R.cabs.cabinets ? R.cabs : null;
  function loadCabs() {
    if (R.cabs) return Promise.resolve(R.cabs);
    if (R.cabsLoading) return R.cabsLoading;
    R.cabsErr = "";
    R.cabsLoading = fetchJSON("/api/cabinets").then((d) => {
      if (!d || !Array.isArray(d.cabinets)) throw new Error("no catalogue");
      R.cabs = d;
      return d;
    }).catch((e) => { R.cabsErr = "The engine didn’t send its cabinet list."; void e; return null; })
      .finally(() => { R.cabsLoading = null; if (R.focus && R.focus.mode === "mic") refreshMic(R.focus); });
    return R.cabsLoading;
  }
  function cabInfo(s) {
    const cb = s && s.cabinet, all = cabMics();
    return cb && all ? all.cabinets.find((c) => c.id === cb.cab_id) || null : null;
  }
  const micShort = (m) => String((m && m.label) || "").replace(/^(Shure|Sennheiser|Neumann|Royer|Beyerdynamic|Earthworks|Audix|Electro-Voice|AKG)\s+/, "").replace(/\s*\((Close|Matched)[^)]*\)\s*$/, "");
  const refreshMic = (f) => { if (R.focus !== f || !f.card) return; fillCard(f); paintMic(f); indexControls(); };
  // a studio cabinet with the same speakers (for a capture, which has no matched/studio pair)
  function sameSpeakerStudio(cb) {
    const all = cabMics();
    if (!all || !cb) return null;
    if (cb.pair) return all.cabinets.find((c) => c.id === cb.pair) || null;
    return all.cabinets.find((c) => c.kind === "studio" && c.speakers && c.speakers === cb.speakers) || null;
  }
  // the closest mic on another cabinet: same model, then same kind of mic, then same family
  function closestMic(info, m) {
    if (!info || !info.mics || !info.mics.length) return null;
    if (!m) return null;
    return info.mics.find((x) => x.id === m.id) || info.mics.find((x) => micKind(x) === micKind(m)) || info.mics.find((x) => x.family && x.family === m.family) || null;
  }
  const posOf = (mi, want) => { const ps = (mi && mi.positions || []).map((p) => p.id); return ps.includes(want) ? want : ps.includes("capedge") ? "capedge" : ps[0] || ""; };
  // the speaker grid drawn for a cabinet ("4x12" → 2 × 2, "8x10" → 4 × 2 …)
  function cabGrid(speakers) {
    const m = /^(\d+)x/.exec(speakers || "") || [0, 1];
    const n = Math.max(1, +m[1]);
    const cols = n >= 8 ? 4 : n >= 2 ? 2 : 1, rows = Math.ceil(n / cols);
    return { n, cols, rows };
  }
  function speakerSVG() {
    let ribs = "";
    for (let i = 0; i < 24; i++) { const a = i * 15 * Math.PI / 180; ribs += `<path d="M${(150 + Math.cos(a) * 46).toFixed(1)} ${(150 + Math.sin(a) * 46).toFixed(1)} L${(150 + Math.cos(a) * 112).toFixed(1)} ${(150 + Math.sin(a) * 112).toFixed(1)}" stroke="rgba(0,0,0,.18)" stroke-width="1"/>`; }
    return `<svg class="sr-mk-spk" viewBox="0 0 300 300" aria-hidden="true">
      <defs><radialGradient id="srmk-cone" cx="50%" cy="46%" r="60%"><stop offset="0" stop-color="#4a4038"/><stop offset=".55" stop-color="#2c2620"/><stop offset="1" stop-color="#16120f"/></radialGradient>
        <radialGradient id="srmk-cap" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="#6d6258"/><stop offset=".6" stop-color="#2d2620"/><stop offset="1" stop-color="#16120f"/></radialGradient>
        <radialGradient id="srmk-sur" cx="50%" cy="50%" r="50%"><stop offset=".86" stop-color="#191614"/><stop offset=".93" stop-color="#3a3430"/><stop offset="1" stop-color="#0d0b0a"/></radialGradient></defs>
      <circle cx="150" cy="150" r="146" fill="#0d0e10"/><circle cx="150" cy="150" r="141" fill="none" stroke="#3a3c40" stroke-width="3"/>
      ${[0, 1, 2, 3, 4, 5, 6, 7].map((i) => { const a = (i * 45 + 22.5) * Math.PI / 180; return `<circle cx="${(150 + Math.cos(a) * 136).toFixed(1)}" cy="${(150 + Math.sin(a) * 136).toFixed(1)}" r="3.4" fill="#8b8e93"/>`; }).join("")}
      <circle cx="150" cy="150" r="130" fill="url(#srmk-sur)"/><circle cx="150" cy="150" r="114" fill="url(#srmk-cone)"/>${ribs}
      <circle cx="150" cy="150" r="114" fill="none" stroke="rgba(255,255,255,.06)" stroke-width="1.2"/>
      <circle cx="150" cy="150" r="46" fill="url(#srmk-cap)"/><ellipse cx="136" cy="134" rx="16" ry="10" fill="rgba(255,255,255,.08)"/>
      <circle class="zone" cx="150" cy="150" r="${(0.44 * 114).toFixed(1)}"/><circle class="zone" cx="150" cy="150" r="${(0.8 * 114).toFixed(1)}"/>
    </svg>`;
  }
  function cabFrontHTML(s, cb) {
    const g = cabGrid(cb && cb.speakers);
    const st = R.focus && R.focus.station, ampD = st && st.amp ? describe(st.amp) : null;
    const tolex = ampD && ampD.form === "amp" ? ampD.tolex : "#1d1d1f";
    const big = /15/.test((cb && cb.speakers) || "") ? " s15" : /10/.test((cb && cb.speakers) || "") ? " s10" : "";
    let spk = "";
    for (let i = 0; i < g.n; i++) spk += `<i class="${i === 0 ? "miked" : ""}"></i>`;
    return `<div class="sr-mk-cab${big}" style="--cols:${g.cols};--rows:${g.rows};--tolex:${tolex}" title="The speaker the mics are on is lit">
      <div class="sr-mk-cabgrid">${spk}</div><span class="sr-mk-cabname">${esc(cb ? (String(cb.label || "").startsWith(cb.speakers || "\u0000") ? cb.label : `${cb.speakers || ""} · ${cb.label || ""}`) : (s && s.reference_name) || "")}</span></div>`;
  }
  function puckHTML(which, m, fixed) {
    if (!m) return "";
    const k = micKind(m);
    return `<span class="sr-mk-puck ${which.toLowerCase()} k-${k}${fixed ? " fixed" : ""}" data-puck="${which}" title="Mic ${which}: ${esc(m.label)}${fixed ? " (fixed by the capture)" : " — drag it across the speaker"}"><i></i><b>${which}</b><em>${esc(micShort(m))}</em></span>`;
  }
  function micCardHTML(s) {
    if (!R.cabs) loadCabs();
    if (!s) return `<div class="sr-mk"></div>`;
    const cb = s.cabinet;
    const isCab = window.HRCabinet && HRCabinet.isCabinet(s);
    // a classic cab unit (fixed captures): one honest line and one click to a mikeable cabinet with the same speakers
    if (!isCab) {
      return `<div class="sr-mk sr-mk-msg"><div class="sr-mk-front">${speakerSVG()}</div><div class="sr-mk-side">
        <div class="sr-mk-note"><b>${esc(s.reference_name)}</b> plays captures with their mics fixed — they can’t be moved or swapped on this unit.</div>
        <button class="btn primary" data-sr-act="mic-convert">Make it mikeable — same speakers</button>
        <small class="sr-mk-hint">Swaps in the Cabinet unit (it starts out sounding the same), then puts real mics on the same speakers.</small></div></div>`;
    }
    const info = cabInfo(s);
    const fixed = !cb || cb.kind === "matched" || cb.kind === "capture" || cb.status !== "ok";
    const tab = R.micTab === "B" && cb && cb.mic_b ? "B" : "A";
    const cur = tab === "A" ? cb && cb.mic_a : cb && cb.mic_b;
    const mics = info ? info.mics : [];
    const twin = fixed ? sameSpeakerStudio(cb) : null;
    let locker;
    if (fixed) {
      const why = !cb ? "" : cb.status === "missing" ? "This cabinet isn’t installed on this machine." : cb.status === "no_content" || cb.approximation ? "No cabinet recordings are installed, so this is an approximation." : cb.kind === "matched" ? `An amp-matched capture: its two close mics (${esc(micShort(cb.mic_a))}${cb.mic_b ? ` + ${esc(micShort(cb.mic_b))}` : ""}) are fixed by the recording. You can still blend them.` : "An IR capture: its mic is baked into the file.";
      locker = `<div class="sr-mk-note">${why}</div>${twin ? `<button class="btn primary" data-mk-twin="${esc(twin.id)}">Switch to ${esc(twin.label)} — same speakers, mics you can move</button>` : ""}`;
    } else if (mics.length) {
      locker = `<div class="sr-mk-locker">${mics.map((m) => `<button class="sr-mk-mic${cur && cur.id === m.id ? " on" : ""}" data-mic="${esc(m.id)}" title="${esc(m.note || m.label)}">${micSVG(micKind(m))}<span>${esc(micShort(m))}</span><small>${esc(m.family || "")}</small></button>`).join("")}</div>`;
    } else {
      locker = `<div class="sr-mk-locker"><div class="sr-mk-wait">${R.cabsErr ? `${esc(R.cabsErr)} <button class="text-btn" data-mk-retry>Try again</button>` : "Opening the mic locker…"}</div></div>`;
    }
    const posBtns = !fixed && cur ? (() => {
      const mi = mics.find((x) => x.id === cur.id);
      const ps = mi ? mi.positions || [] : [];
      return ps.length > 1 ? `<div class="sr-mk-pos" role="group" aria-label="Mic ${tab} position">${ps.map((p) => `<button class="${p.id === cur.position ? "on" : ""}" data-mk-pos="${esc(p.id)}">${esc(POS_SHORT[p.id] || p.label)}</button>`).join("")}</div>`
        : `<div class="sr-mk-pos one">${esc(cur.position_label || "")} — the only position this mic was recorded at</div>`;
    })() : "";
    const blend = PP(s, "blend"), room = PP(s, "room"), phase = PP(s, "phaseB");
    const knob = (p, label) => (p ? ctlHTML(s, p, { knob: "alu", size: 34, scale: "arc", label }) : "");
    const chip = (L, m) => `<button role="tab" class="sr-mk-tab ${L.toLowerCase()}${tab === L ? " on" : ""}" data-mk-tab="${L}"><b>${L}</b><span>${m ? esc(micShort(m)) : ""}</span><em>${m ? esc(POS_SHORT[m.position] || m.position_label || "") : ""}</em></button>`;
    const two = !!(cb && cb.mic_b);
    return `<div class="sr-mk${fixed ? " sr-mk-fixed" : ""}" data-slot="${s.slot_idx}">
      <div class="sr-mk-front">${cabFrontHTML(s, cb)}
        <div class="sr-mk-spkwrap">${speakerSVG()}<span class="sr-mk-lbl cap">Cap</span><span class="sr-mk-lbl edge">Cap edge</span><span class="sr-mk-lbl cone">Cone</span>
          ${puckHTML("A", cb && cb.mic_a, fixed)}${puckHTML("B", cb && cb.mic_b, fixed)}</div>
        ${posBtns}</div>
      <div class="sr-mk-side">
        <div class="sr-mk-row"><span class="sr-mk-k">Cabinet</span><button class="sr-mk-cabbtn" data-mk-cabs aria-expanded="${R.cabOpen ? "true" : "false"}"><span>${esc(cb ? cb.label || "" : "")}</span><small>${esc(cb ? [cb.speakers, cb.kind === "studio" ? "mikeable" : cb.kind === "matched" ? "amp-matched" : cb.kind === "capture" ? "IR capture" : ""].filter(Boolean).join(" · ") : "")}</small><i>▾</i></button></div>
        ${R.cabOpen ? cabListHTML(s) : ""}
        <div class="sr-mk-row"><span class="sr-mk-k">Mics</span><div class="sr-mk-tabs" role="tablist">${chip("A", cb && cb.mic_a)}${two ? chip("B", cb.mic_b) : ""}
          ${fixed ? "" : two ? `<button class="sr-mk-x" data-mk-off="B" title="Take mic B down">Remove B</button>` : `<button class="sr-mk-add" data-mk-add="B">+ Add mic B</button>`}</div></div>
        ${locker}
        ${two && !fixed ? `<div class="sr-mk-sv" data-mk-sv title="Mic B distance behind mic A — the time of flight gives real phase and comb effects"><svg viewBox="0 0 340 92" aria-hidden="true"><rect x="8" y="10" width="16" height="72" rx="3" fill="#232427"/><path d="M24 18 C 46 30, 46 62, 24 74" fill="#2c2620" stroke="#3a3430"/><rect x="2" y="6" width="8" height="80" rx="2" fill="#18191b"/>
          <path class="ruler" d="M40 86 H330"/>${[0, 10, 20, 30, 40, 50].map((cm) => `<path class="tick" d="M${(40 + cm * 5.6).toFixed(1)} 82 V90"/><text x="${(40 + cm * 5.6).toFixed(1)}" y="78">${cm}</text>`).join("")}
          <text x="330" y="78" text-anchor="end">cm</text></svg>
          <span class="sr-mk-sm a"><svg viewBox="0 0 56 22">${MIC_ART[micKind(cb.mic_a)] || MIC_ART.sm57}</svg><b>A</b></span>
          <span class="sr-mk-sm b" data-sm="B" title="Drag mic B back from the grille"><svg viewBox="0 0 56 22">${MIC_ART[micKind(cb.mic_b)] || MIC_ART.sm57}</svg><b>B</b></span></div>` : ""}
        <div class="sr-mk-knobs">${two ? knob(blend, "A ↔ B") : ""}${two && !fixed && phase ? ctlHTML(s, phase, { label: "B polarity" }) : ""}${cb && cb.room_available ? knob(room, "Room mic") : ""}</div>
        <div class="sr-mk-read" data-mk-read></div>
      </div>
    </div>`;
  }
  function cabListHTML(s) {
    const all = cabMics(), cb = s.cabinet || {};
    if (!all) return `<div class="sr-mk-cabs"><div class="sr-mk-wait">${R.cabsErr ? esc(R.cabsErr) : "Opening the cabinet list…"}</div></div>`;
    const q = (R.cabQ || "").trim().toLowerCase();
    const groups = [["studio", "Studio cabinets — mics you can move"], ["matched", "Amp-matched — mics fixed by the capture"], ["capture", "IR captures — mic baked in"]];
    const rows = groups.map(([k, title]) => {
      const list = all.cabinets.filter((c) => c.kind === k && (!q || `${c.label} ${c.group} ${c.speakers}`.toLowerCase().includes(q)));
      if (!list.length) return "";
      return `<div class="sr-mk-cg">${esc(title)}</div>${list.map((c) => `<button class="sr-mk-ci${c.id === cb.cab_id ? " on" : ""}" data-mk-cab="${esc(c.id)}"><span>${esc(c.label)}</span><small>${esc([c.speakers, k === "capture" ? c.group : ""].filter(Boolean).join(" · "))}</small></button>`).join("")}`;
    }).join("");
    return `<div class="sr-mk-cabs"><input class="sr-mk-q" data-mk-q type="search" placeholder="Find a cabinet…" value="${esc(R.cabQ || "")}" spellcheck="false"><div class="sr-mk-cl">${rows || `<div class="sr-mk-wait">No cabinet matches.</div>`}</div></div>`;
  }
  /* Place the pucks and the side-view mic from the current selection (the angle a mic was dropped at is kept). */
  function paintMic(f) {
    const card = f.card, s = getSlot(f.slot);
    if (!card || !s || !s.cabinet) return;
    const cb = s.cabinet;
    R.micAng = R.micAng || {};
    const put = (which, m, ang0) => {
      const el = card.querySelector(`.sr-mk-puck[data-puck="${which}"]`);
      if (!el || !m) return;
      const r = POS_R[m.position] != null ? POS_R[m.position] : 0.3;
      const shared = cb.mic_a && cb.mic_b && cb.mic_a.position === cb.mic_b.position;   // two mics on one ring sit apart
      const ang = R.micAng[`${s.slot_idx}${which}`] != null ? R.micAng[`${s.slot_idx}${which}`] : ang0;
      const rr = shared && r < 0.17 ? 0.17 : r;
      el.style.setProperty("--px", (Math.cos(ang) * rr * 0.76).toFixed(3));
      el.style.setProperty("--py", (Math.sin(ang) * rr * 0.76).toFixed(3));
      el.dataset.pos = m.position || "";
    };
    put("A", cb.mic_a, -38 * Math.PI / 180); put("B", cb.mic_b, 148 * Math.PI / 180);
    const al = PP(s, "alignB"), sb = card.querySelector(".sr-mk-sm.b");
    if (sb) sb.style.setProperty("--d", (al ? al.norm : 0).toFixed(3));
    const rd = card.querySelector("[data-mk-read]");
    if (rd) rd.innerHTML = al && cb.mic_b && cb.kind === "studio" ? `<small>Mic B sits ${Math.round((al.norm || 0) * 51)} cm behind A${switchOn(PP(s, "phaseB")) ? " · polarity flipped" : ""}</small>` : "";
  }
  /* One engine call for a cabinet / mic / position change; the rack it returns redraws the panel. */
  async function sendCab(s, sel) {
    const cb = s.cabinet || {};
    const q = { cab: cb.cab_id || "", mic_a: (cb.mic_a || {}).id || "", pos_a: (cb.mic_a || {}).position || "", mic_b: cb.mic_b ? cb.mic_b.id : "", pos_b: cb.mic_b ? cb.mic_b.position : "", ...sel };
    // optimistic: the drawing moves now, the engine confirms with the rack
    const all = cabMics(), info = all && all.cabinets.find((c) => c.id === q.cab);
    const mk = (id, pos) => { if (!id || !info) return null; const m = info.mics.find((x) => x.id === id); const p = m && ((m.positions || []).find((x) => x.id === pos) || (m.positions || [])[0]); return m ? { id: m.id, label: m.label, family: m.family, note: m.note, position: p ? p.id : pos, position_label: p ? p.label : pos } : null; };
    if (info && q.cab === cb.cab_id) s.cabinet = { ...cb, mic_a: mk(q.mic_a, q.pos_a) || cb.mic_a, mic_b: q.mic_b ? mk(q.mic_b, q.pos_b) : null };
    if (R.focus && R.focus.mode === "mic" && R.focus.slot === s.slot_idx) paintMic(R.focus);
    markDirty();
    const d = await rackCall(`/api/cabinet?slot=${s.slot_idx}&cab=${encodeURIComponent(q.cab)}&mic_a=${encodeURIComponent(q.mic_a)}&pos_a=${encodeURIComponent(q.pos_a)}&mic_b=${encodeURIComponent(q.mic_b)}&pos_b=${encodeURIComponent(q.pos_b)}`, "Mic placement needs the updated engine.");
    if (R.focus && R.focus.mode === "mic" && R.focus.slot === s.slot_idx) refreshMic(R.focus);
    return d;
  }
  // Switch the cabinet, keeping the mics as close as the new cabinet allows.
  function switchCab(s, id) {
    const all = cabMics(), info = all && all.cabinets.find((c) => c.id === id), cb = s.cabinet || {};
    R.cabOpen = false; R.cabQ = "";
    if (!info) return sendCab(s, { cab: id, mic_a: "", pos_a: "", mic_b: "", pos_b: "" });
    if (info.kind !== "studio") {   // fixed mics: whatever the capture has (an amp-matched cab: both of its close mics)
      const [a, b] = info.mics;
      return sendCab(s, { cab: id, mic_a: a ? a.id : "", pos_a: a ? posOf(a, "fixed") : "", mic_b: b ? b.id : "", pos_b: b ? posOf(b, "fixed") : "" });
    }
    const a = closestMic(info, cb.mic_a) || info.mics[0], b = cb.mic_b ? closestMic(info, cb.mic_b) : null;
    const pa = posOf(a, cb.mic_a && cb.mic_a.position !== "fixed" ? cb.mic_a.position : "capedge");
    return sendCab(s, { cab: id, mic_a: a ? a.id : "", pos_a: pa, mic_b: b ? b.id : "", pos_b: b ? posOf(b, cb.mic_b.position !== "fixed" ? cb.mic_b.position : "capedge") : "" });
  }
  function micClick(e, f) {
    const s = getSlot(f.slot);
    if (!s) return;
    if (e.target.closest("[data-mk-retry]")) { R.cabsErr = ""; loadCabs(); refreshMic(f); return; }
    if (e.target.closest("[data-mk-cabs]")) { R.cabOpen = !R.cabOpen; if (R.cabOpen) loadCabs(); refreshMic(f); if (R.cabOpen) { const q = f.card.querySelector("[data-mk-q]"); if (q) q.focus(); } return; }
    const ci = e.target.closest("[data-mk-cab]");
    if (ci) { switchCab(s, ci.dataset.mkCab); return; }
    const tw = e.target.closest("[data-mk-twin]");
    if (tw) { switchCab(s, tw.dataset.mkTwin); return; }
    const tb = e.target.closest("[data-mk-tab]") || e.target.closest(".sr-mk-puck[data-puck]");
    if (tb) { R.micTab = tb.dataset.mkTab || tb.dataset.puck; refreshMic(f); return; }
    const pb = e.target.closest("[data-mk-pos]");
    if (pb) {
      const tab = R.micTab === "B" && s.cabinet.mic_b ? "B" : "A";
      sendCab(s, tab === "A" ? { pos_a: pb.dataset.mkPos } : { pos_b: pb.dataset.mkPos });
      return;
    }
    if (e.target.closest("[data-mk-add]")) {
      const all = cabInfo(s), a = s.cabinet && s.cabinet.mic_a;
      const m = all && (all.mics.find((x) => a && x.family !== a.family) || all.mics.find((x) => !a || x.id !== a.id) || all.mics[0]);
      if (!m) { loadCabs(); return; }
      R.micTab = "B";
      const ps = (m.positions || []).map((p) => p.id), pb2 = ps.find((p) => p !== (a && a.position)) || ps[0] || "";
      sendCab(s, { mic_b: m.id, pos_b: pb2 });
      return;
    }
    if (e.target.closest("[data-mk-off]")) { R.micTab = "A"; sendCab(s, { mic_b: "", pos_b: "" }); return; }
    const mb = e.target.closest("[data-mic]");
    if (mb) {
      const tab = R.micTab === "B" && s.cabinet.mic_b ? "B" : "A";
      const all = cabInfo(s), m = all && all.mics.find((x) => x.id === mb.dataset.mic);
      if (!m) return;
      const cur = tab === "A" ? s.cabinet.mic_a : s.cabinet.mic_b;
      const keep = posOf(m, cur && cur.position);
      f.card.querySelectorAll(".sr-mk-mic").forEach((b) => b.classList.toggle("on", b === mb));
      sendCab(s, tab === "A" ? { mic_a: m.id, pos_a: keep } : { mic_b: m.id, pos_b: keep });
      return;
    }
    const ctl = e.target.closest(".ctl");
    if (ctl && ctl.dataset.kind === "tog") cycleTog(ctl);
  }
  function micInput(e, f) {   // the cabinet search field
    if (!e.target.matches("[data-mk-q]")) return;
    R.cabQ = e.target.value;
    const s = getSlot(f.slot), box = f.card.querySelector(".sr-mk-cabs");
    if (!s || !box) return;
    const pos = e.target.selectionStart;
    box.outerHTML = cabListHTML(s);
    const q = f.card.querySelector("[data-mk-q]");
    if (q) { q.focus(); q.setSelectionRange(pos, pos); }
  }
  /* Dragging a mic: it follows the pointer, and on release lands on the nearest position that mic was recorded at
     (keeping the angle it was dropped at). The side-view mic B slides back from the grille (alignB). */
  function micPointerDown(e, f) {
    const puck = e.target.closest(".sr-mk-puck[data-puck]"), sm = e.target.closest(".sr-mk-sm[data-sm]");
    if (!puck && !sm) return false;
    const s = getSlot(f.slot);
    if (!s || !s.cabinet) return false;
    const card = f.card;
    if (sm) {
      e.preventDefault();
      const sv = card.querySelector("[data-mk-sv]").getBoundingClientRect();
      const x0 = sv.left + sv.width * (40 / 340), x1 = sv.left + sv.width * (320 / 340);
      sm.classList.add("drag");
      const p = PP(s, "alignB");
      const mv = (ev) => {
        const n = clamp((ev.clientX - x0) / (x1 - x0), 0, 1);
        sm.style.setProperty("--d", n.toFixed(3));
        if (p) { p.norm = n; queueSend(`p:${s.slot_idx}:alignB`, `/api/param?slot=${s.slot_idx}&id=alignB&norm=${n.toFixed(4)}`); }
        paintMic(f);
      };
      const up = () => { sm.classList.remove("drag"); window.removeEventListener("pointermove", mv); window.removeEventListener("pointerup", up); markDirty(); scheduleRackRefresh(300); };
      window.addEventListener("pointermove", mv); window.addEventListener("pointerup", up);
      return true;
    }
    const which = puck.dataset.puck;
    if (puck.classList.contains("fixed")) { puck.classList.remove("nope"); void puck.offsetWidth; puck.classList.add("nope"); return true; }
    e.preventDefault();
    const m = which === "A" ? s.cabinet.mic_a : s.cabinet.mic_b;
    const all = cabInfo(s), mm = all && m && all.mics.find((x) => x.id === m.id);
    const pos = mm ? (mm.positions || []).map((p) => p.id) : [];
    const wrap = card.querySelector(".sr-mk-spkwrap"), box = wrap.getBoundingClientRect();
    const cx = box.left + box.width / 2, cy = box.top + box.height / 2, rad = box.width / 2 * 0.76;
    puck.classList.add("drag");
    R.micTab = which;
    card.querySelectorAll("[data-mk-tab]").forEach((b) => b.classList.toggle("on", b.dataset.mkTab === which));
    let snapId = m && m.position, ang = null;
    const zones = card.querySelectorAll(".sr-mk-spk .zone");
    const mv = (ev) => {
      let dx = (ev.clientX - cx) / rad, dy = (ev.clientY - cy) / rad;
      const r = Math.hypot(dx, dy);
      if (r > 1.25) { dx *= 1.25 / r; dy *= 1.25 / r; }
      puck.style.setProperty("--px", (dx * 0.76).toFixed(3));
      puck.style.setProperty("--py", (dy * 0.76).toFixed(3));
      if (r > 0.05) ang = Math.atan2(dy, dx);
      let best = null, bd = 9;
      pos.forEach((p) => { const pr = POS_R[p] != null ? POS_R[p] : 0.3; const d = Math.abs(pr - Math.min(1.25, r)); if (d < bd) { bd = d; best = p; } });
      if (best) { snapId = best; puck.dataset.snap = POS_SHORT[best] || best; }
      zones.forEach((z, i) => z.classList.toggle("hot", (i === 0 && snapId === "capedge") || (i === 1 && snapId === "cone")));
      wrap.dataset.hot = snapId || "";
    };
    const up = () => {
      window.removeEventListener("pointermove", mv); window.removeEventListener("pointerup", up);
      puck.classList.remove("drag");
      zones.forEach((z) => z.classList.remove("hot"));
      wrap.dataset.hot = "";
      if (ang != null) { R.micAng = R.micAng || {}; R.micAng[`${s.slot_idx}${which}`] = ang; }
      if (snapId && m && snapId !== m.position) sendCab(s, which === "A" ? { pos_a: snapId } : { pos_b: snapId });
      else refreshMic(f);
    };
    window.addEventListener("pointermove", mv); window.addEventListener("pointerup", up);
    return true;
  }
  /* A classic cab unit → the Cabinet unit (sample-exact for a matched cab), then straight onto the studio recording of
     the same speakers with the matching mics (a 57 and a 121 at the cap edge, the matched blend kept). One click. */
  // The panel follows a classic cab swapped for the Cabinet unit (the engine inserts it at the same place in the chain;
  // its slot index may be new or reused).
  function adoptSwapped(f) {
    if (!f.keepSlot) return false;
    const fresh = (rackState.slots || []).find((x) => window.HRCabinet && HRCabinet.isCabinet(x) && !f.keepSlot.has(x.slot_idx))
      || (() => { const x = getSlot(f.slot); return x && window.HRCabinet && HRCabinet.isCabinet(x) ? x : null; })();
    if (!fresh) return false;
    f.slot = fresh.slot_idx; f.tid = fresh.table_id;
    if (R.ukeys && !R.ukeys.has(f.slot)) R.ukeys.set(f.slot, `u:${fresh.table_id}#${f.slot}`);
    return true;
  }
  async function convertCab(f) {
    f.keepSlot = new Set((rackState.slots || []).map((x) => x.slot_idx));   // the slots before the swap
    const d = await rackCall(`/api/cabinet/convert?slot=${f.slot}`, "Converting cabinets needs the updated engine.");
    if (d) adoptSwapped(f);
    f.keepSlot = null;
    if (!d) return;
    markDirty();
    await loadCabs();
    const s = getSlot(f.slot), cb = s && s.cabinet;
    if (cb && cb.kind !== "studio") { const tw = sameSpeakerStudio(cb); if (tw) await switchCab(s, tw.id); }
    if (R.focus === f) refreshMic(f);
  }

  /* =====================================================================
     WIRING
     ===================================================================== */
  /* Card events, wherever the card is hosted. */
  // Anything that isn't the unit, the panel, the drawer or the header is background: a click there puts it back.
  const BG_NOT = ".sr-cfit, .sr-cmore, .sr-ch, .sr-mk, .sr-split-card, .sr-zoom, .sr-clab, button, input, a";
  const isBg = (t) => !!(t && t.closest && !t.closest(BG_NOT));
  function wireHost(host) {
    let bg = null;   // a press on the background: a click closes, a drag pans a zoomed-in unit
    host.addEventListener("pointerdown", (e) => {
      bg = null;
      if (e.button === 0 && isBg(e.target) && R.focus) {
        const box = R.focus.card && R.focus.card.querySelector(".sr-cface");
        bg = { x: e.clientX, y: e.clientY, moved: false, box: R.focus.uz > 1 ? box : null, sl: box ? box.scrollLeft : 0, st: box ? box.scrollTop : 0 };
        if (bg.box) {
          const mv = (ev) => { if (!bg) return; const dx = ev.clientX - bg.x, dy = ev.clientY - bg.y; if (Math.hypot(dx, dy) > 4) bg.moved = true; if (bg.moved) { bg.box.scrollLeft = bg.sl - dx; bg.box.scrollTop = bg.st - dy; bg.box.classList.add("sr-panning"); } };
          const up = () => { window.removeEventListener("pointermove", mv); window.removeEventListener("pointerup", up); if (bg && bg.box) bg.box.classList.remove("sr-panning"); };
          window.addEventListener("pointermove", mv); window.addEventListener("pointerup", up);
        }
        return;
      }
      const card = e.target.closest(".sr-card");
      if (!card) return;
      if (R.focus && R.focus.mode === "mic" && micPointerDown(e, R.focus)) return;
      onPointerDown(e);
    });
    host.addEventListener("click", (e) => {
      if (bg && !bg.moved && isBg(e.target)) { bg = null; closeFocus(); return; }
      if (bg && bg.moved) { bg = null; return; }
      if (e.target.closest(".sr-card")) cardClick(e);
    });
    host.addEventListener("input", (e) => { if (R.focus && R.focus.mode === "mic") micInput(e, R.focus); });
    host.addEventListener("wheel", (e) => {
      if (e.ctrlKey && R.focus) { e.preventDefault(); zoomBy(Math.exp(-e.deltaY * (e.deltaMode === 1 ? 0.05 : 0.01)), e.clientX, e.clientY); return; }   // trackpad pinch (and ⌃-scroll)
      if (e.target.closest(".sr-card")) onWheel(e);
    }, { passive: false });
    let g0 = 1;   // Safari / WKWebView trackpad pinch
    host.addEventListener("gesturestart", (e) => { e.preventDefault(); g0 = 1; });
    host.addEventListener("gesturechange", (e) => { e.preventDefault(); if (!R.focus) return; zoomBy(e.scale / g0, e.clientX, e.clientY); g0 = e.scale; });
    host.addEventListener("gestureend", (e) => { e.preventDefault(); });
    host.addEventListener("dblclick", (e) => {
      if (!e.target.closest(".sr-card")) return;
      const el = e.target.closest(".ctl");
      if (el && el.dataset.kind !== "tog" && el.dataset.kind !== "engage" && !e.target.closest("button")) resetValue(el);
    });
    host.addEventListener("contextmenu", (e) => {
      if (!e.target.closest(".sr-card")) return;
      const el = e.target.closest(".ctl[data-slot]");
      if (el) { e.preventDefault(); if (String(el.dataset.slot).startsWith("X")) splitCtlMenu(e.clientX, e.clientY, el); else ctlMenu(e.clientX, e.clientY, el); }
    });
  }
  function wire() {
    const root = R.root;
    root.tabIndex = -1;
    wireHost(R.focusEl);
    const hitSel = ".sr-content [data-sr-slot], .sr-content [data-sr-split]";
    root.addEventListener("pointermove", (e) => {
      if (R.drag) { dragMove(e); return; }
      if (R.focus || (typeof drag !== "undefined" && drag)) return;
      const h = e.target.closest && e.target.closest(hitSel);
      if (h !== hoverEl) {
        if (hoverEl) hoverEl.classList.remove("sr-hover");
        hoverEl = h;
        if (h) h.classList.add("sr-hover"); else { hideTag(); hideFoot(); }
        R.footOn = null;
      }
      if (!h) return;
      const isPd = h.classList.contains("sr-pd") && h.dataset.srSlot != null;
      const fr = isPd ? footRect(h) : null;
      const onFoot = !!(fr && inRect(fr, e.clientX, e.clientY));
      if (onFoot !== R.footOn) {
        R.footOn = onFoot;
        if (onFoot) { showFoot(fr); const s = getSlot(+h.dataset.srSlot); showTag(h, s && s.bypassed ? "Stomp to switch it on" : "Stomp to bypass"); }
        else { hideFoot(); showTag(h, isPd ? "Click to pick up · drag to move" : ""); }
      }
    });
    root.addEventListener("pointerleave", () => { if (hoverEl) hoverEl.classList.remove("sr-hover"); hoverEl = null; hideTag(); hideFoot(); });
    root.addEventListener("pointerdown", (e) => {
      if (e.button !== 0 || e.target.closest(".sr-card, .sr-focus")) return;
      if (!e.target.closest(".sr-spaces, .sr-spacebtn")) closeSpaces();
      // a pedal on the board: drag to move / off to remove (a plain click picks it up or stomps)
      const pd = e.target.closest(".sr-content .sr-board .sr-pd[data-sr-slot]");
      if (pd && !R.focus) {
        const g = pedalGhostHTML(pd);
        startDrag(e, { kind: "move", slot: +pd.dataset.srSlot, src: pd, ghostHTML: g.html, gw: g.w, gh: g.h });
        return;
      }
      const it = e.target.closest(".sr-bag-it[data-tid]");
      if (it) {
        const th = it.querySelector(".sr-bag-th");
        const tid = +it.dataset.tid, c = comps.byTid && comps.byTid.get(tid);
        let w = 70, h = 110;
        if (c) { measurePedals([compSlot(c)]); const b = pedalBox({ table_id: tid }, false); w = b.w * (R.scale || 1); h = b.h * (R.scale || 1); }
        startDrag(e, { kind: "new", tid, ghostHTML: th ? th.innerHTML : "", gw: Math.max(40, w), gh: Math.max(60, h), item: it });
        e.preventDefault();
      }
    });
    window.addEventListener("pointermove", (e) => { if (R.drag && !R.root.contains(e.target)) dragMove(e); });
    window.addEventListener("pointerup", (e) => {
      if (!R.drag) return;
      const D = R.drag;
      if (dragEnd(e)) { R.suppress = now(); return; }
      // a clean click on a board pedal: stomp on the footswitch, otherwise pick it up
      if (D.kind === "move" && D.src && document.contains(D.src)) {
        const fr = footRect(D.src);
        if (inRect(fr, e.clientX, e.clientY)) {
          stomp(D.src, D.slot); R.suppress = now();
          const s = getSlot(D.slot);
          if (s) showTag(D.src, s.bypassed ? "Stomp to switch it on" : "Stomp to bypass");
        }
      }
      if (D.kind === "new" && D.item) { addToBoardEnd(D.tid); R.suppress = now(); }
    });
    window.addEventListener("pointercancel", (e) => { if (R.drag) dragEnd(e, true); });
    root.addEventListener("click", (e) => {
      if (R.suppress && now() - R.suppress < 250) { R.suppress = 0; return; }
      if (e.target.closest(".sr-focus")) return;
      const act = e.target.closest("[data-sr-act]");
      if (act) {
        const a = act.dataset.srAct;
        if (a === "spaces") { const p = root.querySelector(".sr-spaces"); if (p.hidden) openSpaces(); else closeSpaces(); }
        else if (a === "bag") { if (root.querySelector(".sr-bag").hidden) openBag(); else closeBag(); }
        return;
      }
      const spc = e.target.closest(".sr-sp-card[data-space]");
      if (spc) { setScene(spc.dataset.space); return; }
      const bc = e.target.closest("[data-bag-cat]");
      if (bc) { R.bagCat = bc.dataset.bagCat; openBag(); return; }
      if (e.target.closest("[data-sr-comps]")) { e.preventDefault(); setLeftTab("components"); return; }
      const nb = e.target.closest("[data-sr-new]");
      if (nb) { newRig(nb.dataset.srNew); return; }
      const h = e.target.closest(hitSel);
      if (!h) {
        if (e.target.closest(".sr-env .sr-wall, .sr-env .sr-floor") && !e.target.closest(".sr-tools") && (root.querySelector(".sr-spaces").hidden)) { /* the room itself */ }
        return;
      }
      if (h.dataset.srSplit != null) openFocus({ split: +h.dataset.srSplit });
      else if (h.dataset.srMic) openFocus({ slot: +h.dataset.srSlot, mode: "mic" });
      else openFocus({ slot: +h.dataset.srSlot });
    });
    root.addEventListener("keydown", (e) => {
      if ((e.key === "Enter" || e.key === " ") && e.target.matches && e.target.matches(hitSel)) { e.preventDefault(); e.stopPropagation(); e.target.click(); }
      if (e.key === "Escape" && R.drag) { dragEnd(e, true); e.stopPropagation(); }
    });
    root.addEventListener("contextmenu", (e) => {
      if (e.target.closest(".sr-card")) return;
      const h = e.target.closest(".sr-content [data-sr-slot]");
      if (h) { e.preventDefault(); unitMenu(e.clientX, e.clientY, +h.dataset.srSlot); }
    });
    // illustrated faces load their moving parts on first use: refresh the faces that were drawn without them
    window.addEventListener("hr-unit-art", () => {
      const scope = [R.root, $("sr-fhost")].filter(Boolean);
      if (!scope.some((s) => s.querySelector(".ua-loading"))) return;
      scope.forEach((sc) => sc.querySelectorAll(".sr-fit[data-sr-face]").forEach((fx) => {
        if (!fx.querySelector(".ua-loading")) return;
        const s = getSlot(+fx.dataset.srFace);
        if (!s) return;
        const st = fx.classList.contains("sr-cfit") && R.focus ? R.focus.station : null;
        fx.innerHTML = faceHTML(s, st && (st.amp || st.inherit)).html;
      }));
      indexControls();
    });
    window.addEventListener("resize", () => { if (R.focus) fitCard(R.focus); });
  }
  /* =====================================================================
     VIEW HOOKS
     ===================================================================== */
  function enter() {
    R.visible = true;
    if (!R.built && !build()) return;
    const fresh = R.stale || R.key !== structKey();
    if (fresh) render("enter");
    else { fit(); paintState(); }
    R.lvlShown = -1;
    AU.slot = outSlot(); patchQuery();
    prewarm();
  }
  function leave() {
    if (R.focus && R.focus.room) closeFocus(true);
    if (R.drag) dragEnd(null, true);
    R.visible = false;
    clearTimeout(R.warmT);
    trimRasters([R.sceneId]);
    hideTag(); hideFoot(); closeSpaces(); closeBag();
    if (R.wrap) R.wrap.classList.remove("sr-sounding");
    R.live = false;
    pulseRest();
  }
  /* Called after every rack update (applyRackState) and bypass toggle. */
  function sync() {
    if (R.focus && !R.focus.room) {     // a chain unit is lifted: keep it hidden in the redrawn chain
      const f = R.focus, s = getSlot(f.slot);
      if ((!s || s.table_id !== f.tid) && adoptSwapped(f)) { fillCard(f); indexControls(); }   // a classic cab swapped for the Cabinet unit
      const s2 = getSlot(f.slot);
      if (!s2 || s2.table_id !== f.tid || f.psig !== `${rackState.preset_idx}|${rackState.preset_name}`) closeFocus(true);
      else {
        let src = focusSrc(f);
        if (!src && f.src && !document.contains(f.src)) { const u = document.querySelector(`#rack-bay .unit[data-unit="${f.slot}"]`); if (u) src = u.querySelector(":scope > .hw-body") || u; }   // its chain unit was redrawn
        if (src && src !== f.src) { f.src = src; }
        if (src) src.classList.add("sr-lifted");
        paintCardHead(f);
      }
    }
    if (!R.built) return;
    const k = structKey();
    if (k !== R.key) {
      if (!R.visible) { R.stale = true; return; }
      render("change");
    } else {
      paintState();
      if (R.focus && R.focus.mode === "mic") paintMic(R.focus);
    }
  }
  /* The Signal Chain's click on a unit's body lifts it into the same card. Only a clean click (no drag, no control). */
  let chainDown = null;
  function chainUnitClick(e, unitEl) {
    if (!unitEl || e.button !== 0 || e.shiftKey || e.metaKey || e.ctrlKey || e.altKey) return false;
    if (e.target.closest(".ctl, button, .drawer, .mic, a, input, select, textarea, [data-act], .lane-drop, .ua-tools, .piano, .sel-box")) return false;
    if (!chainDown || Math.hypot(e.clientX - chainDown.x, e.clientY - chainDown.y) > 4 || now() - chainDown.t > 900) return false;
    if (typeof ui !== "undefined" && (ui.learnCol != null)) return false;
    const idx = +unitEl.dataset.unit;
    if (!getSlot(idx)) return false;
    focusUnit(idx, unitEl);
    return true;
  }
  function focusUnit(slot, fromEl, opts) {
    const host = chainHost();
    if (!host) return;
    if (!R.ukeys) { const seen = {}; R.ukeys = new Map((rackState.slots || []).map((s) => { const n = (seen[s.table_id] = (seen[s.table_id] || 0) + 1); return [s.slot_idx, `u:${s.table_id}#${n}`]; })); }
    if (!fromEl) fromEl = document.querySelector(`#rack-bay .unit[data-unit="${slot}"]`) || document.querySelector(`#sheet-body .unit[data-unit="${slot}"]`);
    const src = fromEl ? (fromEl.querySelector(":scope > .hw-body") || fromEl) : null;
    if (ui.selected !== slot) selectUnit(slot, false);
    openFocus(opts && opts.mode ? { slot, mode: opts.mode } : { slot }, { host, src });
  }
  // Esc puts the unit back (after menus, dialogs and text fields have had their turn). Installed up front: the
  // Signal Chain's pop-out works before the Room was ever built.
  document.addEventListener("keydown", (e) => {
    if (e.key !== "Escape") return;
    if (!R.focus) { if (R.visible && R.root) { closeSpaces(); closeBag(); } return; }
    if (R.focus.room && !R.visible) return;
    const menu = $("menu");
    if ((menu && !menu.hidden) || (typeof ui !== "undefined" && (ui.modalOpen || ui.sheetSlot != null || ui.learnCol != null)) || typingIn(document.activeElement)) return;
    e.preventDefault(); e.stopPropagation();
    closeFocus();
  }, true);
  document.addEventListener("keydown", (e) => {
    if (!R.focus || e.metaKey || e.ctrlKey || e.altKey || typingIn(document.activeElement)) return;
    if (R.focus.room && !R.visible) return;
    const k = e.key === "+" || e.key === "=" ? 1.5 : e.key === "-" || e.key === "_" ? 1 / 1.5 : e.key === "0" ? 0 : null;
    if (k == null) return;
    e.preventDefault(); e.stopPropagation();
    zoomBy(k);
  }, true);
  document.addEventListener("pointerdown", (e) => { if (e.target.closest && e.target.closest("#rack-bay")) chainDown = { x: e.clientX, y: e.clientY, t: now() }; }, true);

  document.addEventListener("visibilitychange", () => {
    if (!R.wrap) return;
    R.wrap.classList.toggle("sr-paused", document.hidden);   // CSS animations stop; telemetry writes stop (onTelemetry)
    if (document.hidden) clearTimeout(R.warmT); else if (R.visible) prewarm();
  });
  if (window.HRViews) HRViews.register("room", { enter, leave });
  window.HRRoom = { sync, onTelemetry, open: () => window.HRViews && HRViews.show("room"), isOpen: () => R.visible, lastChange: () => (R.flipLog || []).slice(),
    focusUnit, chainUnitClick, close: () => closeFocus(),
    // the Cab & mics panel for a cabinet slot, from anywhere (the Cabinet faceplate, a classic cab's button)
    micPanel: (slot, fromEl, opts) => {
      if (R.visible && R.built) openFocus({ slot, mode: "mic" }); else focusUnit(slot, fromEl, { mode: "mic" });
      const s = getSlot(slot);
      if (opts && opts.convert && R.focus && R.focus.slot === slot && s && !(window.HRCabinet && HRCabinet.isCabinet(s))) convertCab(R.focus);
    },
    micArt: (m) => micSVG(micKind(m)), micKind, zoomBy: (k) => zoomBy(k), zoomInfo: () => (R.focus ? { uz: R.focus.uz || 1, k: R.focus.k } : null), scene: () => R.sceneId, setScene, scenes: () => SCENES.map((s) => s.id),
    _debug: () => R, _au: () => AU,
    // test hook: board cables drawn over the pedals that cross a pedal's face anywhere but at their own jacks
    _cableCheck: () => {
      const rs = R.cableRects || [], bad = [];
      (R.cableTop || []).forEach((pts, i) => { const e = [pts[0], pts[pts.length - 1]]; pts.forEach((p) => { if (!e.some((q) => Math.hypot(p.x - q.x, p.y - q.y) < 5) && rs.some((r) => p.x > r.x && p.x < r.x + r.w && p.y > r.y && p.y < r.y + r.h)) bad.push(i); }); });
      return { top: (R.cableTop || []).length, under: (R.root && R.root.querySelectorAll(".sr-under .sr-cab-c").length) || 0, violations: [...new Set(bad)].length };
    } };
})();
