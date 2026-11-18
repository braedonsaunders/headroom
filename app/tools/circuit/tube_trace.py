"""Seed-based curve tracer for merged or dashed curves (chart key "traces" in a tube_digitise.py config): follows a
seed column by column / traces a riser from the origin. usage: tube_trace.py <cfg.json> <chart>  -> replaces that chart's
rows in the tube CSV; overlay in $TUBEFIT_SCRATCH. (Written for the RCA power-tube charts, work/tube-curves/.)"""
import sys, os, csv, math, json
import numpy as np
from PIL import ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tube_digitise as td

def inv(ch):
    (px0, v0), (px1, v1) = ch['xcal']; (py0, w0), (py1, w1) = ch['ycal']
    return (lambda v: px0 + (v - v0) * (px1 - px0) / (v1 - v0)), (lambda w: py0 + (w - w0) * (py1 - py0) / (w1 - w0))

def dark(o, x, y):
    xi, yi = int(round(x)), int(round(y))
    if yi < 1 or xi < 1 or yi >= o.shape[0] - 1 or xi >= o.shape[1] - 1: return False
    return o[yi - 1:yi + 2, xi - 1:xi + 2].any()

def recentre(o, x, y, d, maxw):
    nx, ny = -d[1], d[0]
    hits = [t for t in range(-maxw, maxw + 1) if dark(o, x + t * nx, y + t * ny)]
    if not hits: return x, y, False
    # contiguous run containing the hit closest to 0
    hits.sort(key=abs); c = hits[0]; run = [c]
    t = c
    while t + 1 in hits: t += 1; run.append(t)
    t = c
    while t - 1 in hits: t -= 1; run.append(t)
    if len(run) > maxw: return x, y, False
    m = (min(run) + max(run)) / 2
    return x + m * nx, y + m * ny, True

def trace(o, box, seed, d0, step=5, win=55, maxgap=45, maxw=9, stop=None, maxlen=6000):
    x, y = seed; d = np.array(d0, float); d /= np.linalg.norm(d)
    x, y, _ = recentre(o, x, y, d, maxw)
    pts = [(x, y)]
    L = 0
    while L < maxlen:
        best = None
        for r, w in ((step, win),) + tuple((rr, 18) for rr in range(step + 4, maxgap + 1, 4)):
            ang = math.atan2(d[1], d[0]); hits = []
            for k in range(-w, w + 1, 2):
                a = ang + math.radians(k)
                if dark(o, x + r * math.cos(a), y + r * math.sin(a)): hits.append(k)
            if hits:
                cl = []; cur = [hits[0]]
                for h in hits[1:]:
                    if h - cur[-1] <= 2: cur.append(h)
                    else: cl.append(cur); cur = [h]
                cl.append(cur)
                cen = min((sum(c) / len(c) for c in cl), key=abs)
                a = ang + math.radians(cen)
                best = (x + r * math.cos(a), y + r * math.sin(a)); break
        if best is None: break
        nx_, ny_ = best
        nd = np.array([nx_ - x, ny_ - y]); nd /= np.linalg.norm(nd)
        nx_, ny_, ok = recentre(o, nx_, ny_, nd, maxw)
        nd2 = np.array([nx_ - x, ny_ - y])
        if np.linalg.norm(nd2) > 0: nd = nd2 / np.linalg.norm(nd2)
        d = 0.5 * d + 0.5 * nd; d /= np.linalg.norm(d)
        L += math.hypot(nx_ - x, ny_ - y); x, y = nx_, ny_
        if not (box[0] < x < box[2] and box[1] < y < box[3]): break
        pts.append((x, y))
        if stop and stop(x, y): break
    return pts

def coltrack(o, box, sx, sy, step, stopx, tol, maxgap):
    x = int(round(sx)); hist = []; y = sy; gap = 0; pts = []
    while box[0] + 8 < x < box[2] - 8:
        if stopx is not None and ((step < 0 and x <= stopx) or (step > 0 and x >= stopx)): break
        if len(hist) >= 2:
            (xa, ya), (xb, yb) = hist[max(0, len(hist) - 30)], hist[-1]
            pred = yb + (yb - ya) / (xb - xa) * (x - xb) if xb != xa else yb
        else:
            pred = y
        idx = np.nonzero(o[:, x])[0]
        best = None
        if len(idx):
            runs = np.split(idx, np.nonzero(np.diff(idx) > 1)[0] + 1)
            cands = [((r[0] + r[-1]) / 2, len(r)) for r in runs if len(r) <= 12]
            if cands:
                c = min(cands, key=lambda c: abs(c[0] - pred))
                if abs(c[0] - pred) <= tol: best = c[0]
        if best is not None:
            hist.append((x, best)); pts.append((float(x), best)); gap = 0
        else:
            gap += 1
            if gap > maxgap: break
        x += step
    return pts

