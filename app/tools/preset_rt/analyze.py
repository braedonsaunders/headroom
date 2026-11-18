"""Tables for work/circuit-netlists/PRESET_REALTIME.md from preset_rt JSON lines (run_all.py output).

  analyze.py sweep.jsonl [repeats.jsonl]   -> ranked miss lists per buffer size, the unit table, every rig
"""
import json, sys, collections, os, statistics

FR = [64, 128, 256]


def load(path, tags=None):
    recs = collections.defaultdict(list)
    for l in open(path):
        r = json.loads(l)
        if tags and r.get("tag") not in tags:
            continue
        recs[r["path"]].append(r)
    return recs


def run_of(r, F):
    for x in r.get("runs", []):
        if x["frames"] == F:
            return x
    return None


def units_of(r, x):
    out = []
    for u in x["units"]:
        n = r["nodes"][u["i"]]
        if n["structural"] or n["utility"]:
            continue
        out.append(dict(u, name=n["name"], tid=n["tid"], lane=n["lane"], pct=100 * u["mean_us"] / x["period_us"],
                        p99pct=100 * u["p99_us"] / x["period_us"], maxpct=100 * u["max_us"] / x["period_us"]))
    return out


def lanes_cp(r, x, key="pct"):
    us = units_of(r, x)
    by_lane = collections.defaultdict(float)
    for u in us:
        by_lane[u["lane"]] += u[key]
    lanes = set(by_lane) | {n["lane"] for n in r["nodes"]}

    def cp(lane):
        t = by_lane.get(lane, 0.0)
        kids = [l for l in lanes if l.startswith(lane + "/") and "/" not in l[len(lane) + 1:]]
        splits = collections.defaultdict(list)
        for k in kids:
            tail = k[len(lane) + 1:]
            if tail.startswith("S"):
                splits[tail[:-1]].append(k)
            else:
                t += cp(k)
        for s, ks in splits.items():
            t += max(cp(k) for k in ks)
        return t

    # heaviest single lane (a chain that has to pipeline on its own)
    heaviest = max(by_lane.values()) if by_lane else 0.0
    return cp("top"), sum(u[key] for u in us), heaviest, by_lane


def short(p):
    return os.path.splitext(os.path.basename(p))[0]


def src_label(r):
    return {"showcase": "Showcase", "library": "Library/" + os.path.basename(os.path.dirname(r["path"])), "user": "User"}[r["source"]]


def top_units(r, x, k=3):
    us = sorted(units_of(r, x), key=lambda u: -u["mean_us"])[:k]
    return ", ".join(f"{u['name']} {u['pct']:.0f}%" for u in us)


def classify(r, x):
    """unit / depth / chain / load"""
    us = units_of(r, x)
    if not us:
        return "load", {}
    umax = max(u["pct"] for u in us)
    up99 = max(u["p99pct"] for u in us)
    cp, serial, heavy, _ = lanes_cp(r, x)
    info = dict(umax=umax, up99=up99, cp=cp, serial=serial, heavy=heavy)
    if umax >= 90:
        return "unit", info
    if up99 >= 100:
        return "unit-tail", info
    if heavy / 3 >= 85 or cp >= 100 and x["mean_pct"] >= 85:
        return "chain", info
    if x["mean_pct"] >= 80:
        return "chain", info
    return "load", info


def best_of(recs_list):
    """per frames: the repeat with the fewest misses (then lowest p99)"""
    out = {}
    for F in FR:
        cands = [(r, run_of(r, F)) for r in recs_list if run_of(r, F)]
        if cands:
            out[F] = min(cands, key=lambda c: (c[1]["missed"], c[1]["p99_pct"]))
    return out


def cell(x):
    return f"{x['mean_pct']:.0f} / {x['p99_pct']:.0f} / {x['max_pct']:.0f} / **{x['missed']}**" if x["missed"] else \
        f"{x['mean_pct']:.0f} / {x['p99_pct']:.0f} / {x['max_pct']:.0f} / 0"


