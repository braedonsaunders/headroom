#!/usr/bin/env python3
"""Fit the circuit engine's valve cards to digitised plate characteristics (docs/CIRCUIT_ENGINE.md §4).

  tube_fit.py report <TUBE> [--card k=v,...]     error of the current card (ModelLibrary.hpp or the netlist-local one)
  tube_fit.py fit    <TUBE> [--free mu,ex,...] [--fix k=v,...] [--json out.json]
  tube_fit.py all    [--json out.json] [--emit]  report + fit every tube that has data (and fit.json); --emit prints
                                                 the fitted cards as ModelLibrary.hpp-style {"key", value} fragments
  Triode "vgo": contact-potential offset added to the grid voltage in the plate law (engine card parameter since
  2026-11-10; 0 = Koren's law). "gdiv" (plate current reduced by gdiv * Ig) is an evaluation-only extension, not in
  the engine.

Data: work/tube-curves/<TUBE>/curves.csv (columns source, chart, quantity, vg1, vg2, va, i_ma; quantity is ia, ig1
or ig2), digitised from the manufacturers' published plate families with tools/circuit/tube_digitise.py; each
source's config JSON (same folder) carries the citation. The laws below are the engine's, term for term
(src/circuit/Devices.hpp, NlKind::Triode / NlKind::Pentode; the SPICE exporter writes the same equations):

  grid    Ig  = gscale * (gknee * softplus(vgk / gknee))^1.5
  triode  E1  = vpk/kp * softplus(kp * (1/mu + vgk / sqrt(kvb + vpk^2)));    Ia = 2/kg1 * E1^ex
  pentode E1  = vg2/kp * softplus(kp * (1/mu + vg1/vg2));  Q = E1^ex
          Ia  = 2/kg1 * Q * atan(vpk/kvb);   Ig2 = Q * (1/kg2 + 2/kg1 * (pi/2 - atan(vpk/kvb)))

Residuals use a mixed tolerance, sigma = tol_abs * Iref + tol_rel * |I|, so the bias and cut-off region (small
currents) and the high-current region both count; Iref is the largest plate current in the reference source.
Secondary sources (a single measured tube, e.g. a published curve-tracer set) get their own perveance scale factor
(fitted, reported), so they inform the shape (grid conduction, low plate voltage) without moving the average
datasheet perveance. Errors are reported as RMS in mA and as % of Iref, per region:

  triode : neg_grid (vg <= 0, va >= 50), low_va (vg <= 0, va < 50), pos_grid (vg > 0, plate), grid (Ig)
  pentode: plate (va >= knee), knee (va < knee = 0.25*vg2), pos_grid (vg1 > 0, plate), screen (Ig2), grid (Ig1)
  both   : table (the sheet's tabulated typical points, when fit.json sets anchor_weight)

Requires numpy and scipy.
"""
import csv
import glob
import json
import math
import os
import re
import sys

import numpy as np
from scipy.optimize import least_squares

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))  # app/
DATA = os.path.abspath(os.path.join(ROOT, "..", "work", "tube-curves"))
TRIODE_KEYS = ["mu", "ex", "kg1", "kp", "kvb", "gscale", "gknee"]
PENTODE_KEYS = ["mu", "ex", "kg1", "kg2", "kp", "kvb", "gscale", "gknee"]


def softplus(z):
    return np.maximum(z, 0) + np.log1p(np.exp(-np.abs(z)))


def grid_law(p, vg):
    return p["gscale"] * (p["gknee"] * softplus(vg / p["gknee"])) ** 1.5


def triode_ia(p, vg, va):
    vg = vg + p.get("vgo", 0.0)  # proposed law extension (contact-potential offset); not in the engine: keep 0 for cards
    q = np.sqrt(p["kvb"] + va * va)
    e1 = va / p["kp"] * softplus(p["kp"] * (1.0 / p["mu"] + vg / q))
    return np.where(e1 > 0, 2.0 / p["kg1"] * np.maximum(e1, 0) ** p["ex"], 0.0)