cfg = td.load_cfg(sys.argv[1]); ch = td.chart_by_name(cfg, sys.argv[2])
im, a, o = td.masks(cfg, ch)
if ch.get('trace_raw'): o = a.copy()
fx, fy = td.calib(ch); X, Y = inv(ch)
rgb = im.convert('RGB'); dr = ImageDraw.Draw(rgb)
rows = []
for tr in ch['traces']:
    sx, sy = X(tr['seed'][0]), Y(tr['seed'][1])
    stopva = tr.get('stop_va'); stopia = tr.get('stop_ia')
    stop = None
    if 'stop_ia_up' in tr:
        siu = tr['stop_ia_up']; stop = lambda x, y, siu=siu: fy(y) >= siu
    elif stopva is not None or stopia is not None:
        stop = lambda x, y: (stopva is not None and ((fx(x) <= stopva) if tr['dir'][0] < 0 else (fx(x) >= stopva))) or (stopia is not None and fy(y) <= stopia)
    oo = o
    if 'open' in tr:
        from scipy import ndimage as nd
        x0_, y0_, x1_, y1_ = ch['box']; m_ = ch.get('frame_margin', 6)
        oo = nd.binary_opening(a, structure=np.ones(tuple(tr['open'])))
        inner = np.zeros_like(a); inner[y0_ + m_:y1_ - m_, x0_ + m_:x1_ - m_] = True; oo &= inner
    if tr.get('mode') == 'col':
        pts = coltrack(oo, ch['box'], sx, sy, int(tr['dir'][0]), X(tr['stop_va']) if 'stop_va' in tr else None, tr.get('tol', 4), tr.get('maxgap', 60))
    else:
      pts = trace(oo, ch['box'], (sx, sy), tr['dir'], stop=stop, maxgap=tr.get('maxgap', 45), win=tr.get('win', 55), maxw=tr.get('maxw', 9), step=tr.get('step', 5))
    pts = td.order_and_resample(pts, ch.get('step', 12), ch.get('win', 8)) if tr.get('resample', True) else pts
    for px, py in pts:
        va, ia = fx(px), fy(py)
        if 'yscale' in tr: ia = tr['yscale'][1] * (ia - tr['yscale'][0])
        if 'va_range' in tr and not (tr['va_range'][0] <= va <= tr['va_range'][1]): continue
        rows.append(dict(source=cfg['source_id'], chart=ch['name'], quantity=tr.get('quantity', 'ia'), vg1=tr['vg1'], vg2=tr.get('vg2', ch.get('vg2', '')), va=round(va, 2), i_ma=round(ia, 4)))
        col = (0, 0, 255) if tr.get('quantity', 'ia') == 'ig2' else (255, 0, 0)
        dr.ellipse((px - 3, py - 3, px + 3, py + 3), outline=col, width=2)
    print(tr['quantity'] if 'quantity' in tr else 'ia', tr['vg1'], len(pts), 'pts, Va %.1f..%.1f' % (min(fx(p[0]) for p in pts), max(fx(p[0]) for p in pts)))
x0, y0, x1, y1 = ch['box']
rgb.crop((max(0, x0 - 20), max(0, y0 - 20), x1 + 20, y1 + 20)).save(os.path.join(td.SCRATCH, 'trace_%s.png' % ch['name']))
p = os.path.join(cfg['_dir'], cfg.get('csv', 'curves.csv'))
keep = []
if os.path.exists(p):
    keep = [r for r in csv.DictReader(open(p)) if not (r['source'] == cfg['source_id'] and r['chart'] == ch['name'])]
F = ['source', 'chart', 'quantity', 'vg1', 'vg2', 'va', 'i_ma']
with open(p, 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=F); w.writeheader()
    for r in keep + rows: w.writerow({k: r.get(k, '') for k in F})
print(len(rows), 'rows ->', p)
