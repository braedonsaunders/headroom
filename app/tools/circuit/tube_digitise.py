#!/usr/bin/env python3
"""Semi-automatic digitiser for scanned valve plate-characteristic charts (feeds tools/circuit/tube_fit.py).

The datasheet scans stay outside the repository; each chart is described by a small JSON config kept next to the
digitised values in work/tube-curves/<tube>/ (source PDF URL, page, rotation, plot box, pixel calibration, and which
traced component is which grid voltage). Only the resulting numbers are stored.

  tube_digitise.py render     <cfg.json> <chart>            rasterise the page (native scan resolution) to <scratch>
  tube_digitise.py grid       <cfg.json> <chart>            print gridline pixel positions inside the plot box
  tube_digitise.py components <cfg.json> <chart> [out.png]  label curve-like components (grid lines removed); overlay
  tube_digitise.py extract    <cfg.json> [chart...]         centre-lines -> data; appends to the tube's CSV; overlay

Method: threshold, morphological opening with a k×k square (removes the 1-3 px grid, keeps the 5-8 px curve
strokes), connected components, then a centre-line from the per-column and per-row runs that are no wider than a
stroke (labels and crossings are skipped), resampled along the arc length with a running median.
Charts whose curves carry "seed" are re-extracted with tube_guided.py, charts with a "traces" key with tube_trace.py
(seed-and-track tracers for dashed or merged curves; both reuse this module's masks and calibration).
Requires numpy, scipy, Pillow and poppler's pdftoppm. Scratch images go to $TUBEFIT_SCRATCH (default /tmp/tubefit).
"""
import csv
import json
import math
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as nd

SCRATCH = os.environ.get("TUBEFIT_SCRATCH", "/tmp/tubefit")


def load_cfg(path):
    with open(path) as f:
        cfg = json.load(f)
    cfg["_dir"] = os.path.dirname(os.path.abspath(path))
    return cfg


def page_image(cfg, ch):
    """Grayscale page at the scan's native resolution, rotated so plate volts run left to right."""
    os.makedirs(SCRATCH, exist_ok=True)
    pdf = os.path.join(SCRATCH, cfg["pdf_file"])
    if not os.path.exists(pdf):
        subprocess.run(["curl", "-sL", "-o", pdf, cfg["pdf_url"]], check=True)
    stem = os.path.join(SCRATCH, "%s_p%d" % (os.path.splitext(cfg["pdf_file"])[0], ch["page"]))
    png = stem + "-%d.png" % ch["page"]
    if not os.path.exists(png):
        subprocess.run(["pdftoppm", "-r", str(ch.get("dpi", 300)), "-gray", "-png", "-f", str(ch["page"]), "-l", str(ch["page"]), pdf, stem], check=True)
        cands = [p for p in os.listdir(SCRATCH) if p.startswith(os.path.basename(stem) + "-") and p.endswith(".png")]
        if cands and not os.path.exists(png):
            os.rename(os.path.join(SCRATCH, cands[0]), png)
    im = Image.open(png).convert("L")
    if ch.get("rotate", 0):
        im = im.rotate(ch["rotate"], expand=True)
    if ch.get("deskew", 0):  # small scan skew, degrees counter-clockwise
        im = im.rotate(ch["deskew"], resample=Image.BICUBIC, expand=False, fillcolor=255)
    return im


def chart_by_name(cfg, name):
    for ch in cfg["charts"]:
        if ch["name"] == name:
            return ch
    raise SystemExit("no chart %s" % name)


def masks(cfg, ch):
    im = page_image(cfg, ch)
    a = np.array(im) < ch.get("threshold", 128)
    x0, y0, x1, y1 = ch["box"]
    inner = np.zeros_like(a)
    m = ch.get("frame_margin", 6)
    inner[y0 + m:y1 - m, x0 + m:x1 - m] = True
    for bx in ch.get("exclude", []):
        inner[bx[1]:bx[3], bx[0]:bx[2]] = False
    k = ch.get("open_px", 4)
    src = a
    if ch.get("erase_grid"):
        # Heavy grids (major lines as thick as the curves): blank every row/column of the box that is mostly dark,
        # then close the small gaps this cuts into the curves before the opening.
        src = a.copy()
        frac = ch.get("grid_frac", 0.5)
        sub = src[y0:y1, x0:x1]
        cols = np.nonzero(sub.mean(0) > frac)[0]
        rows = np.nonzero(sub.mean(1) > frac)[0]
        sub[:, cols] = False
        sub[rows, :] = False
        g = ch.get("close_px", 7)  # bridge the blanked stripes along each axis separately
        src = nd.binary_closing(src, structure=np.ones((1, g)))
        src = nd.binary_closing(src, structure=np.ones((g, 1)))
    o = nd.binary_opening(src, structure=np.ones((k, k))) & inner
    for bx in ch.get("cut", []):  # separate curves that touch (e.g. at the origin) before labelling
        o[bx[1]:bx[3], bx[0]:bx[2]] = False
    return im, a, o


