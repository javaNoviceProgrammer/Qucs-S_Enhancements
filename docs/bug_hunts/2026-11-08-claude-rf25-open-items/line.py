"""eps_eff, Z0 and alpha of a uniform line from its calibrated 2-port S (ABCD), at each frequency asked."""
import sys, json, time, numpy as np; sys.path.insert(0, "."); from run import Server
folder, names, freqs, L = sys.argv[1], sys.argv[2].split(","), [float(f) for f in sys.argv[3].split(",")], float(sys.argv[4]) * 1e-6
s = Server(folder)
for name in names:
    for f in freqs:
        t0 = time.time()
        bad, ans, text = s.call("rf25_solve", {"path": name + ".qrf25", "solver": "full_wave", "frequency": f"{f}[GHz]", "timeout": 3600})
        if bad: print(name, f, "ERROR", text[:600], flush=True); continue
        sm = ans["s matrix"]; S_ = np.array(sm["re"]) + 1j * np.array(sm["im"]); z0 = 50
        s11, s12, s21, s22 = S_[0, 0], S_[0, 1], S_[1, 0], S_[1, 1]
        A = ((1 + s11) * (1 - s22) + s12 * s21) / (2 * s21)
        B = z0 * ((1 + s11) * (1 + s22) - s12 * s21) / (2 * s21)
        C = ((1 - s11) * (1 - s22) - s12 * s21) / (2 * s21) / z0
        g = np.arccosh(A) / L
        if g.imag < 0: g = -g
        Zc = np.sqrt(B / C)
        w = 2 * np.pi * f * 1e9; c = 299792458.0
        print(f"{name} {f} GHz: eps_eff {(g.imag * c / w) ** 2:.4f}  Z0 {Zc.real:.2f} Ohm  alpha {g.real * 8.686:.1f} dB/m  "
              f"S21 {sm['dB'][1][0]:.4f} dB {sm['degrees'][1][0]:.2f} deg  {time.time() - t0:.0f} s  {json.dumps(ans.get('matrix'))}", flush=True)
s.close()
