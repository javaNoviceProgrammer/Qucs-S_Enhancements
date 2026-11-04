# What a full-wave edge cell would want: its current down its half of the
# wall too. faces.py's emulator with each cell made of thin rectangles, an
# edge cell its face's ribbon and its half of the wall, the current spread
# over both by length - the inductance only, the loss as faces.py's -
# beside a perfect metal's inductance with and without walls that carry it.
#   python3 -B walls.py   the measured line: L of perfect faces against the
#                         boundary-element solve's; then R and L on both meshes,
#                         walls or none, crowding() or each band's own factor
import math, cmath, numpy as np
from meanlog import meanLog
from faces import gridLines, zs_parts, crowding_now, alumina, MU0, um
THIN = 1e-3
def parts(c, h, t, walls):
    cond, x0, x1, y, free = c
    top = y > h + t / 2
    th = THIN * (x1 - x0)
    segs = [((x0, x1, y - th / 2, y + th / 2), x1 - x0)]
    if walls and free:
        ys = (h + t / 2, y) if top else (y, h + t / 2)
        for xe in edgesOf(c):
            tw = THIN * t / 2
            segs.append(((xe - tw / 2, xe + tw / 2, ys[0], ys[1]), t / 2))
    return segs
EDGES = {}
def edgesOf(c): return EDGES.get((c[0], c[1], c[2]), [])
def solve(e, f, crowd, walls=False, mirror=True):
    C = e.cells; n = len(C)
    P = [parts(c, e.h, e.t, walls) for c in C]
    def mlog(A, B):   # mean ln r between composite cells
        tot = 0.0; La = sum(l for _, l in A); Lb = sum(l for _, l in B)
        for ra, la in A:
            for rb, lb in B:
                tot += la * lb * float(meanLog(np.array(ra), np.array(rb)))
        return tot / (La * Lb)
    def flipx(B): return [((-r[1], -r[0], r[2], r[3]), l) for r, l in B]
    def flipy(B): return [((r[0], r[1], -r[3], -r[2]), l) for r, l in B]
    Lm = np.zeros((n, n))
    for i in range(n):
        for j in range(n):
            v = -mlog(P[i], P[j]) + mlog(P[i], flipy(P[j]))
            if mirror: v += -mlog(P[i], flipx(P[j])) + mlog(P[i], flipx(flipy(P[j])))
            Lm[i, j] = v
    om = 2 * math.pi * f
    Z = 1j * om * MU0 / (2 * math.pi) * Lm
    own, across, sheet = zs_parts(e.sigma, e.t, f)
    key = {(c[0], c[1], c[2], c[3] > e.h + e.t / 2): i for i, c in enumerate(C)}
    for i, c in enumerate(C):
        tw = key[(c[0], c[1], c[2], not (c[3] > e.h + e.t / 2))]
        w = c[2] - c[1]
        cr = crowd(i, w, c[4], f, sheet)
        edge = (cr - 1) * sheet.real * (1 + 1j)
        Z[i, i] += (own + edge) / w; Z[i, tw] += (across + edge) / w
    cond = np.array([c[0] for c in C]); m = 2
    M = np.zeros((n + m, n + m), complex); rhs = np.zeros(n + m, complex); M[:n, :n] = Z
    for k, (name, cur) in enumerate((('s', 0.5), ('g', -0.5))):
        sel = cond == name; M[:n, n + k][sel] = -1; M[n + k, :n][sel] = 1; rhs[n + k] = cur
    sol = np.linalg.solve(M, rhs); e.I = sol[:n]
    return sol[n] - sol[n + 1]
def setEdges(e):
    EDGES.clear()
    for c in e.cells:
        if c[4]:
            xs = []
            # which side is the edge: the conductor's extreme lines
            lo = min(d[1] for d in e.cells if d[0] == c[0]); hi = max(d[2] for d in e.cells if d[0] == c[0])
            if abs(c[1] - lo) < 1e-12 and not (c[0] == 's' and abs(lo) < 1e-12): xs.append(c[1])
            if abs(c[2] - hi) < 1e-12: xs.append(c[2])
            EDGES[(c[0], c[1], c[2])] = xs