def pentode_q(p, vg1, vg2):
    vg1 = vg1 + p.get("vgo", 0.0)  # proposed law extension, see triode_ia
    vg2 = np.maximum(vg2, 1e-3)
    e1 = vg2 / p["kp"] * softplus(p["kp"] * (1.0 / p["mu"] + vg1 / vg2))
    return np.maximum(e1, 0) ** p["ex"]


def pentode_ia(p, vg1, vg2, va):
    return 2.0 / p["kg1"] * pentode_q(p, vg1, vg2) * np.arctan(va / p["kvb"])


def pentode_ig2(p, vg1, vg2, va):
    return pentode_q(p, vg1, vg2) * (1.0 / p["kg2"] + 2.0 / p["kg1"] * (0.5 * math.pi - np.arctan(va / p["kvb"])))


# ───────────────────────── cards ─────────────────────────
def library_cards():
    """Built-in cards from ModelLibrary.hpp plus netlist-local cards (first netlist that defines a name)."""
    cards = {}
    src = open(os.path.join(ROOT, "src", "circuit", "ModelLibrary.hpp"), encoding="utf-8").read()
    for m in re.finditer(r'\{"([^"]+)", "(triode|pentode)", \{(.*?)\},\s*\n', src):
        p = {k: float(v) for k, v in re.findall(r'\{"(\w+)", ([-\d.e+]+)\}', m.group(3))}
        cards[m.group(1)] = dict(kind=m.group(2), p=p, where="ModelLibrary.hpp")
    for f in sorted(glob.glob(os.path.join(ROOT, "src", "circuit", "library", "*.circuit.json"))):
        d = json.load(open(f, encoding="utf-8"))
        for name, m in (d.get("models") or {}).items():
            if isinstance(m, dict) and m.get("kind") in ("triode", "pentode") and name not in cards:
                p = {k: float(v) for k, v in m.items() if isinstance(v, (int, float))}
                cards[name] = dict(kind=m["kind"], p=p, where=os.path.basename(f))
    for c in cards.values():  # engine defaults for omitted grid-law terms
        c["p"].setdefault("gscale", 6.2e-4 if c["kind"] == "triode" else 1.5e-3)
        c["p"].setdefault("gknee", 0.06 if c["kind"] == "triode" else 0.08)
    return cards


# ───────────────────────── data ─────────────────────────
def load_data(tube):
    path = os.path.join(DATA, tube, "curves.csv")
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            try:
                float(r.get("vg2") or 0)
            except ValueError:
                continue  # e.g. triode-connected pentode charts (vg2 = "triode"): not used by the pentode fit
            rows.append(dict(source=r["source"], chart=r["chart"], q=r["quantity"], vg1=float(r["vg1"]),
                             vg2=float(r["vg2"]) if r.get("vg2") not in (None, "") else float("nan"),
                             va=float(r["va"]), i=float(r["i_ma"]) * 1e-3))
    cites = {}
    for cj in glob.glob(os.path.join(DATA, tube, "*.json")):
        try:
            c = json.load(open(cj))
            cites[c["source_id"]] = c.get("citation", "")
        except Exception:
            pass
    meta_p = os.path.join(DATA, tube, "fit.json")
    meta = json.load(open(meta_p)) if os.path.exists(meta_p) else {}
    # The sheet's tabulated typical operating points are published data too: with "anchor_weight" they join the fit
    # (same source as the reference curves, region "table").
    if meta.get("anchor_weight"):
        for c in meta.get("checks", []):
            for q, key in (("ia", "ia_ma_sheet"), ("ig2", "ig2_ma_sheet")):
                if key in c:
                    rows.append(dict(source=meta.get("reference"), chart="table", q=q, vg1=float(c["vg1"]),
                                     vg2=float(c.get("vg2", float("nan"))), va=float(c["va"]), i=float(c[key]) * 1e-3, anchor=True))
    return rows, cites, meta


def region(kind, r):
    if r.get("anchor"):
        return "table"
    if r["q"] == "ig1":
        return "grid"
    if kind == "triode":
        if r["vg1"] > 0:
            return "pos_grid"
        return "low_va" if r["va"] < 50 else "neg_grid"
    if r["q"] == "ig2":
        return "screen"
    if r["vg1"] > 0:
        return "pos_grid"
    return "knee" if r["va"] < 0.25 * r["vg2"] else "plate"


