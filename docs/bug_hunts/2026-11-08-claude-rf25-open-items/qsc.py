"""Quasi-static: each net's mutual capacitance matrix (fF) and the solve's lines on walls."""
import sys, json; sys.path.insert(0, "."); from run import Server
folder, names = sys.argv[1], sys.argv[2].split(",")
s = Server(folder)
for name in names:
    bad, ans, text = s.call("rf25_solve", {"path": name + ".qrf25", "solver": "quasi_static", "timeout": 3600})
    if bad: print(name, "ERROR", text[:300]); continue
    nets = ans.get("nets", [])
    print(name, "c (fF):", [[round(v * 1e15, 3) for v in n.get("c", [])] for n in nets])
s.close()