h, t = 500 * um, 6.5 * um   # (the measured line's)
def perfectL(walls, n=40, thin=0.002 * um):
    """A perfect metal's faces, cosine-graded ribbons, and their walls (12 strips each) or none."""
    R = []; cond = []
    for name, a, b in (('s', 0.0, 25 * um), ('g', 50 * um, 320 * um)):
        xs = np.linspace(a, b, n + 1) if name == 'g' else np.linspace(a, b, n // 2 + 1)
        # cosine-graded toward the edges
        u = np.linspace(0, 1, len(xs)); xs = a + (b - a) * (0.5 * (1 - np.cos(math.pi * u)) if name == 'g' else np.sin(0.5 * math.pi * u))
        for k in range(len(xs) - 1):
            for y in (h, h + t - thin):
                R.append((xs[k], xs[k + 1], y, y + thin)); cond.append(name)
        if walls:
            ys = h + t * 0.5 * (1 - np.cos(math.pi * np.linspace(0, 1, 13)))
            for k in range(len(ys) - 1):
                for x in ((b - thin,) if name == 's' else (a, b - thin)):
                    R.append((x, x + thin, max(ys[k], h + thin), min(ys[k + 1], h + t - thin))); cond.append(name)
    R = np.array(R); n_ = len(R); cond = np.array(cond)
    def blk(src): return meanLog(R[:, None, :].repeat(n_, 1), src[None, :, :].repeat(n_, 0))
    Rm = R[:, [1, 0, 2, 3]].copy(); Rm[:, :2] *= -1
    Ri = R[:, [0, 1, 3, 2]].copy(); Ri[:, 2:] *= -1
    Rim = Ri[:, [1, 0, 2, 3]].copy(); Rim[:, :2] *= -1
    Lm = MU0 / (2 * math.pi) * (blk(Ri) + blk(Rim) - blk(R) - blk(Rm))
    # perfect: equal voltage drop on each conductor's filaments, currents +-0.5
    M = np.zeros((n_ + 2, n_ + 2)); rhs = np.zeros(n_ + 2)
    M[:n_, :n_] = Lm
    for k, (name, cur) in enumerate((('s', 0.5), ('g', -0.5))):
        sel = cond == name; M[:n_, n_ + k][sel] = -1; M[n_ + k, :n_][sel] = 1; rhs[n_ + k] = cur
    sol = np.linalg.solve(M, rhs)
    return sol[n_] - sol[n_ + 1], n_

if __name__ == "__main__":
    from alumina import problem, bandFactor, solve as solve2d
    for w_ in (False, True):
        for n in (40, 80):
            L, m = perfectL(w_, n)
            print("perfect faces, %s, %d strips: L %.6g H/m (the boundary-element solve's 3.38279e-07, %+.2f %%)" % (
                "their walls carrying current" if w_ else "no walls", m, L, 100 * (L / 3.38279e-07 - 1)), flush=True)
    for gaps in (False, True):
        e = alumina(gaps); setEdges(e)
        xs = sorted(set(c[1] for c in e.cells if c[0] == 's') | set(c[2] for c in e.cells if c[0] == 's'))
        xg = sorted(set(c[1] for c in e.cells if c[0] == 'g') | set(c[2] for c in e.cells if c[0] == 'g'))
        p = problem(extra_s=xs, extra_g=xg)
        for f in (10e9, 30e9, 60e9):
            R2, L2, I = solve2d(p, f)
            F = [bandFactor(p, I, c[0], c[1], c[2], f)[0] for c in e.cells]
            out = []
            for walls in (False, True):
                for name, cr in (("crowding()", lambda i, w, fr, ff, sh: crowding_now(w, e.t, e.sigma, fr, ff, sh)),
                                 ("bands' own", lambda i, w, fr, ff, sh: F[i])):
                    Z = solve(e, f, cr, walls)
                    out.append("%s%s R %+.1f %% L %+.1f %%" % ("walls, " if walls else "", name, 100 * (Z.real / R2 - 1),
                                                                100 * (Z.imag / (2 * math.pi * f) / L2 - 1)))
            print("%s %g GHz: %s" % ("graded" if gaps else "halved", f / 1e9, "; ".join(out)), flush=True)
