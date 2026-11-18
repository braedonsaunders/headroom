/* Headroom — Guitar Tech: the shop and Augie  (guitar-tech-art.js; used by guitar-tech.js)
 *
 * Original SVG art, built from JS templates, plus the rig that animates it.
 *
 *   HRTechArt.markup(name)  → the scene: six stacked <svg> layers sharing one coordinate system (800×560 core,
 *                             art runs wider/taller for other pane shapes). Back → front:
 *                             back (wall, guitars, pegboard, window: static) · bfx (fan, rain, neon, clock hands)
 *                             · char (Augie's body and head) · bench (bench, props, Fuse) · arms (arms, held
 *                             props, laptop) · light (lamp, light, foreground, vignette).
 *   HRTechArt.create(sceneEl, env) → the rig: { setState, react, speak, typing, calm, poke, frame, still, reset }.
 *                             frame(now) is driven by guitar-tech.js's single requestAnimationFrame loop and
 *                             returns false when it has nothing left to do; still() poses everything once
 *                             (reduced motion).
 *
 * Augie's face is a small rig (brows, lids, pupils, cheeks, mustache, jaw, a viseme mouth); his arms are two-bone
 * IK chains whose hands switch between drawn poses. Actions (gestures, idles, reactions) are short keyframed clips
 * layered over a per-state base pose; everything eases through springs.
 */