def report(path, repeat_path=None):
    recs = load(path, {"sweep"})
    reps = load(repeat_path, None) if repeat_path and os.path.exists(repeat_path) else {}
    L = []
    allrows = []
    for p, rs in recs.items():
        r = rs[-1]
        allrows.append(r)
    order = {"showcase": 0, "library": 1, "user": 2}
    allrows.sort(key=lambda r: (order[r["source"]], r["path"]))
    # full table
    L.append("| # | Source | Rig | load 1m | 64: mean / p99 / max / miss | 128 | 256 | Top 3 units (% of one core, 256-frame run) |")
    L.append("|---|---|---|---|---|---|---|---|")
    for k, r in enumerate(allrows, 1):
        if "runs" not in r:
            L.append(f"| {k} | {src_label(r)} | {short(r['path'])} | | ERROR {r.get('error','')[:60]} | | | |")
            continue
        xs = {F: run_of(r, F) for F in FR}
        L.append(f"| {k} | {src_label(r)} | {short(r['path'])} | {xs[64]['load_before'][0]:.1f} | {cell(xs[64])} | {cell(xs[128])} | {cell(xs[256])} | {top_units(r, xs[256])} |")
    return allrows, "\n".join(L), recs, reps




def unit_table(rows):
    """per unit (table id): presets, worst in-graph mean and p99 % of one core at 64 and 256, and the per-block fit"""
    agg = collections.defaultdict(lambda: dict(name="", presets=set(), m64=0, p64=0, m256=0, p256=0, fits=[]))
    for r in rows:
        if "runs" not in r:
            continue
        per = {}
        for F in FR:
            x = run_of(r, F)
            for u in units_of(r, x):
                per.setdefault(u["i"], {})[F] = u
        for i, d in per.items():
            if 64 not in d or 256 not in d:
                continue
            a = agg[d[64]["tid"]]
            a["name"] = d[64]["name"]
            a["presets"].add(short(r["path"]))
            a["m64"] = max(a["m64"], d[64]["pct"])
            a["p64"] = max(a["p64"], d[64]["p99pct"])
            a["m256"] = max(a["m256"], d[256]["pct"])
            a["p256"] = max(a["p256"], d[256]["p99pct"])
            if 128 in d:
                xs = [64, 128, 256]
                ys = [d[F]["mean_us"] for F in xs]
                n = 3
                sx, sy = sum(xs), sum(ys)
                sxx = sum(v * v for v in xs)
                sxy = sum(xs[k] * ys[k] for k in range(3))
                b = (n * sxy - sx * sy) / (n * sxx - sx * sx)
                a0 = (sy - b * sx) / n
                a["fits"].append((d[256]["mean_us"], a0, b, d[64]["mean_us"]))
    return agg


def merged(sweep_path, rep_path):
    recs = load(sweep_path, {"sweep"})
    if rep_path and os.path.exists(rep_path):
        for p, rs in load(rep_path, None).items():
            # repeats ran on the copy of user presets at the same path
            recs[p].extend(rs)
    return recs


def best_cell(rs, F):
    """min-of-repeats by missed count; returns (run, all_missed list, loads list)"""
    xs = [run_of(r, F) for r in rs if run_of(r, F)]
    b = min(xs, key=lambda x: (x["missed"], x["p99_pct"]))
    return b, [x["missed"] for x in xs], [x["load_before"][0] for x in xs]


def final(sweep_path, rep_path):
    recs = merged(sweep_path, rep_path)
    order = {"showcase": 0, "library": 1, "user": 2}
    keys = sorted(recs, key=lambda p: (order[recs[p][0]["source"]], p))
    full = ["| # | Source | Rig | Runs | Load (1 min) | 64 frames: mean / p99 / max % / missed | 128 frames | 256 frames | Top 3 units, % of one core (64 / 256 frames) |",
            "|---|---|---|---|---|---|---|---|---|"]
    ranked = []
    for k, p in enumerate(keys, 1):
        rs = recs[p]
        r0 = rs[0]
        cells, loads = {}, []
        for F in FR:
            b, ms, ls = best_cell(rs, F)
            cells[F] = (b, ms)
            loads += ls
        x64 = run_of(r0, 64)
        x256 = run_of(r0, 256)
        u64 = {u["i"]: u for u in units_of(r0, x64)}
        top = sorted(units_of(r0, x256), key=lambda u: -u["mean_us"])[:3]
        tops = ", ".join(f"{u['name']} {u64[u['i']]['pct']:.0f} / {u['pct']:.0f}" for u in top if u["i"] in u64)

        def c(F):
            b, ms = cells[F]
            s = f"{b['mean_pct']:.0f} / {b['p99_pct']:.0f} / {b['max_pct']:.0f} / "
            s += (f"**{b['missed']}**" if b["missed"] else "0")
            if len(ms) > 1 and max(ms) != b["missed"]:
                s += f" ({'/'.join(str(m) for m in ms)})"
            return s
        lo = f"{min(loads):.0f}–{max(loads):.0f}" if len(rs) > 1 else f"{loads[0]:.0f}"
        full.append(f"| {k} | {src_label(r0)} | {short(p)} | {len(rs)} | {lo} | {c(64)} | {c(128)} | {c(256)} | {tops} |")
        ranked.append((p, r0, cells))
    return "\n".join(full), ranked


