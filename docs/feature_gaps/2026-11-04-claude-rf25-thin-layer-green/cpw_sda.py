"""Quasi-static C per metre of a coplanar line of zero thickness (signal |x| < a, grounds b < |x| < c) on top of a stack
of dielectric slabs, nothing under it, air above: the spectral-domain method (Galerkin, Chebyshev bases with the edge's
1/sqrt singularity on each strip; even symmetry; the grounds held at 0 V and the signal at 1 V, the total charge nought).
The homogeneous part of the kernel - the interface average eps_avg = (1 + eps_top)/2 - is taken in the space domain in
closed form (same strip) or by Gauss-Chebyshev (strip to strip); what the stack adds decays in k and is integrated."""
import sys
import numpy as np
from scipy.special import jv, eval_chebyt, ellipk

EPS0 = 8.8541878128e-12

def reflection(k, slabs, above=1.0, below=1.0):
    eps = [e for _, e in slabs]; h = [t for t, _ in slabs]; n = len(slabs)
    up = (eps[-1] - above) / (eps[-1] + above)
    def under(j):
        u = eps[j - 1] if j > 0 else below
        return (eps[j] - u) / (eps[j] + u)
    rd = np.full_like(k, under(0))
    for j in range(1, n):
        e = np.exp(-2 * k * h[j - 1])
        rd = (under(j) + rd * e) / (1 + under(j) * rd * e)
    e = np.exp(-2 * k * h[-1])
    return (rd * e + up + 2 * up * rd * e) / (1 - up * rd * e), up

def capacitance(slabs, a, b, c, N=8, M=96):
    eps_top = slabs[-1][1]
    eps_avg = (1 + eps_top) / 2
    d, x0 = (c - b) / 2, (b + c) / 2
    # bases: ('s', n): T_2n(x/a)/sqrt(1-(x/a)^2) on |x|<a; ('g', n): h_n(|x|-x0), h_n(u) = T_n(u/d)/sqrt(1-(u/d)^2)
    bases = [('s', n) for n in range(N)] + [('g', n) for n in range(N)]
    nb = len(bases)
    Q = np.array([np.pi * a if (t == 's' and n == 0) else (2 * np.pi * d if (t == 'g' and n == 0) else 0.0) for t, n in bases])
    # --- the homogeneous part, -1/(2 pi eps0 eps_avg) Int Int b_i b_j ln|x-x'| ---
    kh = -1.0 / (2 * np.pi * EPS0 * eps_avg)
    u = np.cos((2 * np.arange(1, M + 1) - 1) * np.pi / (2 * M))   # Chebyshev nodes (weight pi/M)
    def pieces(t, n):
        """each piece of a basis: (centre, half-width, order, sign of its argument)"""
        if t == 's': return [(0.0, a, 2 * n, 1.0)]
        return [(x0, d, n, 1.0), (-x0, d, n, -1.0)]   # h_n(x - x0) and h_n(-x - x0) = T_n(-(x+x0)/d)...
    def same(width, m, n):
        if m != n: return 0.0
        return width * width * np.pi ** 2 * np.log(width / 2) if m == 0 else -width * width * np.pi ** 2 / (2 * m)
    Zh = np.zeros((nb, nb))
    for i, (ti, ni) in enumerate(bases):
        for j, (tj, nj) in enumerate(bases):
            total = 0.0
            for (ci, wi, mi, si) in pieces(ti, ni):
                for (cj, wj, mj, sj) in pieces(tj, nj):
                    if ci == cj:
                        total += same(wi, mi, mj) * (si ** mi) * (sj ** mj)
                    else:
                        xi = ci + wi * u; xj = cj + wj * u
                        Ti = eval_chebyt(mi, si * u); Tj = eval_chebyt(mj, sj * u)
                        L = np.log(np.abs(xi[:, None] - xj[None, :]))
                        total += wi * wj * (np.pi / M) ** 2 * (Ti @ L @ Tj)
            Zh[i, j] = kh * total
    # --- what the stack adds: (1/pi) Int (G(k) - Ginf(k)) B_i B_j dk, G = (1 + R)/(2 eps0 eps_top k) ---
    _, rinf = reflection(np.array([1.0]), slabs)
    r0, _ = reflection(np.array([1e-9]), slabs)
    r0 = float(r0[0])
    zmin = 2 * slabs[-1][0]
    kmax = 80.0 / zmin
    big = sum(t for t, _ in slabs)
    edges = np.unique(np.concatenate([np.geomspace(1e-4 / big, kmax, 4000), np.arange(0, kmax, np.pi / (4 * c))]))
    xg, wg = np.polynomial.legendre.leggauss(16)
    mid = (edges[1:] + edges[:-1]) / 2; half = (edges[1:] - edges[:-1]) / 2
    k = (mid[:, None] + half[:, None] * xg[None, :]).ravel(); w = (half[:, None] * wg[None, :]).ravel()
    R, _ = reflection(k, slabs)
    L = 10 * big
    dG = (R - rinf) / (2 * EPS0 * eps_top * k)
    sing = (r0 - rinf) / (2 * EPS0 * eps_top) * np.exp(-k * L) / k
    B = np.zeros((nb, k.size))
    for i, (t, n) in enumerate(bases):
        B[i] = (np.pi * a * (-1) ** n * jv(2 * n, k * a)) if t == 's' else (2 * d * np.pi * jv(n, k * d) * np.cos(k * x0 + n * np.pi / 2))
    Zs = (B * (w * dG)) @ B.T / np.pi - np.outer(Q, Q) * np.sum(w * sing) / np.pi
    Z = Zh + Zs
    # --- solve: Z c = (V0 + v_i) Q_i, sum c Q = 0 ---
    A = np.zeros((nb + 1, nb + 1)); rhs = np.zeros(nb + 1)
    A[:nb, :nb] = Z; A[:nb, nb] = -Q; A[nb, :nb] = Q
    for i, (t, n) in enumerate(bases):
        if t == 's': rhs[i] = Q[i]
    sol = np.linalg.solve(A, rhs)
    return float(sum(sol[i] * Q[i] for i, (t, n) in enumerate(bases) if t == 's'))

a, b, c = 25e-6, 31e-6, 181e-6
kk = (a / b) * np.sqrt((1 - b * b / (c * c)) / (1 - a * a / (c * c)))
exact0 = 4 * EPS0 * ellipk(kk * kk) / ellipk(1 - kk * kk)
air = [(5e-6, 1.0)]
for N in (4, 8, 12):
    print(f"vacuum, N {N}: C0 {capacitance(air, a, b, c, N) * 1e12:.6f} pF/m; conformal mapping {exact0 * 1e12:.6f}")
tfln = [(500e-6, 11.7), (4.7e-6, 3.9), (0.3e-6, 28.0), (0.5e-6, 3.9)]
for N in (4, 8, 12):
    C = capacitance(tfln, a, b, c, N)
    print(f"TFLN, N {N}: C {C * 1e12:.6f} pF/m, eps_eff {C / exact0:.6f}")
two = [(500e-6, 11.7), (5.5e-6, 3.9)]
print(f"Si + 5.5 um SiO2, N 8: C {capacitance(two, a, b, c, 8) * 1e12:.6f} pF/m")
print(f"TFLN LN as SiO2 (4 slabs), N 8: C {capacitance([(500e-6, 11.7), (4.7e-6, 3.9), (0.3e-6, 3.9), (0.5e-6, 3.9)], a, b, c, 8) * 1e12:.6f} pF/m")
