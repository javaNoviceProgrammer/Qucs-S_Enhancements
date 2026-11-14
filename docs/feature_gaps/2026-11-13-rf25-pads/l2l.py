"""The line's own propagation from two lengths with the same ends (L-2L): T_long T_short^-1 has the eigenvalues
exp(-+gamma dL), whatever the pads and ports are. Prints alpha (dB/mm) and eps_eff at each common frequency.
usage: python3 -I l2l.py short long dL_um   (each a .s2p or a Palace port-S.csv)"""
import cmath, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loss import read

def palace(path):
    rows = []
    for line in open(path):
        v = line.split(",")
        try:
            f = float(v[0])
        except ValueError:
            continue
        s11 = cmath.rect(10 ** (float(v[1]) / 20), math.radians(float(v[2])))
        s21 = cmath.rect(10 ** (float(v[3]) / 20), math.radians(float(v[4])))
        # (Symmetric and reciprocal: one port driven.)
        rows.append((f * 1e9, s11, s21, s21, s11))
    return rows

def load(path):
    return {round(r[0] / 1e9, 6): r[1:] for r in (palace(path) if path.endswith(".csv") else read(path))}

def tmatrix(s11, s21, s12, s22):
    return [[-(s11 * s22 - s12 * s21) / s21, s11 / s21], [-s22 / s21, 1 / s21]]

def l2l(short, long, dl):
    a, b = load(short), load(long)
    out = []
    for f in sorted(set(a) & set(b)):
        ta, tb = tmatrix(*a[f]), tmatrix(*b[f])
        det = ta[0][0] * ta[1][1] - ta[0][1] * ta[1][0]
        inv = [[ta[1][1] / det, -ta[0][1] / det], [-ta[1][0] / det, ta[0][0] / det]]
        m = [[sum(tb[i][k] * inv[k][j] for k in range(2)) for j in range(2)] for i in range(2)]
        tr, dm = m[0][0] + m[1][1], m[0][0] * m[1][1] - m[0][1] * m[1][0]
        r = cmath.sqrt(tr * tr / 4 - dm)
        lam = max((tr / 2 + r, tr / 2 - r), key=abs)   # exp(+gamma dL) of the wave going the other way
        g = cmath.log(lam) / (dl * 1e-6)
        alpha, beta = g.real, g.imag
        k0 = 2 * math.pi * f * 1e9 / 299792458.0
        # (beta's branch: the one nearest a line of eps_eff 2.37.)
        n = round((k0 * math.sqrt(2.37) * dl * 1e-6 - abs(beta) * dl * 1e-6) / (2 * math.pi))
        beta = abs(beta) + 2 * math.pi * n / (dl * 1e-6)
        out.append((f, alpha * 20 / math.log(10) / 1e3, (beta / k0) ** 2))
    return out

if __name__ == "__main__":
    for f, a, e in l2l(sys.argv[1], sys.argv[2], float(sys.argv[3])):
        print(f"{f:6.1f} GHz  alpha {a:7.4f} dB/mm  eps_eff {e:.4f}")
