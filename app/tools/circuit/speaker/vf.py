import numpy as np
def basis(s, poles):
    cols = []
    for p in poles:
        if abs(p.imag) < 1e-9:
            cols.append(1 / (s - p.real))
        else:
            a, b = 1 / (s - p), 1 / (s - np.conj(p))
            cols += [a + b, 1j * a - 1j * b]
    return np.array(cols).T
def vectfit(s, H, poles, w, iters=12, d=True):
    for it in range(iters):
        Phi = basis(s, poles); n = Phi.shape[1]
        A = np.hstack([Phi * w[:, None], (np.ones((len(s), 1)) * w[:, None]) if d else np.zeros((len(s), 0)), -(H * w)[:, None] * Phi])
        b = H * w
        Ar = np.vstack([A.real, A.imag]); br = np.concatenate([b.real, b.imag])
        sc = np.linalg.norm(Ar, axis=0); sc[sc == 0] = 1
        x, *_ = np.linalg.lstsq(Ar / sc, br, rcond=None); x /= sc
        ct = x[n + (1 if d else 0):]
        # state-space of sigma: A, b, c~
        m = 0; Am = []; bv = []
        k = 0
        blocks = []
        for p in poles:
            if abs(p.imag) < 1e-9: blocks.append(("r", p.real)); 
            else: blocks.append(("c", p))
        dim = sum(1 if t == "r" else 2 for t, _ in blocks)
        Amat = np.zeros((dim, dim)); bvec = np.zeros(dim); i = 0
        for t, p in blocks:
            if t == "r":
                Amat[i, i] = p; bvec[i] = 1; i += 1
            else:
                Amat[i:i+2, i:i+2] = [[p.real, p.imag], [-p.imag, p.real]]; bvec[i] = 2; i += 2
        newp = np.linalg.eigvals(Amat - np.outer(bvec, ct))
        newp = np.where(newp.real > 0, -newp.real + 1j * newp.imag, newp)
        # keep one of each conjugate pair (imag >= 0)
        poles = sorted([p for p in newp if p.imag >= 0], key=lambda z: abs(z))
        # merge tiny imag into real
        poles = [complex(p.real, 0.0) if abs(p.imag) < 1e-6 * abs(p) else p for p in poles]
    Phi = basis(s, poles)
    A = np.hstack([Phi, np.ones((len(s), 1))]) if d else Phi
    A = A * w[:, None]
    Ar = np.vstack([A.real, A.imag]); br = np.concatenate([(H * w).real, (H * w).imag])
    x, *_ = np.linalg.lstsq(Ar, br, rcond=None)
    res = []; k = 0
    for p in poles:
        if abs(p.imag) < 1e-9: res.append(complex(x[k], 0)); k += 1
        else: res.append(complex(x[k], x[k + 1])); k += 2
    dd = x[k] if d else 0.0
    return poles, res, dd
def evalfit(s, poles, res, d):
    h = np.full(len(s), d, complex)
    for p, r in zip(poles, res):
        if abs(p.imag) < 1e-9: h += r.real / (s - p.real)
        else: h += r / (s - p) + np.conj(r) / (s - np.conj(p))
    return h
