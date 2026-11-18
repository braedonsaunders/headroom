"""Regenerates src/circuit/library/cabs.blocks.json (docs/CIRCUIT_ENGINE.md section 10.21).
Usage: SPK_CARDS=cards.json python3 make_cabs.py ../../../src/circuit/library/cabs.blocks.json [pole pairs, 24]
Needs numpy, scipy, soundfile and the CC0 Jester IRs (HEADROOM_CAB_IRS)."""
import json, os, numpy as np, sys
from cabfit import target, fit
from spkmodel import RHO, C, card
NP = int(sys.argv[2]) if len(sys.argv) > 2 else 24
CABS = [
 dict(id="cab-1960ax-g12m25-sm57", ir="jester-1960ax/4x12 G12M-25 SM57 UL A.wav", card="G12M25_16", vb=0.130,
      src="jester-emerald/Emerald Pack 1.0/Impulses/48kHz/1_Nacho_Guacamole_48.wav",
      desc="Marshall 1960AX 4x12 (1998) with Celestion G12M-25 Greenbacks, SM57 on the upper-left speaker",
      credit="Jester's Emerald Pack 1.0 by Jester Dyne Productions (Bastian Karschewski), CC0 1.0"),
 dict(id="cab-bg412s-v30-sm57", ir="jester-bg412s/4x12 V30 SM57 A.wav", card="V30_16", vb=0.130,
      src="jester-brutal/Jesters_Brutal_Pack_1.0/Impulses/48kHz/1_Cookie_Monster.wav",
      desc="Behringer BG412S 4x12 (modified, oversized) with Celestion Vintage 30s, SM57",
      credit="Jester's Brutal Pack 1.0 by Jester Dyne Productions (Bastian Karschewski), CC0 1.0"),
]
RBOX = 1.5   # N s/m per driver: box leakage / absorption (assumed, equal to the G12M's Rms)
OUT = 0.1    # 'out' = terminal volts convolved with the IR, x 0.1: about the speaker voltage in the midband (a cranked amp's
             # mic node then stays far below the unit's 1 kV smoke-test bound); outputVoltsPerFS x 0.1 keeps the IR path's level
