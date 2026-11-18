#!/usr/bin/env python3
"""Impact of refitted valve cards on the bound amp netlists (companion to tools/circuit/tube_fit.py).

  tube_impact.py <fits.json> --cli <build>/circuit_cli [--only 12AX7,EL34] [--amps id,...] [--di file.wav] [--json out]

For every netlist in src/circuit/library that uses a refitted card, a variant is written to $TUBEFIT_SCRATCH with
the new card as a netlist-local model (local models override the built-in library, so nothing in the tree is touched)
and both are compared with circuit_cli at the netlist's default controls:
  * DC operating point: the largest node-voltage change (V and % of the node's value) and the valve plate/cathode
    nodes' changes;
  * 220 Hz sines at 0.03, 0.3 and 3 V peak: fundamental level change (dB) and THD (old vs new);
  * a 3 s guitar DI at 0.5 V peak: null residual of new against old, dB relative to the old output RMS.
Requires numpy and scipy.
"""
import json
import os
import re
import subprocess
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LIB = os.path.join(ROOT, "src", "circuit", "library")
SCRATCH = os.environ.get("TUBEFIT_SCRATCH", "/tmp/tubefit")
RATE = 96000
FIT_KEYS = ["mu", "ex", "kg1", "kg2", "kp", "kvb", "gscale", "gknee"]


_LIB = None


def library_card(name):
    """Every parameter of a built-in card (ModelLibrary.hpp), via tube_fit's parser."""
    global _LIB
    if _LIB is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import tube_fit
        _LIB = tube_fit.library_cards()
    return dict(_LIB[name]["p"])


def tubes_in(net):
    out = set()
    for p in net.get("parts", []):
        if p.get("type") in ("triode", "pentode"):
            out.add(p.get("model"))
    return out


def variant(net, fits, only):
    v = json.loads(json.dumps(net))
    models = v.setdefault("models", {})
    changed = []
    for name in sorted(tubes_in(net)):
        if name not in fits or (only and name not in only):
            continue
        f = fits[name]
        base = dict(models.get(name) or {})
        if not base:  # built-in card: start from the full library entry (capacitances included)
            base = {"kind": f["kind"]}
            base.update(library_card(name))
        for k in FIT_KEYS:
            if k in f["new"]:
                base[k] = f["new"][k]
        base["note"] = "tube_impact variant (refitted card)"
        models[name] = base
        changed.append(name)
    return v, changed


