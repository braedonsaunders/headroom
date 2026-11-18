"""seed-and-track column tracer on top of tube_digitise (handles dashed curves, crossings).
Config: chart entries like tube_digitise, curves with "seed": [x_val, y_val] (data units) instead of "ids";
optional "xmin"/"xmax" (data), "maxgap" (px). Writes the same CSV columns as tube_digitise extract."""
import csv, os, sys, math, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import ImageDraw
import tube_digitise as td

def runs_in(col):
    idx = np.nonzero(col)[0]
    if len(idx) == 0:
        return []
    return [(r[0], r[-1]) for r in np.split(idx, np.nonzero(np.diff(idx) > 1)[0] + 1)]

def track(o, x0, y0, direction, ch, cv, inv, box=None):
    box = box or ch["box"]
    x1lim = box[2] - 7 if direction > 0 else box[0] + 7
    stroke = ch.get("stroke", 7)
    maxgap = cv.get("maxgap", ch.get("maxgap", 40))
    tol = cv.get("tol", ch.get("tol", 10))
    pts = []
    x, y, slope, gap = x0, y0, 0.0, 0
    hist = []
    while (x - x1lim) * direction < 0:
        x += direction
        pred = y + slope * direction * (gap + 1)
        allow = stroke * math.sqrt(1 + slope * slope) + 6
        best = None
        for a, b in runs_in(o[:, x]):
            c = 0.5 * (a + b)
            L = b - a + 1
            t = tol + abs(slope) * 1.0
            if abs(c - pred) <= t and L <= allow + (2 if len(hist) < 5 else 0):
                if best is None or abs(c - pred) < abs(best - pred):
                    best = c
            elif L > allow and a - t <= pred <= b + t and len(hist) < 3:
                pass
        xv = inv[0](x)
        if cv.get("axis") == "y":
            xv = -1e9
        if (cv.get("xmax") is not None and xv > cv["xmax"]) or (cv.get("xmin") is not None and xv < cv["xmin"]):
            break
        if best is None:
            gap += 1
            if gap > maxgap:
                break
            continue
        hist.append((x, best))
        pts.append((x, best))
        # slope from last ~15 px of points
        tail = [p for p in hist if abs(p[0] - x) <= 18]
        if len(tail) >= 4:
            xs = np.array([p[0] for p in tail], float); ys = np.array([p[1] for p in tail], float)
            s = np.polyfit(xs, ys, 1)[0]
            slope = max(-8, min(8, s))
        y = best
        gap = 0
    return pts

