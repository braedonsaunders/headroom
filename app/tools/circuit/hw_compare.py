#!/usr/bin/env python3
"""Hardware comparison tier: a circuit netlist against a NAM capture of the real amp, on the same DI.

  hw_compare.py <spec.json> --cli <build>/circuit_cli --nam <hw_nam_render> [--lib dir] [--json out.json] [--only name,...]

spec.json: list of {"name", "netlist" (file in src/circuit/library), "capture" (.nam path), "ctl": {id: value},
"sweep": {"ids": [control ids moved together], "values": [...]}, "input_level_dbu" (overrides the capture metadata;
null = uncalibrated), "note"}. Captures stay outside the repository (most public ones are not redistributable).

The same analytic test signal is generated at 96 kHz for the circuit (volts) and at the capture's rate (digital, with
1.0 = the peak of a sine at the capture's calibrated input level, input_level_dbu) and rendered through both:
  1. small-signal exponential sweep (-40 dBV)          -> linear response H(f) of each;
  2. 220 Hz level ladder, -40..+10 dBV peak            -> gain compression curve (fundamental gain vs input level,
                                                          re its small-signal value) and harmonic profile H2..H5 re
                                                          H1, each harmonic divided by the system's own linear
                                                          response ratio |H(k f0)|/|H(f0)| (removes a cab/mic that
                                                          follows the non-linearity: an approximation);
  3. 220 Hz burst, -20 dBV -> 0 dBV -> -20 dBV          -> gain envelope: sag/bias-shift after the step up and the
                                                          recovery after the step down (dB re the settled value);
  4. 3 s guitar DI at 0.5 V peak                        -> 1/3-octave long-term spectra, level-matched, and the same
                                                          divided by each system's small-signal response.
NAM captures are feed-forward networks with a receptive field of tens of milliseconds: they cannot hold supply sag or
bias shift beyond it, so (3) mostly tests our side's fast dynamics. Agreement is reported calibrated (the capture's stated input level) and with the best input-level shift (settings
and calibration are uncertain: the shift is reported, not hidden). Control sweeps pick the setting whose compression
curve matches best. Requires numpy and scipy.
"""
import json
import os
import subprocess
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LIB = os.path.join(ROOT, "src", "circuit", "library")
SCRATCH = os.environ.get("TUBEFIT_SCRATCH", "/tmp/tubefit")
F0 = 220.0
LEVELS = np.arange(-40.0, 10.01, 2.5)  # dBV peak
STEP_S, SETTLE_S = 0.3, 0.6
DI = os.path.join(ROOT, "samples", "di", "Rock DI.wav")


