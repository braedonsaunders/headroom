/* Headroom — hosted Audio Unit plugins in the rack UI.
 *
 *   - "Import plugins" dialog: lists the Audio Units installed on this Mac (GET /api/plugins) and imports
 *     the chosen ones (GET /api/plugins/import?uid=a,b). Imported plugins become cards in the
 *     Components → Plugins group and drag into the chain / split lanes like any unit.
 *   - Plugin units render through the generic rack-unit UI (knobs from the plugin's parameter list) and get
 *     an "Editor" button that opens the plugin's own window (native bridge in the app, HTTP fallback).
 *   - While an editor window is open the rack polls so knobs follow what you change in the plugin.
 *
 * index.html calls in through window.HRPlugins: describe(), unitButton(), menuItems(), decorateComponents().
 * Everything else in this file is private. It reads the page's globals (API_BASE, rackState, comps, …)
 * only at call time, so it can load before the main script.
 */
(function () {
  "use strict";

  const CSS = `
.hr-plug-ed { margin-right: 4px; }
.hr-plug-ed svg { width: 9px; height: 9px; margin: -1px 3px 0 0; vertical-align: middle; }
.hr-plug-missing { display: inline-block; padding: 1px 7px; border-radius: 999px; font-size: 10px; font-weight: 700; line-height: 16px;
  background: rgba(217, 70, 59, 0.18); color: #ff8a7f; margin-right: 4px; letter-spacing: 0.2px; }
.hr-plug-import { grid-column: span 2; display: flex; align-items: center; gap: 10px; width: 100%; text-align: left; cursor: pointer;
  border: 1px dashed var(--line-3); border-radius: 10px; background: transparent; color: var(--text-2); padding: 10px 12px; font: inherit; }
.hr-plug-import:hover { border-color: var(--accent); color: var(--text); background: var(--accent-soft); }
.hr-plug-import .ico { flex: none; width: 28px; height: 28px; border-radius: 8px; display: grid; place-items: center; background: var(--card-2); color: var(--accent); }
.hr-plug-import .ico svg { width: 14px; height: 14px; }
.hr-plug-import b { display: block; font-size: 12px; color: var(--text); font-weight: 650; }
.hr-plug-import span { display: block; font-size: 10.5px; color: var(--text-3); margin-top: 1px; }
.modal.hr-plug-modal { width: min(620px, calc(100vw - 40px)); }
.hr-plug-modal .hr-search { width: 100%; height: 30px; border-radius: 7px; border: 1px solid var(--line-2); background: var(--inset); color: var(--text);
  padding: 0 10px; font-size: 12px; box-sizing: border-box; }
.hr-plug-list { max-height: min(52vh, 460px); overflow-y: auto; margin: 0 -6px; padding: 0 6px; }
.hr-plug-sec { font-size: 10.5px; font-weight: 650; letter-spacing: 0.6px; text-transform: uppercase; color: var(--text-3); padding: 10px 2px 4px; }
.hr-plug-row { display: flex; align-items: center; gap: 10px; padding: 7px 8px; border-radius: 8px; cursor: pointer; }
.hr-plug-row:hover { background: var(--hover); }
.hr-plug-row.off { cursor: default; opacity: 0.55; }
.hr-plug-row input { flex: none; width: 15px; height: 15px; accent-color: var(--accent); margin: 0; }
.hr-plug-row .nm { flex: 1; min-width: 0; }
.hr-plug-row .nm b { display: block; font-size: 12.5px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hr-plug-row .nm span { display: block; font-size: 10.5px; color: var(--text-3); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hr-plug-row .tag { flex: none; font-size: 10px; font-weight: 700; padding: 1px 7px; border-radius: 999px; background: var(--accent-soft); color: var(--accent); }
.hr-plug-row .rm { flex: none; border: none; background: transparent; color: var(--text-3); font-size: 11px; cursor: pointer; padding: 2px 4px; }
.hr-plug-row .rm:hover { color: #ff6b5f; }
.hr-plug-note { font-size: 11px; color: var(--text-3); line-height: 1.45; }
.hr-plug-foot-l { margin-right: auto; align-self: center; font-size: 11px; color: var(--text-3); }
`;
  function injectCSS() {
    if (document.getElementById("hr-plugins-css")) return;
    const st = document.createElement("style");
    st.id = "hr-plugins-css";
    st.textContent = CSS;
    (document.head || document.documentElement).appendChild(st);
  }
  injectCSS();

  const e = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const hash = (s) => { let h = 7; for (const c of String(s)) h = (h * 31 + c.charCodeAt(0)) | 0; return Math.abs(h); };
  const api = async (path) => { const r = await fetch(API_BASE + path, { cache: "no-store" }); if (!r.ok) throw new Error(r.status); return r.json(); };
  const KIND = { effect: "Effect", music_effect: "Music effect", instrument: "Instrument" };
  const PLUG_SVG = `<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><rect x="1.5" y="2" width="9" height="8" rx="1.5"/><path d="M1.5 4.5h9"/></svg>`;
  const PLUS_SVG = `<svg viewBox="0 0 14 14" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M7 2.5v9M2.5 7h9"/></svg>`;

  /* Rack faceplates for plugin units: understated studio-rack finishes, picked per plugin so two
     different plugins never look identical; instruments get the synth violet. */
  const FACES = [
    { face: "#2a2d33", ink: "#e6e6e6", trace: "#9fd3ff" },
    { face: "#30343a", ink: "#e8e8ea", trace: "#9fe0ff" },
    { face: "#23262b", ink: "#e4e4e6", trace: "#ffcf8a" },
    { face: "#363a41", ink: "#eceef2", trace: "#b8f0c8" },
    { face: "#1f2730", ink: "#e2e8ee", trace: "#8ccfff" },
    { face: "#2c2a30", ink: "#ebe8ee", trace: "#f2b3c9" },
    { face: "#d9dadc", ink: "#1b1c1f", trace: "#3a7bd5" },
  ];
  function describe(slot) {
    const key = `${slot.reference_name || ""}|${(slot.plugin && slot.plugin.vendor) || slot.display_name || ""}`;
    const inst = slot.archetype === "plugin_instrument";
    const f = inst ? { face: "#26233a", ink: "#ebe8ff", trace: "#b6a9ff" } : FACES[hash(key) % FACES.length];
    return { form: "rack", face: f.face, ink: f.ink, trace: f.trace, knob: "alu", scale: "arc", font: "cond", plugin: true };
  }

  function unitButton(s) {
    if (!s || !s.plugin || s.slot_idx < 0) return "";
    if (s.plugin.missing)
      return `<span class="hr-plug-missing" title="${e(s.reference_name)} isn’t installed on this Mac. The unit passes audio through and keeps its settings for when it is.">Not installed</span>`;
    return `<button class="more-btn hr-plug-ed${s.plugin.editor_open ? " on" : ""}" data-plugin-editor="${s.slot_idx}" title="Open ${e(s.reference_name)}’s own editor window">${PLUG_SVG}Editor</button>`;
  }

  function menuItems(s) {
    if (!s || !s.plugin) return [];
    return [{ label: s.plugin.missing ? "Plugin not installed" : "Open plugin editor", disabled: !!s.plugin.missing, onClick: () => openEditor(s.slot_idx) }];
  }

  /* ───────────── editor windows ───────────── */
  const bridge = () => window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.headroom;
  async function openEditor(slotIdx) {
    const s = (rackState.slots || []).find((x) => x.slot_idx === slotIdx);
    const p = s && s.plugin;
    if (!p) return;
    if (p.missing) { toast(`${s.reference_name} isn’t installed on this Mac — the unit passes audio through.`); return; }
    if (bridge()) {
      try { bridge().postMessage({ action: "openPluginEditor", instance: p.instance }); } catch (err) { toast("Couldn’t reach the app."); return; }
    } else {
      try {
        const d = await api(`/api/plugins/editor?slot=${slotIdx}`);
        if (!d.ok) { toast(d.error || "Couldn’t open the plugin editor."); return; }
      } catch (err) { toast("Couldn’t reach the engine."); return; }
    }
    startSync(true);
  }

  /* Knobs follow the plugin window: poll the rack while any editor is open (skipped mid-drag/mid-send). */
  let syncTimer = 0, syncGrace = 0;
  function pluginSig() {
    return (rackState.slots || []).filter((s) => s.plugin).map((s) => `${s.slot_idx}:${(s.params || []).map((p) => p.norm.toFixed(4)).join(",")}`).join("|");
  }
  function startSync(fresh) {
    if (fresh) syncGrace = 4; // the window opens asynchronously
    if (syncTimer) return;
    syncTimer = setTimeout(syncTick, 700);
  }
  async function syncTick() {
    syncTimer = 0;
    if (!(typeof drag !== "undefined" && drag) && !(inflight.size || sendQ.size)) {
      try {
        const before = pluginSig();
        applyRackState(await api("/api/rack"));
        if (before && pluginSig() !== before && typeof markDirty === "function") markDirty();
      } catch (err) { /* engine busy */ }
    }
    (rackState.slots || []).forEach((s) => { if (s.plugin) document.querySelectorAll(`[data-plugin-editor="${s.slot_idx}"]`).forEach((b) => b.classList.toggle("on", !!s.plugin.editor_open)); });
    const open = (rackState.slots || []).some((s) => s.plugin && s.plugin.editor_open);
    if (open || syncGrace-- > 0) startSync(false);
  }

  document.addEventListener("click", (ev) => {
    const b = ev.target.closest && ev.target.closest("[data-plugin-editor]");
    if (!b) return;
    ev.preventDefault();
    ev.stopPropagation(); // don't also select / toggle the unit
    openEditor(+b.dataset.pluginEditor);
  }, true);

  /* ───────────── Components → Plugins ───────────── */
  function importTileHTML(hasAny) {
    return `<button type="button" class="hr-plug-import" data-hr-import><span class="ico">${PLUS_SVG}</span><span><b>${hasAny ? "Import more plugins" : "Import plugins"}</b><span>Audio Unit effects and instruments installed on this Mac</span></span></button>`;
  }
  function decorateComponents(box) {
    if (!box || !comps.list.length) return;
    const q = (comps.search || "").trim();
    let grp = box.querySelector('.comp-group[data-cat="Plugins"]');
    if (!grp) {
      if (q) return; // searching: don't add an empty group
      const closed = comps.closed.has("Plugins");
      box.insertAdjacentHTML("beforeend", `<div class="comp-group${closed ? " closed" : ""}" data-cat="Plugins">
        <button class="comp-ghdr" data-cat-toggle="Plugins"><svg viewBox="0 0 10 10" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M2 3.5 5 6.5 8 3.5"/></svg>Plugins<span class="n">0</span></button>
        <div class="comp-grid"></div></div>`);
      grp = box.querySelector('.comp-group[data-cat="Plugins"]');
    }
    const grid = grp.querySelector(".comp-grid");
    if (grid && !grid.querySelector("[data-hr-import]")) grid.insertAdjacentHTML("beforeend", importTileHTML(grid.querySelector(".comp-card")));
  }
  document.addEventListener("click", (ev) => {
    if (ev.target.closest && ev.target.closest("[data-hr-import]")) { ev.preventDefault(); openImport(); }
  });

  /* ───────────── import dialog ───────────── */
  const dlg = { data: null, picked: new Set(), q: "", busy: false };
  async function openImport() {
    dlg.picked.clear();
    dlg.q = "";
    openModal(`<div class="modal-hdr"><div><h3>Import plugins</h3><div class="sub">Choose plugins once — they live in Components → Plugins and drag into any rig.</div></div></div>
      <div class="modal-body"><input type="search" class="hr-search" id="hr-plug-q" placeholder="Search installed plugins" autocomplete="off" spellcheck="false">
      <div class="hr-plug-list" id="hr-plug-list"><div class="comp-empty">Looking for Audio Units…</div></div></div>
      <div class="modal-foot"><span class="hr-plug-foot-l" id="hr-plug-count"></span><button class="btn" data-hr="cancel">Close</button><button class="btn primary" data-hr="import" disabled>Import</button></div>`, "hr-plug-modal");
    const m = $("modal");
    m.onclick = onDialogClick;
    m.oninput = (ev) => { if (ev.target.id === "hr-plug-q") { dlg.q = ev.target.value; renderList(); } };
    m.onchange = (ev) => {
      const cb = ev.target.closest("input[data-uid]");
      if (!cb) return;
      if (cb.checked) dlg.picked.add(cb.dataset.uid); else dlg.picked.delete(cb.dataset.uid);
      renderFoot();
    };
    setTimeout(() => { const i = $("hr-plug-q"); if (i) i.focus(); }, 30);
    await reload();
  }
  async function reload() {
    try { dlg.data = await api("/api/plugins"); }
    catch (err) { const l = $("hr-plug-list"); if (l) l.innerHTML = `<div class="comp-empty">Couldn’t reach the engine.</div>`; return; }
    renderList();
  }
  function rowHTML(p) {
    const imported = p.table_id != null;
    const meta = [p.vendor, KIND[p.kind] || p.kind, p.format === "AU" ? (p.v3 ? "AUv3" : "Audio Unit") : p.format, p.version && p.version !== "0.0.0" ? "v" + p.version : ""].filter(Boolean).join(" · ");
    if (!p.supported)
      return `<div class="hr-plug-row off" title="${e(p.path || "")}"><input type="checkbox" disabled><div class="nm"><b>${e(p.name)}</b><span>${e(meta)}</span></div><span class="tag" style="background:var(--card-2);color:var(--text-3)">Not supported yet</span></div>`;
    if (imported)
      return `<div class="hr-plug-row off"><input type="checkbox" checked disabled><div class="nm"><b>${e(p.name)}</b><span>${e(meta)}</span></div><span class="tag">In Components</span><button class="rm" data-hr-remove="${p.table_id}" title="Remove from Components (rigs that use it keep working)">Remove</button></div>`;
    return `<label class="hr-plug-row"><input type="checkbox" data-uid="${e(p.uid)}"${dlg.picked.has(p.uid) ? " checked" : ""}><div class="nm"><b>${e(p.name)}</b><span>${e(meta)}</span></div></label>`;
  }
  function renderList() {
    const l = $("hr-plug-list");
    if (!l || !dlg.data) return;
    const q = dlg.q.trim().toLowerCase();
    const all = (dlg.data.available || []).filter((p) => !q || `${p.name} ${p.vendor} ${p.kind} ${p.format}`.toLowerCase().includes(q));
    const byName = (a, b) => (a.vendor === "Apple") - (b.vendor === "Apple") || a.name.localeCompare(b.name);
    const sec = (title, list) => list.length ? `<div class="hr-plug-sec">${e(title)} · ${list.length}</div>${list.sort(byName).map(rowHTML).join("")}` : "";
    const fx = all.filter((p) => p.format === "AU" && p.kind !== "instrument");
    const inst = all.filter((p) => p.format === "AU" && p.kind === "instrument");
    const vst = all.filter((p) => p.format !== "AU");
    let html = sec("Effects", fx) + sec("Instruments — for MIDI lanes", inst) + sec("VST3", vst);
    if (!html) html = `<div class="comp-empty">${q ? "No plugins match." : "No Audio Units found."}</div>`;
    html += `<div class="hr-plug-note" style="margin-top:10px">Instruments play the MIDI from a lane’s Pitch → MIDI unit. Plugin settings are saved inside your rigs; a rig whose plugin isn’t installed still opens and passes audio through.</div>`;
    l.innerHTML = html;
    renderFoot();
  }
  function renderFoot() {
    const n = dlg.picked.size, b = $("modal") && $("modal").querySelector("[data-hr='import']"), c = $("hr-plug-count");
    if (b) { b.disabled = !n || dlg.busy; b.textContent = dlg.busy ? "Importing…" : n ? `Import ${n}` : "Import"; }
    if (c) c.textContent = dlg.data ? `${(dlg.data.imported || []).length} in Components` : "";
  }
  async function onDialogClick(ev) {
    const rm = ev.target.closest("[data-hr-remove]");
    if (rm) {
      ev.preventDefault();
      try { await api(`/api/plugins/remove?table_id=${rm.dataset.hrRemove}`); } catch (err) { /* ignore */ }
      await reload();
      fetchComponents();
      return;
    }
    const b = ev.target.closest("[data-hr]");
    if (!b) return;
    if (b.dataset.hr === "cancel") { closeModal(); return; }
    if (b.dataset.hr === "import" && dlg.picked.size && !dlg.busy) {
      dlg.busy = true;
      renderFoot();
      let res = null;
      try { res = await api(`/api/plugins/import?uid=${encodeURIComponent([...dlg.picked].join(","))}`); } catch (err) { /* below */ }
      dlg.busy = false;
      if (!res) { toast("Couldn’t reach the engine."); renderFoot(); return; }
      const ok = (res.results || []).filter((r) => r.table_id != null), bad = (res.results || []).filter((r) => r.table_id == null);
      closeModal();
      comps.closed.delete("Plugins");
      await fetchComponents();
      const g = document.querySelector('#comp-list .comp-group[data-cat="Plugins"]');
      if (g) g.scrollIntoView({ block: "start", behavior: "smooth" });
      toast(bad.length ? `Imported ${ok.length}; ${bad.length} failed (${bad[0].error || "error"})` : `Imported ${ok.length} plugin${ok.length === 1 ? "" : "s"} — drag ${ok.length === 1 ? "it" : "them"} into the chain`);
    }
  }

  // Settings → Plugins (assets/settings.js): the same list and remove path as the import dialog.
  const list = () => api("/api/plugins");
  async function remove(tableId) {
    const d = await api(`/api/plugins/remove?table_id=${encodeURIComponent(tableId)}`);
    if (typeof fetchComponents === "function") fetchComponents();
    return d;
  }
  window.HRPlugins = { describe, unitButton, menuItems, decorateComponents, openImport, openEditor, list, remove };
})();
