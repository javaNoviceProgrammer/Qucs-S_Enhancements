"""C = -Im(Y12)/w of a 2-port comb at each frequency asked; GMRES counts and time."""
import sys, json, time, numpy as np; sys.path.insert(0, "."); from run import Server
folder, names, freqs = sys.argv[1], sys.argv[2].split(","), [float(f) for f in sys.argv[3].split(",")]
extra = json.loads(sys.argv[4]) if len(sys.argv) > 4 else {}
s = Server(folder)
for name in names:
    for f in freqs:
        t0 = time.time()
        a = {"path": name + ".qrf25", "solver": "full_wave", "frequency": f"{f}[GHz]", "timeout": 7200}; a.update(extra)
        bad, ans, text = s.call("rf25_solve", a)
        dt = time.time() - t0
        if bad: print(name, f, "ERROR", text[:800], flush=True); continue
        sm = ans["s matrix"]; Sm = np.array(sm["re"]) + 1j * np.array(sm["im"])
        Z0 = 50; I = np.eye(2); Y = np.linalg.inv(Z0 * (I + Sm) @ np.linalg.inv(I - Sm))
        w = 2 * np.pi * f * 1e9
        C12 = -Y[0, 1].imag / w
        mx = ans.get("matrix", {})
        print(f"{name} {f} GHz: C12 {C12*1e15:.3f} fF  S11 {sm['dB'][0][0]:.5f} S21 {sm['dB'][1][0]:.5f} dB  {dt:.1f} s  unknowns {ans.get('unknowns')}  matrix {json.dumps(mx)}", flush=True)
        if "-v" in sys.argv:
            for l in ans.get("what it did", []): print("   -", l[:300])
s.close()
