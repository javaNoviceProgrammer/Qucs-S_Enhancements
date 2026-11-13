"""A bandstop filter's notch from its S: the frequency of |S21|'s least (a parabola in dB through its three lowest
points), its depth, and its -20 dB edges (where |S21| crosses -20 dB, linearly between samples), their centre and width.
usage: python3 -I notch.py a.s2p|port-S.csv [...]"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loss import read

def s21dB(path):
    if path.endswith(".csv"):
        out = []
        for line in open(path):
            v = line.split(",")
            try:
                out.append((float(v[0]), float(v[3])))
            except ValueError:
                pass
        return out
    return [(f / 1e9, 20 * math.log10(abs(s21))) for f, s11, s21, s12, s22 in read(path)]

def notch(path):
    rows = s21dB(path)
    k = min(range(1, len(rows) - 1), key=lambda i: rows[i][1])
    (f0, a), (f1, b), (f2, c) = rows[k - 1], rows[k], rows[k + 1]
    den = (f0 - f1) * (f0 - f2) * (f1 - f2)
    A = (f2 * (b - a) + f1 * (a - c) + f0 * (c - b)) / den
    B = (f2 * f2 * (a - b) + f1 * f1 * (c - a) + f0 * f0 * (b - c)) / den
    fn = -B / (2 * A)
    depth = a + (b - a) * 0 if A == 0 else (A * fn * fn + B * fn + (a - A * f0 * f0 - B * f0))
    cross = []
    for (fa, da), (fb, db) in zip(rows, rows[1:]):
        if (da + 20) * (db + 20) < 0:
            cross.append(fa + (fb - fa) * (-20 - da) / (db - da))
    low, high = (cross[0], cross[-1]) if len(cross) >= 2 else (float("nan"), float("nan"))
    return fn, depth, low, high

if __name__ == "__main__":
    for path in sys.argv[1:]:
        fn, depth, low, high = notch(path)
        print(f"{os.path.basename(path)}: least |S21| {depth:.1f} dB at {fn:.4f} GHz; -20 dB from {low:.4f} to {high:.4f} GHz,"
              f" centre {(low + high) / 2:.4f}, width {high - low:.4f}")
