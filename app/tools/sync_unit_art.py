#!/usr/bin/env python3
"""Bundle the illustrated unit faces into the app UI.

Reads   work/illustrations/svg/<table_id>.svg + meta/<table_id>.json (+ components.json for panel order)
Writes  app/ui/assets/units/
          <tid>.<hash>.svg     full face, sanitised        -> Components pane thumbnails (<img>)
          <tid>.<hash>.bg.svg  face without moving parts    -> chain background (<img>, rasterised once)
          <tid>.<hash>.fg.js   moving parts only (knobs, switches, LED, footswitch, meter needles) plus the
                               defs they reference, ids namespaced with "__P__" so each instance can prefix them
          index.js / index.json  per-unit metadata (viewBox, kind, controls, file names)

Only finished pairs are taken: both files present, both parse, every control element exists. A pair that is
still being written fails to parse and is simply skipped, so this is safe to re-run at any time; files for
units that are no longer (or not yet) valid are removed. Usage:  python3 app/tools/sync_unit_art.py [--quiet] [--prune]
Superseded hashed files are kept for 3 days (a running app still references the index it loaded at startup);
--prune deletes every file the new index does not reference and lists them.
"""
import hashlib
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "work", "illustrations")
OUT = os.path.join(ROOT, "app", "ui", "assets", "units")
SVGNS = "http://www.w3.org/2000/svg"
XLINK = "http://www.w3.org/1999/xlink"
ET.register_namespace("", SVGNS)
ET.register_namespace("xlink", XLINK)
HREF_KEYS = ("href", "{%s}href" % XLINK)
FOOT_IDS = ("footswitch-bypass", "footswitch", "stomp-switch", "footswitch-pad", "ts9-footswitch-pad")
# Presentation attributes copied from a moving part's ancestors so it renders identically on its own layer.
PRES = {"fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity", "stroke-linecap", "stroke-linejoin",
        "stroke-dasharray", "stroke-miterlimit", "opacity", "filter", "clip-path", "mask", "font-family", "font-size",
        "font-weight", "font-style", "letter-spacing", "text-anchor", "dominant-baseline", "color", "style", "transform"}
QUIET = "--quiet" in sys.argv


def log(*a):
    if not QUIET:
        print(*a)


def local(tag):
    return tag.split("}", 1)[-1] if isinstance(tag, str) else ""


def sanitize(txt):
    # Bare '&' in text is fine for an HTML parser but makes the file invalid XML (an <img> would not render it).
    return re.sub(r"&(?!#\d+;|#x[0-9a-fA-F]+;|[A-Za-z][A-Za-z0-9]*;)", "&amp;", txt)


def refs_in(el):
    """ids referenced by el's own attributes (url(#x), href="#x")."""
    out = set()
    for k, v in el.attrib.items():
        if k in HREF_KEYS and v.startswith("#"):
            out.add(v[1:])
        for m in re.finditer(r"url\(\s*['\"]?#([^)'\"\s]+)", v):
            out.add(m.group(1))
    return out


def strip_ws(el):
    """Drop indentation-only text so the bundled files stay small (never touches <text> content)."""
    for e in el.iter():
        if local(e.tag) in ("text", "tspan", "textPath", "title", "desc"):
            continue
        if e.text is not None and not e.text.strip():
            e.text = None
        for c in e:
            if c.tail is not None and not c.tail.strip():
                c.tail = None


def drop_backdrop(root, vb):
    """Remove a full-canvas 'stage' backdrop (the first thing drawn, covering the whole viewBox): in the app the
    face sits on the app's own stage, so only the hardware (and its shadow) should be drawn."""
    for c in list(root):
        if local(c.tag) in ("defs", "title", "desc", "style", "metadata"):
            continue
        if local(c.tag) == "rect" and not c.get("transform"):
            try:
                x, y = float(c.get("x", 0) or 0), float(c.get("y", 0) or 0)
                w, h = float(c.get("width", "0").rstrip("%") or 0), float(c.get("height", "0").rstrip("%") or 0)
            except ValueError:
                return False
            if c.get("width", "").endswith("%"):
                w = vb[2] * w / 100
            if c.get("height", "").endswith("%"):
                h = vb[3] * h / 100
            if x <= vb[0] + 1 and y <= vb[1] + 1 and x + w >= vb[0] + vb[2] - 1 and y + h >= vb[1] + vb[3] - 1:
                root.remove(c)
                return True
        return False
    return False



# ---------- clean-up: baked floor shadows, feet/legs, oversized cast shadows ----------
# The illustrators drew each unit "on a floor": dark ovals under the chassis, feet/tilt legs, and big blurred
# drop-shadow filters tuned for a black page. In the app those read as smudges and stray legs, so they are
# removed here and the UI adds one shadow traced from each face's real outline (unit-art.css).
FLOOR_ID = re.compile(r"(^|[-_])(floor|feet|foot-?pads?|legs?|tilt-?legs?|stand-?legs?|casters?|ground-shadow|floor-shadow|cast-shadow)([-_]|$)", re.I)
def _is_dark(c):
    c = (c or "").strip().lower()
    if c in ("black",):
        return True
    m = re.match(r"^#([0-9a-f]{3}|[0-9a-f]{6})$", c)
    if not m:
        return False
    h = m.group(1)
    if len(h) == 3:
        h = "".join(ch * 2 for ch in h)
    r, g, b = (int(h[k:k + 2], 16) for k in (0, 2, 4))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b < 40


