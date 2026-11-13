"""Reads Touchstone 2-ports and prints, at each frequency, |S11|, |S21| (dB) and the power lost, 1 - |S11|^2 - |S21|^2.
usage: python3 -I loss.py a.s2p [b.s2p ...]"""
import cmath, math, sys

def read(path):
    rows, fmt, unit = [], "MA", 1e9
    for line in open(path):
        line = line.split("!")[0].strip()
        if not line or line.startswith("["):
            continue
        if line.startswith("#"):
            t = line[1:].upper().split()
            unit = {"HZ": 1, "KHZ": 1e3, "MHZ": 1e6, "GHZ": 1e9}.get(t[0], 1e9)
            fmt = t[2] if len(t) > 2 else "MA"
            continue
        v = [float(x) for x in line.split()]
        if len(v) < 9:
            continue
        def c(a, b):
            if fmt == "RI": return complex(a, b)
            if fmt == "DB": return cmath.rect(10 ** (a / 20), math.radians(b))
            return cmath.rect(a, math.radians(b))
        rows.append((v[0] * unit, c(v[1], v[2]), c(v[3], v[4]), c(v[5], v[6]), c(v[7], v[8])))
    return rows

if __name__ == "__main__":
    for path in sys.argv[1:]:
        print(path.split("/")[-1])
        for f, s11, s21, s12, s22 in read(path):
            lost = 1 - abs(s11) ** 2 - abs(s21) ** 2
            print(f"  {f / 1e9:6.1f} GHz  S11 {20 * math.log10(abs(s11)):7.2f}  S21 {20 * math.log10(abs(s21)):8.4f} dB  lost {lost:.5f}")
