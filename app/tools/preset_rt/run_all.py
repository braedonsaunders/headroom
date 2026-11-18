#!/usr/bin/env python3
"""Runs preset_rt over every rig, one at a time, and appends one JSON line per rig to the output file.

  run_all.py --bin build-rt/preset_rt --root <repo> --data <isolated data dir> --out results.jsonl
             [--only name1,name2] [--frames 64,128,256] [--seconds 10] [--tag run1]

Rigs: app/presets/Showcase, app/presets/Library/**, and <data>/User Presets (a COPY of the user's folder).
HEADROOM_DATA_DIR is always set to --data; the tool refuses to run without it.
"""
import argparse, json, os, subprocess, sys, time, glob

DI = ["Crunch DI.wav", "Rock DI.wav", "Metal DI.wav", "Lead DI.wav", "Clean Strumming.wav"]


def rigs(root, data):
    out = []
    for p in sorted(glob.glob(os.path.join(root, "app/presets/Showcase/*.rig"))):
        out.append(("showcase", p))
    for p in sorted(glob.glob(os.path.join(root, "app/presets/Library/**/*.rig"), recursive=True)):
        out.append(("library", p))
    for p in sorted(glob.glob(os.path.join(data, "User Presets/*.rig"))):
        out.append(("user", p))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--frames", default="64,128,256")
    ap.add_argument("--seconds", default="10")
    ap.add_argument("--warmup", default="2")
    ap.add_argument("--tag", default="sweep")
    ap.add_argument("--extra", default="")
    a = ap.parse_args()
    env = dict(os.environ, HEADROOM_DATA_DIR=a.data)
    di = ",".join(os.path.join(a.root, "app/samples/di", d) for d in DI)
    only = [s for s in a.only.split("|") if s]
    todo = rigs(a.root, a.data)
    if only:
        todo = [(s, p) for s, p in todo if any(o == os.path.splitext(os.path.basename(p))[0] for o in only)]
    for k, (src, p) in enumerate(todo):
        cmd = [a.bin, "--root", a.root, "--rig", p, "--di", di, "--frames", a.frames, "--seconds", a.seconds, "--warmup", a.warmup]
        if a.extra:
            cmd += a.extra.split()
        t0 = time.time()
        la = os.getloadavg()
        try:
            r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=900)
            line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
            rec = json.loads(line) if line.startswith("{") else {"error": (r.stderr or "")[-400:], "rc": r.returncode}
        except Exception as e:  # noqa
            rec = {"error": str(e)}
        rec.update({"source": src, "path": p, "tag": a.tag, "wall_s": round(time.time() - t0, 1), "loadavg_start": la})
        with open(a.out, "a") as f:
            f.write(json.dumps(rec) + "\n")
        brief = " ".join(f"{x['frames']}:{x['mean_pct']:.0f}/{x['p99_pct']:.0f}/{x['max_pct']:.0f} miss={x['missed']}" for x in rec.get("runs", []))
        print(f"[{k + 1}/{len(todo)}] {src} {os.path.basename(p)} la={la[0]:.1f} {brief} {rec.get('error', '')[:120]}", flush=True)


if __name__ == "__main__":
    main()
