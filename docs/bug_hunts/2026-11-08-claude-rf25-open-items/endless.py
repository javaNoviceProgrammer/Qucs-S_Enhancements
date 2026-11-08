import sys, json; sys.path.insert(0, "."); from mk import *; from run import Server
out = sys.argv[1]
for t, mm, cells in ((1, "faces", 3), (0, "sheet", 3), (1, "faces", 4), (0, "sheet", 4)):
    name = f"strips_{mm}_t{t}_c{cells}"
    stack = [node("rf_ground", "gnd", "Ground", {"kind": "perfect"}),
             node("rf_substrate", "sub", "Oxide", {"thickness": "4[um]", "epsr": "4.1", "tand": "0", "sigma": "0"}),
             node("rf_metal", "m1", "M", {"thickness": f"{t}[um]", "sigma": "1e12", "model": mm}),
             node("rf_dielectric", "ild", "Oxide2", {"thickness": f"{4 - t}[um]", "epsr": "4.1", "tand": "0"}),
             node("rf_cover", "cov", "Cover", {"kind": "open"})]
    model(f"{out}/{name}.qrf25", [(1, (0, 2 * i, 40, 2 * i + 1)) for i in range(8)], stack, [(1, 0, "M")], [], cells=cells,
          study={"kind": "quasi_static", "from": "1[GHz]", "to": "1[GHz]", "points": "1"})
    s = Server(out)
    bad, ans, text = s.call("rf25_solve", {"path": name + ".qrf25", "solver": "quasi_static", "timeout": 3600})
    s.close()
    if bad: print(name, "ERROR", text[:300]); continue
    cl = ans.get("coupled lines", {})
    C = cl.get("per metre", {}).get("C")
    if not C: print(name, "no per-metre matrix:", list(ans.keys()), text[:500]); continue
    i = 3   # a middle strip; Maxwell form: off-diagonal negative
    gap = sum(-C[i][j] for j in range(8) if abs(j - i) % 2 == 1) / 2
    print(f"{name}: per gap {gap * 1e12:.2f} pF/m (C to neighbours {[round(-C[i][j]*1e12,2) for j in range(8) if j != i]})")