def model_i(kind, p, r):
    vg, va = np.array([x["vg1"] for x in r]), np.array([x["va"] for x in r])
    q = np.array([x["q"] for x in r])
    out = np.zeros(len(r))
    g = q == "ig1"
    out[g] = grid_law(p, vg[g])
    if kind == "triode":
        m = q == "ia"
        out[m] = triode_ia(p, vg[m], va[m])
        if p.get("gdiv", 0.0):  # proposed law extension: the grid takes its current out of the cathode stream
            out[m] = np.maximum(out[m] - p["gdiv"] * grid_law(p, vg[m]), 0.0)
    else:
        vg2 = np.array([x["vg2"] for x in r])
        m = q == "ia"
        out[m] = pentode_ia(p, vg[m], vg2[m], va[m])
        m = q == "ig2"
        out[m] = pentode_ig2(p, vg[m], vg2[m], va[m])
    return out


class Problem:
    def __init__(self, tube, kind, rows, meta):
        self.tube, self.kind, self.rows, self.meta = tube, kind, rows, meta
        self.ref = meta.get("reference") or rows[0]["source"]
        refia = [r["i"] for r in rows if r["source"] == self.ref and r["q"] == "ia"]
        self.iref = max(refia) if refia else max(r["i"] for r in rows)
        self.tol_abs, self.tol_rel = meta.get("tol_abs", 0.01), meta.get("tol_rel", 0.05)
        self.secondary = sorted({r["source"] for r in rows if r["source"] != self.ref and r["q"] in ("ia", "ig2")})
        self.reg = [region(kind, r) for r in rows]
        wts = dict(meta.get("weights", {}))
        wts.setdefault("table", meta.get("anchor_weight", 1.0))
        self.w = np.array([wts.get(g, 1.0) for g in self.reg])
        self.I = np.array([r["i"] for r in rows])
        # grid-current points get their own absolute scale (they are ~10x smaller than plate currents)
        gi = [r["i"] for r in rows if r["q"] == "ig1"]
        self.gref = max(gi) if gi else 1.0
        self.isgrid = np.array([r["q"] == "ig1" for r in rows])
        self.istable = np.array([bool(r.get("anchor")) for r in rows])
        self.src_idx = np.array([self.secondary.index(r["source"]) if (r["source"] in self.secondary and r["q"] != "ig1") else -1 for r in rows])

    def predict(self, p, scales=None):
        # scales = [s_0, dv_0, s_1, dv_1, ...]: per secondary source, perveance scale and grid offset (plate/screen
        # currents only; tube-to-tube spread is mostly perveance and contact potential)
        if scales is None:
            return model_i(self.kind, p, self.rows)
        y = model_i(self.kind, p, self.rows)
        for k in range(len(self.secondary)):
            m = self.src_idx == k
            if m.any():
                sub = [dict(r, vg1=r["vg1"] + scales[2 * k + 1]) for r, mm in zip(self.rows, m) if mm]
                y[m] = scales[2 * k] * model_i(self.kind, p, sub)
        return y

    def residual(self, p, scales=None):
        y = self.predict(p, scales)
        sig = np.where(self.isgrid, self.tol_abs * self.gref, self.tol_abs * self.iref) + self.tol_rel * np.abs(self.I)
        sig = np.where(self.istable, 0.03 * np.abs(self.I) + 1e-4, sig)  # tabulated points: 3 % (their stated precision)
        return self.w * (y - self.I) / sig

    def errors(self, p, scales=None):
        y = self.predict(p, scales)
        out = {}
        for g in sorted(set(self.reg)):
            m = np.array([x == g for x in self.reg])
            e = y[m] - self.I[m]
            base = self.gref if g == "grid" else self.iref
            out[g] = dict(n=int(m.sum()), rms_ma=float(np.sqrt(np.mean(e * e)) * 1e3), rms_pct=float(100 * np.sqrt(np.mean(e * e)) / base))
        # reference-source-only plate errors (the datasheet average), same regions
        return out


