# A 2D eddy-current solve of a line's cross-section (magneto-quasistatic, the
# current along the line): each conductor cut into rectangular filaments of
# uniform current, graded geometrically toward its faces; the mean of ln r
# between filaments in closed form (meanlog.py); a mirror plane at x = 0
# (currents even in x) and a perfect ground plane at y = 0 by images. Each
# filament: I/(sigma A) + jw sum_j L_ij I_j = u of its conductor, u the
# conductor's voltage drop per metre. Driven groups carry a set current; the
# voltage drop of each group is one unknown (conductors in a group are tied).
import math, numpy as np
from meanlog import meanLog
MU0 = 4e-7 * math.pi

def graded(a, b, first, ratio, cap, low=True, high=True):
    """Lines over [a, b]: cells from `first` at a graded end, growing by `ratio` to `cap`."""
    L = b - a
    def run(n_len):
        cells = []; w = first
        while sum(cells) + w < n_len:
            cells.append(w); w = min(w * ratio, cap)
        return cells
    if low and high:
        half = run(L / 2)
        rest = L - 2 * sum(half)
        mid = max(1, int(math.ceil(rest / cap - 1e-9))) if rest > 1e-15 * L else 0
        cells = half + [rest / mid] * mid + half[::-1] if mid else half + half[::-1]
    elif low or high:
        g = run(L)
        rest = L - sum(g)
        mid = max(1, int(math.ceil(rest / cap - 1e-9))) if rest > 1e-15 * L else 0
        cells = g + [rest / mid] * mid if mid else g
        if high and not low: cells = cells[::-1]
    else:
        n = max(1, int(math.ceil(L / cap - 1e-9))); cells = [L / n] * n
    xs = a + np.concatenate([[0.0], np.cumsum(cells)])
    xs[-1] = b
    return xs

class Problem:
    def __init__(self, mirror=True, plane=True):
        self.rects = []; self.cond = []; self.sigma = []
        self.mirror = mirror; self.plane = plane
    def add(self, cond, xs, ys, sigma):
        for i in range(len(xs) - 1):
            for j in range(len(ys) - 1):
                self.rects.append((xs[i], xs[i + 1], ys[j], ys[j + 1])); self.cond.append(cond); self.sigma.append(sigma)
    def build(self):
        R = np.array(self.rects); n = len(R)
        self.R = R
        self.A = (R[:, 1] - R[:, 0]) * (R[:, 3] - R[:, 2])
        def block(src):
            M = np.empty((n, n))
            for i0 in range(0, n, 256):
                i1 = min(n, i0 + 256)
                M[i0:i1] = meanLog(R[i0:i1, None, :].repeat(n, 1), src[None, :, :].repeat(i1 - i0, 0))
            return M
        # mean ln r to each source, its mirror in x, and the images in the plane
        lnr = block(R)
        if self.mirror:
            Rm = R[:, [1, 0, 2, 3]].copy(); Rm[:, 0] *= -1; Rm[:, 1] *= -1
            lnr = lnr + block(Rm)
        img = 0
        if self.plane:
            Ri = R[:, [0, 1, 3, 2]].copy(); Ri[:, 2] *= -1; Ri[:, 3] *= -1
            img = block(Ri)
            if self.mirror:
                Rim = Ri[:, [1, 0, 2, 3]].copy(); Rim[:, 0] *= -1; Rim[:, 1] *= -1
                img = img + block(Rim)
        self.L = MU0 / (2 * math.pi) * (img - lnr)
    def solve(self, f, groups, currents):
        """groups: list of lists of conductor names tied together; currents: each group's total (None: free, u = 0)."""
        if not hasattr(self, 'L'): self.build()
        n = len(self.rects); w = 2 * math.pi * f
        sig = np.array(self.sigma, float)
        res = 1.0 / (sig * self.A)
        cond = np.array(self.cond)
        Z = 1j * w * self.L
        Z[np.diag_indices(n)] += res
        set_ = [k for k, c in enumerate(currents) if c is not None]
        m = len(set_)
        M = np.zeros((n + m, n + m), complex); rhs = np.zeros(n + m, complex)
        M[:n, :n] = Z
        for col, k in enumerate(set_):
            sel = np.isin(cond, groups[k])
            M[:n, n + col][sel] = -1.0
            M[n + col, :n][sel] = 1.0
            rhs[n + col] = currents[k]
        sol = np.linalg.solve(M, rhs)
        self.I = sol[:n]; self.res = res
        return sol[n:], sol[:n]
