"""RF 2.5D's Touchstone against Palace's port-S.csv: at each of RF 2.5D's frequencies, |S11| and |S21| (dB) and the
power lost (1 - |S11|^2 - |S21|^2) by each, and the lost power's ratio.
usage: python3 -I compare.py rf25.s2p palace/port-S.csv"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loss import read

def palace(path):
    rows = {}
    for line in open(path):
        v = line.split(",")
        try:
            f = float(v[0])
        except ValueError:
            continue
        rows[round(f, 6)] = (10 ** (float(v[1]) / 20), 10 ** (float(v[3]) / 20))
    return rows

def compare(a, b, quiet=False):
    p = palace(b)
    out = []
    for f, s11, s21, _, _ in read(a):
        g = round(f / 1e9, 6)
        if g not in p:
            continue
        q11, q21 = p[g]
        la, lb = 1 - abs(s11) ** 2 - abs(s21) ** 2, 1 - q11 ** 2 - q21 ** 2
        out.append((g, abs(s11), abs(s21), la, q11, q21, lb))
        if not quiet:
            print(f"{g:6.1f} GHz  S11 {20*math.log10(abs(s11)):7.2f} / {20*math.log10(q11):7.2f}  S21 {20*math.log10(abs(s21)):8.4f} / "
                  f"{20*math.log10(q21):8.4f} dB  lost {la:.5f} / {lb:.5f}" + (f"  ({la / lb:.3f})" if lb > 1e-5 else ""))
    return out

if __name__ == "__main__":
    compare(sys.argv[1], sys.argv[2])