def fit(prob, p0, free, fixed):
    keys = [k for k in free if k not in fixed]
    p = dict(p0)
    p.update(fixed)
    ns = len(prob.secondary)
    lo0 = {"gdiv": 0.0, "vgo": -3.0, "mu": 1, "ex": 1.0, "kg1": 10, "kg2": 10, "kp": 1, "kvb": 0.05, "gscale": 1e-6, "gknee": 0.005}
    hi0 = {"gdiv": 3.0, "vgo": 3.0, "mu": 500, "ex": 2.0, "kg1": 1e5, "kg2": 1e6, "kp": 5000, "kvb": 1e5, "gscale": 1e-1, "gknee": 2.0}

    lo, hi = dict(lo0), dict(hi0)
    for k, (a, b) in prob.meta.get("bounds", {}).items():  # fit.json: {"bounds": {"ex": [1.2, 1.6]}}
        lo[k], hi[k] = a, b
    dvmax = prob.meta.get("source_offset_max", 1.0)

    def unpack(x):
        q = dict(p)
        for k, v in zip(keys, x[:len(keys)]):
            q[k] = v if k in ("vgo", "gdiv") else math.exp(v)
        if not ns:
            return q, None
        t = x[len(keys):]
        return q, np.ravel([[math.exp(t[2 * j]), t[2 * j + 1]] for j in range(ns)])

    p.setdefault("vgo", 0.0)
    p.setdefault("gdiv", 0.0)
    tr = lambda k, v: v if k in ("vgo", "gdiv") else math.log(v)
    x0 = np.array([tr(k, min(max(p[k], lo[k] * 1.0001), hi[k] * 0.9999)) for k in keys] + [0.0] * (2 * ns))
    lb = np.array([tr(k, lo[k]) for k in keys] + [math.log(0.3), -dvmax] * ns)
    ub = np.array([tr(k, hi[k]) for k in keys] + [math.log(3.0), dvmax] * ns)

    def f(x):
        q, s = unpack(x)
        return prob.residual(q, s)

    best = None
    for start in (x0, x0 + 0.3, x0 - 0.3):
        st = np.clip(start, lb + 1e-6, ub - 1e-6)
        st[len(keys):] = 0.0
        r = least_squares(f, st, bounds=(lb, ub), x_scale=1.0, max_nfev=4000)
        if best is None or r.cost < best.cost:
            best = r
    q, s = unpack(best.x)
    return q, s


def fmt_card(p, keys):
    return ", ".join('{"%s", %s}' % (k, ("%.4g" % p[k])) for k in keys)


def run(tube, args):
    cards = library_cards()
    if tube not in cards:
        raise SystemExit("no card %s" % tube)
    card = cards[tube]
    rows, cites, meta = load_data(tube)
    if "--weight" in args:  # region=w,... (overrides fit.json "weights")
        meta = dict(meta, weights=dict(meta.get("weights", {}), **{kv.split("=")[0]: float(kv.split("=")[1]) for kv in args[args.index("--weight") + 1].split(",")}))
    prob = Problem(tube, card["kind"], rows, meta)
    keys = TRIODE_KEYS if card["kind"] == "triode" else PENTODE_KEYS
    old = dict(card["p"])
    if "--card" in args:
        for kv in args[args.index("--card") + 1].split(","):
            k, v = kv.split("=")
            old[k] = float(v)
    _, s_old = fit(prob, old, [], {k: old[k] for k in keys}) if prob.secondary else (None, None)
    res = dict(tube=tube, kind=card["kind"], where=card["where"], reference=prob.ref, iref_ma=prob.iref * 1e3,
               sources={k: cites.get(k, "") for k in sorted({r["source"] for r in rows})},
               old=dict(card={k: old[k] for k in keys}, errors=prob.errors(old, s_old),
                        scales={n: dict(scale=float(s_old[2 * j]), dvg=float(s_old[2 * j + 1])) for j, n in enumerate(prob.secondary)} if s_old is not None else {}))
    if args and args[0] == "fit" or "--fit" in args:
        free = meta.get("free") or list(keys)
        if "--free" in args:
            free = args[args.index("--free") + 1].split(",")
        if not any(r["q"] == "ig1" for r in rows):
            free = [k for k in free if k not in ("gscale", "gknee")]
        fixed = dict(meta.get("fixed", {}))
        if "--fix" in args:
            for kv in args[args.index("--fix") + 1].split(","):
                k, v = kv.split("=")
                fixed[k] = float(v)
        new, s_new = fit(prob, old, free, fixed)
        res["new"] = dict(card={k: new[k] for k in keys + [k for k in ("vgo", "gdiv") if k in free]}, errors=prob.errors(new, s_new), free=free,
                          scales={n: dict(scale=float(s_new[2 * j]), dvg=float(s_new[2 * j + 1])) for j, n in enumerate(prob.secondary)} if s_new is not None else {})
        # datasheet typical point(s) from fit.json: {"checks": [{"vg1":..,"va":..,"vg2":..,"ia_ma":..,"ig2_ma":..}]}
        chk = []
        for c in meta.get("checks", []):
            row = dict(c)
            for tag, pp in (("old", old), ("new", new)):
                if card["kind"] == "triode":
                    f = lambda g, a: float(triode_ia(pp, np.array(g), np.array(a)))
                    row[tag + "_ia_ma"] = f(c["vg1"], c["va"]) * 1e3
                    gm = (f(c["vg1"] + 0.01, c["va"]) - f(c["vg1"] - 0.01, c["va"])) / 0.02
                    gp = (f(c["vg1"], c["va"] + 0.5) - f(c["vg1"], c["va"] - 0.5)) / 1.0
                    row[tag + "_gm_mA_V"], row[tag + "_rp_k"], row[tag + "_mu"] = gm * 1e3, 1e-3 / gp, gm / gp
                else:
                    row[tag + "_ia_ma"] = float(pentode_ia(pp, np.array(c["vg1"]), np.array(c["vg2"]), np.array(c["va"]))) * 1e3
                    row[tag + "_ig2_ma"] = float(pentode_ig2(pp, np.array(c["vg1"]), np.array(c["vg2"]), np.array(c["va"]))) * 1e3
            chk.append(row)
        res["checks"] = chk
    return res


