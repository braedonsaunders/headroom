/* =====================================================================
   UNIT ART — illustrated hardware faces (assets/units/, bundled by
   app/tools/sync_unit_art.py from work/illustrations).

   Each face is two layers stacked in one box with the art's aspect ratio:
     • <img> of the static drawing (everything that never moves). The browser
       rasterises it once per size, at device resolution, and shares it
       between instances — no DOM cost for the thousands of vector paths.
     • a small inline <svg> holding only the moving parts (knobs, switches,
       LED, footswitch, meter needles) and the defs they use. Its ids are
       prefixed per instance, so any number of units can share the page.
   Moving parts are ordinary `.ctl[data-slot][data-pid]` controls, so the
   chain's own machinery drives them: drag / fine drag / wheel / keyboard,
   double-click reset, right-click → macro, learn mode, ctlIndex painting.
   paintEl() hands `.ua-ctl` elements to HRUnitArt.paint(), which only
   touches the DOM when the angle or state actually changes.

   Hooks (index.html): pedalHTML / ampHTML / cabHTML / rackHTML → html(),
   extraParams → extraParams(), paintEl → paint(), fillThumb → thumb(),
   indexControls → indexMeters(), onTelemetry → onVU() + onTelemetry(), pollTelemetry → query().
   Globals used at call time: unitCls, moreBtn, drawerHTML, XBTN, PWR_SVG,
   esc, paramKind, stepCount, displayVal, isVisibleParam, panelOnly,
   UTILITY_PARAM, describe, showTip, hideTipSoon, modelOf, setValue, scheduleRackRefresh,
   drag, ui, renderChain, renderSheet, computeStructureKey.
   ===================================================================== */