def _num(v, d=0.0):
    try:
        return float(str(v).strip().rstrip("px"))
    except (TypeError, ValueError):
        return d


def _dark_fill(e, byid):
    f = (e.get("fill") or "").strip()
    st = e.get("style") or ""
    m = re.search(r"fill\s*:\s*([^;]+)", st)
    if m:
        f = m.group(1).strip()
    if _is_dark(f):
        return True
    m = re.match(r"url\(\s*['\"]?#([^)'\"]+)", f)
    if m and m.group(1) in byid:   # gradient whose stops are all black (a soft floor shadow)
        stops = [x for x in byid[m.group(1)].iter() if local(x.tag) == "stop"]
        cols = [(x.get("stop-color") or re.search(r"stop-color\s*:\s*([^;]+)", x.get("style") or "") and re.search(r"stop-color\s*:\s*([^;]+)", x.get("style")).group(1) or "") for x in stops]
        return bool(cols) and all(_is_dark(c or "#000") for c in cols)
    return False


def deshadow(root, vb):
    parent = {c: p for p in root.iter() for c in p}
    byid = {e.get("id"): e for e in root.iter() if e.get("id")}

    def in_defs(e):
        a = parent.get(e)
        while a is not None:
            if local(a.tag) in ("defs", "symbol", "pattern", "clipPath", "mask", "marker"):
                return True
            a = parent.get(a)
        return False

    removed = 0
    # 1. floor / feet / legs groups by id
    for e in list(root.iter()):
        i = e.get("id") or ""
        if i and FLOOR_ID.search(i) and not in_defs(e) and not i.startswith(("knob-", "switch-", "led")):
            p = parent.get(e)
            if p is not None and e in list(p):
                p.remove(e)
                removed += 1
    parent = {c: p for p in root.iter() for c in p}
    # 2. wide dark ovals in the lower part of the canvas (floor shadows)
    for e in list(root.iter()):
        if local(e.tag) != "ellipse" or in_defs(e):
            continue
        rx, cy = _num(e.get("rx")), _num(e.get("cy"))
        if rx >= 0.22 * vb[2] and cy >= vb[1] + 0.68 * vb[3] and _dark_fill(e, byid):
            p = parent.get(e)
            if p is not None:
                p.remove(e)
                removed += 1
    # 3. big cast-shadow filters (blur >= 5 or offset >= 8) on drawn elements: drop the filter reference
    big = set()
    for f in root.iter():
        if local(f.tag) != "filter" or not f.get("id"):
            continue
        prims = [local(x.tag) for x in f.iter() if x is not f]
        sd = max([_num((x.get("stdDeviation") or "0").split()[0]) for x in f.iter() if x.get("stdDeviation")] or [0])
        dy = max([abs(_num(x.get("dy"))) for x in f.iter() if x.get("dy")] or [0])
        shadowy = "feDropShadow" in prims or ("feOffset" in prims and "feGaussianBlur" in prims)
        if shadowy and (sd >= 5 or dy >= 8):
            big.add(f.get("id"))
    if big:
        for e in root.iter():
            v = e.get("filter") or ""
            m = re.match(r"url\(\s*['\"]?#([^)'\"]+)", v)
            if m and m.group(1) in big and not in_defs(e):
                del e.attrib["filter"]
                removed += 1
    return removed


CROP_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "unit_art_crop.json")
try:
    CROPS = json.load(open(CROP_PATH, encoding="utf-8"))
except Exception:
    CROPS = {}

def strip_meta_text(root):
    """Drop <title>/<desc>/<metadata> from bundled faces: they're authoring notes (some name the products the
    drawings were researched from) and are never shown."""
    for p in list(root.iter()):
        for c in list(p):
            if local(c.tag) in ("desc", "title", "metadata"):
                p.remove(c)


def ser(el):
    tail, el.tail = el.tail, None
    try:
        return ET.tostring(el, encoding="unicode", short_empty_elements=True)
    finally:
        el.tail = tail


