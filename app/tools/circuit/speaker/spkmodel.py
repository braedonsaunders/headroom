"""Small-signal speaker model shared by the cab fit (docs/CIRCUIT_ENGINE.md section 10.21).
Cards come from spk_params.py (SPK_CARDS, default ./cards.json)."""
import numpy as np, json, os
RHO, C = 1.184, 343.0
def card(name):
    return json.load(open(os.environ.get("SPK_CARDS", "cards.json")))[name]
def hu(f, c, s=1, p=1, kbox=0.0, rbox=0.0, rsrc=0.0):
    """Cone velocity per volt at the cab terminals (equivalent driver), continuous time. kbox/rbox per driver."""
    w = 2j * np.pi * np.asarray(f, float)
    e = s / p; m = s * p
    re, le, l2, r2 = c["re"] * e, c["le"] * e, c["l2"] * e, c["r2"] * e
    bl = c["bl"] * s
    ze = rsrc + re + w * le + (w * l2 * r2) / (r2 + w * l2)
    zm = m * (w * c["mms"] + c["rms"] + (c["kms"] + kbox) / w + rbox)
    return bl / (ze * zm + bl * bl), ze + bl * bl / zm