def show(res):
    print("== %s (%s, card in %s); reference %s, Iref %.2f mA" % (res["tube"], res["kind"], res["where"], res["reference"], res["iref_ma"]))
    for k, v in res["sources"].items():
        print("   source %s: %s" % (k, v[:140]))
    regs = sorted(res["old"]["errors"])
    print("   %-10s %5s %18s %18s" % ("region", "n", "old rms mA (%)", "new rms mA (%)"))
    for g in regs:
        o = res["old"]["errors"][g]
        n = res.get("new", {}).get("errors", {}).get(g)
        print("   %-10s %5d %10.4f (%5.2f%%) %s" % (g, o["n"], o["rms_ma"], o["rms_pct"], "%10.4f (%5.2f%%)" % (n["rms_ma"], n["rms_pct"]) if n else ""))
    if res["old"]["scales"]:
        print("   secondary-source scale (old card): %s" % res["old"]["scales"])
    if "new" in res:
        print("   new card: " + ", ".join("%s=%.5g" % kv for kv in res["new"]["card"].items()) + "   (free: %s)" % ",".join(res["new"]["free"]))
        if res["new"]["scales"]:
            print("   secondary-source scale (new card): %s" % res["new"]["scales"])
        for c in res.get("checks", []):
            print("   check " + ", ".join("%s=%s" % (k, ("%.3g" % v) if isinstance(v, float) else v) for k, v in c.items()))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    out = None
    if "--json" in a:
        out = a[a.index("--json") + 1]
    results = []
    if a[0] == "all":
        for d in sorted(os.listdir(DATA)):
            if os.path.exists(os.path.join(DATA, d, "curves.csv")) and os.path.exists(os.path.join(DATA, d, "fit.json")):
                results.append(run(d, ["fit"] + a[1:]))
    else:
        results.append(run(a[1], [a[0]] + a[2:]))
    for r in results:
        show(r)
    if out:
        json.dump(results, open(out, "w"), indent=1)
    if "--emit" in a:  # card parameters as ModelLibrary.hpp / netlist "models" fragments (fitted keys only)
        for r in results:
            if "new" in r:
                c = r["new"]["card"]
                print("%s (%s): %s" % (r["tube"], r["where"], ", ".join('{"%s", %.5g}' % (k, c[k]) for k in c if k != "gdiv")))


if __name__ == "__main__":
    main()