def main(cfgp, names):
    cfg = td.load_cfg(cfgp)
    out = os.path.join(cfg["_dir"], cfg.get("csv", "curves.csv"))
    rows = []
    for ch in cfg["charts"]:
        if names and ch["name"] not in names:
            continue
        im, a, o = td.masks(cfg, ch)
        fx, fy = td.calib(ch)
        (px0, v0), (px1, v1) = ch["xcal"]; (py0, w0), (py1, w1) = ch["ycal"]
        tx = lambda v: px0 + (v - v0) * (px1 - px0) / (v1 - v0)
        ty = lambda w: py0 + (w - w0) * (py1 - py0) / (w1 - w0)
        rgb = im.convert("RGB"); dr = ImageDraw.Draw(rgb)
        hb = ch.get("erase_hyperbola")  # {"w": watts, "band": px}: erase the plate-dissipation curve Ia = W/Va
        if hb:
            X0, Y0, X1, Y1 = ch["box"]
            for xx in range(X0, X1):
                va = fx(xx)
                if va <= 1:
                    continue
                yy = ty(1000.0 * hb["w"] / va)
                yy2 = ty(1000.0 * hb["w"] / fx(xx + 1))
                band = hb.get("band", 8) * math.sqrt(1 + (yy2 - yy) ** 2)
                lo, hi = int(max(Y0, yy - band)), int(min(Y1, yy + band))
                if lo < hi:
                    o[lo:hi, xx] = False
        for cv in ch["curves"]:
            sx, sy = int(round(tx(cv["seed"][0]))), ty(cv["seed"][1])
            if cv.get("axis") == "y":  # steep segment: track along rows (transpose), limited to [ymin, ymax] in data units
                oo = o.T.copy()
                lo_, hi_ = sorted((ty(cv["ymax"]), ty(cv["ymin"])))
                oo[:, :int(lo_)] = False; oo[:, int(hi_):] = False
                bx = ch["box"]; tb = [bx[1], bx[0], bx[3], bx[2]]
                syy = int(round(sy)); sxx = float(sx)
                rr = [0.5 * (p + q) for p, q in runs_in(oo[:, syy])]
                if rr:
                    sxx = min(rr, key=lambda r: abs(r - sxx))
                cvv = dict(cv); cvv.pop("xmax", None); cvv.pop("xmin", None)
                pp = track(oo, syy, sxx, -1, ch, cvv, (fx, fy), tb)[::-1] + [(syy, sxx)] + track(oo, syy, sxx, 1, ch, cvv, (fx, fy), tb)
                pts = sorted([(b, a) for a, b in pp])
                pts = [(px, py) for px, py in pts]
                step = ch.get("step", 6)
                last = -1e9
                for py, px in pp:
                    if abs(py - last) < step:
                        continue
                    last = py
                    r = dict(source=cfg["source_id"], chart=ch["name"], quantity=cv.get("quantity", ch.get("quantity", "ia")), vg1=cv.get("vg1"), vg2=cv.get("vg2", ch.get("vg2", "")), va=round(fx(px), 2), i_ma=round(fy(py), 4))
                    if ch.get("x", "va") == "vg1":
                        r["vg1"] = round(fx(px), 3); r["va"] = cv.get("va", ch.get("va"))
                    rows.append(r)
                    dr.ellipse((px - 3, py - 3, px + 3, py + 3), outline=(0, 160, 0), width=2)
                print(ch["name"], "rows-mode", cv.get("vg1"), len(pp), "pts", "I %.1f..%.1f Va %.1f..%.1f" % (fy(pp[0][0]), fy(pp[-1][0]), fx(pp[0][1]), fx(pp[-1][1])) if pp else "")
                continue
            # snap seed to nearest run
            rr = [0.5 * (p + q) for p, q in runs_in(o[:, sx])]
            if rr:
                c = min(rr, key=lambda r: abs(r - sy))
                if abs(c - sy) < 25:
                    sy = c
            pts = track(o, sx, sy, -1, ch, cv, (fx, fy))[::-1] + [(sx, sy)] + track(o, sx, sy, 1, ch, cv, (fx, fy))
            step = ch.get("step", 6)
            last = -1e9
            for px, py in pts:
                if abs(px - last) < step:
                    continue
                last = px
                q = cv.get("quantity", ch.get("quantity", "ia"))
                r = dict(source=cfg["source_id"], chart=ch["name"], quantity=q, vg1=cv.get("vg1"), vg2=cv.get("vg2", ch.get("vg2", "")), va=round(fx(px), 2), i_ma=round(fy(py), 4))
                if ch.get("x", "va") == "vg1":
                    r["vg1"] = round(fx(px), 3); r["va"] = cv.get("va", ch.get("va"))
                rows.append(r)
                col = (255, 0, 0) if q == "ia" else (0, 0, 255)
                dr.ellipse((px - 3, py - 3, px + 3, py + 3), outline=col, width=2)
            print(ch["name"], cv.get("quantity", "ia"), cv.get("vg1"), cv.get("va", ""), len(pts), "px pts", "x %.1f..%.1f" % (fx(pts[0][0]), fx(pts[-1][0])) if pts else "")
        x0, y0, x1, y1 = ch["box"]
        rgb.crop((max(0, x0 - 20), max(0, y0 - 20), x1 + 20, y1 + 20)).save(os.path.join(td.SCRATCH, "extract_%s.png" % ch["name"]))
    keep = []
    if os.path.exists(out):
        for r in csv.DictReader(open(out)):
            if r["source"] == cfg["source_id"] and (not names or r["chart"] in names):
                continue
            keep.append(r)
    f = ["source", "chart", "quantity", "vg1", "vg2", "va", "i_ma"]
    with open(out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=f); w.writeheader()
        for r in keep + rows:
            w.writerow({k: r.get(k, "") for k in f})
    print(len(rows), "rows ->", out)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