def process(tid, svg_path, meta, order):
    raw = open(svg_path, encoding="utf-8").read()
    root = ET.fromstring(sanitize(raw))
    if local(root.tag) != "svg":
        raise ValueError("root is not <svg>")
    vb = root.get("viewBox") or meta.get("viewBox") or "0 0 1200 560"
    vbn = [float(x) for x in re.split(r"[ ,]+", vb.strip())]
    deshadow(root, vbn)
    strip_meta_text(root)
    raw_key = hashlib.sha1(raw.encode("utf-8")).hexdigest()[:12]
    crop = CROPS.get(str(tid))
    full_vbn = list(vbn)
    if crop and crop.get("k") == raw_key:   # tight box around the hardware, measured by measure_unit_art.js
        vbn = [float(x) for x in crop["vb"]]
        vb = " ".join("%g" % x for x in vbn)
        root.set("viewBox", vb)
    parent = {c: p for p in root.iter() for c in p}

    def in_defs(e):
        a = parent.get(e)
        while a is not None:
            if local(a.tag) in ("defs", "symbol", "pattern", "clipPath", "mask", "marker"):
                return True
            a = parent.get(a)
        return False

    byid, rendered = {}, {}
    for e in root.iter():
        i = e.get("id")
        if not i:
            continue
        byid.setdefault(i, e)
        if i not in rendered and not in_defs(e) and local(e.tag) not in ("filter", "linearGradient", "radialGradient", "pattern", "clipPath", "mask", "symbol"):
            rendered[i] = e

    controls, problems = [], []
    dyn = []  # (element_id, element)
    disp_specs = [dict(d) for d in (meta.get("displays") or []) if isinstance(d, dict) and d.get("type")]
    disp_ids = {d.get("element_id") for d in disp_specs}
    for c in meta.get("controls", []):
        eid = c.get("element_id", "")
        if eid.split("-", 1)[0] == "meter":
            # Legacy needle drawn as a "control": now a live display (VU of this unit's level, or its gain reduction).
            if eid not in disp_ids:
                gr = bool(re.search(r"comp|reduc|\bgr\b", c.get("param_name", ""), re.I))
                disp_specs.append({"element_id": eid, "type": "needle", "source": "gr" if gr else "out.rms", "scale": "vu",
                                   "cx": c.get("cx"), "cy": c.get("cy"), "a0": c.get("min_angle", -42), "a1": c.get("max_angle", 42),
                                   "label": c.get("param_name", "")})
                disp_ids.add(eid)
            continue
        el = rendered.get(eid)
        if el is None:
            problems.append("missing element %s" % eid)
            continue
        kind = eid.split("-", 1)[0]
        t = el.get("transform") or ""
        m = re.match(r"^\s*rotate\(\s*([-\d.eE]+)(?:[\s,]+([-\d.eE]+)[\s,]+([-\d.eE]+))?\s*\)\s*$", t)
        cx, cy = float(c.get("cx", 0)), float(c.get("cy", 0))
        rot = bool(m)
        if m and m.group(2) is not None:
            dcx, dcy = float(m.group(2)), float(m.group(3))
            if abs(dcx - cx) > 0.6 or abs(dcy - cy) > 0.6:
                problems.append("%s: meta centre (%g,%g) != drawn rotation centre (%g,%g); using drawn" % (eid, cx, cy, dcx, dcy))
            cx, cy = dcx, dcy
        rec = {"p": c.get("param_name", ""), "el": eid, "t": kind, "cx": round(cx, 2), "cy": round(cy, 2),
               "a0": c.get("min_angle", -135), "a1": c.get("max_angle", 135)}
        if c.get("voicing"):
            rec["v"] = 1
        if not rot and kind == "knob":
            rec["fixed"] = 1       # a push-button drawn as a "knob": never rotated, lights up when on
        if any(d[0] == eid for d in dyn):
            continue
        controls.append(rec)
        dyn.append((eid, el))
    # ---------- live displays (meters, lamps, screens, glows; see work/illustrations/BRIEF.md "Live displays") ----------
    displays, gen_art = [], []
    for k, d in enumerate(disp_specs):
        t, eid = d.get("type"), d.get("element_id") or ""
        rec = {kk: v for kk, v in d.items() if kk not in ("label", "what", "should_show", "note", "notes")}
        rec["k"] = k
        generated = t in ("spectrum", "curve", "scope", "ring") or (t == "ladder" and d.get("bbox") and not d.get("segments")) \
            or (t == "led" and d.get("bbox") and not eid)   # a lamp the app draws itself (e.g. a small indicator pip)
        if t == "ring" and not d.get("bbox") and d.get("r") is not None:
            pad = float(d.get("r")) + float(d.get("width", 5))
            rec["bbox"] = d["bbox"] = [float(d["cx"]) - pad, float(d["cy"]) - pad, 2 * pad, 2 * pad]
        if generated:
            if not d.get("bbox"):
                problems.append("display %d (%s): no bbox" % (k, t))
                continue
            if eid and eid in rendered:
                gen_art.append(rendered[eid])      # the painted stand-in the live content replaces
            elif eid:
                problems.append("display %d: art element %s not found (drawn live anyway)" % (k, eid))
            rec["el"] = "disp-%d" % k
            rec["gen"] = 1
            displays.append(rec)
            continue
        el = rendered.get(eid)
        if el is None:
            problems.append("display %d (%s): missing element %s" % (k, t, eid))
            continue
        if any(x[0] == eid for x in dyn):
            problems.append("display %d: %s is already a control; skipped" % (k, eid))
            continue
        if t == "needle" or t == "spin" or (t == "move" and d.get("rotate")):
            m = re.match(r"^\s*rotate\(\s*([-\d.eE]+)[\s,]+([-\d.eE]+)[\s,]+([-\d.eE]+)\s*\)\s*$", el.get("transform") or "")
            if m and (rec.get("cx") is None or rec.get("cy") is None):
                rec["cx"], rec["cy"] = float(m.group(2)), float(m.group(3))
            if rec.get("cx") is None:
                rec["cx"], rec["cy"] = _num(el.get("data-cx")), _num(el.get("data-cy"))
        if t == "text" and local(el.tag) != "text" and not any(local(x.tag) == "text" for x in el.iter()):
            problems.append("display %d: %s has no <text>; skipped" % (k, eid))
            continue
        if t == "ladder":
            n = int(d.get("segments") or 0)
            have = sum(1 for x in el.iter() if re.match(r"^%s-seg-\d+$" % re.escape(eid), x.get("id") or ""))
            if not n or have < n:
                problems.append("display %d: ladder %s has %d/%d segments" % (k, eid, have, n))
                if not have:
                    continue
                rec["segments"] = have
        rec["el"] = eid
        displays.append(rec)
        dyn.append((eid, el))
    led = rendered.get("led")
    if led is not None and not any(d[0] == "led" for d in dyn):
        dyn.append(("led", led))
    dyn_ids = {d[0] for d in dyn}
    has_led = "led" in dyn_ids
    # Footswitch: the bypass hit area. Skip it if it would swallow another moving part.
    foot = None
    for fid in FOOT_IDS:
        el = rendered.get(fid)
        if el is None or fid in dyn_ids:
            continue
        inner = {x.get("id") for x in el.iter() if x is not el}
        if inner & dyn_ids:
            continue
        foot = fid
        dyn.append((fid, el))
        break
    # No moving part may contain another (it would be drawn twice).
    dyn_els = {id(e) for _, e in dyn}
    for eid, el in list(dyn):
        a = parent.get(el)
        while a is not None:
            if id(a) in dyn_els:
                problems.append("%s nested inside another moving part; left static" % eid)
                dyn = [d for d in dyn if d[0] != eid]
                controls = [c for c in controls if c["el"] != eid]
                displays = [x for x in displays if x.get("gen") or x["el"] != eid]
                break
            a = parent.get(a)

    # Printed labels drawn inside a switch's group (e.g. "VINTAGE / 1964") belong to the faceplate: move them out
    # of the moving part, so flipping the lever doesn't flip its legend too.
    for eid, el in dyn:
        if not eid.startswith("switch-"):
            continue
        p = parent.get(el)
        if p is None:
            continue
        texts = [c for c in list(el) if local(c.tag) == "text"]
        if not texts:
            continue
        keep_attrs = {k: v for k, v in el.attrib.items() if k in PRES and not (k == "transform" and v.strip().startswith("rotate("))}
        holder = ET.Element("{%s}g" % SVGNS, keep_attrs)
        for t in texts:
            el.remove(t)
            holder.append(t)
        p.insert(list(p).index(el), holder)
        parent[holder] = p
        for t in texts:
            parent[t] = holder

    # ---------- foreground layer ----------
    needed, queue = [], []
    seen = set()

    def want(ids):
        for i in ids:
            if i not in seen and i in byid:
                seen.add(i)
                queue.append(i)

    for _, el in dyn:
        for x in el.iter():
            want(refs_in(x))
        a = parent.get(el)
        while a is not None and a is not root:
            want(refs_in(a))
            a = parent.get(a)
    dyn_inner = {x.get("id") for _, el in dyn for x in el.iter() if x.get("id")}
    while queue:
        i = queue.pop(0)
        el = byid[i]
        for x in el.iter():
            want(refs_in(x))
        if i not in dyn_inner:
            needed.append(i)
    # keep document order for defs
    order_ix = {id(e): k for k, e in enumerate(root.iter())}
    needed.sort(key=lambda i: order_ix.get(id(byid[i]), 0))
    defs_xml = ""
    # drop defs nested inside other needed defs (they come along with their parent)
    needed_set = set(needed)
    top_needed = []
    for i in needed:
        a, nested = parent.get(byid[i]), False
        while a is not None:
            if a.get("id") in needed_set:
                nested = True
                break
            a = parent.get(a)
        if not nested:
            top_needed.append(i)
    for i in top_needed:
        e = byid[i]
        strip_ws(e)
        defs_xml += ser(e)

    parts = []
    for eid, el in dyn:
        chain = []
        a = parent.get(el)
        while a is not None and a is not root:
            attrs = {k: v for k, v in a.attrib.items() if k in PRES}
            if attrs:
                chain.append(attrs)
            a = parent.get(a)
        clone = ET.fromstring(ser(el))
        strip_ws(clone)
        drec = next((x for x in displays if not x.get("gen") and x["el"] == eid), None)
        if re.match(r"^\s*rotate\(", clone.get("transform") or "") and (not drec or drec.get("type") in ("needle", "spin", "move")):
            clone.attrib.pop("transform")   # the runtime rotates the wrapper from the parameter value / level
        for k in ("data-cx", "data-cy"):
            clone.attrib.pop(k, None)
        if drec and drec.get("type") == "ladder":
            for x in clone.iter():
                m = re.match(r"^%s-seg-(\d+)$" % re.escape(eid), x.get("id") or "")
                if m:
                    x.set("data-seg", m.group(1))
        if drec and drec.get("type") == "text":
            tx = clone if local(clone.tag) == "text" else next(x for x in clone.iter() if local(x.tag) == "text")
            tx.set("data-txt", "1")
        # Each moving part sits in its own wrapper <g data-ua="…">: the runtime sets the rotation, state classes
        # and bindings on the wrapper, so the part keeps its own transform / filter untouched.
        x = '<g data-ua="%s">%s</g>' % (eid, ser(clone))
        for attrs in chain:   # innermost first
            x = "<g %s>%s</g>" % (" ".join('%s="%s"' % (k, v.replace('"', "&quot;")) for k, v in attrs.items()), x)
        parts.append(x)
    gen_parts = "".join('<g data-ua="%s"></g>' % x["el"] for x in displays if x.get("gen"))
    fg = '<svg xmlns="%s" xmlns:xlink="%s" viewBox="%s" preserveAspectRatio="xMidYMid meet" class="ua-fg"><defs>%s</defs>%s%s</svg>' % (
        SVGNS, XLINK, vb, defs_xml, gen_parts, "".join(parts))
    fg = re.sub(r'\sxmlns(:\w+)?="[^"]*"', "", fg[fg.index(">") + 1:])   # inner namespace noise (HTML parser handles SVG)
    fg = '<svg viewBox="%s" preserveAspectRatio="xMidYMid meet" class="ua-fg" aria-hidden="false">' % vb + fg
    # namespace every id inside the layer so several instances can share one page
    fg = re.sub(r'(?<![\w-])id="([^"]+)"', r'id="__P__\1"', fg)
    fg = re.sub(r"url\(\s*(['\"]?)#", r"url(\1#__P__", fg)
    fg = re.sub(r'((?:xlink:)?href)="#', r'\1="#__P__', fg)
    fg = fg.replace('data-ua="__P__', 'data-ua="')   # (data-ua is never an id)

    # ---------- background (everything that never moves) ----------
    for _, el in dyn:
        p = parent.get(el)
        if p is not None:
            p.remove(el)
    for el in gen_art:
        p = parent.get(el)
        if p is not None and el in list(p):
            p.remove(el)
    backdrop = drop_backdrop(root, full_vbn)
    strip_ws(root)
    for k in ("width", "height"):
        root.attrib.pop(k, None)
    root.set("preserveAspectRatio", "xMidYMid meet")
    bg = ser(root)
    # full (thumbnail) copy: the original drawing, sanitised and compacted
    full_root = ET.fromstring(sanitize(raw))
    deshadow(full_root, full_vbn)
    strip_meta_text(full_root)
    drop_backdrop(full_root, full_vbn)
    full_root.set("viewBox", vb)
    strip_ws(full_root)
    for k in ("width", "height"):
        full_root.attrib.pop(k, None)
    full = ser(full_root)

    h = hashlib.sha1((raw + json.dumps(meta, sort_keys=True) + json.dumps(order) + VERSION + vb).encode("utf-8")).hexdigest()[:10]
    jacks, jacks_src = jacks_for(meta, meta.get("kind", "other"), vbn, crop, raw_key, ET.fromstring(sanitize(raw)), problems)
    return {
        "info": {"name": meta.get("name", ""), "kind": meta.get("kind", "other"), "vb": vbn, "controls": controls,
                 "order": order or [], "led": has_led, "foot": foot, "backdrop": backdrop, "h": h, "k": raw_key,
                 "jacks": jacks, "jacks_src": jacks_src,
                 **({"displays": displays} if displays else {})},
        "files": {"%s.%s.svg" % (tid, h): full, "%s.%s.bg.svg" % (tid, h): bg,
                  "%s.%s.fg.js" % (tid, h): "HRUnitArt.def(%d,%s);\n" % (tid, json.dumps(fg, ensure_ascii=False))},
        "problems": problems,
    }


