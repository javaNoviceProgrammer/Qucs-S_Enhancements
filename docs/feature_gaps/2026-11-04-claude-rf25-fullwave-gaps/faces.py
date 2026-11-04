# The full-wave's conductor model in a line's cross-section: a metal solved
# as its two faces is two rows of ribbons - its cells across the line, as
# the mesh cuts them (gridLines() as rf25_mesh.cpp has it) - each carrying a
# uniform current; per metre, a ribbon w wide has its own Zs coth(gamma t)/w,
# its twin's Zs csch(gamma t)/w, and an edge cell (crowd - 1) Re(sheet)
# (1 + j)/w on both faces' current together (ownImpedances()); the ribbons'
# inductance by the mean of ln r between segments, the mirror at x = 0 and
# the chuck at y = 0 by images. R = Re(u_s - u_g) per ampere, L its Im / w.
#   python3 -B faces.py   the measured line on the edges' one halving and graded
#                         at its gaps, beside the full-wave solve's own R and L
#                         (probe.txt) and the 2D eddy solve's (alumina.py)
import math, cmath, numpy as np
MU0 = 4e-7 * math.pi
um = 1e-6

def gridLines(frm, length, step, gradeLow, gradeHigh, finestLow=0, finestHigh=0):
    n = max(1, int(math.ceil(length / step - 1e-9))) if step > 0 else 1
    d = length / n; at = [frm]
    if gradeLow:
        at.append(frm + d / 2); w = d / 2
        while finestLow > 0 and w > finestLow and w > 1e-6 * length:
            at.append(frm + w / 2); w /= 2
    at += [frm + i * d for i in range(1, n)]
    if gradeHigh:
        at.append(frm + length - d / 2); w = d / 2
        while finestHigh > 0 and w > finestHigh and w > 1e-6 * length:
            at.append(frm + length - w / 2); w /= 2
    at.append(frm + length); at.sort(); out = []
    for x in at:
        if not out or x - out[-1] > 1e-9 * max(1.0, abs(length)): out.append(x)
    return out

def segMeanLog(x1, x2, y1, x3, x4, y2):
    """Mean of ln r between [x1,x2] at y1 and [x3,x4] at y2 (vectorized)."""
    Y = np.abs(y1 - y2)
    def G(X):
        r2 = X * X + Y * Y
        with np.errstate(divide='ignore', invalid='ignore'):
            lg = np.where(r2 > 0, 0.5 * np.log(np.where(r2 > 0, r2, 1.0)), 0.0)
            at = np.where(Y > 0, np.arctan(X / np.where(Y > 0, Y, 1.0)), 0.0)
        return 0.5 * (X * X - Y * Y) * lg + X * Y * at - 0.75 * X * X
    tot = G(x2 - x3) - G(x1 - x3) - G(x2 - x4) + G(x1 - x4)
    return tot / ((x2 - x1) * (x4 - x3))

def zs_parts(sigma, t, f):
    d = math.sqrt(1 / (math.pi * f * MU0 * sigma)); g = (1 + 1j) / d; zs = g / sigma
    e = cmath.exp(-g * t)
    own = zs * (1 + e * e) / (1 - e * e); across = zs * 2 * e / (1 - e * e)
    zh = g / sigma * (1 + cmath.exp(-g * t)) / (1 - cmath.exp(-g * t))   # surfaceImpedance(sigma, t/2): coth(g t/2)
    sheet = 0.5 * zh
    return own, across, sheet

def crowding_now(width, t, sigma, free, f, sheet, faces=True):
    if free <= 0: return 1.0
    w = 2 * math.pi * f
    stop = t / (4 * math.pi * math.exp(math.pi)) * (3 if faces else 1)
    lg = math.log(1 + width / stop)
    thick = max(1.0, 2 / math.pi ** 2 * lg if free >= 2 else lg / 4)
    pearl = 2 * sheet.real / (w * MU0) / width
    crowd = 1 + (thick - 1) / (1 + pearl * pearl)
    half = math.sqrt(w * MU0 / (2 * sigma)) / 2
    return (crowd * half + max(0.0, sheet.real - half)) / sheet.real

