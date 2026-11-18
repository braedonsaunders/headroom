/* =====================================================================
   CIRCUIT VIEW: the component page for units that run on the circuit engine (model.kind === "circuit").
   Draws the unit's actual netlist (docs/CIRCUIT_ENGINE.md §6):
     * schematic: stored layout hints or an automatic left-to-right net-bus layout. Rails (DC sources) are
       flags, not wires. Parts are tinted by the solve group the engine put them in.
     * pots show their live wiper position (they move with the unit's knobs).
     * click a part: its values, device model, tolerance and provenance; in edit mode, change values or swap
       devices (edits go through the page's overrides, so My Components / audition work unchanged).
     * click a net: scope it (steady-state waveform for the page's test tone) with its DC operating point.
   Loaded on demand by component-detail.js; no globals besides window.CircuitView.
   ===================================================================== */
(function () {
  "use strict";
  const CSS = `
  .cv-wrap { position: relative; }
  .cv-legend { display: flex; flex-wrap: wrap; gap: 6px 14px; padding: 10px 14px 0; font-size: 11px; color: var(--text-3); }
  .cv-legend .g { display: inline-flex; align-items: center; gap: 6px; }
  .cv-legend .sw { width: 10px; height: 10px; border-radius: 3px; }
  .cv-legend .rail { font-family: ui-monospace, Menlo, monospace; color: var(--text-2); }
  /* the schematic is a pan/zoom canvas: the SVG fills it and its viewBox is the camera */
  .cv-wrap { overflow: hidden; }
  .cv-legend { padding-bottom: 10px; border-bottom: 1px solid var(--line); }
  .cv-canvas { position: relative; height: clamp(440px, calc(100vh - 290px), 1100px); overflow: hidden; cursor: grab; touch-action: none; outline: none; border-radius: 0 0 14px 14px;
    -webkit-user-select: none; user-select: none; --cv-halo: color-mix(in srgb, var(--card) 92%, var(--stage-2, #000)); }
  .cv-canvas.panning, .cv-canvas.panning .part { cursor: grabbing; }
  body.kbd .cv-canvas:focus-visible { box-shadow: inset 0 0 0 2px var(--accent); }
  .cv-canvas svg.cv-svg { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
  .cv-canvas svg.cv-figs { position: absolute; left: 0; top: 0; width: 0; height: 0; overflow: hidden; pointer-events: none; }
  .cv-grid-dot { fill: color-mix(in srgb, var(--text) 13%, transparent); }
  .cv-canvas.zoomed-out .cv-bg { fill: none; }
  .cv-schem text { paint-order: stroke; stroke: var(--cv-halo); stroke-width: 3px; stroke-linejoin: round; }
  .cv-schem .net-lbl-hit { fill: transparent; cursor: crosshair; }
  .cv-zoom { position: absolute; left: 12px; bottom: 12px; z-index: 2; display: flex; align-items: center; gap: 1px; padding: 3px; border-radius: 10px;
    background: color-mix(in srgb, var(--chrome, var(--card)) 88%, transparent); -webkit-backdrop-filter: blur(10px); backdrop-filter: blur(10px);
    box-shadow: inset 0 0 0 1px var(--line-2), 0 8px 20px -12px rgba(0, 0, 0, 0.6); cursor: default; }
  .cv-zoom button { width: 26px; height: 24px; border: 0; border-radius: 7px; background: transparent; color: var(--text-2); display: inline-grid; place-items: center; cursor: pointer; padding: 0; }
  .cv-zoom button:hover { background: var(--hover); color: var(--text); }
  .cv-zoom button svg { width: 14px; height: 14px; }
  .cv-zoom .pct { min-width: 42px; text-align: center; font: 600 11px var(--font-ui, system-ui); color: var(--text-2); font-variant-numeric: tabular-nums; }
  .cv-zoom i { width: 1px; height: 14px; margin: 0 3px; background: var(--line-2); }
  .cv-hint { position: absolute; right: 12px; bottom: 12px; z-index: 1; font-size: 10.5px; color: var(--text-3); pointer-events: none; padding: 5px 10px; border-radius: 8px;
    background: color-mix(in srgb, var(--chrome, var(--card)) 88%, transparent); box-shadow: inset 0 0 0 1px var(--line); transition: opacity 0.4s; }
  .cv-dr-open .cv-hint, .cv-canvas.hinted .cv-hint { opacity: 0; }
  .cv-schem .bus { stroke: var(--wire, var(--text-3)); stroke-width: 1.6; fill: none; stroke-linecap: round; }
  .cv-schem .stub { stroke: var(--wire, var(--text-3)); stroke-width: 1.4; fill: none; stroke-linecap: round; stroke-linejoin: round; }
  .cv-schem .dot { fill: var(--wire, var(--text-3)); }
  .mod-tag { margin-left: 6px; padding: 0 5px; border-radius: 6px; font-size: 10px; letter-spacing: .06em; text-transform: uppercase; color: var(--warn, #e0a040); border: 1px solid currentColor; vertical-align: 1px; }
  .cv-schem .net-hit { stroke: transparent; stroke-width: 14; cursor: crosshair; }
  .cv-schem .net-lbl { font: 600 9.5px ui-monospace, Menlo, monospace; fill: var(--text-3); }
  .cv-schem .net-dc { font: 500 9px ui-monospace, Menlo, monospace; fill: color-mix(in srgb, var(--accent) 70%, var(--text-3)); }
  .cv-schem .net.probed .bus { stroke-width: 2.6; }
  .cv-schem .part { cursor: pointer; }
  .cv-schem .part .body { fill: none; stroke: var(--text); stroke-width: 1.6; stroke-linejoin: round; stroke-linecap: round; }
  .cv-schem .part .fillb { fill: var(--text); stroke: none; }
  .cv-schem .part .hit { fill: transparent; }
  .cv-schem .part .ref { font: 700 10.5px var(--font-ui, system-ui); fill: var(--text); }
  .cv-schem .part .val { font: 500 10px var(--font-ui, system-ui); fill: var(--text-2); }
  .cv-schem .part .ctl { font: 600 9.5px var(--font-ui, system-ui); fill: var(--accent); }
  .cv-schem .part.sel .body { stroke: var(--accent); stroke-width: 2.2; }
  .cv-schem .part.mod .ref { fill: var(--accent); }
  .cv-schem .part:hover .body { stroke-width: 2.2; }
  .cv-schem .wiper { stroke: var(--accent); stroke-width: 1.8; fill: none; }
  .cv-schem .wiper-head { fill: var(--accent); }
  .cv-schem .rail-flag rect { fill: color-mix(in srgb, var(--card) 70%, transparent); stroke: var(--line-2); }
  .cv-schem .rail-flag text { font: 600 9px ui-monospace, Menlo, monospace; fill: var(--text-2); }
  .cv-schem .gnd { stroke: var(--text-3); stroke-width: 1.4; fill: none; }
  .cv-schem .jack text { font: 700 10px var(--font-ui, system-ui); fill: var(--text-2); letter-spacing: .08em; }
  .cv-schem .jack circle { fill: none; stroke: var(--text-2); stroke-width: 1.6; }
  .cv-scope { margin-top: 12px; }
  .cv-insp, .cv-scope { padding: 14px 16px; min-height: 120px; }
  .cv-insp h3, .cv-scope h3 { margin: 0 0 8px; font-size: 13px; font-weight: 700; color: var(--text); display: flex; gap: 8px; align-items: baseline; }
  .cv-insp h3 span, .cv-scope h3 span { font-size: 11px; font-weight: 500; color: var(--text-3); }
  .cv-row { display: grid; grid-template-columns: 110px 1fr; gap: 4px 10px; font-size: 12px; padding: 3px 0; }
  .cv-row .k { color: var(--text-3); }
  .cv-row .v { color: var(--text); font-variant-numeric: tabular-nums; }
  .cv-row .n { grid-column: 2; color: var(--text-3); font-size: 11px; }
  .cv-ed { display: flex; gap: 8px; align-items: center; margin-top: 4px; grid-column: 2; }
  .cv-ed input[type=range] { flex: 1; }
  .cv-ed input[type=text] { width: 86px; }
  .cv-ed select { max-width: 220px; }
  .cv-ed button { font-size: 11px; }
  .cv-empty { color: var(--text-3); font-size: 12px; padding: 6px 0; }
  .cv-trace { display: grid; grid-template-columns: 92px 1fr; align-items: center; gap: 8px; margin: 4px 0; }
  .cv-trace .nm { font: 600 11px ui-monospace, Menlo, monospace; }
  .cv-trace .st { font: 500 10px ui-monospace, Menlo, monospace; color: var(--text-3); }
  .cv-trace svg { width: 100%; height: 54px; background: color-mix(in srgb, var(--card) 60%, transparent); border-radius: 6px; }
  .cv-trace .grid { stroke: var(--line); stroke-width: 1; }
  .cv-trace .ln { fill: none; stroke-width: 1.6; }
  .cv-groups { font-size: 11px; color: var(--text-3); margin-top: 8px; line-height: 1.6; }
  .cv-src { font-size: 11px; color: var(--text-3); margin-top: 10px; line-height: 1.5; }
  .cv-src a { color: var(--accent); }
  .cv-schem .part.pot-drag { cursor: ns-resize; }
  .cv-err { color: #e5534b; font-size: 12px; margin: 4px 0 8px; }
  .cv-warn { color: var(--text-3); font-size: 11px; }
  .cv-import { margin-left: auto; }
  .cv-schem .rail-flag.part.sel rect { stroke: var(--accent); stroke-width: 1.4; }
  .cv-schem .rail-flag.part:hover rect { stroke: var(--text-2); }
  .cv-schem .part.sel .ref { fill: var(--accent); }
  .cv-schem .part.sel .hit { fill: color-mix(in srgb, var(--accent) 12%, transparent); stroke: color-mix(in srgb, var(--accent) 55%, transparent); stroke-width: 1; rx: 6; }

  /* ---- part drawer ---- */
  .cv-drawer {
    position: absolute; top: var(--cv-dr-top, 40px); right: 0; bottom: 0; z-index: 6; width: var(--cv-dr-w, 460px); max-width: calc(100% - 40px);
    display: flex; flex-direction: column; color: var(--text);
    background: linear-gradient(180deg, color-mix(in srgb, var(--card) 97%, var(--text) 3%), var(--card));
    border-left: 1px solid var(--line-2);
    box-shadow: -28px 0 60px -26px rgba(0, 0, 0, 0.75), -1px 0 0 rgba(255, 255, 255, 0.02);
    transform: translateX(calc(100% + 30px)); visibility: hidden;
    transition: transform 0.36s cubic-bezier(0.22, 0.8, 0.24, 1), visibility 0s linear 0.36s;
    outline: none;
  }
  body.light .cv-drawer { box-shadow: -24px 0 50px -26px rgba(20, 25, 40, 0.45); }
  .cv-drawer.open { transform: none; visibility: visible; transition: transform 0.36s cubic-bezier(0.22, 0.8, 0.24, 1), visibility 0s; }
  .cv-dr-head { flex-shrink: 0; display: flex; align-items: center; gap: 8px; height: 44px; padding: 0 10px 0 18px; border-bottom: 1px solid var(--line); }
  .cv-dr-head .crumb { font-size: 11px; font-weight: 650; letter-spacing: 0.08em; text-transform: uppercase; color: var(--text-3); flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .cv-dr-head .crumb b { color: var(--text-2); font-weight: 700; }
  .cv-dr-nav, .cv-dr-x { width: 28px; height: 28px; border-radius: 8px; border: 0; background: transparent; color: var(--text-2); display: inline-grid; place-items: center; cursor: pointer; }
  .cv-dr-nav:hover, .cv-dr-x:hover { background: var(--hover); color: var(--text); }
  .cv-dr-nav svg, .cv-dr-x svg { width: 15px; height: 15px; }
  .cv-dr-scroll { flex: 1; overflow-y: auto; overflow-x: hidden; overscroll-behavior: contain; padding: 14px 16px 28px; }
  .cv-dr-hero {
    position: relative; border-radius: 16px; overflow: hidden; padding: 16px 12px 8px; isolation: isolate;
    background: radial-gradient(120% 92% at 50% 16%, #2d2f35 0%, #1a1b1f 60%, #121316 100%);
    box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.05), inset 0 0 0 1px rgba(255, 255, 255, 0.04);
  }
  body.light .cv-dr-hero { background: radial-gradient(120% 92% at 50% 16%, #ffffff 0%, #eef0f3 58%, #e1e4e9 100%); box-shadow: inset 0 0 0 1px rgba(15, 20, 35, 0.06); }
  .cv-dr-hero::after { content: ""; position: absolute; left: 0; right: 0; bottom: 0; height: 30%; background: linear-gradient(180deg, transparent, rgba(0, 0, 0, 0.18)); pointer-events: none; z-index: -1; }
  body.light .cv-dr-hero::after { background: linear-gradient(180deg, transparent, rgba(20, 25, 40, 0.05)); }
  .cv-dr-hero .cp-art { width: 100%; height: auto; }
  .cv-dr-hero.swap .cp-art { animation: cv-dr-swap 0.28s ease-out; }
  @keyframes cv-dr-swap { from { opacity: 0; transform: translateY(6px) scale(0.985); } to { opacity: 1; transform: none; } }
  .cv-dr-hero .ph { height: 220px; display: grid; place-items: center; color: var(--text-3); font-size: 12px; }
  .cv-dr-chips { position: absolute; top: 10px; left: 12px; display: flex; flex-wrap: wrap; gap: 5px; max-width: calc(100% - 24px); pointer-events: none; }
  .cv-dr-chips span { font: 650 10px var(--font-ui, system-ui); letter-spacing: 0.04em; padding: 3px 7px; border-radius: 6px; color: var(--text-2); background: color-mix(in srgb, var(--card) 70%, transparent); border: 1px solid var(--line-2); }
  .cv-dr-title { padding: 14px 2px 4px; }
  .cv-dr-title .ref { display: flex; align-items: center; gap: 9px; font: 760 24px/1.1 var(--font-ui, system-ui); letter-spacing: -0.01em; }
  .cv-dr-title .ref .gdot { width: 9px; height: 9px; border-radius: 50%; background: var(--gc, var(--text-3)); box-shadow: 0 0 0 3px color-mix(in srgb, var(--gc, var(--text-3)) 22%, transparent); }
  .cv-dr-title .ref .mod { font: 650 10px var(--font-ui, system-ui); letter-spacing: 0.06em; text-transform: uppercase; color: var(--accent); border: 1px solid color-mix(in srgb, var(--accent) 50%, transparent); border-radius: 5px; padding: 2px 5px; }
  .cv-dr-title .nm { margin-top: 5px; font-size: 14.5px; font-weight: 640; color: var(--text); }
  .cv-dr-title .lbl { margin-top: 3px; font-size: 12.5px; color: var(--text-2); }
  .cv-dr-title .pkg { margin-top: 4px; font-size: 11.5px; color: var(--text-3); line-height: 1.45; }
  .cv-bands { display: flex; flex-wrap: wrap; gap: 6px; margin: 8px 2px 2px; }
  .cv-bands span { display: inline-flex; align-items: center; gap: 5px; font-size: 11px; color: var(--text-2); padding: 3px 8px 3px 4px; border-radius: 999px; background: var(--card-2, var(--hover)); border: 1px solid var(--line); }
  .cv-bands i { width: 12px; height: 12px; border-radius: 4px; background: var(--b); box-shadow: inset 0 0 0 1px rgba(0, 0, 0, 0.25); }
  .cv-dr-body { margin-top: 10px; border-top: 1px solid var(--line); }
  .cv-drawer .cv-row { grid-template-columns: 104px 1fr; padding: 9px 2px; border-bottom: 1px solid var(--line); font-size: 12.5px; }
  .cv-drawer .cv-row .k { font-size: 11.5px; padding-top: 1px; }
  .cv-drawer .cv-ed { margin-top: 8px; }
  .cv-drawer .cv-ed input[type=range] { accent-color: var(--accent); min-width: 0; }
  .cv-drawer .cv-ed input[type=text] { width: 92px; height: 26px; border-radius: 7px; border: 1px solid var(--line-2); background: var(--inset, transparent); color: var(--text); padding: 0 8px; font: 12px ui-monospace, Menlo, monospace; }
  .cv-drawer .cv-ed select { height: 28px; border-radius: 7px; border: 1px solid var(--line-2); background: var(--inset, transparent); color: var(--text); padding: 0 6px; font-size: 12px; max-width: 100%; }
  .cv-nodes { display: flex; flex-wrap: wrap; gap: 6px; }
  .cv-nodes span { display: inline-flex; align-items: baseline; gap: 6px; padding: 3px 8px; border-radius: 7px; background: var(--card-2, var(--hover)); border: 1px solid var(--line); }
  .cv-nodes code { font: 600 12px ui-monospace, Menlo, monospace; color: var(--text); }
  .cv-nodes em { font: 500 11px ui-monospace, Menlo, monospace; font-style: normal; color: var(--accent); }
  .cv-kvs { display: grid; grid-template-columns: repeat(auto-fill, minmax(92px, 1fr)); gap: 5px; }
  .cv-kvs span { padding: 4px 7px; border-radius: 7px; background: var(--card-2, var(--hover)); border: 1px solid var(--line); font: 500 11px ui-monospace, Menlo, monospace; color: var(--text); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .cv-kvs b { display: block; font: 650 9.5px var(--font-ui, system-ui); letter-spacing: 0.06em; text-transform: uppercase; color: var(--text-3); margin-bottom: 1px; }
  .cv-wiper { display: flex; align-items: center; gap: 8px; }
  .cv-wiper .bar { flex: 1; height: 6px; border-radius: 3px; background: var(--line-2); overflow: hidden; }
  .cv-wiper .bar i { display: block; height: 100%; background: var(--accent); border-radius: 3px; }
  @media (prefers-reduced-motion: reduce) {
    .cv-drawer, .cv-drawer.open { transition: none; }
    .cv-dr-hero.swap .cp-art { animation: none; }
  }

  /* the circuit's title line keeps its own row under the page's sticky sub-tabs (never scrolls half under them) */
  .cd-pane > .cv-head { position: sticky; top: var(--cv-tabs-h, 43px); z-index: 4; margin: -10px -32px 2px; padding: 10px 32px 10px;
    background: linear-gradient(color-mix(in srgb, var(--chrome, var(--card)) 84%, transparent), color-mix(in srgb, var(--chrome, var(--card)) 84%, transparent)), var(--stage-2, var(--card)); } /* opaque: nothing shows through it */
  .cd-pane > .cv-head { flex-wrap: nowrap; align-items: baseline; }
  .cd-pane > .cv-head h2 { flex-shrink: 0; white-space: nowrap; }
  .cd-pane > .cv-head .s { flex: 1 1 0; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .cd-pane > .cv-head .cv-import { flex-shrink: 0; }

  /* ---- circuit editor (edit mode): parts palette, pins, wiring, status ---- */
  .cv-tools { position: absolute; left: 12px; top: 12px; z-index: 3; width: 120px; padding: 6px; border-radius: 12px; cursor: default;
    background: color-mix(in srgb, var(--chrome, var(--card)) 90%, transparent); -webkit-backdrop-filter: blur(10px); backdrop-filter: blur(10px);
    box-shadow: inset 0 0 0 1px var(--line-2), 0 10px 26px -14px rgba(0, 0, 0, 0.7); }
  .cv-tools .row { display: flex; gap: 2px; padding-bottom: 6px; margin-bottom: 6px; border-bottom: 1px solid var(--line); }
  .cv-tools .row button { flex: 1; height: 26px; border: 0; border-radius: 7px; background: transparent; color: var(--text-2); display: inline-grid; place-items: center; cursor: pointer; padding: 0; }
  .cv-tools .row button:hover:not(:disabled) { background: var(--hover); color: var(--text); }
  .cv-tools .row button:disabled { opacity: 0.35; cursor: default; }
  .cv-tools .row button svg { width: 15px; height: 15px; }
  .cv-tools .cap { font: 650 9.5px var(--font-ui, system-ui); letter-spacing: 0.08em; text-transform: uppercase; color: var(--text-3); padding: 0 4px 5px; }
  .cv-pal { display: grid; grid-template-columns: 1fr 1fr; gap: 3px; }
  .cv-pal button { display: flex; flex-direction: column; align-items: center; gap: 2px; padding: 5px 2px 4px; border: 0; border-radius: 8px; background: transparent; color: var(--text-2);
    cursor: grab; touch-action: none; font: 600 9.5px var(--font-ui, system-ui); }
  .cv-pal button:hover { background: var(--hover); color: var(--text); }
  .cv-pal button svg { width: 40px; height: 20px; overflow: visible; }
  .cv-pal svg .s { fill: none; stroke: currentColor; stroke-width: 1.4; stroke-linecap: round; stroke-linejoin: round; }
  .cv-ghost { position: fixed; z-index: 50; pointer-events: none; padding: 6px 9px; border-radius: 9px; font: 650 11px var(--font-ui, system-ui); color: var(--text);
    background: color-mix(in srgb, var(--card) 92%, transparent); box-shadow: inset 0 0 0 1px var(--accent), 0 10px 24px -10px rgba(0, 0, 0, 0.7); transform: translate(-50%, -130%); white-space: nowrap; }
  .cv-ghost.off { box-shadow: inset 0 0 0 1px var(--line-2), 0 10px 24px -10px rgba(0, 0, 0, 0.7); color: var(--text-3); }
  .cv-schem .cv-pin { fill: var(--card); stroke: var(--text-3); stroke-width: 1.2; cursor: crosshair; }
  .cv-schem .cv-pin:hover, .cv-schem .cv-pin.cv-target { fill: var(--accent); stroke: var(--accent); }
  .cv-schem .cv-pin.nc { fill: #e5534b; stroke: #e5534b; }
  .cv-schem .net.dangling .bus { stroke: #e5534b; stroke-dasharray: 3 3; }
  .cv-schem .net.dangling .net-lbl { fill: #e5534b; }
  .cv-schem .net.cv-target .bus { stroke: var(--accent); stroke-width: 3; }
  .cv-schem .rail-flag.cv-target rect, .cv-schem .gnd-g.cv-target .gnd { stroke: var(--accent); stroke-width: 2; }
  .cv-schem .gnd-hit { fill: transparent; }
  .cv-schem .cv-rubber { stroke: var(--accent); stroke-width: 1.6; stroke-dasharray: 5 4; fill: none; pointer-events: none; }
  .cv-canvas.wiring, .cv-canvas.wiring * { cursor: crosshair !important; }
  .cv-schem .part.added .ref { fill: var(--accent); }
  .cv-status { display: flex; flex-wrap: wrap; align-items: center; gap: 6px 10px; padding: 8px 14px; border-bottom: 1px solid var(--line); font-size: 11.5px; color: var(--text-2); min-height: 22px; }
  .cv-status[hidden], .cv-tools[hidden] { display: none; }
  .cv-canvas.docked svg.cv-svg { left: 144px; width: calc(100% - 144px); border-left: 1px solid var(--line); }
  .cv-canvas.docked .cv-tools { box-shadow: inset 0 0 0 1px var(--line-2); }
  .cv-wrap.cv-dr-open .cv-status { padding-right: min(46%, 450px); } /* the part drawer covers the right side */
  .cv-status .chip { display: inline-flex; align-items: center; gap: 6px; font-weight: 650; padding: 2px 9px 2px 7px; border-radius: 999px; border: 1px solid var(--line-2); white-space: nowrap; }
  .cv-status .chip i { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
  .cv-status .chip.ok { color: #3fb27f; } .cv-status .chip.busy { color: var(--text-2); } .cv-status .chip.wait { color: #d29922; } .cv-status .chip.bad { color: #e5534b; }
  .cv-status .msgs { flex: 1 1 320px; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .cv-status .msgs div { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .cv-status .msgs .b { color: #e5534b; } .cv-status .msgs .w { color: #d29922; } .cv-status .msgs .e { color: #e5534b; white-space: normal; }
  .cv-status .msgs button { border: 0; background: none; padding: 0; color: inherit; font: inherit; text-decoration: underline dotted; cursor: pointer; }
  .cv-status .rm { display: inline-flex; gap: 4px; align-items: center; color: var(--text-3); }
  .cv-status .rm button { border: 1px solid var(--line-2); background: transparent; color: var(--text-2); border-radius: 6px; font: 600 10.5px var(--font-ui, system-ui); padding: 1px 6px; cursor: pointer; }
  .cv-status .rm button:hover { color: var(--text); border-color: var(--text-3); }
  .cv-netpop { position: absolute; z-index: 7; padding: 10px; width: 230px; border-radius: 11px; background: var(--card); box-shadow: inset 0 0 0 1px var(--line-2), 0 16px 34px -14px rgba(0, 0, 0, 0.75); cursor: default; }
  .cv-netpop .h { font: 650 10px var(--font-ui, system-ui); letter-spacing: 0.08em; text-transform: uppercase; color: var(--text-3); margin-bottom: 6px; }
  .cv-netpop input { width: 100%; box-sizing: border-box; height: 28px; border-radius: 7px; border: 1px solid var(--line-2); background: var(--inset, transparent); color: var(--text); padding: 0 8px; font: 12px ui-monospace, Menlo, monospace; }
  .cv-netpop .n { font-size: 11px; color: var(--text-3); margin: 6px 0 8px; min-height: 14px; }
  .cv-netpop .b { display: flex; gap: 6px; justify-content: flex-end; }
  .cv-pins { display: grid; grid-template-columns: auto 1fr auto; gap: 5px 8px; align-items: center; }
  .cv-pins label { font: 600 11px ui-monospace, Menlo, monospace; color: var(--text-3); }
  .cv-pins input { height: 26px; min-width: 0; border-radius: 7px; border: 1px solid var(--line-2); background: var(--inset, transparent); color: var(--text); padding: 0 8px; font: 12px ui-monospace, Menlo, monospace; }
  .cv-pins input.nc { border-color: #e5534b; }
  .cv-pins em { font: 500 11px ui-monospace, Menlo, monospace; font-style: normal; color: var(--accent); min-width: 44px; text-align: right; }
  .cv-dr-actions { display: flex; gap: 8px; padding: 14px 2px 0; }

  /* ---- view toolbar: drawing style + power supply ---- */
  .cv-views { position: absolute; right: 12px; top: 12px; z-index: 3; display: flex; align-items: center; gap: 6px; cursor: default; }
  .cv-views .seg, .cv-views .cv-sup { display: flex; padding: 3px; border-radius: 10px; background: color-mix(in srgb, var(--chrome, var(--card)) 88%, transparent);
    -webkit-backdrop-filter: blur(10px); backdrop-filter: blur(10px); box-shadow: inset 0 0 0 1px var(--line-2), 0 8px 20px -12px rgba(0, 0, 0, 0.6); }
  .cv-views .seg button { height: 24px; padding: 0 11px; border: 0; border-radius: 7px; background: transparent; color: var(--text-2); font: 600 11.5px var(--font-ui, system-ui); cursor: pointer; }
  .cv-views .seg button:hover { color: var(--text); }
  .cv-views .seg button[aria-pressed="true"] { background: var(--hover); color: var(--text); box-shadow: inset 0 0 0 1px var(--line-2); }
  .cv-views .cv-sup { align-items: center; gap: 6px; height: 30px; box-sizing: border-box; padding: 0 11px 0 9px; border: 0; color: var(--text-2); font: 600 11.5px var(--font-ui, system-ui); cursor: pointer; }
  .cv-views .cv-sup svg { width: 14px; height: 14px; }
  .cv-views .cv-sup:hover { color: var(--text); }
  .cv-views .cv-sup[aria-pressed="true"] { color: var(--accent); box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--accent) 55%, transparent), 0 8px 20px -12px rgba(0, 0, 0, 0.6); }
  .cv-views .cv-sup[hidden] { display: none; }
  .cv-wrap.cv-dr-open .cv-views { opacity: 0; pointer-events: none; }
  /* ---- the power supply section ---- */
  .cv-schem .cv-sec-frame { fill: color-mix(in srgb, var(--text) 2.5%, transparent); stroke: var(--line-2); stroke-width: 1; stroke-dasharray: 5 5; }
  .cv-schem .cv-sec-t { font: 750 11px var(--font-ui, system-ui); letter-spacing: .12em; fill: var(--accent); }
  .cv-schem .cv-sec-s { font: 500 10.5px var(--font-ui, system-ui); letter-spacing: 0; fill: var(--text-3); }
  .cv-schem .rail-flag.supply-link { cursor: pointer; }
  .cv-schem .rail-flag.supply-link rect { stroke: color-mix(in srgb, var(--accent) 60%, var(--line-2)); }
  .cv-schem .rail-flag.supply-link text { fill: color-mix(in srgb, var(--accent) 75%, var(--text-2)); }
  .cv-schem .rail-flag.supply-link:hover rect { stroke: var(--accent); fill: color-mix(in srgb, var(--accent) 14%, var(--card)); }
  .cv-schem .net.cv-flash .bus { animation: cv-flash 0.6s ease-in-out 3; }
  @keyframes cv-flash { 50% { stroke: var(--accent); stroke-width: 5; } }
  /* ---- Illustrated view: the same layout, text at ILLUS_TS (0.625) of the schematic's size, parts drawn as the real thing ---- */
  .cv-illus .cv-schem text, .cv-schem.cv-illus text { stroke-width: 1.9px; }
  .cv-schem.cv-illus .part .ref { font-size: 6.56px; }
  .cv-schem.cv-illus .part .val { font-size: 6.25px; }
  .cv-schem.cv-illus .part .ctl { font-size: 5.94px; }
  .cv-schem.cv-illus .net-lbl { font-size: 5.94px; }
  .cv-schem.cv-illus .net-dc, .cv-schem.cv-illus .rail-flag text { font-size: 5.63px; }
  .cv-schem.cv-illus .jack text { font-size: 6.25px; }
  .cv-schem.cv-illus .cv-sec-t { font-size: 6.9px; } .cv-schem.cv-illus .cv-sec-s { font-size: 6.56px; }
  .cv-schem.cv-illus .bus { stroke-width: 1.1; } .cv-schem.cv-illus .stub, .cv-schem.cv-illus .gnd { stroke-width: 1; }
  .cv-schem.cv-illus .net.probed .bus { stroke-width: 1.8; }
  .cv-schem.cv-illus .lead-o { fill: none; stroke: rgba(10, 11, 13, 0.7); stroke-width: 1.45; stroke-linecap: round; }
  .cv-schem.cv-illus .lead { fill: none; stroke: #c5cad0; stroke-width: 0.8; stroke-linecap: round; }
  body.light .cv-schem.cv-illus .lead-o { stroke: rgba(40, 44, 52, 0.55); }
  .cv-schem.cv-illus .part .hit { rx: 4; }
  .cv-schem.cv-illus .part:hover .hit { fill: color-mix(in srgb, var(--text) 5%, transparent); stroke: color-mix(in srgb, var(--text) 18%, transparent); stroke-width: 0.6; }
  .cv-schem.cv-illus .part.sel .hit { fill: color-mix(in srgb, var(--accent) 10%, transparent); stroke: color-mix(in srgb, var(--accent) 70%, transparent); stroke-width: 0.9; }
  .cv-schem.cv-illus .cp-glow { animation: none; } /* no idle animation on the canvas */
  .cv-schem.cv-illus .cv-pin { stroke-width: 0.8; }
  .cv-canvas:not(.cv-lod):not(.cv-zooming) .art .r, .cv-canvas:not(.cv-rready) .art .r { display: none; }
  .cv-canvas.cv-lod.cv-rready .art .v, .cv-canvas.cv-zooming.cv-rready .art .v { display: none; }
  .cv-canvas.cv-zooming .art .r image { image-rendering: optimizeSpeed; } /* only while the scale is moving */
  .cd-btn.danger, .cv-dr-actions .danger { color: #e5534b; }
  `;
  const BASE = ((document.currentScript && document.currentScript.src) || "").replace(/[^/]*$/, "") || "assets/";
  const GROUP_COLORS = ["#3987e5", "#d95926", "#199e70", "#c98500", "#d55181", "#7a6ff0", "#2aa6b8", "#9b7b4e"];
  const TRACE_COLORS = ["var(--cd-s1, #3987e5)", "var(--cd-s2, #d95926)", "var(--cd-s3, #199e70)", "var(--cd-s4, #c98500)", "var(--cd-s5, #d55181)"];
  const esc = (s) => String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const isGnd = (n) => ["0", "gnd", "ground"].includes(String(n).toLowerCase());

  const V = { sel: null, probes: null, curves: null, model: null, ctx: null, host: null, layoutKey: "", dragMoved: false, dragEnd: 0,
    geo: null, cam: null, camFor: null, cams: {},
    drawer: null, heroKey: "", bodyKey: "", drawerBusy: false, drawerPending: false, knobT: 0,
    // circuit editor: draft topology edits, undo / redo, placement hints for new parts' nets, engine status
    srcModel: null, topo: {}, appliedTopo: {}, appliedKey: "null", hist: [], fut: [], hints: {}, status: null, lint: null, cseq: 0, baseNet: null,
    // the canvas's drawing style ("schematic" | "illus") and whether a modelled power supply is laid out as its own section
    view: "schematic", supply: true };
  try {
    V.view = localStorage.getItem("headroom.circuit.view") === "illus" ? "illus" : "schematic";
    V.supply = localStorage.getItem("headroom.circuit.supply") !== "0";
  } catch (e) { /* private mode */ }

  function injectCss() {
    if (document.getElementById("cv-css")) return;
    const s = document.createElement("style");
    s.id = "cv-css";
    s.textContent = CSS;
    document.head.appendChild(s);
  }

  /* ---------------- topology: columns, rows, tracks ---------------- */
  // Columns: longest-path layering of a signal-flow DAG. Active devices point inputs -> output; passive parts
  // point from the net discovered first (BFS from the input jack) to the other. Rails are not nets.
  // Rows: the nets on the shortest input -> output path share row 0, so the main signal runs along the top.
  function topology(c) {
    const parts = c.parts.filter((p) => p.type !== "V");
    const rails = {};
    c.parts.filter((p) => p.type === "V").forEach((p) => { rails[p.nodes[0]] = p; });
    const isRail = (n) => isGnd(n) || rails[n] != null;
    const nets = [];
    const seen = new Set();
    const addNet = (n) => { if (!isRail(n) && !seen.has(n)) { seen.add(n); nets.push(n); } };
    addNet(c.io.input);
    parts.forEach((p) => p.nodes.forEach(addNet));
    addNet(c.io.output);
    const adj = Object.fromEntries(nets.map((n) => [n, new Set()]));
    const sig = (p) => p.nodes.filter((n) => !isRail(n));
    parts.forEach((p) => { const s = sig(p); s.forEach((a) => s.forEach((b) => { if (a !== b) adj[a].add(b); })); });
    const depth = { [c.io.input]: 0 }, prev = {};
    const q = [c.io.input];
    while (q.length) { const n = q.shift(); adj[n].forEach((m) => { if (depth[m] == null) { depth[m] = depth[n] + 1; prev[m] = n; q.push(m); } }); }
    nets.forEach((n) => { if (depth[n] == null) depth[n] = 0; });
    const edges = [];
    const io = (p) => {
      if (p.type === "opamp") return { ins: [p.nodes[0], p.nodes[1]], outs: [p.nodes[2]] };
      if (p.type === "triode" || p.type === "pentode") return { ins: [p.nodes[p.type === "pentode" ? 2 : 1]], outs: [p.nodes[0], p.nodes[p.nodes.length - 1]] };
      if (/^(npn|pnp|njf|pjf)$/.test(p.type)) return { ins: [p.nodes[1]], outs: [p.nodes[0], p.nodes[2]] };
      return null;
    };
    parts.forEach((p) => {
      const a = io(p);
      if (a) { a.ins.forEach((i) => a.outs.forEach((o) => { if (!isRail(i) && !isRail(o) && i !== o) edges.push([i, o]); })); return; }
      const s = sig(p);
      for (let i = 0; i < s.length; i++) for (let j = i + 1; j < s.length; j++) {
        const [x, y] = depth[s[i]] <= depth[s[j]] ? [s[i], s[j]] : [s[j], s[i]];
        if (depth[x] < depth[y]) edges.push([x, y]);
      }
    });
    const col = Object.fromEntries(nets.map((n) => [n, depth[n]])); // seed with BFS depth, then longest path
    for (let it = 0; it < nets.length + 2; it++) {
      let moved = false;
      edges.forEach(([a, b]) => { if (col[b] < col[a] + 1 && col[a] + 1 < nets.length + 1) { col[b] = col[a] + 1; moved = true; } });
      if (!moved) break;
    }
    const hints = (c.layout && c.layout.columns) || {};
    Object.keys(hints).forEach((n) => { if (n in col) col[n] = hints[n]; });
    const used = [...new Set(Object.values(col))].sort((a, b) => a - b);
    const rawCol = { ...col }; // before compaction: the space layout.columns hints are written in (the editor's drop hints)
    nets.forEach((n) => { col[n] = used.indexOf(col[n]); });
    const main = new Set();
    for (let n = c.io.output; n != null && depth[n] != null; n = prev[n]) { main.add(n); if (n === c.io.input) break; }
    const row = {}, rowsIn = {};
    nets.forEach((n) => { if (main.has(n)) row[n] = 0; });
    nets.forEach((n) => { if (row[n] == null) { const k = col[n]; rowsIn[k] = (rowsIn[k] || 0) + 1; row[n] = rowsIn[k]; } });
    const rowHints = (c.layout && c.layout.rows) || {};
    Object.keys(rowHints).forEach((n) => { if (n in row) row[n] = rowHints[n]; });
    const ncol = used.length;
    const tracks = Array.from({ length: ncol }, () => []);
    nets.forEach((n) => tracks[col[n]].push(n));
    tracks.forEach((t) => t.sort((a, b) => row[a] - row[b]));
    // the highest positive supply hangs above the parts it feeds; every other rail (ground, bias, Vref) below
    const vmax = Math.max(0, ...Object.values(rails).map((r) => +r.value || 0));
    const railUp = (n) => !isGnd(n) && rails[n] && +rails[n].value > 0 && +rails[n].value >= vmax - 1e-9;
    return { nets, col, rawCol, row, rails, isRail, railUp, parts, tracks, ncol, main };
  }

  /* ---------------- placement: every part, label and wire gets free space ---------------- */
  // Occupancy map of what is already drawn. Kinds: b = body/label/flag (solid), h/v = wire segment of net n,
  // j = junction (where a wire lands on a net's bus). Wires may cross; they may not run on top of each other,
  // end on a wire of another net (that would read as a connection) or pass through a part or a label.
  function occMap() {
    const G = 64, cells = new Map(), all = [];
    const each = (r, f) => {
      for (let gx = Math.floor(r.x0 / G); gx <= Math.floor(r.x1 / G); gx++)
        for (let gy = Math.floor(r.y0 / G); gy <= Math.floor(r.y1 / G); gy++) if (f((gx + 4096) * 8192 + gy + 4096)) return true;
      return false;
    };
    return {
      all,
      add(it) { all.push(it); each(it, (k) => { let a = cells.get(k); if (!a) cells.set(k, (a = [])); a.push(it); }); },
      hits(it) { return each(it, (k) => { const a = cells.get(k); if (a) for (const o of a) if (clash(it, o)) return true; return false; }); },
    };
  }
  const ov = (a, b, p) => a.x0 < b.x1 + p && b.x0 < a.x1 + p && a.y0 < b.y1 + p && b.y0 < a.y1 + p;
  function clash(a, b) {
    const A = a.k, B = b.k;
    if (A === "j" && B === "j") return false;
    if (A === "b" || B === "b") return A !== "j" && B !== "j" && ov(a, b, A === B ? 5 : 3); // solids keep a little air between them
    if (A === "j" || B === "j") { const w = A === "j" ? b : a, j = A === "j" ? a : b; return w.n !== j.n && ov(w, j, 1); }
    if (A === B) {
      if (A === "h") return Math.abs(a.y0 - b.y0) < 6 && a.x0 < b.x1 - 1 && b.x0 < a.x1 - 1;
      return Math.abs(a.x0 - b.x0) < 6 && a.y0 < b.y1 - 1 && b.y0 < a.y1 - 1;
    }
    const h = A === "h" ? a : b, v = A === "h" ? b : a;
    if (h.n === v.n && h.n != null) return false;
    const x = v.x0, y = h.y0;
    if (x < h.x0 - 2 || x > h.x1 + 2 || y < v.y0 - 2 || y > v.y1 + 2) return false;
    return Math.abs(x - h.x0) < 5 || Math.abs(x - h.x1) < 5 || Math.abs(y - v.y0) < 5 || Math.abs(y - v.y1) < 5; // a T, not a crossing
  }
  const BOX = (x0, y0, x1, y1) => ({ k: "b", x0: Math.min(x0, x1), y0: Math.min(y0, y1), x1: Math.max(x0, x1), y1: Math.max(y0, y1) });
  const HW = (xa, xb, y, n) => ({ k: "h", x0: Math.min(xa, xb), x1: Math.max(xa, xb), y0: y, y1: y, n });
  const VW = (x, ya, yb, n) => ({ k: "v", x0: x, x1: x, y0: Math.min(ya, yb), y1: Math.max(ya, yb), n });
  const JN = (x, y, n) => ({ k: "j", x0: x - 3, x1: x + 3, y0: y - 3, y1: y + 3, n });
  // text metrics (px per character) for the schematic's fonts; generous so estimates never under-reserve
  const CH = { ref: 6.5, val: 5.9, ctl: 5.9, mono: 5.9 };
  const L = 56, TR = 26, Y0 = 64, RH = 76, GPAD = 16;
  // Text scale of the view being laid out / drawn. The Illustrated view draws every label, flag and net name at
  // 1/1.6 of the schematic's size in world units (and opens 1.6x closer), so on screen the text reads the same while
  // every part body is 1.6x bigger: room for the part drawings without changing the placer's geometry.
  const ILLUS_TS = 0.625;
  let TS = 1, MODE = "schematic";

  function railText(lay, n) { const r = lay.rails[n]; return `${n} ${r ? (r.text || r.value + " V") : ""}`.trim(); }
  function flagBox(lay, n, x, y, up) {
    if (isGnd(n)) return BOX(x - 10, y, x + 10, y + 16);
    const w = (railText(lay, n).length * CH.mono + 12) * TS, h = 5 + 14 * TS;
    return up ? BOX(x - w / 2, y - h, x + w / 2, y) : BOX(x - w / 2, y, x + w / 2, y + h);
  }
  // a rail on a side pin: a short lead out to the flag, far enough that the flag clears the part
  function sideFlag(lay, n, px, py, side, its, flags) {
    const fw = isGnd(n) ? 20 : (railText(lay, n).length * CH.mono + 12) * TS;
    const cx = px + side * (fw / 2 + 4);
    its.push(HW(px, cx, py, null), flagBox(lay, n, cx, py + 4, false));
    flags.push([n, cx, py, false]);
    return `M${px} ${py} H${cx}`;
  }
  function labelLines(c, p) {
    const val = p.type === "pot" ? `${p.text || ""}` : p.model ? p.model : p.text || "";
    const lines = [{ t: p.ref, k: "ref" }];
    if (val) lines.push({ t: val, k: "val" });
    if (p.control) lines.push({ t: `${(c.controls.find((k) => k.id === p.control) || {}).label || p.control} 100%`, k: "ctl" });
    return lines;
  }
  const blockW = (lines) => Math.max(...lines.map((l) => l.t.length * CH[l.k])) * TS + 2;

  // Classify every part: where its body goes (which gap between net columns) and how it is drawn.
  function classify(lay) {
    const out = [];
    const sig = (n) => !lay.isRail(n);
    lay.parts.forEach((p) => {
      const t = p.type, nd = p.nodes;
      const two = (a, b, wiper) => {
        if (!sig(a) && !sig(b)) return null;
        if (!sig(a) || !sig(b)) { const n = sig(a) ? a : b, r = sig(a) ? b : a; return { kind: "shunt", n, r, up: lay.railUp(r), a, b, wiper }; }
        if (lay.col[a] === lay.col[b]) { const [t1, t2] = lay.row[a] <= lay.row[b] ? [a, b] : [b, a]; return { kind: "vert2", n: t1, n2: t2, a, b, wiper }; }
        const [l, r] = lay.col[a] < lay.col[b] ? [a, b] : [b, a];
        return { kind: "series", l, r, a, b, wiper };
      };
      let s = null;
      if (t === "pot") {
        const [p1, w, p3] = nd;
        if (w === p1 || w === p3) s = two(w === p1 ? p3 : p1, w, { rheo: true });
        else if (!sig(p1) && !sig(p3)) s = sig(w) ? { kind: "block" } : null;
        else s = two(p1, p3, { net: sig(w) ? w : null, rail: sig(w) ? null : w, p1, p3 });
      } else if (nd.length === 2) s = two(nd[0], nd[1], null);
      else if (t === "opamp") s = { kind: "opamp" };
      else if (/^(npn|pnp|njf|pjf)$/.test(t)) s = { kind: "bjt" };
      else if (t === "triode") s = { kind: "triode" };
      else if (t === "pentode") s = { kind: "pentode" };
      else s = { kind: "block" };
      if (!s) return;
      s.p = p;
      s.lines = labelLines(lay.c, p);
      s.lw = blockW(s.lines);
      s.lh = s.lines.length * 12 * TS;
      // the gap (between column g and g+1; -1 = left of the first column) that holds the body
      const sc = (ns) => ns.filter(sig).map((n) => lay.col[n]);
      if (s.kind === "shunt" || s.kind === "vert2") s.gap = lay.col[s.n];
      else if (s.kind === "series") s.gap = lay.col[s.l];
      else if (s.kind === "opamp") s.gap = sig(nd[2]) ? lay.col[nd[2]] - 1 : Math.max(-1, ...sc(nd.slice(0, 2)));
      else if (s.kind === "bjt") { const o = sc([nd[2], nd[0]]); s.gap = o.length ? Math.min(...o) - 1 : Math.max(-1, ...sc([nd[1]])); }
      else if (s.kind === "triode" || s.kind === "pentode") s.gap = sig(nd[0]) ? lay.col[nd[0]] - 1 : Math.max(-1, ...sc(nd.slice(1)));
      else {
        const half = Math.ceil(nd.length / 2), left = sc(nd.slice(0, half)), right = sc(nd.slice(half));
        s.gap = left.length ? Math.max(...left) : right.length ? Math.min(...right) - 1 : -1;
      }
      s.gap = Math.max(-1, Math.min(lay.ncol - 1, s.gap));
      // body extents around the anchor (x: left/right of anchor; used to size the gaps)
      const k = s.kind;
      // Illustrated view: a part drawing that stands on its wire (radial caps, pots, switches, magnetics) needs room
      // above a horizontal part / beside a vertical one; axial parts (resistors, diodes, axial caps) lie on the wire.
      s.th = MODE === "illus" ? 32 : 20; // the Illustrated view's tubes stand taller
      s.room = MODE === "illus" && (k === "shunt" || k === "vert2" || k === "series") ? (figClass(p, lay.c) === "axial" ? 0 : 18) : 0;
      // rail flags centred on the part (a shunt's rail end, a wiper's rail) need their whole width inside the gap
      const fwOf = (n) => (n != null && !isGnd(n) && lay.isRail(n) ? (railText(lay, n).length * CH.mono + 12) * TS : 20);
      if (k === "shunt" || k === "vert2") {
        s.xl = Math.max(14, (k === "shunt" ? fwOf(s.r) : 0) / 2 + 1);
        s.xr = Math.max(14 + s.lw, s.wiper && s.wiper.rail ? 22 + fwOf(s.wiper.rail) : 0);
        s.w = Math.max(30 + s.lw + 14, s.xl + s.xr + 18) + (s.wiper && s.wiper.net ? 18 : 0) + s.room;
      } else if (k === "series") {
        s.xm = Math.max(4, s.lw / 2 - L / 2 + 2, s.wiper && s.wiper.rail ? fwOf(s.wiper.rail) / 2 - L / 2 + 2 : 0);
        s.w = Math.max(L + 10, s.lw + 6, L + 2 * s.xm + 2);
      }
      else if (k === "opamp") s.w = Math.max(84, s.lw + 10);
      else if (k === "bjt") s.w = 52 + s.lw;
      else if (k === "triode" || k === "pentode") s.w = 64 + s.lw;
      else s.w = Math.max(70, s.lw + 10);
      // rails on side pins hang their flag beside the part: room for it inside the gap
      const sidePins = k === "opamp" ? nd.slice(0, 2) : k === "bjt" ? [nd[1]] : k === "triode" ? [nd[1]] : k === "pentode" ? [nd[1], nd[2]] : k === "block" ? nd : [];
      const fws = sidePins.filter((n) => lay.isRail(n)).map((n) => (isGnd(n) ? 20 : (railText(lay, n).length * CH.mono + 12) * TS) + 8);
      if (fws.length) s.w += Math.max(...fws) * (k === "block" ? 2 : 1);
      out.push(s);
    });
    return out;
  }

  // Columns whose net labels found no room get wider track spacing and the layout runs again.
  function placeAll(c) {
    let wide = new Set(), G = null;
    for (let pass = 0; pass < 6; pass++) {
      G = placeOnce(c, wide);
      const bad = G.misses.filter((m) => m.startsWith("net ")).map((m) => G.lay.col[m.slice(4)]).filter((k) => !wide.has(k));
      if (!bad.length) break;
      wide = new Set([...wide, ...bad]);
    }
    return G;
  }
  function placeOnce(c, wide) {
    const lay = topology(c);
    lay.c = c;
    const items = classify(lay);
    // gap widths from what each gap has to hold
    const G = {};
    for (let g = -1; g < lay.ncol; g++) G[g] = g === -1 ? 54 : 70;
    const shuntW = {};
    items.forEach((s) => {
      G[s.gap] = Math.max(G[s.gap], s.w + 8);
      if (s.kind === "shunt" || s.kind === "vert2") (shuntW[s.gap] = shuntW[s.gap] || []).push(s.w);
    });
    Object.entries(shuntW).forEach(([g, ws]) => { ws.sort((a, b) => b - a); const k = ws.length >= 5 ? 3 : ws.length >= 2 ? 2 : 1; G[g] = Math.max(G[g], ws.slice(0, k).reduce((a, b) => a + b, 0) + 6); });
    // x of every net's bus (its track) and of every gap
    const tx = {}, gap = {};
    let x = 40;
    gap[-1] = [x, x + G[-1]];
    x += G[-1] + GPAD;
    const wideTr = c.wideDc ? Math.max(66, (17 * 5.6 + 6) * TS + 16) : 66; // a widened supply column: a whole DC + ripple label between buses
    for (let k = 0; k < lay.ncol; k++) {
      const tr = wide.has(k) ? wideTr : TR;
      lay.tracks[k].forEach((n, j) => { tx[n] = x + j * tr + (wide.has(k) ? 30 : 0); });
      const last = x + (lay.tracks[k].length - 1) * tr + (wide.has(k) ? 60 : 0);
      gap[k] = [last + GPAD, last + GPAD + G[k]];
      x = gap[k][1] + GPAD;
    }
    const baseY = (n) => Y0 + lay.row[n] * RH;
    const occ = occMap();
    const attach = Object.fromEntries(lay.nets.map((n) => [n, []]));
    const placed = [], misses = [];
    const commit = (its, att) => { its.forEach((it) => occ.add(it)); att.forEach(([n, y]) => attach[n].push(y)); };
    const free = (its) => !its.some((it) => it.k === "b" && occ.hits(it)) && !its.some((it) => it.k !== "b" && occ.hits(it)); // solids first: they fail fastest
    // a wire from a pin to net n's bus; straight when the bus is on the pin's side, else around below the body
    const wire = (n, px, py, side, under, its, att, k = 0) => {
      const X = tx[n];
      if ((X - px) * side >= 0) { its.push(HW(px, X, py, n), JN(X, py, n)); att.push([n, py]); return `M${px} ${py} H${X}`; }
      const ox = px + side * 8, yb = under + 10 + k * 8;
      its.push(HW(px, ox, py, n), VW(ox, py, yb, n), HW(ox, X, yb, n), JN(X, yb, n));
      att.push([n, yb]);
      return `M${px} ${py} H${ox} V${yb} H${X}`;
    };
    // labels: first candidate block position that is free (also of the part's own body and wires)
    const pickLabel = (s, cands, own, g0, g1) => {
      for (const [lx, ly, an] of cands) {
        const x0 = an === "middle" ? lx - s.lw / 2 : an === "start" ? lx : lx - s.lw;
        if (x0 < g0 - 2 || x0 + s.lw > g1 + 2) continue; // labels stay inside the gap, off the buses
        const b = BOX(x0, ly, x0 + s.lw, ly + s.lh);
        if (!occ.hits(b) && !own.some((o) => o.k !== "j" && ov(b, o, 1))) return { x: lx, y: ly, an, box: b };
      }
      return null;
    };
    // jacks first: they sit just outside the input and output buses
    const inN = c.io.input, outN = c.io.output, inY = baseY(inN), outY = tx[outN] != null ? baseY(outN) : Y0;
    if (!c.io.noJacks) commit([BOX(tx[inN] - 36, inY - 8, tx[inN] - 16, inY + 26), HW(tx[inN] - 21, tx[inN], inY, inN), JN(tx[inN], inY, inN)], [[inN, inY]]);
    if (tx[outN] != null && !c.io.noJacks) commit([BOX(tx[outN] + 20, outY - 8, tx[outN] + 42, outY + 26), HW(tx[outN], tx[outN] + 25, outY, outN), JN(tx[outN], outY, outN)], [[outN, outY]]);

    const prio = (s) => (/^(opamp|bjt|triode|pentode)$/.test(s.kind) ? 0 : s.kind === "series" && lay.main.has(s.l) && lay.main.has(s.r) ? 1 : s.kind === "series" ? 3 : s.kind === "block" ? 4 : lay.main.has(s.n) ? 2 : 3);
    const minCol = (s) => Math.min(...s.p.nodes.filter((n) => !lay.isRail(n)).map((n) => lay.col[n]), 99);
    items.sort((a, b) => prio(a) - prio(b) || minCol(a) - minCol(b));

    items.forEach((s) => {
      const p = s.p, nd = p.nodes, [g0, g1] = gap[s.gap];
      const sigs = nd.filter((n) => !lay.isRail(n));
      const pref = s.kind === "shunt" || s.kind === "vert2" ? baseY(s.n) + 18
        : s.kind === "series" ? Math.min(baseY(s.l), baseY(s.r))
        : s.kind === "opamp" ? (lay.isRail(nd[2]) ? Y0 : baseY(nd[2]))
        : s.kind === "bjt" ? (lay.isRail(nd[2]) ? (lay.isRail(nd[1]) ? Y0 : baseY(nd[1])) : baseY(nd[2]) - 22)
        : s.kind === "triode" || s.kind === "pentode" ? (lay.isRail(nd[0]) ? Y0 : baseY(nd[0]) + (s.th || 20) + 10)
        : Math.min(...sigs.map(baseY), Y0 + 999);
      const build = BUILD[s.kind];
      let best = null;
      const xs = build.xs(s, g0, g1);
      for (let y = pref, step = 0; step < 900 && !best; step++, y += 8) {
        // the wires outside the gap are the same for every x at this y: test them once
        if (build.pre && !free(build.pre(s, y, g0, g1, tx))) continue;
        for (const ax of xs) {
          const r = build.at(s, ax, y, { lay, tx, wire });
          if (!r || r.its.some((it) => it.k === "b" && (it.x0 < g0 - 2 || it.x1 > g1 + 2)) || !free(r.its)) continue; // solids stay in the gap, off the buses
          const lab = pickLabel(s, r.labels, r.its, g0, g1);
          if (!lab) continue;
          best = { r, lab };
          break;
        }
      }
      if (!best) { // nothing free in range: place at the preferred spot anyway
        let r = null;
        for (const ax of xs) if ((r = build.at(s, ax, pref, { lay, tx, wire }))) break;
        if (!r) return;
        const [lx, ly, an] = r.labels[0], lx0 = an === "middle" ? lx - s.lw / 2 : an === "start" ? lx : lx - s.lw;
        best = { r, lab: { x: lx, y: ly, an, box: BOX(lx0, ly, lx0 + s.lw, ly + s.lh) } };
        misses.push(p.ref);
      }
      commit([...best.r.its, best.lab.box], best.r.att);
      placed.push({ s, g: best.r, lab: best.lab, box: best.r.body });
    });

    // buses and net labels
    const nets = lay.nets.filter((n) => attach[n].length).map((n) => {
      const ys = attach[n], y0 = Math.min(...ys), y1 = Math.max(...ys), X = tx[n];
      return { n, x: X, y0, y1, ys };
    });
    nets.forEach((b) => occ.add(VW(b.x, b.y0, b.y1, b.n)));
    nets.forEach((b) => {
      const w = (Math.max(b.n.length * CH.mono, (c.wideDc ? 17 : 9) * 5.6) + 6) * TS, h = 24 * TS;
      // candidates: above the bus top, then beside the bus anywhere along it, then higher above it
      const cands = [];
      ["middle", "start", "end"].forEach((an) => cands.push([an, 0]));
      for (let yy = b.y0 + 6; yy + h <= b.y1 - 2; yy += 6) ["start", "end"].forEach((an) => cands.push([an, null, yy]));
      for (let raise = 10; raise <= 1200; raise += raise < 400 ? 10 : 20) ["middle", "start", "end"].forEach((an) => cands.push([an, raise]));
      let got = null;
      for (const [an, raise, yy] of cands) {
        const x0 = an === "middle" ? b.x - w / 2 : an === "start" ? b.x + 5 : b.x - 5 - w;
        if (raise == null) { // beside the bus, no extension
          const box = BOX(x0, yy, x0 + w, yy + h);
          if (!occ.hits(box)) { got = { an, box, top: b.y0 }; break; }
          continue;
        }
        const yb = b.y0 - 6 - raise;
        const box = BOX(x0, yb - h, x0 + w, yb);
        const ext = an === "middle" ? VW(b.x, yb, b.y0, b.n) : VW(b.x, yb - h / 2, b.y0, b.n);
        if (occ.hits(box) || ((raise || an !== "middle") && occ.hits(ext))) continue;
        got = { an, box, top: an === "middle" ? yb : Math.min(b.y0, yb - h / 2) };
        break;
      }
      if (!got) { const x0 = b.x - w / 2; got = { an: "middle", box: BOX(x0, b.y0 - 30, x0 + w, b.y0 - 6), top: b.y0 - 6 }; misses.push(`net ${b.n}`); }
      occ.add(got.box);
      if (got.top < b.y0) occ.add(VW(b.x, got.top, b.y0, b.n));
      b.lab = got;
      b.top = Math.min(b.y0, got.top);
    });
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    occ.all.forEach((r) => { x0 = Math.min(x0, r.x0); y0 = Math.min(y0, r.y0); x1 = Math.max(x1, r.x1); y1 = Math.max(y1, r.y1); });
    return { lay, tx, placed, nets, misses, inY, outY, bounds: { x0: x0 - 30, y0: y0 - 30, x1: x1 + 30, y1: y1 + 30 } };
  }

  // Geometry of each kind: candidate anchor xs within the gap, and the items/attachments/label spots at an anchor.
  const span = (g0, g1, l, r, step = 8) => { const out = []; for (let x = g0 + l; x <= g1 - r + 0.1; x += step) out.push(x); return out.length ? out : [g0 + l]; };
  const BUILD = {
    // vertical two-terminal part hanging off a bus (to a rail, or to a second bus in the same column)
    shunt: {
      xs: (s, g0, g1) => span(g0, g1, s.xl || 14, s.xr || 14 + s.lw),
      pre: (s, y, g0, g1, tx) => [HW(tx[s.n], g0, s.kind === "shunt" && s.up ? y + L : y, s.n)].concat(s.kind === "vert2" ? [HW(tx[s.n2], g0, y + L, s.n2)] : []),
      at(s, x, y, E) {
        const { lay } = E, its = [], att = [], flags = [];
        const p = s.p, w = s.wiper;
        const up = s.kind === "shunt" && s.up;
        // top terminal at y, bottom at y + L; the net lands on the top end (or the bottom for a supply hanging above)
        const yN = up ? y + L : y;
        let d = `M${E.tx[s.n]} ${yN} H${x}`;
        if (E.tx[s.n] > x) return null; // buses always sit left of their gap
        its.push(HW(E.tx[s.n], x, yN, s.n), JN(E.tx[s.n], yN, s.n));
        att.push([s.n, yN]);
        if (s.kind === "vert2") {
          if (E.tx[s.n2] > x) return null;
          its.push(HW(E.tx[s.n2], x, y + L, s.n2), JN(E.tx[s.n2], y + L, s.n2));
          att.push([s.n2, y + L]);
          d += ` M${x} ${y + L} H${E.tx[s.n2]}`;
        } else {
          const fy = up ? y : y + L;
          its.push(flagBox(lay, s.r, x, up ? fy - 4 : fy + 4, up), BOX(x - 1, up ? fy - 4 : fy, x + 1, up ? fy : fy + 4));
          flags.push([s.r, x, fy, up]);
        }
        let side = 1, wd = "";
        if (w && w.net) {
          side = E.tx[w.net] < x ? -1 : 1;
          const yc = y + L / 2;
          its.push(HW(x + side * 16, E.tx[w.net], yc, w.net), JN(E.tx[w.net], yc, w.net));
          att.push([w.net, yc]);
          wd = `M${x + side * 16} ${yc} H${E.tx[w.net]}`;
        } else if (w && w.rail) {
          const yc = y + L / 2;
          // the flag starts clear of the body (a long rail name would otherwise reach back over the part)
          const fx = x + 20 + (isGnd(w.rail) ? 10 : (railText(lay, w.rail).length * CH.mono + 12) * TS / 2);
          its.push(flagBox(lay, w.rail, fx, yc + 4, false), HW(x + 16, fx, yc, null));
          flags.push([w.rail, fx, yc, false]);
          wd = `M${x + 16} ${yc} H${fx}`;
        }
        // the drawing's side (Illustrated): away from a wiper lead or a wiper rail flag
        const artSide = w && !w.rheo && (w.rail || side > 0) ? -1 : 1, room = s.room || 0;
        const body = BOX(x - (w && !w.rheo && side < 0 ? 17 : 11) - (artSide < 0 ? room : 0), y - 1, x + (w && !w.rheo && side > 0 ? 17 : 11) + (artSide > 0 ? room : 0), y + L + 1);
        its.push(body);
        const top = y + 6;
        const labels = [...(room ? [[x + 14 + room, top, "start"], [x - 14 - room, top, "end"], [x + 14 + room, top + 12, "start"], [x - 14 - room, top + 12, "end"]] : []), [x + 14, top, "start"], [x - 14, top, "end"], [x + 14, top + 12, "start"], [x - 14, top + 12, "end"], [x + 26, top, "start"], [x - 26, top, "end"],
          // a wiper lead on that side: above the part, or below the lead
          [x + 8, y - 5 - s.lh, "start"], [x - 8, y - 5 - s.lh, "end"], [x + 14, y + L / 2 + 6, "start"], [x - 14, y + L / 2 + 6, "end"]];
        return { its, att, flags, d: d + (wd ? ` ${wd}` : ""), x, y, side, artSide, body, labels, vert: true };
      },
    },
    series: {
      xs: (s, g0, g1) => { const m = s.xm || Math.max(4, s.lw / 2 - L / 2 + 2), c = (g0 + g1) / 2 - L / 2, all = span(g0, g1, m, L + m); all.sort((a, b) => Math.abs(a - c) - Math.abs(b - c)); return all.slice(0, 7); },
      pre: (s, y, g0, g1, tx) => [HW(tx[s.l], g0, y, s.l), HW(g1, tx[s.r], y, s.r)],
      at(s, x0, y, E) {
        const its = [], att = [], flags = [];
        const w = s.wiper, xl = E.tx[s.l], xr = E.tx[s.r];
        if (xl > x0 || xr < x0 + L) return null;
        its.push(HW(xl, x0, y, s.l), JN(xl, y, s.l), HW(x0 + L, xr, y, s.r), JN(xr, y, s.r));
        att.push([s.l, y], [s.r, y]);
        let d = `M${xl} ${y} H${x0} M${x0 + L} ${y} H${xr}`;
        let bot = y + 11;
        if (w && w.net) {
          const X = E.tx[w.net];
          its.push(X < x0 + L / 2 ? HW(X, x0 + 12, y + 22, w.net) : HW(x0 + L - 12, X, y + 22, w.net), JN(X, y + 22, w.net));
          att.push([w.net, y + 22]);
          d += ` M${X < x0 + L / 2 ? x0 + 12 : x0 + L - 12} ${y + 22} H${X}`;
          bot = y + 24;
        } else if (w && w.rail) {
          its.push(flagBox(E.lay, w.rail, x0 + L / 2, y + 26, false));
          flags.push([w.rail, x0 + L / 2, y + 22, false]);
          bot = y + 46;
        }
        const up = s.room || 0;
        const body = BOX(x0, y - 11 - up, x0 + L, bot);
        its.push(body);
        const cx = x0 + L / 2;
        const labels = [[cx, y - 14 - up - s.lh, "middle"]];
        if (!w || w.rheo) labels.push([cx, y + 14, "middle"]);
        labels.push([cx, y - 25 - up - s.lh, "middle"]);
        return { its, att, flags, d, x: x0, y, body, labels, wX: w && w.net ? E.tx[w.net] : null };
      },
    },
    opamp: {
      xs: (s, g0, g1) => span(g0, g1, 30, 46 + 4).reverse(),
      at(s, x, y, E) {
        const its = [], att = [], flags = [];
        const [ip, im, out] = s.p.nodes;
        let d = "";
        const pin = (n, py) => {
          if (E.lay.isRail(n)) return sideFlag(E.lay, n, x, py, -1, its, flags);
          return E.wire(n, x, py, -1, y + 24, its, att, py > y ? 1 : 0);
        };
        d += pin(ip, y - 10) + " " + pin(im, y + 10);
        if (!E.lay.isRail(out)) { if (E.tx[out] < x + 44) return null; its.push(HW(x + 44, E.tx[out], y, out), JN(E.tx[out], y, out)); att.push([out, y]); d += ` M${x + 44} ${y} H${E.tx[out]}`; }
        const body = BOX(x - 2, y - 24, x + 46, y + 24);
        its.push(body);
        const labels = [[x + 22, y - 26 - s.lh, "middle"], [x + 22, y + 27, "middle"], [x + 48, y - 26 - s.lh + 12, "start"], [x - 4, y - 26 - s.lh, "end"]];
        return { its, att, flags, d, x, y, body, labels };
      },
    },
    bjt: {
      xs: (s, g0, g1) => span(g0, g1, 20, 24, 8).reverse(),
      at(s, x, y, E) {
        const its = [], att = [], flags = [];
        const [cN, bN, eN] = s.p.nodes;
        let d = "";
        const out = (n, py, up) => {
          if (E.lay.isRail(n)) { its.push(flagBox(E.lay, n, x + 14, up ? py - 4 : py + 4, up)); flags.push([n, x + 14, py, up]); return ""; }
          return E.wire(n, x + 14, py, E.tx[n] >= x + 14 ? 1 : -1, y + 26, its, att);
        };
        d += out(cN, y - 22, true) + " " + out(eN, y + 22, false);
        if (E.lay.isRail(bN)) sideFlag(E.lay, bN, x - 16, y, -1, its, flags);
        else d += " " + E.wire(bN, x - 16, y, -1, y + 26, its, att, 1);
        const body = BOX(x - 17, y - 23, x + 20, y + 23);
        its.push(body);
        const labels = [[x - 14, y - 32 - s.lh + 12, "end"], [x + 22, y - 8 - s.lh / 2, "start"], [x - 18, y + 8, "end"], [x + 4, y - 28 - s.lh, "middle"], [x + 4, y + 28, "middle"]];
        return { its, att, flags, d, x, y, body, labels };
      },
    },
    triode: {
      xs: (s, g0, g1) => span(g0, g1, 30, 26).reverse(),
      at(s, x, y, E) {
        const its = [], att = [], flags = [];
        const nd = s.p.nodes, pl = nd[0], g = nd[s.kind === "pentode" ? 2 : 1], k = nd[nd.length - 1], h = s.th || 20; // plate / cathode pin offset
        let d = "";
        if (E.lay.isRail(pl)) { its.push(flagBox(E.lay, pl, x, y - h - 4, true)); flags.push([pl, x, y - h, true]); }
        else {
          const X = E.tx[pl];
          its.push(VW(x, y - h - 10, y - h, pl), HW(x, X, y - h - 10, pl), JN(X, y - h - 10, pl));
          att.push([pl, y - h - 10]);
          d += `M${x} ${y - h} V${y - h - 10} H${X}`;
        }
        if (E.lay.isRail(g)) sideFlag(E.lay, g, x - 26, y, -1, its, flags);
        else d += " " + E.wire(g, x - 26, y, -1, y + h + 10, its, att, 1);
        if (E.lay.isRail(k)) { its.push(flagBox(E.lay, k, x, y + h + 4, false)); flags.push([k, x, y + h, false]); }
        else { const X = E.tx[k]; its.push(VW(x, y + h, y + h + 12, k), HW(x, X, y + h + 12, k), JN(X, y + h + 12, k)); att.push([k, y + h + 12]); d += ` M${x} ${y + h} V${y + h + 12} H${X}`; }
        if (s.kind === "pentode") {
          const sc = nd[1];
          if (E.lay.isRail(sc)) sideFlag(E.lay, sc, x + 20, y, 1, its, flags);
          else d += " " + E.wire(sc, x + 20, y, E.tx[sc] >= x ? 1 : -1, y + h + 12, its, att, 2);
        }
        const body = BOX(x - 26, y - h - 2, x + 22, y + h + 2);
        its.push(body);
        const labels = [[x + 24, y - h - 6, "start"], [x - 24, y - h - 6 - s.lh + 6, "end"], [x + 24, y + 4, "start"], [x - 28, y + 8, "end"], [x + 26, y + h + 6, "start"]];
        return { its, att, flags, d, x, y, h, body, labels };
      },
    },
    block: {
      xs: (s, g0, g1) => span(g0, g1, 22, 22 + 46),
      at(s, x, y, E) {
        const its = [], att = [], flags = [];
        const nd = s.p.nodes, half = Math.ceil(nd.length / 2), lp = nd.slice(0, half), rp = nd.slice(half);
        const hgt = Math.max(30, Math.max(lp.length, rp.length) * 16 + 6), W = 46;
        let d = "";
        const pin = (n, px, py, side, i) => {
          if (E.lay.isRail(n)) { sideFlag(E.lay, n, px, py, side, its, flags); return; }
          d += " " + E.wire(n, px, py, side, y + hgt, its, att, i);
        };
        lp.forEach((n, i) => pin(n, x, y + 11 + i * 16, -1, i));
        rp.forEach((n, i) => pin(n, x + W, y + 11 + i * 16, 1, i + lp.length));
        const body = BOX(x, y - 2, x + W, y + hgt + 2);
        its.push(body);
        const labels = [[x + W / 2, y - 5 - s.lh, "middle"], [x + W / 2, y + hgt + 6, "middle"], [x + W + 14, y - 4 - s.lh, "start"]];
        return { its, att, flags, d, x, y, hgt, W, body, labels, lp, rp };
      },
    },
  };
  BUILD.vert2 = BUILD.shunt;
  BUILD.pentode = BUILD.triode;

  // Where each pin of a placed part is on the schematic (world coordinates): the editor's wiring handles.
  function pinPoints(pl) {
    const { s, g } = pl, p = s.p, nd = p.nodes, x = g.x, y = g.y, k = s.kind, out = [];
    const add = (i, px, py) => { if (i >= 0 && i < nd.length && !out.some((o) => o.i === i)) out.push({ i, x: px, y: py }); };
    const ends = (A, B) => { // s.a at A or B, whichever end its node sits on (the other pin at the other end)
      const w = s.wiper;
      let ia = 0, ib = 1;
      if (p.type === "pot") { if (w && w.rheo) { ia = nd[1] === nd[0] ? 2 : 0; ib = 1; } else { ia = 0; ib = 2; } }
      const aAtA = A.n === s.a;
      add(ia, ...(aAtA ? A.p : B.p));
      add(ib, ...(aAtA ? B.p : A.p));
    };
    if (k === "shunt" || k === "vert2") {
      const top = k === "vert2" ? s.n : s.up ? s.r : s.n, bot = k === "vert2" ? s.n2 : s.up ? s.n : s.r;
      ends({ n: top, p: [x, y] }, { n: bot, p: [x, y + L] });
      const w = s.wiper;
      if (w && !w.rheo) add(1, w.net ? x + g.side * 16 : x + 22, y + L / 2);
    } else if (k === "series") {
      ends({ n: s.l, p: [x, y] }, { n: s.r, p: [x + L, y] });
      const w = s.wiper;
      if (w && !w.rheo) add(1, w.net && g.wX != null ? (g.wX < x + L / 2 ? x + 12 : x + L - 12) : x + L / 2, y + 22);
    } else if (k === "opamp") {
      add(0, x, y - 10); add(1, x, y + 10); add(2, x + 44, y); add(3, x + 22, y - 11); add(4, x + 22, y + 11);
    } else if (k === "bjt") {
      add(0, x + 14, y - 22); add(1, x - 16, y); add(2, x + 14, y + 22);
    } else if (k === "triode") {
      add(0, x, y - (g.h || 20)); add(1, x - 26, y); add(2, x, y + (g.h || 20));
    } else if (k === "pentode") {
      add(0, x, y - (g.h || 20)); add(1, x + 20, y); add(2, x - 26, y); add(3, x, y + (g.h || 20));
    } else if (k === "block") {
      g.lp.forEach((n, i) => add(i, x - 6, y + 11 + i * 16));
      g.rp.forEach((n, i) => add(g.lp.length + i, x + g.W + 6, y + 11 + i * 16));
    }
    return out;
  }

  /* ---------------- drawing ---------------- */
  function sym2(p) { // horizontal, (0,0) -> (L,0)
    const t = p.type;
    if (t === "R" || t === "pot") {
      let d = "M0 0 H12";
      for (let i = 0; i < 6; i++) d += ` L${14 + i * 5} ${i % 2 ? 5 : -5}`;
      d += ` L${44} 0 H${L}`;
      return `<path class="body" d="${d}"/>`;
    }
    if (t === "C") return `<path class="body" d="M0 0 H25 M25 -9 V9 M31 -9 V9 M31 0 H${L}"/>`;
    if (t === "L") { let d = "M0 0 H10"; for (let i = 0; i < 4; i++) d += ` a4.5 4.5 0 0 1 9 0`; d += ` H${L}`; return `<path class="body" d="${d}"/>`; }
    if (t === "D" || t === "Dpair") return `<path class="body" d="M0 0 H20 M20 -7 L34 0 L20 7 Z M34 -7 V7 M34 0 H${L}"/>`;
    if (t === "switch") return `<path class="body" d="M0 0 H18 M36 0 H${L} M18 0 L35 ${p.closed ? 0 : -10}"/><circle class="fillb" cx="18" cy="0" r="2"/><circle class="fillb" cx="36" cy="0" r="2"/>`;
    if (t === "acsrc") return `<path class="body" d="M0 0 H17 M39 0 H${L}"/><circle class="body" cx="28" cy="0" r="11"/><path class="body" d="M21 0 q3.5 -8 7 0 t7 0"/>`;
    if (t === "lamp") return `<path class="body" d="M0 0 H18 M38 0 H${L}"/><circle class="body" cx="28" cy="0" r="10"/><path class="body" d="M21 -7 L35 7 M21 7 L35 -7"/>`;
    if (t === "cds") return `<path class="body" d="M0 0 H12 M44 0 H${L}"/><rect class="body" x="12" y="-6" width="32" height="12" rx="2"/><path class="body" d="M17 -14 l6 6 M25 -14 l6 6"/>`;
    return `<path class="body" d="M0 0 H10 M46 0 H${L}"/><rect class="body" x="10" y="-8" width="36" height="16" rx="3"/>`;
  }
  function railFlag(lay, n, x, y, up, sel) {
    if (isGnd(n)) return `<g class="gnd-g" data-rail="${esc(n)}"><rect class="gnd-hit" x="${x - 10}" y="${y}" width="20" height="16"/><path class="gnd" d="M${x} ${y} V${y + 6} M${x - 9} ${y + 6} H${x + 9} M${x - 6} ${y + 10} H${x + 6} M${x - 3} ${y + 14} H${x + 3}"/></g>`;
    const r = lay.rails[n];
    if (r && r.pseudo) { // a supply node: its live DC, and a link to where the supply section makes it
      const v = V.curves && V.curves.dc ? V.curves.dc[n] : null;
      const label = `${n} ${v != null ? fmtV(v).replace(" V", "\u2009V") : "\u2026"}`, w = (railText(lay, n).length * CH.mono + 10) * TS, fh = 14 * TS;
      const yy = up ? y - 4 - fh : y + 4;
      return `<g class="rail-flag supply-link" data-supply="${esc(n)}" data-rail="${esc(n)}"><title>${esc(`Supply node ${n}${v != null ? ` at ${fmtV(v)}` : ""}: click to see where the power supply makes it`)}</title><path class="stub" d="M${x} ${y} V${up ? y - 4 : y + 4}"/><rect x="${x - w / 2}" y="${yy}" width="${w}" height="${fh}" rx="${4 * TS}"/><text x="${x}" y="${yy + 10 * TS}" text-anchor="middle">${esc(label)}</text></g>`;
    }
    const label = railText(lay, n);
    const w = (label.length * CH.mono + 10) * TS, fh = 14 * TS;
    const yy = up ? y - 4 - fh : y + 4;
    return `<g class="rail-flag${r ? ` part${sel === r.ref ? " sel" : ""}" data-part="${esc(r.ref)}` : ""}" data-rail="${esc(n)}"><title>${esc(r ? `${r.ref}: ${r.label || "DC source"}` : n)}</title><path class="stub" d="M${x} ${y} V${up ? y - 4 : y + 4}"/><rect x="${x - w / 2}" y="${yy}" width="${w}" height="${fh}" rx="${4 * TS}"/><text x="${x}" y="${yy + 10 * TS}" text-anchor="middle">${esc(label)}</text></g>`;
  }
  function labelSvg(c, s, lab, p) {
    const lines = s.lines.map((l, i) => {
      let t = l.t;
      if (l.k === "ctl") t = `${(c.controls.find((k) => k.id === p.control) || {}).label || p.control}${p.position != null ? ` ${Math.round(p.position * 100)}%` : ""}`;
      if (l.k === "val" && s.p.acsrc) t = acText(s.p.nodes[0]);
      return `<text class="${l.k}" x="${lab.x}" y="${lab.y + (9 + i * 12) * TS}" text-anchor="${lab.an}">${esc(t)}</text>`;
    });
    return lines.join("");
  }
  function partSvg(c, pl, live) {
    const { s, g, lab } = pl, p = s.p.acsrc ? s.p : live.part(s.p.ref) || s.p, x = g.x, y = g.y;
    let inner = "";
    const k = s.kind, w = s.wiper;
    if (live.figs) { // the drawing goes in the art layers (tiled vector + bitmap), under the part's hit area and labels
      const F = live.figs.get(p.ref);
      if (F) live.addArt(pl, p, F);
    } else if (k === "shunt" || k === "vert2") {
      inner += `<g transform="translate(${x} ${y}) rotate(90)">${sym2(p)}</g>`;
      if (w && w.rheo) inner += `<path class="wiper" d="M${x + 12} ${y + L / 2 + 8} L${x - 6} ${y + L / 2 - 8}"/>`;
      else if (w) {
        const pos = p.position != null ? p.position : 0.5;
        const top = s.kind === "shunt" && s.up ? s.r : s.n;
        const wy = y + 12 + (top === w.p1 ? pos : 1 - pos) * 32, sd = w.net ? g.side : 1; // position runs p1 -> p3
        inner += `<path class="wiper" d="M${x + sd * 16} ${y + L / 2} V${wy} H${x + sd * 7}"/><path class="wiper-head" d="M${x + sd * 6} ${wy} l${sd * 6} -4 v8 z"/>`;
      }
    } else if (k === "series") {
      inner += `<g transform="translate(${x} ${y})">${sym2(p)}</g>`;
      if (w && w.rheo) inner += `<path class="wiper" d="M${x + L / 2 - 10} ${y + 9} L${x + L / 2 + 8} ${y - 9}"/>`;
      else if (w) {
        const pos = p.position != null ? p.position : 0.5;
        const t = s.l === w.p1 ? pos : 1 - pos;
        const wx = x + 12 + t * 32;
        const X = w.net ? g.wX : null;
        const end = X == null ? x + L / 2 : X < x + L / 2 ? x + 12 : x + L - 12;
        inner += `<path class="wiper" d="M${end} ${y + 22} H${wx} V${y + 7}"/><path class="wiper-head" d="M${wx} ${y + 7} l-4 6 h8 z"/>`;
      }
    } else if (k === "opamp") {
      inner += `<path class="body" d="M${x} ${y - 22} L${x + 44} ${y} L${x} ${y + 22} Z"/><text class="val" x="${x + 4}" y="${y - 6}">+</text><text class="val" x="${x + 4}" y="${y + 14}">−</text>`;
    } else if (k === "bjt") {
      const t = p.type, cx = x + 4;
      if (t === "njf" || t === "pjf") {
        inner += `<circle class="body" cx="${cx}" cy="${y}" r="15"/><path class="body" d="M${x - 16} ${y} H${x + 2} M${x + 2} ${y - 10} V${y + 10} M${x + 2} ${y - 7} H${x + 14} V${y - 22} M${x + 2} ${y + 7} H${x + 14} V${y + 22}"/>` +
          (t === "njf" ? `<path class="fillb" d="M${x + 2} ${y} l-7 -3.5 v7 z"/>` : `<path class="fillb" d="M${x - 8} ${y} l7 -3.5 v7 z"/>`);
      } else {
        inner += `<circle class="body" cx="${cx}" cy="${y}" r="15"/><path class="body" d="M${x - 16} ${y} H${x - 2} M${x - 2} ${y - 9} V${y + 9} M${x - 2} ${y - 5} L${x + 14} ${y - 22} M${x - 2} ${y + 5} L${x + 14} ${y + 22}"/>` +
          (t === "pnp" ? `<path class="fillb" d="M${x + 1} ${y + 7} l8 1 l-4 5 z"/>` : `<path class="fillb" d="M${x + 14} ${y + 22} l-8 -2 l4 -5 z"/>`);
      }
    } else if (k === "triode" || k === "pentode") {
      const pen = k === "pentode";
      inner += `<circle class="body" cx="${x}" cy="${y}" r="20"/>
        <path class="body" d="M${x - 9} ${y - 10} H${x + 9} M${x - 12} ${y} H${x - 9} M${x - 6} ${y} H${x - 3} M${x} ${y} H${x + 3} M${x + 6} ${y} H${x + 9} M${x - 26} ${y} H${x - 13} M${x - 9} ${y + 10} H${x + 9} M${x} ${y + 10} V${y + 20} M${x} ${y - 10} V${y - 20}${pen ? ` M${x - 12} ${y - 5} H${x + 12} M${x + 12} ${y - 5} V${y} H${x + 20}` : ""}"/>`;
    } else {
      const t = p.type, W = g.W, H = g.hgt;
      inner += `<rect class="body" x="${x}" y="${y}" width="${W}" height="${H}" rx="5"/>`;
      g.lp.forEach((n, i) => { inner += `<path class="body" d="M${x - 6} ${y + 11 + i * 16} H${x}"/>`; });
      g.rp.forEach((n, i) => { inner += `<path class="body" d="M${x + W} ${y + 11 + i * 16} H${x + W + 6}"/>`; });
      const cx = x + W / 2, cy = y + H / 2;
      if (t === "transformer") {
        let a = "", b = "";
        for (let i = 0; i < 4; i++) { a += ` a3.6 3.6 0 0 1 0 7.2`; b += ` a3.6 3.6 0 0 0 0 7.2`; }
        inner += `<path class="body" d="M${cx - 9} ${cy - 14.4}${a} M${cx + 9} ${cy - 14.4}${b} M${cx - 2} ${cy - 15} V${cy + 15} M${cx + 2} ${cy - 15} V${cy + 15}"/>`;
      } else if (t === "spring") {
        inner += `<path class="body" d="M${x + 6} ${cy} ${Array.from({ length: 7 }, (_, i) => `q2.4 ${i % 2 ? 6 : -6} 4.8 0`).join(" ")}"/>`;
      } else if (t === "speaker") {
        // loudspeaker: magnet block, cone, and the sound it radiates (the cone-velocity pin is on the right)
        inner += `<path class="body" d="M${cx - 13} ${cy - 5} h6 v10 h-6 z M${cx - 7} ${cy - 5} L${cx + 3} ${cy - 13} V${cy + 13} L${cx - 7} ${cy + 5}"/><path class="body" d="M${cx + 7} ${cy - 5} a7 7 0 0 1 0 10 M${cx + 10} ${cy - 9} a11 11 0 0 1 0 18" opacity=".6"/>`;
      } else if (t === "ldr") {
        inner += `<path class="body" d="M${cx - 12} ${cy - 6} l7 6 l-7 6 z M${cx - 5} ${cy - 6} v12 M${cx + 1} ${cy - 3} l5 -3 M${cx + 1} ${cy + 3} l5 -3"/><rect class="body" x="${cx + 7}" y="${cy - 8}" width="7" height="16" rx="2"/>`;
      } else {
        inner += `<text class="val" x="${cx}" y="${cy + 3.5}" text-anchor="middle">${esc((p.of || t || "").slice(0, 6))}</text>`;
      }
    }
    const hb = pl.box;
    const hit = `<rect class="hit" x="${hb.x0}" y="${hb.y0}" width="${hb.x1 - hb.x0}" height="${hb.y1 - hb.y0}" rx="6"/>`;
    const flags = g.flags.map(([n, fx, fy, up]) => railFlag(pl.lay || live.lay, n, fx, fy, up, live.sel)).join("");
    const pins = live.edit ? pinPoints(pl).map((q) => `<circle class="cv-pin${live.dangling.has(p.nodes[q.i]) ? " nc" : ""}" data-pin="${q.i}" cx="${q.x}" cy="${q.y}" r="${TS < 1 ? 2.6 : 3.4}"><title>${esc(`${p.ref} ${pinName(p, q.i)} → ${p.nodes[q.i]} · drag to wire`)}</title></circle>`).join("") : "";
    return `<g class="part${live.sel === p.ref ? " sel" : ""}${live.isMod(p) ? " mod" : ""}${live.isAdded(p) ? " added" : ""}${p.type === "pot" && p.control ? " pot-drag" : ""}" data-part="${esc(p.ref)}" ${live.tint(p)}>${hit}${inner}${labelSvg(live.c, s, lab, p)}${pins}</g>${flags}`;
  }

  /* ---------------- sections: the power supply as its own part of the diagram ---------------- */
  // A netlist that models its supply (mains / transformer secondaries as AC sources, rectifiers, reservoir and filter
  // caps, chokes, droppers, bias and regulators) has those parts mixed into the audio circuit by the placer. They are
  // found from the netlist itself: flood out from the AC sources through transformers, rectifiers, series R / L into
  // nodes that are filtered (a big cap to ground, a rectifier, a reference diode), filter caps and bleeders between
  // supply nodes, and regulator devices whose pins all sit on supply nodes. The audio section then sees each supply
  // node it uses as a rail flag (live DC), and the supply is laid out as its own section, linked from those flags.
  const AC_RE = /mains|secondar|\bHT\b|winding|\bPT\b|VAC\b|Vrms|Vpp|\bAC\b|heater|filament/i;
  const isAcSource = (p) => p.type === "V" && (p.ac != null || (Math.abs(+p.value || 0) < 1e-12 && AC_RE.test(`${p.label || ""} ${p.note || ""}`) && !/kick|start-?up/i.test(`${p.ref} ${p.label || ""}`)));
  const acNodes = (p) => p.nodes.filter((n) => !isGnd(n));
  function supplySplit(c) {
    const parts = c.parts || [], ac = parts.filter(isAcSource);
    if (!ac.length) return null;
    const io = c.io || {}, ioN = new Set([io.input, io.output, io.inputR, io.outputR].filter((n) => n != null));
    if (ac.some((p) => acNodes(p).some((n) => ioN.has(n)))) return null;
    const S = new Set(), sign = {}, depth = {}, sup = new Set(ac.map((p) => p.ref));
    ac.forEach((p) => acNodes(p).forEach((n) => { S.add(n); sign[n] = 0; depth[n] = 0; }));
    const pins = new Map();
    parts.forEach((p) => p.nodes.forEach((n) => { if (!pins.has(n)) pins.set(n, []); pins.get(n).push(p); }));
    const dcRails = new Set(parts.filter((p) => p.type === "V" && !sup.has(p.ref)).map((p) => p.nodes[0]));
    const G = (n) => isGnd(n) || dcRails.has(n), known = (n) => G(n) || S.has(n);
    const fam = (p) => String(p.family || "").toLowerCase();
    const isRect = (p) => /rectifier/.test(fam(p));
    const isDiode = (p) => p.type === "D" || p.type === "Dpair";
    const bigCap = (p) => p.type === "C" && (fam(p) === "electrolytic" || +p.value >= 0.9e-6);
    const other = (q, n) => q.nodes.find((m) => m !== n);
    // a stacked reservoir's midpoint: only big caps and (balancing) resistors, each to a supply node or ground
    const stackMid = (m, n) => !ioN.has(m) && (pins.get(m) || []).every((q) => (bigCap(q) || q.type === "R") && q.nodes.length === 2 && q.nodes.every((k) => k === m || k === n || known(k)));
    const capNode = (n) => (pins.get(n) || []).some((q) => bigCap(q) && q.nodes.length === 2 && (known(other(q, n)) || stackMid(other(q, n), n)));
    const filterNode = (n, via) => !ioN.has(n) && (capNode(n)
      // a rectifier, a reference diode to ground, or a diode into a reservoir (a bridge's input node)
      || (pins.get(n) || []).some((q) => isRect(q) || (isDiode(q) && q.nodes.length === 2 && (q.nodes.every((m) => m === n || G(m)) || capNode(other(q, n)))))
      // a node inside a series chain (winding resistance + choke, two droppers in series): passes on to the next node
      || (via && (pins.get(n) || []).length === 2 && (pins.get(n) || []).every((q) => q === via || (/^(R|L)$/.test(q.type) && q.nodes.length === 2 && filterNode(other(q, n), null)))));
    const addNet = (n, sg, d) => { if (known(n)) return; S.add(n); sign[n] = sg; depth[n] = d; };
    for (let it = 0; it < 60; it++) {
      let grew = false;
      parts.forEach((p) => {
        if (sup.has(p.ref) || p.type === "V") return;
        const nd = p.nodes, inS = nd.filter((n) => S.has(n));
        if (!inS.length || nd.some((n) => ioN.has(n))) return;
        const unk = [...new Set(nd.filter((n) => !known(n)))];
        const d0 = Math.max(...inS.map((n) => depth[n])) + 1, sgs = inS.map((n) => sign[n]), sg = Math.max(...sgs) || Math.min(...sgs);
        let ok = false;
        if ((p.type === "vccs" || p.type === "vcvs") && nd.length === 4) { // a winding modelled as a source controlled by the mains
          ok = S.has(nd[2]) && sign[nd[2]] === 0;
          if (ok) unk.forEach((n) => addNet(n, 0, d0));
        } else if (p.type === "transformer") { ok = [nd[0], nd[1]].some((n) => S.has(n) && sign[n] === 0); if (ok) unk.forEach((n) => addNet(n, 0, d0)); }
        else if (isDiode(p) && nd.length === 2) {
          if (!unk.length) ok = true;
          else if (isRect(p) || filterNode(unk[0], p)) { ok = true; addNet(unk[0], unk[0] === nd[1] ? 1 : -1, d0); }
        } else if (nd.length === 2 && /^(R|L|C|switch)$/.test(p.type)) {
          if (!unk.length) ok = true;
          else if (p.type !== "C" && filterNode(unk[0], p)) { ok = true; addNet(unk[0], sg, d0); }
          else if (p.type === "C" && bigCap(p) && stackMid(unk[0], inS[0])) { ok = true; addNet(unk[0], sg, d0); }
        } else if (/^(npn|pnp|njf|pjf|triode|pentode)$/.test(p.type) && unk.length <= 1 && nd.length - unk.length >= 2) {
          if (!unk.length || filterNode(unk[0])) { ok = true; if (unk.length) addNet(unk[0], sg, d0); }
        }
        if (ok) { sup.add(p.ref); grew = true; }
      });
      if (!grew) break;
    }
    const real = parts.filter((p) => sup.has(p.ref) && p.type !== "V");
    if (real.length < 2 || real.length > parts.length * 0.6) return null;
    // supply nodes the rest of the circuit uses: rail flags in the audio section
    const feeds = new Set();
    parts.forEach((p) => { if (!sup.has(p.ref)) p.nodes.forEach((n) => { if (S.has(n)) feeds.add(n); }); });
    const out = [...S].filter((n) => feeds.has(n) && sign[n]).sort((a, b) => depth[b] - depth[a])[0] || [...S].sort((a, b) => depth[b] - depth[a])[0];
    return { sup, S, sign, depth, ac, feeds, out };
  }

  // The audio section (supply nodes become rail flags) and the supply section (AC sources drawn as parts, fed from
  // nothing, ending in the supply nodes the circuit uses), as two netlists for the placer.
  function splitCircuits(c, sp) {
    const audio = c.parts.filter((p) => !sp.sup.has(p.ref));
    const used = new Set();
    audio.forEach((p) => p.nodes.forEach((n) => { if (sp.S.has(n)) used.add(n); }));
    // the placeholder text reserves room for the live DC ("+000.0 V"): the layout never waits for the operating point
    const pseudo = [...used].map((n) => ({ ref: `supply:${n}`, type: "V", nodes: [n, "0"], value: sp.sign[n] > 0 ? 1 : sp.sign[n] < 0 ? -1 : 0, text: "+000.0 V", pseudo: true }));
    const cA = { ...c, parts: [...audio, ...pseudo] };
    const supParts = c.parts.filter((p) => sp.sup.has(p.ref) && p.type !== "V")
      .concat(sp.ac.map((p) => ({ ...p, type: "acsrc", nodes: p.nodes.slice(), text: "AC 000.0 V pk", acsrc: true })));
    const dcRails = c.parts.filter((p) => p.type === "V" && !sp.sup.has(p.ref) && supParts.some((q) => q.nodes.includes(p.nodes[0])));
    const acIn = acNodes(sp.ac[0])[0];
    const cS = { ...c, id: `${c.id}:supply`, parts: [...supParts, ...dcRails], io: { input: acIn, output: sp.out, noJacks: true }, layout: {}, probes: [], wideDc: true };
    return { cA, cS };
  }
  // Move a laid-out section (every coordinate the drawing, the hit tests and the overlap audit use).
  function shiftGeo(G, dx, dy) {
    const done = new Set();
    const box = (r) => { if (!r || done.has(r)) return; done.add(r); r.x0 += dx; r.x1 += dx; r.y0 += dy; r.y1 += dy; };
    G.placed.forEach((pl) => {
      const g = pl.g;
      g.its.forEach(box); box(g.body); box(pl.box); box(pl.lab.box);
      g.x += dx; g.y += dy; if (g.wX != null) g.wX += dx;
      g.flags = g.flags.map(([n, x, y, up]) => [n, x + dx, y + dy, up]);
      pl.lab.x += dx; pl.lab.y += dy;
    });
    G.nets.forEach((b) => { b.x += dx; b.y0 += dy; b.y1 += dy; b.top += dy; b.ys = b.ys.map((y) => y + dy); box(b.lab.box); });
    Object.keys(G.tx).forEach((n) => { G.tx[n] += dx; });
    G.inY += dy; G.outY += dy;
    box(G.bounds);
  }
  function placeView(c, view) {
    const pTS = TS, pM = MODE;
    TS = view === "illus" ? ILLUS_TS : 1; MODE = view;
    try { return placeAll(c); } finally { TS = pTS; MODE = pM; }
  }
  // layout cache: the geometry only depends on the netlist's structure and label text, not on knob positions or DC values
  const LAYOUTS = new Map();
  function layoutFor(c, view) {
    const supOn = !!V.supply;
    const key = `${c.id}~${view}|${supOn ? 1 : 0}|` + c.parts.map((p) => `${p.ref}:${p.type}:${p.nodes.join(",")}:${p.text || ""}:${p.model || ""}:${p.control || ""}`).join(";") + JSON.stringify(c.layout || {});
    let g = LAYOUTS.get(key);
    if (!g) {
      const t0 = performance.now();
      const sp = supplySplit(c);
      if (!sp) {
        g = placeView(c, view);
        g.sections = [{ name: "circuit", G: g }];
      } else {
        const { cA, cS } = splitCircuits(c, sp);
        const GA = placeView(cA, view);
        const secs = [{ name: "audio", G: GA }];
        if (supOn) {
          const GS = placeView(cS, view);
          shiftGeo(GS, GA.bounds.x0 - GS.bounds.x0, GA.bounds.y1 - GS.bounds.y0 + 70);
          secs.push({ name: "supply", G: GS });
        }
        g = { lay: GA.lay, tx: GA.tx, inY: GA.inY, outY: GA.outY, sections: secs, placed: [], nets: [], misses: [] };
        const B = { x0: Infinity, y0: Infinity, x1: -Infinity, y1: -Infinity };
        secs.forEach((sc) => {
          sc.G.placed.forEach((pl) => { pl.lay = sc.G.lay; pl.sec = sc.name; g.placed.push(pl); });
          sc.G.nets.forEach((b) => { b.sec = sc.name; g.nets.push(b); });
          g.misses.push(...sc.G.misses);
          B.x0 = Math.min(B.x0, sc.G.bounds.x0); B.y0 = Math.min(B.y0, sc.G.bounds.y0); B.x1 = Math.max(B.x1, sc.G.bounds.x1); B.y1 = Math.max(B.y1, sc.G.bounds.y1);
        });
        g.bounds = B;
      }
      g.supply = sp;
      g.view = view;
      g.ms = Math.round(performance.now() - t0);
      g.key = key;
      if (LAYOUTS.size > 12) LAYOUTS.delete(LAYOUTS.keys().next().value);
      LAYOUTS.set(key, g);
    }
    return g;
  }
  const camKey = (G) => G.key.split("|")[0];
  const figClass = (p, c) => (window.CircuitParts && CircuitParts.figClass ? CircuitParts.figClass(p, { circuit: c }) : "axial");

  /* ---------------- Illustrated view: the part drawer's art on the canvas ---------------- */
  // Every part is drawn with its drawer illustration (CircuitParts.figure), fitted into its slot in the placed layout,
  // turned to lie along its wire (two-terminal parts) and joined to the schematic's wire ends by short metal leads from
  // the drawing's own pin / lead ends. One <g> per distinct drawing in <defs>, a <use> per part.
  const FIGS = new Map();
  let FIGN = 0;
  function figKey(p, c, dc, on) {
    const v = (n) => (isGnd(n) ? 0 : dc[n] != null ? Math.round(+dc[n]) : "");
    const k = [p.type, p.value, p.model, p.family, p.package, p.text, p.tolerance, p.taper && (p.taper.type || p.taper), p.closed ? 1 : 0, on ? 1 : 0];
    if (p.type === "pot") k.push(Math.round((p.position != null ? p.position : 0.5) * 100));
    if (/^(C|D|Dpair)$/.test(p.type)) k.push(p.nodes.map(v).join(","));
    if (/^(triode|opamp|pentode)$/.test(p.type)) k.push(p.ref.replace(/^.*?([AB])$/, "$1"), p.nodes.join(","));
    if (p.type === "transformer" || p.type === "V" || p.acsrc) k.push(p.nodes.join(","), p.label || "");
    return k.join("|");
  }
  function figFor(p, c, dc, on) {
    const key = figKey(p, c, dc, on);
    let F = FIGS.get(key);
    if (!F) {
      const art = CircuitParts.figure(p.acsrc ? { ...p, type: "V", ac: true } : p, { circuit: c, dc, on });
      F = { id: `cvf${(++FIGN).toString(36)}`, key, art, bb: null };
      FIGS.set(key, F);
      if (FIGS.size > 3000) { // forget the oldest drawing and its bitmaps
        const [k0, F0] = FIGS.entries().next().value;
        FIGS.delete(k0);
        const r = RAST.get(F0.id);
        if (r) { RAST.delete(F0.id); (r.urls || []).forEach((u) => u && URL.revokeObjectURL(u)); }
      }
    }
    return F;
  }
  // bounding boxes of new drawings, measured in one layout pass in a hidden SVG
  function measureFigs(list) {
    const todo = list.filter((F) => !F.bb);
    if (!todo.length) return;
    const ns = "http://www.w3.org/2000/svg", host = document.createElementNS(ns, "svg");
    host.setAttribute("style", "position:absolute;left:-9999px;top:0;width:10px;height:10px;visibility:hidden;pointer-events:none");
    host.innerHTML = todo.map((F) => `<g>${F.art.defs ? `<defs>${F.art.defs}</defs>` : ""}<g>${F.art.shadow}</g><g>${F.art.body}</g></g>`).join("");
    document.body.appendChild(host);
    [...host.children].forEach((gEl, i) => {
      const F = todo[i], b = gEl.lastElementChild.getBBox(), sh = gEl.children[gEl.children.length - 2].getBBox();
      F.bb = [b.x, b.y, b.x + b.width, b.y + b.height];
      // the shadow may reach a little past the part; the fit keeps it inside the slot too
      F.fb = sh.width ? [Math.min(b.x, sh.x), Math.min(b.y, sh.y), Math.max(b.x + b.width, sh.x + sh.width), Math.max(b.y + b.height, sh.y + sh.height)] : F.bb;
      if (!isFinite(F.bb[0]) || b.width <= 0) F.bb = F.fb = [0, 0, F.art.w, F.art.h];
      // each anchor's way out of the drawing: toward the nearest edge of the body
      F.anchors = F.art.anchors.map((a) => {
        const d = [[a.x - F.bb[0], -1, 0], [F.bb[2] - a.x, 1, 0], [a.y - F.bb[1], 0, -1], [F.bb[3] - a.y, 0, 1]].sort((u, w) => u[0] - w[0])[0];
        return { ...a, dx: d[1], dy: d[2] };
      });
    });
    host.remove();
  }
  // where the drawing goes in a placed part's slot, and the wire ends its leads run to
  function artRect(pl) {
    const { s, g } = pl, k = s.kind, x = g.x, y = g.y, room = s.room || 0;
    if (k === "series") {
      const bot = pl.box.y1, w = s.wiper;
      if (!room) return [x + 1, y - 10, x + L - 1, y + 10];
      return [x + 5, y - 10 - room, x + L - 5, w && !w.rheo ? Math.min(bot - 8, y + 12) : y + 6];
    }
    if (k === "shunt" || k === "vert2") {
      if (!room) return [x - 10, y + 1, x + 10, y + L - 1];
      return g.artSide > 0 ? [x - 6, y + 5, x + 10 + room, y + L - 5] : [x - 10 - room, y + 5, x + 6, y + L - 5];
    }
    if (k === "opamp") return [x + 3, y - 21, x + 43, y + 21];
    if (k === "bjt") return [x - 12, y - 19, x + 12, y + 19];
    if (k === "triode" || k === "pentode") return [x - 21, y - g.h + 1, x + 17, y + g.h - 1];
    return [x + 3, y + 1, x + g.W - 3, y + g.hgt - 1];
  }
  function leadTargets(pl) {
    const { s, g } = pl;
    let pts = pinPoints(pl);
    if (s.kind === "opamp") pts = pts.filter((q) => q.i < 3);
    if (s.kind === "block") pts = pts.map((q) => ({ ...q, x: q.x < g.x ? g.x : g.x + g.W }));
    return pts;
  }
  const ROT = { 0: (x, y) => [x, y], 90: (x, y) => [-y, x], 180: (x, y) => [-x, -y], 270: (x, y) => [y, -x] };
  const POLAR = (p) => /^(D|Dpair)$/.test(p.type) || (p.type === "C" && /electrolytic/i.test(p.family || "")) || p.acsrc;
  function fitFigure(pl, F, p) {
    const R = artRect(pl), k = pl.s.kind, tg = leadTargets(pl);
    const rots = k === "series" ? [0, 180] : k === "shunt" || k === "vert2" ? [90, 270] : [0];
    const rw = R[2] - R[0], rh = R[3] - R[1], rcx = (R[0] + R[2]) / 2, rcy = (R[1] + R[3]) / 2;
    const name = (i) => (isGnd(p.nodes[i]) ? "GND" : String(p.nodes[i]));
    let best = null;
    const two = k === "series" || k === "shunt" || k === "vert2", horiz = k === "series";
    rots.forEach((rot) => {
      const f = ROT[rot], fb = F.bb;
      const cs = [[fb[0], fb[1]], [fb[2], fb[1]], [fb[0], fb[3]], [fb[2], fb[3]]].map(([a, b]) => f(a, b));
      const bx0 = Math.min(...cs.map((q) => q[0])), bx1 = Math.max(...cs.map((q) => q[0])), by0 = Math.min(...cs.map((q) => q[1])), by1 = Math.max(...cs.map((q) => q[1]));
      let sc = Math.min(rw / (bx1 - bx0), rh / (by1 - by0));
      let ox = rcx - sc * (bx0 + bx1) / 2, oy = rcy - sc * (by0 + by1) / 2;
      // a two-terminal part lies on its wire: its lead / pin ends land on the wire's line, so the leads run straight
      const main = two ? F.anchors.filter((a) => a.node === name(0) || a.node === name(p.nodes.length - 1)) : [];
      if (two && main.length) {
        const rp = main.map((a) => f(a.x, a.y)), ap = rp.reduce((u, q) => u + (horiz ? q[1] : q[0]), 0) / rp.length;
        const line = horiz ? pl.g.y : pl.g.x, lo = horiz ? by0 : bx0, hi = horiz ? by1 : bx1, Rlo = horiz ? R[1] : R[0], Rhi = horiz ? R[3] : R[2];
        const along = horiz ? rw / (bx1 - bx0) : rh / (by1 - by0);
        const s2 = Math.min(along, ap - lo > 1e-6 ? (line - Rlo) / (ap - lo) : Infinity, hi - ap > 1e-6 ? (Rhi - line) / (hi - ap) : Infinity);
        if (isFinite(s2) && s2 > 0) {
          sc = s2;
          if (horiz) { ox = rcx - sc * (bx0 + bx1) / 2; oy = line - sc * ap; } else { oy = rcy - sc * (by0 + by1) / 2; ox = line - sc * ap; }
        }
      }
      const an = F.anchors.map((a) => { const [u, v] = f(a.x, a.y), [du, dv] = f(a.dx, a.dy); return { ...a, X: ox + sc * u, Y: oy + sc * v, DX: du, DY: dv }; });
      const tries = tg.length === 2 && !POLAR(p) ? [[0, 1], [1, 0]] : [null];
      tries.forEach((perm) => {
        const used = new Set(), leads = [];
        let cost = 0;
        tg.forEach((t, j) => {
          const nm = name(perm ? tg[perm[j]].i : t.i);
          let a = an.find((q, qi) => q.node === nm && !used.has(qi) && !q.dim) || an.find((q, qi) => q.node === nm && !used.has(qi)) || an.find((q) => q.node === nm);
          if (!a) return;
          used.add(an.indexOf(a));
          cost += Math.hypot(a.X - t.x, a.Y - t.y);
          leads.push({ a, t });
        });
        cost += rot === 180 ? 24 : rot === 270 ? 0.5 : 0; // upright drawings win unless the other way round saves real lead
        if (!best || cost < best.cost) best = { cost, rot, sc, ox, oy, leads, wb: [ox + sc * bx0, oy + sc * by0, ox + sc * bx1, oy + sc * by1] };
      });
    });
    return best;
  }
  // a part's metal leads: from the drawing's pin out along its own direction, bending into the wire end's direction
  function leadPath(a, t, R) {
    const tdx = t.x <= R[0] + 0.5 ? 1 : t.x >= R[2] - 0.5 ? -1 : 0, tdy = tdx ? 0 : t.y <= R[1] + 0.5 ? 1 : t.y >= R[3] - 0.5 ? -1 : 0;
    const dist = Math.hypot(a.X - t.x, a.Y - t.y);
    if (dist < 0.4) return "";
    const f = (v) => +v.toFixed(2);
    if (Math.abs(a.X - t.x) < 0.6 || Math.abs(a.Y - t.y) < 0.6) return `M${f(a.X)} ${f(a.Y)} L${f(t.x)} ${f(t.y)}`;
    const ka = Math.max(1, Math.min(3.5, dist * 0.2)), kt = Math.max(1.5, Math.min(9, dist * 0.42));
    return `M${f(a.X)} ${f(a.Y)} C${f(a.X + a.DX * ka)} ${f(a.Y + a.DY * ka)} ${f(t.x + tdx * kt)} ${f(t.y + tdy * kt)} ${f(t.x)} ${f(t.y)}`;
  }
  function partFigSvg(pl, p, F) {
    const fit = pl.fit && pl.fitKey === F.id ? pl.fit : (pl.fit = fitFigure(pl, F, p), pl.fitKey = F.id, pl.fit);
    if (!fit) return "";
    const R = artRect(pl);
    const leads = fit.leads.map((l) => leadPath(l.a, l.t, R)).filter(Boolean).join(" ");
    const f = (v) => +v.toFixed(3);
    const tf = `translate(${f(fit.ox)} ${f(fit.oy)}) scale(${f(fit.sc)})${fit.rot ? ` rotate(${fit.rot})` : ""}`;
    const lead = leads ? `<path class="lead-o" d="${leads}"/><path class="lead" d="${leads}"/>` : "";
    return { tf, fit, lead, vec: (!fit.rot && F.art.shadow ? `<use href="#${F.id}s" transform="${tf}"/>` : "") + lead + `<use href="#${F.id}" transform="${tf}"/>` };
  }

  /* Performance (hundreds of drawings on screen): the vector drawings sit in spatial tiles and only the tiles near the
     viewport are displayed (checked when the camera moves, never while idle); below LOD_S each distinct drawing is
     shown as a bitmap rendered once (cached), which the GPU scales for free. */
  const TILE = 420, LOD_S = 1.2;
  const RAST = new Map();
  function artTiles(el) {
    V.tiles = [...el.querySelectorAll("svg.cv-svg .art > g[data-t]")].map((g) => { const [i, j] = g.dataset.t.split(",").map(Number); return { el: g, x0: i * TILE, y0: j * TILE, x1: (i + 1) * TILE, y1: (j + 1) * TILE, on: true }; });
    artCull();
  }
  const mipOf = (s) => (s >= LOD_S / 2 ? 0 : s >= LOD_S / 4 ? 1 : 2);
  function artCull() {
    const el = canvasEl(), c = V.cam;
    if (!el || !c || !CAM.cw || !V.geo) return;
    const illus = V.geo.view === "illus", lod = illus && c.s < LOD_S;
    el.classList.toggle("cv-lod", lod);
    if (!illus) return;
    // while the scale is changing the bitmaps stand in for the vector drawings (no re-rasterising every frame);
    // the vector drawings come back once the zoom has settled
    if (V.lastS != null && Math.abs(c.s - V.lastS) > 1e-6) {
      el.classList.add("cv-zooming");
      clearTimeout(V.zoomT);
      V.zoomT = setTimeout(() => { el.classList.remove("cv-zooming"); }, 200);
    }
    V.lastS = c.s;
    // the bitmap level whose size is closest to what is on screen (resampling a much bigger bitmap is slow)
    const mip = mipOf(c.s);
    if (mip !== V.mip && el.classList.contains("cv-rready")) artMip(el, mip);
    if (!V.tiles) return;
    const w = CAM.cw / c.s, h = CAM.ch / c.s, m = Math.max(140, w * 0.25);
    const x0 = c.x - m, y0 = c.y - m, x1 = c.x + w + m, y1 = c.y + h + m;
    V.tiles.forEach((t) => {
      const on = t.x1 > x0 && t.x0 < x1 && t.y1 > y0 && t.y0 < y1;
      if (on !== t.on) { t.on = on; t.el.style.display = on ? "" : "none"; }
    });
  }
  function artMip(el, mip) {
    V.mip = mip;
    el.querySelectorAll("svg.cv-svg .art .r image[data-f]").forEach((im) => {
      const r = RAST.get(im.dataset.f), u = r && r.urls && (r.urls[mip] || r.urls[0]);
      if (u && im.getAttribute("href") !== u) im.setAttribute("href", u);
    });
  }
  // one bitmap per distinct drawing (and half / quarter size levels), sized for the sharpest it is shown at below LOD_S
  function rasterFig(F, res) {
    const have = RAST.get(F.id);
    if (have && have.res >= res * 0.98) return have.p;
    const b = F.bb, pw = (b[2] - b[0]) * 0.03 + 2, ph = (b[3] - b[1]) * 0.03 + 2;
    const x0 = b[0] - pw, y0 = b[1] - ph, w = b[2] - b[0] + 2 * pw, h = b[3] - b[1] + 2 * ph;
    const W = Math.max(8, Math.min(900, Math.round(w * res))), H = Math.max(8, Math.round(h * W / w));
    const acc = getComputedStyle(canvasEl() || document.body).getPropertyValue("--accent").trim() || "#e3a04b";
    const css = `text{font-family:-apple-system,BlinkMacSystemFont,"Helvetica Neue",Arial,sans-serif}.cp-dial{stroke:${acc};fill:none;stroke-linecap:round}.cp-dialbg{stroke:rgba(20,22,26,.35);fill:none;stroke-linecap:round}.cp-ann{font:700 13px -apple-system,sans-serif;letter-spacing:.1em;fill:#6c6f78}.cp-ann.on{fill:${acc}}`;
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="${x0} ${y0} ${w} ${h}"><style>${css}</style><defs>${F.art.defs}</defs>${F.art.body}</svg>`;
    const rec = { res, box: [x0, y0, w, h], urls: null };
    rec.p = new Promise((resolve) => {
      const src = URL.createObjectURL(new Blob([svg], { type: "image/svg+xml" }));
      const img = new Image();
      const done = (urls) => { if (have && have.urls) have.urls.forEach((u) => { if (u && !urls.includes(u)) URL.revokeObjectURL(u); }); rec.urls = urls; resolve(rec); };
      const blob = (cv) => new Promise((r) => cv.toBlob((bl) => r(bl ? URL.createObjectURL(bl) : null), "image/png"));
      img.onload = () => {
        try {
          const lv = [];
          let prev = img;
          for (let k = 0; k < 3; k++) { // full, half, quarter: each drawn from the one before (a clean downsample)
            const cv = document.createElement("canvas");
            cv.width = Math.max(4, Math.round(W / 2 ** k)); cv.height = Math.max(4, Math.round(H / 2 ** k));
            const g = cv.getContext("2d");
            g.imageSmoothingQuality = "high";
            g.drawImage(prev, 0, 0, cv.width, cv.height);
            lv.push(cv); prev = cv;
          }
          Promise.all(lv.map(blob)).then((urls) => { URL.revokeObjectURL(src); done(urls); });
        } catch (e) { done([src]); } // a canvas that will not export: the SVG image itself (still one image to draw)
      };
      img.onerror = () => { URL.revokeObjectURL(src); done([]); };
      img.src = src;
    });
    RAST.set(F.id, rec);
    return rec.p;
  }
  function artRasters(el, need) {
    const seq = (V.rseq = (V.rseq || 0) + 1);
    el.classList.remove("cv-rready");
    if (!need.size) return;
    Promise.all([...need].map(([F, res]) => rasterFig(F, res))).then(() => {
      if (seq !== V.rseq || !el.isConnected) return;
      const mip = V.cam ? mipOf(V.cam.s) : 0;
      V.mip = mip;
      el.querySelectorAll("svg.cv-svg .art .r image[data-f]").forEach((im) => {
        const r = RAST.get(im.dataset.f), u = r && r.urls && (r.urls[mip] || r.urls[0]);
        if (!u) return;
        im.setAttribute("href", u);
        im.setAttribute("x", r.box[0]); im.setAttribute("y", r.box[1]); im.setAttribute("width", r.box[2]); im.setAttribute("height", r.box[3]);
      });
      el.classList.add("cv-rready");
    });
  }

  function schematicSvg(c, model, ctx, G) {
    const pTS = TS, pM = MODE;
    TS = G.view === "illus" ? ILLUS_TS : 1; MODE = G.view;
    try { return drawSvg(c, model, ctx, G); } finally { TS = pTS; MODE = pM; }
  }
  // the live DC (and, on a supply node, its ripple; on an AC source node, its peak) under a net's name
  function netDcText(n, v, b) {
    const sp = V.geo && V.geo.supply;
    if (!sp || !sp.S.has(n)) return fmtV(v);
    if (sp.sign[n] === 0) return acText(n);
    const tr = V.curves && V.curves.probes && V.curves.probes.traces && V.curves.probes.traces[n];
    if (!tr || !tr.length) return fmtV(v);
    let mn = Infinity, mx = -Infinity;
    tr.forEach((y) => { if (y < mn) mn = y; if (y > mx) mx = y; });
    const pp = mx - mn;
    return pp < 1e-4 ? fmtV(v) : `${fmtV(v)} · ${pp >= 10 ? `${pp.toFixed(0)} ` : pp >= 1 ? `${pp.toFixed(1)} ` : `${(pp * 1e3).toFixed(0)} m`}Vpp`;
  }
  function acText(n) {
    const tr = V.curves && V.curves.probes && V.curves.probes.traces && V.curves.probes.traces[n];
    if (!tr || !tr.length) return "AC";
    const pk = Math.max(...tr.map(Math.abs));
    return `AC ${pk >= 100 ? pk.toFixed(0) : pk.toFixed(1)} V pk`;
  }
  function drawSvg(c, model, ctx, G) {
    const lay = G.lay;
    const groupOf = {};
    (c.groups || []).forEach((g, i) => g.parts.forEach((r) => { groupOf[r] = i; }));
    const ovPaths = ctx.overridePaths ? ctx.overridePaths() : new Set();
    const byRef = Object.fromEntries(c.parts.map((p) => [p.ref, p]));
    const T = V.topo || {};
    const dc = (V.curves && V.curves.dc) || {};
    // Illustrated: the drawing of every placed part (cached per distinct drawing), measured before anything is fitted
    let figs = null, figDefs = "";
    if (G.view === "illus" && window.CircuitParts && CircuitParts.figure) {
      figs = new Map();
      const on = unitOn();
      G.placed.forEach((pl) => {
        const p = pl.s.p.acsrc ? pl.s.p : byRef[pl.s.p.ref] || pl.s.p;
        try { figs.set(p.ref, figFor(p, c, dc, on)); } catch (e) { console.error("figure", p.ref, e); }
      });
      const uniq = [...new Set(figs.values())];
      V.figKey = uniq.map((F) => F.id).join(",");
      measureFigs(uniq);
      // the floor shadow is its own symbol: only a drawing standing upright casts it (a turned one would cast it sideways)
      figDefs = uniq.map((F) => `${F.art.defs ? `<defs>${F.art.defs}</defs>` : ""}<g id="${F.id}">${F.art.body}</g>${F.art.shadow ? `<g id="${F.id}s">${F.art.shadow}</g>` : ""}`).join("");
    }
    const tiles = new Map(), need = new Map(), dpr = window.devicePixelRatio || 2;
    const live = {
      c, lay, sel: V.sel, part: (r) => byRef[r], edit: !!ctx.edit, dangling: danglingNets(c), figs,
      addArt(pl, p, F) {
        const a = partFigSvg(pl, p, F);
        if (!a) return;
        const k = `${Math.floor((pl.box.x0 + pl.box.x1) / 2 / TILE)},${Math.floor((pl.box.y0 + pl.box.y1) / 2 / TILE)}`;
        const t = tiles.get(k) || { v: "", r: "" };
        t.v += a.vec;
        t.r += `${a.lead}<image data-f="${F.id}" preserveAspectRatio="none" transform="${a.tf}"/>`;
        tiles.set(k, t);
        need.set(F, Math.max(need.get(F) || 0, Math.min(0.8, a.fit.sc * LOD_S * dpr * 1.2)));
      },
      isMod: (p) => [...ovPaths].some((x) => x.startsWith(`circuit.parts.${p.ref}.`)) || !!(T.wire && T.wire[p.ref]),
      isAdded: (p) => !!(T.add && T.add[p.ref]),
      tint: (p) => { const g = groupOf[p.ref]; return g == null ? "" : `style="--gc:${GROUP_COLORS[g % GROUP_COLORS.length]}"`; },
    };
    // wires grouped by net so a probed net lights up end to end
    const netD = {}, loose = [];
    G.placed.forEach((pl) => {
      // re-split the part's wire path by net: each wire item carries its net
      pl.g.its.forEach((it) => {
        if (it.k !== "h" && it.k !== "v") return;
        const d = it.k === "h" ? `M${it.x0} ${it.y0} H${it.x1}` : `M${it.x0} ${it.y0} V${it.y1}`;
        if (it.n != null) (netD[it.n] = netD[it.n] || []).push(d); else loose.push(d);
      });
    });
    let jacks = "";
    (G.sections || [{ G }]).forEach(({ G: sg }) => {
      const io = sg.lay.c.io;
      if (io.noJacks) return;
      const inN = io.input, outN = io.output, ix = sg.tx[inN], ox = sg.tx[outN];
      (netD[inN] = netD[inN] || []).push(`M${ix - 21} ${sg.inY} H${ix}`);
      if (ox != null) (netD[outN] = netD[outN] || []).push(`M${ox} ${sg.outY} H${ox + 25}`);
      jacks += `<g class="jack"><circle cx="${ix - 26}" cy="${sg.inY}" r="5"/><text x="${ix - 26}" y="${sg.inY + 20}" text-anchor="middle">IN</text></g>` +
        (ox != null ? `<g class="jack"><circle cx="${ox + 30}" cy="${sg.outY}" r="5"/><text x="${ox + 30}" y="${sg.outY + 20}" text-anchor="middle">OUT</text></g>` : "");
    });
    const netSvg = G.nets.map((b) => {
      const n = b.n, pi = V.probes.indexOf(n);
      const color = pi >= 0 ? TRACE_COLORS[pi % TRACE_COLORS.length] : null;
      const st = color ? ` style="stroke:${color}"` : "";
      const cnt = {};
      b.ys.forEach((y) => { cnt[y] = (cnt[y] || 0) + 1; });
      const dots = Object.keys(cnt).map(Number).filter((y) => cnt[y] > 1 || (y > b.y0 + 0.5 && y < b.y1 - 0.5) || (y === b.y0 && b.top < b.y0 - 0.5)).map((y) => `<circle class="dot" cx="${b.x}" cy="${y}" r="${2.6 * (TS < 1 ? 0.75 : 1)}"${color ? ` style="fill:${color}"` : ""}/>`).join("");
      const v = dc[n], lb = b.lab, bx = lb.box;
      const tx = lb.an === "middle" ? b.x : lb.an === "start" ? bx.x0 + 1 : bx.x1 - 1;
      return `<g class="net${pi >= 0 ? " probed" : ""}${live.dangling.has(n) ? " dangling" : ""}" data-net="${esc(n)}"${b.sec ? ` data-sec="${b.sec}"` : ""}><path class="bus" d="M${b.x} ${b.top} V${b.y1}${(netD[n] || []).join("")}"${st}/>${dots}
        <path class="net-hit" d="M${b.x} ${b.top - 4} V${b.y1 + 4}"><title>Scope ${esc(n)}</title></path>
        <rect class="net-lbl-hit" x="${bx.x0}" y="${bx.y0}" width="${bx.x1 - bx.x0}" height="${bx.y1 - bx.y0}"/>
        ${v != null ? `<text class="net-dc" x="${tx}" y="${bx.y0 + 9 * TS}" text-anchor="${lb.an}">${esc(netDcText(n, v, b))}</text>` : ""}
        <text class="net-lbl" x="${tx}" y="${bx.y0 + 21 * TS}" text-anchor="${lb.an}"${color ? ` style="fill:${color}"` : ""}>${esc(n)}</text></g>`;
    }).join("");
    // the power supply section: a frame and its title in the gap above it
    let frames = "";
    (G.sections || []).filter((sc) => sc.name === "supply").forEach(({ G: sg }) => {
      const B = sg.bounds, sp = G.supply, n = sg.placed.filter((pl) => !pl.s.p.acsrc).length;
      const feeds = [...sp.feeds].filter((x) => sp.sign[x] !== 0).sort((a, b) => a.localeCompare(b, undefined, { numeric: true }));
      frames += `<g class="cv-sec"><rect class="cv-sec-frame" x="${B.x0 + 6}" y="${B.y0 + 4}" width="${B.x1 - B.x0 - 12}" height="${B.y1 - B.y0 - 8}" rx="14"/>
        <text class="cv-sec-t" x="${B.x0 + 22}" y="${B.y0 + 4 - 9 * TS}">POWER SUPPLY<tspan class="cv-sec-s" dx="${10 * TS}">${esc(`as modelled · ${n} parts · feeds ${feeds.join(", ")}`)}</tspan></text></g>`;
    });
    const parts = G.placed.map((pl) => partSvg(c, pl, live)).join("");
    // each tile holds its parts' vector drawings (.v) and their bitmaps (.r); CSS shows one of the two
    const art = figs ? `<g class="art">${[...tiles].map(([k, t]) => `<g data-t="${k}"><g class="v">${t.v}</g><g class="r">${t.r}</g></g>`).join("")}</g>` : "";
    V.rneed = need;
    const B = G.bounds, pad = 2000;
    // the drawings' symbols live in a separate, fixed SVG (.cv-figs): moving the camera re-lays-out the canvas SVG, and
    // thousands of symbol elements in its <defs> would be laid out again on every frame
    V.figDefs = figDefs;
    return `<defs><pattern id="cv-grid" width="24" height="24" patternUnits="userSpaceOnUse"><circle cx="12" cy="12" r="0.9" class="cv-grid-dot"/></pattern></defs>
      <rect class="cv-bg" x="${B.x0 - pad}" y="${B.y0 - pad}" width="${B.x1 - B.x0 + 2 * pad}" height="${B.y1 - B.y0 + 2 * pad}" fill="url(#cv-grid)"/>
      ${frames}${loose.length ? `<path class="stub" d="${loose.join("")}"/>` : ""}${netSvg}${jacks}${art}${parts}`;
  }

  /* ---------------- canvas: pan and zoom (SVG viewBox) ---------------- */
  // V.cam = { x, y, s }: the world point at the canvas' top-left corner and the scale in screen px per unit.
  // Nothing runs while idle: wheel/drag/gesture input is coalesced into one requestAnimationFrame, and the
  // only animation (zoom-to-fit, the zoom buttons) is a short tween that stops when the Component tab hides.
  const CAM = { raf: 0, tween: null, ro: null, cw: 0, ch: 0 };
  const ZMAX = 4, READABLE = 0.8;
  // the Illustrated view's world is drawn with smaller text (TS): every zoom limit scales with it
  const ZS = () => (V.geo && V.geo.view === "illus" ? 1 / ILLUS_TS : 1);
  // a zoom animation never outlives the Component tab being on screen
  if (window.HRActivity) HRActivity.on(() => { if (!HRActivity.isActive("component")) { camStop(); if (CAM.raf) { cancelAnimationFrame(CAM.raf); CAM.raf = 0; } } });
  function canvasEl() { return V.host && V.host.querySelector(".cv-canvas"); }
  function camFit(G, cw, ch) {
    const B = G.bounds, bw = B.x1 - B.x0, bh = B.y1 - B.y0;
    const s = Math.max(0.05, Math.min(1.5 * ZS(), Math.min(cw / bw, ch / bh) * 0.97));
    return { x: B.x0 - (cw / s - bw) / 2, y: B.y0 - (ch / s - bh) / 2, s };
  }
  // The schematic's viewport on screen: the SVG, which in edit mode starts right of the docked parts palette (so the
  // palette never covers a part, label or wire). Every screen <-> world conversion goes through it.
  function viewRect() {
    const el = canvasEl(), sv = el && el.querySelector("svg.cv-svg");
    return (sv || el).getBoundingClientRect();
  }
  function camLimits() {
    const G = V.geo, f = camFit(G, CAM.cw, CAM.ch);
    return { min: Math.max(0.05, Math.min(f.s, READABLE * ZS()) * 0.75), max: ZMAX * ZS() };
  }
  function camInitial(G, cw, ch) {
    const f = camFit(G, cw, ch), z = ZS(), cap = 1.25 * z;
    if (f.s >= READABLE * z) return { ...f, s: Math.min(f.s, cap), x: f.s > cap ? G.bounds.x0 - (cw / cap - (G.bounds.x1 - G.bounds.x0)) / 2 : f.x, y: f.s > cap ? G.bounds.y0 - (ch / cap - (G.bounds.y1 - G.bounds.y0)) / 2 : f.y };
    // too big to read when fitted: start readable at the input, top-aligned (fit is one double-click away)
    const s = READABLE * z, B = G.bounds;
    return { x: B.x0, y: (B.y1 - B.y0) * s < ch ? B.y0 - (ch / s - (B.y1 - B.y0)) / 2 : B.y0, s };
  }
  function camClamp(c) {
    const B = V.geo.bounds, vw = CAM.cw / c.s, vh = CAM.ch / c.s;
    const mx = Math.min(vw * 0.75, 400 / c.s), my = Math.min(vh * 0.75, 300 / c.s);
    c.x = Math.max(B.x0 - vw + mx, Math.min(B.x1 - mx, c.x));
    c.y = Math.max(B.y0 - vh + my, Math.min(B.y1 - my, c.y));
    return c;
  }
  function hinted() {
    const el = canvasEl();
    if (!el || el.classList.contains("hinted")) return;
    el.classList.add("hinted");
    try { localStorage.setItem("headroom.circuit.hinted", "1"); } catch (e) { /* private mode */ }
  }
  function camApply() {
    CAM.raf = 0;
    const el = canvasEl(), svg = el && el.querySelector("svg.cv-svg");
    if (!svg || !V.cam || !CAM.cw) return;
    const c = V.cam;
    if (V.camFor) V.cams[V.camFor] = c;
    svg.setAttribute("viewBox", `${c.x.toFixed(2)} ${c.y.toFixed(2)} ${(CAM.cw / c.s).toFixed(2)} ${(CAM.ch / c.s).toFixed(2)}`);
    const pct = el.querySelector(".cv-zoom .pct");
    if (pct) pct.textContent = `${Math.round((c.s / ZS()) * 100)}%`;
    el.classList.toggle("zoomed-out", c.s < 0.45 * ZS());
    artCull();
  }
  function camSchedule() { if (!CAM.raf) CAM.raf = requestAnimationFrame(camApply); }
  function camStop() { if (CAM.tween) { cancelAnimationFrame(CAM.tween.raf); CAM.tween = null; } }
  function camSet(c, animate) {
    camStop();
    c = camClamp({ ...c });
    const reduce = window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches;
    const awake = !window.HRActivity || HRActivity.isActive("component");
    if (!animate || reduce || !awake || !V.cam) { V.cam = c; camSchedule(); return; }
    const a = { ...V.cam }, t0 = performance.now(), T = 240;
    const ls0 = Math.log(a.s), ls1 = Math.log(c.s);
    // interpolate the world point at the canvas centre and the log of the scale (no swooping)
    const ca = { x: a.x + CAM.cw / a.s / 2, y: a.y + CAM.ch / a.s / 2 }, cb = { x: c.x + CAM.cw / c.s / 2, y: c.y + CAM.ch / c.s / 2 };
    const step = (now) => {
      const u = Math.min(1, (now - t0) / T), e = 1 - Math.pow(1 - u, 3);
      const s = Math.exp(ls0 + (ls1 - ls0) * e), cx = ca.x + (cb.x - ca.x) * e, cy = ca.y + (cb.y - ca.y) * e;
      V.cam = { s, x: cx - CAM.cw / s / 2, y: cy - CAM.ch / s / 2 };
      camApply();
      if (u < 1 && CAM.tween) CAM.tween.raf = requestAnimationFrame(step);
      else { CAM.tween = null; V.cam = c; camApply(); }
    };
    CAM.tween = { raf: requestAnimationFrame(step) };
  }
  function camZoomAt(px, py, f, animate) {
    if (!V.cam) return false;
    const lim = camLimits(), c = V.cam;
    const s = Math.max(lim.min, Math.min(lim.max, c.s * f));
    if (Math.abs(s - c.s) < 1e-4) return false;
    const wx = c.x + px / c.s, wy = c.y + py / c.s;
    camSet({ s, x: wx - px / s, y: wy - py / s }, animate);
    return true;
  }
  function camFitNow(animate) { if (V.geo && CAM.cw) camSet(camFit(V.geo, CAM.cw, CAM.ch), animate); }
  function camMeasure() {
    const el = canvasEl();
    if (!el) return false;
    const vr = viewRect(), cw = Math.round(vr.width), ch = Math.round(vr.height);
    if (!cw || !ch) return false;
    const was = { cw: CAM.cw, ch: CAM.ch };
    CAM.cw = cw; CAM.ch = ch;
    if (!V.cam || V.camFor !== camKey(V.geo)) {
      V.cam = V.cams[camKey(V.geo)] || camInitial(V.geo, cw, ch);
      V.camFor = camKey(V.geo);
    } else if (was.cw && (was.cw !== cw || was.ch !== ch)) {
      // keep the world point at the centre where it was
      const c = V.cam;
      c.x += (was.cw - cw) / c.s / 2; c.y += (was.ch - ch) / c.s / 2;
    }
    camApply();
    return true;
  }
  function wireCanvas(el) {
    if (window.ResizeObserver) { if (CAM.ro) CAM.ro.disconnect(); CAM.ro = new ResizeObserver(() => camMeasure()); CAM.ro.observe(el); }
    el.addEventListener("wheel", onWheel, { passive: false });
    el.addEventListener("dblclick", (e) => { if (e.target.closest(".part, .cv-zoom, .cv-views")) return; e.preventDefault(); camFitNow(true); });
    // Safari / WKWebView trackpad pinch
    let g0 = null;
    el.addEventListener("gesturestart", (e) => { e.preventDefault(); const r = viewRect(); g0 = { s: V.cam ? V.cam.s : 1, px: (e.clientX || r.left + r.width / 2) - r.left, py: (e.clientY || r.top + r.height / 2) - r.top }; });
    el.addEventListener("gesturechange", (e) => { e.preventDefault(); if (!g0 || !V.cam) return; camZoomAt(g0.px, g0.py, (g0.s * e.scale) / V.cam.s, false); });
    el.addEventListener("gestureend", (e) => { e.preventDefault(); g0 = null; });
    el.addEventListener("keydown", (e) => {
      if (e.target !== el || !V.cam) return;
      const k = e.key, st = 60;
      if (k === "+" || k === "=") camZoomAt(CAM.cw / 2, CAM.ch / 2, 1.3, true);
      else if (k === "-" || k === "_") camZoomAt(CAM.cw / 2, CAM.ch / 2, 1 / 1.3, true);
      else if (k === "0") camFitNow(true);
      else if (/^Arrow/.test(k)) { const c = V.cam; camSet({ ...c, x: c.x + (k === "ArrowRight" ? st : k === "ArrowLeft" ? -st : 0) / c.s, y: c.y + (k === "ArrowDown" ? st : k === "ArrowUp" ? -st : 0) / c.s }, true); }
      else return;
      e.preventDefault();
    });
  }
  // Mouse wheel and trackpad: pinch (ctrl+wheel) and vertical scroll zoom about the cursor; a sideways or
  // shift-scroll pans. Zooming out past the minimum lets the page scroll on, so the canvas never traps it.
  function onWheel(e) {
    if (!V.cam || !CAM.cw) return;
    const r = viewRect();
    let dx = e.deltaX, dy = e.deltaY;
    if (e.deltaMode === 1) { dx *= 16; dy *= 16; } else if (e.deltaMode === 2) { dx *= CAM.ch; dy *= CAM.ch; }
    const pinch = e.ctrlKey || e.metaKey;
    if (!pinch && (e.shiftKey || Math.abs(dx) > Math.abs(dy) * 0.5 && Math.abs(dx) > 0.5)) {
      e.preventDefault();
      camStop();
      const c = V.cam, sx = e.shiftKey && !dx ? dy : dx, sy = e.shiftKey && !dx ? 0 : dy;
      V.cam = camClamp({ ...c, x: c.x + sx / c.s, y: c.y + sy / c.s });
      camSchedule();
      return;
    }
    const f = Math.exp(-Math.max(-60, Math.min(60, dy)) * (pinch ? 0.012 : 0.0025));
    const lim = camLimits();
    if (f < 1 && V.cam.s <= lim.min + 1e-4) return; // fully out: let the page scroll
    e.preventDefault();
    hinted();
    camStop();
    camZoomAt(e.clientX - r.left, e.clientY - r.top, f, false);
  }
  function startPan(e) {
    const el = canvasEl();
    if (!el || !V.cam) return;
    camStop();
    const x0 = e.clientX, y0 = e.clientY, c0 = { ...V.cam };
    let moved = false;
    const move = (ev) => {
      const dx = ev.clientX - x0, dy = ev.clientY - y0;
      if (!moved && Math.hypot(dx, dy) < 4) return;
      if (!moved) { moved = true; V.dragMoved = true; el.classList.add("panning"); hinted(); }
      V.cam = camClamp({ ...c0, x: c0.x - dx / c0.s, y: c0.y - dy / c0.s });
      camSchedule();
    };
    const up = () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      window.removeEventListener("pointercancel", up);
      el.classList.remove("panning");
      if (moved) V.dragEnd = performance.now();
      if (V.cam && V.geo) V.cams[V.camFor] = { ...V.cam };
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    window.addEventListener("pointercancel", up);
  }
  // Keep a part on screen (left of the drawer when it is open).
  function revealPart(ref, animate) {
    const el = canvasEl();
    const pl = V.geo && V.geo.placed.find((q) => q.s.p.ref === ref);
    if (!el || !pl || !V.cam || !CAM.cw) return;
    const c = V.cam, b = pl.box, lb = pl.lab.box;
    const x0 = Math.min(b.x0, lb.x0), x1 = Math.max(b.x1, lb.x1), y0 = Math.min(b.y0, lb.y0), y1 = Math.max(b.y1, lb.y1);
    let right = CAM.cw;
    if (V.drawer && V.drawer.classList.contains("open")) {
      const er = viewRect(), pr = (V.drawer.offsetParent || document.body).getBoundingClientRect();
      right = Math.min(CAM.cw, pr.right - V.drawer.offsetWidth - er.left);
    }
    const m = 30;
    let sx0 = (x0 - c.x) * c.s, sx1 = (x1 - c.x) * c.s, sy0 = (y0 - c.y) * c.s, sy1 = (y1 - c.y) * c.s;
    let dx = 0, dy = 0;
    if (sx1 > right - m) dx = sx1 - (right - m);
    if (sx0 - dx < m) dx = sx0 - m;
    if (sy1 > CAM.ch - m) dy = sy1 - (CAM.ch - m);
    if (sy0 - dy < m) dy = sy0 - m;
    if (dx || dy) camSet({ ...c, x: c.x + dx / c.s, y: c.y + dy / c.s }, animate);
  }

  function render(sec, model, ctx) {
    injectCss();
    V.srcModel = model;
    V.model = model;
    V.ctx = ctx;
    if (!model.circuit || !model.circuit.parts) { V.host = sec; sec.innerHTML = `<div class="cd-card" style="padding:16px">${esc(model.error || "Circuit unavailable")}</div>`; return; }
    editorSync(model, ctx);
    // what is drawn: the engine's circuit with the editor's draft applied (identical when nothing is edited)
    const c = displayCircuit(model);
    V.model = c === model.circuit ? model : { ...model, circuit: c };
    V.lint = lintCircuit(c);
    if (V.drawer && !V.drawer.isConnected) { V.drawer = null; V.sel = null; } // the page was closed and rebuilt
    if (V.layoutKey && V.layoutKey !== c.id) V.curves = null; // another circuit: the last one's operating point is not this one's
    if (!V.probes || V.layoutKey !== c.id) { V.probes = (c.probes || []).filter((n) => n !== c.io.input).slice(0, 3); V.sel = null; V.layoutKey = c.id; }
    if (V.sel && !partByRef(V.sel)) V.sel = null;
    loadParts();
    // the Illustrated view needs the part drawings (loaded on demand): until they are here, the schematic stands in
    const view = V.view === "illus" ? (window.CircuitParts ? "illus" : (loadParts(() => { if (V.host && V.srcModel && V.view === "illus") render(V.host, V.srcModel, V.ctx); }), "schematic")) : "schematic";
    let G;
    try { G = layoutFor(c, view); } catch (err) { console.error(err); sec.innerHTML = `<div class="cd-card" style="padding:16px">Could not draw this circuit (${esc(err.message)})</div>`; return; }
    V.geo = G;
    const groups = (c.groups || []).map((g, i) => `<span class="g"><span class="sw" style="background:${GROUP_COLORS[i % GROUP_COLORS.length]}"></span>Partition ${i + 1}</span>`).join("");
    const rails = Object.values(G.lay.rails).filter((r) => !r.pseudo).map((r) => `<span class="rail">${esc(r.nodes[0])} = ${esc(isAcSource(r) ? "AC" : r.text || r.value + " V")}</span>`).join("");
    const imp = ctx.edit ? `<button class="cd-btn ghost cv-import" data-cv-import="1" title="Replace this unit's circuit with a SPICE netlist (R C L V D Q E G; *@pot and *@io annotations). Its pots follow this unit's knobs.">Import SPICE…</button><input type="file" accept=".cir,.sp,.spi,.net,.txt,.ckt" data-cv-file="1" hidden>` : "";
    const editing = !!(ctx.edit && V.baseNet);
    const head = `<h2>${esc(c.name)}</h2><span class="s">${c.parts.filter((p) => p.type !== "V").length} components · ${editing
      ? "drag parts in from the palette, drag a pin onto a net to wire it, click a net's name to rename it"
      : "click a part to open it, drag a pot to turn it, click a net to scope it"}</span>${imp}`;
    const svgInner = schematicSvg(c, model, ctx, G);
    // keep the canvas (and its listeners, pan state, pointer capture) across re-renders; only its content changes
    let el = sec.querySelector(".cv-canvas");
    if (!el || V.host !== sec) {
      sec.innerHTML = `<div class="cd-sec-h cv-head"></div>
        <div class="cd-card cv-wrap"><div class="cv-legend"></div><div class="cv-status" role="status" aria-live="polite" hidden></div>
          <div class="cv-canvas cv-schem" tabindex="0" aria-label="Schematic: drag to pan, scroll or pinch to zoom, double-click to fit">
            <svg class="cv-svg" role="img" aria-label="Schematic" preserveAspectRatio="xMinYMin meet"></svg>
            <svg class="cv-figs" aria-hidden="true" focusable="false"></svg>
            <div class="cv-tools" role="toolbar" aria-label="Circuit editor" hidden></div>
            <div class="cv-zoom" role="toolbar" aria-label="Zoom">
              <button data-cv-zoom="out" title="Zoom out (−)" aria-label="Zoom out">${ICO_MINUS}</button><span class="pct">100%</span><button data-cv-zoom="in" title="Zoom in (+)" aria-label="Zoom in">${ICO_PLUS}</button><i></i><button data-cv-zoom="fit" title="Zoom to fit (double-click the canvas, or 0)" aria-label="Zoom to fit">${ICO_FIT}</button>
            </div>
            <div class="cv-hint">Drag to pan · scroll or pinch to zoom · double-click to fit</div>
            <div class="cv-views" role="toolbar" aria-label="View">
              <div class="seg" role="group" aria-label="Drawing style"><button data-cv-view="schematic" title="Electrical schematic symbols">Schematic</button><button data-cv-view="illus" title="Each part drawn as the real component">Illustrated</button></div>
              <button class="cv-sup" data-cv-supply="1" title="Lay out the power supply this circuit models as its own section (its rail flags link to it)">${ICO_BOLT}<span>Power supply</span></button>
            </div>
          </div></div>
        <div class="cd-card cv-scope" id="cv-scope"></div>`;
      el = sec.querySelector(".cv-canvas");
      V.host = sec;
      try { if (localStorage.getItem("headroom.circuit.hinted")) el.classList.add("hinted"); } catch (e) { /* private mode */ }
      wireCanvas(el);
    }
    // the canvas is as tall as the circuit needs at a readable scale (within the window)
    const bh = G.bounds.y1 - G.bounds.y0, maxH = Math.max(440, Math.min(1100, window.innerHeight - 290));
    el.style.height = `${Math.round(Math.max(420, Math.min(maxH, bh * READABLE * ZS() + 70)))}px`;
    el.classList.toggle("cv-illus", G.view === "illus");
    el.querySelectorAll("[data-cv-view]").forEach((b) => b.setAttribute("aria-pressed", String(b.dataset.cvView === (V.view === "illus" ? "illus" : "schematic"))));
    const sb = el.querySelector("[data-cv-supply]");
    if (sb) { sb.hidden = !G.supply; sb.setAttribute("aria-pressed", String(!!V.supply)); }
    sec.querySelector(".cv-head").innerHTML = head;
    const tabs = document.getElementById("cd-tabs"); // the page's sticky sub-tab bar: the title row sticks just below it
    if (tabs && tabs.offsetHeight) sec.style.setProperty("--cv-tabs-h", `${tabs.offsetHeight}px`);
    sec.querySelector(".cv-legend").innerHTML = groups + rails;
    const hint = el.querySelector(".cv-hint");
    if (hint) hint.textContent = editing ? `Drag a pin onto a net to wire it · Delete removes the selected part · ${MOD}Z undoes` : "Drag to pan · scroll or pinch to zoom · double-click to fit";
    paintEditor();
    const figSvg = el.querySelector("svg.cv-figs");
    const fk = G.view === "illus" ? V.figKey || "" : "";
    if (figSvg && figSvg.dataset.k !== fk) { figSvg.innerHTML = fk ? `<defs>${V.figDefs || ""}</defs>` : ""; figSvg.dataset.k = fk; }
    el.querySelector("svg.cv-svg").innerHTML = svgInner;
    V.tiles = null;
    if (G.view === "illus") { artTiles(el); artRasters(el, V.rneed || new Map()); }
    // group tints on part bodies
    el.querySelectorAll(".part[style]").forEach((g) => g.querySelectorAll(".body").forEach((b) => { if (!g.classList.contains("sel")) b.style.stroke = "color-mix(in srgb, var(--gc) 55%, var(--text))"; }));
    sec.onclick = onClick;
    sec.onpointerdown = onPointerDown;
    sec.onchange = onChange;
    sec.oninput = onInput;
    if (V.camFor !== camKey(G)) V.cam = null;
    el.classList.toggle("docked", editing);
    camMeasure();
    // entering edit mode narrows the viewport by the docked palette: keep the circuit's start in view
    if (editing && !V.wasEditing && V.cam && CAM.cw) {
      const sx = (G.bounds.x0 - V.cam.x) * V.cam.s;
      if (sx < 0) camSet({ ...V.cam, x: G.bounds.x0 }, false);
    }
    V.wasEditing = editing;
    renderScope();
    renderDrawer();
  }

  function fmtV(v) {
    const a = Math.abs(v);
    if (a < 5e-7) return "0 V";
    if (a >= 100) return `${v.toFixed(1)} V`;
    if (a >= 1) return `${v.toFixed(2)} V`;
    if (a >= 1e-3) return `${(v * 1e3).toFixed(1)} mV`;
    return `${(v * 1e6).toFixed(0)} µV`;
  }
  function fmtEng(v, unit) {
    const a = Math.abs(v);
    const P = [[1e6, "M"], [1e3, "k"], [1, ""], [1e-3, "m"], [1e-6, "µ"], [1e-9, "n"], [1e-12, "p"]];
    for (const [m, s] of P) if (a >= m * 0.9999) return `${+(v / m).toPrecision(4)} ${s}${unit}`;
    return `${v.toPrecision(3)} ${unit}`;
  }

  function partByRef(ref) { return (V.model.circuit.parts || []).find((p) => p.ref === ref); }
  const TYPE_NAME = { R: "Resistor", C: "Capacitor", L: "Inductor", pot: "Potentiometer", switch: "Switch", D: "Diode", Dpair: "Diode pair", npn: "NPN transistor", pnp: "PNP transistor",
    njf: "N-channel JFET", pjf: "P-channel JFET", opamp: "Op-amp", triode: "Triode", pentode: "Pentode", transformer: "Transformer", V: "DC source", vcvs: "VCVS", vccs: "VCCS", ldr: "LDR", speaker: "Speaker", sub: "Sub-circuit" };
  const FAMILY_NAME = { "power-pentode": "Power pentode", "vacuum-rectifier": "Vacuum rectifier", optocoupler: "Optocoupler", speaker: "Speaker", reverb: "Reverb tank", choke: "Choke",
    "germanium-diode": "Germanium diode", "schottky-diode": "Schottky diode", "rectifier-diode": "Rectifier diode", led: "LED" };
  const typeName = (p) => FAMILY_NAME[p.family] || (p.type === "sub" && /speaker/i.test(p.of || "") ? "Speaker" : p.type === "sub" && /spring|reverb|tank/i.test(p.of || "") ? "Reverb tank" : TYPE_NAME[p.type] || p.type);

  // The Details tab of a circuit unit: what the circuit is, its parts list, device models, how the engine
  // partitions and solves it, sources and notes (component-detail.js puts it in its own tab).
  const GROUP_ORDER = ["Triode", "Pentode", "Power pentode", "Op-amp", "NPN transistor", "PNP transistor", "N-channel JFET", "P-channel JFET", "Diode", "Diode pair", "Germanium diode", "Schottky diode", "Rectifier diode", "LED",
    "Potentiometer", "Switch", "Resistor", "Capacitor", "Inductor", "Choke", "Transformer", "Optocoupler", "Reverb tank", "Speaker", "DC source"];
  const plural = (s, n) => (n === 1 ? s : /x$/.test(s) ? `${s}es` : /y$/.test(s) ? `${s.slice(0, -1)}ies` : `${s}s`);
  function detailsHTML(model) {
    const c = model && model.circuit;
    if (!c || !c.parts) return "";
    const sec = (title, sub, body, cls = "") => `<section class="cd-card cd-dt-sec ${cls}"><h3>${esc(title)}${sub ? `<span>${sub}</span>` : ""}</h3>${body}</section>`;
    const refBtn = (r) => `<button class="cd-dt-ref" data-goto-part="${esc(r)}" title="Show ${esc(r)} on the schematic">${esc(r)}</button>`;
    const nGroups = (c.groups || []).length;
    const verified = model.verification === "spice-validated" ? "Checked against SPICE" : model.verification === "hardware" ? "Measured against hardware" : "";
    const chips = [`${c.parts.filter((p) => p.type !== "V").length} components`, `${nGroups} solver partition${nGroups === 1 ? "" : "s"}`, verified, model.oversampling > 1 ? `${model.oversampling}× oversampled` : "", model.ends_in_cab ? "Speaker + cab in the circuit" : ""].filter(Boolean);
    // a netlist that ends in speaker + cab (docs/CIRCUIT_ENGINE.md §10.21) outputs the microphone: the rack adds no cabinet after it
    const cabNote = model.ends_in_cab ? `<p class="mute">This circuit ends in its speakers and cabinet: the output is the microphone in front of the cab, so the rack adds no cabinet after this unit and bypasses one that follows it (turn it back on if you want both).</p>` : "";
    const about = sec("About this circuit", c.version ? `revision ${esc(c.version)}` : "",
      `<div class="cd-dt-name">${esc(c.name)}</div><div class="cd-dt-chips">${chips.map((x) => `<span>${esc(x)}</span>`).join("")}</div>
       ${c.description ? `<p>${esc(c.description)}</p>` : ""}${cabNote}${model.verification_scope ? `<p class="mute">${esc(model.verification_scope)}</p>` : ""}`);
    // parts list, grouped by kind
    const groups = new Map();
    c.parts.forEach((p) => { const k = typeName(p); if (!groups.has(k)) groups.set(k, []); groups.get(k).push(p); });
    const order = [...groups.keys()].sort((a, b) => ((GROUP_ORDER.indexOf(a) + 1) || 99) - ((GROUP_ORDER.indexOf(b) + 1) || 99) || a.localeCompare(b));
    const rows = order.map((k) => {
      const ps = groups.get(k);
      return `<tr class="grp"><th colspan="4">${esc(plural(k, ps.length))} <span>${ps.length}</span></th></tr>` + ps.map((p) =>
        `<tr><td>${refBtn(p.ref)}</td><td class="v">${esc(p.type === "V" ? p.text || `${p.value} V` : p.model || p.text || "")}</td><td class="r">${esc(p.label || "")}</td><td class="t">${p.tolerance ? `±${+(p.tolerance * 100).toFixed(1)} %` : ""}</td></tr>`).join("");
    }).join("");
    const nRails = c.parts.filter((p) => p.type === "V").length;
    const bom = sec("Parts list", `${c.parts.length - nRails} components${nRails ? ` + ${nRails} supply rail${nRails === 1 ? "" : "s"}` : ""} · click one to find it on the schematic`, `<table class="cd-dt-bom"><tbody>${rows}</tbody></table>`);
    // controls
    const ctls = (c.controls || []).filter((k) => !k.legacyOnly);
    const legacy = (c.controls || []).filter((k) => k.legacyOnly);
    const controls = ctls.length ? sec("Controls", "what each knob and switch moves", ctls.map((k) => {
      const ps = c.parts.filter((p) => p.control === k.id).map((p) => p.ref);
      return `<div class="cd-dt-row"><div class="k">${esc(k.label)}${k.mod ? `<span class="mod-tag" title="${esc(k.modNote || "A modification: not on the original unit")}">mod</span>` : ""}${k.group && k.group !== "Main" ? `<small>${esc(k.group)}</small>` : ""}</div><div class="v">${ps.length ? `<div class="refs">${ps.map(refBtn).join("")}</div>` : ""}${k.note ? `<div class="n">${esc(k.note)}</div>` : ""}</div></div>`;
    }).join("") + (legacy.length ? `<p class="mute small">Kept for preset compatibility, no part in this circuit: ${legacy.map((k) => esc(k.label)).join(", ")}.</p>` : "")) : "";
    // device models
    const models = Object.entries(c.models || {});
    const devs = models.length ? sec("Device models", "the equations' parameters", models.map(([name, m]) => {
      const users = c.parts.filter((p) => p.model === name).map((p) => p.ref);
      const params = Object.entries(m.params || {}).filter(([k, v]) => typeof v === "number" && !["level", "be"].includes(k));
      return `<div class="cd-dt-dev"><div class="h"><b>${esc(name)}</b><span>${esc(m.kind || "")}</span><span class="sp"></span><div class="refs">${users.map(refBtn).join("")}</div></div>
        ${m.note ? `<div class="n">${esc(m.note)}</div>` : ""}${params.length ? `<div class="cd-dt-kv">${params.map(([k, v]) => `<span title="${esc(k)} = ${esc(v)}"><b>${esc(k)}</b>${+Number(v).toPrecision(4)}</span>`).join("")}</div>` : ""}</div>`;
    }).join("")) : "";
    // how it is solved
    const solve = nGroups ? sec("How the engine solves it", `${nGroups} exact partition${nGroups === 1 ? "" : "s"}${model.internal_rate ? ` at ${Math.round(model.internal_rate / 1000)} kHz` : ""}`,
      `<p class="mute">The netlist is split into partitions that share no state; each is solved exactly every sample (nodal equations, Newton iterations for the nonlinear devices). The colours match the schematic's legend.</p>` +
      (c.groups || []).map((g, i) => `<div class="cd-dt-part"><div class="h"><i style="background:${GROUP_COLORS[i % GROUP_COLORS.length]}"></i><b>Partition ${i + 1}</b><span>${esc(g.solver)} · ${g.unknowns} unknowns · ${g.states} states · ${g.nonlinear_ports} nonlinear port${g.nonlinear_ports === 1 ? "" : "s"}</span></div><div class="refs">${g.parts.map(refBtn).join("")}</div></div>`).join("")) : "";
    const srcs = (c.sources || []).length ? sec("Sources", "where the values come from", `<ol class="cd-dt-src">${c.sources.map((x) => `<li>${x.url ? `<a href="${esc(x.url)}" target="_blank" rel="noopener">${esc(x.title)}</a>` : esc(x.title)}</li>`).join("")}</ol>`) : "";
    const notes = [model.error, ...(model.import_warnings || []), ...(c.warnings || [])].filter(Boolean);
    const nts = notes.length ? sec("Solver notes", "simplifications the engine reports", `<ul class="cd-dt-notes">${notes.map((w) => `<li>${esc(w)}</li>`).join("")}</ul>`) : "";
    return `<div class="cd-dt"><div class="col">${about}${bom}</div><div class="col">${controls}${solve}${devs}${srcs}${nts}</div></div>`;
  }

  // Every field the inspector used to show, with the same data-cv-* edit controls.
  function partRows(p) {
    const c = V.model.circuit, ctx = V.ctx, rows = [];
    const ctlOf = (id) => c.controls.find((x) => x.id === id);
    const row = (k, v, n, ed) => rows.push(`<div class="cv-row"><div class="k">${esc(k)}</div><div class="v">${v}</div>${n ? `<div class="n">${esc(n)}</div>` : ""}${ed || ""}</div>`);
    row("Type", esc(typeName(p)), p.label);
    const editing = ctx.edit && V.baseNet;
    if (editing) {
      const dang = danglingNets(c), nets = [...allNets(c)].sort((a, b) => a.localeCompare(b, undefined, { numeric: true }));
      row("Pins", `<div class="cv-pins">${p.nodes.map((n, i) => `<label for="cv-pin-${i}">${esc(pinName(p, i))}</label><input id="cv-pin-${i}" type="text" list="cv-netlist" value="${esc(n)}" data-cv-pin="${i}"${dang.has(n) ? ` class="nc" title="Not connected"` : ""} spellcheck="false" autocomplete="off" aria-label="${esc(p.ref)} ${esc(pinName(p, i))} net"><em data-cv-dc="${esc(n)}"></em>`).join("")}</div>
        <datalist id="cv-netlist">${nets.map((n) => `<option value="${esc(n)}">`).join("")}</datalist>`, "Type the net a pin connects to (0 is ground); a new name makes a new net. Or drag the pin on the schematic.");
    } else row("Nodes", `<div class="cv-nodes">${p.nodes.map((n) => `<span><code>${esc(n)}</code><em data-cv-dc="${esc(n)}"></em></span>`).join("")}</div>`);
    if (p.value != null && p.type !== "V") {
      const path = p.edit && p.edit.path && /\.value$/.test(p.edit.path) ? p.edit.path : null;
      const ovv = path && ctx.getPath ? ctx.getPath(path) : undefined;
      let ed = "";
      if (ctx.edit && path) {
        const e = p.edit, cur = ovv !== undefined ? +ovv : p.nominal;
        const t = Math.log(cur / e.min) / Math.log(e.max / e.min);
        ed = `<div class="cv-ed"><input type="range" min="0" max="1000" value="${Math.round(Math.max(0, Math.min(1, t)) * 1000)}" data-cv-path="${esc(path)}" data-min="${e.min}" data-max="${e.max}" aria-label="${esc(p.ref)} value">
          <input type="text" value="${esc(fmtEng(cur, p.unit).replace(" ", ""))}" data-cv-num="${esc(path)}" aria-label="${esc(p.ref)} value">${ovv !== undefined ? `<button class="cd-btn ghost" data-cv-reset="${esc(path)}">Reset</button>` : ""}</div>`;
      }
      row(p.type === "pot" ? "Track" : "Value", `${esc(p.text)}${p.law ? ` <span style="color:var(--text-3)">(${esc(p.law.law)} law on ${esc(p.law.control)}, ${fmtEng(p.law.min, p.unit)} … ${fmtEng(p.law.max, p.unit)})</span>` : ""}`,
        p.tolerance ? `±${Math.round(p.tolerance * 100)} % tolerance` : "", ed);
    } else if (p.type === "V") {
      row("Voltage", esc(p.text || `${p.value} V`));
    }
    if (p.type === "pot") {
      const k = ctlOf(p.control);
      const pos = p.position || 0;
      const ed = k && ctx.setControl ? `<div class="cv-ed"><input type="range" min="0" max="1000" value="${Math.round(pos * 1000)}" data-cv-knob="${esc(k.param)}" aria-label="${esc(k.label)}"></div>` : "";
      row("Wiper", `<div class="cv-wiper"><span>${Math.round(pos * 100)} % · taper ${esc(typeof p.taper === "object" ? p.taper.type : p.taper)}</span><span class="bar"><i style="width:${(pos * 100).toFixed(1)}%"></i></span></div>`,
        `pin1–wiper ${fmtEng(p.segments[0], "Ω")}, wiper–pin3 ${fmtEng(p.segments[1], "Ω")} · turns the unit's ${k ? k.label : p.control} knob`, ed);
    }
    if (p.type === "switch") {
      const k = ctlOf(p.control);
      row("State", `${p.closed ? "closed" : "open"} ${k && ctx.setControl ? `<button class="cd-btn ghost" data-cv-toggle="${esc(k.param)}" data-cv-val="${p.closed ? 0 : 1}">${p.closed ? "Open" : "Close"}</button>` : ""}`);
    }
    const added = !!(V.topo && V.topo.add && own(V.topo.add, p.ref));
    if (editing && added && p.type === "pot") {
      const ks = c.controls.filter((k) => k.kind === "continuous" && !k.legacyOnly);
      row("Control", "", "Which of the unit's knobs turns it (fixed at half-way when none).", `<div class="cv-ed"><select data-cv-addctl="1" aria-label="${esc(p.ref)} control"><option value="">Fixed (50 %)</option>${ks.map((k) => `<option value="${esc(k.id)}"${k.id === p.control ? " selected" : ""}>${esc(k.label)}</option>`).join("")}</select></div>`);
    } else if (p.control) {
      const k = ctlOf(p.control);
      row("Control", esc(k ? k.label : p.control), k ? `${k.note || ""}${k.param && k.param !== k.id ? ` · preset parameter ${k.param}` : ""}` : "");
    }
    if (p.model) {
      const m = c.models[p.model] || {};
      const path = p.edit && /\.model$/.test(p.edit.path) ? p.edit.path : null;
      let ed = "";
      if (ctx.edit && path) ed = `<div class="cv-ed"><select data-cv-model="${esc(path)}" aria-label="${esc(p.ref)} device model">${p.edit.options.map((o) => `<option value="${esc(o.v)}"${o.v === p.model ? " selected" : ""}>${esc(o.label)}</option>`).join("")}</select></div>`;
      row("Device model", `<b>${esc(p.model)}</b> <span style="color:var(--text-3)">${esc(m.kind || "")}</span>`, m.note || "", ed);
      if (m.params) row("Parameters", `<div class="cv-kvs">${Object.entries(m.params).filter(([k]) => !["level", "be"].includes(k)).map(([k, v]) => `<span title="${esc(k)} = ${esc(v)}"><b>${esc(k)}</b>${+Number(v).toPrecision(4)}</span>`).join("")}</div>`);
    }
    const gi = (c.groups || []).findIndex((g) => g.parts.includes(p.ref));
    if (gi >= 0) row("Solved in", `<span style="color:${GROUP_COLORS[gi % GROUP_COLORS.length]}">●</span> partition ${gi + 1}: ${esc(c.groups[gi].solver)}`);
    if (p.note) row("Note", "", p.note);
    if (editing) rows.push(`<div class="cv-dr-actions"><button class="cd-btn ghost danger" data-cv-del="${esc(p.ref)}" title="Delete this part (Delete)">${added ? "Delete part" : "Remove from the circuit"}</button></div>`);
    return rows.join("");
  }

  /* ---------------- part drawer ---------------- */
  function loadParts(cb) {
    if (window.CircuitParts) { if (cb) cb(); return; }
    if (!loadParts.q) {
      loadParts.q = [];
      const s = document.createElement("script");
      s.src = BASE + "circuit-parts.js";
      s.onload = () => { const q = loadParts.q || []; loadParts.q = null; q.forEach((f) => f()); };
      s.onerror = () => { loadParts.q = null; };
      document.head.appendChild(s);
    }
    if (cb && loadParts.q) loadParts.q.push(cb);
  }
  // Heaters glow while the unit is on: in the rack and not bypassed (a unit only on the Components page counts as on).
  function unitOn() {
    try {
      // eslint-disable-next-line no-undef
      const rs = typeof rackState !== "undefined" ? rackState : null;
      const tid = V.model.table_id != null ? V.model.table_id : V.model.model_id;
      const slots = ((rs && rs.slots) || []).filter((s) => s.table_id === tid);
      return !slots.length || slots.some((s) => !s.bypassed);
    } catch (e) { return true; }
  }
  const ICO_PLUS = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"><path d="M8 3.5v9M3.5 8h9"/></svg>`;
  const ICO_MINUS = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"><path d="M3.5 8h9"/></svg>`;
  const ICO_FIT = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M2.5 6V3.5a1 1 0 0 1 1-1H6M10 2.5h2.5a1 1 0 0 1 1 1V6M13.5 10v2.5a1 1 0 0 1-1 1H10M6 13.5H3.5a1 1 0 0 1-1-1V10"/></svg>`;
  const ICO_BOLT = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M9 1.8 3.8 9h4l-1 5.2L12.2 7h-4z"/></svg>`;
  const ICO_X = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"><path d="M4 4l8 8M12 4l-8 8"/></svg>`;
  const ICO_UP = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M4 10l4-4 4 4"/></svg>`;
  const ICO_DN = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M4 6l4 4 4-4"/></svg>`;
  function ensureDrawer() {
    if (V.drawer && V.drawer.isConnected) return V.drawer;
    const page = (V.host && V.host.closest(".cd-page")) || document.body;
    const d = document.createElement("aside");
    d.className = "cv-drawer";
    d.setAttribute("role", "dialog");
    d.setAttribute("aria-label", "Circuit part");
    d.tabIndex = -1;
    d.innerHTML = `<div class="cv-dr-head"><div class="crumb"></div>
        <button class="cv-dr-nav" data-cv-step="-1" title="Previous part">${ICO_UP}</button><button class="cv-dr-nav" data-cv-step="1" title="Next part">${ICO_DN}</button>
        <button class="cv-dr-x" data-cv-close="1" title="Close (Esc)" aria-label="Close">${ICO_X}</button></div>
      <div class="cv-dr-scroll"><div class="cv-dr-hero"></div><div class="cv-dr-title"></div><div class="cv-dr-bands"></div><div class="cv-dr-body"></div></div>`;
    page.appendChild(d);
    const bar = page.querySelector && page.querySelector(".cd-bar");
    if (bar && page !== document.body) d.style.setProperty("--cv-dr-top", `${bar.offsetHeight}px`);
    else d.style.setProperty("--cv-dr-top", "0px");
    d.addEventListener("click", onDrawerClick);
    d.addEventListener("change", onChange);
    d.addEventListener("input", onInput);
    d.addEventListener("pointerdown", () => {
      V.drawerBusy = true;
      const up = () => { window.removeEventListener("pointerup", up, true); V.drawerBusy = false; if (V.drawerPending) { V.drawerPending = false; renderDrawer(); } };
      window.addEventListener("pointerup", up, true);
    });
    V.drawer = d;
    V.heroKey = V.bodyKey = "";
    return d;
  }
  function closeDrawer() {
    if (!V.sel) return;
    V.sel = null;
    if (V.host) {
      V.host.querySelectorAll(".cv-schem .part.sel").forEach((g) => g.classList.remove("sel"));
      const w = V.host.querySelector(".cv-wrap");
      if (w) w.classList.remove("cv-dr-open");
    }
    if (V.drawer) {
      V.drawer.classList.remove("open");
      const a = document.activeElement;
      if (a && V.drawer.contains(a)) a.blur();
    }
    paintEditor();
  }
  function selectPart(ref) {
    const was = V.sel;
    V.sel = ref;
    V.host.querySelectorAll(".cv-schem .part").forEach((g) => g.classList.toggle("sel", g.dataset.part === ref));
    const w = V.host.querySelector(".cv-wrap");
    if (w) w.classList.add("cv-dr-open");
    renderDrawer(was !== ref);
    paintEditor();
    // keep the selected part on screen, left of the drawer (its resting position: it may still be sliding in)
    revealPart(ref, true);
  }
  function stepPart(dir) {
    const list = (V.model.circuit.parts || []).map((p) => p.ref);
    const i = list.indexOf(V.sel);
    if (i < 0 || !list.length) return;
    selectPart(list[(i + dir + list.length) % list.length]);
  }
  function renderDrawer(swap) {
    if (!V.sel) { if (V.drawer) V.drawer.classList.remove("open"); return; }
    const p = partByRef(V.sel);
    if (!p) { closeDrawer(); return; }
    const d = ensureDrawer();
    const c = V.model.circuit, dc = (V.curves && V.curves.dc) || {};
    // hero art (re-drawn only when something it shows changed)
    const hero = d.querySelector(".cv-dr-hero");
    let art = null;
    if (window.CircuitParts) {
      art = window.CircuitParts.render(p, { circuit: c, dc, on: unitOn() });
      const key = `${p.ref}|${art.svg.replace(/cp[0-9a-z]+-/g, "")}`;
      if (key !== V.heroKey) {
        V.heroKey = key;
        hero.innerHTML = `${art.svg}<div class="cv-dr-chips">${art.chips.map((t) => `<span>${esc(t)}</span>`).join("")}</div>`;
        if (swap) { hero.classList.remove("swap"); void hero.offsetWidth; hero.classList.add("swap"); }
      }
    } else {
      if (V.heroKey !== "loading") { V.heroKey = "loading"; hero.innerHTML = `<div class="ph">Drawing the part…</div>`; }
      loadParts(() => { V.heroKey = ""; renderDrawer(true); });
    }
    const list = c.parts || [];
    const idx = list.indexOf(p);
    d.querySelector(".crumb").innerHTML = `${esc(typeName(p))} <b>· ${idx + 1} / ${list.length}</b>`;
    // title + fields (deferred while a control in the drawer is being dragged)
    const gi = (c.groups || []).findIndex((g) => g.parts.includes(p.ref));
    const ov = V.ctx.overridePaths ? V.ctx.overridePaths() : new Set();
    const T = V.topo || {};
    const badge = T.add && own(T.add, p.ref) ? "added" : [...ov].some((x) => x.startsWith(`circuit.parts.${p.ref}.`)) || (T.wire && own(T.wire, p.ref)) ? "edited" : "";
    const title = `<div class="ref" ${gi >= 0 ? `style="--gc:${GROUP_COLORS[gi % GROUP_COLORS.length]}"` : ""}><span class="gdot" title="${gi >= 0 ? `Partition ${gi + 1}` : "Rail"}"></span>${esc(p.ref)}${badge ? `<span class="mod">${badge}</span>` : ""}</div>
      <div class="nm">${esc(art ? art.name : typeName(p))}</div>${p.label ? `<div class="lbl">${esc(p.label)}</div>` : ""}${art && art.pkg ? `<div class="pkg">${esc(art.pkg)}</div>` : ""}`;
    const bands = art && art.bands ? art.bands.map((b) => `<span><i style="--b:${b.color}"></i>${esc(b.name)}</span>`).join("") : "";
    const body = partRows(p);
    const key = `${p.ref}|${title}|${bands}|${body}|${V.ctx.edit ? 1 : 0}`;
    if (key !== V.bodyKey) {
      if (V.drawerBusy) V.drawerPending = true;
      else {
        V.bodyKey = key;
        d.querySelector(".cv-dr-title").innerHTML = title;
        d.querySelector(".cv-dr-bands").innerHTML = bands ? `<div class="cv-bands" title="Colour code">${bands}</div>` : "";
        d.querySelector(".cv-dr-body").innerHTML = body;
      }
    }
    // live operating point on the node chips
    d.querySelectorAll("[data-cv-dc]").forEach((e) => { const n = e.dataset.cvDc, v = isGnd(n) ? 0 : dc[n]; e.textContent = v != null ? fmtV(v) : ""; });
    if (!d.classList.contains("open")) {
      d.classList.add("open");
      try { d.focus({ preventScroll: true }); } catch (e) { /* old WebKit */ }
    }
    if (swap) d.querySelector(".cv-dr-scroll").scrollTop = 0;
  }
  function onDrawerClick(e) {
    if (e.target.closest("[data-cv-close]")) { closeDrawer(); return; }
    const st = e.target.closest("[data-cv-step]");
    if (st) { stepPart(+st.dataset.cvStep); return; }
    const tg = e.target.closest("[data-cv-toggle]");
    if (tg) { V.ctx.setControl(tg.dataset.cvToggle, +tg.dataset.cvVal); return; }
    const r = e.target.closest("[data-cv-reset]");
    if (r) { V.ctx.onReset(r.dataset.cvReset); return; }
    const dl = e.target.closest("[data-cv-del]");
    if (dl) opRemove(dl.dataset.cvDel);
  }
  // Esc closes the drawer before the page's own Esc (back to the chain) sees it.
  window.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && V.host && V.host.querySelector(".cv-netpop")) { e.preventDefault(); e.stopImmediatePropagation(); closeNetPop(); return; }
    if (e.key !== "Escape" || !V.sel || !V.drawer || !V.drawer.isConnected || !V.drawer.offsetParent) return;
    const menu = document.getElementById("menu");
    // eslint-disable-next-line no-undef
    if ((menu && !menu.hidden) || (typeof ui !== "undefined" && ui && ui.modalOpen)) return;
    const a = document.activeElement;
    e.preventDefault();
    e.stopImmediatePropagation();
    if (a && V.drawer.contains(a) && /^(INPUT|SELECT|TEXTAREA)$/.test(a.tagName)) { a.blur(); V.drawer.focus({ preventScroll: true }); return; }
    closeDrawer();
  }, true);

  function renderScope() {
    const box = V.host && V.host.querySelector("#cv-scope");
    if (!box) return;
    const pr = V.curves && V.curves.probes;
    const c = V.model.circuit;
    if (!pr || !pr.traces) { box.innerHTML = `<h3>Probes</h3><div class="cv-empty">Computing node waveforms…</div>`; return; }
    const nets = [c.io.input, ...V.probes.filter((n) => n !== c.io.input)];
    const t = pr.t_ms;
    const rows = nets.map((n, i) => {
      const y = pr.traces[n];
      if (!y) return "";
      const mn = Math.min(...y), mx = Math.max(...y), mean = y.reduce((a, b) => a + b, 0) / y.length;
      const span = Math.max(mx - mn, 1e-9);
      const color = i === 0 ? "var(--text-3)" : TRACE_COLORS[(i - 1) % TRACE_COLORS.length];
      const W = 400, H = 54;
      const d = y.map((v, k) => `${k ? "L" : "M"}${(k / (y.length - 1) * W).toFixed(1)} ${(H - 6 - (v - mn) / span * (H - 12)).toFixed(1)}`).join(" ");
      return `<div class="cv-trace"><div><div class="nm" style="color:${color}">${esc(n)}</div><div class="st">${fmtV(mx - mn)} p-p</div><div class="st">DC ${fmtV(mean)}</div></div>
        <svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"><path class="grid" d="M0 ${H / 2} H${W}"/><path class="ln" d="${d}" style="stroke:${color}"/></svg></div>`;
    }).join("");
    box.innerHTML = `<h3>Probes <span>${fmtV(pr.level_v)} sine at ${pr.freq_hz} Hz into the input, steady state, ${t.length ? t[t.length - 1].toFixed(1) : 0} ms shown · click any net to add or remove it</span></h3>${rows}`;
  }

  // Switch the drawing style: the camera keeps the same world point in the middle at the same on-screen size.
  function setView(v) {
    v = v === "illus" ? "illus" : "schematic";
    if (v === V.view) return;
    try { localStorage.setItem("headroom.circuit.view", v); } catch (e) { /* private mode */ }
    const was = V.cam && CAM.cw ? { cx: V.cam.x + CAM.cw / V.cam.s / 2, cy: V.cam.y + CAM.ch / V.cam.s / 2, s: V.cam.s / ZS() } : null;
    V.view = v;
    render(V.host, V.srcModel, V.ctx);
    if (was && V.geo && !V.cams[`${camKey(V.geo)}~seen`]) {
      V.cams[`${camKey(V.geo)}~seen`] = 1;
      const s = was.s * ZS();
      camSet({ s, x: was.cx - CAM.cw / s / 2, y: was.cy - CAM.ch / s / 2 }, false);
    }
  }
  function setSupply(on) {
    V.supply = !!on;
    try { localStorage.setItem("headroom.circuit.supply", on ? "1" : "0"); } catch (e) { /* private mode */ }
    render(V.host, V.srcModel, V.ctx);
  }
  // A supply rail flag: bring its node in the power supply section on screen and flash it.
  function showSupply(n) {
    if (!V.supply) setSupply(true);
    const b = V.geo && V.geo.nets.find((q) => q.sec === "supply" && q.n === n);
    if (!b || !V.cam) return;
    const s = Math.max(V.cam.s, READABLE * ZS());
    camSet({ s, x: b.x - CAM.cw / s / 2, y: (b.top + b.y1) / 2 - CAM.ch / s / 2 }, true);
    const el = canvasEl(), g = el && el.querySelector(`.net[data-sec="supply"][data-net="${window.CSS && window.CSS.escape ? window.CSS.escape(n) : n}"]`);
    if (g) { g.classList.remove("cv-flash"); void g.getBoundingClientRect(); g.classList.add("cv-flash"); g.addEventListener("animationend", () => g.classList.remove("cv-flash"), { once: true }); }
  }
  function onClick(e) {
    // a drag (pan or pot turn) ends in a click on whatever is under the pointer: swallow that one
    if (V.dragMoved) { V.dragMoved = false; if (performance.now() - V.dragEnd < 400) return; }
    const zb = e.target.closest("[data-cv-zoom]");
    if (zb) {
      const z = zb.dataset.cvZoom;
      if (z === "fit") camFitNow(true); else camZoomAt(CAM.cw / 2, CAM.ch / 2, z === "in" ? 1.4 : 1 / 1.4, true);
      return;
    }
    if (e.target.closest(".cv-zoom")) return;
    const vb = e.target.closest("[data-cv-view]");
    if (vb) { setView(vb.dataset.cvView); return; }
    if (e.target.closest("[data-cv-supply]")) { setSupply(!V.supply); return; }
    if (e.target.closest(".cv-views")) return;
    const sl = e.target.closest(".supply-link");
    if (sl) { showSupply(sl.dataset.supply); return; }
    if (e.target.closest("[data-cv-undo]")) { undo(); return; }
    if (e.target.closest("[data-cv-redo]")) { redo(); return; }
    if (e.target.closest("[data-cv-delsel]")) { if (V.sel) opRemove(V.sel); return; }
    if (e.target.closest(".cv-tools")) return;
    const rst = e.target.closest("[data-cv-restore]");
    if (rst) { opRestore(rst.dataset.cvRestore); return; }
    const gt = e.target.closest("[data-cv-goto]");
    if (gt) { if (partByRef(gt.dataset.cvGoto)) selectPart(gt.dataset.cvGoto); return; }
    const pop = e.target.closest(".cv-netpop");
    if (pop) {
      if (e.target.closest("[data-cv-rename]")) { const inp = pop.querySelector("input"); if (!opRename(pop.dataset.net, inp.value)) closeNetPop(); }
      else if (e.target.closest("[data-cv-probe]")) { closeNetPop(); toggleProbe(pop.dataset.net); }
      return;
    }
    closeNetPop();
    if (V.ctx.edit && V.baseNet && e.target.closest(".net-lbl-hit, .net-lbl, .net-dc")) { openNetPop(e.target.closest(".net").dataset.net, e); return; }
    const imp = e.target.closest("[data-cv-import]");
    if (imp) { const f = V.host.querySelector("[data-cv-file]"); if (f) f.click(); return; }
    const tg = e.target.closest("[data-cv-toggle]");
    if (tg) { V.ctx.setControl(tg.dataset.cvToggle, +tg.dataset.cvVal); return; }
    const r = e.target.closest("[data-cv-reset]");
    if (r) { V.ctx.onReset(r.dataset.cvReset); return; }
    const pg = e.target.closest(".part");
    if (pg) {
      if (V.sel === pg.dataset.part) closeDrawer();
      else selectPart(pg.dataset.part);
      return;
    }
    const ng = e.target.closest(".net");
    if (ng) { toggleProbe(ng.dataset.net); return; }
    // a click on empty schematic closes the drawer
    if (e.target.closest(".cv-schem")) closeDrawer();
  }
  function toggleProbe(n) {
    const i = V.probes.indexOf(n);
    if (i >= 0) V.probes.splice(i, 1);
    else { V.probes.push(n); if (V.probes.length > 5) V.probes.shift(); }
    render(V.host, V.srcModel, V.ctx);
  }
  function onInput(e) {
    const t = e.target;
    if (t.matches("[data-cv-knob]") && V.ctx.setControl) {
      const now = performance.now();
      clearTimeout(V.knobT);
      if (now - (V.knobLast || 0) > 40) { V.knobLast = now; V.ctx.setControl(t.dataset.cvKnob, +t.value / 1000); }
      else V.knobT = setTimeout(() => { V.knobLast = performance.now(); V.ctx.setControl(t.dataset.cvKnob, +t.value / 1000); }, 45);
      return;
    }
    if (t.matches("input[type=range][data-cv-path]")) {
      const mn = +t.dataset.min, mx = +t.dataset.max;
      const v = mn * Math.pow(mx / mn, +t.value / 1000);
      const num = t.parentElement.querySelector("[data-cv-num]");
      const p = partByRef(V.sel);
      if (num && p) num.value = fmtEng(v, p.unit).replace(" ", "");
    }
  }
  // Drag a pot's body vertically to turn it: the unit's knob (and the playing slot, when auditioning) follows.
  // Anywhere else on the canvas, a drag pans it (a click without movement still selects / scopes).
  function onPointerDown(e) {
    if (e.button !== 0 || e.target.closest(".cv-zoom, .cv-netpop, .cv-status, .cv-views")) return;
    V.dragMoved = false;
    if (V.ctx.edit && V.baseNet) {
      const add = e.target.closest(".cv-pal [data-add]");
      if (add) { startPalette(e, PALETTE[+add.dataset.add]); return; }
      if (e.target.closest(".cv-tools")) return;
      const pin = e.target.closest(".cv-pin"), g = pin && pin.closest("[data-part]");
      if (g) { startWire(e, g.dataset.part, +pin.dataset.pin); return; }
    }
    const g = e.target.closest(".part.pot-drag");
    const pot = g && V.ctx.setControl && partByRef(g.dataset.part);
    if (!pot || !V.model.circuit.controls.find((x) => x.id === pot.control)) { if (e.target.closest(".cv-canvas")) startPan(e); return; }
    const p = partByRef(g.dataset.part);
    const k = p && V.model.circuit.controls.find((x) => x.id === p.control);
    if (!k) return;
    e.preventDefault();
    const start = p.position != null ? p.position : 0.5, y0 = e.clientY;
    V.dragMoved = false;
    let last = 0, pending = null;
    const move = (ev) => {
      const pos = Math.max(0, Math.min(1, start + (y0 - ev.clientY) / 160));
      if (Math.abs(ev.clientY - y0) > 3) V.dragMoved = true;
      pending = pos;
      const now = performance.now();
      if (now - last > 40) { last = now; V.ctx.setControl(k.param, pending); pending = null; }
    };
    const up = () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      if (V.dragMoved) V.dragEnd = performance.now();
      if (pending != null) V.ctx.setControl(k.param, pending);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  }
  function parseEng(s) {
    const m = String(s).trim().replace(/[ΩFHV]$/i, "").match(/^([-+]?\d*\.?\d+(?:e[-+]?\d+)?)\s*([pnuµmkKMG]?)$/);
    if (!m) return NaN;
    const mult = { p: 1e-12, n: 1e-9, u: 1e-6, "µ": 1e-6, m: 1e-3, "": 1, k: 1e3, K: 1e3, M: 1e6, G: 1e9 }[m[2]];
    return parseFloat(m[1]) * mult;
  }
  function onChange(e) {
    const t = e.target;
    if (t.matches("[data-cv-file]") && t.files && t.files[0]) {
      const fr = new FileReader();
      fr.onload = () => V.ctx.onEdit("circuit.spice", String(fr.result || ""));
      fr.readAsText(t.files[0]);
      return;
    }
    if (t.matches("[data-cv-knob]")) { V.ctx.setControl(t.dataset.cvKnob, +t.value / 1000); return; }
    if (t.matches("[data-cv-pin]")) { if (V.sel) opWire(V.sel, +t.dataset.cvPin, t.value); return; }
    if (t.matches("[data-cv-addctl]")) { if (V.sel) opSetAdd(V.sel, "control", t.value); return; }
    if (t.matches("input[type=range][data-cv-path]")) {
      const mn = +t.dataset.min, mx = +t.dataset.max;
      editPath(t.dataset.cvPath, +(mn * Math.pow(mx / mn, +t.value / 1000)).toPrecision(4));
    } else if (t.matches("[data-cv-num]")) {
      const v = parseEng(t.value);
      if (isFinite(v) && v > 0) editPath(t.dataset.cvNum, v);
    } else if (t.matches("[data-cv-model]")) {
      editPath(t.dataset.cvModel, t.value);
    }
  }

  /* =====================================================================
     CIRCUIT EDITOR (edit mode): add, delete and wire parts (docs/CIRCUIT_ENGINE.md §6).
     The user's topology edits are a small diff over the unit's netlist, the same one the engine applies
     (CircuitUnit.cpp applyTopology): {rename: {old: new}, remove: [ref], wire: {ref: [nets]}, add: {ref: {part}}}.
     V.topo is the working draft. It is sent to the engine (ctx.onTopology) once every pin is connected; the engine
     compiles it, finds its DC operating point and smoke-renders it before swapping it in, so a rejected circuit
     leaves the previous one playing and the page shows why. Undo / redo step through the draft.
     ===================================================================== */
  const PIN_NAMES = {
    R: ["a", "b"], C: ["a", "b"], L: ["a", "b"], switch: ["a", "b"], V: ["+", "−"], pot: ["1", "wiper", "3"], D: ["anode", "cathode"], Dpair: ["anode", "cathode"],
    npn: ["c", "b", "e"], pnp: ["c", "b", "e"], njf: ["d", "g", "s"], pjf: ["d", "g", "s"], opamp: ["+", "−", "out", "V+", "V−"], triode: ["plate", "grid", "cathode"],
    pentode: ["plate", "screen", "grid", "cathode"], vcvs: ["out+", "out−", "ctrl+", "ctrl−"], vccs: ["out+", "out−", "ctrl+", "ctrl−"], ldr: ["a", "b", "lamp+", "lamp−"],
    spring: ["coil+", "coil−", "out", "gnd"], speaker: ["+", "−", "cone"], vca: ["in", "ec+", "ec−", "sym", "v−", "gnd", "v+", "out"], rms: ["in", "ibias", "gnd", "sym", "v−", "cap", "out", "v+"],
    dff: ["d", "clk", "set", "reset", "q", "qb", "vdd", "vss"], jkff: ["j", "k", "clk", "set", "reset", "q", "qb", "vdd", "vss"], nand: ["a", "b", "y", "vdd", "vss"], inv: ["a", "y", "vdd", "vss"],
  };
  function pinName(p, i) {
    if (p.type === "transformer") return `${i % 2 ? "b" : "a"}${Math.floor(i / 2) + 1}`;
    const n = PIN_NAMES[p.type];
    return (n && n[i]) || `pin ${i + 1}`;
  }
  const PALETTE = [
    { type: "R", label: "Resistor", value: "10k" }, { type: "C", label: "Capacitor", value: "100n" }, { type: "L", label: "Inductor", value: "100m" },
    { type: "pot", label: "Pot", value: "100k", taper: "B" }, { type: "D", label: "Diode", model: "1N914" }, { type: "npn", label: "NPN", model: "2N3904" },
    { type: "pnp", label: "PNP", model: "2N3906" }, { type: "njf", label: "JFET", model: "J201" }, { type: "opamp", label: "Op-amp", model: "TL072" },
    { type: "triode", label: "Triode", model: "12AX7" }, { type: "pentode", label: "Pentode", model: "EL84" }, { type: "V", label: "DC supply", value: 9 },
  ];
  const PAL_ICON = {
    R: `<path class="s" d="M0 10 H9 L11 5 L15 15 L19 5 L23 15 L27 5 L29 10 H40"/>`,
    C: `<path class="s" d="M0 10 H18 M18 3 V17 M22 3 V17 M22 10 H40"/>`,
    L: `<path class="s" d="M0 10 H7 a3.3 3.3 0 0 1 6.6 0 a3.3 3.3 0 0 1 6.6 0 a3.3 3.3 0 0 1 6.6 0 a3.3 3.3 0 0 1 6.6 0 H40"/>`,
    pot: `<path class="s" d="M0 10 H9 L11 5 L15 15 L19 5 L23 15 L27 5 L29 10 H40 M20 20 V13 M17 15 L20 12 L23 15"/>`,
    D: `<path class="s" d="M0 10 H14 M14 4 L25 10 L14 16 Z M25 4 V16 M25 10 H40"/>`,
    npn: `<circle class="s" cx="21" cy="10" r="9"/><path class="s" d="M8 10 H17 M17 4 V16 M17 7 L26 1 M17 13 L26 19"/>`,
    pnp: `<circle class="s" cx="21" cy="10" r="9"/><path class="s" d="M8 10 H17 M17 4 V16 M17 7 L26 1 M17 13 L26 19 M18 13 l3 3"/>`,
    njf: `<circle class="s" cx="21" cy="10" r="9"/><path class="s" d="M8 10 H18 M18 3 V17 M18 6 H26 V1 M18 14 H26 V19"/>`,
    opamp: `<path class="s" d="M12 1 L32 10 L12 19 Z M4 6 H12 M4 14 H12 M32 10 H40"/>`,
    triode: `<circle class="s" cx="20" cy="10" r="9.5"/><path class="s" d="M14 5 H26 M14 10 H16 M18 10 H20 M22 10 H24 M14 15 H26"/>`,
    pentode: `<circle class="s" cx="20" cy="10" r="9.5"/><path class="s" d="M14 4 H26 M14 8 H26 M14 11 H16 M18 11 H20 M22 11 H24 M14 15 H26"/>`,
    V: `<rect class="s" x="8" y="4" width="24" height="12" rx="3"/><path class="s" d="M14 10 H20 M17 7 V13 M23 10 H27"/>`,
  };
  const PREFIX = { R: "R", C: "C", L: "L", pot: "VR", D: "D", npn: "Q", pnp: "Q", njf: "J", pjf: "J", opamp: "IC", triode: "V", pentode: "V", V: "VS" };
  const UNIT_OF = { R: "Ω", pot: "Ω", C: "F", L: "H", V: "V" };
  const MODEL_KIND = { D: "diode", Dpair: "diode", npn: "npn", pnp: "pnp", njf: "njf", pjf: "pjf", opamp: "opamp", triode: "triode", pentode: "pentode", speaker: "speaker" };
  // DC: which pins conduct to each other (capacitors do not); driven pins hold their net (op-amp output and rails)
  const DC_SETS = { R: [[0, 1]], L: [[0, 1]], switch: [[0, 1]], D: [[0, 1]], Dpair: [[0, 1]], V: [[0, 1]], pot: [[0, 1, 2]], npn: [[0, 1, 2]], pnp: [[0, 1, 2]],
    njf: [[0, 1, 2]], pjf: [[0, 1, 2]], triode: [[0, 1, 2]], pentode: [[0, 1, 2, 3]], vcvs: [[0, 1]], ldr: [[0, 1], [2, 3]], spring: [[0, 1], [2, 3]], speaker: [[0, 1]], C: [], vccs: [] };
  const DRIVEN = { opamp: [2, 3, 4], vcvs: [0] };
  const SENSE = { opamp: [0, 1], vcvs: [2, 3], vccs: [2, 3] };
  const own = (o, k) => !!o && Object.prototype.hasOwnProperty.call(o, k);
  const sameList = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

  function cleanTopo(t) {
    const o = {};
    if (t && t.rename) { const r = Object.fromEntries(Object.entries(t.rename).filter(([k, v]) => k !== v)); if (Object.keys(r).length) o.rename = r; }
    if (t && t.remove && t.remove.length) o.remove = [...t.remove];
    if (t && t.wire && Object.keys(t.wire).length) o.wire = t.wire;
    if (t && t.add && Object.keys(t.add).length) o.add = t.add;
    return Object.keys(o).length ? JSON.parse(JSON.stringify(o)) : null;
  }
  const topoKey = (t) => JSON.stringify(cleanTopo(t));
  // The same steps as the engine: rename, remove, wire, add (the engine then validates the result).
  function applyTopo(net, t) {
    t = t || {};
    const rn = t.rename || {}, r = (n) => (own(rn, n) ? rn[n] : n);
    const gone = new Set(t.remove || []), wire = t.wire || {};
    const parts = [];
    (net.parts || []).forEach((p) => {
      if (gone.has(p.ref)) return;
      const w = wire[p.ref];
      parts.push({ ...p, nodes: w && w.length === p.nodes.length ? w.slice() : p.nodes.map(r) });
    });
    Object.entries(t.add || {}).forEach(([ref, a]) => { if (!parts.some((p) => p.ref === ref)) parts.push({ ...a, ref, nodes: (a.nodes || []).slice() }); });
    const io = { ...(net.io || {}) };
    ["input", "output", "outputR", "inputR"].forEach((k) => { if (io[k] != null) io[k] = r(io[k]); });
    const layout = JSON.parse(JSON.stringify(net.layout || {}));
    ["columns", "rows"].forEach((k) => { if (layout[k]) layout[k] = Object.fromEntries(Object.entries(layout[k]).map(([n, v]) => [r(n), v])); });
    return { ...net, parts, io, layout, probes: (net.probes || []).map(r) };
  }
  function railsOf(parts) { return new Set(parts.filter((p) => p.type === "V").map((p) => p.nodes[0])); }
  function danglingOf(parts, io) {
    const cnt = new Map(), rails = railsOf(parts), out = new Set();
    parts.forEach((p) => p.nodes.forEach((n) => cnt.set(n, (cnt.get(n) || 0) + 1)));
    cnt.forEach((k, n) => { if (k === 1 && !isGnd(n) && !rails.has(n) && ![io.input, io.output, io.inputR, io.outputR].includes(n)) out.add(n); });
    return out;
  }
  // Nets the draft leaves with one pin (the original circuit's own unconnected pins don't count).
  function danglingNets(c) {
    if (!V.ctx || !V.ctx.edit || !V.baseNet) return new Set();
    const base = V.baseDangle || new Set();
    return new Set([...danglingOf(c.parts, c.io)].filter((n) => !base.has(n)));
  }
  // Nets nothing drives (only op-amp / controlled-source inputs touch them), and nets with no DC path to ground.
  function netIssues(parts, io) {
    const pins = new Map(), rails = railsOf(parts);
    parts.forEach((p) => p.nodes.forEach((n, i) => { if (!pins.has(n)) pins.set(n, []); pins.get(n).push([p, i]); }));
    const sensed = new Set();
    pins.forEach((list, n) => {
      if (isGnd(n) || rails.has(n) || n === io.input || n === io.inputR) return;
      if (list.every(([p, i]) => (SENSE[p.type] || []).includes(i))) sensed.add(n);
    });
    const up = new Map(), find = (a) => { while (up.has(a) && up.get(a) !== a) a = up.get(a); return a; };
    const join = (a, b) => { const x = find(a), y = find(b); if (x !== y) up.set(x, y); };
    const ANCHOR = "\u0000ground";
    pins.forEach((list, n) => { if (isGnd(n) || rails.has(n) || n === io.input || n === io.inputR) join(n, ANCHOR); });
    parts.forEach((p) => {
      const sets = DC_SETS[p.type];
      if (p.type === "transformer") { for (let i = 0; i + 1 < p.nodes.length; i += 2) join(p.nodes[i], p.nodes[i + 1]); return; }
      if (!sets) { p.nodes.forEach((n) => join(n, ANCHOR)); return; } // other devices: assume they hold their pins
      sets.forEach((g) => g.forEach((i) => { if (p.nodes[i] != null) join(p.nodes[g[0]], p.nodes[i]); }));
      (DRIVEN[p.type] || []).forEach((i) => { if (p.nodes[i] != null) join(p.nodes[i], ANCHOR); });
    });
    const floating = new Set();
    pins.forEach((list, n) => { if (find(n) !== find(ANCHOR) && !sensed.has(n)) floating.add(n); });
    return { sensed, floating, pins };
  }
  function lintCircuit(c) {
    const block = [], warn = [];
    if (!V.ctx || !V.ctx.edit || !V.baseNet) return { block, warn };
    const dang = danglingNets(c), T = V.topo || {};
    c.parts.forEach((p) => p.nodes.forEach((n, i) => { if (dang.has(n)) block.push({ ref: p.ref, t: `${p.ref} ${pinName(p, i)} is not connected: drag it to a net, or type one in its drawer` }); }));
    c.parts.forEach((p) => { if (p.nodes.length === 2 && p.type !== "V" && p.nodes[0] === p.nodes[1] && (own(T.add, p.ref) || own(T.wire, p.ref))) warn.push({ ref: p.ref, t: `${p.ref} is shorted: both pins are on ${p.nodes[0]}` }); });
    const now = netIssues(c.parts, c.io), was = V.baseIssues || { sensed: new Set(), floating: new Set() };
    const who = (n) => (now.pins.get(n) || []).map(([p, i]) => `${p.ref} ${pinName(p, i)}`).slice(0, 3).join(", ");
    now.sensed.forEach((n) => { if (!was.sensed.has(n) && !dang.has(n)) warn.push({ net: n, t: `${n} is only sensed (${who(n)}): nothing drives it, so the engine will reject it` }); });
    now.floating.forEach((n) => { if (!was.floating.has(n) && !dang.has(n)) warn.push({ net: n, t: `${n} has no DC path to ground (only capacitors reach it): it may not settle` }); });
    return { block, warn };
  }

  // The circuit as the editor shows it: the engine's parts (values, positions, solve groups) re-wired by the draft,
  // with parts the engine has not seen yet drawn from their draft JSON.
  function synthPart(q, c) {
    const p = { ref: q.ref, type: q.type, nodes: q.nodes.slice(), draft: true };
    if (q.label) p.label = q.label;
    const u = UNIT_OF[q.type] || "";
    if (q.value != null) {
      const raw = typeof q.value === "object" ? q.value.nominal : q.value;
      const v = typeof raw === "number" ? raw : parseEng(raw);
      if (isFinite(v)) {
        Object.assign(p, { value: v, nominal: v, unit: u, text: q.type === "V" ? `${+v.toPrecision(4)} V` : fmtEng(v, u) });
        if (q.type !== "V") p.edit = { path: `circuit.parts.${q.ref}.value`, min: v / 10, max: v * 10, step: v / 100, scale: "log" };
      }
    }
    if (q.model) {
      const kind = MODEL_KIND[q.type], lib = ((c.model_library || {})[kind]) || [];
      Object.assign(p, { model: q.model, model_kind: kind, edit: { path: `circuit.parts.${q.ref}.model`, options: (lib.includes(q.model) ? lib : [q.model, ...lib]).map((v) => ({ v, label: v })) } });
    }
    if (q.type === "pot") {
      const k = c.controls.find((x) => x.id === q.control);
      p.position = k ? k.value : 0.5;
      p.taper = (q.taper && (q.taper.type || q.taper)) || "B";
      p.segments = [(p.value || 0) * p.position, (p.value || 0) * (1 - p.position)];
      if (q.control) p.control = q.control;
    }
    return p;
  }
  function displayCircuit(model) {
    const c = model.circuit, base = model.base_netlist || model.netlist;
    if (!base || !Array.isArray(base.parts)) { V.baseNet = null; return c; }
    V.baseNet = base;
    const T = V.topo || {}, A = V.appliedTopo || {};
    const renamed = applyTopo(base, { rename: T.rename || {} });
    V.baseDangle = danglingOf(renamed.parts, renamed.io);
    V.baseIssues = netIssues(renamed.parts, renamed.io);
    if (!cleanTopo(T) && !cleanTopo(A) && !Object.keys(V.hints).length) return c;
    const d = applyTopo(base, T);
    const rich = new Map(c.parts.map((p) => [p.ref, p]));
    const parts = d.parts.map((q) => {
      const r = rich.get(q.ref), a = T.add && T.add[q.ref];
      if (r && (!a || JSON.stringify(a) === JSON.stringify((A.add || {})[q.ref]))) return sameList(r.nodes, q.nodes) ? r : { ...r, nodes: q.nodes };
      return synthPart(q, c);
    });
    const layout = { ...(d.layout || {}) };
    layout.columns = { ...(layout.columns || {}) };
    Object.entries(V.hints).forEach(([n, k]) => { if (!own(layout.columns, n)) layout.columns[n] = k; });
    return { ...c, parts, io: { ...c.io, ...d.io }, layout, probes: d.probes };
  }
  function editorSync(model, ctx) {
    const c = model.circuit;
    const appliedKey = topoKey(ctx.getPath ? ctx.getPath("circuit.topology") : null);
    if (V.edUnit !== c.id || V.edEpoch !== ctx.editEpoch) {
      Object.assign(V, { edUnit: c.id, edEpoch: ctx.editEpoch, topo: JSON.parse(appliedKey) || {}, hist: [], fut: [], hints: {}, status: null, appliedKey });
    } else if (appliedKey !== V.appliedKey) {
      // the applied edits changed outside the editor (reset, a My Component): the draft follows
      const draftKey = topoKey(V.topo);
      V.appliedKey = appliedKey;
      if (draftKey !== appliedKey && !(V.status && V.status.kind === "busy")) Object.assign(V, { topo: JSON.parse(appliedKey) || {}, hist: [], fut: [], status: null });
    }
    V.appliedTopo = JSON.parse(appliedKey) || {};
  }

  /* ---------- edit operations (each is one undo step) ---------- */
  function edited(fn) {
    const before = topoKey(V.topo);
    if (!V.topo) V.topo = {};
    fn(V.topo);
    if (topoKey(V.topo) === before) return false;
    V.hist.push(before);
    if (V.hist.length > 200) V.hist.shift();
    V.fut = [];
    afterEdit();
    return true;
  }
  function afterEdit() {
    closeNetPop();
    render(V.host, V.srcModel, V.ctx);
    commit();
  }
  function undo() { if (!V.hist.length) return; V.fut.push(topoKey(V.topo)); V.topo = JSON.parse(V.hist.pop()) || {}; afterEdit(); }
  function redo() { if (!V.fut.length) return; V.hist.push(topoKey(V.topo)); V.topo = JSON.parse(V.fut.pop()) || {}; afterEdit(); }
  function allNets(c) { const s = new Set(); c.parts.forEach((p) => p.nodes.forEach((n) => s.add(n))); return s; }
  function freshNet(ref, i, p) {
    const used = allNets(V.model.circuit);
    const stem = `nc_${ref}_${String(pinName(p, i)).replace(/[^A-Za-z0-9]+/g, "") || i}`;
    let n = stem;
    for (let k = 2; used.has(n); k++) n = `${stem}${k}`;
    return n;
  }
  function newRef(type) {
    const pre = PREFIX[type] || "X", re = new RegExp(`^${pre}(\\d+)$`);
    let max = 0;
    [...(V.baseNet.parts || []).map((p) => p.ref), ...Object.keys((V.topo && V.topo.add) || {})].forEach((r) => { const m = re.exec(r); if (m) max = Math.max(max, +m[1]); });
    return `${pre}${max + 1}`;
  }
  function opAdd(tpl, wx, net) {
    const c = V.model.circuit, ref = newRef(tpl.type), T = V.topo;
    const p = { type: tpl.type };
    if (tpl.value != null) p.value = tpl.value;
    if (tpl.taper) p.taper = tpl.taper;
    if (tpl.model) { const lib = ((c.model_library || {})[MODEL_KIND[tpl.type]]) || []; p.model = !lib.length || lib.includes(tpl.model) ? tpl.model : lib[0]; }
    const rails = Object.values((V.geo && V.geo.lay.rails) || {});
    const top = rails.filter((r) => !isGnd(r.nodes[0]) && +r.value > 0).sort((a, b) => +b.value - +a.value)[0];
    const n = tpl.type === "opamp" && !top ? 3 : (PIN_NAMES[tpl.type] || ["1", "2"]).length;
    const proto = { type: tpl.type, nodes: [] };
    const nodes = [];
    const used = allNets(c);
    for (let i = 0; i < n; i++) {
      let nn;
      if (i === 0 && net && tpl.type !== "V") nn = net;
      else if (tpl.type === "V") nn = i === 1 ? "0" : (() => { let k = `v${String(tpl.value).replace(/\W/g, "")}`, s = k; for (let j = 2; used.has(s); j++) s = `${k}_${j}`; return s; })();
      else if (tpl.type === "opamp" && i === 3) nn = top.nodes[0];
      else if (tpl.type === "opamp" && i === 4) nn = "0";
      else { proto.nodes = nodes; nn = freshNet(ref, i, proto); }
      nodes.push(nn);
      used.add(nn);
    }
    p.nodes = nodes;
    // place it near where it was dropped: its new nets join the column of the nearest bus
    if (V.geo && V.geo.nets.length && wx != null) {
      const G = V.geo, raw = G.lay.rawCol;
      const near = G.nets.reduce((a, b) => (Math.abs(b.x - wx) < Math.abs(a.x - wx) ? b : a));
      const maxX = Math.max(...G.nets.map((b) => b.x));
      const col = wx > maxX + 40 ? Math.max(...Object.values(raw)) + 1 : raw[near.n];
      nodes.forEach((nn) => { if (/^nc_/.test(nn) && col != null) V.hints[nn] = col; });
    }
    const ok = edited((t) => { t.add = t.add || {}; t.add[ref] = p; });
    if (ok) requestAnimationFrame(() => { if (partByRef(ref)) selectPart(ref); });
    return ref;
  }
  function opRemove(ref) {
    if (V.sel === ref) closeDrawer();
    edited((t) => {
      if (t.add && own(t.add, ref)) { delete t.add[ref]; return; }
      if (t.wire) delete t.wire[ref];
      t.remove = [...new Set([...(t.remove || []), ref])];
    });
  }
  function opRestore(ref) { edited((t) => { t.remove = (t.remove || []).filter((r) => r !== ref); }); }
  function opWire(ref, i, net) {
    net = String(net || "").trim();
    if (!net) return;
    edited((t) => {
      if (t.add && own(t.add, ref)) { t.add[ref].nodes[i] = net; return; }
      const cur = partByRef(ref);
      if (!cur) return;
      const nodes = cur.nodes.slice();
      nodes[i] = net;
      const b = applyTopo(V.baseNet, { rename: t.rename || {} }).parts.find((p) => p.ref === ref);
      t.wire = t.wire || {};
      if (b && sameList(b.nodes, nodes)) delete t.wire[ref];
      else t.wire[ref] = nodes;
    });
  }
  function opRename(from, to) {
    to = String(to || "").trim();
    if (!to || to === from || isGnd(from) || isGnd(to)) return false;
    const baseNets = new Set();
    (V.baseNet.parts || []).forEach((p) => p.nodes.forEach((n) => baseNets.add(n)));
    const sub = (arr) => arr.map((n) => (n === from ? to : n));
    return edited((t) => {
      t.rename = t.rename || {};
      Object.keys(t.rename).forEach((k) => { if (t.rename[k] === from) t.rename[k] = to; });
      if (baseNets.has(from) && !own(t.rename, from)) t.rename[from] = to;
      Object.keys(t.wire || {}).forEach((r) => { t.wire[r] = sub(t.wire[r]); });
      Object.keys(t.add || {}).forEach((r) => { t.add[r].nodes = sub(t.add[r].nodes); });
      if (own(V.hints, from)) { V.hints[to] = V.hints[from]; delete V.hints[from]; }
      const pi = V.probes.indexOf(from);
      if (pi >= 0) V.probes[pi] = to;
    });
  }
  function opSetAdd(ref, key, value) {
    edited((t) => {
      if (!t.add || !own(t.add, ref)) return;
      if (value == null || value === "") delete t.add[ref][key];
      else t.add[ref][key] = value;
    });
  }
  // value / model edits: on an added part they change its draft entry (one undoable step); on any other part they
  // are the page's usual value overrides
  function editPath(path, value) {
    const m = /^circuit\.parts\.([^.]+)\.(value|model)$/.exec(path);
    if (m && V.topo && V.topo.add && own(V.topo.add, m[1])) { opSetAdd(m[1], m[2], value); return; }
    V.ctx.onEdit(path, value);
  }

  /* ---------- applying: through the engine's gate ---------- */
  function commit() {
    const t = cleanTopo(V.topo), key = JSON.stringify(t);
    V.lint = lintCircuit(V.model.circuit);
    if (V.lint.block.length) { V.cseq++; V.status = { kind: "wait" }; paintEditor(); return; }
    if (key === V.appliedKey) { V.cseq++; V.status = { kind: "ok" }; paintEditor(); return; }
    if (!V.ctx.onTopology) return;
    const seq = ++V.cseq;
    V.status = { kind: "busy" };
    paintEditor();
    V.ctx.onTopology(t).then((r) => {
      if (seq !== V.cseq || !r || r.stale) return;
      V.status = r.ok ? { kind: "ok" } : { kind: "bad", msg: r.error || "the engine rejected the circuit", key };
      paintEditor();
    });
  }

  /* ---------- edit chrome: tools (undo, redo, delete, palette) and the status line ---------- */
  const ICO_UNDO = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M6 4 3 7l3 3"/><path d="M3 7h6.5a3.5 3.5 0 0 1 0 7H7"/></svg>`;
  const ICO_REDO = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="m10 4 3 3-3 3"/><path d="M13 7H6.5a3.5 3.5 0 0 0 0 7H9"/></svg>`;
  const ICO_DEL = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.6 8.5h5.8l.6-8.5"/></svg>`;
  const MOD = /Mac|iPhone|iPad/.test(navigator.platform || "") ? "⌘" : "Ctrl+";
  function toolsHtml() {
    return `<div class="row"><button data-cv-undo="1" title="Undo (${MOD}Z)" aria-label="Undo"${V.hist.length ? "" : " disabled"}>${ICO_UNDO}</button>
      <button data-cv-redo="1" title="Redo (${MOD}⇧Z)" aria-label="Redo"${V.fut.length ? "" : " disabled"}>${ICO_REDO}</button>
      <button data-cv-delsel="1" title="Delete the selected part (Delete)" aria-label="Delete the selected part"${V.sel ? "" : " disabled"}>${ICO_DEL}</button></div>
      <div class="cap">Parts</div><div class="cv-pal">${PALETTE.map((t, i) => `<button data-add="${i}" title="Drag onto the schematic (onto a net to connect its first pin), or click to add">
        <svg viewBox="0 0 40 20">${PAL_ICON[t.type] || ""}</svg>${esc(t.label)}</button>`).join("")}</div>`;
  }
  function statusHtml() {
    const st = V.status || { kind: "ok" }, L = V.lint || { block: [], warn: [] }, t = cleanTopo(V.topo) || {};
    const nEd = Object.keys(t.add || {}).length + (t.remove || []).length + Object.keys(t.wire || {}).length + Object.keys(t.rename || {}).length;
    let chip;
    if (L.block.length) chip = `<span class="chip wait"><i></i>Not applied yet</span>`;
    else if (st.kind === "busy") chip = `<span class="chip busy"><i></i>Checking in the engine…</span>`;
    else if (st.kind === "bad") chip = `<span class="chip bad"><i></i>Rejected</span>`;
    else chip = `<span class="chip ok"><i></i>${nEd ? "Applied" : "Original circuit"}</span>`;
    const item = (cls, m) => `<div class="${cls}">${m.ref ? `<button data-cv-goto="${esc(m.ref)}">${esc(m.t)}</button>` : esc(m.t)}</div>`;
    const msgs = [];
    if (st.kind === "bad" && !L.block.length) msgs.push(`<div class="e">The engine rejected this circuit: ${esc(st.msg)}. The previous circuit keeps playing; fix it or undo (${MOD}Z).</div>`);
    L.block.slice(0, 3).forEach((m) => msgs.push(item("b", m)));
    if (L.block.length > 3) msgs.push(`<div class="b">… and ${L.block.length - 3} more unconnected pins</div>`);
    L.warn.slice(0, 3).forEach((m) => msgs.push(item("w", m)));
    const lint = (V.srcModel && V.srcModel.circuit && V.srcModel.circuit.lint) || [];
    if (topoKey(V.topo) === V.appliedKey) lint.slice(0, 3).forEach((m) => msgs.push(item("w", { t: m })));
    if (!msgs.length && st.kind === "busy") msgs.push(`<div>The engine is compiling the edited circuit, settling it and test-playing it; the previous circuit plays until it passes.</div>`);
    if (!msgs.length) msgs.push(`<div>${nEd ? `${nEd} circuit edit${nEd > 1 ? "s" : ""} playing. Every edit is compiled, settled and test-played by the engine before it is heard.` : "Drag a part from the palette onto a net, then drag each of its pins onto the net it connects to."}</div>`);
    const rm = (t.remove || []).length ? `<span class="rm">Removed:${t.remove.map((r) => `<button data-cv-restore="${esc(r)}" title="Put ${esc(r)} back">${esc(r)} ↺</button>`).join("")}</span>` : "";
    return `${chip}<div class="msgs">${msgs.join("")}</div>${rm}`;
  }
  function paintEditor() {
    if (!V.host) return;
    const st = V.host.querySelector(".cv-status"), tl = V.host.querySelector(".cv-tools");
    const on = !!(V.ctx && V.ctx.edit && V.baseNet);
    if (st) { st.hidden = !on; if (on) st.innerHTML = statusHtml(); }
    if (tl) {
      tl.hidden = !on;
      if (on) {
        if (!tl.firstChild) tl.innerHTML = toolsHtml();
        else {
          tl.querySelector("[data-cv-undo]").disabled = !V.hist.length;
          tl.querySelector("[data-cv-redo]").disabled = !V.fut.length;
          tl.querySelector("[data-cv-delsel]").disabled = !V.sel;
        }
      }
    }
  }

  /* ---------- pointer: wiring from a pin, parts from the palette ---------- */
  function worldAt(ev) {
    const r = viewRect();
    return { x: V.cam.x + (ev.clientX - r.left) / V.cam.s, y: V.cam.y + (ev.clientY - r.top) / V.cam.s, inside: ev.clientX >= r.left && ev.clientX <= r.right && ev.clientY >= r.top && ev.clientY <= r.bottom };
  }
  // what a wire (or a dropped part) would connect to under the pointer
  function dropTarget(ev) {
    const h = document.elementFromPoint(ev.clientX, ev.clientY);
    if (!h || !canvasEl() || !canvasEl().contains(h) || h.closest(".cv-tools, .cv-zoom, .cv-netpop")) return null;
    const pin = h.closest(".cv-pin");
    if (pin) { const g = pin.closest("[data-part]"), p = g && partByRef(g.dataset.part); if (p) return { el: pin, net: p.nodes[+pin.dataset.pin] }; }
    const rail = h.closest("[data-rail]");
    if (rail) return { el: rail, net: rail.dataset.rail };
    const net = h.closest(".net");
    if (net) return { el: net, net: net.dataset.net };
    return { el: null, net: null };
  }
  function markTarget(t) {
    V.host.querySelectorAll(".cv-target").forEach((x) => x.classList.remove("cv-target"));
    if (t && t.el) t.el.classList.add("cv-target");
  }
  function startWire(e, ref, i) {
    e.preventDefault();
    const el = canvasEl(), svg = el.querySelector("svg.cv-svg"), pl = V.geo.placed.find((q) => q.s.p.ref === ref);
    const pt = pl && pinPoints(pl).find((q) => q.i === i), p = partByRef(ref);
    if (!pt || !p || !V.cam) return;
    const line = document.createElementNS("http://www.w3.org/2000/svg", "path");
    line.setAttribute("class", "cv-rubber");
    svg.appendChild(line);
    const x0 = e.clientX, y0 = e.clientY;
    let moved = false, tgt = null;
    const move = (ev) => {
      if (!moved && Math.hypot(ev.clientX - x0, ev.clientY - y0) < 4) return;
      if (!moved) { moved = true; V.dragMoved = true; el.classList.add("wiring"); }
      const w = worldAt(ev);
      line.setAttribute("d", `M${pt.x} ${pt.y} L${w.x.toFixed(1)} ${w.y.toFixed(1)}`);
      tgt = dropTarget(ev);
      markTarget(tgt);
    };
    const up = (ev) => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      window.removeEventListener("pointercancel", up);
      line.remove();
      el.classList.remove("wiring");
      markTarget(null);
      if (!moved) return; // a click on a pin selects its part
      V.dragEnd = performance.now();
      if (ev.type === "pointercancel") return;
      const w = worldAt(ev);
      tgt = dropTarget(ev);
      if (!w.inside || !tgt) return;
      const cur = p.nodes[i];
      const net = tgt.net != null ? tgt.net : (danglingNets(V.model.circuit).has(cur) ? null : freshNet(ref, i, p)); // empty canvas: disconnect
      if (net != null && net !== cur) opWire(ref, i, net);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    window.addEventListener("pointercancel", up);
  }
  function startPalette(e, tpl) {
    e.preventDefault();
    const ghost = document.createElement("div");
    ghost.className = "cv-ghost off";
    ghost.textContent = `${tpl.label} · drop on the schematic`;
    const x0 = e.clientX, y0 = e.clientY;
    let moved = false;
    const move = (ev) => {
      if (!moved && Math.hypot(ev.clientX - x0, ev.clientY - y0) < 4) return;
      if (!moved) { moved = true; document.body.appendChild(ghost); }
      ghost.style.left = `${ev.clientX}px`;
      ghost.style.top = `${ev.clientY}px`;
      const t = dropTarget(ev);
      markTarget(t && t.el && !t.el.classList.contains("cv-pin") ? t : null);
      ghost.classList.toggle("off", !t);
      ghost.textContent = t ? (t.net ? `${tpl.label} → ${t.net}` : `${tpl.label} · unconnected`) : `${tpl.label} · drop on the schematic`;
    };
    const up = (ev) => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      window.removeEventListener("pointercancel", up);
      ghost.remove();
      markTarget(null);
      if (ev.type === "pointercancel") return;
      if (!moved) { // a click: next to the selected part, else mid-view
        const sel = V.sel && V.geo.placed.find((q) => q.s.p.ref === V.sel);
        opAdd(tpl, sel ? sel.box.x1 : V.cam.x + CAM.cw / V.cam.s / 2, null);
        return;
      }
      const t = dropTarget(ev);
      if (!t) return;
      opAdd(tpl, worldAt(ev).x, t.net);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    window.addEventListener("pointercancel", up);
  }

  /* ---------- net popover: rename (or merge), scope ---------- */
  function closeNetPop() { const p = V.host && V.host.querySelector(".cv-netpop"); if (p) p.remove(); }
  function openNetPop(n, ev) {
    closeNetPop();
    const el = canvasEl(), r = el.getBoundingClientRect();
    const pop = document.createElement("div");
    pop.className = "cv-netpop";
    pop.dataset.net = n;
    const k = (netIssues(V.model.circuit.parts, V.model.circuit.io).pins.get(n) || []).length;
    pop.innerHTML = `<div class="h">Net · ${k} pin${k === 1 ? "" : "s"}</div><input type="text" value="${esc(n)}" spellcheck="false" aria-label="Net name" data-cv-netname="1"${isGnd(n) ? " disabled" : ""}>
      <div class="n">${isGnd(n) ? "Ground can't be renamed." : "Rename it, or type another net's name to merge the two."}</div>
      <div class="b"><button class="cd-btn ghost" data-cv-probe="1">${V.probes.includes(n) ? "Unscope" : "Scope"}</button><button class="cd-btn primary" data-cv-rename="1"${isGnd(n) ? " disabled" : ""}>Rename</button></div>`;
    el.appendChild(pop);
    const x = Math.min(r.width - 242, Math.max(8, ev.clientX - r.left + 10)), y = Math.min(r.height - 130, Math.max(8, ev.clientY - r.top + 10));
    pop.style.left = `${x}px`;
    pop.style.top = `${y}px`;
    const inp = pop.querySelector("input");
    const note = pop.querySelector(".n");
    inp.addEventListener("input", () => {
      const v = inp.value.trim();
      note.textContent = !v ? "A net needs a name." : isGnd(v) ? "To ground a pin, wire it to 0." : v !== n && allNets(V.model.circuit).has(v) ? `Merges ${n} into the existing net ${v}.` : "Rename it, or type another net's name to merge the two.";
    });
    inp.addEventListener("keydown", (e2) => {
      if (e2.key === "Enter") { e2.preventDefault(); opRename(n, inp.value) || closeNetPop(); }
      else if (e2.key === "Escape") { e2.preventDefault(); e2.stopPropagation(); closeNetPop(); }
    });
    if (!isGnd(n)) { inp.focus(); inp.select(); }
  }

  // Delete, undo and redo from the keyboard while the schematic (or its drawer) is on screen in edit mode.
  window.addEventListener("keydown", (e) => {
    if (!V.ctx || !V.ctx.edit || !V.host || !V.host.isConnected || !V.host.offsetParent || !V.baseNet) return;
    const a = document.activeElement;
    if (a && /^(INPUT|SELECT|TEXTAREA)$/.test(a.tagName)) return;
    if (a && a !== document.body && !V.host.contains(a) && !(V.drawer && V.drawer.contains(a))) return;
    const k = e.key.toLowerCase(), mod = e.metaKey || e.ctrlKey;
    if (mod && k === "z") { e.preventDefault(); if (e.shiftKey) redo(); else undo(); }
    else if (mod && k === "y") { e.preventDefault(); redo(); }
    else if ((e.key === "Delete" || e.key === "Backspace") && V.sel && partByRef(V.sel)) { e.preventDefault(); opRemove(V.sel); }
  });

  window.CircuitView = {
    render,
    updateCurves(curves) {
      V.curves = curves || null;
      if (!V.host || !V.model) return;
      // DC labels live in the schematic: re-render it (cheap) when the operating point arrives.
      render(V.host, V.srcModel, V.ctx);
    },
    // the Details tab content for a circuit model (HTML)
    details: (model) => { try { return detailsHTML(model); } catch (e) { console.error(e); return ""; } },
    // open a part's drawer and bring it on screen (from the Details tab's parts list)
    select(ref) {
      if (!V.host || !V.model || !partByRef(ref)) return;
      if (!V.supply && V.geo && V.geo.supply && V.geo.supply.sup.has(ref)) setSupply(true);
      camMeasure(); selectPart(ref);
    },
    // the canvas became visible (tab switch): measure it, and fit on first show
    shown() { camMeasure(); },
    // tab hidden: stop any zoom animation (nothing else runs while idle)
    hidden() { camStop(); if (CAM.raf) { cancelAnimationFrame(CAM.raf); CAM.raf = 0; } },
    // test/inspection hook: the computed layout (parts, labels, wires) for overlap checks
    _geo: () => V.geo,
    _cam: () => V.cam && { ...V.cam, cw: CAM.cw, ch: CAM.ch },
    _busy: () => !!(CAM.raf || CAM.tween),
    // test/inspection hook: centre the camera on a world point at an on-screen scale (1 = 100 % in the zoom control)
    _look: (x, y, z) => { if (!V.cam) return; const s = (z || 1) * ZS(); camSet({ s, x: x - CAM.cw / s / 2, y: y - CAM.ch / s / 2 }, false); },
    _layout: (c) => placeAll(c),
    _view: () => ({ view: V.view, supply: V.supply, drawn: V.geo && V.geo.view, sections: V.geo && (V.geo.sections || []).map((x) => x.name) }),
    setView, setSupply,
  };
})();