VERSION = "ua-9"


# ---------- sockets drawn in the art ----------
# A socket is drawn as a dark hole (a near-black circle, r 2.5..9) with a larger ring or bushing circle around the
# same centre. They are read straight from the source drawing on every sync, through all its transforms, so a
# redrawn face can never keep a stale position (the browser measurement in unit_art_crop.json is only valid for the
# drawing it was taken from).
def _affine(t):
    m = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
    for fn, args in re.findall(r"(matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\)", t or ""):
        a = [float(x) for x in re.split(r"[\s,]+", args.strip()) if x]
        if fn == "matrix" and len(a) == 6:
            n = tuple(a)
        elif fn == "translate":
            n = (1, 0, 0, 1, a[0], a[1] if len(a) > 1 else 0)
        elif fn == "scale":
            n = (a[0], 0, 0, a[1] if len(a) > 1 else a[0], 0, 0)
        elif fn == "rotate":
            import math
            c, s_ = math.cos(math.radians(a[0])), math.sin(math.radians(a[0]))
            cx, cy = (a[1], a[2]) if len(a) > 2 else (0, 0)
            n = (c, s_, -s_, c, cx - c * cx + s_ * cy, cy - s_ * cx - c * cy)
        else:
            continue
        m = _mul(m, n)
    return m