def calib(ch):
    (px0, v0), (px1, v1) = ch["xcal"]
    (py0, w0), (py1, w1) = ch["ycal"]
    fx = lambda px: v0 + (px - px0) * (v1 - v0) / (px1 - px0)
    fy = lambda py: w0 + (py - py0) * (w1 - w0) / (py1 - py0)
    return fx, fy


def cmd_grid(cfg, name):
    ch = chart_by_name(cfg, name)
    im = page_image(cfg, ch)
    a = np.array(im) < ch.get("threshold", 128)
    x0, y0, x1, y1 = ch["box"]
    sub = a[y0:y1, x0:x1]
    for axis, lab, off in ((0, "x (columns)", x0), (1, "y (rows)", y0)):
        f = sub.mean(axis)
        thr = ch.get("grid_frac", 0.5)
        peaks, cur = [], []
        for i, v in enumerate(f):
            if v > thr:
                cur.append(i)
            elif cur:
                peaks.append(off + sum(cur) / len(cur))
                cur = []
        if cur:
            peaks.append(off + sum(cur) / len(cur))
        d = np.diff(peaks) if len(peaks) > 1 else []
        print(lab, len(peaks), "lines; median spacing %.2f" % (np.median(d) if len(d) else 0))
        print("  " + " ".join("%.1f" % p for p in peaks))


def cmd_components(cfg, name, out=None):
    ch = chart_by_name(cfg, name)
    im, a, o = masks(cfg, ch)
    lab, n = nd.label(o, structure=np.ones((3, 3)))
    sz = nd.sum(o, lab, range(1, n + 1))
    objs = nd.find_objects(lab)
    rgb = im.convert("RGB")
    dr = ImageDraw.Draw(rgb)
    cols = [(230, 25, 75), (60, 180, 75), (0, 130, 200), (245, 130, 48), (145, 30, 180), (70, 240, 240), (240, 50, 230), (128, 128, 0)]
    keep = []
    for i in range(n):
        if sz[i] < ch.get("min_px", 250):
            continue
        sl = objs[i]
        keep.append((i + 1, int(sz[i]), sl))
    ov = np.array(rgb)
    for j, (cid, s, sl) in enumerate(keep):
        ov[lab == cid] = cols[j % len(cols)]
    rgb = Image.fromarray(ov)
    dr = ImageDraw.Draw(rgb)
    fx, fy = calib(ch)
    for j, (cid, s, sl) in enumerate(keep):
        ys, xs = sl
        cx, cy = (xs.start + xs.stop) // 2, (ys.start + ys.stop) // 2
        dr.text((xs.stop + 2, ys.start), str(cid), fill=(255, 0, 0))
        print("%5d px %6d  x %4d-%4d (%.1f..%.1f)  y %4d-%4d (%.3f..%.3f)" % (cid, s, xs.start, xs.stop, fx(xs.start), fx(xs.stop), ys.start, ys.stop, fy(ys.stop), fy(ys.start)))
    x0, y0, x1, y1 = ch["box"]
    rgb = rgb.crop((max(0, x0 - 20), max(0, y0 - 20), x1 + 20, y1 + 20))
    rgb.save(out or os.path.join(SCRATCH, "components_%s.png" % name))


def centreline(o_comp, a, maxrun):
    """Centre points of a component from narrow runs along columns and rows."""
    pts = []
    ys, xs = np.nonzero(o_comp)
    if len(xs) == 0:
        return pts
    for axis in (0, 1):
        lines = np.unique(xs if axis == 0 else ys)
        for L in lines:
            if axis == 0:
                idx = np.nonzero(o_comp[:, L])[0]
            else:
                idx = np.nonzero(o_comp[L, :])[0]
            if len(idx) == 0:
                continue
            runs = np.split(idx, np.nonzero(np.diff(idx) > 1)[0] + 1)
            if len(runs) != 1:
                continue
            r = runs[0]
            if len(r) > maxrun:
                continue
            c = 0.5 * (r[0] + r[-1])
            pts.append((float(L), c) if axis == 0 else (c, float(L)))
    return pts


