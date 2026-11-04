# Five lines - four coplanar and a microstrip - each by the 2D eddy solve
# and by faces.py's emulator of the full-wave's faces model, on meshes cut
# by a rule for a gap's finest edge cell, and with the edge factors tried in
# place of crowding(). Each line's attenuation is R sqrt(eps_eff)/(2 c L):
# its error the emulator's (1 + dR)/(1 + dL) - 1, the faces' L and C off
# alike (their eps_eff right).
#   python3 -B lines.py   the rules' table, then the factors' (some 2 min)
import math
import numpy as np
from eddy import Problem, graded
from faces import Emu, gridLines, MU0, segMeanLog, zs_parts
um = 1e-6
LINES = {
    # signal, gap, ground (µm), thickness (µm), sigma, height over the chuck (µm), cells across, largest cell (µm), GHz
    'alumina': dict(s=50, g=25, wg=270, t=6.5, sigma=36e6, h=500, across=3, maximum=100, fs=(10, 30, 60)),
    'copper': dict(s=100, g=50, wg=250, t=3, sigma=5.8e7, h=200, across=4, maximum=100, fs=(2, 10, 30)),
    'film': dict(s=10, g=5, wg=100, t=1, sigma=4.1e7, h=500, across=3, maximum=20, fs=(10, 40, 100)),
    'narrow': dict(s=20, g=6, wg=100, t=3, sigma=4.1e7, h=100, across=3, maximum=50, fs=(10, 30, 60)),
    'microstrip': dict(s=100, g=0, wg=0, t=3, sigma=5.8e7, h=100, across=4, maximum=100, fs=(2, 10, 30)),
}
_refs = {}
def reference(name):
    """The 2D solve's R and L at each frequency."""
    if name in _refs: return _refs[name]
    d = LINES[name]; s, g, wg, t, h = (d[k] * um for k in ('s', 'g', 'wg', 't', 'h'))
    p = Problem(); first = min(0.01 * um, t / 100)
    ys = graded(h, h + t, first, 1.2, min(0.3 * um, t / 6))
    p.add('s', graded(0, s / 2, first, 1.2, min(1 * um, s / 20), low=False), ys, d['sigma'])
    if wg > 0: p.add('g', graded(s / 2 + g, s / 2 + g + wg, first, 1.2, min(3 * um, wg / 30)), ys, d['sigma'])
    out = {}
    for f in d['fs']:
        if wg > 0:
            u, I = p.solve(f * 1e9, [['s'], ['g']], [0.5, -0.5]); Z = u[0] - u[1]
        else:
            u, I = p.solve(f * 1e9, [['s']], [0.5]); Z = 2 * u[0]   # (the half carries half the current: its drop is the line's)
        out[f] = (Z.real, Z.imag / (2 * math.pi * f * 1e9))
    _refs[name] = out
    return out

def emulator(name, finest):
    """finest(gap, t): the finest edge cell facing a gap (0: none)."""
    d = LINES[name]; s, g, wg, t, h = (d[k] * um for k in ('s', 'g', 'wg', 't', 'h'))
    maximum = d['maximum'] * um
    e = Emu(h, t, d['sigma'])
    def lines(frm, width, gapLow, gapHigh):
        step = min(maximum, width / d['across']); reach = maximum / (2 * 0.25)
        fl = finest(gapLow, t) if 0 < gapLow < reach else 0
        fh = finest(gapHigh, t) if 0 < gapHigh < reach else 0
        return gridLines(frm, width, step, True, True, fl, fh)
    e.strip('s', lines(-s / 2, s, g if wg > 0 else 0, g if wg > 0 else 0), True, True, half_from=0.0)
    if wg > 0: e.strip('g', lines(s / 2 + g, wg, g, 0), True, True)
    return e