def _mul(m, n):
    a, b, c, d, e, f = m
    A, B, C, D, E, F = n
    return (a * A + c * B, b * A + d * B, a * C + c * D, b * C + d * D, a * E + c * F + e, b * E + d * F + f)


def _attr(e, k):
    m = re.search(r"(?:^|;)\s*%s\s*:\s*([^;]+)" % k, e.get("style") or "")
    return (m.group(1) if m else e.get(k) or "").strip()


def sockets_in(svg_root, el):
    """Socket centres inside element el (or the whole drawing), in the drawing's coordinates, in reading order
    (rows top to bottom, then left to right)."""
    parent = {c: p for p in svg_root.iter() for c in p}
    def ctm(e):
        chain = []
        while e is not None:
            chain.append(e)
            e = parent.get(e)
        m = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
        for x in reversed(chain):
            if x.get("transform"):
                m = _mul(m, _affine(x.get("transform")))
        return m
    circles = []
    for c in (el if el is not None else svg_root).iter():
        if local(c.tag) != "circle":
            continue
        a = parent.get(c)
        hidden = False
        while a is not None:
            if local(a.tag) in ("defs", "symbol", "pattern", "clipPath", "mask", "marker"):
                hidden = True
            a = parent.get(a)
        if hidden:
            continue
        m = ctm(c)
        cx, cy, r = _num(c.get("cx")), _num(c.get("cy")), _num(c.get("r"))
        k = (abs(m[0] * m[3] - m[1] * m[2])) ** 0.5
        circles.append((m[0] * cx + m[2] * cy + m[4], m[1] * cx + m[3] * cy + m[5], r * k, _attr(c, "fill"), _num(_attr(c, "opacity") or 1, 1)))
    holes = []
    for x, y, r, fill, op in circles:
        if not (2.5 <= r <= 9 and op >= 0.9 and _is_dark(fill)):
            continue
        if not any(abs(x2 - x) < 1.0 and abs(y2 - y) < 1.0 and r < r2 <= 3 * r and o2 >= 0.9 for x2, y2, r2, f2, o2 in circles):
            continue   # a lone dark dot (screw head, lamp) is not a socket
        if not any(abs(hx - x) < 3 and abs(hy - y) < 3 for hx, hy in holes):
            holes.append((x, y))
    rows = []
    for h in sorted(holes, key=lambda p: p[1]):
        if rows and abs(rows[-1][0][1] - h[1]) < 12:
            rows[-1].append(h)
        else:
            rows.append([h])
    return [h for row in rows for h in sorted(row)]


