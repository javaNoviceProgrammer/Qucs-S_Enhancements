"""QFEM's mode search on the CPW with PML near 90-100 GHz (cmp500/qfem_cpw_modes.qfem, cut to 70-100 GHz): n_eff, loss
and Z_PV of each mode the search returns, as qfem_modes_run.py takes them."""
import sys, json, os, time; sys.path.insert(0, "."); from run import Server
folder = sys.argv[1]; freqs = [70, 80, 90, 100]
m = json.load(open(os.path.join(folder, "qfem_cpw_modes.qfem")))
m["studies"][0]["steps"][0]["parameters"][0][1] = ", ".join(str(f) for f in freqs)
p = os.path.join(folder, "modes_70_100.qfem"); json.dump(m, open(p, "w"), indent=1)
s = Server(folder); t0 = time.time()
bad, ans, text = s.call("fem_solve", {"path": p, "timeout": 7200, "max_chars": 4000})
print(f"solved in {time.time()-t0:.0f} s, error {bad}: {text[:300] if bad else ''}", flush=True)
for pt, fg in enumerate(freqs, 1):
    for mode in (1, 2):
        _, a, t = s.call("fem_evaluate", {"path": p, "expression": "emw.neff; emw.loss", "point": pt, "mode": mode})
        if "values" not in a: print(fg, mode, t[:300]); continue
        vals = {v["name"]: v["SI"] for v in a["values"]}
        V = []
        for ex in ("emw.Ex", "emw.Ex_im"):
            _, a, _ = s.call("fem_evaluate", {"path": p, "expression": ex, "over": "boundary", "selection": {"numbers": [15]}, "kind": "integral", "point": pt, "mode": mode})
            V.append(a["values"][0]["SI"])
        _, a, _ = s.call("fem_evaluate", {"path": p, "expression": "emw.Sz", "over": "domain", "selection": {"numbers": [2, 3, 4, 5, 6]}, "kind": "integral", "point": pt, "mode": mode})
        P = a["values"][0]["SI"]
        print(f"{fg} GHz mode {mode}: n_eff {vals['emw.neff']:.5f} loss {vals['emw.loss']:.1f} dB/m  Z_PV {(V[0]**2+V[1]**2)/(4*P) if P else float('nan'):.2f} Ohm", flush=True)
s.close()