def solveMicrostrip(e, f, crowd):
    """The emulator's solve of a signal alone, its image the return."""
    C = e.cells; n = len(C)
    x0 = np.array([c[1] for c in C]); x1 = np.array([c[2] for c in C]); y = np.array([c[3] for c in C])
    A = lambda b0, b1, yb: segMeanLog(x0[:, None], x1[:, None], y[:, None], b0[None, :], b1[None, :], yb[None, :])
    Lm = (A(x0, x1, -y) + A(-x1, -x0, -y)) - (A(x0, x1, y) + A(-x1, -x0, y))
    Z = 1j * 2 * math.pi * f * MU0 / (2 * math.pi) * Lm
    own, across, sheet = zs_parts(e.sigma, e.t, f)
    key = {(c[1], c[2], c[3] > e.h + e.t / 2): i for i, c in enumerate(C)}
    for i, c in enumerate(C):
        tw = key[(c[1], c[2], not (c[3] > e.h + e.t / 2))]; w = c[2] - c[1]
        edge = (crowd(w, e.t, e.sigma, c[4], f, sheet) - 1) * sheet.real * (1 + 1j)
        Z[i, i] += (own + edge) / w; Z[i, tw] += (across + edge) / w
    M = np.zeros((n + 1, n + 1), complex); rhs = np.zeros(n + 1, complex); M[:n, :n] = Z
    M[:n, n] = -1; M[n, :n] = 1; rhs[n] = 0.5
    return 2 * np.linalg.solve(M, rhs)[n]

def factor(kind):
    """crowding() as it is ('now'), and the variants tried."""
    def F(width, t, sigma, free, f, sheet):
        if free <= 0: return 1.0
        w = 2 * math.pi * f
        lg = math.log(1 + width / (3 * t / (4 * math.pi * math.exp(math.pi))))
        thick = 2 / math.pi ** 2 * lg if free >= 2 else lg / 4
        if kind in ('now', 'no fading'): thick = max(1.0, thick)
        if kind in ('to nought', 'to nought, no fading'): thick *= 1 - math.exp(-2.2 * width / t)   # (the bands' own, alumina.py bands)
        pearl = 2 * sheet.real / (w * MU0) / width
        fade = 1 / (1 + pearl * pearl) if kind in ('now', 'unclamped', 'to nought') else 1.0
        crowd = 1 + (thick - 1) * fade
        half = math.sqrt(w * MU0 / (2 * sigma)) / 2
        return (crowd * half + max(0.0, sheet.real - half)) / sheet.real
    return F

def errors(name, finest, crowd):
    ref = reference(name); e = emulator(name, finest); out = []
    for f in LINES[name]['fs']:
        Z = solveMicrostrip(e, f * 1e9, crowd) if LINES[name]['wg'] == 0 else e.solve(f * 1e9, crowd=crowd)
        R2, L2 = ref[f]; dR = Z.real / R2 - 1; dL = Z.imag / (2 * math.pi * f * 1e9) / L2 - 1
        out.append((f, dR, dL, (1 + dR) / (1 + dL) - 1))
    return out

if __name__ == "__main__":
    rules = {'none': lambda g, t: 0, 'g/4': lambda g, t: 0.25 * g, 'g/2': lambda g, t: 0.5 * g}
    for k in (1.5, 2.0):
        rules['max(g/4, %gt)' % k] = (lambda kk: lambda g, t: max(0.25 * g, kk * t))(k)
    print("The finest edge cell at a gap, crowding() as it is: R, L and alpha against the 2D solve's (%)")
    for name in [n for n in LINES if LINES[n]['wg'] > 0]:
        for rn, rule in rules.items():
            row = errors(name, rule, factor('now'))
            print("  %-8s %-14s %s" % (name, rn, " | ".join("%g GHz R %+5.1f L %+5.1f alpha %+5.1f" % (f, 100 * a, 100 * b, 100 * c) for f, a, b, c in row)), flush=True)
    print("The edge factors tried, on the edges' one halving and graded at the gaps (g/4): alpha against the 2D solve's (%)")
    for kind in ('now', 'no fading', 'unclamped', 'to nought', 'to nought, no fading'):
        errs = {False: [], True: []}; rows = []
        for name in LINES:
            for g_ in (False, True):
                if LINES[name]['wg'] == 0 and g_: continue
                row = errors(name, (lambda g, t: 0.25 * g) if g_ else (lambda g, t: 0), factor(kind))
                errs[g_] += [a for _, _, _, a in row]
                rows.append("%s%s %s" % (name, "/g4" if g_ else "", " ".join("%+5.1f" % (100 * a) for _, _, _, a in row)))
        print("  %-20s halved: mean |alpha| %4.1f, most %4.1f; graded: mean %4.1f, most %4.1f\n      %s" % (
            kind, 100 * np.mean(np.abs(errs[False])), 100 * np.max(np.abs(errs[False])), 100 * np.mean(np.abs(errs[True])),
            100 * np.max(np.abs(errs[True])), "; ".join(rows)), flush=True)