def cli(args, exe):
    r = subprocess.run([exe] + args, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("circuit_cli %s failed: %s" % (" ".join(args[:2]), r.stderr[-400:]))
    return r.stdout


def dc_points(path, exe):
    out = cli(["info", path], exe)
    d = {}
    on = False
    for line in out.splitlines():
        if line.startswith("DC operating point"):
            on = True
            continue
        if on:
            m = re.match(r"\s+(\S+)\s+([-+0-9.eE]+) V", line)
            if m:
                d[m.group(1)] = float(m.group(2))
    return d


def render(path, exe, extra, out):
    cli(["render", path, "--rate", str(RATE), "--out", out] + extra, exe)
    r, y = wavfile.read(out)
    os.remove(out)
    return y.astype(np.float64), r


def harmonics(y, r, f0, secs=0.25):
    n = int(secs * r)
    n -= n % int(round(r / f0)) if r % f0 == 0 else 0
    x = y[-n:] - np.mean(y[-n:])
    t = np.arange(n) / r
    amps = []
    for h in range(1, 8):
        c = np.exp(-2j * np.pi * h * f0 * t)
        amps.append(2 * abs(np.dot(x, c)) / n)
    a = np.array(amps)
    return a[0], float(np.sqrt(np.sum(a[1:] ** 2)) / max(a[0], 1e-30))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    fits_raw = json.load(open(a[0]))
    exe = a[a.index("--cli") + 1]
    only = set(a[a.index("--only") + 1].split(",")) if "--only" in a else None
    amps = set(a[a.index("--amps") + 1].split(",")) if "--amps" in a else None
    di = a[a.index("--di") + 1] if "--di" in a else os.path.join(ROOT, "samples", "di", "Rock DI.wav")
    fits = {}
    for f in fits_raw:
        if "new" not in f:
            continue
        fits[f["tube"]] = dict(kind=f["kind"], new=f["new"]["card"], old_full=f["old"]["card"])
    os.makedirs(SCRATCH, exist_ok=True)
    # DI: 3 s from 0.5 s in, mono, 48 kHz float, 0.5 V peak
    r0, x = wavfile.read(di)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    x = x[int(0.5 * r0):int(3.5 * r0)]
    if r0 != 48000:
        g = np.gcd(48000, r0)
        x = resample_poly(x, 48000 // g, r0 // g)
    x *= 0.5 / np.max(np.abs(x))
    di48 = os.path.join(SCRATCH, "impact_di.wav")
    wavfile.write(di48, 48000, x.astype(np.float32))
    results = []
    for fn in sorted(os.listdir(LIB)):
        if not fn.endswith(".circuit.json"):
            continue
        net = json.load(open(os.path.join(LIB, fn)))
        if amps and net["id"] not in amps:
            continue
        v, changed = variant(net, fits, only)
        if not changed:
            continue
        vp = os.path.join(SCRATCH, "impact_" + fn)
        json.dump(v, open(vp, "w"), indent=1)
        op = os.path.join(LIB, fn)
        res = dict(id=net["id"], file=fn, changed=changed)
        try:
            d0, d1 = dc_points(op, exe), dc_points(vp, exe)
            worst = max(((abs(d1[k] - d0[k]), k) for k in d0 if k in d1), default=(0, ""))
            res["dc_max_dv"] = dict(node=worst[1], old=d0.get(worst[1]), new=d1.get(worst[1]), dv=worst[0])
            sines = []
            for amp in (0.03, 0.3, 3.0):
                y0, r = render(op, exe, ["--sine", "220", str(amp), "0.6"], os.path.join(SCRATCH, "i0.wav"))
                y1, _ = render(vp, exe, ["--sine", "220", str(amp), "0.6"], os.path.join(SCRATCH, "i1.wav"))
                h0, t0 = harmonics(y0, r, 220.0)
                h1, t1 = harmonics(y1, r, 220.0)
                sines.append(dict(amp_v=amp, d_fund_db=20 * np.log10(h1 / max(h0, 1e-30)), thd_old_pct=100 * t0, thd_new_pct=100 * t1))
            res["sines"] = sines
            y0, r = render(op, exe, ["--wav", di48], os.path.join(SCRATCH, "i0.wav"))
            y1, _ = render(vp, exe, ["--wav", di48], os.path.join(SCRATCH, "i1.wav"))
            s = int(0.3 * r)
            e0 = y0[s:] - np.mean(y0[s:])
            e1 = y1[s:] - np.mean(y1[s:])
            res["di_level_db"] = 20 * np.log10(np.std(e1) / max(np.std(e0), 1e-30))
            res["di_null_db"] = 20 * np.log10(np.std(e1 - e0) / max(np.std(e0), 1e-30))
            g = np.dot(e0, e1) / max(np.dot(e1, e1), 1e-30)  # level-matched residual
            res["di_null_matched_db"] = 20 * np.log10(np.std(g * e1 - e0) / max(np.std(e0), 1e-30))
        except Exception as ex:  # report and continue
            res["error"] = str(ex)
        os.remove(vp)
        results.append(res)
        s = res.get("sines", [])
        print("%-32s %-22s DC max dV %7.2f V (%s %.1f->%.1f)  sine dB %s  THD%% %s  DI level %+.2f dB null %.1f dB (matched %.1f dB)%s" % (
            res["id"], ",".join(changed), res.get("dc_max_dv", {}).get("dv", float("nan")), res.get("dc_max_dv", {}).get("node", ""),
            res.get("dc_max_dv", {}).get("old") or 0, res.get("dc_max_dv", {}).get("new") or 0,
            "/".join("%+.2f" % q["d_fund_db"] for q in s), "/".join("%.1f>%.1f" % (q["thd_old_pct"], q["thd_new_pct"]) for q in s),
            res.get("di_level_db", float("nan")), res.get("di_null_db", float("nan")), res.get("di_null_matched_db", float("nan")),
            "  ERROR " + res["error"] if "error" in res else ""), flush=True)
    os.remove(di48)
    if "--json" in a:
        json.dump(results, open(a[a.index("--json") + 1], "w"), indent=1)


if __name__ == "__main__":
    main()
