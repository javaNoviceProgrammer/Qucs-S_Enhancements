# The measured coplanar line on alumina (Gold et al., Adv. Radio Sci. 17,
# 51 (2019); test_rf25_measured) by the 2D eddy-current solve of its
# cross-section: a 50 µm signal, 25 µm gaps, 270 µm grounds of 6.5 µm metal
# at 36 MS/m, 500 µm over the chuck (a perfect plane); the signal +1 A, its
# grounds -1/2 each (tied, as the coplanar port drives them), the chuck none
# net.
#   python3 -B alumina.py          the direct-current check, R and L at 10, 30
#                                  and 60 GHz at three refinements, and at 4 sigma
#   python3 -B alumina.py bands    the loss of each band in from an edge over its
#                                  current's average, against crowding()'s
#   python3 -B alumina.py thz      against Wheeler's rule as the skin depth goes
import cmath, math, sys
import numpy as np
from eddy import Problem, graded, MU0
um = 1e-6
h, t, s, g, wg, sigma = 500 * um, 6.5 * um, 50 * um, 25 * um, 270 * um, 36e6

def merged(xs, extra):
    """xs with the lines of extra inside it: a band's edges where a full-wave cell's are."""
    out = sorted(set(list(xs) + [x for x in extra if xs[0] < x < xs[-1]]))
    keep = [out[0]]
    for x in out[1:]:
        if x - keep[-1] > 1e-4 * um: keep.append(x)
    return np.array(keep)

def problem(first=0.01 * um, ratio=1.2, capx=1 * um, capy=0.3 * um, capg=3 * um, sig=sigma, extra_s=(), extra_g=()):
    p = Problem()
    ys = graded(h, h + t, first, ratio, capy)
    p.add('s', merged(graded(0, s / 2, first, ratio, capx, low=False, high=True), extra_s), ys, sig)
    p.add('g', merged(graded(s / 2 + g, s / 2 + g + wg, first, ratio, capg), extra_g), ys, sig)
    return p

def solve(p, f):
    u, I = p.solve(f, [['s'], ['g']], [0.5, -0.5])
    Z = u[0] - u[1]
    return Z.real, Z.imag / (2 * math.pi * f), I

def sheet(f, sig=sigma):
    """sheetImpedance(): half the slab of half the thickness."""
    k = (1 + 1j) * math.sqrt(math.pi * f * MU0 * sig)
    return 0.5 * k / sig / cmath.tanh(k * t / 2)

def bandFactor(p, I, cond, x0, x1, f):
    """The band's loss over Re(sheet) |I_band|^2 / w, its wall in it where it is an edge's."""
    R = p.R; cx = 0.5 * (R[:, 0] + R[:, 1]); c = np.array(p.cond)
    band = (c == cond) & (cx > x0) & (cx < x1)
    Ib = I[band].sum(); Pb = np.sum(p.res[band] * np.abs(I[band]) ** 2)
    return Pb / (sheet(f).real * abs(Ib) ** 2 / (x1 - x0)), Ib

def crowding(width, f):
    """rf25_fullwave.cpp's, of a face's edge cell with one free side."""
    w = 2 * math.pi * f; sh = sheet(f)
    stop = 3 * t / (4 * math.pi * math.exp(math.pi))
    thick = max(1.0, math.log(1 + width / stop) / 4)
    pearl = 2 * sh.real / (w * MU0) / width
    crowd = 1 + (thick - 1) / (1 + pearl * pearl)
    half = math.sqrt(w * MU0 / (2 * sigma)) / 2
    return (crowd * half + max(0.0, sh.real - half)) / sh.real

if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else ''
    if what == '':
        p = problem(0.05 * um, 1.4, 4 * um, 1 * um, 10 * um)
        R, L, _ = solve(p, 1.0)
        dc = 1 / (sigma * s * t) + 1 / (sigma * 2 * wg * t)
        print("1 Hz: R %.6f Ohm/m, 1/(sigma A) of the signal and its grounds %.6f (%+.1e)" % (R, dc, R / dc - 1))
        for args in ((0.05 * um, 1.4, 4 * um, 1 * um, 10 * um), (0.02 * um, 1.3, 2 * um, 0.5 * um, 5 * um),
                     (0.01 * um, 1.2, 1 * um, 0.3 * um, 3 * um)):
            p = problem(*args)
            print("%5d filaments (half the line): " % len(p.rects)
                  + "; ".join("%g GHz R %.2f Ohm/m L %.5g H/m" % ((f / 1e9,) + solve(p, f)[:2]) for f in (10e9, 30e9, 60e9)), flush=True)
        R1 = [solve(p, f)[0] for f in (10e9, 30e9, 60e9)]
        p4 = problem(sig=4 * sigma)
        R4 = [solve(p4, f)[0] for f in (10e9, 30e9, 60e9)]
        print("at 4 sigma R over R: " + ", ".join("%.4f" % (b / a) for a, b in zip(R1, R4)) + " (a thick metal's 0.5)")
    elif what == 'bands':
        ws = (1.04, 2.08, 3.25, 4.17, 5.625, 8.33, 11.25, 16.7, 22.5)
        p = problem(extra_s=[s / 2 - w * um for w in ws], extra_g=[s / 2 + g + w * um for w in ws])
        for f in (10e9, 30e9, 60e9):
            _, _, I = solve(p, f)
            print("%g GHz:" % (f / 1e9))
            for w in ws:
                Fs, Is = bandFactor(p, I, 's', s / 2 - w * um, s / 2, f)
                Fg, Ig = bandFactor(p, I, 'g', s / 2 + g, s / 2 + g + w * um, f)
                print("  %5.2f um in (w/t %.2f): the signal's %.3f (of its half's current %.3f), the ground's %.3f (%.3f); "
                      "crowding() %.3f" % (w, w / 6.5, Fs, abs(Is) / 0.5, Fg, abs(Ig) / 0.5, crowding(w * um, f)), flush=True)
    elif what == 'thz':
        from wheeler import dLdn
        d = dLdn()
        p = problem(0.001 * um, 1.25, 1 * um, 0.25 * um, 4 * um)
        for f in (10e9, 60e9, 360e9, 2160e9, 12960e9):
            R = solve(p, f)[0]
            Rs = math.sqrt(math.pi * f * MU0 / sigma); delta = 1 / math.sqrt(math.pi * f * MU0 * sigma)
            print("%8.0f GHz, skin depth %.4f um: R %.2f, Wheeler's %.2f, %+.2f %%" % (f / 1e9, delta / um, R, Rs / MU0 * d, 100 * (R / (Rs / MU0 * d) - 1)), flush=True)
