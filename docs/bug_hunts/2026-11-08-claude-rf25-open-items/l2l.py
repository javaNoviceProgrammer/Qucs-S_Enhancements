"""eps_eff and alpha of a line by L-2L: the same structure at two lengths, T_2L T_L^-1 = the extra line's T."""
import sys, json, numpy as np; sys.path.insert(0, "."); from run import Server
folder, pairs, freqs, dL = sys.argv[1], [p.split("/") for p in sys.argv[2].split(",")], [float(f) for f in sys.argv[3].split(",")], float(sys.argv[4]) * 1e-6
s = Server(folder)
def T(name, f):
    bad, ans, text = s.call("rf25_solve", {"path": name + ".qrf25", "solver": "full_wave", "frequency": f"{f}[GHz]", "timeout": 3600})
    if bad: raise SystemExit(name + ": " + text[:400])
    S_ = np.array(ans["s matrix"]["re"]) + 1j * np.array(ans["s matrix"]["im"])
    s11, s12, s21, s22 = S_[0, 0], S_[0, 1], S_[1, 0], S_[1, 1]
    return np.array([[-(s11 * s22 - s12 * s21), s11], [-s22, 1]]) / s21, ans.get("unknowns")
for short, long_ in pairs:
    for f in freqs:
        t1, u1 = T(short, f); t2, u2 = T(long_, f)
        ev = np.linalg.eigvals(t2 @ np.linalg.inv(t1))
        g = np.log(ev) / dL
        g = g[np.argmax(np.abs(g.imag))] if True else g
        gam = -g if g.real < 0 or g.imag < 0 else g
        gam = complex(abs(gam.real), abs(gam.imag))
        w = 2 * np.pi * f * 1e9
        print(f"{short} / {long_} {f} GHz: eps_eff {(gam.imag * 299792458 / w) ** 2:.5f}  alpha {gam.real * 8.686:.2f} dB/m  unknowns {u1}", flush=True)
s.close()