def segments(fs):
    """Test signal in volts at rate fs, plus the segment boundaries (samples)."""
    seg, x = {}, []
    n = lambda s: int(round(s * fs))
    x.append(np.zeros(n(SETTLE_S)))
    # 1. exponential sweep 20 Hz..20 kHz, 3 s, 10 mV peak, then 0.5 s silence
    T, f1, f2 = 3.0, 20.0, 20000.0
    t = np.arange(n(T)) / fs
    L = T / np.log(f2 / f1)
    sw = 0.01 * np.sin(2 * np.pi * f1 * L * (np.exp(t / L) - 1))
    fade = np.minimum(1, np.minimum(t / 0.01, (T - t) / 0.01))
    sw *= fade
    s0 = sum(map(len, x))
    x += [sw, np.zeros(n(0.5))]
    seg["sweep"] = (s0, s0 + len(sw) + n(0.5), sw)
    # 2. level ladder (phase-continuous, 5 ms ramps between steps)
    s0 = sum(map(len, x))
    m = n(STEP_S)
    amp = np.repeat(10 ** (LEVELS / 20), m)
    k = np.ones(n(0.005)) / n(0.005)
    amp = np.convolve(np.r_[np.full(len(k), amp[0]), amp], k, mode="same")[len(k):]
    tt = np.arange(len(amp)) / fs
    x.append(amp * np.sin(2 * np.pi * F0 * tt))
    seg["ladder"] = (s0, s0 + len(amp))
    x.append(np.zeros(n(0.3)))
    # 3. burst
    s0 = sum(map(len, x))
    a = np.r_[np.full(n(1.0), 0.1), np.full(n(1.5), 1.0), np.full(n(1.5), 0.1)]
    a = np.convolve(np.r_[np.full(len(k), a[0]), a], k, mode="same")[len(k):]
    tt = np.arange(len(a)) / fs
    x.append(a * np.sin(2 * np.pi * F0 * tt))
    seg["burst"] = (s0, s0 + len(a), a)
    x.append(np.zeros(n(0.3)))
    # 4. DI
    r0, d = wavfile.read(DI)
    d = d.astype(np.float64)
    d = d.mean(axis=1) if d.ndim > 1 else d
    d = d[int(0.5 * r0):int(3.5 * r0)]
    g = np.gcd(int(fs), r0)
    d = resample_poly(d, int(fs) // g, r0 // g)
    d *= 0.5 / np.max(np.abs(d))
    s0 = sum(map(len, x))
    x += [d, np.zeros(n(0.3))]
    seg["di"] = (s0, s0 + len(d))
    return np.concatenate(x), seg


def tone(y, fs, f, a, b):
    """Complex amplitude of frequency f over y[a:b] (Hann window)."""
    z = y[a:b]
    w = np.hanning(len(z))
    t = np.arange(a, b) / fs
    return 2 * np.sum(z * w * np.exp(-2j * np.pi * f * t)) / np.sum(w)


def analyse(y, fs, seg):
    out = {}
    # 1. linear response from the sweep (deconvolution by spectral division, smoothed to 1/6 octave)
    a, b, sw = seg["sweep"]
    Y = np.fft.rfft(y[a:b])
    X = np.fft.rfft(np.r_[sw, np.zeros(b - a - len(sw))])
    f = np.fft.rfftfreq(b - a, 1 / fs)
    H = np.abs(Y) / np.maximum(np.abs(X), 1e-12)
    fc = np.geomspace(30, 16000, 120)
    Hs = np.array([np.sqrt(np.mean(H[(f > c * 2 ** (-1 / 12)) & (f < c * 2 ** (1 / 12))] ** 2)) for c in fc])
    out["H_f"], out["H"] = fc, Hs
    Hat = lambda q: np.interp(np.log(q), np.log(fc), Hs)
    # 2. ladder
    a, b = seg["ladder"]
    m = int(round(STEP_S * fs))
    g, harm = [], []
    for i, L in enumerate(LEVELS):
        s, e = a + i * m + int(0.1 * fs), a + (i + 1) * m - int(0.01 * fs)
        h = [abs(tone(y, fs, k * F0, s, e)) for k in range(1, 6)]
        g.append(h[0] / 10 ** (L / 20))
        harm.append([20 * np.log10(max(h[k - 1], 1e-15) / max(h[0], 1e-15) / (Hat(k * F0) / Hat(F0))) for k in range(2, 6)])
    g = np.array(g)
    out["comp_db"] = 20 * np.log10(g / g[0])
    out["harm_db"] = np.array(harm)
    out["gain_ss"] = g[0]
    # 3. burst: gain envelope in 10 ms frames
    a, b, amp = seg["burst"]
    hop = int(0.01 * fs)
    env = []
    for s in range(a, b - 2 * hop, hop):
        env.append(abs(tone(y, fs, F0, s, s + 2 * hop)) / max(amp[s - a + hop], 1e-9))
    env = np.array(env)
    i_up, i_dn = int(1.0 / 0.01), int(2.5 / 0.01)
    hi_final = np.mean(env[i_dn - 15:i_dn - 3])
    lo_final = np.mean(env[-15:-3])
    out["sag_db"] = {str(ms): 20 * np.log10(env[i_up + ms // 10] / hi_final) for ms in (20, 100, 500)}
    out["recover_db"] = {str(ms): 20 * np.log10(env[i_dn + ms // 10] / lo_final) for ms in (20, 100, 500)}
    # 4. DI LTAS (1/3 octave), raw and divided by the small-signal response
    a, b = seg["di"]
    Pxx = np.abs(np.fft.rfft(y[a:b] * np.hanning(b - a))) ** 2
    fd = np.fft.rfftfreq(b - a, 1 / fs)
    bands = 1000 * 2 ** (np.arange(-15, 13) / 3)
    lt = np.array([np.mean(Pxx[(fd > c * 2 ** (-1 / 6)) & (fd < c * 2 ** (1 / 6))]) for c in bands])
    out["ltas_f"] = bands
    out["ltas_db"] = 10 * np.log10(lt + 1e-30)
    out["ltas_eq_db"] = out["ltas_db"] - 20 * np.log10(Hat(bands) + 1e-30)
    return out


def render_circuit(cli, netlist, ctl, x, fs):
    wi, wo = os.path.join(SCRATCH, "hw_in96.wav"), os.path.join(SCRATCH, "hw_out96.wav")
    wavfile.write(wi, int(fs), x.astype(np.float32))
    args = [cli, "render", os.path.join(LIB, netlist), "--rate", str(int(fs)), "--wav", wi, "--out", wo]
    for k, v in ctl.items():
        args += ["--ctl", "%s=%g" % (k, v)]
    subprocess.run(args, check=True, capture_output=True)
    r, y = wavfile.read(wo)
    os.remove(wi)
    os.remove(wo)
    return y.astype(np.float64)


def render_nam(nam, capture, x, fs):
    wi, wo = os.path.join(SCRATCH, "hw_in48.wav"), os.path.join(SCRATCH, "hw_out48.wav")
    wavfile.write(wi, int(fs), x.astype(np.float32))
    p = subprocess.run([nam, capture, wi, wo], check=True, capture_output=True, text=True)
    r, y = wavfile.read(wo)
    os.remove(wi)
    os.remove(wo)
    return y.astype(np.float64), p.stdout


def capture_meta(path):
    d = json.load(open(path))
    m = d.get("metadata") or {}
    rate = d.get("sample_rate") or 48000
    return float(rate), m.get("input_level_dbu")


def compare(ours, cap):
    """Compression-curve RMS difference (dB) calibrated and with the best level shift (whole 2.5 dB steps)."""
    res = {}
    c0, c1 = ours["comp_db"], cap["comp_db"]
    act = (c0 < -1.0) | (c1 < -1.0)  # where either compresses by more than 1 dB
    res["comp_rms_db"] = float(np.sqrt(np.mean((c0 - c1) ** 2)))
    best = (1e9, 0)
    for sh in range(-2, 3):  # shift the capture's level axis by up to +-5 dB (calibration / pickup-level slack)
        if sh >= 0:
            d = c0[sh:] - c1[:len(c1) - sh] if sh else c0 - c1
        else:
            d = c0[:sh] - c1[-sh:]
        r = float(np.sqrt(np.mean(d ** 2)))
        if r < best[0]:
            best = (r, sh)
    res["comp_rms_shifted_db"], res["shift_db"] = best[0], best[1] * 2.5
    # harmonic profile at three ladder levels (re each one's own fundamental, linear-response corrected)
    res["harm"] = {}
    for L in (-20.0, -10.0, 0.0):
        i = int(np.argmin(np.abs(LEVELS - L)))
        res["harm"]["%+.0f dBV" % L] = dict(ours=[round(float(v), 1) for v in ours["harm_db"][i]], capture=[round(float(v), 1) for v in cap["harm_db"][i]])
    res["sag_db"] = dict(ours={k: round(float(v), 2) for k, v in ours["sag_db"].items()}, capture={k: round(float(v), 2) for k, v in cap["sag_db"].items()})
    res["recover_db"] = dict(ours={k: round(float(v), 2) for k, v in ours["recover_db"].items()}, capture={k: round(float(v), 2) for k, v in cap["recover_db"].items()})
    sel = (ours["ltas_f"] >= 80) & (ours["ltas_f"] <= 6000)
    for key in ("ltas_db", "ltas_eq_db"):
        d = ours[key][sel] - cap[key][sel]
        d -= np.mean(d)
        res[key + "_rms"] = float(np.sqrt(np.mean(d ** 2)))
    return res


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    spec = json.load(open(a[0]))
    cli = a[a.index("--cli") + 1]
    nam = a[a.index("--nam") + 1]
    only = set(a[a.index("--only") + 1].split(",")) if "--only" in a else None
    global LIB
    if "--lib" in a:  # e.g. a variant library with refitted cards (tools/circuit/tube_impact.py)
        LIB = a[a.index("--lib") + 1]
    os.makedirs(SCRATCH, exist_ok=True)
    results = []
    for s in spec:
        if only and s["name"] not in only:
            continue
        rate, dbu = capture_meta(s["capture"])
        if "input_level_dbu" in s:
            dbu = s["input_level_dbu"]
        vfs = np.sqrt(2) * 0.7746 * 10 ** ((dbu if dbu is not None else 12.0) / 20)  # uncalibrated: assume 12 dBu
        x48, seg48 = segments(rate)
        y48, _ = render_nam(nam, s["capture"], x48 / vfs, rate)
        cap = analyse(y48, rate, seg48)
        x96, seg96 = segments(96000.0)
        best = None
        sweep = s.get("sweep") or {"ids": [], "values": [None]}
        rows = []
        for v in sweep["values"]:
            ctl = dict(s.get("ctl", {}))
            for cid in sweep["ids"]:
                ctl[cid] = v
            y96 = render_circuit(cli, s["netlist"], ctl, x96, 96000.0)
            ours = analyse(y96, 96000.0, seg96)
            c = compare(ours, cap)
            rows.append(dict(setting=v, **{k: c[k] for k in ("comp_rms_db", "comp_rms_shifted_db", "shift_db", "ltas_eq_db_rms")}))
            key = "comp_rms_db" if dbu is not None else "comp_rms_shifted_db"  # calibrated captures: no level slack
            if best is None or c[key] < best[0][key]:
                best = (c, v, ours)
        c, v, ours = best
        r = dict(name=s["name"], netlist=s["netlist"], capture=os.path.basename(s["capture"]), input_level_dbu=dbu,
                 calibrated=dbu is not None, best_setting=v, sweep=rows, **c,
                 comp_curve=dict(levels=LEVELS.tolist(), ours=np.round(ours["comp_db"], 2).tolist(), capture=np.round(cap["comp_db"], 2).tolist()))
        results.append(r)
        print("%-18s %-34s dBu %-6s best %s=%s  comp RMS %.2f dB (shifted %.2f dB, shift %+.1f dB)  LTAS RMS %.1f dB (eq %.1f dB)" % (
            s["name"], r["capture"][:34], dbu, "/".join(sweep["ids"]) or "-", v, c["comp_rms_db"], c["comp_rms_shifted_db"], c["shift_db"],
            c["ltas_db_rms"], c["ltas_eq_db_rms"]), flush=True)
        for L, h in c["harm"].items():
            print("    harmonics H2..H5 re H1 (dB, lin-corrected) at %s: ours %s  capture %s" % (L, h["ours"], h["capture"]))
        print("    sag after step up (dB at 20/100/500 ms): ours %s capture %s; recovery after step down: ours %s capture %s" % (
            c["sag_db"]["ours"], c["sag_db"]["capture"], c["recover_db"]["ours"], c["recover_db"]["capture"]))
        print("    sweep: " + "; ".join("%s: %.2f/%.2f dB" % (q["setting"], q["comp_rms_db"], q["comp_rms_shifted_db"]) for q in rows))
    if "--json" in a:
        json.dump(results, open(a[a.index("--json") + 1], "w"), indent=1)


if __name__ == "__main__":
    main()
