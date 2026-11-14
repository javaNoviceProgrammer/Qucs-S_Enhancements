"""What de-embedding left of each line: from its 2-port S (any reference), its ABCD's (A + D)/2 = cosh(gamma l) and
Z = sqrt(B/C) - alpha (dB/m), eps_eff, Z - and the power it loses, 1 - |S11|^2 - |S21|^2, which a lossless line
de-embedded should not. L-2L from the uncalibrated pair: gamma alone, the ports cancelled.
usage: python3 -I lines.py <folder> <ms|cpw> [step GHz]"""
import cmath, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loss import read

C0 = 299792458.0

def line(path, l):
    out = {}
    for f, s11, s21, s12, s22 in read(path):
        A = ((1 + s11) * (1 - s22) + s12 * s21) / (2 * s21)
        B = 50 * ((1 + s11) * (1 + s22) - s12 * s21) / (2 * s21)
        C = ((1 - s11) * (1 - s22) - s12 * s21) / (2 * s21) / 50
        D = ((1 - s11) * (1 + s22) + s12 * s21) / (2 * s21)
        g = cmath.acosh((A + D) / 2) / l
        if g.real < 0: g = -g
        k0 = 2 * math.pi * f / C0
        beta = g.imag
        out[round(f / 1e9, 3)] = dict(alpha=g.real * 20 / math.log(10), beta=beta, k0=k0, z=cmath.sqrt(B / C),
                                     lost=1 - abs(s11) ** 2 - abs(s21) ** 2, s11=abs(s11))
    return out

def l2l(short, long, dl, guess):
    out = {}
    a = {round(f / 1e9, 3): (s11, s21, s12, s22) for f, s11, s21, s12, s22 in read(short)}
    b = {round(f / 1e9, 3): (s11, s21, s12, s22) for f, s11, s21, s12, s22 in read(long)}
    def t(s11, s21, s12, s22): return [[-(s11 * s22 - s12 * s21) / s21, s11 / s21], [-s22 / s21, 1 / s21]]
    for f in sorted(set(a) & set(b)):
        ta, tb = t(*a[f]), t(*b[f])
        det = ta[0][0] * ta[1][1] - ta[0][1] * ta[1][0]
        inv = [[ta[1][1] / det, -ta[0][1] / det], [-ta[1][0] / det, ta[0][0] / det]]
        m = [[sum(tb[i][k] * inv[k][j] for k in range(2)) for j in range(2)] for i in range(2)]
        tr, dm = m[0][0] + m[1][1], m[0][0] * m[1][1] - m[0][1] * m[1][0]
        r = cmath.sqrt(tr * tr / 4 - dm)
        lam = max((tr / 2 + r, tr / 2 - r), key=abs)
        g = cmath.log(lam) / dl
        # (Its phase unwrapped: the turn nearest a line of eps_eff guess.)
        k0 = 2 * math.pi * f * 1e9 / C0
        want = k0 * math.sqrt(guess)
        cands = [sg * g.imag + 2 * math.pi * round((want - sg * g.imag) * dl / (2 * math.pi)) / dl for sg in (1, -1)]
        beta = min(cands, key=lambda b: abs(b - want))
        out[f] = dict(alpha=g.real * 20 / math.log(10), gimag=beta)
    return out

if __name__ == "__main__":
    folder, kind = sys.argv[1], sys.argv[2]
    step = float(sys.argv[3]) if len(sys.argv) > 3 else 10
    l1, l2 = (1000e-6, 2000e-6) if kind == "ms" else (500e-6, 1000e-6)
    n1, n2 = (f"{kind}_L{round(l * 1e6)}" for l in (l1, l2))
    c1, c2 = line(f"{folder}/{n1}_cal_c6.s2p", l1), line(f"{folder}/{n2}_cal_c6.s2p", l2)
    r = l2l(f"{folder}/{n1}_raw_c6.s2p", f"{folder}/{n2}_raw_c6.s2p", l2 - l1, 6.7 if kind == "ms" else 2.36)
    print(f"{kind}: de-embedded {round(l1*1e6)} / {round(l2*1e6)} um, and L-2L of the uncalibrated pair")
    print("  GHz | alpha dB/m: short   long   L-2L | eps_eff: short   long    L-2L | Z: short   long | lost: short    long")
    for f in sorted(c1):
        if abs(f / step - round(f / step)) > 1e-6: continue
        a, b = c1[f], c2[f]
        # eps_eff from beta, the branch the nearer the L-2L one (beta l taken mod 2 pi).
        def eps(x, l):
            want = x["k0"] * math.sqrt(6.7 if kind == "ms" else 2.36)
            cands = [sg * x["beta"] + 2 * math.pi * round((want - sg * x["beta"]) * l / (2 * math.pi)) / l for sg in (1, -1)]
            return (min(cands, key=lambda b: abs(b - want)) / x["k0"]) ** 2
        er = (r[f]["gimag"] / a["k0"]) ** 2 if f in r else float("nan")
        print(f"{f:6.0f} | {a['alpha']:7.1f} {b['alpha']:7.1f} {r[f]['alpha']:7.1f} | {eps(a, l1):7.4f} {eps(b, l2):7.4f} {er:7.4f} |"
              f" {a['z'].real:6.2f} {b['z'].real:6.2f} | {a['lost']:+.5f} {b['lost']:+.5f}")
