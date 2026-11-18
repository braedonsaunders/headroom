#!/usr/bin/env python3
"""PGO training DI (docs/CIRCUIT_ENGINE.md §10.54): GuitarSet clips the `bench --real` timing set does not use.

training_di.py <app/samples/di> <outdir>: writes 48 kHz 16-bit mono clips of 10 s, peak-normalised to 0.35 FS as
bench --real normalises its DI, plus a "driven" copy of each through bench --real's fuzz pre-stage (tanh(12x), 6 kHz
one-pole), so training sees clean, crunch and pre-driven playing on material held out from the measurement.
"""
import math, os, struct, sys, wave

CLIPS = ["005 Rock Solo 1 · 130 BPM · A · player 1.wav", "009 Rock Comping 2 · 142 BPM · D · player 1.wav",
         "025 Funk Comping 1 · 114 BPM · Ab · player 1.wav"]
HOST = 48000

def read(path):
    w = wave.open(path)
    n, ch, sw, rate = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    assert sw == 2, path
    raw = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    return [raw[i * ch] / 32768.0 for i in range(n)], rate

def write(path, x):
    w = wave.open(path, "wb")
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(HOST)
    w.writeframes(struct.pack("<%dh" % len(x), *[max(-32767, min(32767, round(v * 32767))) for v in x]))

def main(src, out):
    os.makedirs(out, exist_ok=True)
    for k, name in enumerate(CLIPS):
        w, rate = read(os.path.join(src, "GuitarSet Comps & Solos", name))
        frames = int(len(w) * HOST / rate)
        x = []
        for i in range(frames):  # linear resample to 48 kHz, as bench --real does
            s = i * rate / HOST; i0 = min(int(s), len(w) - 1); i1 = min(i0 + 1, len(w) - 1)
            x.append(w[i0] + (s - i0) * (w[i1] - w[i0]))
        off, end = 2 * HOST, min(12 * HOST, frames)
        peak = max(abs(v) for v in x[off:end]) or 1.0
        clean = [v * 0.35 / peak for v in x[off:end]]
        a, lp, driven = 1.0 - math.exp(-2.0 * math.pi * 6000.0 / HOST), 0.0, []
        for v in clean:
            lp += a * (math.tanh(12.0 * v) - lp); driven.append(lp)
        write(os.path.join(out, "train%d.wav" % k), clean)
        write(os.path.join(out, "train%d-driven.wav" % k), driven)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