def drawn_jacks(meta, svg_root, problems):
    """Front-panel jacks named in the meta and resolved from the drawing on every sync: "input_jack" (the guitar
    input) and "output_jack". Each names the socket element ("jack-high"), or the n-th socket inside an element that
    holds several ("vox-inputs#3", reading order). For an amp the input is the one the circuit actually uses (its
    io.input jack) or, for behavioural models, the conventional High / channel-1 input."""
    out = []
    for key, k in (("input_jack", "in"), ("output_jack", "out")):
        ref = meta.get(key)
        if not ref:
            continue
        eid, _, nth = str(ref).partition("#")
        el = next((e for e in svg_root.iter() if e.get("id") == eid), None)
        if el is None:
            problems.append("%s %r: no element with that id" % (key, ref))
            continue
        socks = sockets_in(svg_root, el)
        i = int(nth or 1) - 1
        if not 0 <= i < len(socks):
            problems.append("%s %r: %d socket(s) drawn in it" % (key, ref, len(socks)))
            continue
        out.append({"k": k, "x": socks[i][0], "y": socks[i][1], "edge": "front", "label": ""})
    return out


def jacks_for(meta, kind, vb, crop, raw_key, svg_root=None, problems=None):
    """Where cables plug in, in the drawing's own coordinates: {k: in|out|send|return|exp|power, x, y, edge, label}.
    edge = which way the cable leaves (left/right/top/bottom of the hardware, 'front' out of a panel, 'rear' = behind it).
    Source order: meta "jacks" (hand-placed) > auto-detected from the SVG by measure_unit_art.js > real-world defaults."""
    x0, y0, W, H = vb
    src, js = "default", []
    problems = problems if problems is not None else []
    dj = drawn_jacks(meta, svg_root, problems) if svg_root is not None else []
    if dj:
        # resolved from the drawing itself; hand-placed jacks of the other kinds still apply
        ks = {j["k"] for j in dj}
        src, js = "art", dj + [dict(j) for j in (meta.get("jacks") or []) if j.get("k") not in ks]
    elif isinstance(meta.get("jacks"), list) and meta["jacks"]:
        src, js = "meta", [dict(j) for j in meta["jacks"]]
    elif crop and crop.get("k") == raw_key and crop.get("jacks"):
        src, js = "auto", [dict(j) for j in crop["jacks"] if x0 - 2 <= j.get("x", -1e9) <= x0 + W + 2 and y0 - 2 <= j.get("y", -1e9) <= y0 + H + 2]
    if src == "auto" and kind == "amp":
        # an amp: one guitar input (a socket the drawing names beats one found by a label; HI / 1 / INPUT first);
        # label-found "inputs" next to EQ knobs or switches are dropped
        def score(j):
            t = (str(j.get("src", "")) + " " + j.get("label", "")).lower()
            return (0 if t.startswith(("id:", "group:")) else 2) + (0 if re.search(r"hi|high|-1\b|\b1\b|input|inst", t) else 1)
        def guitar_in(j):
            lab = j.get("label", "")
            if re.search(r"input|inst", lab, re.I):
                return True
            return (j.get("k") or "in") == "in" and not re.search(r"power|low|\blo\b|normal|bright|output", lab, re.I)
        ins = sorted([j for j in js if guitar_in(j)], key=score)
        js = ([dict(ins[0], k="in")] if ins else []) + [j for j in js if j.get("k") in ("send", "return") and str(j.get("src", "")).startswith(("id:", "group:"))]
    if src == "auto" and kind == "pedal":
        js = [j for j in js if not re.search(r"power|bright|low|\blo\b|\bhi\b|high", j.get("label", ""), re.I)]
    if src == "auto" and kind in ("rack", "other"):
        # rack gear is cabled at the back: only sockets the drawing itself names count (labels next to knobs are not jacks)
        js = [j for j in js if str(j.get("src", "")).startswith(("id:", "group:")) and j.get("k") != "power"]
    for j in js:
        j.pop("src", None)
        j["k"] = j.get("k") or ""
        if kind == "amp" and j["k"] == "in" and j.get("edge") in (None, "", "left", "right"):
            j["edge"] = "front"   # an amp's guitar input is on its front panel: the plug goes in end-on
        if not j.get("edge"):
            d = {"left": (j["x"] - x0) / W, "right": (x0 + W - j["x"]) / W, "top": (j["y"] - y0) / H, "bottom": (y0 + H - j["y"]) / H}
            e = min(d, key=d.get)
            j["edge"] = e if d[e] < 0.12 else "front"
    if src != "meta":
        unk = [j for j in js if j["k"] not in ("in", "out", "send", "return", "exp", "power")]
        if kind == "pedal" and unk:
            # stompboxes: input on the right, output on the left (top-mounted jacks: rightmost in, leftmost out)
            known = {j["k"] for j in js}
            side = sorted(unk, key=lambda j: j["x"])
            if "out" not in known and side:
                side[0]["k"] = "out"
            if "in" not in known and len(side) > 1:
                side[-1]["k"] = "in"
            for j in side:
                if not j["k"]:
                    j["k"] = "aux"
        elif unk:
            for j in unk:
                j["k"] = "in" if kind in ("amp", "other") else "aux"
        js = [j for j in js if j["k"] != "aux"]
    have = {j["k"] for j in js}
    if kind == "pedal":
        if "in" not in have:
            js.append({"k": "in", "x": x0 + W, "y": y0 + H * 0.2, "edge": "right", "label": ""})
        if "out" not in have:
            js.append({"k": "out", "x": x0, "y": y0 + H * 0.2, "edge": "left", "label": ""})
    elif kind == "amp":
        if "in" not in have:
            problems.append("no guitar input found in the drawing: set meta input_jack (cable falls back to a guess)")
            js.append({"k": "in", "x": x0 + W * 0.08, "y": y0 + H * 0.72, "edge": "front", "label": ""})
        if "out" not in have:   # the speaker output is on the back panel
            js.append({"k": "out", "x": x0 + W * 0.9, "y": y0 + H * 0.5, "edge": "rear", "label": "speaker out"})
    elif kind == "cab":
        js = [{"k": "in", "x": x0 + W * 0.97, "y": y0 + H * 0.5, "edge": "rear", "label": "speaker in"}]
    elif kind == "rack" or kind == "other":
        if "in" not in have:
            js.append({"k": "in", "x": x0 + W * 0.85, "y": y0 + H * 0.5, "edge": "rear", "label": ""})
        if "out" not in have:
            js.append({"k": "out", "x": x0 + W * 0.15, "y": y0 + H * 0.5, "edge": "rear", "label": ""})
    for j in js:
        j["x"], j["y"] = round(float(j["x"]), 1), round(float(j["y"]), 1)
    if src == "default" and js:
        src = "default"
    return js, src


