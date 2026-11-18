"""Derive Headroom 'speaker' model cards (ModelLibrary.hpp) from published small-signal data + motor geometry
(docs/CIRCUIT_ENGINE.md section 10.21). All x in mm inside the polynomials. Usage: python3 spk_params.py cards.json;
then python3 emit_cards.py cards.json prints the C++ library entries."""
import json, math, sys
import numpy as np

RHO, C = 1.184, 343.0  # air, 25 C

# Published / measured small-signal data (sources in the report):
drivers = {
 # Celestion G12M-25 Greenback 8 ohm: Re 6.7, Fs 75 (Celestion sheet); Mms 29.22 g, Bl 12.14, Qms 9, Le 0.41 mH, Sd 525.1 cm2,
 # Xmax 1.2, Xmech 1.8 (measured set, Music Electronics Forum 'fyl' 2010, which read Fs 85 Hz). Cms refit to Fs 75.
 "G12M25_8":  dict(re=6.7, fs=75.0, mms=29.22e-3, bl=12.14, qms=9.0, le1k=0.41e-3, sd=525.1e-4, xmax=1.2, hg=6.0, prated=25),
 # 16 ohm: Re 13.1 (Celestion); same motor and cone, coil scaled: Bl ~ sqrt(Re), Le ~ Re.
 "G12M25_16": dict(re=13.1, fs=75.0, mms=29.22e-3, bl=12.14*math.sqrt(13.1/6.7), qms=9.0, le1k=0.41e-3*13.1/6.7, sd=525.1e-4, xmax=1.2, hg=6.0, prated=25),
 # Celestion Vintage 30 16 ohm: Fs 75, Re 12.9 (Celestion); Mms 29 g, Bl 16.4, Qms 9.2 from a forum measurement
 # attributed by its Re 12.91; Le, Sd, geometry assumed as the G12M (same 44 mm coil family).
 "V30_16":    dict(re=12.9, fs=75.0, mms=29.0e-3, bl=16.4, qms=9.2, le1k=0.80e-3, sd=525.1e-4, xmax=1.2, hg=6.0, prated=60),
 # Jensen P12Q 8 ohm (Jensen spec sheet): Re 5.6, Fs 90.5, Qms 11.57, Mms 26.8 g, Bl 5.88, Le 0.67 mH, Sd 490.9, Xmax 1,
 # winding depth 8 mm, gap depth 6 mm.
 "P12Q_8":    dict(re=5.6, fs=90.5, mms=26.8e-3, bl=5.88, qms=11.57, le1k=0.67e-3, sd=490.9e-4, xmax=1.0, hg=6.0, hc=8.0, prated=40),
 # Jensen C12N 8 ohm (Jensen spec sheet): Re 6.05, Fs 113, Qms 7.52, Mms 29.9 g, Bl 10.46, Le 0.9 mH, Sd 490.9, Xmax 1,
 # winding 10 mm, gap 8 mm.
 "C12N_8":    dict(re=6.05, fs=113.0, mms=29.9e-3, bl=10.46, qms=7.52, le1k=0.90e-3, sd=490.9e-4, xmax=1.0, hg=8.0, hc=10.0, prated=50),
 # Eminence Legend 1258 8 ohm (Eminence datasheet): Re 7.44, Fs 94, Qms 6.15, Mms 32 g, Bl 10.9, Le 0.70 mH, Sd 506.7,
 # Xmax 0.5, gap height 7.92 mm.
 "LEGEND1258_8": dict(re=7.44, fs=94.0, mms=32.0e-3, bl=10.9, qms=6.15, le1k=0.70e-3, sd=506.7e-4, xmax=0.5, hg=7.92, prated=75),
}

def bl_profile(x, hc, hg, ff, fb):
    """Coil [x-hc/2, x+hc/2] in a gap |z|<hg/2 with exponential fringes (front ff, back fb). Integral of B over coil."""
    def F(z):  # antiderivative of B(z)
        h = hg / 2
        if z <= -h: return -h - fb * (1 - math.exp((z + h) / fb)) - 0.0
        if z <= h: return z
        return h + ff * (1 - math.exp(-(z - h) / ff))
    # shift so F(-inf) offset cancels in the difference
    return F(x + hc / 2) - F(x - hc / 2)

