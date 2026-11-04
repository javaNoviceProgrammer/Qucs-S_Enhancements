# Wheeler's incremental inductance rule on the alumina line's cross-section,
# independent of the eddy-current solve: the external inductance from an
# electrostatic boundary-element solve in vacuum (L = 1/(c^2 C0)) - panels
# of constant charge round each rectangle, graded toward its corners,
# collocated at their middles; the mirror at x = 0 and the chuck at y = 0
# by images; the signal +Q, its grounds -Q, tied, the chuck none net - and
# R = (Rs/mu0) dL/dn, every surface receded by n.
#   python3 -B wheeler.py          C0, L and Wheeler's R at three refinements
#   python3 -B wheeler.py plates   wide plates by both methods (their fringing
#                                  6 %, so neither is 2 Rs/W)
import math, sys
import numpy as np
from eddy import graded, MU0
EPS0 = 8.8541878128e-12; C0V = 1 / math.sqrt(MU0 * EPS0)
um = 1e-6

def panels(rect, first, ratio, cap, mirror_side=False):
    """Panels round rect (x0, x1, y0, y1), each (xa, ya, xb, yb); mirror_side: x0 is the mirror (no panel there)."""
    x0, x1, y0, y1 = rect
    xs = graded(x0, x1, first, ratio, cap, low=not mirror_side, high=True)
    ys = graded(y0, y1, first, ratio, cap)
    out = []
    for k in range(len(xs) - 1):
        out += [(xs[k], y0, xs[k + 1], y0), (xs[k], y1, xs[k + 1], y1)]
    for k in range(len(ys) - 1):
        out.append((x1, ys[k], x1, ys[k + 1]))
        if not mirror_side: out.append((x0, ys[k], x0, ys[k + 1]))
    return out

def lnInt(px, py, P):
    """The integral of ln |p - s| over each panel (columns) at each point (rows)."""
    xa, ya, xb, yb = P[:, 0], P[:, 1], P[:, 2], P[:, 3]
    L = np.hypot(xb - xa, yb - ya); tx, ty = (xb - xa) / L, (yb - ya) / L
    dx = px[:, None] - xa[None, :]; dy = py[:, None] - ya[None, :]
    u = dx * tx + dy * ty; Y = np.abs(-dx * ty + dy * tx)
    def G(sv):
        v = sv - u; r2 = v * v + Y * Y
        with np.errstate(divide='ignore', invalid='ignore'):
            lg = np.where(r2 > 0, 0.5 * np.log(np.where(r2 > 0, r2, 1.0)), 0.0)
            at = np.where(Y > 0, np.arctan(v / np.where(Y > 0, Y, 1.0)), 0.0)
        return v * lg - v + Y * at
    return G(L[None, :]) - G(0.0)

def capacitance(rects, first=0.002 * um, ratio=1.15, cap=0.5 * um):
    """rects: (name, rect, mirror_side); C0 per metre between 's' and 'g'."""
    P = []; who = []
    for name, rect, ms in rects:
        for q in panels(rect, first, ratio, cap, ms):
            P.append(q); who.append(name)
    P = np.array(P); who = np.array(who); n = len(P)
    mx = 0.5 * (P[:, 0] + P[:, 2]); my = 0.5 * (P[:, 1] + P[:, 3])
    Pm = P.copy(); Pm[:, 0] *= -1; Pm[:, 2] *= -1
    Pi = P.copy(); Pi[:, 1] *= -1; Pi[:, 3] *= -1
    Pim = Pi.copy(); Pim[:, 0] *= -1; Pim[:, 2] *= -1
    K = -(lnInt(mx, my, P) + lnInt(mx, my, Pm) - lnInt(mx, my, Pi) - lnInt(mx, my, Pim)) / (2 * math.pi * EPS0)
    Lp = np.hypot(P[:, 2] - P[:, 0], P[:, 3] - P[:, 1])
    M = np.zeros((n + 2, n + 2)); rhs = np.zeros(n + 2); M[:n, :n] = K
    for col, name in enumerate(('s', 'g')):
        sel = who == name
        M[:n, n + col][sel] = -1.0
        M[n + col, :n][sel] = 2 * Lp[sel]          # (both halves' charge)
        rhs[n + col] = 1.0 if name == 's' else -1.0
    V = np.linalg.solve(M, rhs)[n:]
    return 1.0 / (V[0] - V[1]), n

h, t, s, g, wg, sigma = 500 * um, 6.5 * um, 50 * um, 25 * um, 270 * um, 36e6
def line(n):
    return [('s', (0, s / 2 - n, h + n, h + t - n), True), ('g', (s / 2 + g + n, s / 2 + g + wg - n, h + n, h + t - n), False)]
def dLdn(geometry=line, first=0.002 * um, ratio=1.15, cap=0.5 * um, dn=0.02 * um):
    Lof = lambda n: 1 / (C0V ** 2 * capacitance(geometry(n), first, ratio, cap)[0])
    return (Lof(dn) - Lof(-dn)) / (2 * dn)

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == 'plates':
        from eddy import Problem
        W, d = 400 * um, 10 * um; hp = 3000 * um
        def plates(n):
            return [('s', (0, W / 2 - n, hp + n, hp + t - n), True), ('g', (0, W / 2 - n, hp + t + d + n, hp + 2 * t + d - n), True)]
        C, _ = capacitance(plates(0)); dl = dLdn(plates)
        p = Problem()
        p.add('s', graded(0, W / 2, 0.02 * um, 1.3, 4 * um, low=False), graded(hp, hp + t, 0.02 * um, 1.3, 0.5 * um), sigma)
        p.add('g', graded(0, W / 2, 0.02 * um, 1.3, 4 * um, low=False), graded(hp + t + d, hp + 2 * t + d, 0.02 * um, 1.3, 0.5 * um), sigma)
        print("plates %g um wide, %g apart: C0 %.4f pF/m against eps0 W/d %.4f" % (W / um, d / um, C * 1e12, EPS0 * W / d * 1e12))
        for f in (10e9, 60e9, 240e9):
            u, I = p.solve(f, [['s'], ['g']], [0.5, -0.5])
            Rs = math.sqrt(math.pi * f * MU0 / sigma)
            print("  %g GHz: eddy %.3f, Wheeler %.3f Ohm/m (%+.2f %%); 2 Rs/W %.3f" % (f / 1e9, (u[0] - u[1]).real, Rs / MU0 * dl,
                  100 * ((u[0] - u[1]).real / (Rs / MU0 * dl) - 1), 2 * Rs / W))
    else:
        for first, ratio, cap in ((0.004 * um, 1.25, 1 * um), (0.002 * um, 1.15, 0.5 * um), (0.001 * um, 1.1, 0.25 * um)):
            C, n = capacitance(line(0), first, ratio, cap)
            dl = dLdn(line, first, ratio, cap)
            Rw = ["%g GHz %.2f" % (f / 1e9, math.sqrt(math.pi * f * MU0 / sigma) / MU0 * dl) for f in (10e9, 30e9, 60e9)]
            print("%d panels (half the line): C0 %.5f pF/m, L %.6g H/m, dL/dn %.6g H/m/m; Wheeler's R (Ohm/m): %s"
                  % (n, C * 1e12, 1 / (C0V ** 2 * C), dl, ", ".join(Rw)), flush=True)
