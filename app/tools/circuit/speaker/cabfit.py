"""Fit the acoustic part of a cab IR (cone velocity -> mic pressure) as a stable s-domain section bank."""
import numpy as np, soundfile as sf, json, sys
from spkmodel import *
from vf import *
import os
# The normalised CC0 cab IRs (work/ir-library/norm/cabs in the research checkout; not shipped)
IRD = os.environ.get("HEADROOM_CAB_IRS", os.path.join(os.path.dirname(__file__), "../../../../work/ir-library/norm/cabs")) + "/"
def target(irfile, cardname, s_, p_, kbox, rbox, smooth_oct=1/24):
    x, fs = sf.read(IRD + irfile, always_2d=True); x = x[:, 0]
    N = 1 << 16
    H = np.fft.rfft(x, N); fr = np.fft.rfftfreq(N, 1 / fs)
    c = card(cardname)
    h, z = hu(np.maximum(fr, 1e-3), c, s_, p_, kbox, rbox)
    T = np.abs(H) / np.abs(h)
    # fractional-octave power smoothing on the linear grid
    P = T ** 2; cs = np.concatenate([[0], np.cumsum(P)])
    Ts = np.empty_like(T)
    for i, f in enumerate(fr):
        lo = f * 2 ** (-smooth_oct / 2); hi = f * 2 ** (smooth_oct / 2)
        a = int(np.floor(lo / fr[1])); b = max(int(np.ceil(hi / fr[1])), a + 1)
        b = min(b, len(P)); a = min(a, b - 1)
        Ts[i] = np.sqrt((cs[b] - cs[a]) / (b - a))
    # extrapolate outside 25 Hz .. 18 kHz with 12 dB/oct slopes
    f0, f1 = 25.0, 18000.0
    i0 = np.searchsorted(fr, f0); i1 = np.searchsorted(fr, f1)
    Ts[:i0] = Ts[i0] * (np.maximum(fr[:i0], 1.0) / fr[i0]) ** 2
    Ts[i1:] = Ts[i1] * (fr[i1] / fr[i1:]) ** 2
    # minimum phase via the real cepstrum
    L = np.log(np.maximum(Ts, 1e-12))
    full = np.concatenate([L, L[-2:0:-1]])
    cep = np.fft.ifft(full).real
    n = len(full); fold = np.zeros(n); fold[0] = cep[0]; fold[1:n // 2] = 2 * cep[1:n // 2]; fold[n // 2] = cep[n // 2]
    Hmp = np.exp(np.fft.fft(fold))[: len(fr)]
    return fr, H, h, T, Ts, Hmp, fs
def fit(fr, Hmp, Ts, npairs, fmin=12, fmax=23900):
    sel = np.unique(np.searchsorted(fr, np.geomspace(fmin, fmax, 700)))
    f = fr[sel]; s = 2j * np.pi * f; Ht = Hmp[sel]
    beta = 2 * np.pi * np.geomspace(40, 16000, npairs)
    poles = list(-beta / 50 + 1j * beta)
    w = 1 / np.abs(Ht) * np.where((f > 50) & (f < 9000), 1.0, 0.3)
    poles, res, d = vectfit(s, Ht, poles, w, iters=20, d=False)
    hf = evalfit(s, poles, res, d)
    err = 20 * np.log10(np.abs(hf) / np.abs(Ht))
    return poles, res, d, f, err
if __name__ == "__main__":
    fr, H, h, T, Ts, Hmp, fs = target("jester-1960ax/4x12 G12M-25 SM57 UL A.wav", "G12M25_16", 2, 2, 11818.0, 1.5)
    for npairs in [16, 20, 24, 28, 32]:
        poles, res, d, f, err = fit(fr, Hmp, Ts, npairs)
        band = (f > 60) & (f < 8000); b2 = (f > 30) & (f < 16000)
        print(npairs, "pairs: rms err %.2f dB, max %.2f dB (60 Hz-8 kHz); max %.2f dB (30 Hz-16 kHz); real poles %d; max f0 %.0f Hz" % (
            np.sqrt(np.mean(err[band] ** 2)), np.max(np.abs(err[band])), np.max(np.abs(err[b2])), sum(abs(p.imag) < 1e-9 for p in poles), max(abs(p) for p in poles) / 2 / np.pi))