def pclass(r, x):
    us = units_of(r, x)
    mx = max(us, key=lambda u: u["pct"])
    mp = max(us, key=lambda u: u["p99pct"])
    if mx["pct"] >= 90:
        return "unit", mx
    if x["mean_pct"] >= 75 and x["mean_pct"] > 1.15 * mx["pct"]:
        return "chain", mx
    if mp["p99pct"] >= 100:
        return "unit tail", mp
    return "burst", mp


def verdict(b):
    if b["mean_pct"] >= 95:
        return "fails idle too"
    if b["max_pct"] <= 110:
        return "may pass idle"
    if b["max_pct"] <= 125:
        return "borderline"
    return "fails idle too"


def ranked_lists(ranked):
    out = {}
    for F in (256, 128, 64):
        rows = []
        for p, r0, cells in ranked:
            b, ms = cells[F]
            if b["missed"] == 0:
                continue
            x = run_of(r0, F)
            cl, u = pclass(r0, x)
            us = sorted(units_of(r0, x), key=lambda v: -v["mean_us"])
            resp = ", ".join(f"{v['name']} {v['pct']:.0f}% (p99 {v['p99pct']:.0f}%)" for v in us if v["pct"] >= 15 or v is us[0])
            rows.append((b["missed"], b["p99_pct"], f"| {short(p)} | {src_label(r0)} | {b['missed']} / {b['blocks']} | {b['mean_pct']:.0f} | {b['p99_pct']:.0f} | {b['max_pct']:.0f} | {cl} | {resp} | {verdict(b)} |"))
        rows.sort(key=lambda t: (-t[0], -t[1]))
        hdr = ["| Rig | Source | Missed (best run) | Mean % | p99 % | Max % | Class | Units responsible (% of one core, mean; p99) | Idle machine |",
               "|---|---|---|---|---|---|---|---|---|"]
        out[F] = "\n".join(hdr + [f"| {k} " + t[2] for k, t in enumerate(rows, 1)]).replace("| Rig |", "| # | Rig |", 1).replace("|---|---|---|---|---|---|---|---|---|", "|---|---|---|---|---|---|---|---|---|---|", 1)
        out[str(F) + "_n"] = len(rows)
    return out


def unit_md(rows, min_pct=3):
    agg = unit_table(rows)
    items = sorted(agg.items(), key=lambda kv: -kv[1]["m256"])
    L = ["| Unit | Table id | Rigs | Worst mean % of one core, 64 / 256 frames | Worst p99 %, 64 / 256 | Fixed cost per block (µs) | Per-frame cost (µs) | Fixed share at 64 frames | Alone over 90 %? |",
         "|---|---|---|---|---|---|---|---|---|"]
    for tid, a in items:
        if a["m256"] < min_pct and a["m64"] < min_pct:
            continue
        f = a["fits"]
        a0 = statistics.median([x[1] for x in f])
        b = statistics.median([x[2] for x in f])
        c64 = statistics.median([x[3] for x in f])
        share = 100 * a0 / c64 if c64 else 0
        over = "**yes**" if a["m64"] >= 90 or a["m256"] >= 90 else ("tail only (p99 ≥ 100 %)" if a["p64"] >= 100 else "")
        a0s = f"{a0:.1f}" if a0 >= 1.5 else "≈ 0"
        shs = f"{share:.0f} %" if abs(a0) >= 1.5 and share > 0 else "≈ 0"
        L.append(f"| {a['name']} | {tid} | {len(a['presets'])} | {a['m64']:.0f} / {a['m256']:.0f} | {a['p64']:.0f} / {a['p256']:.0f} | {a0s} | {b:.2f} | {shs} | {over} |")
    return "\n".join(L)


def main():
    if len(sys.argv) < 2:
        print("usage: analyze.py sweep.jsonl [repeats.jsonl]", file=sys.stderr)
        sys.exit(2)
    sweep = sys.argv[1]
    reps = sys.argv[2] if len(sys.argv) > 2 else None
    full, rk = final(sweep, reps)
    o = ranked_lists(rk)
    rows = [json.loads(l) for l in open(sweep) if json.loads(l).get("tag") == "sweep"]
    for F in (256, 128, 64):
        print(f"### Missed deadlines at {F} frames ({o[str(F) + '_n']} rigs)\n")
        print(o[F] + "\n")
    print("### Units\n")
    print(unit_md(rows) + "\n")
    print("### Every rig\n")
    print(full)


if __name__ == "__main__":
    main()
