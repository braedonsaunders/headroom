/* Library rig info: an "About this tone" button on the current-rig header for Headroom library rigs.
   Shows the rig's description, what inspired it (artist / song / part / year), its research sources (opened in the
   default browser) and the no-affiliation note. Reads index.html's globals (lib, rackState, rigContext) and only
   touches the DOM it creates. */
(() => {
  "use strict";
  const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
  const NOTE = "Artist, song and product names describe the sound only. Headroom is not affiliated with, sponsored or endorsed by any of them.";
  let btn = null, pop = null, shown = null;

  function current() {
    try {
      const c = typeof rigContext === "function" ? rigContext() : null;
      const p = c && c.isLib ? c.cur : null;
      return p && p.source === "library" ? p : null;
    } catch (e) { return null; }
  }
  function openURL(url) {
    let u;
    try { u = new URL(url); } catch (e) { return; }
    if (!/^https?:$/.test(u.protocol)) return;
    const b = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.headroom;
    if (b) { try { b.postMessage({ action: "openURL", url: u.href }); return; } catch (e) { /* fall through */ } }
    window.open(u.href, "_blank", "noopener,noreferrer");
  }
  function host(url) { try { return new URL(url).host.replace(/^www\./, ""); } catch (e) { return url; } }
  function render(p) {
    const ib = p.inspired_by;
    const who = ib && [ib.artist, ib.song && `“${ib.song}”`, ib.album && !ib.song ? ib.album : "", ib.year].filter(Boolean).join(" · ");
    const src = p.sources || [];
    pop.innerHTML = `
      <div class="li-h"><b>${esc(p.name)}</b><span>${esc([p.category, p.style].filter(Boolean).join(" · "))}</span></div>
      ${p.comment ? `<p class="li-d">${esc(p.comment)}</p>` : ""}
      ${who ? `<div class="li-ib"><span class="li-k">Inspired by</span><span>${esc(who)}${ib.part ? `<em> · ${esc(ib.part)}</em>` : ""}</span></div>` : ""}
      ${src.length ? `<div class="li-src"><span class="li-k">Sources</span>${src.map((s, i) => `<a href="${esc(s.url)}" data-url="${esc(s.url)}" title="${esc(s.title ? `${s.title}\n${s.url}` : s.url)}"><b>${i + 1}</b>${esc(s.title || host(s.url))}<i>${esc(host(s.url))}</i></a>`).join("")}</div>` : ""}
      ${who ? `<p class="li-note">${esc(NOTE)}</p>` : ""}`;
  }
  function close() { if (pop) pop.hidden = true; shown = null; }
  function toggle(e) {
    e.stopPropagation();
    e.preventDefault();
    const p = current();
    if (!p || (pop && !pop.hidden)) return close();
    render(p);
    const r = btn.getBoundingClientRect();
    pop.style.left = `${Math.max(8, Math.min(r.left - 8, window.innerWidth - 348))}px`;
    pop.style.top = `${r.bottom + 6}px`;
    pop.hidden = false;
    shown = p.idx;
  }
  function sync() {
    if (!btn) return;
    const p = current();
    btn.hidden = !p;
    if (!p || (shown !== null && shown !== p.idx)) close();
  }
  function boot() {
    const host = document.getElementById("preset-current");
    if (!host || btn) return;
    btn = document.createElement("button");
    btn.type = "button";
    btn.className = "li-btn";
    btn.hidden = true;
    btn.title = "About this tone: description, inspiration and sources";
    btn.setAttribute("aria-label", "About this tone");
    btn.innerHTML = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><circle cx="8" cy="8" r="6.2"/><path d="M8 7.2v4"/><circle cx="8" cy="4.9" r=".55" fill="currentColor" stroke="none"/></svg>`;
    btn.addEventListener("click", toggle);
    btn.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") toggle(e); });
    host.appendChild(btn);
    pop = document.createElement("div");
    pop.className = "li-pop";
    pop.hidden = true;
    pop.setAttribute("role", "dialog");
    pop.addEventListener("click", (e) => {
      e.stopPropagation();
      const a = e.target.closest("a[data-url]");
      if (a) { e.preventDefault(); openURL(a.dataset.url); }
    });
    document.body.appendChild(pop);
    document.addEventListener("click", (e) => { if (pop && !pop.hidden && !pop.contains(e.target) && e.target !== btn) close(); });
    document.addEventListener("keydown", (e) => { if (e.key === "Escape") close(); });
    const name = document.getElementById("pc-name");
    if (name) new MutationObserver(sync).observe(name, { childList: true, characterData: true, subtree: true });
    // the library list can arrive after the rig name; paused while the window or plugin editor is hidden
    if (window.HRActivity && HRActivity.every) HRActivity.every(1500, sync); else setInterval(sync, 1500);
    sync();
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot); else boot();
  window.HRLibraryInfo = { sync, close, note: NOTE };
})();
