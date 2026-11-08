"""2D Laplace (finite differences) of two coplanar conductors across a gap S, thickness t, in a uniform dielectric:
the mutual capacitance per length with solid metal, and with each conductor as two sheets (its bottom and top faces,
joined, no side walls). Their difference, over eps, is what a two-face model misses at the gap: h(t/S).
usage: python3 walls2d.py h t1,t2,...   (lengths in units of S)"""
import sys, numpy as np, scipy.sparse as sp, scipy.sparse.linalg as spl
def mutual(h, t, sheets, W=6.0, margin=14.0):
    # x across, y up; gap centred at x=0, metal from y=0 to y=t.
    nx = int(round((2 * W + 1 + 2 * margin) / h)); ny = int(round((t + 2 * margin) / h))
    x0 = -(W + 0.5 + margin); y0 = -margin
    xs = x0 + h * np.arange(nx + 1); ys = y0 + h * np.arange(ny + 1)
    X, Y = np.meshgrid(xs, ys, indexing="ij")
    inY = (Y >= -1e-9) & (Y <= t + 1e-9)
    if sheets:
        inY = (np.abs(Y) < 1e-9) | (np.abs(Y - t) < 1e-9)
    A = (X <= -0.5 + 1e-9) & (X >= -0.5 - W - 1e-9) & inY
    B = (X >= 0.5 - 1e-9) & (X <= 0.5 + W + 1e-9) & inY
    fixed = A | B
    edge = np.zeros_like(fixed); edge[0, :] = edge[-1, :] = edge[:, 0] = edge[:, -1] = True
    fixed = fixed | edge
    V = np.zeros(X.shape); V[A] = 1.0
    idx = -np.ones(X.shape, dtype=np.int64); free = ~fixed; idx[free] = np.arange(free.sum())
    N = free.sum(); rows, cols, vals = [], [], []; rhs = np.zeros(N)
    I, J = np.nonzero(free)
    k = idx[I, J]
    rows.append(k); cols.append(k); vals.append(np.full(N, 4.0))
    for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        In, Jn = I + di, J + dj
        nb = idx[In, Jn]
        f = nb >= 0
        rows.append(k[f]); cols.append(nb[f]); vals.append(np.full(f.sum(), -1.0))
        rhs[k[~f]] += V[In[~f], Jn[~f]]
    M = sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))), shape=(N, N))
    sol = spl.spsolve(M.tocsc(), rhs)
    V[free] = sol
    # Charge on B (at 0 V) with A at 1 V: the flux into B's nodes, eps0 = 1 units: q = sum over B of (4V - neighbours).
    Bi, Bj = np.nonzero(B)
    q = 0.0
    for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        q += (V[Bi, Bj] - V[Bi + di, Bj + dj]).sum()
    return -q   # C_AB per length / eps
h0 = float(sys.argv[1])
import math
for t in [float(v) for v in sys.argv[2].split(",")]:
    h = min(h0, t / math.ceil(t / h0))   # (a whole number of steps through the metal: its top face on a grid line)
    # (and the gap edges on grid lines: 0.5 a whole number of steps too)
    h = 0.5 / math.ceil(0.5 / h) if abs(round(t / (0.5 / math.ceil(0.5 / h))) * (0.5 / math.ceil(0.5 / h)) - t) < 1e-9 else h
    cs, cw = mutual(h, t, False), mutual(h, t, True)
    print(f"h {h} t/S {t}: solid {cs:.4f}  sheets {cw:.4f}  excess {cs - cw:.4f} (x eps)", flush=True)