def order_and_resample(pts, step, win):
    """Order points along the curve (greedy nearest neighbour from one end), resample every `step` px of arc length
    with a running median over +-win px of arc."""
    if len(pts) < 3:
        return pts
    P = np.array(pts)
    # start from the extreme point along the principal direction
    c = P - P.mean(0)
    u = np.linalg.svd(c, full_matrices=False)[2][0]
    t = c @ u
    order = np.argsort(t)
    P = P[order]
    # arc length along the sorted projection, refined by local distance
    d = np.r_[0, np.cumsum(np.hypot(*np.diff(P, axis=0).T))]
    out = []
    s = 0.0
    while s <= d[-1]:
        sel = (d >= s - win) & (d <= s + win)
        if sel.sum() >= 3:
            out.append(tuple(np.median(P[sel], axis=0)))
        s += step
    return out


def trace_refine(a, way, step=4.0, half=10, tmin=5, tmax=12):
    """Curves on heavy grids: follow a hand-given rough polyline (pixel waypoints) and, every `step` px, take the
    centre of the dark run across it (along the normal) whose width is stroke-like (tmin..tmax px) and nearest the
    guide. Grid lines are thinner than tmin across their own direction; a run is rejected when it is too wide
    (a crossing or a label)."""
    W = np.array(way, dtype=float)
    pts = []
    H, Wd = a.shape
    for i in range(len(W) - 1):
        p0, p1 = W[i], W[i + 1]
        L = float(np.hypot(*(p1 - p0)))
        u = (p1 - p0) / L
        nrm = np.array([-u[1], u[0]])
        for s in np.arange(0.0, L, step):
            c = p0 + s * u
            ts = np.arange(-half, half + 1)
            xy = np.rint(c[None, :] + ts[:, None] * nrm[None, :]).astype(int)
            ok = (xy[:, 0] >= 0) & (xy[:, 0] < Wd) & (xy[:, 1] >= 0) & (xy[:, 1] < H)
            prof = np.zeros(len(ts), bool)
            prof[ok] = a[xy[ok, 1], xy[ok, 0]]
            best = None
            j = 0
            while j < len(ts):
                if prof[j]:
                    k = j
                    while k + 1 < len(ts) and prof[k + 1]:
                        k += 1
                    w = k - j + 1
                    if tmin <= w <= tmax and j > 0 and k < len(ts) - 1:
                        mid = 0.5 * (ts[j] + ts[k])
                        if best is None or abs(mid) < abs(best):
                            best = mid
                    j = k + 1
                else:
                    j += 1
            if best is not None:
                q = c + best * nrm
                pts.append((float(q[0]), float(q[1])))
    return pts


def stroke_across(a, c, nrm, half, tmin, tmax):
    """Offset (px, along nrm) of the stroke-like dark run nearest c, or None."""
    H, Wd = a.shape
    ts = np.arange(-half, half + 1)
    xy = np.rint(c[None, :] + ts[:, None] * nrm[None, :]).astype(int)
    ok = (xy[:, 0] >= 0) & (xy[:, 0] < Wd) & (xy[:, 1] >= 0) & (xy[:, 1] < H)
    prof = np.zeros(len(ts), bool)
    prof[ok] = a[xy[ok, 1], xy[ok, 0]]
    best, j = None, 0
    while j < len(ts):
        if prof[j]:
            k = j
            while k + 1 < len(ts) and prof[k + 1]:
                k += 1
            if tmin <= k - j + 1 <= tmax and j > 0 and k < len(ts) - 1:
                mid = 0.5 * (ts[j] + ts[k])
                if best is None or abs(mid) < abs(best):
                    best = mid
            j = k + 1
        else:
            j += 1
    return best


def track(a, start, direction, stop, step=3.0, half=8, tmin=5, tmax=12, max_miss=6):
    """Follow a curve from `start` (pixel) heading `direction`, re-centring on the stroke every step and steering by
    the recent points; ends at the stop box [x0, y0, x1, y1] (leaving it) or after max_miss steps without a stroke."""
    p = np.array(start, float)
    u = np.array(direction, float)
    u /= np.hypot(*u)
    pts, miss = [], 0
    hist = [p.copy()]
    for _ in range(5000):
        c = p + step * u
        nrm = np.array([-u[1], u[0]])
        b = stroke_across(a, c, nrm, half, tmin, tmax)
        if b is None:
            miss += 1
            if miss > max_miss:
                break
            p = c
            continue
        miss = 0
        p = c + b * nrm
        pts.append((float(p[0]), float(p[1])))
        hist.append(p.copy())
        if len(hist) > 6:
            d = hist[-1] - hist[-7]
            if np.hypot(*d) > 1e-6:
                u = 0.5 * u + 0.5 * d / np.hypot(*d)
                u /= np.hypot(*u)
        if not (stop[0] <= p[0] <= stop[2] and stop[1] <= p[1] <= stop[3]):
            break
    return pts