class Emu:
    def __init__(self, h, t, sigma, plane=True):
        self.h, self.t, self.sigma, self.plane = h, t, sigma, plane
        self.cells = []   # (cond, x0, x1, face y, free sides)
    def strip(self, cond, lines, free_low, free_high, half_from=None):
        """lines across one conductor (absolute x); free_low/high: its ends are edges. half_from: keep x >= it (mirror)."""
        for k in range(len(lines) - 1):
            x0, x1 = lines[k], lines[k + 1]
            free = (1 if (k == 0 and free_low) else 0) + (1 if (k == len(lines) - 2 and free_high) else 0)
            if half_from is not None:
                if x1 <= half_from + 1e-15: continue
                if x0 < half_from: x0 = half_from   # (the middle cell straddles the mirror: its half, its current even)
            for y in (self.h, self.h + self.t):
                self.cells.append((cond, x0, x1, y, free))
    def solve(self, f, crowd=crowding_now, mirror=True):
        C = self.cells; n = len(C)
        x0 = np.array([c[1] for c in C]); x1 = np.array([c[2] for c in C]); y = np.array([c[3] for c in C])
        w = x1 - x0
        A = lambda b0, b1, yb: segMeanLog(x0[:, None], x1[:, None], y[:, None], b0[None, :], b1[None, :], yb[None, :])
        lnr = A(x0, x1, y)
        if mirror: lnr = lnr + A(-x1, -x0, y)
        img = 0
        if self.plane:
            img = A(x0, x1, -y)
            if mirror: img = img + A(-x1, -x0, -y)
        om = 2 * math.pi * f
        Z = 1j * om * MU0 / (2 * math.pi) * (img - lnr)
        own, across, sheet = zs_parts(self.sigma, self.t, f)
        # the twin of each cell: same cond, same x0, x1, other face
        key = {(c[0], round(c[1] / 1e-12), round(c[2] / 1e-12), c[3] > self.h + self.t / 2): i for i, c in enumerate(C)}
        for i, c in enumerate(C):
            tw = key[(c[0], round(c[1] / 1e-12), round(c[2] / 1e-12), not (c[3] > self.h + self.t / 2))]
            cr = crowd(w[i] if c[4] == 0 else self.fullwidth(i), self.t, self.sigma, c[4], f, sheet)
            edge = (cr - 1) * sheet.real * (1 + 1j)
            Z[i, i] += (own + edge) / w[i]
            Z[i, tw] += (across + edge) / w[i]
        conds = sorted(set(c[0] for c in C)); cond = np.array([c[0] for c in C])
        m = len(conds); M = np.zeros((n + m, n + m), complex); rhs = np.zeros(n + m, complex)
        M[:n, :n] = Z
        cur = {'s': 0.5, 'g': -0.5}
        for k, name in enumerate(conds):
            sel = cond == name
            M[:n, n + k][sel] = -1.0; M[n + k, :n][sel] = 1.0; rhs[n + k] = cur[name]
        sol = np.linalg.solve(M, rhs)
        self.I = sol[:n]
        u = dict(zip(conds, sol[n:]))
        return u['s'] - u['g']
    def fullwidth(self, i):
        c = self.cells[i]; return c[2] - c[1]

def alumina(gaps, across=3, maximum=100 * um, share=0.25):
    """The measured line's cells as the mesh cuts them (stretch along does not matter across)."""
    s, g, wg = 50 * um, 25 * um, 270 * um
    e = Emu(500 * um, 6.5 * um, 36e6)
    def lines(frm, width, gapLow, gapHigh):
        step = min(maximum, width / across)
        reach = maximum / (2 * share)
        fl = share * gapLow if gaps and 0 < gapLow < reach else 0
        fh = share * gapHigh if gaps and 0 < gapHigh < reach else 0
        return gridLines(frm, width, step, True, True, fl, fh)
    e.strip('s', lines(-s / 2, s, g, g), True, True, half_from=0.0)
    e.strip('g', lines(s / 2 + g, wg, g, 0), True, True)
    return e

if __name__ == "__main__":
    # The full-wave's calibrated R = Re(gamma Z) and L = Im(gamma Z)/w
    # (probe.txt, the substrate lossless), and the 2D eddy solve's.
    fullwave = {False: {10e9: (682.90, 4.0479e-07), 30e9: (1206.80, 4.0001e-07), 60e9: (1740.34, 4.0028e-07)},
                True: {10e9: (763.73, 3.71e-07), 30e9: (1401.13, 3.6551e-07), 60e9: (2024.56, 3.6531e-07)}}
    eddy = {10e9: (731.44, 3.4995e-7), 30e9: (1267.33, 3.4506e-7), 60e9: (1795.37, 3.4309e-7)}
    for gaps in (False, True):
        e = alumina(gaps)
        xs = sorted(set(round(c[k] / um, 3) for c in e.cells if c[0] == 's' for k in (1, 2)))
        xg = sorted(set(round(c[k] / um, 3) for c in e.cells if c[0] == 'g' for k in (1, 2)))
        print("%s: the signal's lines %s, a ground's %s" % ("graded at the gaps" if gaps else "the edges' one halving", xs, xg))
        for f in (10e9, 30e9, 60e9):
            Z = e.solve(f); R, L = Z.real, Z.imag / (2 * math.pi * f)
            Rf, Lf = fullwave[gaps][f]; R2, L2 = eddy[f]
            print("  %g GHz: R %.2f Ohm/m (the full wave's %.2f, %+.1f %%; the 2D solve's %+.1f %%), L %.5g H/m (%+.1f %%; %+.1f %%)" % (
                f / 1e9, R, Rf, 100 * (R / Rf - 1), 100 * (R / R2 - 1), L, 100 * (L / Lf - 1), 100 * (L / L2 - 1)))
        # The edge cells' share of the current as the frequency rises, to a
        # perfect metal's (1e15 Hz).
        for f in (1e9, 10e9, 60e9, 1e15):
            Z = e.solve(f, crowd=lambda *a: 1.0)
            def share(cond, x, upper):
                return abs(sum(e.I[i] for i, c in enumerate(e.cells) if c[0] == cond and abs((c[2] if upper else c[1]) - x) < 1e-12)) / 0.5
            print("    %8.3g GHz: the outermost cells carry %.3f of the signal's half, %.3f of a ground's at the gap" % (
                f / 1e9, share('s', 25 * um, True), share('g', 50 * um, False)))