def fit_card(name, d):
    re, fs, mms, bl, qms = d["re"], d["fs"], d["mms"], d["bl"], d["qms"]
    w = 2 * math.pi * fs
    kms = w * w * mms
    rms = w * mms / qms
    qes = w * mms * re / bl ** 2
    # LR-2 voice coil: Le + (L2 || R2), L2 = 2 Le, R2 = 3 Re (assumed split); Le from the datasheet Le at 1 kHz
    w1 = 2 * math.pi * 1000
    def leff(le):
        l2, r2 = 2 * le, 3 * re
        z = 1j * w1 * le + (1j * w1 * l2 * r2) / (r2 + 1j * w1 * l2)
        return z.imag / w1
    lo, hi = 1e-6, d["le1k"]
    for _ in range(100):
        mid = 0.5 * (lo + hi)
        if leff(mid) < d["le1k"]: lo = mid
        else: hi = mid
    le = 0.5 * (lo + hi)
    # Bl(x): geometry
    xmax, hg = d["xmax"], d["hg"]
    hc = d.get("hc", hg + 2 * xmax)
    ff, fb = 0.25 * hg, 0.40 * hg   # assumed fringe lengths (front shorter than back: pole-piece side)
    xf = (hc + hg) / 2
    xs = np.linspace(-xf, xf, 401)
    prof = np.array([bl_profile(x, hc, hg, ff, fb) for x in xs]) / bl_profile(0.0, hc, hg, ff, fb)
    wts = np.exp(-0.5 * (xs / (0.6 * xf)) ** 2) + 0.2
    A = np.vstack([xs ** k for k in range(1, 5)]).T
    b, *_ = np.linalg.lstsq(A * wts[:, None], (prof - 1.0) * wts, rcond=None)
    poly = lambda x: 1 + b[0] * x + b[1] * x ** 2 + b[2] * x ** 3 + b[3] * x ** 4
    # Klippel X_Bl (82 %)
    def first_below(sign, thr):
        x = 0.0
        while abs(x) < 20:
            x += sign * 0.001
            if poly(x) < thr: return x
        return float('nan')
    xbl = (first_below(-1, 0.82), first_below(1, 0.82))
    # Kms(x): ratio 1.33 at 1.5 Xmax (Klippel X_C criterion), 3.0 at 2.5 Xmax, +5 % asymmetry at X_C (assumed)
    xc, x3 = 1.5 * xmax, 2.5 * xmax
    M = np.array([[xc ** 2, xc ** 4], [x3 ** 2, x3 ** 4]])
    k2, k4 = np.linalg.solve(M, [0.33, 2.0])
    k1 = 0.05 / xc
    # Le(x): +17 % at -Xmax (coil in), -13 % at +Xmax (coil out) (Klippel-typical, assumed)
    l1 = -0.15 / xmax
    l2x = 0.02 / xmax ** 2
    # thermal: copper; 110 K steady rise at rated power (about 3 dB power compression), 70 % coil->magnet, 30 % magnet->air;
    # tau_v 8 s, tau_m 15 min (assumed)
    rt = 110.0 / d["prated"]
    rtv, rtm = 0.7 * rt, 0.3 * rt
    ctv, ctm = 8.0 / rtv, 900.0 / rtm
    vas = RHO * C * C * d["sd"] ** 2 / kms
    card = dict(re=re, le=le, l2=2 * le, r2=3 * re, bl=bl, mms=mms, kms=kms, rms=rms, sd=d["sd"],
                b1=b[0], b2=b[1], b3=b[2], b4=b[3], k1=k1, k2=k2, k4=k4, lx1=l1, lx2=l2x,
                alpha=0.00393, rtv=rtv, ctv=ctv, rtm=rtm, ctm=ctm)
    info = dict(qes=qes, qts=1 / (1 / qes + 1 / qms), vas_l=vas * 1e3, hc=hc, hg=hg, xbl=xbl,
                kms_at_xmax=1 + k1 * xmax + k2 * xmax ** 2 + k4 * xmax ** 4, bl_at_xmax=(poly(-xmax), poly(xmax)))
    return card, info

out = {}
for n, d in drivers.items():
    card, info = fit_card(n, d)
    out[n] = card
    print(n, {k: (round(v, 6) if isinstance(v, float) else v) for k, v in info.items()})
    print("   ", {k: float('%.5g' % v) for k, v in card.items()})
json.dump(out, open(sys.argv[1] if len(sys.argv) > 1 else "cards.json", "w"), indent=1)