def cmd_extract(cfg, names):
    tube_csv = os.path.join(cfg["_dir"], cfg.get("csv", "curves.csv"))
    rows_all = []
    done = set()
    for ch in cfg["charts"]:
        if (names and ch["name"] not in names) or not ch.get("curves"):  # "traces" charts: tools/circuit/tube_trace.py
            continue
        if any("seed" in cv for cv in ch["curves"]):  # "seed" curves: tools/circuit/tube_guided.py
            continue
        done.add(ch["name"])
        im, a, o = masks(cfg, ch)
        lab, n = nd.label(o, structure=np.ones((3, 3)))
        fx, fy = calib(ch)
        rgb = im.convert("RGB")
        dr = ImageDraw.Draw(rgb)
        for cv in ch["curves"]:
            comp = np.isin(lab, cv.get("ids", []))
            for bx in cv.get("exclude", []):
                comp[bx[1]:bx[3], bx[0]:bx[2]] = False
            if "track" in cv:  # {"start": [x, y], "dir": [dx, dy]}; stops at the chart's box (or cv "stop")
                t = cv["track"]
                pts = track(a, t["start"], t["dir"], cv.get("stop", ch["box"]), ch.get("trace_step", 3.0), ch.get("trace_half", 8),
                            ch.get("tmin", 5), ch.get("tmax", 12))
            elif "trace" in cv:
                pts = trace_refine(a, cv["trace"], ch.get("trace_step", 4.0), ch.get("trace_half", 10), ch.get("tmin", 5), ch.get("tmax", 12))
            else:
                pts = centreline(comp, a, ch.get("maxrun", 14))
            pts = order_and_resample(pts, ch.get("step", 12), ch.get("win", 8))
            for (px, py) in pts:
                x, y = fx(px), fy(py)
                if "xmin" in cv and x < cv["xmin"]:
                    continue
                if "ymin" in cv and y < cv["ymin"]:
                    continue
                row = dict(source=cfg["source_id"], chart=ch["name"], quantity=cv.get("quantity", ch.get("quantity", "ia")),
                           vg1=cv.get("vg1"), vg2=cv.get("vg2", ch.get("vg2", "")), va=round(x, 2) if ch.get("x", "va") == "va" else cv.get("va"),
                           i_ma=round(y, 4))
                if ch.get("x", "va") == "vg1":  # transfer chart: x is grid voltage
                    row["vg1"] = round(x, 3)
                    row["va"] = cv.get("va")
                rows_all.append(row)
                dr.ellipse((px - 3, py - 3, px + 3, py + 3), outline=(255, 0, 0), width=2)
        x0, y0, x1, y1 = ch["box"]
        rgb.crop((max(0, x0 - 20), max(0, y0 - 20), x1 + 20, y1 + 20)).save(os.path.join(SCRATCH, "extract_%s.png" % ch["name"]))
    keep = []
    if os.path.exists(tube_csv):
        with open(tube_csv) as f:
            for r in csv.DictReader(f):
                if r["source"] == cfg["source_id"] and r["chart"] in done:  # only the charts re-extracted here
                    continue
                keep.append(r)
    fields = ["source", "chart", "quantity", "vg1", "vg2", "va", "i_ma"]
    with open(tube_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in keep + rows_all:
            w.writerow({k: r.get(k, "") for k in fields})
    print("%d points -> %s" % (len(rows_all), tube_csv))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd, cfg = sys.argv[1], load_cfg(sys.argv[2])
    if cmd == "render":
        ch = chart_by_name(cfg, sys.argv[3])
        im = page_image(cfg, ch)
        p = os.path.join(SCRATCH, "page_%s.png" % ch["name"])
        im.save(p)
        print(p, im.size)
    elif cmd == "grid":
        cmd_grid(cfg, sys.argv[3])
    elif cmd == "components":
        cmd_components(cfg, sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
    elif cmd == "extract":
        cmd_extract(cfg, sys.argv[3:])
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