blocks = {}
report = {}
for cab in CABS:
    c = card(cab["card"])
    kb = RHO * C * C * c["sd"] ** 2 / (cab["vb"] / 4)
    fr, H, h, T, Ts, Hmp, fs = target(cab["ir"], cab["card"], 2, 2, kb, RBOX)
    poles, res, d, f, err = fit(fr, Hmp, Ts, NP)
    band = (f > 60) & (f < 8000)
    report[cab["id"]] = dict(kbox_per_driver=kb, rms_err_db=float(np.sqrt(np.mean(err[band] ** 2))), max_err_db=float(np.max(np.abs(err[band]))),
                             poles=[[p.real, p.imag] for p in poles], res=[[r.real, r.imag] for r in res])
    Cv = 1e-6
    parts = [
        {"ref": "BOX", "type": "L", "nodes": ["vel", "0"], "value": float('%.6g' % (1.0 / (4 * kb))),
         "label": "closed-box air spring (mobility analogue: 1/K)", "note": "%.0f L internal shared by 4 drivers: K = rho c^2 Sd^2 / (V/4) = %.0f N/m per driver, x4 for the 2s2p equivalent driver (A: volume from the cabinet's outside dimensions)" % (cab["vb"] * 1e3, kb)},
        {"ref": "RBOX", "type": "R", "nodes": ["vel", "0"], "value": float('%.6g' % (1.0 / (4 * RBOX))),
         "label": "box leakage and absorption", "note": "%.1f N s/m per driver (A)" % RBOX},
        {"ref": "RSUM", "type": "R", "nodes": ["out", "0"], "value": 1.0, "label": "sum of the acoustic sections (1 ohm: out = sum of the section currents)"},
    ]
    for k, (p, r) in enumerate(zip(poles, res)):
        tag = "S%02d" % (k + 1)
        if abs(p.imag) < 1e-9:
            a = -p.real; g = a
            v = tag.lower()
            parts += [
                {"ref": tag + "C", "type": "C", "nodes": [v, "0"], "value": "1u"},
                {"ref": tag + "G", "type": "R", "nodes": [v, "0"], "value": float('%.10g' % (1 / (Cv * a)))},
                {"ref": tag + "I", "type": "vccs", "nodes": ["0", v, "vel", "0"], "gain": float('%.10g' % (Cv * g))},
                {"ref": tag + "O", "type": "vccs", "nodes": ["0", "out", v, "0"], "gain": float('%.10g' % (OUT * r.real / g))},
            ]
            continue
        sig, wd = p.real, p.imag
        a1 = -2 * sig; w0 = abs(p); g = a1
        b1 = 2 * r.real; b0 = -2 * (r.real * sig + r.imag * wd)
        v1, v2 = tag.lower() + "a", tag.lower() + "b"
        parts += [
            {"ref": tag + "CA", "type": "C", "nodes": [v1, "0"], "value": "1u", "note": "pole pair %.1f Hz, Q %.2f" % (w0 / 2 / np.pi, w0 / a1)},
            {"ref": tag + "CB", "type": "C", "nodes": [v2, "0"], "value": "1u"},
            {"ref": tag + "RD", "type": "R", "nodes": [v1, "0"], "value": float('%.10g' % (1 / (Cv * a1)))},
            {"ref": tag + "GI", "type": "vccs", "nodes": ["0", v1, "vel", "0"], "gain": float('%.10g' % (Cv * g))},
            {"ref": tag + "GF", "type": "vccs", "nodes": [v1, "0", v2, "0"], "gain": float('%.10g' % (Cv * w0))},
            {"ref": tag + "GR", "type": "vccs", "nodes": ["0", v2, v1, "0"], "gain": float('%.10g' % (Cv * w0))},
            {"ref": tag + "OA", "type": "vccs", "nodes": ["0", "out", v1, "0"], "gain": float('%.10g' % (OUT * b1 / g))},
            {"ref": tag + "OB", "type": "vccs", "nodes": ["0", "out", v2, "0"], "gain": float('%.10g' % (OUT * b0 / (g * w0)))},
        ]
    blocks[cab["id"]] = {
        "pins": ["vel", "out"],
        "note": ("%s. Acoustic path from cone velocity to the microphone, fitted to the open-licence IR %s (%s): the IR divided by the "
                 "small-signal cone velocity per volt of 4 x %s as one 2s2p equivalent driver in this box, smoothed to 1/24 octave, made minimum "
                 "phase and vector-fitted (Gustavsen-Semlyen) with %d stable pole pairs (%.2f dB rms, %.2f dB max error 60 Hz-8 kHz). "
                 "Attach 'vel' to the cone node of a speaker part with series x parallel = 4 (the box elements load it); 'out' is the mic "
                 "signal as speaker volts convolved with the IR, times 0.1 (about the speaker voltage in the midband), so a netlist moving "
                 "from the IR path keeps its level with outputVoltsPerFS x 0.1. "
                 "Each section is a two-integrator resonator (s = a1, w0) driven from 'vel', so the engine runs it as its own small linear "
                 "group and the SPICE export is exact.")
                % (cab["desc"], cab["src"], cab["credit"], cab["card"], len(poles), report[cab["id"]]["rms_err_db"], report[cab["id"]]["max_err_db"]),
        "parts": parts,
    }
    print(cab["id"], json.dumps({k: v for k, v in report[cab["id"]].items() if k not in ("poles", "res")}))
out = {"schema": "headroom.blocks.v1",
       "note": "Loudspeaker cabinets as circuits (docs/CIRCUIT_ENGINE.md section 10.21): a closed-box air spring on the speaker's cone node plus the acoustic path (cone breakup, cabinet, microphone) as a linear section bank fitted to CC0 cabinet IRs. Referenced as {\"type\":\"sub\",\"of\":\"lib:<name>\",\"nodes\":[<cone node>, <mic node>]}. Generated by tools/circuit/speaker/make_cabs.py; IR credits in each block.",
       "subcircuits": blocks}
json.dump(out, open(sys.argv[1], "w"), indent=1)
json.dump(report, open(os.environ.get("CAB_REPORT", "cabfit_report.json"), "w"), indent=1)