(function () {
  "use strict";
  let DATA = window.HR_UNIT_ART || { units: {} };
  let UNITS = DATA.units || {};
  const BASE = "assets/units/";
  const FG = new Map();        // table_id -> foreground template ("__P__" id placeholder)
  const loading = new Map();   // table_id -> Promise
  const warned = new Set();
  let seq = 0;

  const norm = (s) => String(s || "").toLowerCase().replace(/[^a-z0-9]/g, "");
  const info = (tid) => UNITS[tid] || null;

  /* Units whose faces carry live, non-parameter UI keep their own renderer:
     MIDI units (keyboards, note readouts, sound browser), NAM (capture browser / import),
     the Cabinet unit (cabinet + microphone pickers and placement), hosted plugins. */
  function artFor(s) {
    if (!s || window.HR_UNIT_ART_OFF) return null;
    const a = info(s.table_id);
    if (!a) return null;
    if (/^plugin/.test(s.archetype || "") || s.archetype === "nam_capture" || /^midi_|^instrument|^synth|^arpeggiator/.test(s.archetype || "")) return null;
    if (window.HRCabinet && HRCabinet.isCabinet(s)) return null;
    if (s.table_id >= 900001 && s.table_id <= 900006) return null;   // 900006 Bloom: own renderer too
    return a;
  }

  /* ---------- loading (script tags: works from file:// in WKWebView and over http) ---------- */
  function load(tid) {
    if (FG.has(tid)) return Promise.resolve();
    if (loading.has(tid)) return loading.get(tid);
    const a = info(tid);
    const pr = new Promise((resolve) => {
      const sc = document.createElement("script");
      sc.src = `${BASE}${tid}.${a.h}.fg.js`;
      sc.async = true;
      sc.onload = () => resolve();
      sc.onerror = () => {
        loading.delete(tid);
        // The art was re-bundled while the app was open (file names carry a content hash): fetch the new index.
        if (reindex()) { resolve(); return; }
        console.warn(`[unit-art] could not load moving parts for ${tid}`); FG.set(tid, ""); clearTimeout(refreshT); refreshT = setTimeout(refreshPending, 16); resolve();
      };
      document.head.appendChild(sc);
    });
    loading.set(tid, pr);
    return pr;
  }
  let refreshT = 0;
  /* Re-read assets/units/index.js after the art was re-bundled under a running app (its old hashed files may be
     gone): swap in the new index, drop cached parts and re-render faces. Rate-limited so a genuinely missing
     file can't loop. Returns true if a reload was started. */
  let lastReindex = 0;
  function reindex() {
    const now = Date.now();
    if (now - lastReindex < 8000) return false;
    lastReindex = now;
    const sc = document.createElement("script");
    sc.src = `${BASE}index.js?t=${now}`;
    sc.onload = () => {
      sc.remove();
      DATA = window.HR_UNIT_ART || DATA;
      UNITS = DATA.units || {};
      FG.clear();
      loading.clear();
      bindCache && bindCache.clear && bindCache.clear();
      try { if (typeof renderChain === "function") renderChain(); } catch (e) { console.error(e); }
      try { document.querySelectorAll("#comp-list .comp-thumb").forEach((t) => { if (t.querySelector("img.ua-thumb") || t.dataset.uaFail) { delete t.dataset.uaFail; t.innerHTML = ""; if (typeof fillThumb === "function") fillThumb(t); } }); } catch (e) { /* pane not open */ }
      try { if (window.HRRoom && HRRoom.sync) HRRoom.sync(); } catch (e) { /* room not loaded */ }
    };
    sc.onerror = () => sc.remove();
    document.head.appendChild(sc);
    return true;
  }
  // A face image that fails to load (file re-bundled away) triggers the same refresh.
  document.addEventListener("error", (e) => {
    const t = e.target;
    if (t && t.tagName === "IMG" && t.classList && (t.classList.contains("ua-bg") || t.classList.contains("ua-thumb")) && String(t.getAttribute("src") || "").startsWith(BASE)) reindex();
  }, true);
  /* Drop shadows on the moving-parts layer, drawn so the parts themselves stay sharp. WebKit rasterises an SVG
     filter's result at a fixed, low scale and stretches it, so everything inside a filtered group (the TS9DX's
     nameplate lettering, footswitch pads, tape reels) goes soft once the face is magnified, e.g. under the pop-out
     card's CSS zoom. Each filtered group gets a static copy underneath that carries a shadow-only version of its
     filter, and the group itself is drawn unfiltered on top: the same picture (shadow under the part), but the part
     is plain vector. The copy is static (no data-ua, ids suffixed), so live parts never rebuild it. Only plain
     feDropShadow chains are converted (that is all the art uses); anything else is left as drawn. */
  function crispShadows(tpl) {
    if (!tpl || tpl.indexOf('filter="url(#') < 0) return tpl;
    const sh = new Map();   // filter id -> shadow-only filter id
    const at = (t, n) => { const m = t.match(new RegExp(`\\s${n}="([^"]*)"`)); return m ? m[1] : null; };
    tpl = tpl.replace(/<filter\b([^>]*?)\sid="([^"]+)"([^>]*)>([\s\S]*?)<\/filter>/g, (all, a1, id, a2, body) => {
      const ds = body.match(/<feDropShadow\b[^>]*>/g) || [];
      if (!ds.length || body.replace(/<feDropShadow\b[^>]*>(\s*<\/feDropShadow>)?/g, "").trim() || ds.some((t) => at(t, "in") || at(t, "result"))) return all;
      // feDropShadow i = merge(shadow of the previous result, previous result); the shadow-only output merges just the shadows
      let prev = "SourceGraphic", out = "";
      ds.forEach((t, i) => {
        out += `<feGaussianBlur in="${prev}" stdDeviation="${at(t, "stdDeviation") || 2}"/><feOffset dx="${at(t, "dx") || 0}" dy="${at(t, "dy") || 0}" result="s${i}o"/>`
          + `<feFlood flood-color="${at(t, "flood-color") || "#000"}" flood-opacity="${at(t, "flood-opacity") || 1}"/><feComposite in2="s${i}o" operator="in" result="s${i}"/>`;
        if (i < ds.length - 1) { out += `<feMerge result="r${i}"><feMergeNode in="s${i}"/><feMergeNode in="${prev}"/></feMerge>`; prev = `r${i}`; }
      });
      out += `<feMerge>${ds.map((_, i) => `<feMergeNode in="s${ds.length - 1 - i}"/>`).join("")}</feMerge>`;
      sh.set(id, `${id}-sh`);
      return `${all}<filter${a1} id="${id}-sh"${a2}>${out}</filter>`;
    });
    if (!sh.size) return tpl;
    const hits = [], re = /<g\b[^>]*\sfilter="url\(#([^)"]+)\)"[^>]*>/g;
    for (let m; (m = re.exec(tpl));) if (sh.has(m[1])) hits.push({ at: m.index, tag: m[0], f: m[1] });
    for (let k = hits.length - 1; k >= 0; k--) {   // back to front: earlier offsets stay valid
      const { at: p, tag, f } = hits[k];
      if (tag.endsWith("/>")) continue;
      const gre = /<g\b[^>]*?(\/?)>|<\/g>/g;
      gre.lastIndex = p;
      let depth = 0, end = -1;
      for (let t; (t = gre.exec(tpl));) {
        if (t[0][1] === "/") { if (--depth === 0) { end = gre.lastIndex; break; } }
        else if (!t[1]) depth++;
      }
      if (end < 0) continue;
      const grp = tpl.slice(p, end), sfx = `-s${k}`;
      const ids = new Set([...grp.matchAll(/\sid="([^"]+)"/g)].map((x) => x[1]));
      const copy = grp.replace(/\sdata-ua="[^"]*"/g, "")
        .replace(/\sid="([^"]+)"/g, (x, id) => ` id="${id}${sfx}"`)
        .replace(/url\(#([^)"]+)\)/g, (x, id) => (ids.has(id) ? `url(#${id}${sfx})` : x))
        .replace(/href="#([^"]+)"/g, (x, id) => (ids.has(id) ? `href="#${id}${sfx}"` : x))
        .replace(/^<g\b([^>]*?)\sfilter="url\(#[^)"]+\)"/, `<g aria-hidden="true" pointer-events="none"$1 filter="url(#${sh.get(f)})"`);
      tpl = tpl.slice(0, p) + copy + tag.replace(/\sfilter="url\(#[^)"]+\)"/, "") + tpl.slice(p + tag.length);
    }
    return tpl;
  }
  /* The static drawing (an SVG <img>) has the same trouble: WebKit draws filtered groups inside an SVG image
     (a tape deck's headblock with its ERASE / SYNC lettering, side jacks) from a low-resolution raster. For a face
     shown big (the pop-out card), crispBg swaps the <img> to the same drawing with crispShadows applied, as a blob URL
     (cached per drawing). Drawings without filters are left alone. Resolves when the swap is done (or not needed). */
  const BG_CRISP = new Map();   // bg url -> Promise<blob url | "">
  function crispBg(img) {
    const src = img && (img.dataset.uaSrc || img.getAttribute("src"));
    if (!src || !src.startsWith(BASE)) return Promise.resolve();
    if (!BG_CRISP.has(src)) {
      BG_CRISP.set(src, fetch(src).then((r) => (r.ok ? r.text() : "")).then((t) => {
        if (!t || t.indexOf('filter="url(#') < 0) return "";
        const out = crispShadows(t);
        return out === t ? "" : URL.createObjectURL(new Blob([out], { type: "image/svg+xml" }));
      }).catch(() => ""));
    }
    return BG_CRISP.get(src).then((u) => {
      if (!u || !img.isConnected || img.getAttribute("src") === u) return;
      const im = new Image();
      im.src = u;
      return (im.decode ? im.decode() : Promise.resolve()).then(() => {
        if (!img.isConnected) return;
        img.dataset.uaSrc = src;   // the original stays recorded (error handling, re-swaps)
        img.src = u;
      }).catch(() => {});
    });
  }
  function def(tid, tpl) {
    FG.set(+tid, crispShadows(tpl));
    loading.delete(+tid);
    // Faces rendered before their moving parts arrived: re-render once the burst of loads settles.
    clearTimeout(refreshT);
    refreshT = setTimeout(refreshPending, 16);
  }
  function refreshPending() {
    if (typeof drag !== "undefined" && drag) { refreshT = setTimeout(refreshPending, 120); return; }
    const pend = document.querySelectorAll(".unit.ua-loading");
    if (!pend.length) return;
    if (document.querySelector("#rack-bay .unit.ua-loading") && typeof renderChain === "function") {
      ui.structureKey = computeStructureKey();
      renderChain(false);
    }
    if (document.querySelector("#sheet-body .unit.ua-loading") && typeof renderSheet === "function" && ui.sheetSlot != null) renderSheet();
    window.dispatchEvent(new CustomEvent("hr-unit-art"));   // component detail hero, previews
  }

  /* ---------- parameter binding ---------- */
  const bindCache = new Map();
  function bindings(s, a) {
    const params = s.params || [];
    const key = `${s.table_id}|${params.map((p) => p.id).join(",")}`;
    if (bindCache.has(key)) return bindCache.get(key);
    const used = new Set(), out = [];
    const byName = (n) => params.find((p) => !used.has(p.id) && norm(p.name) === norm(n));
    const pending = [];
    a.controls.forEach((c) => {
      if (c.t === "led") { out.push({ c, role: "led" }); return; }
      if (c.t === "meter") { const p = byName(c.p); if (p) used.add(p.id); out.push({ c, role: "meter" }); return; }   // read-only: never in the drawer
      const p = byName(c.p);
      if (p) { used.add(p.id); out.push({ c, p: p.id }); }
      else {
        const b = { c, p: null };
        out.push(b);
        // The SSL filter switches are circuit extras and have no legacy panel aliases.
        if (!(s.table_id === 121000 && (c.p === "HP In" || c.p === "LP In"))) pending.push(b);
      }
    });
    // Fallback: pair what's left with the unit's unclaimed controls in panel order (components.json).
    if (pending.length) {
      const drawn = new Set(a.controls.map((c) => norm(c.p)));
      const free = (a.order || []).filter((n) => !drawn.has(norm(n))).map((n) => byName(n)).filter(Boolean);
      pending.forEach((b) => {
        const p = free.shift();
        if (p) { used.add(p.id); b.p = p.id; console.warn(`[unit-art] ${s.table_id} "${b.c.p}": no parameter by that name; bound to "${p.name}" by panel order`); }
        else if (!warned.has(`${s.table_id}:${b.c.p}`)) { warned.add(`${s.table_id}:${b.c.p}`); console.warn(`[unit-art] ${s.table_id} "${b.c.p}" (${b.c.el}): no matching parameter; left static`); }
      });
    }
    const res = { list: out, bound: used };
    bindCache.set(key, res);
    return res;
  }

  /* ---------- geometry ---------- */
  const r1 = (x) => Math.round(x * 10) / 10;
  function snapN(p) {
    const k = paramKind(p);
    if (k === "switch") return switchOn(p) ? 1 : 0;   // the engine's reading (an untouched 0.5 is off on most units)
    if (k === "step") { const c = stepCount(p); return c > 1 ? Math.round(p.norm * (c - 1)) / (c - 1) : 0; }
    return Math.max(0, Math.min(1, p.norm));
  }
  const rotates = (c) => !c.fixed && Math.abs((c.a1 ?? 0) - (c.a0 ?? 0)) > 0.5;
  const angleOf = (c, p) => r1((c.a0 ?? -135) + ((c.a1 ?? 135) - (c.a0 ?? -135)) * snapN(p));
  const rotAttr = (a, cx, cy) => `rotate(${a} ${cx} ${cy})`;
  /* Two-position bat/lever toggles are drawn standing straight up. Rotating them like a knob (±30°) left them
     tilted sideways; a real toggle flips over its pivot instead: up = on (as drawn), down = off (mirrored). */
  const isFlipToggle = (c, p) => c.t === "switch" && Math.abs((c.a1 ?? 0) - (c.a0 ?? 0)) <= 70 && (paramKind(p) !== "step" || stepCount(p) <= 2);
  const xformOf = (c, p) => {
    if (isFlipToggle(c, p)) {
      const on = paramKind(p) === "switch" ? switchOn(p) : p.norm >= 0.5;
      return { key: on ? "up" : "dn", t: on ? "matrix(1 0 0 1 0 0)" : `matrix(1 0 0 -0.82 0 ${r1(1.82 * c.cy)})` };
    }
    const a = angleOf(c, p);
    return { key: String(a), t: rotAttr(a, c.cx, c.cy) };
  };

  /* ---------- markup ---------- */
  function attrsFor(s, b, p) {
    const c = b.c;
    const k = paramKind(p);
    const visible = isVisibleParam(p);
    const rot = rotates(c);
    const parts = [];
    let cls = "ctl ua-ctl";
    if (!visible) {   // a read-only lamp drawn as a part: lights with the value, never grabbed
      cls += " ua-ind" + (p.norm >= 0.5 ? " ua-on" : "");
      parts.push(`data-kind="ind"`);
    } else if (k === "switch" || !rot) {   // toggles, push-buttons, selector buttons (a static part can only show on/off)
      const steps = k === "step" ? stepCount(p) : 2;
      const on = k === "switch" ? switchOn(p) : p.norm >= 0.5;
      cls += (rot ? " ua-tog" : " ua-btn") + (on ? " ua-on" : "");
      parts.push(`data-kind="tog" data-steps="${steps}" role="switch" aria-checked="${on}" tabindex="0"`);
    } else {   // knobs, rotary selectors, and continuous parameters drawn as levers
      cls += " ua-knob" + (c.t === "switch" ? " ua-lever" : "");
      parts.push(`data-kind="${k === "step" ? "step" : "knob"}" role="slider" aria-valuemin="0" aria-valuemax="100" aria-valuenow="${Math.round(p.norm * 100)}" tabindex="0"`);
    }
    if (c.v) cls += " ua-voicing";
    parts.unshift(`class="${cls}"`);
    parts.push(`data-slot="${s.slot_idx}" data-pid="${esc(p.id)}" aria-label="${esc(p.name)}" aria-valuetext="${esc(displayVal(p))}"`);
    if (rot) { const x = xformOf(c, p); parts.push(`transform="${x.t}" data-ua-a="${x.key}"`); }
    return parts.join(" ");
  }
  function instantiate(tpl, s, a) {
    const prefix = `ua${++seq}-`;
    let h = tpl.split("__P__").join(prefix);
    const { list } = bindings(s, a);
    const pmap = new Map((s.params || []).map((p) => [p.id, p]));
    list.forEach((b) => {
      const tag = `data-ua="${b.c.el}"`;
      let extra = "";
      if (b.role === "led") extra = `class="ua-led" data-act="bypass"`;
      else if (b.role === "meter") extra = `class="ua-meter" data-cx="${b.c.cx}" data-cy="${b.c.cy}" data-a0="${b.c.a0}" data-a1="${b.c.a1}" transform="${rotAttr(r1(b.c.a0 + (b.c.a1 - b.c.a0) * 0.05), b.c.cx, b.c.cy)}"`;
      else if (b.p && pmap.get(b.p)) extra = attrsFor(s, b, pmap.get(b.p));
      else extra = `class="ua-static"`;
      h = h.replace(tag, `${tag} ${extra}`);
    });
    h = displayMarkup(h, a, prefix);   // live meters, lamps and screens
    if (!list.some((b) => b.c.el === "led") && !(a.displays || []).some((d) => d.el === "led")) h = h.replace(`data-ua="led"`, `data-ua="led" class="ua-led" data-act="bypass"`);
    if (a.foot) h = h.replace(`data-ua="${a.foot}"`, `data-ua="${a.foot}" class="ua-foot" data-act="bypass"`);
    return h;
  }

  /* Chain placement from the drawing: stompboxes on the pedalboard, amps and cabinets in a stack, the rest in
     the rack. ("other" art keeps the catalog's amp/cab placement, otherwise it is wide rack-style gear.) */
  function form(s, d) {
    const a = artFor(s);
    if (!a) return null;
    d = d || describe(s);
    const [, , W, H] = a.vb;
    if (a.kind === "pedal" || H > W) return d.form === "wah" ? "wah" : "pedal";
    if (a.kind === "amp") return "amp";
    if (a.kind === "cab") return "cab";
    if (a.kind === "rack") return "rack";
    return d.form === "amp" || d.form === "cab" ? d.form : "rack";
  }
  function sizeCls(s, a, d) {
    const [, , W, H] = a.vb;
    const f = form(s, d);
    if (f === "pedal" || f === "wah") return H > W ? "ua-s-pedal" : "ua-s-wide-pedal";
    if (f === "amp" || f === "cab") return "ua-s-stack";
    return H / W < 0.4 ? "ua-s-rack1" : "ua-s-rack";
  }

  function html(s, d, i) {
    const a = artFor(s);
    if (!a) return null;
    const tid = s.table_id;
    const tpl = FG.get(tid);
    if (tpl == null) load(tid);
    const fg = tpl ? instantiate(tpl, s, a) : "";
    const [, , W, H] = a.vb;
    const pending = tpl == null;
    const label = esc(s.reference_name || a.name);
    const pwr = `<button class="pwr-btn ua-pwr" data-act="bypass" title="Power: bypass" aria-label="Power ${label}">${PWR_SVG}</button>`;
    // Busy stompboxes get a little more room so their knobs stay grabbable.
    const nMain = a.controls.filter((c) => !c.v && (c.t === "knob" || c.t === "switch")).length;
    const pw = nMain <= 4 ? 196 : nMain <= 7 ? 218 : 240;
    return `<div class="${unitCls(s, `ua ua-k-${a.kind} ${sizeCls(s, a, d)}${pending ? " ua-loading" : ""}`)}" data-unit="${s.slot_idx}" data-ua-tid="${tid}" style="--i:${i};--ua-ar:${W} / ${H};--ua-pw:${pw}px">
      <div class="hw-body ua-face" title="">
        <img class="ua-bg" src="${BASE}${tid}.${a.h}.bg.svg" alt="${label}" draggable="false">
        ${fg}
        <div class="ua-tools">${pwr}${moreBtn(s)}</div>
      </div>${drawerHTML(s)}${XBTN}</div>`;
  }

  /* Parameters the art doesn't show: they go to the "+N more" drawer (the Inspector lists everything too). */
  function extraParams(s) {
    const a = artFor(s);
    if (!a) return null;
    const { bound } = bindings(s, a);
    return (s.params || []).filter((p) => isVisibleParam(p) && !bound.has(p.id)
      && !(s.table_id === 121000 && p.id === "M12"));
  }

  /* ---------- painting (called from paintEl for every bound part) ---------- */
  function paint(el, p) {
    if (disps.length) kick();   // parameter-driven displays (curves, readouts, lamps) follow the knob
    const k = el.dataset.kind;
    const on = paramKind(p) === "switch" ? switchOn(p) : p.norm >= 0.5;
    if (el.classList.contains("ua-on") !== on && (k === "tog" || k === "ind")) el.classList.toggle("ua-on", on);
    const a0 = el.getAttribute("data-ua-a");
    if (a0 != null) {
      const c = ctlInfo(el);
      if (c) {
        const x = xformOf(c, p);
        if (a0 !== x.key) { el.setAttribute("data-ua-a", x.key); el.setAttribute("transform", x.t); }
      }
    }
    if (k === "ind") return;
    const nv = String(Math.round(p.norm * 100));
    if (k === "tog") { const v = String(on); if (el.getAttribute("aria-checked") !== v) el.setAttribute("aria-checked", v); }
    else if (el.getAttribute("aria-valuenow") !== nv) el.setAttribute("aria-valuenow", nv);
    const t = displayVal(p);
    if (el.getAttribute("aria-valuetext") !== t) el.setAttribute("aria-valuetext", t);
  }
  function ctlInfo(el) {
    if (el._uaC) return el._uaC;
    const unit = el.closest("[data-ua-tid]");
    const a = unit && info(+unit.dataset.uaTid);
    const c = a && a.controls.find((x) => x.el === el.dataset.ua);
    if (c) el._uaC = c;
    return c || null;
  }

  /* ---------- legacy meters (pre-"displays" index: needles follow the rig output) ---------- */
  let meters = [];
  function onVU(f) {
    if (!meters.length) return;
    meters.forEach((m) => {
      const bypassed = m.el.closest(".unit.bypassed");
      const a = r1(m.a0 + (m.a1 - m.a0) * (bypassed ? 0 : Math.max(0, Math.min(1, f))));
      if (a !== m.a) { m.a = a; m.el.setAttribute("transform", rotAttr(a, m.cx, m.cy)); }
    });
  }

  /* =====================================================================
     LIVE DISPLAYS — every meter, lamp and screen on a face (meta "displays", work/illustrations/BRIEF.md).
     Inputs: per-unit telemetry (/api/telemetry?units=…: in/out peak + RMS, gain reduction, LFO phase, spectrum,
     pitch, waveform — see ENGINE_API.md "Per-unit meters") and the unit's live parameters (rackState), so
     parameter-driven graphics (EQ curves, readouts, mode lamps) follow a knob while it is being dragged.
     One requestAnimationFrame loop drives every display with meter ballistics (VU ≈300 ms, peak hold on ladders),
     runs only while something moves, skips units scrolled out of view, and batches its DOM writes.
     A bypassed unit's needles and ladders fall to rest, lamps go out, reels stop, screens dim.
     ===================================================================== */
  const SVGNS = "http://www.w3.org/2000/svg";
  const clamp = (x, a, b) => (x < a ? a : x > b ? b : x);
  const VU_LAW = [[-20, 0], [-10, 0.25], [-7, 0.38], [-5, 0.48], [-3, 0.6], [-2, 0.66], [-1, 0.72], [0, 0.78], [1, 0.85], [2, 0.92], [3, 1]];
  function vuPos(v) {
    if (!(v > -20)) return 0;
    if (v >= 3) return 1;
    for (let i = 1; i < VU_LAW.length; i++) if (v <= VU_LAW[i][0]) { const [x0, y0] = VU_LAW[i - 1], [x1, y1] = VU_LAW[i]; return y0 + (v - x0) / (x1 - x0) * (y1 - y0); }
    return 1;
  }
  const NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  function mapVal(n, m) {
    if (!m) return n;
    const lo = +(m.min ?? 0), hi = +(m.max ?? 1), c = m.curve || "lin";
    if (c === "log" && lo > 0 && hi > 0) return lo * Math.pow(hi / lo, n);
    if (c === "exp") return lo + (hi - lo) * n * n;
    return lo + (hi - lo) * n;
  }
  const lerpColor = (a, b, t) => {
    const p = (c) => { c = String(c || "#fff").replace("#", ""); if (c.length === 3) c = c.replace(/./g, "$&$&"); return [0, 2, 4].map((i) => parseInt(c.slice(i, i + 2), 16) || 0); };
    const x = p(a), y = p(b);
    return "#" + x.map((v, i) => Math.round(v + (y[i] - v) * t).toString(16).padStart(2, "0")).join("");
  };
  const colorsOf = (d, def) => { const c = d.colors ?? d.color ?? def; return Array.isArray(c) ? c : [c, c, c]; };

  /* ---- markup for generated displays (spectrum / curve / scope / drawn ladders), inside a clipped viewport ---- */
  function genHTML(d, prefix) {
    const [x, y, w, h] = d.bbox.map(Number);
    const vp = (inner, defs = "") => `<svg x="${x}" y="${y}" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" preserveAspectRatio="none" overflow="hidden">${defs}${inner}</svg>`;
    if (d.type === "spectrum") {
      const n = d.bands || 28, cols = colorsOf(d, ["#7be0ff", "#c58bff"]), gap = d.gap ?? 0.28, bw = w / n;
      if (d.style === "line") return vp(`<path class="ua-sp-f" fill="${cols[0]}" fill-opacity="${d.fill ?? 0.25}" d=""/><path class="ua-sp-l" fill="none" stroke="${cols[0]}" stroke-width="${d.width ?? 1.6}" stroke-linejoin="round" d=""/>`);
      if (d.style === "segments") {
        const sc = d.seg_count || 12, sh = h / sc;
        let s = "";
        for (let i = 0; i < n; i++) for (let j = 0; j < sc; j++) s += `<rect data-b="${i}" data-j="${j}" x="${(i * bw + bw * gap / 2).toFixed(2)}" y="${(h - (j + 1) * sh + sh * 0.12).toFixed(2)}" width="${(bw * (1 - gap)).toFixed(2)}" height="${(sh * 0.76).toFixed(2)}" rx="0.8" fill="${lerpColor(cols[0], cols[cols.length - 1], j / Math.max(1, sc - 1))}" opacity="${d.off_opacity ?? 0.1}"/>`;
        return vp(s);
      }
      const gid = `${prefix}spg${d.k}`;
      let s = "";
      for (let i = 0; i < n; i++) s += `<rect data-b="${i}" x="${(i * bw + bw * gap / 2).toFixed(2)}" y="${h}" width="${(bw * (1 - gap)).toFixed(2)}" height="0" fill="url(#${gid})"/>`;
      return vp(s, `<defs><linearGradient id="${gid}" gradientUnits="userSpaceOnUse" x1="0" y1="${h}" x2="0" y2="0"><stop offset="0" stop-color="${cols[0]}"/><stop offset="1" stop-color="${cols[cols.length - 1]}"/></linearGradient></defs>`);
    }
    if (d.type === "curve") {
      const c = d.color || "#f2f2f2";
      const handles = (d.handles && d.bands || []).map((b, i) => `<circle class="ua-cv-h" data-i="${i}" r="${d.handle_r ?? 4}" cx="-20" cy="-20" fill="${b.color || c}" stroke="#fff" stroke-width="1"/>`).join("");
      return vp(`<path class="ua-cv-f" fill="${d.fill_color || c}" fill-opacity="${d.fill ?? 0.18}" stroke="none" d=""/><path class="ua-cv-l" fill="none" stroke="${c}" stroke-width="${d.width ?? 2}" stroke-linejoin="round" stroke-linecap="round" d=""/>${handles}`);
    }
    if (d.type === "scope") {
      const c = d.color || "#7cf29a";
      return vp(`<path class="ua-sc-l" fill="none" stroke="${c}" stroke-width="${d.width ?? 1.6}" stroke-linejoin="round" d="M0 ${h / 2}H${w}"/>${d.source === "lfo" || d.dot ? `<circle class="ua-sc-d" r="${d.dot_r ?? 3.2}" cx="0" cy="${h / 2}" fill="${d.dot_color || c}"/>` : ""}`);
    }
    if (d.type === "led") {   // a lamp drawn by the app (bbox, color, shape)
      return vp(d.shape === "dot" ? `<circle class="ua-lp" cx="${w / 2}" cy="${h / 2}" r="${Math.min(w, h) / 2}" fill="${d.color || "#46ff8e"}" opacity="${d.off_opacity ?? 0}"/>`
        : `<rect class="ua-lp" x="0" y="0" width="${w}" height="${h}" rx="${d.rx ?? Math.min(w, h) / 3}" fill="${d.color || "#46ff8e"}" opacity="${d.off_opacity ?? 0}"/>`);
    }
    if (d.type === "ring") {   // progress ring (looper position, a level, a parameter)
      return vp(`<path class="ua-rg" fill="none" stroke="${d.color || "#29e0ff"}" stroke-width="${d.width ?? 5}" stroke-linecap="round" d=""/>`);
    }
    if (d.type === "ladder") {   // drawn segments
      const n = d.count || 12, o = d.orient || "up", cols = colorsOf(d, ["#3ddc84", "#ffd23f", "#ff4b3e"]), gap = d.gap ?? 0.22;
      const z = d.zones || [-12, -3], [lo, hi] = d.range || (d.source === "gr" ? [0, 20] : [-48, 0]);
      let s = "";
      for (let i = 0; i < n; i++) {
        const db = lo + (hi - lo) * (i + 0.5) / n;
        const col = d.source === "gr" ? cols[Math.min(2, Math.floor(i / n * 3))] : (db >= z[1] ? cols[2] : db >= z[0] ? cols[1] : cols[0]);
        // segment i (0 = rest end) sits `off` from the end the bar grows from: up → bottom, down → top, right → left edge, left → right edge
        const vert = o === "up" || o === "down", L = vert ? h : w, sl = L / n, off = i * sl + sl * gap / 2, len = sl * (1 - gap);
        const at = (o === "up" || o === "left") ? L - off - len : off;
        s += vert ? `<rect data-seg="${i + 1}" x="0" y="${at.toFixed(2)}" width="${w}" height="${len.toFixed(2)}" rx="1" fill="${col}" opacity="${d.off_opacity ?? 0.12}"/>`
          : `<rect data-seg="${i + 1}" x="${at.toFixed(2)}" y="0" width="${len.toFixed(2)}" height="${h}" rx="1" fill="${col}" opacity="${d.off_opacity ?? 0.12}"/>`;
      }
      return vp(s);
    }
    return "";
  }
  function displayMarkup(h, a, prefix) {
    (a.displays || []).forEach((d) => {
      const tag = `data-ua="${d.el}"`, cls = `class="ua-disp ua-d-${d.type}" data-dk="${d.k}"`;
      if (d.gen) h = h.replace(`${tag}></g>`, `${tag} ${cls}>${genHTML(d, prefix)}</g>`);
      else {
        let x = cls;
        if (d.type === "needle") x += ` transform="${rotAttr(d.a0 ?? -42, d.cx, d.cy)}"`;
        h = h.replace(tag, `${tag} ${x}`);
      }
    });
    return h;
  }

  /* ---- registry ---- */
  const DSTATE = new Map();   // ballistic state per display, kept across re-renders
  let disps = [], dQuery = "", io = null;
  const telem = { t: 0, d: null, units: new Map(), has: false, rigIn: -120, rigOut: -120, lfo: new Map() };
  function indexMeters() {
    meters = [...document.querySelectorAll("#rack-bay .ua-meter, #sheet-body .ua-meter, #view-room .ua-meter, #sr-fhost .ua-meter, #cd-hero-unit .ua-meter")].map((el) => ({
      el, cx: el.dataset.cx, cy: el.dataset.cy, a0: +el.dataset.a0, a1: +el.dataset.a1, a: null }));
    const keep = new Set(), units = new Set(), want = { u: new Set(), sp: new Set(), tn: new Set(), wv: new Set() };
    if (!io && "IntersectionObserver" in window) io = new IntersectionObserver((es) => { es.forEach((e) => { e.target._uaVis = e.isIntersecting; }); qMemo = null; kick(); });
    disps = [];
    // #sr-fhost: the pop-out card over the Signal Chain; #cd-hero-unit: the component page's hero (its rack slot, if the
    // chain carries the unit, comes from data-ua-slot on the hero: the face itself is drawn as a preview, slot -1)
    document.querySelectorAll("#rack-bay .ua-disp, #sheet-body .ua-disp, #view-room .ua-disp, #sr-fhost .ua-disp, #cd-hero-unit .ua-disp").forEach((el) => {
      const unit = el.closest("[data-ua-tid]");
      const a = unit && info(+unit.dataset.uaTid);
      const spec = a && (a.displays || []).find((x) => x.k === +el.dataset.dk);
      if (!spec) return;
      let slot = unit.dataset.unit != null && unit.dataset.unit !== "" ? +unit.dataset.unit : -1;
      const host = slot < 0 && unit.closest("[data-ua-slot]");
      if (host && host.dataset.uaSlot !== "" && +host.dataset.uaSlot >= 0) slot = +host.dataset.uaSlot;
      const where = el.closest("#rack-bay") ? "r" : el.closest("#sheet-body") ? "s" : el.closest("#sr-fhost") ? "f" : el.closest("#cd-hero-unit") ? "c" : "v";
      const key = `${where}:${slot}:${unit.dataset.uaTid}:${spec.k}`;
      if (!DSTATE.has(key)) DSTATE.set(key, {});
      keep.add(key);
      units.add(unit);
      const D = { el, unit, slot, spec, st: DSTATE.get(key), w: {} };
      if (spec.type === "ladder") D.segs = [...el.querySelectorAll("[data-seg]")].sort((p, q) => p.dataset.seg - q.dataset.seg);
      if (spec.type === "text") D.txt = el.querySelector("[data-txt]") || el.querySelector("text");
      if (spec.type === "spectrum") D.bars = [...el.querySelectorAll("[data-b]")];
      if (spec.type === "curve") { D.cf = el.querySelector(".ua-cv-f"); D.cl = el.querySelector(".ua-cv-l"); D.hs = [...el.querySelectorAll(".ua-cv-h")]; }
      if (spec.type === "scope") { D.sl = el.querySelector(".ua-sc-l"); D.sd = el.querySelector(".ua-sc-d"); }
      if (spec.type === "ring") D.rg = el.querySelector(".ua-rg");
      if (spec.type === "spectrum" && spec.style === "line") { D.spf = el.querySelector(".ua-sp-f"); D.spl = el.querySelector(".ua-sp-l"); }
      // restore the last drawn state onto fresh markup
      if (D.st.lastA != null && spec.type === "needle") el.setAttribute("transform", rotAttr(D.st.lastA, spec.cx, spec.cy));
      D.st.drawn = null;
      disps.push(D);
      if (slot >= 0) {
        want.u.add(slot);
        const src = String(spec.source || "");
        D.q = { sp: spec.type === "spectrum", tn: src === "tuner" || !!(spec.source_by && JSON.stringify(spec.source_by).includes("tuner")), wv: spec.type === "scope" && src !== "lfo" };
        if (D.q.sp) want.sp.add(slot);
        if (D.q.tn) want.tn.add(slot);
        if (D.q.wv) want.wv.add(slot);
      }
    });
    for (const k of DSTATE.keys()) if (!keep.has(k)) DSTATE.delete(k);
    if (io) { io.disconnect(); units.forEach((u) => io.observe(u)); }
    const lst = (s) => [...s].slice(0, 64).join(",");
    dQuery = want.u.size ? `?units=${lst(want.u)}${want.sp.size ? "&spec=" + lst(want.sp) : ""}${want.tn.size ? "&tune=" + lst(want.tn) : ""}${want.wv.size ? "&wave=" + lst(want.wv) : ""}` : "";
    qMemo = null;
    kick();
  }
  /* Query string index.html appends to its /api/telemetry poll (per-unit data only for faces with displays).
     Idle CPU: only faces that are on screen (IntersectionObserver: a hidden view is display:none, a unit
     scrolled away is off screen) are asked for, so the engine's per-unit metering and FFT/pitch analysis
     disarm (~1.5 s) while the Signal Chain / Room aren't showing them. */
  let qMemo = null;
  const query = () => {
    if (qMemo != null) return qMemo;
    const want = { u: new Set(), sp: new Set(), tn: new Set(), wv: new Set() };
    for (const D of disps) {
      if (!D.q || D.unit._uaVis === false || !D.el.isConnected) continue;
      want.u.add(D.slot);
      if (D.q.sp) want.sp.add(D.slot);
      if (D.q.tn) want.tn.add(D.slot);
      if (D.q.wv) want.wv.add(D.slot);
    }
    const lst = (s) => [...s].slice(0, 64).join(",");
    return (qMemo = want.u.size ? `?units=${lst(want.u)}${want.sp.size ? "&spec=" + lst(want.sp) : ""}${want.tn.size ? "&tune=" + lst(want.tn) : ""}${want.wv.size ? "&wave=" + lst(want.wv) : ""}` : "");
  };

  const toDb = (v) => (v > 1e-6 ? Math.max(-120, 20 * Math.log10(v)) : -120);
  function onTelemetry(d) {
    if (!d) return;
    const now = performance.now();
    telem.d = d;
    telem.t = now;
    telem.has = Array.isArray(d.units);
    telem.units = new Map();
    (d.units || []).forEach((u) => {
      telem.units.set(u.s, u);
      // LFO / rotor phases ("lfo", and "lfo2" for a second rotor): keep a phase + rate estimate to run smoothly between polls
      [["lfo", u.s], ["lfo2", `${u.s}:2`]].forEach(([k, key]) => {
        const v = u[k];
        if (typeof v !== "number") return;
        const L = telem.lfo.get(key) || {};
        if (v < 0) { L.off = true; L.t = null; L.rate = null; telem.lfo.set(key, L); return; }   // the unit's oscillator is not running
        L.off = false;
        if (L.t != null) {
          const dt = (now - L.t) / 1000;
          let dp = v - L.ph; dp -= Math.floor(dp);
          if (L.hint && dt > 0) dp += Math.round(L.hint * dt - dp);   // resolve aliasing with the rate knob (signed: a rotor may turn backwards)
          if (dt > 0.02 && dt < 1) L.rate = L.rate == null ? dp / dt : L.rate + (dp / dt - L.rate) * 0.5;
        }
        L.ph = v; L.t = now;
        telem.lfo.set(key, L);
      });
    });
    telem.rigIn = toDb(d.in_rms || 0);
    telem.rigOut = toDb(Math.sqrt(((d.out_rms_l || 0) ** 2 + (d.out_rms_r || 0) ** 2) / 2));
    kick();
  }

  /* ---- model access ---- */
  let slotMapRef = null, slotMap = new Map();
  function slotModel(slot) {
    const arr = (typeof rackState !== "undefined" && rackState && rackState.slots) || [];
    if (arr !== slotMapRef) { slotMapRef = arr; slotMap = new Map(arr.map((s) => [s.slot_idx, s])); }
    return slotMap.get(slot) || null;
  }
  function param(D, name) {
    const s = slotModel(D.slot);
    if (!s || !s.params) return null;
    const c = D.pc || (D.pc = {});
    if (c._ref !== s.params) { for (const k in c) delete c[k]; c._ref = s.params; }
    if (!(name in c)) { const n = norm(name); c[name] = s.params.find((p) => norm(p.name) === n) || s.params.find((p) => p.id === name) || null; }
    return c[name];
  }
  // Position of a selector. `positions` describes a continuous parameter that stands for an n-position switch.
  const stepIdx = (p, positions) => { if (positions > 1) return Math.round(p.norm * (positions - 1)); const k = paramKind(p); if (k === "step") { const c = stepCount(p); return Math.round(p.norm * (c - 1)); } return k === "switch" ? (switchOn(p) ? 1 : 0) : p.norm >= 0.5 ? 1 : 0; };
  function pval(D, ref, dflt) {   // number | {p, min, max, curve} | "param:Name"
    if (ref == null) return dflt;
    if (typeof ref === "number") return ref;
    if (typeof ref === "string") { const p = param(D, ref.replace(/^param:/, "")); return p ? p.norm : dflt; }
    const p = param(D, ref.p);
    if (!p) return ref.default ?? dflt;
    return ref.min != null ? mapVal(ref.invert ? 1 - p.norm : p.norm, ref) : (ref.invert ? 1 - p.norm : p.norm);
  }
  function srcOf(D) {
    const sb = D.spec.source_by;
    D.ref = null; D.scale = null;
    if (sb && sb.p && Array.isArray(sb.values)) {
      const p = param(D, sb.p);
      if (p) {
        const v = sb.values[clamp(stepIdx(p, sb.positions), 0, sb.values.length - 1)];
        if (v && typeof v === "object") { D.ref = v.ref ?? null; D.scale = v.scale ?? null; return String(v.source || "out.rms"); }
        return String(v);
      }
    }
    return String(D.spec.source || "out.rms");
  }
  const isByp = (D) => D.unit.classList.contains("bypassed");
  const stale = (now) => now - telem.t > 1500;
  function levelDb(D, src, now) {
    if (isByp(D) || stale(now)) return -120;
    const key = { "in.rms": "ir", "in.peak": "ip", "out.rms": "or", "out.peak": "op" }[src] || "or";
    const u = telem.units.get(D.slot);
    if (u) return u.idle ? -120 : (u[key] ?? -120);
    if (!telem.has) return (key[0] === "i" ? telem.rigIn : telem.rigOut) + (key[1] === "p" ? 3 : 0);   // older engine: the rig's levels
    return -120;
  }
  function grDb(D, now) {
    if (isByp(D) || stale(now)) return 0;
    const u = telem.units.get(D.slot);
    if (!u || u.idle) return 0;
    if (typeof u.gr === "number") return u.gr;
    return u.ir != null && u.or != null ? clamp(u.ir - u.or, 0, 40) : 0;   // estimate for units that don't report it
  }
  function lfoPhase(D, now) {
    const L = telem.lfo.get(D.slot), s = D.spec, st = D.st;
    const rp = s.rate ? pval(D, s.rate, null) : null;
    const hz = rp == null ? null : (s.rate_hz ? mapVal(rp, { min: s.rate_hz[0], max: s.rate_hz[1], curve: s.rate_curve || "log" }) : (typeof s.rate === "object" && s.rate.min != null ? rp : 0.1 + 9.9 * rp * rp));
    if (L && L.hint !== hz) L.hint = hz;
    if (L && L.off && !stale(now)) return null;   // oscillator switched off: the lamp / dot rests
    if (L && L.t != null && !stale(now)) return (L.ph + (L.rate || hz || 0) * (now - L.t) / 1000) % 1;
    st.fph = ((st.fph || 0) + (hz ?? 1) * (st.fdt || 0)) % 1;   // free-running from the rate knob
    return st.fph;
  }
  const looperState = () => (telem.d && telem.d.looper) || (typeof rackState !== "undefined" && rackState && rackState.looper) || {};
  const bpmNow = () => { const t = typeof rackState !== "undefined" && rackState && rackState.tempo; return (t && (t.bpm || t.rig_bpm)) || 120; };

  /* ---- values for each kind of display ---- */
  function ledOn(D, now) {
    const s = D.spec, src = srcOf(D), byp = isByp(D);
    if (src === "bypass") return !byp;
    if (byp && !s.always) return false;
    if (src.startsWith("param:")) {
      const p = param(D, src.slice(6));
      if (!p) return !!s.default_on;
      if (s.on_index != null) return stepIdx(p, s.positions) === s.on_index;
      if (s.on_indexes) return s.on_indexes.includes(stepIdx(p, s.positions));
      if (s.on_range) return p.norm >= s.on_range[0] && p.norm <= s.on_range[1];
      if (s.on_below == null && s.on_above == null && paramKind(p) === "switch") return switchOn(p);   // as the engine reads it
      return s.on_below != null ? p.norm <= s.on_below : p.norm >= (s.on_above ?? 0.5);
    }
    if (src === "transport") return !!(telem.d && telem.d.playing) && !stale(now);
    if (src.startsWith("looper")) {
      const lp = (telem.d && telem.d.looper) || (typeof rackState !== "undefined" && rackState && rackState.looper) || {};
      const want = src.split(":")[1];
      return want ? String(lp.state || "").toLowerCase().startsWith(want) : /rec|play|dub/.test(String(lp.state || ""));
    }
    if (src === "beat") {
      const ph = (now / 1000) * bpmNow() / 60;
      if (s.chase) { const n = s.chase.n || 4; return Math.floor(ph) % n === (s.chase.i || 0); }
      return ph - Math.floor(ph) < (s.duty ?? 0.18);
    }
    if (src === "gr") return grDb(D, now) >= (s.threshold ?? 1);
    if (src === "clip" || src === "signal" || /^(in|out)\./.test(src)) {
      const db = levelDb(D, src === "clip" || src === "signal" ? "out.peak" : src, now);
      const th = s.threshold ?? (src === "clip" ? -1 : src === "signal" ? -50 : -24);
      const hold = s.hold ?? (src === "clip" ? 600 : 120);
      if (db >= th) D.st.until = now + hold;
      return now < (D.st.until || 0);
    }
    return !byp;
  }
  function fmtNum(v, s) {
    if (v == null || !isFinite(v)) return null;
    const f = s.format || "";
    let t;
    const dg = s.digits;
    if (f === "ms") t = v >= 10000 && !s.pad ? (v / 1000).toFixed(dg ?? 1) + (s.unit_auto ? " s" : "") : Math.round(v).toString();
    else if (f === "s") t = v.toFixed(dg ?? 2);
    else if (f === "Hz") t = v >= 1000 && !s.no_k ? (v / 1000).toFixed(dg ?? (v >= 10000 ? 1 : 2)) + "k" : v.toFixed(dg ?? (v < 10 ? 2 : v < 100 ? 1 : 0));
    else if (f === "kHz") t = (v / 1000).toFixed(dg ?? 1);
    else if (f === "%" || f === "int" || f === "bpm" || f === "counter") t = v.toFixed(dg ?? 0);
    else if (f === "dB") t = v.toFixed(dg ?? 1);
    else if (f === "st") t = v.toFixed(dg ?? 0);
    else t = v.toFixed(dg ?? 1);
    if ((s.signed || f === "dB" || f === "st") && v > 0 && !t.startsWith("+")) t = "+" + t;
    if (s.pad) { const neg = t.startsWith("-"), body = t.replace(/^[-+]/, ""), [i, fr] = body.split("."); t = (neg ? "-" : t.startsWith("+") ? "+" : "") + i.padStart(s.pad, "0") + (fr != null ? "." + fr : ""); }
    return t;
  }
  function noteOf(hz) {
    if (!(hz > 0)) return null;
    const m = 69 + 12 * Math.log2(hz / 440), r = Math.round(m);
    return { name: NOTE[((r % 12) + 12) % 12], oct: Math.floor(r / 12) - 1, cents: Math.round((m - r) * 100), hz };
  }
  function textOf(D, now) {
    const s = D.spec, src = srcOf(D);
    let out = null;
    if (src.startsWith("param:")) {
      const p = param(D, src.slice(6));
      if (!p) return null;
      // `active`: {p: switch param, on: bool} — the readout belongs to one side of a switch (e.g. a cab's A / B
      // capture); on the other side it reads `inactive_text` (after the prefix) instead of the value.
      if (s.active && param(D, String(s.active.p).replace(/^param:/, "")) && refOn(D, s.active.p) !== !!s.active.on) return `${s.prefix || ""}${s.inactive_text ?? ""}`;
      if (s.min_text != null && p.norm <= (s.min_at ?? 0.005)) return s.min_text;   // whole string, no prefix/suffix
      if (s.max_text != null && p.norm >= (s.max_at ?? 0.995)) return s.max_text;
      if (s.on_text != null || s.off_text != null) out = (s.on_above == null && paramKind(p) === "switch" ? switchOn(p) : p.norm >= (s.on_above ?? 0.5)) ? (s.on_text ?? "") : (s.off_text ?? "");
      else if (s.format === "label" || s.format === "engine") out = displayVal(p);
      else if (s.labels) out = s.labels[clamp(stepIdx(p, s.positions || (paramKind(p) === "step" ? 0 : s.labels.length)), 0, s.labels.length - 1)];
      else if (s.format === "note") { const n = noteOf(mapVal(p.norm, s.map)); out = n && `${n.name}${n.oct}`; }
      else {
        let v = s.map ? mapVal(p.norm, s.map) : s.format === "%" ? p.norm * 100 : p.val;
        if (s.interp && s.source2) {   // e.g. a graphic EQ's band N between Min and Max Freq
          const q = param(D, String(s.source2).replace(/^param:/, "")), hi = q ? mapVal(q.norm, s.map2 || s.map) : v, f = (s.interp.index ?? 0) / Math.max(1, (s.interp.of ?? 2) - 1);
          v = s.interp.curve === "log" && v > 0 && hi > 0 ? v * Math.pow(hi / v, f) : v + (hi - v) * f;
        }
        if (s.negate) v = -v;
        if (s.sign) v *= s.sign;
        if (s.format === "pitch_ratio") out = Math.pow(2, v / 1200).toFixed(s.digits ?? 3);
        else if (s.format === "note_hz") { const n = noteOf(v); out = n ? `${n.name}${n.oct}` : null; }
        else out = fmtNum(v, s);
      }
    } else if (src === "tempo") out = fmtNum(bpmNow(), { format: "bpm", ...s });
    else if (src === "tuner") {
      const u = telem.units.get(D.slot), n = !isByp(D) && !stale(now) && u && noteOf(u.hz);
      if (!n) out = s.idle ?? "--";
      else if (s.format === "cents") out = (n.cents > 0 ? "+" : "") + n.cents;
      else if (s.format === "Hz") out = n.hz.toFixed(1);
      else out = s.octave === false ? n.name : `${n.name}${n.oct}`;
    } else if (src === "counter") {
      const sec = D.st.sec || 0;
      if (s.format === "hms") { const m = Math.floor(sec / 60), x = sec - m * 60; out = `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}${s.short ? "" : ":" + x.toFixed(0).padStart(2, "0")}`; if (s.frames) out = `${String(m).padStart(2, "0")}:${String(Math.floor(x)).padStart(2, "0")}.${String(Math.floor((x % 1) * 100)).padStart(2, "0")}`; }
      else out = String(Math.floor(sec * (s.per_sec ?? 2)) % 10000).padStart(s.pad || 4, "0");
    } else if (src === "preset") { const m = slotModel(D.slot); out = (m && m.component_preset) || s.idle || null; }
    else if (src === "gr") out = fmtNum(-grDb(D, now), { format: "dB", ...s });
    else if (/^(in|out)\./.test(src)) { const db = levelDb(D, src, now); out = db <= -119 ? (s.idle ?? "-inf") : fmtNum(db, { format: "dB", ...s }); }
    else if (src === "looper" || src.startsWith("looper.")) {
      const lp = looperState(), f = s.field || src.split(".")[1] || "state";
      if (f === "state") out = String(lp.state || "stop").toUpperCase();
      else if (f === "position" || f === "pos") out = s.format === "%" ? fmtNum(lp.length_s > 0 ? 100 * (lp.pos || 0) / lp.length_s : 0, s) : fmtNum(lp.pos || 0, { format: "s", ...s });
      else if (f === "length") out = fmtNum(lp.length_s || 0, { format: "s", ...s });
      else out = lp[f] != null ? String(lp[f]) : null;
    }
    if (out == null) return null;
    out = `${s.prefix || ""}${out}${s.suffix || ""}`;
    return s.upper || s.uppercase ? out.toUpperCase() : out;
  }

  /* ---- filter / EQ response (analog prototypes, magnitude in dB) ---- */
  function bandDb(type, f, f0, g, q, order) {
    const w = f / f0, w2 = w * w, A = Math.pow(10, g / 40), sA = Math.sqrt(A);
    const mag = (a, b, c, d, e, h) => Math.hypot(a - c * w2, b * w) / Math.hypot(d - h * w2, e * w);   // |a+bs+cs²| / |d+es+hs²|
    let m;
    switch (type) {
      case "bell": case "peak": m = mag(1, A / q, 1, 1, 1 / (A * q), 1); break;
      case "lowshelf": m = mag(A * A, A * sA / q, A, 1, sA / q, A); break;
      case "highshelf": m = mag(A, A * sA / q, A * A, A, sA / q, 1); break;
      case "lowpass": m = mag(1, 0, 0, 1, 1 / q, 1); break;
      case "highpass": m = mag(0, 0, 1, 1, 1 / q, 1); break;
      case "bandpass": m = mag(0, 1 / q, 0, 1, 1 / q, 1); break;
      case "notch": m = mag(1, 0, 1, 1, 1 / q, 1); break;
      default: return 0;
    }
    const db = 20 * Math.log10(Math.max(1e-6, m));
    // Odd-order Butterworth passes (e.g. a console's 18 dB/oct HPF): exact |H|, as the engine realises them.
    if ((type === "lowpass" || type === "highpass") && order % 12 && Math.abs(q - 0.707) < 0.01) return -10 * Math.log10(1 + Math.pow(type === "lowpass" ? w : 1 / w, order / 3));
    return (type === "lowpass" || type === "highpass" || type === "bandpass" || type === "notch") ? db * Math.max(1, (order || 12) / 12) : db;
  }
  // A param reference as a switch: the engine's on/off for switch parameters, else norm >= 0.5.
  function refOn(D, ref) {
    if (typeof ref === "number") return ref >= 0.5;
    const p = param(D, typeof ref === "string" ? ref.replace(/^param:/, "") : ref && ref.p);
    if (!p) return true;
    return paramKind(p) === "switch" ? switchOn(p) : (typeof ref === "object" && ref.invert ? 1 - p.norm : p.norm) >= 0.5;
  }
  function curveBands(D) {
    const s = D.spec, out = [];
    (s.bands || []).forEach((b) => {
      if (b.on != null && !refOn(D, b.on)) { out.push(null); return; }
      let type = b.type || (s.model === "graphic" ? "bell" : "bell");
      if (type && typeof type === "object") { const p = param(D, type.p); type = p ? type.map[clamp(type.above != null ? ((paramKind(p) === "switch" ? switchOn(p) : p.norm > type.above) ? 1 : 0) : stepIdx(p), 0, type.map.length - 1)] : type.map[0]; }   // `above`: GR switch, untouched 0.5 = off (a switch: the engine's reading)
      let g = pval(D, b.g, 0);
      if (b.invert_by != null && refOn(D, b.invert_by)) g = -g;   // e.g. a Kill switch turning a peak into the same-depth cut
      let q = pval(D, b.q, s.model === "graphic" ? 1.4 : 0.9);
      // `g_mode`: a series switch (e.g. a console's E/G) that, when on, sets a shelf's Q or makes a bell's Q
      // proportional to its gain (StudioEQ.hpp models the same).
      const gm = b.g_mode, gp = gm && param(D, gm.p);
      if (gp && (paramKind(gp) === "switch" ? switchOn(gp) : gp.norm > (gm.above ?? 0.5))) {
        if (gm.q != null) q = gm.q;
        if (gm.qprop != null) q *= gm.qprop + (1 - gm.qprop) * Math.min(1, Math.abs(g) / (gm.full || 15));
      }
      out.push({ type, f: pval(D, b.f, 1000), g, q, order: pval(D, b.order, 12), color: b.color });
    });
    return out;
  }
  // The SSL face uses the component inspector's measurement of a private rack-unit instance.
  // Wait for a knob drag to settle and allow one measurement at a time per display.
  function measuredCurve(D, key, now, previewQuery) {
    const st = D.st;
    if (key !== st.requestKey) {
      st.requestKey = key;
      st.due = now + 180;
    }
    if (st.measuredKey === key || st.pending) return false;
    if (now < st.due) return true;
    const slot = slotModel(D.slot);
    const norms = Object.fromEntries((slot?.params || []).map(p => [p.id, p.norm]));
    const query = new URLSearchParams({ tid: D.unit.dataset.uaTid, slot: D.slot, what: "response", norms: JSON.stringify(norms) });
    st.pending = true;
    fetchJSON("/api/component/curves?" + (previewQuery ? previewQuery + "&what=response" : query)).then(({ response }) => {
      if (!response || !response.db.length || response.f.length !== response.db.length) throw new Error("missing response");
      if (key === st.requestKey) {
        st.response = response;
        st.measuredKey = key;
        st.drawn = null;
      }
    }).catch(() => { st.due = performance.now() + 2000; }).finally(() => { st.pending = false; kick(); });
    return false;
  }
  function curvePoints(D) {
    const s = D.spec, [, , w, h] = s.bbox.map(Number), [lo, hi] = s.range || [-18, 18];
    const yOf = (db) => clamp((hi - db) / (hi - lo) * h, -2, h + 2);
    const pts = [], N = 96;
    if (s.model === "measured") {
      const r = D.st.response, [f0, f1] = s.freq;
      for (let i = 0; i < r.f.length; i++) {
        pts.push([w * Math.log(r.f[i] / f0) / Math.log(f1 / f0), yOf(r.db[i] + r.peak_db)]);
      }
      return { pts, hs: [] };
    }
    if (s.model === "transfer") {
      const k = 0.6 + pval(D, s.drive, 0.4) * (s.k ?? 6), asym = pval(D, s.asym, 0);
      for (let i = 0; i <= N; i++) { const x = i / N * 2 - 1, y = Math.tanh(k * x + asym * x * x) / Math.tanh(k + asym); pts.push([i / N * w, (1 - (y + 1) / 2) * h]); }
      return { pts, hs: [] };
    }
    if (s.model === "transfer" && s.kind === "limiter") {   // dB in → dB out with a ceiling
      const thr = pval(D, s.threshold, -6), lo = -24;
      for (let i = 0; i <= N; i++) { const x = lo + (0 - lo) * i / N, y = Math.min(x, thr); pts.push([i / N * w, (0 - y) / (0 - lo) * h]); }
      return { pts, hs: [] };
    }
    if (s.model === "adsr" || s.model === "gate") {   // envelope shape: attack, (hold | decay→sustain), release
      const a = pval(D, s.attack, 0.2), hd = pval(D, s.hold ?? s.decay, 0.2), su = s.model === "gate" ? 1 : pval(D, s.sustain, 0.7), r = pval(D, s.release, 0.4);
      const tot = a + hd + 0.35 + r || 1, X = (t) => t / tot * w, Y = (v) => h - v * h * 0.9 - h * 0.05;
      let t = 0;
      pts.push([0, Y(0)]); t += a; pts.push([X(t), Y(1)]); t += hd; pts.push([X(t), Y(su)]); t += 0.35; pts.push([X(t), Y(su)]); t += r; pts.push([X(t), Y(0)]);
      return { pts, hs: [] };
    }
    if (s.model === "comb") {   // flanger / comb: |1 + m·e^(-jωτ)| / |1 − fb·e^(-jωτ)|
      const tau = pval(D, s.delay, 2) / 1000, m = pval(D, s.mix, 0.5) * pval(D, s.depth, 1), fb = pval(D, s.feedback, 0), [f0, f1] = s.freq || [20, 20000], L = Math.log(f1 / f0), [lo, hi] = s.range || [-18, 12];
      for (let i = 0; i <= N; i++) {
        const f = f0 * Math.exp(L * i / N), ph = 2 * Math.PI * f * tau;
        const db = 20 * Math.log10(Math.max(1e-4, Math.hypot(1 + m * Math.cos(ph), m * Math.sin(ph)) / Math.hypot(1 - fb * Math.cos(ph), fb * Math.sin(ph))));
        pts.push([i / N * w, clamp((hi - db) / (hi - lo) * h, 0, h)]);
      }
      return { pts, hs: [] };
    }
    if (s.model === "taps") {
      const tm = pval(D, s.time, 400), fb = clamp(pval(D, s.feedback, 0.4), 0, 0.99), span = s.span || ((s.time && s.time.max) || 1000) * 1.6, n = s.count || 8;
      for (let k = 0; k < n; k++) { const x = (k * tm) / span * w; if (x > w) break; pts.push([x, h - Math.pow(fb, k) * h * 0.92, k]); }
      return { pts, hs: [], taps: true };
    }
    const [f0, f1] = s.freq || [20, 20000], bands = curveBands(D), L = Math.log(f1 / f0);
    const total = (f) => bands.reduce((a, b) => a + (b ? bandDb(b.type, f, b.f, b.g, b.q, b.order) : 0), 0) + (s.gain ? pval(D, s.gain, 0) : 0);
    for (let i = 0; i <= N; i++) { const f = f0 * Math.exp(L * i / N); pts.push([i / N * w, yOf(total(f))]); }
    const hs = bands.map((b) => b && [clamp(Math.log(b.f / f0) / L, 0, 1) * w, yOf(total(b.f))]);
    return { pts, hs };
  }
  const pathOf = (pts) => pts.map((p, i) => `${i ? "L" : "M"}${p[0].toFixed(1)} ${p[1].toFixed(1)}`).join("");

  /* ---- the loop ---- */
  let raf = 0, lastT = 0;
  const W = [];   // batched writes: el, kind, name, value
  const put = (el, k, n, v) => { W.push(el, k, n, v); };
  function flush() {
    for (let i = 0; i < W.length; i += 4) {
      const el = W[i], k = W[i + 1], n = W[i + 2], v = W[i + 3];
      if (k === 0) el.setAttribute(n, v); else if (k === 1) el.textContent = v; else el.classList.toggle(n, v);
    }
    W.length = 0;
  }
  // Idle CPU: the loop sleeps while the window/editor is hidden (HRActivity) and skips faces that aren't on
  // screen, so an LFO lamp in a hidden Signal Chain no longer keeps it running; IntersectionObserver and
  // HRActivity kick it again (ballistics resume from the last drawn state).
  const awake = () => !window.HRActivity || HRActivity.awake();
  function kick() { if (!raf && disps.length && awake()) raf = requestAnimationFrame(frame); }
  if (window.HRActivity) HRActivity.on(kick);
  function frame(now) {
    raf = 0;
    if (!awake()) { lastT = 0; return; }
    const dt = lastT ? clamp((now - lastT) / 1000, 0, 0.1) : 1 / 60;
    lastT = now;
    let busy = false;
    for (let i = 0; i < disps.length; i++) {
      const D = disps[i];
      if (!D.el.isConnected || D.unit._uaVis === false) continue;
      D.st.fdt = dt;
      if (step(D, dt, now, false)) busy = true;
    }
    flush();
    if (busy) raf = requestAnimationFrame(frame); else lastT = 0;
  }
  // VU: a slightly under-damped 2nd-order movement, 99 % in ≈300 ms (IEC 60268-17), ~1 % overshoot.
  function vuMove(st, target, dt) {
    const w = 21, z = 0.82;
    let x = st.x ?? target, v = st.v || 0;
    for (let t = dt; t > 1e-6; t -= 1 / 240) { const h = Math.min(t, 1 / 240); v += (w * w * (target - x) - 2 * z * w * v) * h; x += v * h; }
    st.x = x; st.v = v;
    return Math.abs(target - x) > 2e-4 || Math.abs(v) > 2e-3;
  }
  function ballistic(st, target, dt, kind, span, s) {
    if (kind === "vu") return vuMove(st, target, dt);
    if (kind === "motor") {   // a motor-driven pointer (e.g. a plate's remote damper): eases in, travels at its top speed, eases out
      const top = (s && s.speed) || 0.35;   // fraction of the scale per second
      let x = st.x ?? target, v = st.v || 0;
      const want = clamp((target - x) * 3, -top, top);
      v += (want - v) * (1 - Math.exp(-dt / 0.08));   // critically damped with the 3/s approach: no overshoot
      x += v * dt;
      if (Math.abs(target - x) < 1e-3 && Math.abs(v) < 0.01) { x = target; v = 0; }
      st.x = x; st.v = v;
      return x !== target;
    }
    let x = st.x ?? target;
    if (kind === "ppm" || kind === "peak") x = target > x ? x + (target - x) * (1 - Math.exp(-dt / 0.008)) : Math.max(target, x - dt * (kind === "ppm" ? 13 : 20) / span);
    else x += (target - x) * (1 - Math.exp(-dt / (kind === "slow" ? 0.25 : 0.04)));
    st.x = x;
    return Math.abs(target - x) > 1e-3;
  }
  // A needle's printed scale as [value, angle°] pairs (e.g. a GR meter whose 0 / 2 / 4 / 8 / 12 / 16 / 20 dB ticks are
  // evenly spaced): the value is interpolated to its angle, returned as the 0..1 deflection between a0 and a1.
  function lawPos(s, v) {
    const L = s.law, a0 = s.a0 ?? -42, a1 = s.a1 ?? 42;
    let ang = L[0][1];
    if (v >= L[L.length - 1][0]) ang = L[L.length - 1][1];
    else for (let i = 1; i < L.length; i++) if (v <= L[i][0]) { const [x0, y0] = L[i - 1], [x1, y1] = L[i]; ang = v <= x0 ? y0 : y0 + (v - x0) / (x1 - x0) * (y1 - y0); break; }
    return a1 === a0 ? 0 : clamp((ang - a0) / (a1 - a0), 0, 1);
  }
  function levelPos(D, now) {   // 0..1 deflection for needles, ladders, glows and moves
    const s = D.spec, src = srcOf(D);
    if (src === "gr") {
      const g = grDb(D, now);
      if (s.type === "needle" && Array.isArray(s.law) && s.law.length > 1) return lawPos(s, g);   // printed (non-linear) GR scale
      if (s.type === "needle" && (D.scale || s.scale || "vu") === "vu") return isByp(D) ? (s.rest ?? 0) : vuPos(-g);   // VU set to 0 at rest, drops by the GR
      const r = s.range || [0, 20];
      return clamp((g - r[0]) / (r[1] - r[0]), 0, 1);
    }
    if (src === "tuner") { const u = telem.units.get(D.slot), n = !isByp(D) && !stale(now) && u && noteOf(u.hz); return n ? clamp(0.5 + n.cents / 100, 0, 1) : (s.rest ?? 0.5); }
    if (src === "lfo") { const ph = isByp(D) ? null : lfoPhase(D, now); return ph == null ? (s.rest ?? 0) : 0.5 + 0.5 * Math.sin(2 * Math.PI * ph); }
    if (src === "looper" || src === "looper.pos") { const lp = looperState(); return lp.length_s > 0 ? clamp((lp.pos || 0) / lp.length_s, 0, 1) : 0; }
    if (src.startsWith("param:") && s.values) {   // a value per selector position (e.g. each plate's decay time), on the scale `range`
      const p = param(D, src.slice(6));
      if (!p) return s.rest ?? 0;
      const v = s.values[clamp(stepIdx(p, s.positions), 0, s.values.length - 1)], [lo, hi] = s.range || [0, 1];
      return clamp((v - lo) / (hi - lo), 0, 1);
    }
    if (src.startsWith("param:")) { const p = param(D, src.slice(6)); return p ? (s.map ? clamp((mapVal(p.norm, s.map) - (s.map.min ?? 0)) / ((s.map.max ?? 1) - (s.map.min ?? 0)), 0, 1) : p.norm) : 0; }
    const db = levelDb(D, src, now);
    if (s.type === "needle" && (D.scale || s.scale || "vu") === "vu") return db <= -119 ? 0 : vuPos(db - (D.ref ?? s.ref ?? -18));   // `source_by` values may set their own scale
    const [lo, hi] = s.range || (s.type === "glow" ? [-50, -10] : [-48, 0]);
    return clamp((db - lo) / (hi - lo), 0, 1);
  }
  /* A spinning part, drawn at its angle st.ang. `axis: "y"`: it turns about a vertical axis through cx, seen from the
     front (a rotary speaker's horn: scale x by cos); `orbit: r` puts it on a cylinder of radius r (a rotor drum's
     opening slides across and narrows, hidden while it faces the back). Otherwise it turns in the picture plane. */
  function spinDraw(D) {
    const s = D.spec, st = D.st;
    if (s.axis !== "y") {
      const a = Math.round(st.ang * 2) / 2;
      if (a !== st.drawn) { st.drawn = a; put(D.el, 0, "transform", rotAttr(a, s.cx, s.cy)); }
      return;
    }
    const th = st.ang * Math.PI / 180, c = Math.cos(th), r = +s.orbit || 0;
    const sx = r ? Math.max(0.02, c) : (Math.abs(c) < 0.02 ? (c < 0 ? -0.02 : 0.02) : c);
    const tr = `translate(${r1(+s.cx + r * Math.sin(th))} 0) scale(${sx.toFixed(3)} 1) translate(${-s.cx} 0)`;
    if (tr === st.drawn) return;
    st.drawn = tr;
    put(D.el, 0, "transform", tr);
    if (r) put(D.el, 0, "opacity", clamp(c * 4, 0, 1).toFixed(2));
  }
  function step(D, dt, now, hidden) {
    const s = D.spec, st = D.st, t = s.type;
    let busy = false;
    if (t === "needle") {
      const target = isByp(D) ? (s.rest ?? 0) : levelPos(D, now);
      busy = ballistic(st, target, dt, s.ballistics || "vu", 1, s);
      const a = Math.round(((s.a0 ?? -42) + ((s.a1 ?? 42) - (s.a0 ?? -42)) * st.x) * 20) / 20;
      if (!hidden && a !== st.lastA) { st.lastA = a; put(D.el, 0, "transform", rotAttr(a, s.cx, s.cy)); }
      return busy;
    }
    if (t === "ladder") {
      const n = D.segs.length;
      if (!n) return false;
      const src = srcOf(D), isGr = src === "gr";
      const target = isByp(D) ? 0 : levelPos(D, now);
      const [lo, hi] = s.range || (isGr ? [0, 20] : [-48, 0]);
      busy = ballistic(st, target, dt, s.ballistics || "peak", hi - lo);
      const hold = s.peak_hold ?? 900;
      if (st.x >= (st.pk ?? 0)) { st.pk = st.x; st.pkT = now; }
      else if (now - (st.pkT || 0) > hold) st.pk = Math.max(st.x, (st.pk ?? 0) - dt * 30 / (hi - lo));
      if (hold && (st.pk ?? 0) > st.x + 1e-3) busy = true;
      let lit;
      if (s.thresholds) { const v = lo + (hi - lo) * st.x; lit = s.thresholds.filter((x) => v >= x).length; }
      else lit = Math.round(st.x * n);
      const pk = hold ? Math.round((st.pk ?? 0) * n) : 0;
      const key = lit * 1000 + pk;
      if (!hidden && key !== st.drawn) {
        st.drawn = key;
        D.segs.forEach((el, i) => {
          const on = i < lit || (pk > lit && i === pk - 1);
          if (el._on === on) return;
          el._on = on;
          if (s.gen) put(el, 0, "opacity", on ? "1" : String(s.off_opacity ?? 0.12)); else put(el, 2, "ua-off", !on);
        });
      }
      return busy;
    }
    if (t === "led") {
      const src = srcOf(D);
      let lv;
      if (src === "lfo") { lv = isByp(D) ? 0 : Math.round(levelPos(D, now) * 4); busy = !isByp(D); }
      else { let on = ledOn(D, now); if (s.invert) on = !on && !isByp(D); lv = on ? 4 : 0; busy = src === "beat" || ((st.until || 0) > now) || src === "transport"; }
      if (!hidden && lv !== st.drawn) {
        st.drawn = lv;
        if (s.gen) { const lp = D.el.querySelector(".ua-lp"); if (lp) put(lp, 0, "opacity", lv ? String(0.25 + 0.75 * lv / 4) : String(s.off_opacity ?? 0)); return busy && !isByp(D); }
        put(D.el, 2, "ua-off", lv === 0);
        for (let i = 1; i <= 3; i++) put(D.el, 2, `ua-l${i}`, lv === i);
      }
      return busy && !isByp(D);
    }
    if (t === "glow") {
      const src = srcOf(D), [mn, mx] = s.opacity || [0.35, 1];
      const target = isByp(D) ? (s.off ?? 0.06) : src === "bypass" ? mx : mn + (mx - mn) * levelPos(D, now);
      let x = st.x ?? target;
      x += (target - x) * (1 - Math.exp(-dt / (target > x ? 0.08 : 0.35)));   // filament warms fast, cools slower
      st.x = x;
      busy = Math.abs(target - x) > 0.004;
      const o = Math.round(x * 100) / 100;
      if (!hidden && o !== st.drawn) { st.drawn = o; put(D.el, 0, "opacity", String(o)); }
      return busy;
    }
    if (t === "text") {
      const src = srcOf(D);
      if (src === "counter") {
        const run = !isByp(D) && !stale(now) && telem.d && levelDb(D, "in.rms", now) > (telem.d.playing ? -80 : -60);   // the DI player or a live guitar
        if (run) st.sec = (st.sec || 0) + dt * (s.speed ? pval(D, s.speed, 1) : 1);
        busy = !!run;
      }
      if (hidden || !D.txt) return busy;
      const v = textOf(D, now);
      if (v != null && v !== st.drawn) { st.drawn = v; put(D.txt, 1, "", v); }
      return busy;
    }
    if (t === "spin") {
      const live = !isByp(D) && !stale(now) && !!telem.d;
      // The motor runs while audio plays: the DI player, or signal reaching the unit (a live guitar), held 4 s.
      if (live && levelDb(D, "in.peak", now) > -60) st.sigUntil = now + 4000;
      const run = live && (!!s.always || !!telem.d.playing || now < (st.sigUntil || 0));   // `always`: a motor that runs while the unit is on (a rotary cabinet)
      let rpm;
      if (s.rpm_by && s.rpm_by.p) {   // a speed switch picks the rpm (e.g. a rotary cabinet's slow / fast)
        const p = param(D, s.rpm_by.p), vs = s.rpm_by.values || [];
        rpm = pval(D, vs[p ? clamp(stepIdx(p, s.rpm_by.positions), 0, vs.length - 1) : 0], 33);
      } else rpm = typeof s.rpm === "number" ? s.rpm : s.rpm ? mapVal(pval(D, s.rpm, 0.5), { min: (s.rpm_range || [10, 60])[0], max: (s.rpm_range || [10, 60])[1] }) : 33;
      const dir = s.dir === -1 ? -1 : 1;
      // `source: "lfo"`: the engine reports the rotor's own angle (e.g. a rotary horn, with its real spin-up and
      // spin-down); it turns whenever the unit runs, like the real motor.
      const sk = srcOf(D), L = (sk === "lfo" || sk === "lfo2") && live ? telem.lfo.get(sk === "lfo2" ? `${D.slot}:2` : D.slot) : null;
      if (L && !L.off && L.t != null) {
        L.hint = (s.phase_dir === -1 ? -1 : 1) * Math.abs(rpm) / 60;   // `phase_dir: -1`: the reported angle falls (the rotor turns backwards)
        const ph = L.ph + (L.rate ?? L.hint) * (now - L.t) / 1000;
        st.ang = (((ph % 1) * 360 * dir) % 360 + 360) % 360;
        st.w = (L.rate ?? L.hint) * 60;
        if (!hidden) spinDraw(D);
        return true;
      }
      // Inertia: the reel / rotor spins up and coasts down (time constant `inertia` s, a number or param ref).
      const target = run ? rpm : 0, tau = Math.max(0.05, pval(D, s.inertia, 0.6));
      st.w = (st.w || 0) + (target - (st.w || 0)) * (1 - Math.exp(-dt / tau));
      if (!target && Math.abs(st.w) < 0.3) st.w = 0;
      st.ang = (((st.ang || 0) + st.w * 6 * dt * dir) % 360 + 360) % 360;
      if (!hidden) spinDraw(D);
      return !!target || st.w !== 0;
    }
    if (t === "move") {
      const target = isByp(D) && !srcOf(D).startsWith("param:") ? 0 : levelPos(D, now);
      busy = ballistic(st, target, dt, s.ballistics || (srcOf(D).startsWith("param:") ? "fast" : "vu"), 1, s);
      let tr;
      if (s.rotate) tr = rotAttr(Math.round((s.rotate[0] + (s.rotate[1] - s.rotate[0]) * st.x) * 20) / 20, s.cx, s.cy);
      else if (s.translate) { const [[x0, y0], [x1, y1]] = s.translate; tr = `translate(${r1(x0 + (x1 - x0) * st.x)} ${r1(y0 + (y1 - y0) * st.x)})`; }
      if (!hidden && tr && tr !== st.drawn) { st.drawn = tr; put(D.el, 0, "transform", tr); }
      return busy;
    }
    if (t === "spectrum") {
      const u = telem.units.get(D.slot), sp = !isByp(D) && !stale(now) && u && Array.isArray(u.sp) ? u.sp : null;
      const n = s.bands || 28, [lo, hi] = s.range || [-72, -6], [, , w, h] = s.bbox.map(Number);
      const vals = st.vals || (st.vals = new Float32Array(n));
      for (let i = 0; i < n; i++) {
        let db = lo;
        if (sp) { const x = (i + 0.5) / n * sp.length - 0.5, j = Math.floor(clamp(x, 0, sp.length - 1)), f = clamp(x - j, 0, 1); db = sp[j] + ((sp[Math.min(sp.length - 1, j + 1)] ?? sp[j]) - sp[j]) * f; }
        const tg = clamp((db - lo) / (hi - lo), 0, 1);
        const v = vals[i];
        vals[i] = tg > v ? v + (tg - v) * (1 - Math.exp(-dt / 0.03)) : Math.max(tg, v - dt * 26 / (hi - lo));
        if (Math.abs(vals[i] - tg) > 2e-3) busy = true;
      }
      if (hidden) return busy;
      if (s.style === "line") {
        const pts = [];
        for (let i = 0; i < n; i++) pts.push([(i + 0.5) / n * w, h - vals[i] * h]);
        const d = pathOf(pts);
        if (d !== st.drawn) { st.drawn = d; put(D.spl, 0, "d", d); put(D.spf, 0, "d", `${d}L${w} ${h}L0 ${h}Z`); }
        return busy;
      }
      const sc = s.seg_count || 12;
      const last = st.bh || (st.bh = new Float32Array(n).fill(-1));
      D.bars.forEach((el) => {
        const b = +el.dataset.b;
        if (s.style === "segments") {
          const on = vals[b] * sc > +el.dataset.j + 0.35;
          if (el._on !== on) { el._on = on; put(el, 0, "opacity", on ? "1" : String(s.off_opacity ?? 0.1)); }
        } else {
          const bh = Math.round(vals[b] * h * 2) / 2;
          if (bh !== last[b]) { last[b] = bh; put(el, 0, "y", String(h - bh)); put(el, 0, "height", String(bh)); }
        }
      });
      return busy;
    }
    if (t === "curve") {
      if (hidden || !D.cl) return false;
      // parameter-driven: redraw only when one of the parameters it depends on moves
      const s0 = slotModel(D.slot);
      const previewQuery = s.model === "measured" ? D.unit.closest("#cd-hero-unit")?.curveQuery?.() : "";
      const key = previewQuery || (s0 && s0.params ? s0.params.map((p) => p.norm).join(",") : "x");
      if (s.model === "measured") {
        const waiting = measuredCurve(D, key, now, previewQuery);
        if (key !== st.measuredKey) return waiting;
      }
      if (key === st.drawn) return false;
      st.drawn = key;
      const { pts, hs, taps } = curvePoints(D), [, , w, h] = s.bbox.map(Number);
      if (taps) {
        const d = pts.map((p) => `M${p[0].toFixed(1)} ${h}V${p[1].toFixed(1)}`).join("");
        put(D.cl, 0, "d", d);
        put(D.cf, 0, "d", pts.map((p) => `M${(p[0] - 2.5).toFixed(1)} ${p[1].toFixed(1)}a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0 -5 0`).join(""));
        return false;
      }
      const d = pathOf(pts);
      put(D.cl, 0, "d", d);
      put(D.cf, 0, "d", `${d}L${w} ${s.fill_to === "zero" ? (h * (s.range ? s.range[1] / (s.range[1] - s.range[0]) : 0.5)).toFixed(1) : h}L0 ${s.fill_to === "zero" ? (h * (s.range ? s.range[1] / (s.range[1] - s.range[0]) : 0.5)).toFixed(1) : h}Z`);
      D.hs.forEach((el) => { const p = hs[+el.dataset.i]; put(el, 0, "cx", p ? p[0].toFixed(1) : "-20"); put(el, 0, "cy", p ? p[1].toFixed(1) : "-20"); });
      return false;
    }
    if (t === "ring") {
      const x = isByp(D) ? 0 : levelPos(D, now), [, , w, h] = s.bbox.map(Number), r = s.r ?? Math.min(w, h) / 2 - (s.width ?? 5);
      const f = Math.round(clamp(x, 0, 0.9999) * 400) / 400;
      if (hidden || f === st.drawn) return !isByp(D) && /looper|lfo/.test(srcOf(D));
      st.drawn = f;
      const a = -Math.PI / 2 + f * 2 * Math.PI, cx = w / 2, cy = h / 2;
      put(D.rg, 0, "d", f <= 0 ? "" : `M${cx} ${cy - r}A${r} ${r} 0 ${f > 0.5 ? 1 : 0} 1 ${(cx + r * Math.cos(a)).toFixed(1)} ${(cy + r * Math.sin(a)).toFixed(1)}`);
      return !isByp(D) && /looper|lfo/.test(srcOf(D));
    }
    if (t === "scope") {
      const [, , w, h] = s.bbox.map(Number), src = srcOf(D);
      if (src === "lfo") {
        const shapeP = s.shape_param ? param(D, s.shape_param) : null;
        const shapes = s.shapes || ["sine", "triangle", "square", "saw"];
        const shape = shapeP ? shapes[clamp(stepIdx(shapeP), 0, shapes.length - 1)] : (s.shape || "sine");
        const depth = s.depth ? clamp(pval(D, s.depth, 1), 0.05, 1) : 1, cyc = s.cycles || 1;
        const fy = (ph) => { ph -= Math.floor(ph); const v = shape === "triangle" ? 1 - 4 * Math.abs(ph - 0.5) : shape === "square" ? (ph < 0.5 ? 1 : -1) : shape === "saw" ? 1 - 2 * ph : Math.cos(2 * Math.PI * ph); return h / 2 - v * depth * h * 0.42; };
        const key = shape + depth + cyc;
        if (!hidden && key !== st.shape) { st.shape = key; const pts = []; for (let i = 0; i <= 96; i++) pts.push([i / 96 * w, fy(i / 96 * cyc)]); put(D.sl, 0, "d", pathOf(pts)); }
        if (D.sd) {
          const ph = isByp(D) ? 0 : (lfoPhase(D, now) ?? 0), x = Math.round(ph * w / cyc * 2) / 2;
          if (!hidden && x !== st.drawn) { st.drawn = x; put(D.sd, 0, "cx", String(x)); put(D.sd, 0, "cy", fy(ph).toFixed(1)); }
        }
        return !isByp(D);
      }
      if (hidden || telem.t === st.drawn) return false;
      st.drawn = telem.t;
      const u = telem.units.get(D.slot);
      let wv = u && Array.isArray(u.wv) ? u.wv : (!telem.has && telem.d && telem.d.waveform_out) || null;
      if (isByp(D) || stale(now) || (u && u.idle)) wv = null;
      const g = s.gain ?? 1;
      const d = wv && wv.length > 1 ? pathOf(wv.map((v, i) => [i / (wv.length - 1) * w, clamp(h / 2 - v * g * h * 0.46, 0, h)])) : `M0 ${h / 2}H${w}`;
      put(D.sl, 0, "d", d);
      return false;
    }
    return false;
  }

  /* ---------- Components pane thumbnails: the full drawing as a plain image ---------- */
  function thumb(box, tid) {
    const a = info(tid);
    if (!a) return false;
    if (box.dataset.uaFail) return false;   // the image failed once: use the classic preview instead
    // Eager load: the pane already defers thumbnails with an IntersectionObserver, and WKWebView can leave
    // loading="lazy" images inside a scrolled pane (file:// pages) unloaded, which showed up as blank cards.
    box.innerHTML = `<img class="ua-thumb ua-thumb-${a.kind}" src="${BASE}${tid}.${a.h}.svg" alt="" decoding="async" draggable="false">`;
    const img = box.firstChild;
    img.addEventListener("error", () => { box.dataset.uaFail = "1"; box.innerHTML = ""; if (typeof fillThumb === "function") fillThumb(box); }, { once: true });
    return true;
  }

  /* ---------- hover readout + one-time global filters ---------- */
  function install() {
    if (!document.getElementById("ua-filters")) {
      const d = document.createElement("div");
      d.innerHTML = `<svg id="ua-filters" width="0" height="0" style="position:absolute;width:0;height:0" aria-hidden="true"><defs>
        <filter id="ua-led-off" color-interpolation-filters="sRGB"><feColorMatrix type="saturate" values="0.15"/><feComponentTransfer><feFuncR type="linear" slope="0.32"/><feFuncG type="linear" slope="0.32"/><feFuncB type="linear" slope="0.32"/></feComponentTransfer></filter>
        <filter id="ua-lv1" color-interpolation-filters="sRGB"><feComponentTransfer><feFuncR type="linear" slope="0.45"/><feFuncG type="linear" slope="0.45"/><feFuncB type="linear" slope="0.45"/></feComponentTransfer></filter>
        <filter id="ua-lv2" color-interpolation-filters="sRGB"><feComponentTransfer><feFuncR type="linear" slope="0.65"/><feFuncG type="linear" slope="0.65"/><feFuncB type="linear" slope="0.65"/></feComponentTransfer></filter>
        <filter id="ua-lv3" color-interpolation-filters="sRGB"><feComponentTransfer><feFuncR type="linear" slope="0.85"/><feFuncG type="linear" slope="0.85"/><feFuncB type="linear" slope="0.85"/></feComponentTransfer></filter>
        <filter id="ua-dim" color-interpolation-filters="sRGB"><feComponentTransfer><feFuncR type="linear" slope="0.62"/><feFuncG type="linear" slope="0.62"/><feFuncB type="linear" slope="0.62"/></feComponentTransfer></filter>
        <filter id="ua-lit" x="-30%" y="-30%" width="160%" height="160%" color-interpolation-filters="sRGB"><feComponentTransfer in="SourceGraphic" result="b"><feFuncR type="linear" slope="1.12"/><feFuncG type="linear" slope="1.12"/><feFuncB type="linear" slope="1.12"/></feComponentTransfer><feGaussianBlur in="SourceAlpha" stdDeviation="3" result="g"/><feFlood flood-color="#ffb347" flood-opacity="0.55"/><feComposite in2="g" operator="in" result="glow"/><feMerge><feMergeNode in="glow"/><feMergeNode in="b"/></feMerge></filter>
      </defs></svg>`;
      document.body.appendChild(d.firstChild);
    }
    ["rack-bay", "sheet-body", "view-room"].forEach((id) => bindRoot(document.getElementById(id)));   // view-room: the Room view's lifted unit (assets/stage-room.js)
  }
  /* Lever clicks and hover readouts for illustrated faces inside root. Also called by stage-room.js for the pop-out
     card it creates over the Signal Chain (#sr-fhost). Idempotent. */
  function bindRoot(root) {
    if (!root || root._uaBound) return;
    root._uaBound = true;
    {
      // A lever (a switch drawn for a continuous or multi-position parameter) drags like a knob; a plain click
      // flips it (0 <-> 1) or steps it to the next position, like the real switch.
      let down = null;
      root.addEventListener("pointerdown", (e) => { const el = e.target.closest && e.target.closest(".ua-lever"); down = el ? { el, x: e.clientX, y: e.clientY } : null; }, true);
      root.addEventListener("click", (e) => {
        const el = e.target.closest && e.target.closest(".ua-lever");
        if (!el || !down || down.el !== el || Math.abs(e.clientX - down.x) + Math.abs(e.clientY - down.y) > 3 || ui.learnCol != null) return;
        const p = modelOf(el);
        if (!p) return;
        let n;
        if (paramKind(p) === "step") { const c = stepCount(p), i = Math.round(p.norm * (c - 1)); n = (i + 1) % c / Math.max(1, c - 1); }
        else n = p.norm >= 0.5 ? 0 : 1;
        setValue(el, n);
        hideTipSoon(700);
        scheduleRackRefresh(300);
      });
      root.addEventListener("pointerover", (e) => {
        const el = e.target.closest && e.target.closest(".ua-ctl[data-pid]");
        if (!el || el.dataset.kind === "ind" || (typeof drag !== "undefined" && drag)) return;
        const p = modelOf(el);
        if (p) showTip(el, p.name, displayVal(p));
      });
      root.addEventListener("pointerout", (e) => {
        const el = e.target.closest && e.target.closest(".ua-ctl[data-pid]");
        if (!el || (e.relatedTarget && el.contains(e.relatedTarget)) || (typeof drag !== "undefined" && drag)) return;
        hideTipSoon(180);
      });
    }
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", install); else install();

  window.HRUnitArt = {
    def, has: (s) => !!artFor(s), artFor, form, html, extraParams, paint, indexMeters, onVU, thumb, load, bindRoot, crispBg,
    onTelemetry, query, kick,   // live displays: index.html feeds telemetry and appends query() to its poll
    count: () => Object.keys(UNITS).length,
    /* diagnostics: what each illustrated unit binds (used by the verification pass) */
    report(s) { const a = artFor(s); if (!a) return null; const b = bindings(s, a); return { bound: b.list.filter((x) => x.p).length, unmatched: b.list.filter((x) => !x.p && !x.role).map((x) => x.c.p), extra: extraParams(s).map((p) => p.name) }; },
  };
})();