(function () {
  "use strict";

  const TAU = Math.PI * 2;
  const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
  const lerp = (a, b, t) => a + (b - a) * t;
  const ss = (t) => { t = clamp(t, 0, 1); return t * t * (3 - 2 * t); };
  const n2 = (v) => Math.round(v * 100) / 100;
  const rnd = (a, b) => a + Math.random() * (b - a);
  const pick = (arr) => arr[Math.floor(Math.random() * arr.length)];

  /* ---------------------------------------------------------------- palette (character + props; the room's
     colours that change with the theme are CSS variables in guitar-tech.css) */
  const K = {
    skin: "#ebb590", skinD: "#cf9070", skinDD: "#a9684e", skinL: "#f8d6b6", nose: "#e9a286", blush: "#e27d6a", lip: "#b8645a",
    hair: "#e4dfd7", hairD: "#aca397", hairDD: "#8c8378", hairL: "#fbf8f3",
    frame: "#3a2214", frameL: "#8a5a32", iris: "#6d8a96", irisD: "#3f5560",
    plaid: "#8e2f2a", apron: "#bb8f57", apronD: "#8d6638", apronL: "#d7b077", strap: "#6a4126", brass: "#d8b04a", brassD: "#9c7a26",
    cat: "#df8d47", catD: "#b4622b", catDD: "#8f4a1f", catL: "#f2b77c", cream: "#f6e4cc",
    ink: "#2a1a10", steel: "#c8ccd0", steelD: "#7d838a", rubber: "#1e1c1c",
  };

  /* ---------------------------------------------------------------- small generators */
  // n parallel strings from (x1a..x1b, y1) to (x2a..x2b, y2)
  function strings(n, x1a, x1b, y1, x2a, x2b, y2, cls) {
    let s = "";
    for (let i = 0; i < n; i++) {
      const t = n === 1 ? 0.5 : i / (n - 1);
      s += `M${n2(lerp(x1a, x1b, t))} ${y1}L${n2(lerp(x2a, x2b, t))} ${y2}`;
    }
    return `<path d="${s}" class="${cls || "gt-str"}"/>`;
  }
  // frets on a vertical neck from y0 (nut) toward the body: positions follow the 12th-root-of-2 rule
  function frets(x0, x1, yNut, scale, count, cls) {
    let s = "";
    for (let i = 1; i <= count; i++) { const y = yNut + scale * (1 - Math.pow(2, -i / 12)); s += `M${x0} ${n2(y)}H${x1}`; }
    return `<path d="${s}" class="${cls || "gt-fret"}"/>`;
  }
  function fretY(yNut, scale, i) { return yNut + scale * (1 - Math.pow(2, -i / 12)); }
  function inlays(cx, yNut, scale, r, fill) {
    let s = "";
    for (const i of [3, 5, 7, 9]) s += `<circle cx="${cx}" cy="${n2((fretY(yNut, scale, i - 1) + fretY(yNut, scale, i)) / 2)}" r="${r}" fill="${fill}"/>`;
    const y12 = (fretY(yNut, scale, 11) + fretY(yNut, scale, 12)) / 2;
    s += `<circle cx="${cx - 2.2}" cy="${n2(y12)}" r="${r}" fill="${fill}"/><circle cx="${cx + 2.2}" cy="${n2(y12)}" r="${r}" fill="${fill}"/>`;
    return s;
  }
  // a run of tuning keys: posts on the headstock face + buttons sticking out of its side
  function tuners(pts, side, btn) {
    let s = "";
    for (const [x, y] of pts) {
      s += `<path d="M${x} ${y}h${side * 6}" stroke="#9aa0a6" stroke-width="2.2"/>`;
      s += `<rect x="${side > 0 ? x + 5 : x - 5 - (btn || 5)}" y="${y - 2.4}" width="${btn || 5}" height="4.8" rx="2.2" fill="url(#gt-g-chrome)" stroke="#6f757b" stroke-width=".4"/>`;
      s += `<circle cx="${x}" cy="${y}" r="1.7" fill="url(#gt-g-chrome)" stroke="#5d6369" stroke-width=".4"/>`;
    }
    return s;
  }

  /* ---------------------------------------------------------------- shared defs (live in the back layer;
     url(#…) references resolve across the inline SVGs of the page) */
  const DEFS = `
  <defs>
    <linearGradient id="gt-g-wall" gradientUnits="userSpaceOnUse" x1="0" y1="-600" x2="0" y2="420"><stop offset="0" class="gt-s-wall1"/><stop offset="1" class="gt-s-wall2"/></linearGradient>
    <pattern id="gt-p-paper" width="36" height="36" patternUnits="userSpaceOnUse">
      <rect width="36" height="36" class="gt-paper-bg"/><path d="M0 0V36M3 0V36M18 0V36" class="gt-paper-st"/>
      <path d="M10.5 9.5c1.6 1.3 1.6 3.7 0 5c-1.6-1.3-1.6-3.7 0-5zM10.5 13.5v4M28.5 27.5c1.6 1.3 1.6 3.7 0 5c-1.6-1.3-1.6-3.7 0-5zM28.5 31.5v4" class="gt-paper-mo"/>
    </pattern>
    <pattern id="gt-p-wains" x="-20" width="58" height="120" patternUnits="userSpaceOnUse">
      <rect width="58" height="120" class="gt-wains-bg"/><path d="M0 0V120" class="gt-wains-seam"/><path d="M2 0V120" class="gt-wains-hi"/>
      <path d="M14 0c3 30-2 50 1 80s-1 30 0 40M36 0c-2 22 3 46 0 70s2 36 0 50M47 10c2 18-1 30 1 44" class="gt-wains-grain"/>
    </pattern>
    <pattern id="gt-p-grain" width="160" height="22" patternUnits="userSpaceOnUse">
      <path d="M0 4c40-3 60 3 100 0s50 2 60 0M0 11c30 2 70-3 110 1s40-2 50 0M0 18c50-2 70 3 100 0s40 1 60-1" class="gt-grain"/>
      <path d="M62 8c6-3 12-3 16 0c-4 4-12 4-16 0z" class="gt-knot"/>
    </pattern>
    <pattern id="gt-p-peg" width="9" height="9" patternUnits="userSpaceOnUse"><rect width="9" height="9" class="gt-peg-bg"/><circle cx="4.5" cy="4.5" r="1.25" class="gt-peg-hole"/></pattern>
    <pattern id="gt-p-plaid" width="26" height="26" patternUnits="userSpaceOnUse">
      <rect width="26" height="26" fill="${K.plaid}"/>
      <rect x="0" width="9" height="26" fill="#5b1a17" opacity=".62"/><rect y="0" width="26" height="9" fill="#5b1a17" opacity=".55"/>
      <path d="M13.5 0V26M0 13.5H26" stroke="#24090a" stroke-width="1.6" opacity=".7"/>
      <path d="M20.5 0V26M0 20.5H26" stroke="#e7c98e" stroke-width=".8" opacity=".5"/>
      <path d="M4.5 0V26M0 4.5H26" stroke="#2b0c0b" stroke-width=".7" opacity=".45"/>
      <path d="M-2 2L2 -2M-2 8L8 -2M-2 14L14 -2M-2 20L20 -2M-2 26L26 -2M4 28L28 4M10 28L28 10M16 28L28 16M22 28L28 22" stroke="#000" stroke-width=".55" opacity=".13"/>
    </pattern>
    <pattern id="gt-p-canvas" width="4" height="4" patternUnits="userSpaceOnUse"><rect width="4" height="4" fill="${K.apron}"/><path d="M0 1h4M1 0v4" stroke="#7a5a30" stroke-width=".5" opacity=".28"/><path d="M0 3h4" stroke="#f0d29a" stroke-width=".4" opacity=".25"/></pattern>
    <pattern id="gt-p-grille" width="4" height="4" patternUnits="userSpaceOnUse"><rect width="4" height="4" fill="#d6c08f"/><path d="M0 4L4 0M-1 1L1 -1M3 5L5 3" stroke="#9c8152" stroke-width=".9"/></pattern>
    <pattern id="gt-p-tolex" width="5" height="5" patternUnits="userSpaceOnUse"><rect width="5" height="5" fill="#2a2523"/><circle cx="1.2" cy="1.4" r=".7" fill="#3a3330"/><circle cx="3.6" cy="3.5" r=".6" fill="#191615"/></pattern>
    <pattern id="gt-p-slats" width="4" height="10" patternUnits="userSpaceOnUse"><rect width="4" height="10" fill="#3b2414"/><rect width="2.2" height="10" fill="#6a4426"/></pattern>
    <pattern id="gt-p-rag" width="6" height="6" patternUnits="userSpaceOnUse"><rect width="6" height="6" fill="#c0392b"/><rect width="3" height="6" fill="#f2e6d6" opacity=".55"/><rect width="6" height="3" fill="#f2e6d6" opacity=".4"/></pattern>
    <pattern id="gt-p-rainA" width="60" height="44" patternUnits="userSpaceOnUse"><path d="M6 2l-2 9M22 18l-2 9M40 6l-2 9M52 26l-2 9M14 32l-2 9M33 36l-2 7" stroke="#bcd6ff" stroke-width=".9" stroke-linecap="round" opacity=".55"/></pattern>
    <pattern id="gt-p-rainB" width="46" height="62" patternUnits="userSpaceOnUse"><path d="M10 4l-3 14M30 26l-3 14M42 46l-3 13M20 50l-2 10" stroke="#9fbde8" stroke-width=".7" stroke-linecap="round" opacity=".35"/></pattern>

    <linearGradient id="gt-g-chrome" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f4f6f8"/><stop offset=".45" stop-color="#b9bfc5"/><stop offset=".55" stop-color="#8e959c"/><stop offset="1" stop-color="#d8dde1"/></linearGradient>
    <linearGradient id="gt-g-brass" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#f6dc8a"/><stop offset=".5" stop-color="#c99a32"/><stop offset="1" stop-color="#8a6418"/></linearGradient>
    <radialGradient id="gt-g-burst" cx=".5" cy=".58" r=".62"><stop offset="0" stop-color="#f7cf62"/><stop offset=".45" stop-color="#e09a35"/><stop offset=".78" stop-color="#8a3a12"/><stop offset="1" stop-color="#2a0f05"/></radialGradient>
    <linearGradient id="gt-g-gold" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#f1d27a"/><stop offset=".55" stop-color="#cf9f3e"/><stop offset="1" stop-color="#94681e"/></linearGradient>
    <radialGradient id="gt-g-spruce" cx=".45" cy=".62" r=".65"><stop offset="0" stop-color="#f3d39a"/><stop offset=".7" stop-color="#deac68"/><stop offset="1" stop-color="#9b6630"/></radialGradient>
    <linearGradient id="gt-g-cherry" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#b8402c"/><stop offset=".6" stop-color="#7c1c14"/><stop offset="1" stop-color="#3c0b07"/></linearGradient>
    <linearGradient id="gt-g-maple" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#d6a964"/><stop offset=".5" stop-color="#ecc98c"/><stop offset="1" stop-color="#c99a58"/></linearGradient>
    <linearGradient id="gt-g-rose" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#2a160c"/><stop offset=".5" stop-color="#4a2a18"/><stop offset="1" stop-color="#26130a"/></linearGradient>
    <linearGradient id="gt-g-gloss" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#fff" stop-opacity="0"/><stop offset=".5" stop-color="#fff" stop-opacity=".5"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
    <linearGradient id="gt-g-shade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3f6b58"/><stop offset=".6" stop-color="#2b4d40"/><stop offset="1" stop-color="#1c362c"/></linearGradient>
    <linearGradient id="gt-g-top" x1="0" y1="0" x2="0" y2="1"><stop offset="0" class="gt-s-top1"/><stop offset="1" class="gt-s-top2"/></linearGradient>
    <linearGradient id="gt-g-front" gradientUnits="userSpaceOnUse" x1="0" y1="434" x2="0" y2="560"><stop offset="0" class="gt-s-front1"/><stop offset="1" class="gt-s-front2"/></linearGradient>
    <linearGradient id="gt-g-skyN" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0a1128"/><stop offset=".65" stop-color="#1d2a52"/><stop offset="1" stop-color="#3a3a5e"/></linearGradient>
    <linearGradient id="gt-g-skyD" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#7fc0ea"/><stop offset=".7" stop-color="#cfe9f6"/><stop offset="1" stop-color="#f4ead2"/></linearGradient>
    <radialGradient id="gt-g-halo"><stop offset="0" stop-color="#ffd27e" stop-opacity=".95"/><stop offset=".25" stop-color="#ffb24a" stop-opacity=".45"/><stop offset="1" stop-color="#ff9a30" stop-opacity="0"/></radialGradient>
    <radialGradient id="gt-g-bulb"><stop offset="0" stop-color="#fff8e0"/><stop offset=".3" stop-color="#ffd98a" stop-opacity=".9"/><stop offset="1" stop-color="#ffb347" stop-opacity="0"/></radialGradient>
    <radialGradient id="gt-g-pool" gradientUnits="userSpaceOnUse" cx="432" cy="200" r="330"><stop offset="0" class="gt-s-pool" stop-opacity="1"/><stop offset=".55" class="gt-s-pool" stop-opacity=".35"/><stop offset="1" class="gt-s-pool" stop-opacity="0"/></radialGradient>
    <radialGradient id="gt-g-benchpool" gradientUnits="userSpaceOnUse" cx="430" cy="420" r="260" gradientTransform="matrix(1 0 0 .3 0 294)"><stop offset="0" stop-color="#ffd38a" stop-opacity=".55"/><stop offset=".6" stop-color="#ffc067" stop-opacity=".16"/><stop offset="1" stop-color="#ffb04a" stop-opacity="0"/></radialGradient>
    <linearGradient id="gt-g-cone" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffdc96" stop-opacity=".5"/><stop offset=".5" stop-color="#ffc56a" stop-opacity=".13"/><stop offset="1" stop-color="#ffb04a" stop-opacity="0"/></linearGradient>
    <linearGradient id="gt-g-beam" x1=".8" y1="0" x2=".2" y2="1"><stop offset="0" stop-color="#fff6d6" stop-opacity=".55"/><stop offset=".55" stop-color="#ffe8b0" stop-opacity=".2"/><stop offset="1" stop-color="#ffe0a0" stop-opacity="0"/></linearGradient>
    <radialGradient id="gt-g-vig" cx=".55" cy=".45" r=".78"><stop offset=".5" stop-color="#000" stop-opacity="0"/><stop offset="1" class="gt-s-vig"/></radialGradient>
    <radialGradient id="gt-g-shadow"><stop offset="0" stop-color="#0c0603" stop-opacity=".55"/><stop offset=".6" stop-color="#0c0603" stop-opacity=".22"/><stop offset="1" stop-color="#0c0603" stop-opacity="0"/></radialGradient>
    <radialGradient id="gt-g-contact"><stop offset="0" stop-color="#140904" stop-opacity=".5"/><stop offset="1" stop-color="#140904" stop-opacity="0"/></radialGradient>
    <radialGradient id="gt-g-skin" gradientUnits="userSpaceOnUse" cx="420" cy="196" r="122" fx="428" fy="184"><stop offset="0" stop-color="#f9d7b8"/><stop offset=".42" stop-color="${K.skin}"/><stop offset=".78" stop-color="#d99a77"/><stop offset="1" stop-color="#bf7f62"/></radialGradient>
    <linearGradient id="gt-g-neck" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#b9785c"/><stop offset=".45" stop-color="#d79a78"/><stop offset="1" stop-color="#e2a985"/></linearGradient>
    <linearGradient id="gt-g-limb" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f3c7a2"/><stop offset=".5" stop-color="${K.skin}"/><stop offset="1" stop-color="#c88a6a"/></linearGradient>
    <linearGradient id="gt-g-sleeve" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".16"/><stop offset=".45" stop-color="#fff" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity=".35"/></linearGradient>
    <linearGradient id="gt-g-torsoShade" gradientUnits="userSpaceOnUse" x1="318" y1="0" x2="482" y2="0"><stop offset="0" stop-color="#000" stop-opacity=".42"/><stop offset=".3" stop-color="#000" stop-opacity=".08"/><stop offset=".62" stop-color="#fff" stop-opacity=".06"/><stop offset=".88" stop-color="#000" stop-opacity=".12"/><stop offset="1" stop-color="#000" stop-opacity=".38"/></linearGradient>
    <linearGradient id="gt-g-apronShade" gradientUnits="userSpaceOnUse" x1="352" y1="0" x2="448" y2="0"><stop offset="0" stop-color="#3a2410" stop-opacity=".35"/><stop offset=".35" stop-color="#3a2410" stop-opacity="0"/><stop offset=".75" stop-color="#fff2d0" stop-opacity=".12"/><stop offset="1" stop-color="#3a2410" stop-opacity=".25"/></linearGradient>
    <radialGradient id="gt-g-hair" gradientUnits="userSpaceOnUse" cx="420" cy="200" r="90"><stop offset="0" stop-color="${K.hairL}"/><stop offset=".6" stop-color="${K.hair}"/><stop offset="1" stop-color="${K.hairD}"/></radialGradient>
    <linearGradient id="gt-g-stache" x1=".7" y1="0" x2=".3" y2="1"><stop offset="0" stop-color="#fbf8f2"/><stop offset=".55" stop-color="#e2dcd2"/><stop offset="1" stop-color="#b3aa9e"/></linearGradient>
    <linearGradient id="gt-g-lens" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#e8f4ff" stop-opacity=".26"/><stop offset=".45" stop-color="#cfe6ff" stop-opacity=".06"/><stop offset="1" stop-color="#9fc3e6" stop-opacity=".16"/></linearGradient>
    <linearGradient id="gt-g-frame" gradientUnits="userSpaceOnUse" x1="364" y1="200" x2="436" y2="232"><stop offset="0" stop-color="#5a3018"/><stop offset=".35" stop-color="#2c170c"/><stop offset=".6" stop-color="#6e3c1c"/><stop offset="1" stop-color="#2a150a"/></linearGradient>
    <radialGradient id="gt-g-iris" cx=".45" cy=".4" r=".6"><stop offset="0" stop-color="#9fbcc6"/><stop offset=".6" stop-color="${K.iris}"/><stop offset="1" stop-color="${K.irisD}"/></radialGradient>
    <radialGradient id="gt-g-sclera" cx=".55" cy=".35" r=".7"><stop offset="0" stop-color="#fbf6ee"/><stop offset=".75" stop-color="#ece0d0"/><stop offset="1" stop-color="#cdb7a2"/></radialGradient>
    <radialGradient id="gt-g-catfur" gradientUnits="userSpaceOnUse" cx="585" cy="382" r="80"><stop offset="0" stop-color="${K.catL}"/><stop offset=".55" stop-color="${K.cat}"/><stop offset="1" stop-color="${K.catD}"/></radialGradient>
    <linearGradient id="gt-g-mug" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#8f2f20"/><stop offset=".35" stop-color="#c4513a"/><stop offset=".7" stop-color="#b7462f"/><stop offset="1" stop-color="#7a2618"/></linearGradient>
    <linearGradient id="gt-g-radio" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#9a5a2c"/><stop offset="1" stop-color="#5a3016"/></linearGradient>
    <radialGradient id="gt-g-dial" cx=".5" cy=".5" r=".6"><stop offset="0" stop-color="#ffe7a6"/><stop offset=".7" stop-color="#f5b450"/><stop offset="1" stop-color="#c9781e"/></radialGradient>
    <radialGradient id="gt-g-glowA"><stop offset="0" stop-color="#ffbf5a" stop-opacity=".55"/><stop offset="1" stop-color="#ff9a2a" stop-opacity="0"/></radialGradient>
    <radialGradient id="gt-g-glowR"><stop offset="0" stop-color="#ff5a4a" stop-opacity=".7"/><stop offset="1" stop-color="#ff2a2a" stop-opacity="0"/></radialGradient>
    <radialGradient id="gt-g-screen"><stop offset="0" stop-color="#bfe6ff" stop-opacity=".9"/><stop offset="1" stop-color="#7fc4ff" stop-opacity="0"/></radialGradient>
    <radialGradient id="gt-g-tubeglow"><stop offset="0" stop-color="#ffcf7a" stop-opacity=".9"/><stop offset="1" stop-color="#ff9a3a" stop-opacity="0"/></radialGradient>
    <linearGradient id="gt-g-lid" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#e3e7eb"/><stop offset="1" stop-color="#a9b0b8"/></linearGradient>
    <linearGradient id="gt-g-steel" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#5a5f66"/><stop offset="1" stop-color="#2a2d31"/></linearGradient>
    <linearGradient id="gt-g-glass" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#dff2f6" stop-opacity=".35"/><stop offset=".2" stop-color="#fff" stop-opacity=".55"/><stop offset=".35" stop-color="#dff2f6" stop-opacity=".18"/><stop offset="1" stop-color="#bcd8de" stop-opacity=".3"/></linearGradient>
    <linearGradient id="gt-g-leaf" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#7fae5c"/><stop offset="1" stop-color="#3c6a35"/></linearGradient>
    <linearGradient id="gt-g-pot" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#a24b2a"/><stop offset=".4" stop-color="#d47a4e"/><stop offset="1" stop-color="#8e3e22"/></linearGradient>
    <filter id="gt-f-soft" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="2.2"/></filter>
    <clipPath id="gt-c-win"><rect x="592" y="60" width="124" height="170" rx="2"/></clipPath>
  </defs>`;

  /* ---------------------------------------------------------------- guitars (original, generic shapes).
     Local space: neck centred on x=0, headstock top at y=0, body toward +y. */
  const BODY_S = "M-7 132C-9 122-16 107-24 105C-32 103-36 113-34 124C-32 134-38 142-39 156C-41 176-40 204-24 218C-12 228 12 228 24 218C40 204 41 176 38 160C36 150 32 144 33 136C34 128 33 118 26 116C19 115 14 124 9 132Z";
  const BODY_LP = "M-6 132C-14 119-31 118-37 131C-42 142-35 152-38 164C-44 181-42 208-22 221C-10 228 10 228 22 221C42 208 44 181 38 166C34 154 39 144 34 134C30 125 21 121 14 121C10 121 7 125 6 132Z";
  const BODY_AC = "M-8 110C-26 106-38 116-37 134C-36 146-30 150-32 160C-36 168-46 178-46 198C-46 226-26 242 0 242C26 242 46 226 46 198C46 178 36 168 32 160C30 150 36 146 37 134C38 116 26 106 8 110Z";
  function hanger(cls) {
    return `<g class="${cls || "gt-hanger"}"><rect x="-6" y="18" width="12" height="26" rx="2"/><path d="M-9 33q0 6 4 7M9 33q0 6-4 7" fill="none" stroke-width="3" stroke-linecap="round"/></g>`;
  }
  function shadowOf(path, dx, dy) { return `<path d="${path}" transform="translate(${dx} ${dy})" class="gt-wallshadow" filter="url(#gt-f-soft)"/>`; }
  function guitarS(tx, ty, rot, o) {
    o = o || {};
    const body = o.body || "url(#gt-g-burst)", guard = o.guard || "#f3eee4", neck = "url(#gt-g-maple)";
    const tun = tuners([[-2.6, 7], [-2.4, 12], [-2.2, 17], [-2, 22], [-1.8, 27], [-1.6, 32]], -1, 4.6);
    return `<g transform="translate(${tx} ${ty}) rotate(${rot || 0})">
      ${o.shadow === false ? "" : shadowOf(BODY_S, -7, 6)}${hanger()}
      <path d="${BODY_S}" fill="${body}" stroke="#1d0c05" stroke-width=".9"/>
      <path d="M-30 132C-36 150-36 178-27 200" fill="none" stroke="#fff" stroke-opacity=".3" stroke-width="2.6" stroke-linecap="round"/>
      <path d="M-7 133C-18 131-26 135-28 147C-31 160-33 175-29 189C-25 202-11 207 2 204C8 202 12 197 13 194L22 194C27 189 27 182 23 176L11 164L10 133Z" fill="${guard}" stroke="#bdb6a8" stroke-width=".5"/>
      <g fill="#f8f5ee" stroke="#a7a093" stroke-width=".5"><rect x="-10" y="147" width="20" height="6" rx="3"/><rect x="-10" y="166" width="20" height="6" rx="3"/><rect x="-10" y="182" width="20" height="6" rx="3" transform="rotate(-8 0 185)"/></g>
      <g fill="#8a8f95"><circle cx="-6.5" cy="150" r=".7"/><circle cx="-3.9" cy="150" r=".7"/><circle cx="-1.3" cy="150" r=".7"/><circle cx="1.3" cy="150" r=".7"/><circle cx="3.9" cy="150" r=".7"/><circle cx="6.5" cy="150" r=".7"/><circle cx="-6.5" cy="169" r=".7"/><circle cx="-3.9" cy="169" r=".7"/><circle cx="-1.3" cy="169" r=".7"/><circle cx="1.3" cy="169" r=".7"/><circle cx="3.9" cy="169" r=".7"/><circle cx="6.5" cy="169" r=".7"/></g>
      <g fill="#f4efe4" stroke="#8b8478" stroke-width=".5"><circle cx="17" cy="182" r="3"/><circle cx="20" cy="191" r="3"/><circle cx="15" cy="199" r="3"/></g><path d="M12 172l4 5" stroke="#222" stroke-width="1.6" stroke-linecap="round"/>
      <rect x="-9" y="206" width="18" height="8" rx="1.2" fill="url(#gt-g-chrome)" stroke="#6b7177" stroke-width=".4"/><path d="M-6 208v4M-3.5 208v4M-1 208v4M1.5 208v4M4 208v4M6.5 208v4" stroke="#7a8086" stroke-width=".9"/>
      <path d="M-5.5 37L-6.5 10C-7 3-3-1 2 0C7 1 9.5 5 8 9.5C6.5 13 8.5 17 7.5 22L5.5 37Z" fill="${neck}" stroke="#6b4a22" stroke-width=".6"/>${tun}
      <path d="M-1 4c2 0 4 2 4 5" stroke="#3a2a1a" stroke-width=".6" fill="none" opacity=".5"/>
      <path d="M-5.5 36L-6.4 162L6.4 162L5.5 36Z" fill="${neck}" stroke="#7a5a2c" stroke-width=".4"/>
      ${frets(-6.3, 6.3, 37, 176, 21)}${inlays(0, 37, 176, 1, "#3b2a1b")}
      <rect x="-5.6" y="35.6" width="11.2" height="1.8" fill="#f6f1e2"/>
      ${strings(6, -4.2, 4.2, 37, -5, 5, 210)}
      <circle cx="-25" cy="106" r="1.6" fill="url(#gt-g-chrome)"/>
    </g>`;
  }
  function guitarLP(tx, ty, rot, o) {
    o = o || {};
    const body = o.body || "url(#gt-g-gold)";
    const tun = tuners([[-4.6, 10], [-5.2, 19], [-5.8, 28]], -1, 5) + tuners([[4.6, 10], [5.2, 19], [5.8, 28]], 1, 5);
    let inl = "";
    for (const i of [3, 5, 7, 9, 12]) { const a = fretY(39, 168, i - 1), b = fretY(39, 168, i); inl += `<path d="M-3.6 ${n2(a + 1.2)}H3.6L4.2 ${n2(b - 1.2)}H-4.2Z" fill="#efe6d2" opacity=".9"/>`; }
    return `<g transform="translate(${tx} ${ty}) rotate(${rot || 0})">
      ${o.shadow === false ? "" : shadowOf(BODY_LP, -7, 6)}${hanger()}
      <path d="${BODY_LP}" fill="${body}" stroke="#efe4c8" stroke-width="1.8"/>
      <path d="${BODY_LP}" fill="none" stroke="#3a240c" stroke-width=".6"/>
      <path d="M-33 140C-38 160-38 190-24 210" fill="none" stroke="#fff" stroke-opacity=".38" stroke-width="2.6" stroke-linecap="round"/>
      <path d="M9 141C17 140 25 146 25 156C25 168 19 180 12 183C10 176 9 160 9 141Z" fill="#efe6cf" stroke="#b9ae94" stroke-width=".5"/>
      <g fill="#f1e8d0" stroke="#9c9277" stroke-width=".5"><rect x="-10" y="146" width="20" height="10" rx="1.6"/><rect x="-10" y="181" width="20" height="10" rx="1.6"/></g>
      <g fill="#9aa0a6"><circle cx="-7" cy="149" r=".8"/><circle cx="-4.2" cy="149" r=".8"/><circle cx="-1.4" cy="149" r=".8"/><circle cx="1.4" cy="149" r=".8"/><circle cx="4.2" cy="149" r=".8"/><circle cx="7" cy="149" r=".8"/><circle cx="-7" cy="153" r=".8"/><circle cx="-4.2" cy="153" r=".8"/><circle cx="-1.4" cy="153" r=".8"/><circle cx="1.4" cy="153" r=".8"/><circle cx="4.2" cy="153" r=".8"/><circle cx="7" cy="153" r=".8"/><circle cx="-7" cy="184" r=".8"/><circle cx="-4.2" cy="184" r=".8"/><circle cx="-1.4" cy="184" r=".8"/><circle cx="1.4" cy="184" r=".8"/><circle cx="4.2" cy="184" r=".8"/><circle cx="7" cy="184" r=".8"/><circle cx="-7" cy="188" r=".8"/><circle cx="-4.2" cy="188" r=".8"/><circle cx="-1.4" cy="188" r=".8"/><circle cx="1.4" cy="188" r=".8"/><circle cx="4.2" cy="188" r=".8"/><circle cx="7" cy="188" r=".8"/></g>
      <rect x="-11" y="200" width="22" height="3" rx="1.2" fill="url(#gt-g-chrome)"/><rect x="-10" y="210" width="20" height="3.4" rx="1.6" fill="url(#gt-g-chrome)"/>
      <g fill="url(#gt-g-brass)" stroke="#6b4a10" stroke-width=".4"><circle cx="21" cy="194" r="3.2"/><circle cx="29" cy="203" r="3.2"/><circle cx="14" cy="205" r="3.2"/><circle cx="22" cy="214" r="3.2"/></g>
      <circle cx="-27" cy="137" r="2.4" fill="#e8dcc0"/><path d="M-27 137l-2-5" stroke="url(#gt-g-chrome)" stroke-width="1.3"/>
      <path d="M-6.5 39L-9 8C-9.5 3-6-.5-2 1.5Q0 3 2 1.5C6-.5 9.5 3 9 8L6.5 39Z" fill="#1d1612" stroke="#000" stroke-width=".5"/>${tun}
      <path d="M-2 31h4l1 4h-6z" fill="#111" stroke="#cfc4a8" stroke-width=".4"/><path d="M-4 6c2-2 6-2 8 0" stroke="#d9c58d" stroke-width=".7" fill="none"/>
      <path d="M-5.2 38L-6.4 152L6.4 152L5.2 38Z" fill="url(#gt-g-rose)"/><path d="M-5.2 38L-6.4 152M5.2 38L6.4 152" stroke="#efe4c8" stroke-width=".7"/>
      ${frets(-6.2, 6.2, 39, 168, 22, "gt-fret lp")}${inl}
      <rect x="-5.4" y="37.4" width="10.8" height="1.8" fill="#f2ead4"/>
      ${strings(6, -4, 4, 39, -5, 5, 211)}
    </g>`;
  }
  function guitarAC(tx, ty, rot, o) {
    o = o || {};
    const tun = tuners([[-4.2, 9], [-4.6, 17], [-5, 25]], -1, 4.4) + tuners([[4.2, 9], [4.6, 17], [5, 25]], 1, 4.4);
    return `<g transform="translate(${tx} ${ty}) rotate(${rot || 0})">
      ${o.shadow === false ? "" : shadowOf(BODY_AC, -7, 6)}${hanger()}
      <path d="${BODY_AC}" fill="${o.body || "url(#gt-g-spruce)"}" stroke="#4a2810" stroke-width="1"/>
      <path d="${BODY_AC}" fill="none" stroke="#f2e2c0" stroke-width="1.2" transform="translate(0 0) scale(.965 .975) translate(0 3.5)" opacity=".7"/>
      <path d="M-40 176C-44 192-42 214-28 228" fill="none" stroke="#fff" stroke-opacity=".3" stroke-width="2.6" stroke-linecap="round"/>
      <circle cy="152" r="15.5" fill="none" stroke="#5a3418" stroke-width="1"/><circle cy="152" r="13.6" fill="none" stroke="#2f7f73" stroke-width="1.2" stroke-dasharray="1.4 1"/><circle cy="152" r="11" fill="#1d0f06"/>
      <path d="M12 160C20 158 28 164 27 174C26 182 18 184 12 178C9 174 9 166 12 160Z" fill="#5a2a12" opacity=".9"/>
      <rect x="-15" y="203" width="30" height="7" rx="3" fill="#3a1d0c"/><path d="M-9 205.5h18" stroke="#efe6d2" stroke-width="1.1"/>
      <g fill="#efe6d2"><circle cx="-6.5" cy="208" r=".9"/><circle cx="-3.9" cy="208" r=".9"/><circle cx="-1.3" cy="208" r=".9"/><circle cx="1.3" cy="208" r=".9"/><circle cx="3.9" cy="208" r=".9"/><circle cx="6.5" cy="208" r=".9"/></g>
      <path d="M-7 37L-8.5 7C-8.5 2-5 0 0 0S8.5 2 8.5 7L7 37Z" fill="#2a170b" stroke="#000" stroke-width=".5"/>${tun}
      <path d="M-5.4 36L-6.4 136L6.4 136L5.4 36Z" fill="url(#gt-g-rose)"/>
      ${frets(-6.2, 6.2, 37, 176, 20, "gt-fret lp")}${inlays(0, 37, 176, 1, "#e9e2cf")}
      <rect x="-5.6" y="35.6" width="11.2" height="1.8" fill="#f2ead4"/>
      ${strings(6, -4.2, 4.2, 37, -6.5, 6.5, 207, "gt-str ac")}
    </g>`;
  }

  /* ---------------------------------------------------------------- BACK: the wall (static) */
  const BACK = `
  <rect x="-900" y="-1000" width="2800" height="1600" fill="url(#gt-g-wall)"/>
  <rect x="-900" y="-560" width="2800" height="860" fill="url(#gt-p-paper)" class="gt-paper"/>
  <g class="gt-ceiling"><rect x="-900" y="-1000" width="2800" height="436" class="gt-ceil"/><rect x="-900" y="-566" width="2800" height="10" class="gt-crown"/><rect x="-900" y="-556" width="2800" height="5" class="gt-crown2"/></g>
  <rect x="-900" y="-560" width="2800" height="860" fill="url(#gt-g-pool)" class="gt-pool"/>

  <!-- up high (tall panes): a gig poster, Fuse's portrait -->
  <g class="gt-high">
    <g transform="translate(150 -170) rotate(-2)">
      <rect width="86" height="116" fill="#e4b24c"/><rect x="5" y="5" width="76" height="106" fill="none" stroke="#3a2210" stroke-width="1.2"/>
      <text x="43" y="22" text-anchor="middle" class="gt-post-t">THE</text><text x="43" y="36" text-anchor="middle" class="gt-post-h">HUMBUCKERS</text>
      <g transform="translate(43 68)"><rect x="-20" y="-16" width="40" height="30" rx="3" fill="#2a1a10"/><rect x="-16" y="-8" width="32" height="18" rx="2" fill="#c98a3a"/><path d="M-12-12h24" stroke="#e4b24c" stroke-width="1.6" stroke-dasharray="2 2.4"/><path d="M-4-26l-6 12h6l-4 10 12-14h-6l4-8z" fill="#fff4d8" stroke="#2a1a10" stroke-width=".8"/></g>
      <text x="43" y="97" text-anchor="middle" class="gt-post-s">LIVE AT THE TUBE ROOM</text><text x="43" y="105" text-anchor="middle" class="gt-post-s">FRIDAY · 9 PM</text>
      <path d="M-4 4l12-6 3 6-12 6zM78 -2l12 6-3 6-12-6z" fill="#efe6c8" opacity=".75"/>
    </g>
    <path d="M300 -152l-28 22M300 -152l28 22" class="gt-chain"/>
    <rect x="254" y="-132" width="92" height="106" rx="3" class="gt-frame-pic"/><rect x="261" y="-125" width="78" height="72" fill="#efe0c2"/>
    <g transform="translate(300 -88)"><path d="M-18-6l5-18 11 11zM18-6l-5-18-11 11z" fill="${K.cat}"/><path d="M-15-9l3-10 6 6zM15-9l-3-10-6 6z" fill="#f2a9a0"/><ellipse rx="19" ry="17" fill="${K.cat}"/><path d="M-6-15q6-3 12 0M-4-11q4-2 8 0" stroke="${K.catD}" stroke-width="1.8" fill="none"/><ellipse cy="7" rx="9" ry="7" fill="${K.cream}"/><path d="M-10-1q3 3 6 0M4-1q3 3 6 0" stroke="#3a2210" stroke-width="1.6" fill="none" stroke-linecap="round"/><path d="M-2 4l2 2 2-2z" fill="#d9776e"/><path d="M-8 6l-12-1M-8 8l-11 3M8 6l12-1M8 8l11 3" stroke="#fff" stroke-width=".6" opacity=".8"/><path d="M-12 19q12 8 24 0" stroke="#2f7f73" stroke-width="3" fill="none"/><circle cy="24" r="2.4" fill="${K.brass}"/></g>
    <text x="300" y="-42" text-anchor="middle" class="gt-pic-t">EMPLOYEE OF THE MONTH</text><text x="300" y="-33" text-anchor="middle" class="gt-pic-s">Fuse · every month</text>
  </g>

  <!-- the sign -->
  <g class="gt-sign-g">
    <path d="M182 -700V34M338 -700V34" class="gt-chain"/>
    <rect x="160" y="34" width="200" height="60" rx="7" class="gt-sign"/>
    <rect x="160" y="34" width="200" height="60" rx="7" fill="url(#gt-p-grain)" opacity=".5"/>
    <rect x="167" y="41" width="186" height="46" rx="4" class="gt-sign-in"/>
    <circle cx="182" cy="40" r="2.4" fill="url(#gt-g-brass)"/><circle cx="338" cy="40" r="2.4" fill="url(#gt-g-brass)"/>
    <text x="260.8" y="67.8" text-anchor="middle" class="gt-sign-t sh">AUGIE’S BENCH</text>
    <text x="260" y="67" text-anchor="middle" class="gt-sign-t">AUGIE’S BENCH</text>
    <text x="260" y="81" text-anchor="middle" class="gt-sign-s">repairs · setups · tone</text>
  </g>

  <!-- the clock -->
  <g class="gt-clock">
    <circle cx="534" cy="58" r="26" class="gt-clock-rim"/><circle cx="534" cy="58" r="21.5" fill="#f6efe2"/><circle cx="534" cy="58" r="21.5" fill="none" stroke="#d8ccb6" stroke-width="1.2"/>
    <path d="M534 38.5v4M534 73.5v4M514.5 58h4M549.5 58h4M543.8 41l-1.3 2.2M550.9 48.2l-2.2 1.3M550.9 67.8l-2.2-1.3M543.8 75l-1.3-2.2M524.2 75l1.3-2.2M517.1 67.8l2.2-1.3M517.1 48.2l2.2 1.3M524.2 41l1.3 2.2" stroke="#4a3a2a" stroke-width="1.3"/>
    <text x="534" y="68" text-anchor="middle" class="gt-clock-t">AUGIE &amp; SON</text>
  </g>

  <!-- wall of guitars: left of the bench (more show up in wide panes) -->
  <g class="gt-guitars">
    <path d="M-220 128H310" class="gt-slat"/><path d="M-220 133H310" class="gt-slat2"/>
    ${guitarAC(108, 96, 1.5)}
    ${guitarS(186, 100, -1.2)}
    ${guitarLP(262, 96, 1)}
    ${guitarS(32, 98, -0.6, { body: "#7fc4b0", guard: "#2a1d14" })}
    ${guitarLP(-46, 100, 1.2, { body: "url(#gt-g-cherry)" })}
  </g>

  <!-- a tone-stack poster (wide panes) -->
  <g transform="translate(-170 150) rotate(1.5)">
    <rect width="96" height="122" fill="#efe3c8"/><rect x="4" y="4" width="88" height="114" fill="none" stroke="#7a5a3a" stroke-width=".8"/>
    <text x="48" y="18" text-anchor="middle" class="gt-post-d">HOW A TONE STACK</text><text x="48" y="27" text-anchor="middle" class="gt-post-d">REALLY WORKS</text>
    <path d="M12 82C26 82 30 58 48 66S70 46 84 50" fill="none" stroke="#b8452f" stroke-width="1.6"/><path d="M12 92H84M12 40V92" stroke="#5a4a36" stroke-width=".7"/>
    <g fill="#2a1d14"><circle cx="24" cy="104" r="5"/><circle cx="48" cy="104" r="5"/><circle cx="72" cy="104" r="5"/></g>
    <path d="M24 104l-2-4M48 104l3-3M72 104l1-5" stroke="#efe3c8" stroke-width="1.1"/>
  </g>

  <!-- pegboard of tools, between Augie and the window -->
  <g class="gt-pegboard">
    <rect x="486" y="96" width="92" height="196" rx="2" fill="url(#gt-p-peg)"/><rect x="486" y="96" width="92" height="196" rx="2" class="gt-peg-edge"/>
    <g class="gt-hook"><path d="M499 107v5M518 107v5M527 107v5M536 107v5M556 107v5M512 190v6M560 176v6"/></g>
    <!-- needle-nose pliers -->
    <g transform="translate(499 112) rotate(4)"><path d="M-1 0l-1.6 26M1 0l1.6 26" stroke="#9aa1a8" stroke-width="2"/><path d="M-2.4 24l-4 34M2.4 24l4 34" stroke="#c0392b" stroke-width="4.4" stroke-linecap="round"/><circle cy="24" r="2" fill="#6b7177"/></g>
    <!-- screwdrivers -->
    <g><rect x="514.5" y="110" width="7" height="22" rx="2.4" fill="#e9b93a"/><path d="M518 132v30" stroke="#b8bec4" stroke-width="1.6"/><path d="M516.5 114v14M519.5 114v14" stroke="#b5862a" stroke-width=".8"/></g>
    <g><rect x="523.5" y="110" width="7" height="20" rx="2.4" fill="#c0392b"/><path d="M527 130v24" stroke="#b8bec4" stroke-width="1.5"/><path d="M525.5 114v12M528.5 114v12" stroke="#7d2219" stroke-width=".8"/></g>
    <g><rect x="532.5" y="110" width="7" height="24" rx="2.4" fill="#2f6fae"/><path d="M536 134v34" stroke="#b8bec4" stroke-width="1.7"/><path d="M534.5 114v16M537.5 114v16" stroke="#1d4a78" stroke-width=".8"/></g>
    <!-- side cutters -->
    <g transform="translate(556 112) rotate(-6)"><path d="M-3 0q3-6 6 0l-1 10h-4z" fill="#9aa1a8"/><path d="M-2 10l-4 30M2 10l4 30" stroke="#2f6fae" stroke-width="4.2" stroke-linecap="round"/></g>
    <!-- solder sucker -->
    <g transform="translate(548 150) rotate(8)"><rect x="-3.5" width="7" height="34" rx="2" fill="#d9dcdf" stroke="#8a9096" stroke-width=".5"/><rect x="-2" y="-8" width="4" height="8" fill="#2a2a2a"/><path d="M0 34v8" stroke="#2a2a2a" stroke-width="1.6"/><rect x="-4" y="12" width="8" height="5" rx="1" fill="#c0392b"/></g>
    <!-- coiled cable -->
    <g fill="none" stroke="#151515" stroke-linecap="round"><ellipse cx="512" cy="212" rx="15" ry="18" stroke-width="2.6"/><ellipse cx="511" cy="213" rx="13" ry="16" stroke-width="2.4" stroke="#262626"/><ellipse cx="513" cy="211" rx="11" ry="15" stroke-width="2.2"/><path d="M500 224q-4 14 2 22" stroke-width="2.4"/><path d="M524 222q4 10 0 18" stroke-width="2.4"/></g>
    <rect x="499" y="244" width="5" height="10" rx="1" fill="url(#gt-g-chrome)"/><rect x="521.5" y="238" width="5" height="10" rx="1" fill="url(#gt-g-chrome)"/>
    <!-- multimeter -->
    <g transform="translate(560 182)"><rect x="-12" width="24" height="40" rx="4" fill="#e9b93a" stroke="#9a7420" stroke-width=".6"/><rect x="-9" y="4" width="18" height="10" rx="1.4" fill="#2f3a2c"/><text x="0" y="11.6" text-anchor="middle" class="gt-lcd">4.70</text><circle cy="25" r="6" fill="#2a2a2a"/><path d="M0 25l3-4" stroke="#ddd" stroke-width="1.2"/><circle cx="-6" cy="36" r="1.5" fill="#c0392b"/><circle cx="6" cy="36" r="1.5" fill="#222"/></g>
    <!-- jars of knobs on a little shelf -->
    <rect x="488" y="266" width="88" height="4" fill="#6b4426"/>
    <g transform="translate(498 248)"><rect x="-9" y="0" width="18" height="18" rx="3" fill="url(#gt-g-glass)" stroke="#9fb9bf" stroke-width=".5"/><rect x="-9.5" y="-3" width="19" height="4" rx="1" fill="#c0392b"/><g><circle cx="-4" cy="13" r="3" fill="#1d1d1d"/><circle cx="3" cy="14" r="3" fill="#efe6d2"/><circle cx="0" cy="8" r="2.6" fill="#1d1d1d"/><circle cx="5" cy="7" r="2" fill="${K.brass}"/></g></g>
    <g transform="translate(521 246)"><rect x="-9" y="0" width="18" height="20" rx="3" fill="url(#gt-g-glass)" stroke="#9fb9bf" stroke-width=".5"/><rect x="-9.5" y="-3" width="19" height="4" rx="1" fill="url(#gt-g-brass)"/><g><path d="M-6 16l3-5 3 5zM1 17l3-5 3 5z" fill="#2a2a2a"/><circle cx="-3" cy="8" r="2.6" fill="#e9b93a"/><circle cx="3.5" cy="9" r="2.6" fill="#2f6fae"/></g></g>
    <g transform="translate(544 249)"><rect x="-9" y="0" width="18" height="17" rx="3" fill="url(#gt-g-glass)" stroke="#9fb9bf" stroke-width=".5"/><rect x="-9.5" y="-3" width="19" height="4" rx="1" fill="#2f7f73"/><g><circle cx="-3" cy="12" r="2.4" fill="#d9a441"/><circle cx="3" cy="12" r="2.4" fill="#d9a441"/><circle cx="0" cy="7" r="2.4" fill="#b8452f"/></g></g>
    <g transform="translate(567 250)"><rect x="-8" y="0" width="16" height="16" rx="7" fill="#e8dcc0" opacity=".9"/><path d="M-5 6h10M-5 9h8M-5 12h10" stroke="#8a7a60" stroke-width=".6"/></g>
    <g transform="translate(493 230) rotate(-6)"><rect width="15" height="11" fill="#f4ecd8"/><path d="M2 3h10M2 6h8M2 9h9" stroke="#8a7a60" stroke-width=".5"/><circle cx="7.5" cy="1" r="1.2" fill="#c0392b"/></g>
  </g>

  <!-- the window -->
  <g class="gt-window">
    <rect x="578" y="46" width="152" height="198" rx="5" class="gt-casing"/>
    <rect x="584" y="52" width="140" height="186" rx="3" class="gt-frame"/>
    <g clip-path="url(#gt-c-win)">
      <g class="gt-night">
        <rect x="592" y="60" width="124" height="170" fill="url(#gt-g-skyN)"/>
        <g fill="#fff6dc"><circle cx="604" cy="74" r=".9"/><circle cx="630" cy="98" r=".7"/><circle cx="676" cy="70" r=".8"/><circle cx="700" cy="104" r=".6"/><circle cx="616" cy="122" r=".6"/></g>
        <circle cx="626" cy="84" r="11" fill="#f1e6c4"/><circle cx="622" cy="81" r="2.2" fill="#ddd0a8"/><circle cx="630" cy="89" r="1.6" fill="#ddd0a8"/>
        <path d="M600 96q14-8 30-2q12-6 26 0q-14 5-30 4q-14 3-26-2z" fill="#2b3762" opacity=".9"/>
        <path d="M592 230V176h16v-10h14v18h12v-30h20v22h10v-12h18v24h14v-8h20v70z" fill="#0d1226"/>
        <g fill="#f5c46a"><rect x="640" y="160" width="4" height="5"/><rect x="648" y="160" width="4" height="5" opacity=".5"/><rect x="640" y="170" width="4" height="5" opacity=".7"/><rect x="598" y="184" width="3.5" height="4"/><rect x="676" y="182" width="4" height="5"/><rect x="704" y="196" width="4" height="5" opacity=".6"/><rect x="684" y="190" width="4" height="5" opacity=".4"/></g>
        <g class="gt-streetlamp"><circle cx="694" cy="128" r="26" fill="url(#gt-g-halo)" opacity=".75"/><path d="M700 232V132q0-6-6-6h-6" fill="none" stroke="#141a2c" stroke-width="2.6"/><path d="M684 124h14l-3 6h-8z" fill="#1d2338"/><ellipse cx="691" cy="130.5" rx="4.5" ry="1.6" fill="#ffe2a0"/><path d="M684 132l-8 98h36l-8-98z" fill="#ffd27e" opacity=".08"/></g>
        <path d="M592 222h124v8H592z" fill="#1d2440" opacity=".7"/><path d="M600 224h20M640 226h30M684 224h24" stroke="#ffd27e" stroke-width=".7" opacity=".35"/>
        <g class="gt-drops" fill="#cfe2ff" opacity=".55"><ellipse cx="604" cy="88" rx="1.2" ry="1.6"/><ellipse cx="640" cy="72" rx="1" ry="1.4"/><ellipse cx="668" cy="116" rx="1.3" ry="1.8"/><ellipse cx="610" cy="160" rx="1" ry="1.4"/><ellipse cx="702" cy="150" rx="1.1" ry="1.5"/><ellipse cx="682" cy="78" rx=".9" ry="1.3"/><ellipse cx="626" cy="196" rx="1.2" ry="1.7"/><ellipse cx="660" cy="206" rx="1" ry="1.4"/><ellipse cx="712" cy="96" rx="1" ry="1.3"/><ellipse cx="596" cy="130" rx=".9" ry="1.2"/></g>
      </g>
      <g class="gt-day">
        <rect x="592" y="60" width="124" height="170" fill="url(#gt-g-skyD)"/>
        <circle cx="690" cy="86" r="30" fill="#fff6d2" opacity=".55"/><circle cx="690" cy="86" r="13" fill="#fff4c4"/>
        <g class="gt-cloud" fill="#fff" opacity=".92"><ellipse cx="624" cy="104" rx="18" ry="7"/><ellipse cx="636" cy="99" rx="12" ry="8"/><ellipse cx="612" cy="101" rx="9" ry="5.5"/></g>
        <g fill="#fff" opacity=".7"><ellipse cx="672" cy="126" rx="12" ry="4"/><ellipse cx="680" cy="123" rx="7" ry="4.5"/></g>
        <path d="M592 230V188q10-14 22-6q8-12 20-4q10-10 22 0q12-8 22 2q10-6 18 0q8-4 20 2V230z" fill="#9cc48a"/>
        <path d="M600 230v-30h22v30zM646 230v-24h26v24z" fill="#e7d6bc"/><path d="M596 202l15-12 15 12zM642 208l17-12 17 12z" fill="#b8634a"/><g fill="#7fb0d6"><rect x="604" y="208" width="6" height="7"/><rect x="614" y="208" width="6" height="7"/><rect x="652" y="214" width="6" height="7"/><rect x="662" y="214" width="6" height="7"/></g>
        <path d="M684 230v-40" stroke="#7a5a3a" stroke-width="3"/><circle cx="684" cy="184" r="14" fill="#6fa35a"/><circle cx="676" cy="190" r="9" fill="#5f9150"/><circle cx="692" cy="190" r="9" fill="#7eb466"/>
        <path d="M616 78q3-3 6 0q3-3 6 0M640 70q2-2 4 0q2-2 4 0" stroke="#4a4a4a" stroke-width=".9" fill="none"/>
      </g>
    </g>
    <path d="M592 60l40 0-60 170z" fill="#fff" opacity=".05"/><path d="M660 60l26 0-70 170h-20z" fill="#fff" opacity=".035"/>
    <rect x="652" y="58" width="4" height="174" class="gt-frame"/><rect x="590" y="143" width="128" height="4" class="gt-frame"/>
    <rect x="584" y="52" width="140" height="186" rx="3" fill="none" class="gt-frame-line"/>
    <rect x="572" y="236" width="164" height="9" rx="2" class="gt-sill"/><rect x="574" y="244" width="160" height="3" class="gt-sill2"/>
    <!-- plants on the sill -->
    <g><path d="M594 236l2-14h18l2 14z" fill="url(#gt-g-pot)"/><rect x="592" y="219" width="26" height="5" rx="1.5" fill="#d47a4e"/>
      <path d="M605 220v-24a5 5 0 0 1 10 0v10M605 210h-6v-8a4 4 0 0 0-8 0v4" fill="none" stroke="#5f8f4e" stroke-width="7" stroke-linecap="round"/>
      <path d="M607 200v14M613 194v8M597 202v4" stroke="#3f6a35" stroke-width=".8" stroke-dasharray="1 2"/><circle cx="610" cy="191" r="2.4" fill="#f28fa0"/></g>
    <g><path d="M700 236l1.5-10h14l1.5 10z" fill="#e8dcc8"/><path d="M708 227q-10-12-4-20q4 8 4 20M708 227q8-14 14-14q-6 10-14 14M708 227q-2-16 4-22q0 12-4 22" fill="#6fa35a"/></g>
  </g>

  <!-- shelf under the window: a little combo amp, spare tubes, a pick jar, manuals -->
  <g class="gt-shelf">
    <rect x="562" y="290" width="228" height="8" rx="1.5" class="gt-plank"/><rect x="562" y="297" width="228" height="2" fill="#000" opacity=".2"/>
    <path d="M588 298l10 14M766 298l-10 14" class="gt-bracket"/>
    <ellipse cx="628" cy="290" rx="46" ry="3" fill="url(#gt-g-contact)"/>
    <g class="gt-amp">
      <rect x="586" y="238" width="84" height="52" rx="5" fill="url(#gt-p-tolex)" stroke="#0f0d0c" stroke-width="1"/>
      <rect x="590" y="242" width="76" height="11" rx="1.5" fill="#d9cfb8"/><path d="M590 253h76" stroke="#7a6a50" stroke-width=".6"/>
      <g fill="#1d1a18"><circle cx="602" cy="247.5" r="3"/><circle cx="612" cy="247.5" r="3"/><circle cx="622" cy="247.5" r="3"/><circle cx="632" cy="247.5" r="3"/><circle cx="642" cy="247.5" r="3"/></g>
      <path d="M602 247.5l-1-3M612 247.5l2-2.5M622 247.5l0-3M632 247.5l-2-2M642 247.5l2-2" stroke="#efe6d2" stroke-width=".9"/>
      <circle cx="656" cy="247.5" r="3.4" fill="#ff5a3a"/><circle cx="656" cy="247.5" r="8" fill="url(#gt-g-glowR)"/><circle cx="655" cy="246.5" r="1" fill="#ffd2c4"/>
      <rect x="591" y="256" width="74" height="30" rx="2" fill="url(#gt-p-grille)"/><path d="M591 256h74" stroke="#000" stroke-opacity=".3"/>
      <rect x="616" y="262" width="24" height="7" rx="1.5" fill="#e8d6a4" stroke="#8a7240" stroke-width=".4"/><text x="628" y="267.6" text-anchor="middle" class="gt-amp-logo">Augie</text>
      <path d="M612 238q16-9 32 0" fill="none" stroke="#111" stroke-width="3.2" stroke-linecap="round"/><rect x="608" y="235.5" width="5" height="4" rx="1" fill="url(#gt-g-chrome)"/><rect x="643" y="235.5" width="5" height="4" rx="1" fill="url(#gt-g-chrome)"/>
      <g fill="url(#gt-g-chrome)"><circle cx="589" cy="241" r="1.6"/><circle cx="667" cy="241" r="1.6"/><circle cx="589" cy="287" r="1.6"/><circle cx="667" cy="287" r="1.6"/></g>
    </g>
    <g><rect x="676" y="262" width="20" height="28" rx="1.5" fill="#efe4cf"/><path d="M676 270h20" stroke="#b5462f" stroke-width="3"/><text x="686" y="283" text-anchor="middle" class="gt-box-t">12AX7</text><rect x="676" y="262" width="20" height="28" rx="1.5" fill="none" stroke="#a89878" stroke-width=".5"/></g>
    <g><rect x="700" y="260" width="22" height="30" rx="5" fill="url(#gt-g-glass)" stroke="#9fb9bf" stroke-width=".8"/><rect x="699" y="256" width="24" height="5" rx="1.5" fill="url(#gt-g-brass)"/>
      <g><path d="M704 284l4-6 4 6z" fill="#e8553d"/><path d="M712 287l4-6 4 6z" fill="#f1c84b"/><path d="M705 288l4-5 4 5z" fill="#3e8ed0"/><path d="M713 278l4-6 4 6z" fill="#6cc070"/><path d="M707 276l3-5 3 5z" fill="#efe6d2"/></g></g>
    <g><rect x="728" y="254" width="9" height="36" fill="#7a3f2b"/><rect x="737" y="258" width="8" height="32" fill="#2f5e6e"/><rect x="745" y="252" width="10" height="38" fill="#c79a3a"/><rect x="755" y="262" width="7" height="28" fill="#4a3a6a" transform="rotate(8 762 290)"/>
      <path d="M730 260h5M739 264h4M747 258h6" stroke="#f4e6c4" stroke-width="1.2" opacity=".7"/></g>
  </g>

  <!-- right wall (wide panes): a coat hook with a jacket and cap, a clinic poster, an amp stack -->
  <g class="gt-right">
    <rect x="786" y="108" width="62" height="8" rx="2" fill="#5a3a22"/><circle cx="800" cy="118" r="3" fill="url(#gt-g-brass)"/><circle cx="834" cy="118" r="3" fill="url(#gt-g-brass)"/>
    <path d="M800 118c-12 6-20 20-22 40l-4 90h52l-4-92c-2-18-10-30-22-38z" fill="#5a4a3a"/><path d="M800 118l-6 40 6 90M800 118l8 40-8 90" stroke="#3c3127" stroke-width="1.2" fill="none"/><path d="M786 150h28" stroke="#3c3127" stroke-width="1.2"/>
    <path d="M820 120q14-4 28 2q2 8-4 10q-12 2-24-2z" fill="#6b5a44"/><path d="M820 128q-4 2-4 4h30" stroke="#4a3e30" stroke-width="2" fill="none"/>
    <g transform="translate(872 92) rotate(-1.5)"><rect width="86" height="112" fill="#1f3f3a"/><rect x="4" y="4" width="78" height="104" fill="none" stroke="#e8c27a" stroke-width=".8"/>
      <text x="43" y="22" text-anchor="middle" class="gt-post-c">AMP CLINIC</text><text x="43" y="32" text-anchor="middle" class="gt-post-cs">every saturday</text>
      <g transform="translate(43 68)"><path d="M-11 18V-6a11 11 0 0 1 22 0v24z" fill="#e8f2f0" fill-opacity=".25" stroke="#e8c27a" stroke-width="1"/><path d="M-5 14V0q5-6 10 0v14" fill="none" stroke="#ffb347" stroke-width="1.4"/><circle r="16" fill="url(#gt-g-glowA)"/><rect x="-12" y="18" width="24" height="6" rx="1" fill="#e8c27a"/></g>
      <text x="43" y="104" text-anchor="middle" class="gt-post-cs">bring it in · we’ll listen</text></g>
    <g><rect x="864" y="318" width="132" height="98" rx="4" fill="url(#gt-p-tolex)" stroke="#0e0c0b"/><rect x="872" y="326" width="116" height="84" rx="2" fill="#3a3530"/><rect x="874" y="328" width="112" height="80" fill="url(#gt-p-grille)" opacity=".55"/>
      <rect x="870" y="266" width="120" height="50" rx="4" fill="url(#gt-p-tolex)" stroke="#0e0c0b"/><rect x="876" y="290" width="108" height="20" rx="2" fill="#c9b27e"/>
      <g fill="#1d1a18"><circle cx="890" cy="300" r="4"/><circle cx="904" cy="300" r="4"/><circle cx="918" cy="300" r="4"/><circle cx="932" cy="300" r="4"/><circle cx="946" cy="300" r="4"/><circle cx="960" cy="300" r="4"/></g><circle cx="974" cy="300" r="3.2" fill="#ff5a3a"/><circle cx="974" cy="300" r="9" fill="url(#gt-g-glowR)"/>
      <rect x="906" y="274" width="48" height="10" rx="2" fill="#e8d6a4"/><text x="930" y="281.6" text-anchor="middle" class="gt-amp-logo big">Augie</text></g>
  </g>

  <!-- wainscot + chair rail -->
  <rect x="-900" y="300" width="2800" height="140" fill="url(#gt-p-wains)"/>
  <rect x="-900" y="300" width="2800" height="140" class="gt-wains-shade"/>
  <rect x="-900" y="292" width="2800" height="10" rx="1" class="gt-rail"/><rect x="-900" y="292" width="2800" height="2" class="gt-rail-hi"/>
  <rect x="-900" y="-1000" width="2800" height="1440" class="gt-haze"/>`;

  /* ---------------------------------------------------------------- BFX: things on the wall that move */
  const NEON_D = "M624 186a6 7.6 0 1 0 12 0a6 7.6 0 1 0-12 0M641 194V178.6h6q5 0 5 4.6t-5 4.6h-6M666 178.6h-9V194h9M657 186.2h7M671 194V178.6l11 15.4V178.6";
  const BLADE = "M13-5.5L78-8.5Q89 0 78 8.5L13 5.5Z";
  const BFX = `
  <defs><clipPath id="gt-c-win2"><rect x="592" y="60" width="124" height="170" rx="2"/></clipPath></defs>
  <g class="gt-fan">
    <path d="M612 -566V-160" class="gt-fan-rod"/><rect x="606" y="-566" width="12" height="8" rx="2" fill="url(#gt-g-brass)"/>
    <g id="gt-fan-blades" transform="translate(612 -146) scale(1 .2)">
      <g class="gt-blades"><path d="${BLADE}"/><path d="${BLADE}" transform="rotate(90)"/><path d="${BLADE}" transform="rotate(180)"/><path d="${BLADE}" transform="rotate(270)"/></g>
      <g fill="url(#gt-g-brass)"><path d="M0-2.4L22-3.4V3.4L0 2.4Z"/><path d="M0-2.4L22-3.4V3.4L0 2.4Z" transform="rotate(90)"/><path d="M0-2.4L22-3.4V3.4L0 2.4Z" transform="rotate(180)"/><path d="M0-2.4L22-3.4V3.4L0 2.4Z" transform="rotate(270)"/></g>
    </g>
    <path d="M596 -160q16-10 32 0l-3 10h-26z" fill="url(#gt-g-brass)"/><ellipse cx="612" cy="-148" rx="17" ry="4.5" fill="#9c7a26"/>
    <path d="M603 -146q9 16 18 0z" fill="#f4ead2" opacity=".85"/><path d="M612 -136v14" stroke="#9c7a26" stroke-width="1"/><circle cx="612" cy="-121" r="1.8" fill="url(#gt-g-brass)"/>
  </g>

  <g class="gt-night" clip-path="url(#gt-c-win2)">
    <g id="gt-rainB"><rect x="592" y="-62" width="124" height="300" fill="url(#gt-p-rainB)"/></g>
    <g id="gt-rainA"><rect x="592" y="-44" width="124" height="300" fill="url(#gt-p-rainA)"/></g>
    <g id="gt-slide1"><path d="M630 64q-1.6 3 0 4.4q1.6-1.4 0-4.4z" fill="#dbe9ff" opacity=".8"/><path d="M630 52v11" stroke="#cfe2ff" stroke-width=".8" opacity=".3"/></g>
    <g id="gt-slide2"><path d="M690 64q-1.4 2.6 0 3.8q1.4-1.2 0-3.8z" fill="#dbe9ff" opacity=".75"/><path d="M690 54v9" stroke="#cfe2ff" stroke-width=".7" opacity=".28"/></g>
  </g>

  <g class="gt-neon">
    <path d="M626 60V170M682 60V170" stroke="#3a3a3a" stroke-width=".8" stroke-dasharray="1.6 1.2"/>
    <rect x="612" y="169" width="86" height="34" rx="6" fill="#0d0b0a" opacity=".55"/>
    <g id="gt-neon-glow">
      <ellipse cx="655" cy="186" rx="64" ry="34" fill="url(#gt-g-glowR)" class="gt-neon-spill"/>
      <rect x="616" y="172.5" width="78" height="27" rx="5" class="gt-neon-tube b w"/><rect x="616" y="172.5" width="78" height="27" rx="5" class="gt-neon-tube b m"/><rect x="616" y="172.5" width="78" height="27" rx="5" class="gt-neon-tube b c"/>
      <path d="${NEON_D}" class="gt-neon-tube w"/><path d="${NEON_D}" class="gt-neon-tube m"/><path d="${NEON_D}" class="gt-neon-tube c"/>
    </g>
  </g>

  <g class="gt-clock-hands">
    <path id="gt-clock-h" d="M534 61V46" stroke="#2a2018" stroke-width="2.6" stroke-linecap="round"/>
    <path id="gt-clock-m" d="M534 62V40" stroke="#2a2018" stroke-width="1.7" stroke-linecap="round"/>
    <path id="gt-clock-s" d="M534 63V41" stroke="#b8452f" stroke-width=".7" stroke-linecap="round"/>
    <circle cx="534" cy="58" r="1.8" fill="#b8452f"/>
    <path d="M518 46a22 22 0 0 1 22-9" stroke="#fff" stroke-width="2" fill="none" opacity=".35" stroke-linecap="round"/>
  </g>`;

  /* ---------------------------------------------------------------- CHAR: Augie (torso + head), the guitar
     on the stand beside him, his thought bubble */
  const BROW = `<path d="M363 201.5C364 193 375 189.4 386 190.4C391.5 191 395.4 193 396.6 196.6C392 197.6 386 197 380 198C373.5 199 368 200.4 363 201.5Z" fill="url(#gt-g-hair)" stroke="${K.hairD}" stroke-width=".6"/>
      <path d="M366 199.6l4-6M370 198.6l4.4-6.8M374.6 197.8l4.2-6.8M379.4 197.2l4-6.4M384 196.6l4.2-5.4M388.6 196.4l3.6-4" stroke="${K.hairD}" stroke-width=".8" stroke-linecap="round"/>
      <path d="M368 197l4.6-5.4M376.6 196l4.6-5.6M386 195.4l4-4" stroke="${K.hairL}" stroke-width=".9" stroke-linecap="round"/>
      <path d="M365 195.6q-4-3-7.6-1.6M371 192.6q-2-4-5.6-4.4M392.6 193q2.6-2.4 5-1.6" stroke="${K.hair}" stroke-width=".8" fill="none" stroke-linecap="round"/>`;
  const PUFF = `<path d="M357 192C344 189 333 199 334 211C327 217 329 230 335 235C330 244 339 255 349 254C354 259 361 256 363 251Z" fill="url(#gt-g-hair)"/>
      <path d="M352 196c-8 2-14 10-13 18M346 214c-6 4-8 12-5 18M344 236c-2 6 1 12 6 14M356 200c-6 6-8 14-7 22M352 232c0 6 2 12 6 16" stroke="${K.hairD}" stroke-width=".9" fill="none" stroke-linecap="round"/>
      <path d="M349 200c-6 4-9 10-9 16M341 226c-2 4-1 8 1 11M350 242c1 4 3 7 6 8" stroke="${K.hairL}" stroke-width="1.1" fill="none" stroke-linecap="round"/>
      <path d="M336 210q-6-1-8 3M334 232q-6 1-7 5M340 250q-4 3-4 7" stroke="${K.hair}" stroke-width=".9" fill="none" stroke-linecap="round"/>`;
  const EAR = `<path d="M353 213C342 206 333 213 334 226C335 239 342 248 353 245Z" fill="url(#gt-g-skin)"/>
      <path d="M350 218C343 215 339 221 340 228C341 235 345 239 350 238" fill="none" stroke="${K.skinD}" stroke-width="1.8" stroke-linecap="round"/>
      <path d="M347 224q-3 2-2 6" fill="none" stroke="${K.skinDD}" stroke-width="1.2" stroke-linecap="round" opacity=".6"/>
      <path d="M338 238q2 6 8 7" fill="none" stroke="${K.blush}" stroke-width="2" opacity=".35" stroke-linecap="round"/>`;
  const STACHE_STRANDS = `<path d="M398 250C390 251 379 256 371 268M396 254C388 258 380 264 376 277M391 249C383 249 373 253 365 263M398 259C392 263 386 266 382 271M384 252C377 256 369 264 364 276" stroke="${K.hairD}" stroke-width=".9" fill="none" stroke-linecap="round" opacity=".8"/>
      <path d="M394 250C386 252 377 258 370 272M397 256C390 260 383 266 380 274M388 249C380 250 371 256 367 266" stroke="${K.hairL}" stroke-width="1.1" fill="none" stroke-linecap="round" opacity=".9"/>`;
  const EYE = (cx, side) => `
      <g id="gt-eye-${side}" transform="translate(${cx} 216.5) scale(1.13) translate(${-cx} -216.5)">
        <path d="M${cx - 9} 217C${cx - 6} 209.4 ${cx + 6} 209.4 ${cx + 9} 217C${cx + 6} 223 ${cx - 6} 223 ${cx - 9} 217Z" fill="#c78a6c" opacity=".55" transform="translate(0 .6)"/>
        <g clip-path="url(#gt-c-eye-${side})">
          <rect x="${cx - 10}" y="206" width="20" height="20" fill="url(#gt-g-sclera)"/>
          <g id="gt-pupil-${side}"><circle cx="${cx}" cy="216.6" r="4.3" fill="url(#gt-g-iris)"/><circle cx="${cx}" cy="216.6" r="4.3" fill="none" stroke="#2b3a40" stroke-width=".7"/><circle cx="${cx}" cy="216.6" r="2.05" fill="#140c08"/><circle cx="${cx + 1.5}" cy="215" r="1.15" fill="#fff"/><circle cx="${cx - 1.4}" cy="218.3" r=".5" fill="#fff" opacity=".7"/></g>
          <g id="gt-lidd-${side}"><path d="M${cx - 12} 240V217H${cx - 8}C${cx - 5} 222.2 ${cx + 5} 222.2 ${cx + 8} 217H${cx + 12}V240Z" fill="#e7ad8a"/><path d="M${cx - 8} 217C${cx - 5} 222.2 ${cx + 5} 222.2 ${cx + 8} 217" fill="none" stroke="#c98a6a" stroke-width=".8"/></g>
          <g id="gt-lidu-${side}"><path d="M${cx - 12} 196H${cx + 12}V217H${cx + 8}C${cx + 5} 210.6 ${cx - 5} 210.6 ${cx - 8} 217H${cx - 12}Z" fill="#e4a684"/><path d="M${cx - 8} 217C${cx - 5} 210.6 ${cx + 5} 210.6 ${cx + 8} 217" fill="none" stroke="#7a4a35" stroke-width="2.4" opacity=".28"/><path d="M${cx - 8} 217C${cx - 5} 210.6 ${cx + 5} 210.6 ${cx + 8} 217" fill="none" stroke="#3a2016" stroke-width="1.3"/></g>
        </g>
        <path id="gt-closed-${side}" d="M${cx - 8} 217.4Q${cx} 222.6 ${cx + 8} 217.4" fill="none" stroke="#3a2016" stroke-width="1.3" stroke-linecap="round" opacity="0"/>
        <path id="gt-crease-${side}" d="M${cx - 8.5} 211.6C${cx - 5} 207 ${cx + 5} 207 ${cx + 8.5} 211.6" fill="none" stroke="#b97858" stroke-width=".8" opacity=".55" stroke-linecap="round"/>
      </g>`;
  const LENS = (cx) => `M${cx - 16} 215.5C${cx - 16} 206.5 ${cx - 9} 201.2 ${cx} 201.2C${cx + 9} 201.2 ${cx + 16} 206.5 ${cx + 16} 215.5C${cx + 16} 224.5 ${cx + 9} 230.2 ${cx} 230.2C${cx - 9} 230.2 ${cx - 16} 224.5 ${cx - 16} 215.5Z`;
  const CHAR = `
  <defs>
    <clipPath id="gt-c-eye-l"><path d="M371 217C374 209.4 386 209.4 389 217C386 223 374 223 371 217Z"/></clipPath>
    <clipPath id="gt-c-eye-r"><path d="M411 217C414 209.4 426 209.4 429 217C426 223 414 223 411 217Z"/></clipPath>
    <clipPath id="gt-c-mouth"><path id="gt-mouth-clip" d="M392 271Q400 270 408 271Q400 272 392 271Z"/></clipPath>
  </defs>

  <!-- a guitar on a stand beside him, waiting for strings -->
  <g class="gt-standgtr" transform="translate(302 268) rotate(-5)">
    <path d="M-5.5 37L-6.5 10C-7 3-3-1 2 0C7 1 9.5 5 8 9.5C6.5 13 8.5 17 7.5 22L5.5 37Z" fill="url(#gt-g-maple)" stroke="#6b4a22" stroke-width=".6"/>
    ${tuners([[-2.6, 7], [-2.4, 12], [-2.2, 17], [-2, 22], [-1.8, 27], [-1.6, 32]], -1, 4.6)}
    <g id="gt-peg" transform="translate(-9.6 22)"><rect x="-2.4" y="-2.6" width="5" height="5.2" rx="2.2" fill="url(#gt-g-chrome)" stroke="#6f757b" stroke-width=".4"/></g>
    <path d="M-5.5 36L-6.6 160L6.6 160L5.5 36Z" fill="url(#gt-g-maple)" stroke="#7a5a2c" stroke-width=".4"/>
    ${frets(-6.3, 6.3, 37, 176, 14)}${inlays(0, 37, 176, 1, "#3b2a1b")}
    <rect x="-5.6" y="35.6" width="11.2" height="1.8" fill="#f6f1e2"/>
    ${strings(6, -4.2, 4.2, 37, -4.8, 4.8, 160)}
    <g id="gt-tuner" transform="translate(3 2) rotate(14)">
      <path d="M-2 4v-6" stroke="#222" stroke-width="2.4"/>
      <circle cy="-8" r="7.4" fill="#1d1d1f" stroke="#444" stroke-width=".8"/><circle cy="-8" r="5.6" id="gt-tuner-face" fill="#2a1410"/>
      <path id="gt-tuner-needle" d="M0-4.6V-11.6" stroke="#ff6a4a" stroke-width="1.1" stroke-linecap="round"/>
      <circle cy="-8" r="5.6" fill="none" stroke="#fff" stroke-opacity=".15"/><path d="M-3-12q3-2 6 0" stroke="#fff" stroke-width=".7" fill="none" opacity=".35"/>
    </g>
  </g>

  <g id="gt-body">
    <ellipse id="gt-wallshadow" cx="384" cy="292" rx="122" ry="150" fill="url(#gt-g-shadow)" class="gt-augshadow"/>
    <g id="gt-torso">
      <path d="M383 268L417 268L420 310L380 310Z" fill="url(#gt-g-neck)"/>
      <path d="M386 292q14 6 28 0" stroke="#a9684e" stroke-width="1.4" fill="none" opacity=".45"/>
      <path d="M318 430L320 352C321 326 334 312 356 306L382 300L418 300L444 306C466 312 479 326 480 352L482 430Z" fill="url(#gt-p-plaid)"/>
      <path d="M318 430L320 352C321 326 334 312 356 306L382 300L418 300L444 306C466 312 479 326 480 352L482 430Z" fill="url(#gt-g-torsoShade)"/>
      <path d="M318 430L320 352C321 326 334 312 356 306L382 300L418 300L444 306C466 312 479 326 480 352L482 430Z" fill="none" stroke="#2a0b09" stroke-width="1.4" opacity=".6"/>
      <path d="M330 340q6 18 2 40M470 340q-6 18-2 40M336 322q10 4 18 2M464 322q-10 4-18 2" stroke="#2a0b09" stroke-width="1.4" fill="none" opacity=".35" stroke-linecap="round"/>
      <path d="M332 342q5 16 2 34M468 342q-5 16-2 34" stroke="#ffb0a0" stroke-width=".8" fill="none" opacity=".18"/>
      <path d="M386 300Q400 322 414 300Z" fill="#efe6d6"/><path d="M389 301Q400 316 411 301" fill="none" stroke="#c9bca6" stroke-width=".8"/>
      <path d="M384 297L370 318L392 313L399 302Z" fill="url(#gt-p-plaid)" stroke="#3a1210" stroke-width=".9"/><path d="M416 297L430 318L408 313L401 302Z" fill="url(#gt-p-plaid)" stroke="#3a1210" stroke-width=".9"/>
      <path d="M372 316L391 312.4M428 316L409 312.4" stroke="#000" stroke-width="1.6" opacity=".22"/>
      <g id="gt-apron">
        <path d="M367 322L380 299" stroke="${K.strap}" stroke-width="5.2" stroke-linecap="round"/><path d="M433 322L420 299" stroke="${K.strap}" stroke-width="5.2" stroke-linecap="round"/>
        <path d="M367.6 320L380 299M432.4 320L420 299" stroke="#a0704a" stroke-width="1" stroke-dasharray="2 1.6" opacity=".7"/>
        <path d="M364 324Q400 320 436 324L449 430L351 430Z" fill="url(#gt-p-canvas)"/>
        <path d="M364 324Q400 320 436 324L449 430L351 430Z" fill="url(#gt-g-apronShade)"/>
        <path d="M368 328Q400 324.4 432 328L444 428M356 428L368 328" fill="none" stroke="#f3dfb2" stroke-width=".8" stroke-dasharray="2.6 2" opacity=".75"/>
        <path d="M364 324Q400 320 436 324" fill="none" stroke="${K.apronD}" stroke-width="2"/>
        <circle cx="367" cy="326" r="2.4" fill="url(#gt-g-brass)" stroke="${K.brassD}" stroke-width=".5"/><circle cx="433" cy="326" r="2.4" fill="url(#gt-g-brass)" stroke="${K.brassD}" stroke-width=".5"/>
        <path d="M376 356q-4 40-2 74M424 356q5 40 2 74M392 380q-2 30 0 50" stroke="${K.apronD}" stroke-width="1.2" fill="none" opacity=".35"/>
        <ellipse cx="400" cy="340" rx="16" ry="7" fill="#efe2c4" stroke="#8d6b3c" stroke-width=".8"/><ellipse cx="400" cy="340" rx="13.6" ry="5" fill="none" stroke="#b8452f" stroke-width=".6" stroke-dasharray="1.2 1"/>
        <text x="400" y="342.4" text-anchor="middle" class="gt-patch">AUGIE</text>
        <!-- chest pocket, its tools -->
        <path d="M389 366l-3-22" stroke="#f2c74a" stroke-width="3.6" stroke-linecap="round"/><path d="M386.2 345.4l-.6-4" stroke="#e8a3a0" stroke-width="3.6" stroke-linecap="round"/><path d="M386 347.4h3" stroke="#c9ccce" stroke-width="1.2"/>
        <path d="M409 366l2-18" stroke="#c2412e" stroke-width="4.6" stroke-linecap="round"/><path d="M411 349l.6-7" stroke="#bfc4c7" stroke-width="1.5" stroke-linecap="round"/><path d="M408.4 352l1-2.6M410 360l1-2.6" stroke="#7d2219" stroke-width=".7"/>
        <path d="M398 366l3-12 3 12z" fill="#3e8ed0"/><path d="M396 366h10" stroke="#1d4a78" stroke-width="1" opacity=".4"/>
        <path d="M380 362h40v24q-20 3-40 0z" fill="#c49a5e" stroke="#8d6b3c" stroke-width="1"/><path d="M380 362h40v24q-20 3-40 0z" fill="url(#gt-p-canvas)" opacity=".5"/>
        <path d="M382 366h36" stroke="#f3dfb2" stroke-width=".7" stroke-dasharray="2 1.6" opacity=".8"/><path d="M400 363v24" stroke="#8d6b3c" stroke-width=".8" opacity=".6"/>
        <path d="M380 362h40" stroke="#6e4c26" stroke-width="1.6"/>
        <!-- big front pocket, rag, stains, solder burns -->
        <path d="M360 404Q400 409 440 404" fill="none" stroke="${K.apronD}" stroke-width="1.4"/><path d="M361 407Q400 412 439 407" fill="none" stroke="#f3dfb2" stroke-width=".7" stroke-dasharray="2.4 1.8" opacity=".7"/>
        <g id="gt-apron-rag"><path d="M434 404q6 2 10 0l3 22q-6 4-12 0z" fill="url(#gt-p-rag)"/><path d="M434 404q6 2 10 0" stroke="#7a1f16" stroke-width=".8" fill="none"/></g>
        <path d="M371 346c4-2 8 0 7 4s-6 4-8 1c-1-2-1-4 1-5zM418 392c3-1 6 1 5 4s-5 3-6 0z" fill="#6b4a22" opacity=".28"/>
        <g fill="#3a2410" opacity=".55"><circle cx="374" cy="392" r="1"/><circle cx="378" cy="396" r=".7"/><circle cx="425" cy="373" r=".8"/><circle cx="394" cy="398" r=".6"/></g>
      </g>
    </g>

    <g id="gt-head">
      <g transform="translate(0 0)">${PUFF}</g><g transform="matrix(-1 0 0 1 800 0)">${PUFF}</g>
      <g class="gt-pencil"><g transform="rotate(-24 452 204)"><rect x="452" y="201.6" width="34" height="5" fill="#f2c74a"/><path d="M452 203.4h34" stroke="#d9a62a" stroke-width=".9"/><path d="M486 201.6l7 2.5-7 2.5z" fill="#e8c9a0"/><path d="M491 203.4l2 .7-2 .7z" fill="#2a2a2a"/><rect x="446" y="201.6" width="6" height="5" fill="#e8a3a0"/><rect x="451" y="201.6" width="2.4" height="5" fill="#c9ccce"/></g></g>
      <g>${EAR}</g><g transform="matrix(-1 0 0 1 800 0)">${EAR}</g>
      <path d="M400 164C434 164 454 190 454 224C454 250 447 268 432 282C422 292 412 297 400 297C388 297 378 292 368 282C353 268 346 250 346 224C346 190 366 164 400 164Z" fill="url(#gt-g-skin)"/>
      <path d="M350 206C346 236 352 262 370 282C358 268 350 250 349 228C349 220 349 212 350 206Z" fill="#a9684e" opacity=".25"/>
      <g opacity=".14" fill="#8a4a32"><ellipse cx="380" cy="214" rx="15" ry="11"/><ellipse cx="420" cy="214" rx="15" ry="11"/></g>
      <ellipse cx="416" cy="178" rx="20" ry="7.5" fill="#fff" opacity=".28" transform="rotate(12 416 178)"/>
      <g fill="#c98a62" opacity=".35"><ellipse cx="388" cy="174" rx="2.2" ry="1.6"/><ellipse cx="394" cy="170" rx="1.4" ry="1.1"/><ellipse cx="430" cy="176" rx="1.6" ry="1.2"/></g>
      <g id="gt-wrinkles" fill="none" stroke="#b97858" stroke-linecap="round" stroke-width="1"><path d="M380 184q20-5 40 0"/><path d="M376 189.6q24-5.6 48 0"/><path d="M384 179q16-4 32 0" stroke-width=".8"/></g>
      <g id="gt-furrow" fill="none" stroke="#b97858" stroke-linecap="round" stroke-width=".9" opacity="0"><path d="M397 196q-1 4 0 8M403 196q1 4 0 8"/></g>
      <g id="gt-wisps" fill="none" stroke="${K.hair}" stroke-linecap="round" stroke-width="1.1"><path d="M397 165C392 155 400 149 406 153"/><path d="M403 165C406 155 416 154 417 160"/><path d="M393 166C386 160 386 152 391 150" stroke-width=".9"/></g>
      <g id="gt-cheeks"><ellipse cx="365" cy="247" rx="11" ry="6.5" fill="${K.blush}" opacity=".3"/><ellipse cx="435" cy="247" rx="11" ry="6.5" fill="${K.blush}" opacity=".3"/><path d="M357 244q8-5 16 0M427 244q8-5 16 0" stroke="#fff" stroke-width="1.2" opacity=".12" fill="none"/></g>
      <g id="gt-smilel" fill="none" stroke="#b97858" stroke-linecap="round" stroke-width="1.2" opacity=".35"><path d="M366 252Q359 263 362 275"/><path d="M434 252Q441 263 438 275"/></g>
      ${EYE(380, "l")}${EYE(420, "r")}
      <g id="gt-crow" fill="none" stroke="#b97858" stroke-linecap="round" stroke-width=".9" opacity=".4"><path d="M361 214l-6-2M361 219l-6 1M362 223.4l-5 3"/><path d="M439 214l6-2M439 219l6 1M438 223.4l5 3"/></g>
      <path d="M396 220C394 228 388 236 389 243C390 250 396 252.6 400 252C404 252.6 410 250 411 243C412 236 406 228 404 220Z" fill="${K.nose}"/>
      <path d="M396 221C394.4 229 389.6 236 390.4 243C391 248 394 250.4 397.6 251.4C393 247 392.6 238 396 221Z" fill="#b8705a" opacity=".35"/>
      <ellipse cx="404.6" cy="241" rx="3" ry="4" fill="#fff" opacity=".35"/><ellipse cx="402" cy="234" rx="1.2" ry="2.4" fill="#fff" opacity=".25"/>
      <path d="M391.6 246.6q2 2.8 5.4 2.6M408.4 246.6q-2 2.8-5.4 2.6" stroke="#9c5a44" stroke-width="1.2" fill="none" stroke-linecap="round" opacity=".7"/>
      <g id="gt-glasses">
        <path d="M367 233.6Q380 238 392 233.4M408 233.4Q420 238 433 233.6" stroke="#7a4030" stroke-width="2.4" fill="none" opacity=".18" stroke-linecap="round"/>
        <path d="M364.4 211L350 219.6M435.6 211L450 219.6" stroke="url(#gt-g-frame)" stroke-width="2.4" stroke-linecap="round"/>
        <path d="${LENS(380)}" fill="url(#gt-g-lens)"/><path d="${LENS(420)}" fill="url(#gt-g-lens)"/>
        <g id="gt-lens-screen" opacity="0"><path d="${LENS(380)}" fill="#9fd8ff" opacity=".35"/><path d="${LENS(420)}" fill="#9fd8ff" opacity=".35"/><g fill="#e6f6ff" opacity=".7"><rect id="gt-scan-l" x="370" y="210" width="20" height="1.6" rx=".8"/><rect id="gt-scan-r" x="410" y="210" width="20" height="1.6" rx=".8"/><rect x="370" y="216" width="14" height="1.2" rx=".6" opacity=".6"/><rect x="410" y="216" width="14" height="1.2" rx=".6" opacity=".6"/></g></g>
        <path d="${LENS(380)}" fill="none" stroke="url(#gt-g-frame)" stroke-width="3.2"/><path d="${LENS(420)}" fill="none" stroke="url(#gt-g-frame)" stroke-width="3.2"/>
        <path d="M368 205.6Q376 200.6 386 202M408 205.6Q416 200.6 426 202" stroke="#c8885a" stroke-width=".8" fill="none" opacity=".7" stroke-linecap="round"/>
        <path d="M395.6 213.4Q400 207.4 404.4 213.4" stroke="url(#gt-g-frame)" stroke-width="2.6" fill="none" stroke-linecap="round"/>
        <g id="gt-glint" fill="#fff"><path d="M386.6 205.4l2.4 2-7 8.6-1.6-1.4z" opacity=".55"/><path d="M390.4 210l1.1 1-4.2 5.2-1-.9z" opacity=".4"/><path d="M426.6 205.4l2.4 2-7 8.6-1.6-1.4z" opacity=".55"/><path d="M430.4 210l1.1 1-4.2 5.2-1-.9z" opacity=".4"/>
          <rect x="370" y="205" width="7" height="2" rx="1" fill="#ffd58a" opacity=".5" transform="rotate(-8 373 206)"/><rect x="410" y="205" width="7" height="2" rx="1" fill="#ffd58a" opacity=".5" transform="rotate(-8 413 206)"/></g>
      </g>
      <g id="gt-brow-l">${BROW}</g>
      <g id="gt-brow-r"><g transform="matrix(-1 0 0 1 800 0)">${BROW}</g></g>
      <g id="gt-chin">
        <path d="M372 280C382 277 418 277 428 280C423 291 412 298 400 298C388 298 377 291 372 280Z" fill="url(#gt-g-skin)"/>
        <path d="M384 290q16 8 32 0" stroke="#b97858" stroke-width="1" fill="none" opacity=".35"/><ellipse cx="403" cy="290" rx="7" ry="3" fill="#fff" opacity=".18"/>
      </g>
      <path id="gt-mouth-hole" d="M392 271Q400 270 408 271Q400 272 392 271Z" fill="#4a1a16"/>
      <g clip-path="url(#gt-c-mouth)"><rect id="gt-teeth" x="388" y="266" width="24" height="5.4" rx="1.6" fill="#f3ece0"/><ellipse id="gt-tongue" cx="400" cy="284" rx="6" ry="3.6" fill="#c4545a"/></g>
      <path id="gt-mouth-line" d="M391 271Q400 273.4 409 271" fill="none" stroke="#6a2a22" stroke-width="1.6" stroke-linecap="round"/>
      <path id="gt-lip" d="M393 273.6Q400 276.8 407 273.6" fill="none" stroke="${K.lip}" stroke-width="2.6" stroke-linecap="round"/>
      <g id="gt-stache">
        <path d="M400 248C392 244 378 244 370 250C362 256 357 266 359 277C361 283 366 285 368 281C372 272 380 270 390 272C394 272 398 270 400 268C402 270 406 272 410 272C420 270 428 272 432 281C434 285 439 283 441 277C443 266 438 256 430 250C422 244 408 244 400 248Z" fill="#5a2a1e" opacity=".28" transform="translate(0 3)"/>
        <path d="M400 248C392 244 378 244 370 250C362 256 357 266 359 277C361 283 366 285 368 281C372 272 380 270 390 272C394 272 398 270 400 268C402 270 406 272 410 272C420 270 428 272 432 281C434 285 439 283 441 277C443 266 438 256 430 250C422 244 408 244 400 248Z" fill="url(#gt-g-stache)" stroke="#a49b8f" stroke-width=".8"/>
        ${STACHE_STRANDS}<g transform="matrix(-1 0 0 1 800 0)">${STACHE_STRANDS}</g>
        <path d="M402 250q10-3 22 2" stroke="#fff" stroke-width="1.6" opacity=".5" fill="none" stroke-linecap="round"/>
      </g>
      <ellipse id="gt-facelight" cx="400" cy="250" rx="46" ry="40" fill="#8fd0ff" opacity="0"/>
    </g>
  </g>

  <g id="gt-thought" opacity="0">
    <circle cx="456" cy="170" r="3.4" class="gt-bub"/><circle cx="469" cy="153" r="5.6" class="gt-bub"/>
    <g class="gt-bub-cloud"><circle cx="498" cy="122" r="19"/><circle cx="516" cy="110" r="16"/><circle cx="534" cy="121" r="15"/><circle cx="525" cy="137" r="15"/><circle cx="505" cy="139" r="15"/></g>
    <g class="gt-bub-in"><circle cx="498" cy="122" r="17"/><circle cx="516" cy="110" r="14"/><circle cx="534" cy="121" r="13"/><circle cx="525" cy="137" r="13"/><circle cx="505" cy="139" r="13"/></g>
    <g id="gt-gear" transform="rotate(0 512 124)"><circle cx="512" cy="124" r="9.4" fill="none" stroke-width="5" stroke-dasharray="3.5 2.9" class="gt-gear-t"/><circle cx="512" cy="124" r="7.4" class="gt-gear-b"/><circle cx="512" cy="124" r="2.6" class="gt-gear-h"/></g>
    <g id="gt-gear2" transform="rotate(0 528 112)"><circle cx="528" cy="112" r="5.2" fill="none" stroke-width="3.2" stroke-dasharray="2.2 2" class="gt-gear-t"/><circle cx="528" cy="112" r="3.8" class="gt-gear-b"/><circle cx="528" cy="112" r="1.4" class="gt-gear-h"/></g>
  </g>`;

  /* ---------------------------------------------------------------- BENCH: the bench and what's on it, Fuse */
  function drawers() {
    let s = "";
    for (let r = 0; r < 4; r++) for (let c = 0; c < 3; c++) {
      const x = 141 + c * 20.4, y = 341 + r * 19.2, out = r === 2 && c === 1 ? 2.2 : 0;
      const bits = ["#c0392b", "#e9b93a", "#2f6fae", "#1d1d1d", "#6cc070", "#efe6d2", "#b8452f", "#d9a441"];
      s += `<g transform="translate(0 ${out})"><rect x="${x}" y="${y}" width="17.6" height="16" rx="1.4" fill="#dfe9ea" opacity=".55" stroke="#8fa3a8" stroke-width=".5"/>`;
      s += `<circle cx="${n2(x + 5)}" cy="${y + 10}" r="1.6" fill="${bits[(r * 3 + c) % 8]}"/><circle cx="${n2(x + 9)}" cy="${y + 11}" r="1.4" fill="${bits[(r * 3 + c + 3) % 8]}"/><rect x="${n2(x + 11)}" y="${y + 9}" width="4" height="1.4" fill="${bits[(r * 5 + c) % 8]}"/>`;
      s += `<rect x="${n2(x + 3)}" y="${y + 2}" width="11.6" height="3.4" fill="#f8f4ea"/><path d="M${n2(x + 4.4)} ${y + 3.7}h${(5 + ((r + c) % 3) * 2)}" stroke="#6a5a48" stroke-width=".6"/><rect x="${n2(x + 6.3)}" y="${y + 13.4}" width="5" height="1.6" rx=".8" fill="#4a5a60"/>`;
      if (out) s += `<path d="M${x} ${y + 16.4}h17.6" stroke="#000" stroke-opacity=".35" stroke-width="1.2"/>`;
      s += `</g>`;
    }
    return s;
  }
  const BENCH = `
  <rect x="-900" y="416" width="2800" height="18" fill="url(#gt-g-top)"/>
  <rect x="-900" y="416" width="2800" height="18" fill="url(#gt-p-grain)" opacity=".4"/>
  <rect x="-900" y="416" width="2800" height="2.2" class="gt-top-edge"/>
  <path d="M120 424l40 1M260 428l30-1M520 427l26 1M690 425l40-1" stroke="#000" stroke-opacity=".12" stroke-width=".8"/>
  <rect x="-900" y="432" width="2800" height="3" class="gt-top-lip"/>
  <rect x="-900" y="435" width="2800" height="200" fill="url(#gt-g-front)"/>
  <rect x="-900" y="435" width="2800" height="200" fill="url(#gt-p-grain)" opacity=".3"/>
  <g class="gt-drawers">${[-440, -196, 48, 292, 536, 780, 1024].map((x) => `<rect x="${x}" y="452" width="214" height="86" rx="3" class="gt-drawer"/><rect x="${x + 6}" y="458" width="202" height="74" rx="2" class="gt-drawer-in"/><path d="M${x + 92} 486h30v6q-15 7-30 0z" class="gt-pull"/><path d="M${x + 92} 486h30" class="gt-pull-hi"/>`).join("")}</g>
  <g transform="rotate(-4 430 515)"><rect x="372" y="505" width="88" height="20" rx="3" class="gt-sticker"/><text x="416" y="519" text-anchor="middle" class="gt-sticker-t">MORE HEADROOM</text></g>
  <g transform="translate(206 484) rotate(8)"><path d="M0-12C9-12 13-6 11 1C9 7 3 12 0 13C-3 12-9 7-11 1C-13-6-9-12 0-12Z" fill="#2f7f73"/><text y="2.4" text-anchor="middle" class="gt-sticker-p">TONE</text></g>
  <g transform="translate(640 498) rotate(-6)"><circle r="13" fill="#efe3c8"/><circle r="10.6" fill="none" stroke="#b8452f" stroke-width="1"/><path d="M-4 5V-2a4 4 0 0 1 8 0v7z" fill="none" stroke="#b8452f" stroke-width="1.2"/><path d="M-2 4v-4q2-3 4 0v4" stroke="#e3a04b" stroke-width="1" fill="none"/></g>

  <!-- contact shadows -->
  <g class="gt-contact"><ellipse cx="170" cy="418" rx="40" ry="3.6"/><ellipse cx="217" cy="418" rx="14" ry="2.6"/><ellipse cx="258" cy="418" rx="32" ry="3.2"/><ellipse cx="400" cy="418" rx="30" ry="3.2"/><ellipse cx="480" cy="418" rx="18" ry="2.8"/><ellipse cx="592" cy="419" rx="64" ry="4.4"/><ellipse cx="694" cy="418" rx="40" ry="3.4"/></g>

  <!-- parts drawers -->
  <g class="gt-cabinet"><rect x="136" y="336" width="68" height="82" rx="3" fill="#4f5d63" stroke="#2c3538" stroke-width="1"/><rect x="136" y="336" width="68" height="4" rx="2" fill="#7b8b91"/>${drawers()}<rect x="134" y="416" width="72" height="3" rx="1" fill="#2c3538"/></g>
  <!-- tips jar -->
  <g class="gt-tips"><rect x="207" y="391" width="22" height="27" rx="4" fill="url(#gt-g-glass)" stroke="#9fb9bf" stroke-width=".8"/>
    <path d="M210 414l4-8 5 2-3 8zM216 412l5-5 4 4-4 5z" fill="#8fbf7a"/><path d="M211 407h10v4h-10z" fill="#9cc98a" transform="rotate(-12 216 409)"/><circle cx="222" cy="414" r="2.4" fill="url(#gt-g-brass)"/><circle cx="214" cy="415" r="2" fill="#c9ccce"/>
    <rect x="209" y="396" width="18" height="7" rx="1" fill="#f6efe0"/><text x="218" y="401.6" text-anchor="middle" class="gt-tips-t">TIPS</text><path d="M209 391h18" stroke="#fff" stroke-width="1" opacity=".6"/></g>
  <!-- soldering station -->
  <g class="gt-station">
    <rect x="232" y="394" width="50" height="24" rx="3" fill="url(#gt-g-steel)" stroke="#16181a" stroke-width=".8"/>
    <rect x="236" y="399" width="18" height="9" rx="1.2" fill="#1a0806"/><text x="245" y="406.4" text-anchor="middle" class="gt-seg" id="gt-solder-disp">350</text>
    <circle cx="263" cy="406" r="5" fill="#2a2d31" stroke="#6d737a" stroke-width=".8"/><path d="M263 406l2.6-3.6" stroke="#e8e8e8" stroke-width="1.1"/><rect x="237" y="411" width="7" height="4" rx="1" fill="#c0392b"/>
    <rect x="268" y="389" width="16" height="6" rx="1.4" fill="#2a2d31"/><rect x="269.5" y="387.4" width="13" height="4" rx="1" fill="#e9c94a"/>
    <path d="M236 396q-14 4-12 16t20 8" fill="none" stroke="#1a1a1a" stroke-width="2.4" id="gt-iron-cord-rest"/>
    <g id="gt-iron-rest">
      <path d="M277 394l-4-6h12l-4 6z" fill="#3a3d42"/>
      <g fill="none" stroke="#aeb4ba" stroke-width="1.3"><ellipse cx="281" cy="384" rx="5.6" ry="2" transform="rotate(-48 281 384)"/><ellipse cx="285" cy="379" rx="6.4" ry="2.2" transform="rotate(-48 285 379)"/><ellipse cx="289" cy="374" rx="7.2" ry="2.4" transform="rotate(-48 289 374)"/><ellipse cx="293" cy="369" rx="8" ry="2.6" transform="rotate(-48 293 369)"/></g>
      <path d="M291 371l17-19" stroke="#2b5fa0" stroke-width="7.4" stroke-linecap="round"/><path d="M293.6 369l5-5.6M296 366.4l5-5.6" stroke="#1d4372" stroke-width="1"/><path d="M308 352l6-6.6" stroke="#1d1d1d" stroke-width="3.6" stroke-linecap="round"/>
      <path d="M314 345.6q10-8 4-22" fill="none" stroke="#1a1a1a" stroke-width="2.2"/>
    </g>
    <g id="gt-smoke" fill="none" stroke-linecap="round" class="gt-smoke"><path d="M296 364q-5-7 0-13q5-6 0-12" stroke-width="2"/><path d="M299 362q-5-7 0-13q5-6 0-12" stroke-width="1.5"/></g>
  </g>
  <!-- the pedal on the bench -->
  <g class="gt-pedal">
    <path d="M377 404q-14 2-14 10t-30 6" fill="none" stroke="#151515" stroke-width="2.6"/><rect x="371" y="400" width="7" height="6" rx="1" fill="url(#gt-g-chrome)"/>
    <rect x="377" y="384" width="46" height="34" rx="5" fill="#2f7f73" stroke="#1b4a43" stroke-width="1.2"/>
    <path d="M379 386h42" stroke="#9fe0d2" stroke-width="1" opacity=".5" stroke-linecap="round"/><path d="M378 416h44" stroke="#000" stroke-opacity=".25"/>
    <rect x="381" y="388" width="38" height="6" rx="1.4" fill="#ecdfc2"/><text x="400" y="392.8" text-anchor="middle" class="gt-pedal-t">BENCH DRIVE</text>
    <circle cx="388" cy="400" r="4.2" fill="#f1e8d4" stroke="#8c836f" stroke-width=".6"/><path d="M388 400l-2-3.4" stroke="#333" stroke-width="1"/>
    <circle cx="412" cy="400" r="4.2" fill="#f1e8d4" stroke="#8c836f" stroke-width=".6"/><path d="M412 400l2.6-2.8" stroke="#333" stroke-width="1"/>
    <circle cx="400" cy="398" r="1.9" id="gt-led" fill="#5a1a14"/><circle cx="400" cy="398" r="6" id="gt-led-glow" fill="url(#gt-g-glowR)" opacity="0"/>
    <circle cx="400" cy="410" r="4" fill="url(#gt-g-chrome)" stroke="#6a7076" stroke-width=".6"/><circle cx="400" cy="410" r="2.2" fill="#c9ced3"/>
  </g>
  <!-- coffee -->
  <g id="gt-mug-bench">
    <g id="gt-steam" fill="none" stroke-linecap="round" class="gt-steam"><path id="gt-steam1" d="M474 382q-5-7 0-13q5-6 0-12" stroke-width="2.2"/><path id="gt-steam2" d="M482 384q-5-7 0-13q5-6 0-12" stroke-width="2"/><path id="gt-steam3" d="M490 382q-4-6 0-11q4-5 0-10" stroke-width="1.6"/></g>
    <rect x="468" y="389" width="28" height="29" rx="4" fill="url(#gt-g-mug)"/><ellipse cx="482" cy="390" rx="14" ry="2.6" fill="#f1e2cc"/><ellipse cx="482" cy="390.4" rx="11.6" ry="1.8" fill="#4a2a18"/>
    <path d="M496 396q9 0 9 7.6t-9 7.6" fill="none" stroke="#a63f2a" stroke-width="3.6"/><path d="M472 402h20" stroke="#f2e6d4" stroke-width="1.2" opacity=".8"/><path d="M472 406h20" stroke="#f2e6d4" stroke-width=".6" opacity=".6"/>
    <path d="M471 393v20" stroke="#fff" stroke-width="1.6" opacity=".25" stroke-linecap="round"/>
  </g>
  <!-- picks, a capo, string packets -->
  <g><path d="M296 424l4-1 2 4-3 2z" fill="#e8553d"/><path d="M312 427l4-2 2 4-4 1z" fill="#f1c84b"/><path d="M168 426q10-6 22 0l-2 3q-9-4-18 0z" fill="#1d1d1d"/><rect x="186" y="422" width="10" height="5" rx="1.6" fill="#2b2b2b"/><path d="M170 425h14" stroke="#555" stroke-width=".7"/></g>
  <g transform="translate(596 421) rotate(-3)"><rect width="28" height="11" rx="1" fill="#efe3c8"/><rect width="28" height="3" fill="#b8452f"/><path d="M4 7h18M4 9h12" stroke="#7a6a50" stroke-width=".6"/></g>
  <g transform="translate(618 423) rotate(5)"><rect width="26" height="10" rx="1" fill="#e6ecf0"/><rect width="26" height="3" fill="#2f6fae"/><path d="M4 6h16M4 8h10" stroke="#5a6a7a" stroke-width=".6"/></g>

  <!-- the radio -->
  <g class="gt-radio">
    <ellipse id="gt-radio-glow" cx="708" cy="384" rx="38" ry="30" fill="url(#gt-g-glowA)" opacity=".7"/>
    <path d="M660 418V378C660 366 670 360 693 360C716 360 726 366 726 378V418Z" fill="url(#gt-g-radio)" stroke="#3a1e0c" stroke-width="1"/>
    <path d="M660 418V378C660 366 670 360 693 360C716 360 726 366 726 378V418Z" fill="url(#gt-p-grain)" opacity=".25"/>
    <path d="M664 377C665 369 673 365 693 365C713 365 721 369 722 377" fill="none" stroke="#c98a52" stroke-width="1.2" opacity=".6"/>
    <rect x="665" y="372" width="26" height="40" rx="5" fill="url(#gt-p-slats)" stroke="#2a160a" stroke-width=".8"/>
    <rect x="696" y="372" width="25" height="22" rx="3" fill="url(#gt-g-dial)" stroke="#5a3016" stroke-width="1"/>
    <path d="M699 388h19M700 384v4M704 385v3M708 384v4M712 385v3M716 384v4" stroke="#7a3a10" stroke-width=".6"/>
    <path d="M700 380q8.6-5 17 0" fill="none" stroke="#7a3a10" stroke-width=".5"/><path id="gt-radio-needle" d="M707 377v14" stroke="#c0392b" stroke-width="1"/>
    <path d="M697 373h12" stroke="#fff" stroke-width="1" opacity=".45" stroke-linecap="round"/>
    <circle cx="702" cy="404" r="4.6" fill="#3a2010" stroke="#c98a52" stroke-width=".6"/><circle cx="715" cy="404" r="4.6" fill="#3a2010" stroke="#c98a52" stroke-width=".6"/><path d="M702 404v-3.6M715 404l2.6-2.4" stroke="#e9c28a" stroke-width=".9"/>
    <rect x="664" y="416" width="8" height="3" rx="1" fill="#2a160a"/><rect x="714" y="416" width="8" height="3" rx="1" fill="#2a160a"/>
  </g>

  <!-- Fuse, the shop cat -->
  <g id="gt-cat" class="gt-cat">
    <g id="gt-cat-body">
      <path d="M546 418C540 404 548 386 572 380C600 374 630 380 642 394C650 404 649 414 644 418Z" fill="url(#gt-g-catfur)"/>
      <path d="M578 381q5 11 1 24M594 378q5 13 1 27M610 378q5 12 1 26M625 383q4 10 0 22M638 391q3 8 0 16" stroke="${K.catD}" stroke-width="3.2" fill="none" stroke-linecap="round" opacity=".85"/>
      <ellipse cx="626" cy="405" rx="17" ry="12" fill="${K.cat}" stroke="${K.catD}" stroke-width="1"/><path d="M616 400q8-4 18 2M614 407q9-3 20 2" stroke="${K.catD}" stroke-width="2" fill="none" stroke-linecap="round"/>
      <path d="M552 412q30 6 80 2" stroke="${K.catL}" stroke-width="5" fill="none" opacity=".55" stroke-linecap="round"/>
      <path d="M560 388q20-10 50-8" stroke="#fff" stroke-width="2" fill="none" opacity=".18" stroke-linecap="round"/>
    </g>
    <g id="gt-cat-paws"><ellipse cx="549" cy="416.4" rx="9" ry="4.4" fill="${K.cream}"/><ellipse cx="563" cy="417.4" rx="8.4" ry="4" fill="${K.cream}"/><path d="M543 418v-2.6M547 419v-3M552 418.6v-2.6M558 419.6v-2.4M562 420v-2.8" stroke="#c9a888" stroke-width=".7"/></g>
    <g id="gt-cat-head">
      <g id="gt-cat-earL"><path d="M526 385L521 367L536 377Z" fill="${K.cat}" stroke="${K.catD}" stroke-width=".8"/><path d="M527 382L524 371L533 377Z" fill="#f2a9a0"/><path d="M526 374l-3-4M528 376l-2-5" stroke="#fff" stroke-width=".5" opacity=".7"/></g>
      <g id="gt-cat-earR"><path d="M545 376L556 365L557 384Z" fill="${K.cat}" stroke="${K.catD}" stroke-width=".8"/><path d="M547 377L554 369L554.6 381Z" fill="#f2a9a0"/></g>
      <path d="M523 392C523 381 531 375 541 375C551 375 558 381 558 391C560 395 559 401 554 405C548 410 534 410 528 406C522 402 520 397 523 392Z" fill="url(#gt-g-catfur)" stroke="${K.catD}" stroke-width=".6"/>
      <path d="M533 379q4 4 2 8M540 377.6v7M547 379q-4 4-2 8" stroke="${K.catD}" stroke-width="1.6" fill="none" stroke-linecap="round"/>
      <path d="M522 398q-4 2-4 6M524 402q-3 3-2 6M556 400q4 2 4 6" stroke="${K.cat}" stroke-width="2" fill="none" stroke-linecap="round"/>
      <ellipse cx="537" cy="401" rx="8.6" ry="5.6" fill="${K.cream}"/>
      <g id="gt-cat-closed" fill="none" stroke="#3a2210" stroke-width="1.4" stroke-linecap="round"><path d="M527.6 390.6q3 2.6 6.4 0"/><path d="M541 389.6q3 2.6 6.4 0"/></g>
      <g id="gt-cat-open" opacity="0"><ellipse cx="530.8" cy="390.6" rx="3.6" ry="2.8" fill="#b9d36a"/><ellipse cx="544.2" cy="389.6" rx="3.6" ry="2.8" fill="#b9d36a"/><ellipse cx="531.4" cy="390.6" rx=".9" ry="2.4" fill="#1a1208"/><ellipse cx="544.8" cy="389.6" rx=".9" ry="2.4" fill="#1a1208"/><circle cx="532.4" cy="389.4" r=".6" fill="#fff"/><circle cx="545.8" cy="388.4" r=".6" fill="#fff"/>
        <g id="gt-cat-lids"><path d="M526.6 390.6a4.2 3.4 0 0 1 8.4 0z" fill="${K.cat}"/><path d="M540 389.6a4.2 3.4 0 0 1 8.4 0z" fill="${K.cat}"/></g></g>
      <path d="M534.4 397.4h4.4l-2.2 2.6z" fill="#d9776e"/>
      <path d="M536.6 400v1.6M533 402q1.8 1.8 3.6 0q1.8 1.8 3.6 0" stroke="#7a4a30" stroke-width=".8" fill="none" stroke-linecap="round"/>
      <g id="gt-cat-yawn" transform="translate(536.6 403.4) scale(0)"><ellipse rx="4.4" ry="5" fill="#5a1a16"/><ellipse cy="2.6" rx="3" ry="2" fill="#e27d82"/><path d="M-2.6-3.4l.8 2M2.6-3.4l-.8 2" stroke="#fff" stroke-width="1"/></g>
      <path d="M529 400l-14-2M529 402l-13 2.4M529.6 404l-11 5M544 400l12-3M544 402l12 1" stroke="#fff6ea" stroke-width=".55" opacity=".85"/>
      <g id="gt-purr" opacity="0" fill="none" stroke="#f4ead8" stroke-width="1" stroke-linecap="round"><path d="M520 410q-3 2 0 4q3 2 0 4"/><path d="M515 408q-3 2 0 4q3 2 0 4"/></g>
    </g>
    <path id="gt-cat-tail" d="M641 412C657 418 653 428 631 428C613 428 591 427 577 424" fill="none" stroke="${K.cat}" stroke-width="8" stroke-linecap="round"/>
    <path d="M641 412C657 418 653 428 631 428C613 428 591 427 577 424" fill="none" stroke="${K.catD}" stroke-width="8" stroke-dasharray="3 7" opacity=".7"/>
    <g id="gt-cat-tailtip"><path d="M579 424C572 423 568 421 564 417.6" fill="none" stroke="${K.catDD}" stroke-width="7.4" stroke-linecap="round"/></g>
    <g class="gt-zzz"><text id="gt-z1" x="526" y="366" class="z">z</text><text id="gt-z2" x="516" y="352" class="z">z</text><text id="gt-z3" x="504" y="336" class="z">Z</text></g>
  </g>`;

  /* ---------------------------------------------------------------- ARMS: two IK arms, the hands' poses,
     held props, the laptop. Hands are drawn wrist-at-origin, fingers toward +x, thumb toward −y. */
  const sk = (d, w, fill) => `<path d="${d}" stroke="#8a4f38" stroke-width="${w + 1.4}" stroke-linecap="round" fill="none"/><path d="${d}" stroke="${fill || K.skin}" stroke-width="${w}" stroke-linecap="round" fill="none"/>`;
  const PALM = `<path d="M-1-7.6C6-8.8 13-8.4 16.4-6C18.6-3 18.6 3 16.4 6C13 8.4 6 8.8-1 7.6Z" fill="${K.skin}" stroke="#8a4f38" stroke-width=".9"/>`;
  const FIST = `<path d="M-1-7.8C7-9.2 15-8.8 18.4-5.4C20.6-2.2 20.6 4 17.8 7C14 9.4 6 9.2-1 7.8Z" fill="${K.skin}" stroke="#8a4f38" stroke-width=".9"/><path d="M15.6-5.6q3.4 1 3.6 3M16.6-1.4q3.2 1 3 3.2M16.2 2.8q2.8 1 2.4 3.2" stroke="#b06a50" stroke-width=".9" fill="none" stroke-linecap="round"/>`;
  const KNUCK = `<path d="M10-5.2q1.4-.8 2.8 0M11-1.4q1.4-.8 2.8 0M10.6 2.4q1.4-.8 2.8 0" stroke="#c48468" stroke-width=".7" fill="none" stroke-linecap="round"/>`;
  const HS = 1.25;   // hand scale (props in hands are drawn at 1/HS)
  const HAND_POSES = {
    rest: `${sk("M15.4-5.4q5-.8 8.4 1.2M16.4-1.8q5.6 0 8.8 1.8M16.2 1.8q5 .6 8 2.4M15 5q3.8 1.2 6 2.8", 4)}${PALM}${KNUCK}${sk("M4-7q6-4 11.4-4.4", 4.4)}`,
    grip: `${FIST}${sk("M5-8.4q7-2.6 12.6 1.2", 4.4)}`,
    point: `${FIST}${sk("M16-4.6L30-5.4", 4.2)}<path d="M28.6-6.6q1.6 1.2 0 2.4" stroke="#f3d0b8" stroke-width="1" fill="none"/>${sk("M5-8.4q7-2.6 12-0.6", 4.4)}`,
    open: `${sk("M15-6L27-10.6M16.4-2L30-3.4M16.4 2L29 4.6M15 5.6L25 10.4", 4)}${PALM}<path d="M4 0q6 2 10 0M5 3.6q5 1.4 9-.6" stroke="#c48468" stroke-width=".7" fill="none"/>${sk("M4.6-7L10-16", 4.4)}`,
    count1: `${sk("M15.6-5L30-6.4", 4)}${sk("M16.6-1.4q3.4.6 3 3.4M16.4 2.6q3 .8 2.4 3.4M15 5.8q2.4 .8 1.8 3", 4)}${PALM}${sk("M5-7.4q6 4 10 6", 4.4)}`,
    count2: `${sk("M15.6-5L29.4-8.4M16.6-1.4L31-1.4", 4)}${sk("M16.4 2.6q3 .8 2.4 3.4M15 5.8q2.4 .8 1.8 3", 4)}${PALM}${sk("M5-7.4q6 4 10 6", 4.4)}`,
    count3: `${sk("M15.6-5L29-9.6M16.6-1.4L31-2.2M16.4 2.6L29.4 5", 4)}${sk("M15 5.8q2.4.8 1.8 3", 4)}${PALM}${sk("M5-7.4q6 4 10 6", 4.4)}`,
    thumb: `<path d="M-1-8C6-9.6 12-9.4 14-7.6L14 9.4C10 10.6 4 10.4-1 8.6Z" fill="${K.skin}" stroke="#8a4f38" stroke-width=".9"/>
      <g fill="${K.skin}" stroke="#8a4f38" stroke-width=".8"><rect x="11" y="-7.8" width="9.6" height="4.6" rx="2.3"/><rect x="11.6" y="-3.4" width="9.4" height="4.6" rx="2.3"/><rect x="11.4" y="1" width="9" height="4.4" rx="2.2"/><rect x="10.6" y="5.2" width="8.2" height="4" rx="2"/></g>
      <path d="M17.6-6.4h2M18-2h2M17.6 2.4h2" stroke="#c48468" stroke-width=".6"/>
      ${sk("M5.2-7.4C4.6-12 4.8-16.6 6.8-21.4", 5.8)}<path d="M5.4-20.6q1.6-2 3-.2" stroke="#f6dcc6" stroke-width="1.2" fill="none" stroke-linecap="round"/><path d="M4-11.6q2.6-.6 4.6.4" stroke="#c48468" stroke-width=".6" fill="none"/>`,
    pinch: `${sk("M16-1.4q4 0 5.6 2.4M15.4 2.6q3.4.6 3.6 3.2", 4)}${PALM}${sk("M15.6-5.2Q22-7 24-3.6", 4)}${sk("M5-7.6Q16-12 23.4-4.2", 4.2)}`,
    mug: `<g transform="scale(.8)"><rect x="9" y="-22" width="26" height="28" rx="4" fill="url(#gt-g-mug)"/><ellipse cx="22" cy="-21.4" rx="13" ry="2.4" fill="#f1e2cc"/><path d="M13 -10h18" stroke="#f2e6d4" stroke-width="1.2" opacity=".8"/><path d="M9-16q-8 0-8 7t8 7" fill="none" stroke="#a63f2a" stroke-width="3.4"/></g>${FIST}${sk("M5-8.4q7-2.6 12.6 1.2", 4.4)}`,
    iron: `<g transform="scale(.8)"><path d="M14 0H44" stroke="#2b5fa0" stroke-width="7.4" stroke-linecap="round"/><path d="M44 0h9" stroke="#1d1d1d" stroke-width="3.4"/><path d="M53 0h9" stroke="#c9ccce" stroke-width="1.8"/><path d="M62 0l4 .4" stroke="#8a8f94" stroke-width="1.2" stroke-linecap="round"/><path d="M-6 3q-10 4-14 12" stroke="#1a1a1a" stroke-width="2.2" fill="none"/></g>${FIST}${sk("M5-8.4q7-2.6 12.6 1.2", 4.4)}`,
    watch: `${sk("M15-6q6-3 9-1M16.4-2q6-1 9 1M16.4 2q5 0 8 2.6", 4)}${PALM}<g transform="translate(15 -1)"><circle r="8.6" fill="url(#gt-g-brass)" stroke="#7a5a18" stroke-width=".8"/><circle r="6.6" fill="#f8f2e2"/><path d="M0-5.6v1.4M5.6 0h-1.4M0 5.6v-1.4M-5.6 0h1.4" stroke="#5a4a36" stroke-width=".7"/><path d="M0 0V-4.4M0 0l3 1.6" stroke="#2a2018" stroke-width=".9" stroke-linecap="round"/><path d="M-4-4.4a6.6 6.6 0 0 1 6-2" stroke="#fff" stroke-width="1" fill="none" opacity=".7"/><rect x="-1.6" y="-11.4" width="3.2" height="3" rx="1" fill="url(#gt-g-brass)"/></g>${sk("M4.6-7q3-3 6-3.6", 4.4)}`,
    rag: `<path d="M8 2q-2 10 2 22q6 4 12 0q2-12-4-22z" fill="url(#gt-p-rag)"/><path d="M8 2q-2 10 2 22" stroke="#7a1f16" stroke-width=".6" fill="none"/>${FIST}${sk("M5-8.4q7-2.6 12.6 1.2", 4.4)}`,
    knob: `${sk("M16-1.4q4 0 5.6 2.4M15.4 2.6q3.4.6 3.6 3.2", 4)}${PALM}<g transform="translate(26 -4.6)"><circle r="5.2" fill="#1d1a18"/><path d="M0 0L0-7" stroke="#1d1a18" stroke-width="3.4" stroke-linecap="round"/><path d="M0-1v-5" stroke="#efe6d2" stroke-width=".8"/><path d="M-3-2a4 4 0 0 1 4-2" stroke="#fff" stroke-width=".9" fill="none" opacity=".6"/><circle id="gt-knob-shine" cx="-2" cy="-3" r="2.6" fill="#fff" opacity="0"/></g>${sk("M15.6-5.2Q21-8.4 22.4-6M5-7.6Q14-12 21.4-7.2", 4.2)}`,
  };
  const HAND = (side) => `<g id="gt-hand-${side}"><g class="gt-hand-in" transform="scale(${HS} ${side === "L" ? -HS : HS})">${Object.keys(HAND_POSES).map((k) => `<g data-pose="${k}"${k === "rest" ? "" : ' style="display:none"'}>${HAND_POSES[k]}</g>`).join("")}</g></g>`;
  const ARM = (side) => `
  <g id="gt-arm-${side}">
    <g id="gt-ua-${side}"><path d="M-4-17C18-18 40-15.6 60-13.4Q67 0 60 13.4C40 15.6 18 18-4 17Q-17 0-4-17Z" fill="url(#gt-p-plaid)"/><path d="M-4-17C18-18 40-15.6 60-13.4Q67 0 60 13.4C40 15.6 18 18-4 17Q-17 0-4-17Z" fill="url(#gt-g-sleeve)"/><path d="M-4-17C18-18 40-15.6 60-13.4Q67 0 60 13.4C40 15.6 18 18-4 17Q-17 0-4-17Z" fill="none" stroke="#2a0b09" stroke-width="1.2" opacity=".55"/><path d="M18-11q5 10 0 21M34-10q4 9 0 18M46-9q-3 8 1 16" stroke="#1d0706" stroke-width="1.1" fill="none" opacity=".35" stroke-linecap="round"/></g>
    <g id="gt-fa-${side}"><path d="M0-12.6C20-13 40-10.6 56-8.6Q61.6 0 56 8.6C40 10.6 20 13 0 12.6Z" fill="url(#gt-g-limb)" stroke="#9a5a40" stroke-width=".8"/><path d="M14-5.4l3-1.2M22-6l3-1M30-5l3-1M18-1.6l3-.8M27-2l3-.8M38-3l2.6-.8M44-5.4l2.4-.6" stroke="#9a6a50" stroke-width=".6" opacity=".45" stroke-linecap="round"/><path d="M50-6q2 6 0 12" stroke="#b97858" stroke-width=".8" fill="none" opacity=".5"/></g>
    <g id="gt-cuff-${side}"><rect x="-8.4" y="-16.4" width="16.8" height="32.8" rx="6" fill="url(#gt-p-plaid)"/><rect x="-8.4" y="-16.4" width="16.8" height="32.8" rx="6" fill="none" stroke="#2a0b09" stroke-width="1.1" opacity=".6"/><path d="M-6-12v24M6-12v24" stroke="#e7c98e" stroke-width="1" opacity=".35"/><path d="M0-14v28" stroke="#1d0706" stroke-width="1.2" opacity=".35"/></g>
    ${HAND(side)}
  </g>`;
  const ARMS = `
  <defs><clipPath id="gt-c-arms"><rect x="-900" y="-900" width="2800" height="1328"/></clipPath></defs>
  <path id="gt-iron-cable" d="M236 396" fill="none" stroke="#1a1a1a" stroke-width="2.2" opacity="0"/>
  <path id="gt-watch-chain" d="M400 400" fill="none" stroke="url(#gt-g-brass)" stroke-width="1.2" stroke-dasharray="1.6 .8" opacity="0"/>
  <g clip-path="url(#gt-c-arms)">${ARM("R")}${ARM("L")}</g>
<g id="gt-motes" class="gt-motes">${Array.from({ length: 10 }, (_, i) => `<circle r="${(0.8 + (i % 3) * 0.35).toFixed(2)}" cx="0" cy="0"/>`).join("")}</g>
  <g id="gt-sparks" opacity="0"><path d="M0 0l-6-5M0 0l5-6M0 0l7 1M0 0l-4 6M0 0l2-8" stroke="#ffe7a0" stroke-width="1" stroke-linecap="round"/><circle r="2.2" fill="#fff6d8"/><circle r="7" fill="url(#gt-g-glowA)"/></g>
  <g id="gt-smoke2" opacity="0" fill="none" stroke-linecap="round" class="gt-smoke"><path d="M0 0q-5-7 0-13q5-6 0-12" stroke-width="2"/><path d="M3 -2q-5-7 0-13q5-6 0-12" stroke-width="1.4"/></g>
  <g id="gt-laptop" aria-hidden="true" transform="translate(0 419) scale(1 .04) translate(0 -419)" opacity="0">
    <path d="M336 420h128l-7-8H343z" fill="#80868e"/><path d="M336 420h128" stroke="#4a4f55" stroke-width="1.4"/>
    <rect x="350" y="343" width="100" height="70" rx="6" fill="url(#gt-g-lid)" stroke="#6d737b" stroke-width="1.2"/>
    <path d="M355 348h90" stroke="#fff" stroke-opacity=".4" stroke-width="1.2" stroke-linecap="round"/>
    <g><circle id="gt-lid-halo" cx="400" cy="377" r="14" fill="url(#gt-g-tubeglow)" opacity=".3"/>
      <path d="M393.5 389v-13.5a6.5 6.5 0 0 1 13 0V389z" fill="#e9eef3" fill-opacity=".55" stroke="#5f656c" stroke-width="1.1"/>
      <path d="M397 386v-7q3-4 6 0v7" fill="none" stroke="#d9772e" stroke-width="1.2" stroke-linecap="round"/><rect x="392" y="389" width="16" height="4.5" rx="1" fill="#5f656c"/></g>
    <g transform="rotate(-12 432 400)"><path d="M425 395q7-4 14 0q-1 8-7 11q-6-3-7-11z" fill="#e7b33f" stroke="#a87b1d" stroke-width=".7"/></g>
    <circle cx="364" cy="401" r="5.2" fill="#2f7f73"/><text x="364" y="403" text-anchor="middle" class="gt-lap-t">HR</text>
    <rect x="370" y="352" width="18" height="10" rx="2" fill="#b8452f" transform="rotate(6 379 357)"/><path d="M372 357h14" stroke="#f2e6d4" stroke-width="1" transform="rotate(6 379 357)"/>
  </g>`;

  /* ---------------------------------------------------------------- LIGHT: the lamp, its light, the sun,
     foreground and vignette */
  // masks, filters and clips are defined in the layer that uses them (WebKit won't resolve them across <svg>s)
  const LIGHT = `
  <defs>
    <linearGradient id="gt-g-conex" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#fff" stop-opacity="0"/><stop offset=".3" stop-color="#fff" stop-opacity="1"/><stop offset=".7" stop-color="#fff" stop-opacity="1"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
    <mask id="gt-m-cone"><rect x="150" y="100" width="560" height="470" fill="url(#gt-g-conex)"/></mask>
    <filter id="gt-f-fg" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="5"/></filter>
  </defs>
  <g class="gt-lightfx">
    <g class="gt-cone"><polygon id="gt-cone" points="414,104 462,104 700,560 160,560" fill="url(#gt-g-cone)" mask="url(#gt-m-cone)"/></g>
    <rect x="-900" y="380" width="2800" height="80" fill="url(#gt-g-benchpool)" class="gt-benchpool"/>
    <g class="gt-day"><polygon points="600,64 716,64 520,560 170,560" fill="url(#gt-g-beam)" class="gt-beam"/><polygon points="616,64 664,64 430,560 300,560" fill="url(#gt-g-beam)" class="gt-beam"/></g>
  </g>
  <g class="gt-lamp">
    <path d="M438 -900V74" class="gt-cord"/>
    <circle id="gt-bulbglow" cx="438" cy="108" r="44" fill="url(#gt-g-bulb)"/>
    <path d="M433 70h10v6h-10z" fill="url(#gt-g-brass)"/>
    <path d="M409 104C411 87 423 76 438 76C453 76 465 87 467 104Z" fill="url(#gt-g-shade)"/>
    <path d="M418 98C420 88 428 81 438 80" stroke="#fff" stroke-width="2" opacity=".28" fill="none" stroke-linecap="round"/>
    <path d="M400 104.6C414 99.6 462 99.6 476 104.6L476 107C462 103.4 414 103.4 400 107Z" fill="#244236"/>
    <ellipse cx="438" cy="106" rx="36" ry="2.6" fill="#f4ecd8"/>
    <ellipse id="gt-bulb" cx="438" cy="107" rx="12" ry="4.6" fill="#fff4cf"/>
  </g>
  <g class="gt-fg">
    <g filter="url(#gt-f-fg)" class="gt-fg-l"><path d="M92 560l18-130h22l10 130z" fill="#1c120c"/><path d="M104 440l-8-70q-2-12 10-14h16q12 2 10 14l-8 70z" fill="#2a1a10"/><g fill="#5a4a3a"><circle cx="100" cy="372" r="3.6"/><circle cx="100" cy="388" r="3.6"/><circle cx="136" cy="372" r="3.6"/><circle cx="136" cy="388" r="3.6"/></g></g>
    <g filter="url(#gt-f-fg)" class="gt-fg-r"><path d="M760 560c-6-40-2-90 14-150c4 50 4 100-4 150zM784 560c4-50 16-96 40-136c-12 46-18 90-24 136zM742 560c-14-34-26-70-28-120c18 40 30 80 36 120z" fill="#24361f"/><path d="M736 560l6-36h52l6 36z" fill="#3a1e10"/></g>
  </g>
  <rect class="gt-vigrect" x="0" y="0" width="800" height="560" fill="url(#gt-g-vig)" pointer-events="none"/>`;

  function markup(name) {
    const svg = (cls, body, label) => `<svg class="gt-svg ${cls}" viewBox="0 0 800 560" preserveAspectRatio="xMidYMax meet"${label ? ` role="img" aria-label="${label}"` : ' aria-hidden="true"'}>${body}</svg>`;
    return svg("gt-l-back", DEFS + BACK, `${name}, the guitar tech, behind his workbench in a cozy guitar shop`) +
      svg("gt-l-bfx", BFX) + svg("gt-l-char", CHAR) + svg("gt-l-bench", BENCH) + svg("gt-l-arms", ARMS) + svg("gt-l-light", LIGHT);
  }

  /* =====================================================================
     THE RIG
     ===================================================================== */
  const L1 = 58, L2 = 56;                     // upper arm, forearm
  const SHOULDER = { R: [348, 322], L: [452, 322] };
  const REST = { R: [356, 410, 18, "rest", -24, 60], L: [444, 410, 162, "rest", 24, 60] };   // [x, y, hand angle, pose, elbow hint dx, dy from the shoulder]
  // Face/body channels: spring frequency (critically damped)
  const CH = {
    lean: 5, leanX: 5, bob: 8, shrug: 9, hRot: 7, hX: 7, hY: 8, wisp: 4,
    browL: 15, browR: 15, worryL: 12, worryR: 12, furrow: 10, lidU: 20, lidD: 12, smile: 9, purse: 11, cheek: 9,
    gazeX: 30, gazeY: 30, glassY: 10, mo: 32, mw: 26, mr: 24, mt: 28, mtg: 24, press: 30, tuck: 30,
    thought: 7, laptop: 9, screen: 8,
  };
  const NEUTRAL = { lean: 0, leanX: 0, bob: 0, shrug: 0, hRot: 0, hX: 0, hY: 0, browL: 0.05, browR: 0, worryL: 0, worryR: 0, furrow: 0, lidU: 0.1, lidD: 0.05, smile: 0.16, purse: 0, cheek: 0, glassY: 0, thought: 0, laptop: 0, screen: 0 };
  const BASE = {
    idle: {},
    listening: { lean: 2.4, leanX: 5, hRot: 4.5, hX: 2, browL: 0.32, browR: 0.24, smile: 0.28, lidU: 0.1 },
    thinking: { hRot: -5, hX: -2, hY: -1, browL: 0.7, browR: -0.18, worryR: -0.2, furrow: 0.22, purse: 0.9, smile: 0, lidU: 0.24, thought: 1 },
    talking: { smile: 0.24, lidU: 0.12, browL: 0.12, browR: 0.08 },
    searching: { bob: 4, hY: 5, hRot: -1.5, furrow: 0.55, lidU: 0.38, lidD: 0.42, smile: 0, glassY: 4.4, browL: -0.22, browR: -0.12, laptop: 1, screen: 1 },
  };
  const VIS = {
    rest: { mo: 0.02, mw: 1, mr: 0, mt: 0, mtg: 0, press: 0, tuck: 0 },
    A: { mo: 0.92, mw: 1.0, mr: 0.1, mt: 0.6, mtg: 0.35, press: 0, tuck: 0 },
    E: { mo: 0.52, mw: 1.2, mr: 0, mt: 1, mtg: 0.2, press: 0, tuck: 0 },
    I: { mo: 0.34, mw: 1.16, mr: 0, mt: 1, mtg: 0.1, press: 0, tuck: 0 },
    O: { mo: 0.7, mw: 0.7, mr: 0.95, mt: 0.2, mtg: 0.3, press: 0, tuck: 0 },
    U: { mo: 0.3, mw: 0.54, mr: 1, mt: 0, mtg: 0, press: 0, tuck: 0 },
    M: { mo: 0, mw: 0.94, mr: 0, mt: 0, mtg: 0, press: 1, tuck: 0 },
    F: { mo: 0.17, mw: 1.02, mr: 0, mt: 1, mtg: 0, press: 0, tuck: 1 },
    L: { mo: 0.42, mw: 0.98, mr: 0, mt: 0.7, mtg: 1, press: 0, tuck: 0 },
    S: { mo: 0.17, mw: 1.1, mr: 0, mt: 1, mtg: 0, press: 0, tuck: 0 },
  };
  function visOf(c) {
    c = c.toLowerCase();
    if (c === "a") return "A"; if (c === "e") return "E"; if (c === "i" || c === "y") return "I"; if (c === "o") return "O";
    if ("uwq".includes(c)) return "U"; if ("mbp".includes(c)) return "M"; if ("fv".includes(c)) return "F";
    if ("ltdn".includes(c)) return "L"; if ("szcxjkgrh".includes(c)) return "S";
    return /[0-9]/.test(c) ? "E" : null;
  }
  const VIS_PRI = { M: 3, A: 2, E: 2, I: 2, O: 2, U: 2, F: 1.5, L: 1, S: 1 };
  const rotP = (x, y, a) => { const c = Math.cos(a), s = Math.sin(a); return [x * c - y * s, x * s + y * c]; };
  const DEG = Math.PI / 180;
  const tri = (u) => (u < 0 || u > 1 ? 0 : u < 0.4 ? u / 0.4 : 1 - (u - 0.4) / 0.6);
  const bump = (t, a, b, r) => ss((t - a) / (r || 0.35)) * ss((b - t) / (r || 0.35));   // 0→1 over [a,b] with soft edges

  /* ---- actions: short clips layered over the base pose. arms: { R|L: [[t, x, y, angle, pose], …] } (x=null:
     the base pose); face/head/body: (t, A) → channel targets, blended in by the clip's envelope; gaze: (t, A) →
     a point or a name; ev: [[t, fn]] fired once; mod: per-frame tweaks (wiggles) */
  const ACTIONS = {
    nod: { dur: 1.1, override: true, gaze: () => "chat", face: () => ({ smile: 0.36, browL: 0.32, browR: 0.28 }),
      add: (t) => ({ hY: 3.4 * (bump(t, 0.05, 0.4, 0.15) * 0.9 + bump(t, 0.5, 0.85, 0.15) * 0.6), hRot: -1.2 * bump(t, 0.05, 0.85, 0.2) }) },
    approved: { dur: 3, override: true, arms: { L: [[0, null], [0.45, 476, 332, 180, "thumb"], [2.3, 476, 332, 180, "thumb"], [3, null]] }, gaze: () => "user",
      face: () => ({ smile: 1, cheek: 1, lidD: 0.62, lidU: 0.08, browL: 0.55, browR: 0.5 }),
      mouth: (t) => (t > 0.2 && t < 2.6 ? { mo: 0.36, mw: 1.28, mr: 0, mt: 1, mtg: 0.2, press: 0, tuck: 0 } : null),
      add: (t) => ({ hRot: 5 * bump(t, 0.2, 2.6), bob: -3 * bump(t, 0.25, 0.75, 0.2), hY: -1.5 * bump(t, 0.25, 0.75, 0.2) }), after: (R) => R.mood(0.55, 7) },
    declined: { dur: 2, override: true, arms: { R: [[0, null], [0.42, 340, 370, -40, "open"], [1.35, 340, 370, -40, "open"], [2, null]] }, gaze: () => "user",
      face: () => ({ smile: 0.14, browL: 0.4, browR: 0.36, worryL: 0.3, worryR: 0.3 }), add: (t) => ({ hRot: -3 * bump(t, 0.2, 1.6), shrug: 2.6 * bump(t, 0.3, 1.4) }) },
    shrug: { dur: 2.6, override: true, arms: { R: [[0, null], [0.45, 310, 350, 210, "open"], [1.95, 310, 350, 210, "open"], [2.6, null]], L: [[0, null], [0.45, 490, 350, -30, "open"], [1.95, 490, 350, -30, "open"], [2.6, null]] },
      gaze: () => "user", face: () => ({ browL: 0.6, browR: 0.55, worryL: 1, worryR: 1, smile: -0.5, lidU: 0.22, purse: 0.15 }),
      add: (t) => ({ hRot: 6.5 * bump(t, 0.3, 2.2), hY: 2 * bump(t, 0.3, 2.2), shrug: 6.5 * bump(t, 0.35, 1.9, 0.3) }) },
    present: { dur: 2.1, arms: { L: "point" }, gaze: () => "chat", face: () => ({ browL: 0.48, browR: 0.46, smile: 0.4 }), add: (t) => ({ hRot: 3 * bump(t, 0.2, 1.8), hX: 2 * bump(t, 0.2, 1.8) }) },
    surprise: { dur: 1.7, gaze: () => "cat", face: (t) => (t < 0.7 ? { browL: 1, browR: 0.95, lidU: 0, smile: 0 } : { smile: 0.55, browL: 0.4, browR: 0.4, lidD: 0.3 }),
      mouth: (t) => (t > 0.08 && t < 0.62 ? { mo: 0.34, mw: 0.66, mr: 0.95, mt: 0.1, mtg: 0.1 } : null), add: (t) => ({ hY: -2 * bump(t, 0.05, 0.6, 0.12), hRot: 3 * bump(t, 0.1, 1.4) }) },
    // talking gestures
    palmL: { dur: 1.9, arms: { L: [[0, null], [0.45, 458, 368, -125, "open"], [1.3, 455, 364, -116, "open"], [1.9, null]] } },
    palmR: { dur: 1.9, arms: { R: [[0, null], [0.45, 342, 368, -55, "open"], [1.3, 345, 364, -64, "open"], [1.9, null]] } },
    both: { dur: 2.1, arms: { R: [[0, null], [0.5, 350, 374, -58, "open"], [1.4, 354, 370, -66, "open"], [2.1, null]], L: [[0, null], [0.5, 450, 374, -122, "open"], [1.4, 446, 370, -114, "open"], [2.1, null]] } },
    pointChat: { dur: 1.8, arms: { L: "point" }, gaze: (t) => (t < 1.3 ? "chat" : null), face: () => ({ browL: 0.35, browR: 0.3 }) },
    count: { dur: 1.7, arms: { L: "count" }, face: () => ({ browL: 0.25, browR: 0.25 }) },
    adjust: { dur: 1.8, arms: { R: [[0, null], [0.55, 401, 244, -82, "point"], [0.85, 401, 239, -82, "point"], [1.15, 401, 242, -82, "point"], [1.8, null]] }, gaze: () => "user",
      face: () => ({ lidU: 0.24 }), add: (t) => ({ glassY: 1.6 * bump(t, 0.3, 0.75, 0.15) - 1.4 * bump(t, 0.8, 1.15, 0.12), hY: -1 * bump(t, 0.5, 1.2) }) },
    wipe: { dur: 2.7, arms: { R: [[0, null], [0.45, 372, 384, 75, "open"], [0.85, 374, 404, 80, "open"], [1.25, 372, 384, 75, "open"], [1.65, 374, 404, 80, "open"], [2.7, null]],
      L: [[0, null], [0.5, 428, 384, 105, "open"], [0.9, 426, 404, 100, "open"], [1.3, 428, 384, 105, "open"], [1.7, 426, 404, 100, "open"], [2.7, null]] },
      gaze: (t) => (t < 1.9 ? [400, 420] : "user"), face: (t) => (t < 1.9 ? { lidU: 0.3 } : { smile: 0.35 }), add: (t) => ({ hY: 2.4 * bump(t, 0.2, 1.9) }) },
    // personality idles
    sip: { dur: 5.2, idle: true,
      arms: { L: [[0, null], [0.85, 504, 412, 180, "grip"], [0.95, 504, 412, 180, "mug"], [1.85, 434, 310, 178, "mug"], [2.25, 430, 304, 160, "mug"], [3.1, 430, 304, 160, "mug"], [3.5, 436, 312, 178, "mug"], [4.3, 504, 412, 180, "mug"], [4.4, 504, 412, 180, "grip"], [5.2, null]] },
      ev: [[0.95, (R) => R.prop("mug", false)], [4.4, (R) => R.prop("mug", true)]],
      gaze: (t, A, R) => (t < 1.9 ? R.handPt("L") : t < 3.3 ? "user" : t < 4.3 ? R.handPt("L") : "user"),
      face: (t) => (t > 2 && t < 3.15 ? { lidU: 0.9, browL: 0.42, browR: 0.38, purse: 0.5 } : t >= 3.15 && t < 4.8 ? { smile: 0.62, browL: 0.32, browR: 0.3, lidU: 0.3, lidD: 0.35 } : {}),
      add: (t) => ({ hRot: -3.5 * bump(t, 1.9, 3.2), hY: -1.4 * bump(t, 1.9, 3.2) }) },
    tune: { dur: 6.2, idle: true,
      arms: { R: [[0, null], [0.95, 318, 287, 180, "pinch"], [3.7, 318, 287, 180, "pinch"], [4.5, null]] },
      mod: (t, A, R) => { if (t > 1.1 && t < 3.5) R.arm.R.a += Math.sin(t * 9) * 9; },
      tuner: (t) => clamp((t - 1.1) / 2.3, 0, 1),
      gaze: (t) => (t < 1.1 ? "peg" : t < 2 ? "tuner" : t < 3.6 ? [330, 120] : "user"),
      face: (t) => (t < 2 ? { browL: 0.2, browR: 0.1 } : t < 3.6 ? { lidU: 0.5, browL: 0.55, browR: 0.2, purse: 0.4 } : { smile: 0.58, browL: 0.3, browR: 0.3, lidD: 0.3 }),
      add: (t) => ({ hRot: -6 * bump(t, 0.4, 3.7) + 2 * bump(t, 2, 3.6), hX: -4 * bump(t, 0.4, 3.7), hY: 3 * bump(t, 3.9, 4.25, 0.12) + 2 * bump(t, 4.35, 4.7, 0.12), lean: -1.6 * bump(t, 0.4, 3.7), leanX: -3 * bump(t, 0.4, 3.7) }) },
    solder: { dur: 6, idle: true,
      arms: { R: [[0, null], [0.9, 317, 342, 132, "grip"], [1.0, 317, 342, 132, "iron"], [1.5, 332, 330, 70, "iron"], [2.1, 322, 381, 15, "iron"], [3.8, 322, 381, 15, "iron"], [4.3, 332, 330, 70, "iron"], [4.85, 317, 342, 132, "iron"], [4.95, 317, 342, 132, "grip"], [6, null]] },
      ev: [[1.0, (R) => R.prop("iron", false)], [4.95, (R) => R.prop("iron", true)], [4.0, (R) => R.ledFlash(1.4)]],
      mod: (t, A, R) => { if (t > 2.2 && t < 3.7) { R.arm.R.x += Math.sin(t * 31) * 0.5; R.arm.R.y += Math.cos(t * 23) * 0.5; } },
      solder: (t) => t > 2.25 && t < 3.65,
      gaze: (t, A, R) => (t < 1.0 ? [300, 360] : t < 3.9 ? R.tipPt() : t < 4.5 ? "pedal" : "user"),
      face: (t) => (t > 1.9 && t < 3.9 ? { furrow: 0.62, lidU: 0.34, lidD: 0.3, purse: 0.4, browL: -0.15, browR: -0.1, smile: 0 } : t >= 4 && t < 5.8 ? { smile: 0.5, browL: 0.35, browR: 0.3 } : {}),
      add: (t) => ({ hY: 3 * bump(t, 1.9, 3.9) + 2.4 * bump(t, 4.1, 4.5, 0.12), hRot: 2 * bump(t, 1.9, 3.9), glassY: 3 * bump(t, 1.9, 3.9), lean: -1 * bump(t, 1.5, 4) }) },
    scratch: { dur: 5.4, idle: true,
      arms: { L: [[0, null], [0.95, 522, 372, 30, "rest"], [3.9, 522, 372, 30, "rest"], [4.7, null]] },
      mod: (t, A, R) => { if (t > 1.05 && t < 3.8) { R.arm.L.x += Math.sin(t * 21) * 2.4; R.arm.L.a += Math.sin(t * 21) * 6; } },
      cat: (t) => (t > 1.0 && t < 3.9 ? "scratched" : null),
      gaze: () => "cat", face: () => ({ smile: 0.62, browL: 0.32, browR: 0.3, lidU: 0.24, lidD: 0.28 }),
      add: (t) => ({ lean: 3.2 * bump(t, 0.3, 4.5), leanX: 6 * bump(t, 0.3, 4.5), hRot: 5 * bump(t, 0.5, 4.4) }) },
    polish: { dur: 6.4, idle: true,
      arms: { R: [[0, null], [0.85, 372, 356, -30, "knob"], [2.9, 372, 356, -30, "knob"], [3.5, 360, 322, -55, "knob"], [4.8, 360, 322, -55, "knob"], [5.6, null]],
        L: [[0, null], [0.55, 446, 412, 196, "grip"], [0.7, 446, 412, 196, "rag"], [1.25, 408, 360, 200, "rag"], [2.9, 408, 360, 200, "rag"], [3.35, 446, 412, 196, "rag"], [3.5, 446, 412, 196, "grip"], [4.2, null]] },
      ev: [[0.7, (R) => R.prop("rag", false)], [3.5, (R) => R.prop("rag", true)]],
      mod: (t, A, R) => { if (t > 1.3 && t < 2.85) { R.arm.L.x += Math.cos(t * 10) * 4; R.arm.L.y += Math.sin(t * 10) * 3; } },
      shine: (t) => bump(t, 3.7, 4.3, 0.15),
      gaze: (t, A, R) => (t < 0.5 ? "user" : R.handPt("R", 18, -16)),
      face: (t) => (t > 3.5 && t < 5 ? { browL: 0.62, browR: 0.6, smile: 0.5, lidU: 0.26 } : { lidU: 0.28, browL: 0.15 }),
      add: (t) => ({ hRot: -2 * bump(t, 3.3, 5), hY: 1.5 * bump(t, 0.8, 3.2) }) },
    watch: { dur: 5.2, idle: true,
      arms: { R: [[0, null], [0.8, 386, 372, -100, "grip"], [0.9, 386, 372, -100, "watch"], [1.7, 366, 342, -20, "watch"], [3.6, 366, 342, -20, "watch"], [4.3, 386, 372, -100, "watch"], [4.4, 386, 372, -100, "grip"], [5.2, null]] },
      chain: (t) => t > 0.9 && t < 4.4,
      gaze: (t, A, R) => (t < 0.9 ? [392, 366] : t < 4.2 ? R.handPt("R", 14, -6) : "user"),
      face: (t) => (t > 1.8 && t < 3.2 ? { lidU: 0.36, browL: 0.5, browR: 0.18, purse: 0.45 } : t >= 3.2 && t < 4.8 ? { smile: 0.38, browL: 0.3, browR: 0.3 } : {}),
      add: (t) => ({ hY: 3 * bump(t, 1.6, 3.4) + 2 * bump(t, 3.3, 3.6, 0.1), hRot: -2 * bump(t, 1.6, 3.4) }) },
  };
  const IDLES = ["sip", "tune", "solder", "scratch", "polish", "watch", "wipe", "adjust"];
  const TALK_GESTURES = [["palmL", 3], ["palmR", 3], ["both", 2.5], ["pointChat", 1.6], ["adjust", 0.7], ["wipe", 0.4]];

  function create(scene, env) {
    env = env || {};
    const E = {};
    scene.querySelectorAll("[id^='gt-']").forEach((el) => { E[el.id.slice(3)] = el; });
    const cache = new Map();
    const attr = (el, k, v) => {
      if (!el) return;
      let c = cache.get(el);
      if (!c) { c = {}; cache.set(el, c); }
      if (c[k] === v) return;
      c[k] = v;
      el.setAttribute(k, v);
    };
    const tf = (el, s) => attr(el, "transform", s);
    const op = (el, v) => attr(el, "opacity", String(Math.round(clamp(v, 0, 1) * 100) / 100));
    const hands = {};
    for (const side of ["R", "L"]) {
      const m = {};
      if (E["hand-" + side]) E["hand-" + side].querySelectorAll("[data-pose]").forEach((g) => { m[g.dataset.pose] = g; });
      hands[side] = { m, cur: "rest" };
    }
    const setPose = (side, p) => { const H = hands[side]; if (!p || H.cur === p || !H.m[p]) return; H.m[H.cur].style.display = "none"; H.m[p].style.display = ""; H.cur = p; };

    // springs
    const ch = {};
    for (const k in CH) ch[k] = { v: NEUTRAL[k] || 0, t: NEUTRAL[k] || 0, vel: 0, w: CH[k] };
    ch.mw.v = ch.mw.t = 1;
    const spring = (s, dt) => { const w = s.w, x0 = s.v - s.t, v0 = s.vel, e = Math.exp(-w * dt), q = v0 + w * x0; s.v = s.t + (x0 + q * dt) * e; s.vel = (v0 - w * q * dt) * e; };
    const arm = { R: { x: REST.R[0], y: REST.R[1], a: REST.R[2], vx: 0, vy: 0, va: 0, pose: "rest", hint: [REST.R[4], REST.R[5]], hw: 1, hwT: 1 }, L: { x: REST.L[0], y: REST.L[1], a: REST.L[2], vx: 0, vy: 0, va: 0, pose: "rest", hint: [REST.L[4], REST.L[5]], hw: 1, hwT: 1 } };
    const armT = { R: { x: 0, y: 0, a: 0 }, L: { x: 0, y: 0, a: 0 } };

    const R = {   // state shared with actions
      state: "idle", T: 0, last: 0, lastDraw: 0, act: null, calmUntil: 0, moodV: 0, moodUntil: 0, moodDur: 1,
      view: { x: 0, y: 0, w: 800, h: 560 }, chatBelow: false, light: false, lightCheck: 0,
      gaze: [0, 0.04], gazeNext: 0, micro: [0, 0], microNext: 0, blinkT: -9, nextBlink: 1.5, dblBlink: 0,
      bph: 0, speakQ: [], visCur: "rest", visNext: 0, pauseUntil: 0, lastSpeak: -9, talked: 0, tail: "", listN: 0, cues: [],
      nextGesture: 0, nextIdle: 6, recent: [], typingT: -9, props: { mug: true, iron: true, rag: true }, ledUntil: 0, elbow: { R: 1, L: 1 },
      arm, armT,
    };
    R.mood = (v, secs) => { R.moodV = v; R.moodUntil = R.T + secs; R.moodDur = secs; };
    R.prop = (k, v) => { R.props[k] = v; };
    R.ledFlash = (s) => { R.ledUntil = R.T + s; };
    const P = () => R.props;
    // a point in a hand's local space → scene coordinates
    function handXY(side, lx, ly) {
      const a = arm[side].a * DEG;
      const [px, py] = rotP(lx * HS, (side === "L" ? -ly : ly) * HS, a);
      return [arm[side].x + px, arm[side].y + py];
    }
    R.handPt = (side, lx, ly) => handXY(side, lx || 16, ly || 0);
    R.tipPt = () => handXY("R", 52.8, 0);

    const POINTS = () => ({
      user: [400, 226], chat: R.chatBelow ? [430, 900] : [1010, 320], input: R.chatBelow ? [620, 1060] : [1010, 560],
      laptop: [400, 392], pedal: [400, 400], cat: [540, 388], window: [655, 150], peg: [294, 291], tuner: [303, 257], up: [320, 70],
    });
    const toGaze = (p) => { if (!p) return null; if (typeof p === "string") p = POINTS()[p]; return [clamp((p[0] - 400) / 250, -0.9, 0.9), clamp((p[1] - 222) / 170, -0.85, 0.85)]; };

    /* ---- actions */
    function play(name, opts) {
      const def = ACTIONS[name];
      if (!def) return false;
      R.act = { name, def, t0: R.T, dur: def.dur, opts: opts || {}, fired: new Set() };
      if (name === "pointChat" || name === "present") R.act.kf = { L: R.chatBelow ? [[0, null], [0.45, 466, 378, 48, "point"], [def.dur - 0.5, 466, 378, 48, "point"], [def.dur, null]] : [[0, null], [0.45, 478, 350, -6, "point"], [def.dur - 0.5, 478, 350, -6, "point"], [def.dur, null]] };
      if (name === "count") { const n = clamp(opts && opts.n || 1, 1, 3); R.act.kf = { L: [[0, null], [0.4, 452, 350, -98, "count" + n], [1.2, 452, 350, -98, "count" + n], [1.7, null]] }; }
      return true;
    }
    function endAct() {
      const A = R.act;
      if (!A) return;
      if (A.def.after) A.def.after(R);
      R.act = null;
      R.props.mug = R.props.iron = R.props.rag = true;
    }
    function kfEval(list, t, side, base) {
      let i = 0;
      while (i < list.length - 1 && t >= list[i + 1][0]) i++;
      const k0 = list[i], k1 = list[Math.min(i + 1, list.length - 1)];
      const p0 = k0[1] == null ? base : k0.slice(1), p1 = k1[1] == null ? base : k1.slice(1);
      const u = k1 === k0 ? 1 : ss((t - k0[0]) / (k1[0] - k0[0]));
      let da = p1[2] - p0[2];
      if (da > 180) da -= 360; else if (da < -180) da += 360;
      return [lerp(p0[0], p1[0], u), lerp(p0[1], p1[1], u), p0[2] + da * u, (k0[1] == null ? (u > 0.5 ? p1[3] : base[3]) : k0[4]) || base[3]];
    }

    /* ---- the API */
    const api = {
      setState(st) {
        if (st === R.state) return;
        const prev = R.state;
        R.state = st;
        if (st === "talking") { R.talked = 0; R.nextGesture = R.T + rnd(0.6, 1.4); R.listN = 0; if (R.act && ACTIONS[R.act.name].idle) endAct(); }
        if (st === "idle") { R.nextIdle = R.T + rnd(7, 12); R.visCur = "rest"; }
        if (st !== "talking") { R.speakQ.length = 0; R.visCur = "rest"; }
        if ((st === "thinking" || st === "searching" || st === "listening") && R.act && ACTIONS[R.act.name].idle) endAct();
        if (st === "listening" && prev !== "listening") R.calmUntil = 0;
        R.gazeNext = 0;
        if (env.reduced && env.reduced()) api.still();
      },
      react(kind) {
        const map = { nod: "nod", approved: "approved", applied: "approved", built: "approved", declined: "declined", failed: "shrug", error: "shrug", shrug: "shrug", present: "present", surprise: "surprise" };
        const name = map[kind];
        if (!name) return;
        if (R.act && !ACTIONS[R.act.name].idle && R.act.name !== "nod" && name === "nod") return;
        if (R.act) endAct();
        play(name);
        if (name === "approved") R.cat.ear = R.T;
        if (env.reduced && env.reduced()) api.still();
      },
      speak(str) {
        for (const c of str) {
          R.tail = (R.tail + c).slice(-8);
          const v = visOf(c);
          if (v) { R.speakQ.push(v); R.lastSpeak = R.T; R.talked++; continue; }
          if (".!?".includes(c)) { R.pauseUntil = R.T + 0.17; if (c === "?") cue({ browL: 0.55, browR: 0.6, hRot: 3.5 }, 0.8); else if (c === "!") cue({ browL: 0.7, browR: 0.7, hY: -1.5 }, 0.6); }
          else if (",;:".includes(c)) R.pauseUntil = R.T + 0.09;
          else if (c === "\n") { R.pauseUntil = R.T + 0.12; }
          else if (c === " ") R.speakQ.push("_");
          if (R.tail.endsWith("**") && Math.random() < 0.6) cue({ browL: 0.38, browR: 0.34 }, 0.45);
          let m;
          if ((m = R.tail.match(/\n(\d)[.)]\s$/)) || /\n[-*]\s$/.test(R.tail)) {
            R.listN = m ? +m[1] : R.listN + 1;
            if (!R.act && R.listN <= 3 && R.state === "talking") play("count", { n: R.listN });
          } else if (/\n[^-*\d\s]$/.test(R.tail)) R.listN = 0;
        }
      },
      typing() { R.typingT = R.T; R.micro = [rnd(0.02, 0.07), rnd(-0.02, 0.03)]; },
      calm(ms) { R.calmUntil = Math.max(R.calmUntil, R.T + ms / 1000); if (R.act && ACTIONS[R.act.name].idle && R.T - R.act.t0 < 0.4) endAct(); },
      poke() { R.cat.poke = R.T; if (!R.act && R.state === "idle") play("surprise"); },
      setView(x, y, w, h, chatBelow) { R.view = { x, y, w, h }; R.chatBelow = !!chatBelow; },
      reset() { R.last = 0; },
      frame,
      still,
      _R: R, _play: play,
    };
    function cue(f, dur) { R.cues.push({ f, t0: R.T, dur }); if (R.cues.length > 6) R.cues.shift(); }

    /* ---- the cat */
    R.cat = { mode: "sleep", t0: 0, nextWake: rnd(35, 70), tailT: -9, nextTail: rnd(3, 7), ear: -9, nextEar: rnd(5, 11), earSide: 0, poke: -9, ph: 0 };

    /* ---- ambient state */
    const amb = { fan: 0, neonNext: rnd(5, 12), neonT: -9, neonSeq: null, lampNext: rnd(25, 50), lampT: -9, slide: [{ y: 70, v: 0, hold: 1 }, { y: 120, v: 0, hold: 2.4 }], clockSec: -1, radioNext: rnd(15, 30), radioX: 0, radioT: 0, motes: Array.from({ length: 10 }, (_, i) => ({ p: Math.random() * TAU, s: 0.08 + Math.random() * 0.12, ox: Math.random(), oy: Math.random(), i })) };
    const NEON_SEQS = [[[0, 0.15], [0.06, 1], [0.11, 0.2], [0.17, 1]], [[0, 0.3], [0.04, 1], [0.22, 0.1], [0.27, 0.9], [0.31, 0.2], [0.38, 1]], [[0, 0.55], [0.09, 1]]];

    function still() {
      R.last = 0;
      const tg = Object.assign({}, NEUTRAL, BASE[R.state] || {});
      for (const k in ch) { ch[k].v = ch[k].t = k in tg ? tg[k] : ch[k].t; ch[k].vel = 0; }
      Object.assign(ch.mo, { v: 0.02, t: 0.02 });
      ch.mw.v = ch.mw.t = 1;
      ch.gazeX.v = ch.gazeY.v = 0;
      if (R.state === "thinking") { ch.gazeX.v = -0.5; ch.gazeY.v = -0.7; }
      if (R.state === "searching") ch.gazeY.v = 0.75;
      if (R.state === "listening") { const g = toGaze("input"); ch.gazeX.v = g[0]; ch.gazeY.v = g[1]; }
      R.act = null;
      R.props.mug = R.props.iron = R.props.rag = true;
      const base = armBase();
      for (const s of ["R", "L"]) { arm[s].x = base[s][0]; arm[s].y = base[s][1]; arm[s].a = base[s][2]; arm[s].pose = base[s][3]; arm[s].hw = arm[s].hwT = base[s][4] != null ? 1 : 0; if (base[s][4] != null) arm[s].hint = [base[s][4], base[s][5]]; }
      draw(true);
    }
    function armBase(st) {
      st = st || R.state;
      if (st === "thinking") return { R: [394, 317, -72, "grip"], L: REST.L };
      if (st === "searching") return { R: [374, 404, 8, "rest", -18, 62], L: [426, 404, 172, "rest", 18, 62] };
      return REST;
    }

    /* ---- one frame */
    function frame(now) {
      const reading = R.T < R.calmUntil && R.state === "idle";
      const active = R.state === "talking" || R.act || R.state === "searching" || Math.abs(ch.thought.v - ch.thought.t) > 0.01 || Math.abs(ch.laptop.v - ch.laptop.t) > 0.01 || R.T - R.typingT < 0.6;
      const gap = active ? 0 : reading ? 50 : 41;   // ~60 fps while he moves, 24 idle, 20 while you read
      if (R.last && now - R.lastDraw < gap - 1) return true;
      const dt = R.last ? clamp((now - R.last) / 1000, 0, 0.1) : 1 / 60;
      R.last = now; R.lastDraw = now;
      R.T += dt;
      if (R.T > R.lightCheck) { R.light = document.body.classList.contains("light"); R.lightCheck = R.T + 1; }
      think(dt, reading);
      for (const k in ch) spring(ch[k], dt);
      stepArms(dt);
      draw(false, dt, reading);
      return true;
    }

    function think(dt, reading) {
      const T = R.T, st = R.state;
      // schedulers: idles, talking gestures
      if (R.act && T - R.act.t0 >= R.act.dur) endAct();
      if (!R.act && st === "idle" && !reading && T >= R.nextIdle) {
        const pool = IDLES.filter((n) => !R.recent.includes(n));
        const n = pick(pool);
        R.recent.push(n); if (R.recent.length > 3) R.recent.shift();
        play(n);
        R.nextIdle = T + rnd(14, 26);
      }
      if (st === "talking" && !R.act && T >= R.nextGesture) {
        const slow = 1 + R.talked / 500;
        if (R.talked < 2400 || Math.random() < 0.3) {
          let tot = 0; for (const [, w] of TALK_GESTURES) tot += w;
          let r = Math.random() * tot, g = TALK_GESTURES[0][0];
          for (const [n, w] of TALK_GESTURES) { if ((r -= w) <= 0) { g = n; break; } }
          play(g);
        }
        R.nextGesture = T + rnd(2.4, 4.6) * slow;
      }

      // base targets
      const bst = R.act && R.act.def.override ? "idle" : st;   // reactions take over the whole body
      const tg = Object.assign({}, NEUTRAL, BASE[bst] || {});
      if (reading) { tg.smile = 0.2; tg.lidU = 0.2; }
      if (T < R.moodUntil) { const k = (R.moodUntil - T) / R.moodDur; tg.smile += R.moodV * k; tg.lidD += 0.3 * R.moodV * k; tg.cheek += 0.6 * R.moodV * k; }
      // the action's face/head on top, enveloped
      const A = R.act;
      let gz = null, mouthOv = null;
      if (A) {
        const t = T - A.t0, env_ = ss(t / 0.3) * ss((A.dur - t) / 0.45);
        for (const [et, fn] of A.def.ev || []) if (t >= et && !A.fired.has(et)) { A.fired.add(et); fn(R); }
        if (A.def.face) { const f = A.def.face(t, A, R); for (const k in f) tg[k] = lerp(tg[k], f[k], env_); }
        if (A.def.add) { const f = A.def.add(t, A, R); for (const k in f) tg[k] = (tg[k] || 0) + f[k]; }
        if (A.def.gaze) gz = A.def.gaze(t, A, R);
        if (A.def.mouth) mouthOv = A.def.mouth(t);
      }
      for (const c of R.cues) { const u = (T - c.t0) / c.dur; if (u >= 0 && u <= 1) { const e = Math.sin(Math.PI * u); for (const k in c.f) tg[k] = (tg[k] || 0) + c.f[k] * e; } }
      R.cues = R.cues.filter((c) => T - c.t0 < c.dur);

      // gaze: eyes lead, the head follows a little
      if (T >= R.gazeNext) {
        let to;
        if (st === "listening") { to = toGaze("input"); R.gazeNext = T + rnd(0.8, 1.6); }
        else if (st === "thinking") { to = pick([[-0.55, -0.75], [-0.35, -0.85], [-0.7, -0.5], [0.1, -0.8]]); R.gazeNext = T + rnd(1.2, 2.6); }
        else if (st === "searching") { to = [0, 0.78]; R.gazeNext = T + 10; }
        else if (st === "talking") { to = Math.random() < 0.8 ? [rnd(-0.06, 0.08), rnd(0, 0.08)] : toGaze("chat"); R.gazeNext = T + rnd(1.2, 3); }
        else if (reading) { to = Math.random() < 0.55 ? toGaze("chat") : [rnd(-0.05, 0.05), 0.04]; R.gazeNext = T + rnd(4, 8); }
        else { const r = Math.random(); to = r < 0.55 ? [rnd(-0.08, 0.08), rnd(0, 0.06)] : r < 0.72 ? toGaze("chat") : r < 0.84 ? toGaze("cat") : r < 0.94 ? toGaze("pedal") : toGaze("window"); R.gazeNext = T + rnd(1.6, 4.6); }
        if (Math.abs(to[0] - R.gaze[0]) + Math.abs(to[1] - R.gaze[1]) > 0.7 && Math.random() < 0.45) R.blinkT = T;
        R.gaze = to;
      }
      if (T >= R.microNext) { R.micro = [rnd(-0.04, 0.04), rnd(-0.03, 0.03)]; R.microNext = T + rnd(0.35, 1.2); }
      let g = R.gaze;
      if (st === "searching") g = [Math.sin(T * 1.7) * 0.42 + Math.sin(T * 5.1) * 0.05, 0.78 + (Math.floor(T * 0.6) % 3) * 0.04];
      if (gz) { const v = toGaze(gz); if (v) g = v; }
      tg.gazeX = clamp(g[0] + R.micro[0], -1, 1);
      tg.gazeY = clamp(g[1] + R.micro[1], -1, 1);
      tg.hRot += tg.gazeX * 2.6;
      tg.hX += tg.gazeX * 2;
      tg.hY += tg.gazeY * 2.2;
      tg.lidU += Math.max(0, tg.gazeY) * 0.22 - Math.max(0, -tg.gazeY) * 0.08;
      // smiles squeeze the lower lids and lift the cheeks
      tg.lidD += Math.max(0, tg.smile) * 0.4;
      tg.cheek += Math.max(0, tg.smile) * 0.7;
      // the mouth: visemes from the streamed text
      if (st === "talking") {
        if (T >= R.visNext) {
          if (T < R.pauseUntil) R.visCur = "rest";
          else if (R.speakQ.length) { let best = null, bp = -1; for (const v of R.speakQ) { const p = v === "_" ? 0.5 : VIS_PRI[v]; if (p >= bp) { bp = p; best = v; } } R.visCur = best === "_" ? "rest" : best; R.speakQ.length = 0; }
          else if (T - R.lastSpeak > 0.14) R.visCur = "rest";
          R.visNext = T + rnd(0.062, 0.088);
        }
      } else R.visCur = "rest";
      const V = mouthOv || VIS[R.visCur] || VIS.rest;
      for (const k in V) tg[k] = V[k];
      if (st === "talking") { tg.hRot += Math.sin(T * 2.3) * 0.9 + Math.sin(T * 3.7) * 0.5; tg.hY += ch.mo.v * 1.6; tg.browL += ch.mo.v * 0.12; tg.browR += ch.mo.v * 0.1; }
      if (st === "thinking" && !mouthOv) { tg.mo = 0.04; tg.mw = 0.7; }
      tg.wisp = -ch.hRot.vel * 0.06 + Math.sin(T * 1.9) * 1.6 + Math.sin(T * 4.3) * 0.6;
      for (const k in tg) if (ch[k]) ch[k].t = tg[k];
      // blink
      if (T >= R.nextBlink) { R.blinkT = T; R.nextBlink = T + rnd(2.2, 5.6) * (reading ? 1.4 : 1); if (Math.random() < 0.18) R.dblBlink = T + 0.3; }
      if (R.dblBlink && T >= R.dblBlink) { R.blinkT = T; R.dblBlink = 0; }

      // arms: base pose, action keyframes, small idle wiggles
      const base = armBase(bst);
      for (const s of ["R", "L"]) {
        let p = base[s], fast = false;
        const kf = A && ((A.kf && A.kf[s]) || (A.def.arms && Array.isArray(A.def.arms[s]) && A.def.arms[s]));
        if (kf) { p = kfEval(kf, T - A.t0, s, base[s]); fast = true; }
        armT[s].x = p[0]; armT[s].y = p[1]; armT[s].a = p[2]; arm[s].pose = p[3]; arm[s].fast = fast;
        const hint = !fast && base[s][4] != null;
        if (hint) arm[s].hint = [base[s][4], base[s][5]];
        arm[s].hwT = hint ? 1 : 0;
      }
      if (st === "thinking" && !(A && A.def.arms && A.def.arms.L)) armT.L.a += Math.sin(T * 13) > 0.55 ? -5 : 0;
      if (st === "searching") { armT.R.y += Math.max(0, Math.sin(T * 17)) * -2.2; armT.L.y += Math.max(0, Math.sin(T * 17 + 2)) * -2.2; }
      if (A && A.def.mod) A.def.mod(T - A.t0, A, { arm: { R: armT.R, L: armT.L } });
    }

    function stepArms(dt) {
      for (const s of ["R", "L"]) {
        const a = arm[s], t = armT[s], w = a.fast ? 15 : 8;
        for (const [k, vk] of [["x", "vx"], ["y", "vy"], ["a", "va"]]) {
          let target = t[k];
          if (k === "a") { let d = target - a.a; while (d > 180) d -= 360; while (d < -180) d += 360; target = a.a + d; }
          const sp = { v: a[k], t: target, vel: a[vk], w };
          spring(sp, dt);
          a[k] = sp.v; a[vk] = sp.vel;
        }
      }
    }

    function ik(s, sx, sy, wx, wy) {
      const dx = wx - sx, dy = wy - sy;
      let d = Math.hypot(dx, dy) || 1;
      const dmax = L1 + L2 - 0.6, dmin = 14;
      if (d > dmax) { wx = sx + (dx / d) * dmax; wy = sy + (dy / d) * dmax; d = dmax; }
      d = Math.max(d, dmin);
      const th = Math.atan2(dy, dx), al = Math.acos(clamp((L1 * L1 + d * d - L2 * L2) / (2 * L1 * d), -1, 1));
      const out = s === "R" ? 1 : -1;   // the elbow that points away from the body
      const cand = [th + out * al, th - out * al].map((a1) => [a1, sx + L1 * Math.cos(a1), sy + L1 * Math.sin(a1)]);
      const score = (c, i) => c[2] + (i === 0 ? 12 : 0) + (R.elbow[s] === i ? 8 : 0);
      const i = score(cand[0], 0) >= score(cand[1], 1) ? 0 : 1;
      R.elbow[s] = i;
      const [a1, ex, ey] = cand[i];
      return { a1, ex, ey, a2: Math.atan2(wy - ey, wx - ex), wx, wy };
    }

    /* ---- write the DOM */
    function draw(force, dt, reading) {
      const T = R.T, v = (k) => ch[k].v;
      const light = R.light;
      // breath, lean
      R.bph += (dt || 0) * TAU / (R.state === "talking" ? 3.7 : 4.7);
      const b = Math.sin(R.bph);
      const lean = v("lean"), leanX = v("leanX"), bob = v("bob"), shrug = v("shrug");
      tf(E.body, `translate(${n2(leanX)} ${n2(bob)}) rotate(${n2(lean)} 400 430)`);
      const sx = 1 + b * 0.004, sy = 1 + b * 0.012;
      tf(E.torso, `matrix(${n2(sx * 1000) / 1000} 0 0 ${n2(sy * 1000) / 1000} ${n2(400 * (1 - sx))} ${n2(430 * (1 - sy) - shrug * 0.5)})`);
      tf(E.apron, `matrix(1 0 ${n2(-lean * 0.008 * 1000) / 1000} 1 ${n2(lean * 0.008 * 324)} 0)`);
      tf(E.wallshadow, `translate(${n2(-leanX * 0.4 - v("hX") * 0.3)} ${n2(-bob * 0.3)})`);
      const hx = v("hX"), hy = v("hY") - b * 1.3 - shrug * 0.7, hr = v("hRot");
      tf(E.head, `translate(${n2(hx)} ${n2(hy)}) rotate(${n2(hr)} 400 296)`);
      tf(E.wisps, `rotate(${n2(v("wisp"))} 400 166)`);

      // brows
      const bl = v("browL"), br = v("browR"), fu = v("furrow"), wl = v("worryL"), wr = v("worryR");
      tf(E["brow-l"], `translate(${n2(fu * 1.3)} ${n2(-bl * 4.4 + fu * 1.6)}) rotate(${n2(-wl * 9 + fu * 7)} 380 197)`);
      tf(E["brow-r"], `translate(${n2(-fu * 1.3)} ${n2(-br * 4.4 + fu * 1.6)}) rotate(${n2(wr * 9 - fu * 7)} 420 197)`);
      op(E.wrinkles, 0.16 + 0.6 * Math.max(0, (bl + br) / 2) + 0.25 * Math.max(0, (wl + wr) / 2));
      op(E.furrow, fu * 0.85 + Math.max(0, (wl + wr) / 2) * 0.3);
      tf(E["crease-l"], `translate(0 ${n2(-bl * 1.2)})`); tf(E["crease-r"], `translate(0 ${n2(-br * 1.2)})`);
      // eyes
      const since = T - R.blinkT, blink = tri(since / 0.17);
      const sm = v("smile");
      let lu = clamp(Math.max(v("lidU"), blink), 0, 1), ld = clamp(v("lidD"), 0, 0.8);
      if (lu + ld * 0.55 > 1) lu = 1 - ld * 0.55;
      const lidY = lu * 10.6, closedOp = clamp((lu + ld * 0.55 - 0.82) / 0.14, 0, 1);
      for (const s of ["l", "r"]) {
        tf(E["lidu-" + s], `translate(0 ${n2(lidY)})`);
        tf(E["lidd-" + s], `translate(0 ${n2(-ld * 6)})`);
        op(E["closed-" + s], closedOp);
      }
      if (closedOp > 0) {
        const happy = sm > 0.45 || ld > 0.4;
        attr(E["closed-l"], "d", happy ? "M372 219Q380 213.6 388 219" : "M372 217.4Q380 222.6 388 217.4");
        attr(E["closed-r"], "d", happy ? "M412 219Q420 213.6 428 219" : "M412 217.4Q420 222.6 428 217.4");
      }
      const gx = v("gazeX") * 2.7, gyv = v("gazeY") * 1.7;
      tf(E["pupil-l"], `translate(${n2(gx)} ${n2(gyv)})`); tf(E["pupil-r"], `translate(${n2(gx)} ${n2(gyv)})`);
      // cheeks, smile lines, crow's feet
      tf(E.cheeks, `translate(0 ${n2(-v("cheek") * 2.6)})`);
      op(E.cheeks, 0.75 + 0.4 * v("cheek"));
      op(E.smilel, 0.15 + 0.6 * Math.max(0, sm));
      op(E.crow, 0.22 + 0.6 * Math.max(0, sm) + 0.25 * fu);
      // glasses (slide down the nose to peer, pushed back up)
      tf(E.glasses, `translate(0 ${n2(v("glassY"))})`);
      const sc = v("screen");
      op(E["lens-screen"], sc * (0.85 + 0.15 * Math.sin(T * 7.1)));
      if (sc > 0.01) { const y = ((T * 0.9) % 1) * 12 - 4; tf(E["scan-l"], `translate(0 ${n2(y)})`); tf(E["scan-r"], `translate(0 ${n2(y)})`); }
      op(E.facelight, sc * (0.1 + 0.03 * Math.sin(T * 5.3)));
      op(E.glint, 0.55 + 0.45 * Math.pow(Math.max(0, Math.sin(T * 0.8)), 24));
      // the mouth
      const mo = clamp(v("mo"), 0, 1), mw = v("mw"), mr = clamp(v("mr"), 0, 1), pu = clamp(v("purse"), 0, 1), press = clamp(v("press"), 0, 1), tuck = clamp(v("tuck"), 0, 1);
      const cx = 400 - pu * 2.2, hw = 8.8 * mw * (1 - 0.32 * mr) * (1 - 0.34 * pu), cy = 271.4 - sm * 2.4 + pu * 0.4;
      const open = mo * 14 * (1 - tuck * 0.4), top = 270.2 - sm * 0.6 - mo * 1.2, bottom = 271.4 + open;
      tf(E.chin, `translate(0 ${n2(open * 0.42)})`);
      if (open < 0.8) {
        attr(E["mouth-hole"], "d", `M${n2(cx - hw)} ${n2(cy)}Q${n2(cx)} ${n2(cy + 0.4)} ${n2(cx + hw)} ${n2(cy)}Z`);
        attr(E["mouth-clip"], "d", "M0 0Z");
        op(E["mouth-line"], 1);
        attr(E["mouth-line"], "d", `M${n2(cx - hw)} ${n2(cy)}Q${n2(cx)} ${n2(cy + 1.4 + sm * 3.4 - press * 1 - pu * 1.2)} ${n2(cx + hw)} ${n2(cy - pu * 0.8)}`);
        attr(E.lip, "d", `M${n2(cx - hw * 0.68)} ${n2(cy + 2.8 - press * 0.8)}Q${n2(cx)} ${n2(cy + 5.6 + sm * 1.4 + pu * 0.6)} ${n2(cx + hw * 0.68)} ${n2(cy + 2.8 - press * 0.8)}`);
        attr(E.lip, "stroke-width", n2(2.6 + press * 1.2 + pu * 0.8));
      } else {
        const k = 0.86 + 0.24 * mr;
        const d = `M${n2(cx - hw)} ${n2(cy)}Q${n2(cx)} ${n2(top - 1.4)} ${n2(cx + hw)} ${n2(cy)}Q${n2(cx + hw * k)} ${n2(bottom)} ${n2(cx)} ${n2(bottom)}Q${n2(cx - hw * k)} ${n2(bottom)} ${n2(cx - hw)} ${n2(cy)}Z`;
        attr(E["mouth-hole"], "d", d);
        attr(E["mouth-clip"], "d", d);
        op(E["mouth-line"], 0);
        attr(E.lip, "d", `M${n2(cx - hw * 0.74)} ${n2(bottom + 0.6 - tuck * 2)}Q${n2(cx)} ${n2(bottom + 3.6 - tuck * 2)} ${n2(cx + hw * 0.74)} ${n2(bottom + 0.6 - tuck * 2)}`);
        attr(E.lip, "stroke-width", n2(2.6 + tuck * 1.2));
        const mt = clamp(v("mt"), 0, 1), mtg = clamp(v("mtg"), 0, 1);
        attr(E.teeth, "y", n2(top - 2 - (1 - mt) * 3));
        attr(E.teeth, "x", n2(cx - hw)); attr(E.teeth, "width", n2(hw * 2));
        tf(E.tongue, `translate(${n2(cx - 400)} ${n2(bottom - 284 + 1.4 - mtg * Math.min(open, 7) * 0.5)}) translate(400 284) scale(${n2(Math.max(0.3, hw / 9))} 1) translate(-400 -284)`);
      }
      const stx = 1 + 0.05 * Math.max(0, sm) + 0.06 * (mw - 1) - 0.09 * pu, sty = 1 + 0.05 * pu;
      tf(E.stache, `translate(${n2(-pu * 1.6)} ${n2(-(Math.max(0, sm) * 1.7 + mo * 2 + pu * 1.3))}) rotate(${n2(-pu * 3)} 400 258) translate(400 258) scale(${n2(stx * 100) / 100} ${n2(sty * 100) / 100}) translate(-400 -258)`);

      // thought bubble
      const th = clamp(v("thought"), 0, 1.2);
      op(E.thought, th);
      if (th > 0.01) {
        tf(E.thought, `translate(452 175) scale(${n2(0.55 + 0.45 * Math.min(1, th))}) translate(-452 -175)`);
        tf(E.gear, `rotate(${n2((T * 150) % 360)} 512 124)`);
        tf(E.gear2, `rotate(${n2((-T * 240) % 360)} 528 112)`);
      }
      // laptop
      const lap = clamp(v("laptop"), 0, 1.1);
      op(E.laptop, clamp(lap * 3, 0, 1));
      tf(E.laptop, `translate(0 419) scale(1 ${n2(Math.max(0.04, Math.min(1.06, lap)))}) translate(0 -419)`);
      if (lap > 0.05) op(E["lid-halo"], 0.35 + 0.5 * (0.5 + 0.5 * Math.sin(T * 4.4)) * sc);

      drawArms();
      drawProps(T);
      drawCat(dt || 0, reading);
      drawAmbient(dt || 0, reading, light, force);
    }

    function drawArms() {
      const lean = ch.lean.v * DEG, leanX = ch.leanX.v, bob = ch.bob.v, shrug = ch.shrug.v, bphY = Math.sin(R.bph) * 1;
      for (const s of ["R", "L"]) {
        const a = arm[s];
        let [sx0, sy0] = SHOULDER[s];
        const [rx, ry] = rotP(sx0 - 400, sy0 - 430, lean);
        const sx = 400 + rx + leanX, sy = 430 + ry + bob - shrug - bphY;
        const k = ik(s, sx, sy, a.x, a.y);
        a.hw = a.hw == null ? a.hwT : a.hw + ((a.hwT || 0) - a.hw) * 0.12;
        let fs = 1, us = 1;
        if (a.hw > 0.001 && a.hint) {   // blend toward the hinted elbow; the forearm foreshortens to reach the hand
          const hx = sx + a.hint[0], hy = sy + a.hint[1];
          k.ex = lerp(k.ex, hx, a.hw); k.ey = lerp(k.ey, hy, a.hw);
          k.a1 = Math.atan2(k.ey - sy, k.ex - sx); k.a2 = Math.atan2(k.wy - k.ey, k.wx - k.ex);
          us = Math.hypot(k.ex - sx, k.ey - sy) / L1;
          fs = clamp(Math.hypot(k.wx - k.ex, k.wy - k.ey) / L2, 0.35, 1.15);
        }
        tf(E["ua-" + s], `translate(${n2(sx)} ${n2(sy)}) rotate(${n2(k.a1 / DEG)})${us < 0.995 || us > 1.005 ? ` scale(${n2(us)} 1)` : ""}`);
        tf(E["fa-" + s], `translate(${n2(k.ex)} ${n2(k.ey)}) rotate(${n2(k.a2 / DEG)})${fs < 0.995 ? ` scale(${n2(fs)} 1)` : ""}`);
        tf(E["cuff-" + s], `translate(${n2(k.ex)} ${n2(k.ey)}) rotate(${n2(k.a1 / DEG)})`);
        tf(E["hand-" + s], `translate(${n2(k.wx)} ${n2(k.wy)}) rotate(${n2(a.a)})`);
        setPose(s, a.pose);
      }
    }

    function drawProps(T) {
      const A = R.act, t = A ? T - A.t0 : 0, pr = R.props;
      op(E["mug-bench"], pr.mug ? 1 : 0);
      op(E["iron-rest"], pr.iron ? 1 : 0);
      op(E["iron-cord-rest"], pr.iron ? 1 : 0);
      op(E["apron-rag"], pr.rag ? 1 : 0);
      // the soldering iron's cable follows his hand; sparks + smoke at the tip
      if (!pr.iron) {
        const [ex, ey] = handXY("R", -12, 9);
        attr(E["iron-cable"], "d", `M236 397C214 416 ${n2((236 + ex) / 2 - 10)} ${n2(Math.max(ey, 380) + 40)} ${n2(ex)} ${n2(ey)}`);
        op(E["iron-cable"], 1);
      } else op(E["iron-cable"], 0);
      const soldering = !!(A && A.def.solder && A.def.solder(t));
      if (soldering) {
        const [tx, ty] = R.tipPt();
        tf(E.sparks, `translate(${n2(tx)} ${n2(ty)}) rotate(${Math.floor(Math.random() * 360)}) scale(${n2(0.6 + Math.random() * 0.6)})`);
        op(E.sparks, Math.random() < 0.7 ? 1 : 0.2);
        const u = ((T * 0.7) % 1);
        tf(E.smoke2, `translate(${n2(tx + Math.sin(T * 2) * 2)} ${n2(ty - 4 - u * 16)})`);
        op(E.smoke2, Math.sin(u * Math.PI) * 0.7);
      } else { op(E.sparks, 0); op(E.smoke2, 0); }
      // the pocket watch's chain
      if (A && A.def.chain && A.def.chain(t)) {
        const [wx, wy] = handXY("R", 15, -12.4), ox = 396 + ch.leanX.v, oy = 364 + ch.bob.v;
        attr(E["watch-chain"], "d", `M${n2(ox)} ${n2(oy)}Q${n2((ox + wx) / 2 - 4)} ${n2(Math.max(oy, wy) + 14)} ${n2(wx)} ${n2(wy)}`);
        op(E["watch-chain"], 1);
      } else op(E["watch-chain"], 0);
      // the clip-on tuner and its peg
      const tu = A && A.def.tuner ? A.def.tuner(t) : null;
      if (tu != null) {
        const ang = -38 * (1 - tu) + Math.sin(T * 11) * 6 * (1 - tu);
        tf(E["tuner-needle"], `rotate(${n2(ang)} 0 -8)`);
        attr(E["tuner-face"], "fill", tu > 0.92 ? "#1f5a2c" : "#4a1a12");
        attr(E["tuner-needle"], "stroke", tu > 0.92 ? "#8fff9e" : "#ff6a4a");
        tf(E.peg, `translate(-9.6 22) rotate(${n2(Math.sin(T * 9) * 50 * (t > 1.1 && t < 3.5 ? 1 : 0))})`);
      } else { tf(E["tuner-needle"], "rotate(-30 0 -8)"); attr(E["tuner-face"], "fill", "#2a1410"); attr(E["tuner-needle"], "stroke", "#7a4a3a"); }
      // a knob held to the light
      op(E["knob-shine"], A && A.def.shine ? A.def.shine(t) * 0.9 : 0);
      // the pedal's LED: blinks while he thinks, blue while he searches, green when he tests his solder joint
      let led = "#5a1a14", glow = 0;
      if (T < R.ledUntil) { led = "#7dff8e"; glow = 0.6; }
      else if (R.state === "thinking") { const on = (T * 1.4) % 1 < 0.5; led = on ? "#ff4a3a" : "#5a1a14"; glow = on ? 0.8 : 0; }
      else if (R.state === "searching") { const on = (T * 1.1) % 1 < 0.5; led = on ? "#3ad0ff" : "#123a4a"; glow = 0; }
      attr(E.led, "fill", led); op(E["led-glow"], glow);
      // steam off the coffee, a curl of smoke off the iron
      for (let i = 1; i <= 3; i++) {
        const u = ((T / 3.6) + i / 3) % 1;
        tf(E["steam" + i], `translate(${n2(Math.sin(T * 1.3 + i * 2) * 1.6)} ${n2(6 - u * 18)})`);
        op(E["steam" + i], pr.mug ? Math.sin(u * Math.PI) * 0.85 : 0);
      }
      const us = (T / 3) % 1;
      tf(E.smoke, `translate(${n2(Math.sin(T * 1.1) * 1.4)} ${n2(4 - us * 14)})`);
      op(E.smoke, pr.iron ? Math.sin(us * Math.PI) * 0.45 : 0);
    }

    function drawCat(dt, reading) {
      const C = R.cat, T = R.T, A = R.act;
      const scratched = A && A.def.cat && A.def.cat(T - A.t0) === "scratched";
      if (C.mode === "sleep" && !reading && R.state === "idle" && !A && T > C.nextWake) { C.mode = "wake"; C.t0 = T; }
      let ht = 0, hx = 0, hy = 0, px = 0, bsx = 1, eyes = 0, lids = 0.5, yawn = 0, purr = 0, earR = 0, earL = 0, zz = 1;
      if (C.mode === "wake") {
        const t = T - C.t0;
        ht = -14 * bump(t, 0.2, 2.8, 0.5) - 10 * bump(t, 1.1, 2.4, 0.3) + 6 * bump(t, 3, 4.6, 0.4);
        hy = -6 * bump(t, 0.2, 2.8, 0.5) + 2 * bump(t, 3, 4.6, 0.4);
        hx = -5 * bump(t, 3, 4.6, 0.4);
        eyes = bump(t, 0.6, 1.2, 0.15) + bump(t, 2.6, 5.4, 0.3);
        lids = 0.5 - 0.3 * bump(t, 3.2, 4.4);
        yawn = bump(t, 1.2, 2.4, 0.35);
        px = -12 * bump(t, 2.9, 4.8, 0.5);
        bsx = 1 + 0.05 * bump(t, 2.9, 4.8, 0.5);
        earL = -10 * bump(t, 1.2, 2.4, 0.2);
        zz = 0;
        if (t > 6.6) { C.mode = "sleep"; C.nextWake = T + rnd(50, 95); C.tailT = T; if (!R.act && R.state === "idle") R.gazeNext = T + 2.2; }
        if (t > 0.3 && t < 2.6 && !R.act && R.state === "idle") R.gaze = toGaze("cat");
      }
      if (T - C.poke < 2.4) { const t = T - C.poke; ht += -10 * bump(t, 0.05, 2, 0.3); hy += -4 * bump(t, 0.05, 2, 0.3); eyes = Math.max(eyes, bump(t, 0.25, 1.7, 0.15)); lids = 0.25; earR = 14 * (bump(t, 0.1, 0.3, 0.08) + bump(t, 0.45, 0.65, 0.08)); zz = 0; }
      if (scratched) { ht += -9 + Math.sin(T * 6) * 1.5; hy += -2; purr = 1; earL = 12; earR = -10; zz = 0; eyes = 0; }
      // breathing, tail, ear twitches
      C.ph += dt * TAU / 3.9;
      const br = Math.sin(C.ph);
      tf(E["cat-body"], `matrix(${n2(bsx + br * 0.006)} 0 0 ${n2(1 + br * 0.035)} ${n2(644 * (1 - bsx - br * 0.006))} ${n2(418 * -br * 0.035)})`);
      tf(E["cat-head"], `translate(${n2(hx + px * 0.6)} ${n2(hy - br * 0.6)}) rotate(${n2(ht)} 556 404)`);
      tf(E["cat-paws"], `translate(${n2(px)} 0)`);
      if (T > C.nextTail) { C.tailT = T; C.nextTail = T + rnd(3.5, 9) * (reading ? 1.6 : 1); }
      const tt = T - C.tailT, flick = tt < 0.9 ? Math.sin(tt / 0.9 * Math.PI * 2) * 16 * (1 - tt / 0.9) : 0;
      tf(E["cat-tailtip"], `rotate(${n2(flick + Math.sin(T * 0.7) * 2)} 579 424)`);
      if (T > C.nextEar) { C.ear = T; C.earSide = Math.random() < 0.5 ? 0 : 1; C.nextEar = T + rnd(5, 12); }
      const et = T - C.ear, tw = et < 0.5 ? (bump(et, 0, 0.18, 0.06) + bump(et, 0.24, 0.42, 0.06)) * 18 : 0;
      tf(E["cat-earL"], `rotate(${n2(earL - (C.earSide === 0 ? tw : 0))} 528 383)`);
      tf(E["cat-earR"], `rotate(${n2(earR + (C.earSide === 1 ? tw : 0))} 551 381)`);
      op(E["cat-open"], eyes); op(E["cat-closed"], 1 - eyes);
      tf(E["cat-lids"], `translate(0 ${n2(lids * 2.8)})`);
      tf(E["cat-yawn"], `translate(536.6 403.4) scale(${n2(yawn)})`);
      op(E.purr, purr * (0.5 + 0.5 * Math.sin(T * 9)));
      // z z Z
      for (let i = 1; i <= 3; i++) {
        const u = ((T / 4.4) + (i - 1) / 3) % 1;
        tf(E["z" + i], `translate(${n2(-u * 9)} ${n2(-u * 16)})`);
        op(E["z" + i], zz * Math.sin(u * Math.PI) * 0.85);
      }
    }

    function drawAmbient(dt, reading, light, force) {
      const T = R.T, vb = R.view;
      // background motion runs at ~15 fps: rain, a fan and dust don't need more, and the wall layer stays cheap
      const tick = force || T - (amb.at || 0) >= 0.064;
      if (tick) amb.at = T;
      // ceiling fan (only drawn while the ceiling is in view)
      if ((vb.y < -120 && tick) || force) { amb.fan = (T * 190) % 360; tf(E["fan-blades"], `translate(612 -146) scale(1 .2) rotate(${n2(amb.fan)})`); }
      // rain on the glass (night)
      if (!light && tick) {
        const dt = T - (amb.rainT || T); amb.rainT = T;
        tf(E.rainA, `translate(0 ${n2((T * 210) % 44)})`);
        tf(E.rainB, `translate(0 ${n2((T * 150) % 62)})`);
        amb.slide.forEach((d, i) => {
          if (d.hold > 0) d.hold -= dt; else { d.y += (26 + 40 * Math.random()) * dt; if (Math.random() < dt * 1.4) d.hold = rnd(0.4, 2.2); }
          if (d.y > 232) { d.y = rnd(60, 110); d.hold = rnd(1, 3); }
          tf(E["slide" + (i + 1)], `translate(0 ${n2(d.y - 64)})`);
        });
      }
      // the OPEN sign flickers now and then (less often while you read)
      if (T > amb.neonNext) { amb.neonT = T; amb.neonSeq = pick(NEON_SEQS); amb.neonNext = T + rnd(7, 16) * (reading ? 2.2 : 1); }
      let neon = 1;
      if (amb.neonSeq) { const t = T - amb.neonT; for (const [ts, val] of amb.neonSeq) if (t >= ts) neon = val; if (t > 0.6) amb.neonSeq = null; }
      op(E["neon-glow"], neon);
      // the lamp breathes once in a long while
      if (T > amb.lampNext) { amb.lampT = T; amb.lampNext = T + rnd(25, 55); }
      const lt = T - amb.lampT, dip = lt < 0.35 ? 1 - 0.22 * Math.sin(lt / 0.35 * Math.PI) * (lt < 0.1 ? 1 : 0.6) : 1;
      op(E.cone, dip); op(E.bulbglow, 0.75 + 0.25 * dip);
      // dust in the light: the lamp's cone at night, the window's sunbeam by day
      if (tick) for (const m of amb.motes) {
        m.p = T * m.s + m.ox * 9;
        const bx = light ? 450 + m.ox * 170 : 330 + m.ox * 200, by = light ? 140 + m.oy * 260 : 150 + m.oy * 240;
        const x = bx + Math.sin(m.p * 1.3 + m.i) * 18 + (light ? -m.p * 3 % 40 : 0), y = by + Math.sin(m.p * 0.9 + m.i * 2) * 22 - ((m.p * 6) % 30);
        const c = E.motes.children[m.i];
        attr(c, "cx", n2(x)); attr(c, "cy", n2(y));
        op(c, (0.25 + 0.6 * Math.max(0, Math.sin(m.p * 2.1 + m.i))) * (light ? 0.9 : 0.75));
      }
      // the clock (once a second)
      const d = new Date(), sec = d.getSeconds();
      if (sec !== amb.clockSec || force) {
        amb.clockSec = sec;
        const mi = d.getMinutes() + sec / 60, hr = (d.getHours() % 12) + mi / 60;
        tf(E["clock-s"], `rotate(${sec * 6} 534 58)`); tf(E["clock-m"], `rotate(${n2(mi * 6)} 534 58)`); tf(E["clock-h"], `rotate(${n2(hr * 30)} 534 58)`);
      }
      // the radio's dial: a slow glow, and once in a while someone turns the station
      if (T > amb.radioNext) { amb.radioT = rnd(-4, 4); amb.radioNext = T + rnd(18, 40); }
      amb.radioX += (amb.radioT - amb.radioX) * Math.min(1, dt * 1.5);
      tf(E["radio-needle"], `translate(${n2(amb.radioX)} 0)`);
      op(E["radio-glow"], 0.62 + 0.08 * Math.sin(T * 0.9));
    }

    return api;
  }

  window.HRTechArt = { markup, create, HAND_POSES: Object.keys(HAND_POSES), ACTIONS: Object.keys(ACTIONS) };
})();