def main():
    os.makedirs(OUT, exist_ok=True)
    comps = {}
    try:
        for c in json.load(open(os.path.join(SRC, "components.json"), encoding="utf-8")):
            comps[int(c["table_id"])] = c.get("controls", [])
    except Exception as e:  # panel order is only a fallback
        log("components.json unreadable (%s); panel-order fallback disabled" % e)
    units, keep, skipped = {}, {"index.js", "index.json"}, []
    # Last good bundle: a unit whose source is mid-edit (unparseable) keeps its previous version instead of
    # dropping back to the classic face.
    try:
        prev_units = json.load(open(os.path.join(OUT, "index.json"), encoding="utf-8")).get("units", {})
    except Exception:
        prev_units = {}
    metas = sorted(f for f in os.listdir(os.path.join(SRC, "meta")) if re.match(r"^\d+\.json$", f))
    for f in metas:
        tid = int(f[:-5])
        svg = os.path.join(SRC, "svg", "%d.svg" % tid)
        if not os.path.exists(svg):
            skipped.append((tid, "no svg yet"))
            continue
        lastgood = os.path.join(SRC, ".lastgood")
        try:
            meta = json.load(open(os.path.join(SRC, "meta", f), encoding="utf-8"))
            r = process(tid, svg, meta, comps.get(tid))
            # remember this source pair as the unit's last good version
            os.makedirs(lastgood, exist_ok=True)
            import shutil as _sh
            _sh.copyfile(svg, os.path.join(lastgood, "%d.svg" % tid))
            with open(os.path.join(lastgood, "%d.json" % tid), "w", encoding="utf-8") as fh:
                json.dump(meta, fh)
        except Exception as e:
            # Source mid-edit by an illustrator: rebuild from the last good source pair if we have one.
            lg_svg, lg_meta = os.path.join(lastgood, "%d.svg" % tid), os.path.join(lastgood, "%d.json" % tid)
            if os.path.exists(lg_svg) and os.path.exists(lg_meta):
                try:
                    r = process(tid, lg_svg, json.load(open(lg_meta, encoding="utf-8")), comps.get(tid))
                    skipped.append((tid, "source mid-edit, built from last good source"))
                    e = None
                except Exception as e2:
                    e = e2
            if e is None:
                pass
            else:
                old = prev_units.get(str(tid))
                if old and all(os.path.exists(os.path.join(OUT, "%d.%s%s" % (tid, old.get("h", ""), ext))) for ext in (".svg", ".bg.svg", ".fg.js")):
                    units[str(tid)] = old
                    for ext in (".svg", ".bg.svg", ".fg.js"):
                        keep.add("%d.%s%s" % (tid, old["h"], ext))
                    skipped.append((tid, "source mid-edit, kept last good version (%s)" % str(e)[:80]))
                else:
                    skipped.append((tid, "not finished / invalid: %s" % str(e)[:120]))
                continue
        if not r["info"]["controls"]:
            skipped.append((tid, "no bindable controls"))
            continue
        for name, data in r["files"].items():
            keep.add(name)
            path = os.path.join(OUT, name)
            if not os.path.exists(path):
                with open(path + ".tmp", "w", encoding="utf-8") as fh:
                    fh.write(data)
                os.replace(path + ".tmp", path)
        units[str(tid)] = r["info"]
        for p in r["problems"]:
            log("  %d: %s" % (tid, p))
    # Keep the previous generation too: a running app (or plugin) still references the file names from the index
    # it loaded at startup, so deleting them makes its faces fall back to placeholders until it is relaunched.
    try:
        prev = {} if "--prune" in sys.argv else json.load(open(os.path.join(OUT, "index.json"), encoding="utf-8")).get("units", {})
        for t, u in prev.items():
            for ext in (".svg", ".bg.svg", ".fg.js"):
                keep.add("%s.%s%s" % (t, u.get("h", ""), ext))
    except Exception:
        pass
    # Also keep anything written in the last 3 days: several re-syncs can happen while one app session stays open.
    import time as _t
    cutoff = _t.time() - 3 * 86400
    if "--prune" in sys.argv:   # release/commit: keep only what the current index references
        cutoff = float("inf")
    removed = []
    for f in os.listdir(OUT):
        fp = os.path.join(OUT, f)
        if f not in keep and os.path.getmtime(fp) < cutoff:
            os.remove(fp)
            removed.append(f)
    if "--prune" in sys.argv:
        print("pruned %d superseded file(s)%s" % (len(removed), "".join("\n  " + f for f in sorted(removed, key=lambda f: (int(f.split(".")[0]) if f.split(".")[0].isdigit() else 0, f)))))
    index = {"version": VERSION, "units": units}
    js = json.dumps(index, ensure_ascii=False, separators=(",", ":"))
    for name, data in (("index.json", json.dumps(index, ensure_ascii=False, indent=1)), ("index.js", "window.HR_UNIT_ART = %s;\n" % js)):
        with open(os.path.join(OUT, name + ".tmp"), "w", encoding="utf-8") as fh:
            fh.write(data)
        os.replace(os.path.join(OUT, name + ".tmp"), os.path.join(OUT, name))
    pending = sorted(int(c) for c in comps if not os.path.exists(os.path.join(SRC, "meta", "%d.json" % c)))
    log("unit art: %d units bundled, %d skipped, %d without art yet" % (len(units), len(skipped), len(pending)))
    for tid, why in skipped:
        log("  skipped %d: %s" % (tid, why))


if __name__ == "__main__":
    main()
