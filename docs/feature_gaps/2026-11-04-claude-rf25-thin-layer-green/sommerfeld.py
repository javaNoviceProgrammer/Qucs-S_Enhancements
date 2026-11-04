"""Independent reference for the static Green's function of a layered stack: the Sommerfeld
integral of the reflection, by a vectorized recursion and Gauss-Legendre between Bessel zeros,
graded near k = 0, up to where the reflection less its k->inf tail is below 1e-17."""
import sys
import numpy as np
from scipy.special import j0, jn_zeros

EPS0 = 8.8541878128e-12

def reflection(k, slabs, ground, above=1.0, below=1.0):
    """R(k) for a charge on the top face of the top slab: what Green::spectralReflection gives there."""
    eps = [e for _, e in slabs]; h = [t for t, _ in slabs]; n = len(slabs)
    up = (eps[-1] - above) / (eps[-1] + above)            # the top face: the charge is on it
    def under(j):
        u = eps[j - 1] if j > 0 else below
        return (eps[j] - u) / (eps[j] + u)
    rd = np.full_like(k, -1.0 if ground else under(0))
    for j in range(1, n):
        e = np.exp(-2 * k * h[j - 1])
        rd = (under(j) + rd * e) / (1 + under(j) * rd * e)
    hs = h[-1]
    # same slab, u = us = hs: numerator rd e^{-2k hs} + ru + 2 ru rd e^{-2k hs}, over 1 - ru rd e^{-2k hs}
    e = np.exp(-2 * k * hs)
    return (rd * e + up + 2 * up * rd * e) / (1 - up * rd * e), up

def reflected(rho, slabs, ground, kc_scale):
    kk = np.array([1.0])
    _, tail = reflection(kk, slabs, ground)
    f = lambda k: reflection(k, slabs, ground)[0] - tail
    kc = 60.0 / kc_scale
    # graded edges near 0 plus Bessel zeros
    big = max(t for t, _ in slabs)
    decades = np.log10(kc / (1e-5 / big))
    graded = np.geomspace(1e-5 / big, kc, int(decades * 100) + 1)
    nz = int(np.ceil(kc * rho / np.pi)) + 2
    zeros = jn_zeros(0, min(nz, 4_000_000)) / rho
    edges = np.unique(np.concatenate([[0.0], graded, zeros]))
    edges = edges[edges <= kc]
    edges = np.append(edges, kc)
    x, w = np.polynomial.legendre.leggauss(20)
    total = 0.0
    chunk = 200000
    parts = []
    for s in range(0, len(edges) - 1, chunk):
        a = edges[s:s + chunk]; b = edges[s + 1:s + chunk + 1]
        m = min(len(a), len(b)); a = a[:m]; b = b[:m]
        mid = (a + b) / 2; half = (b - a) / 2
        k = mid[:, None] + half[:, None] * x[None, :]
        vals = (f(k.ravel()).reshape(k.shape) * j0(k * rho)) @ w * half
        parts.append(vals)
    vals = np.concatenate(parts)
    total = np.sum(vals[::-1])
    eps_src = slabs[-1][1]
    return (total + tail / rho) / (4 * np.pi * EPS0 * eps_src), len(edges)

stacks = {
    "0": ([(500e-6, 11.7), (4.7e-6, 3.9), (0.3e-6, 28.0), (0.5e-6, 3.9)], False, 1e-6),
    "4": ([(500e-6, 11.7), (5.5e-6, 3.9)], False, 11e-6),
}
which = sys.argv[1]
slabs, ground, scale = stacks[which]
for rho in [float(r) for r in sys.argv[2:]]:
    v, n = reflected(rho, slabs, ground, scale)
    print(f"case {which} rho {rho:.1e}: {v:.10e}  ({n} edges)")
