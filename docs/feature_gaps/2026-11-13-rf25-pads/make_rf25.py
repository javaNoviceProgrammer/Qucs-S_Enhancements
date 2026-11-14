"""Phase 6 of RF 2.5D's accuracy limits: GSG pads with tapers onto the benchmark's coplanar line (S 50, G 20, grounds
160 wide, on 500 um of quartz, eps_r 3.8, open below), lines of 500 and 1000 um between them. The pads are B4's:
the signal pad 80 um wide and 60 long, an 80 um taper to the line, the ground pads from 70 um out. Probed at the pads'
outer ends: an internal port on the signal pad's first 10 um, its returns the ground pads' beside it.
usage: python3 -I make_rf25.py <out folder> [cells across]
Writes pads_{au,pec,sheet}_L{500,1000}_c<cells>.qrf25 (and their .gds)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk import node, model

out = os.path.abspath(sys.argv[1]); cells = int(sys.argv[2]) if len(sys.argv) > 2 else 3
os.makedirs(out, exist_ok=True)
LP, LT = 60, 80

def shapes(L):
    x0, x1 = LP + LT, LP + LT + L
    e = x1 + LT + LP
    sig = [(0, -40), (LP, -40), (x0, -25), (x1, -25), (x1 + LT, -40), (e, -40), (e, 40), (x1 + LT, 40), (x1, 25), (x0, 25), (LP, 40), (0, 40)]
    gup = [(0, 70), (LP, 70), (x0, 45), (x1, 45), (x1 + LT, 70), (e, 70), (e, 205), (0, 205)]
    gdn = [(x, -y) for x, y in reversed(gup)]
    return e, [(1, sig), (1, gup), (1, gdn)]

def metal(kind):
    if kind == "au": return {"thickness": "2[um]", "sigma": "4.1e7", "model": "automatic"}
    if kind == "sheet": return {"thickness": "2[um]", "sigma": "4.1e7", "model": "sheet"}
    return {"thickness": "0[um]", "sigma": "1e12", "model": "sheet"}

def square(x, y):
    return {"x": f"{x}[um]", "y": f"{y}[um]", "width": "10[um]", "height": "10[um]"}

for kind in ("au", "pec", "sheet"):
    stack = [node("rf_ground", "gnd", "Ground", {"kind": "open"}),
             node("rf_substrate", "sub", "Quartz", {"thickness": "500[um]", "epsr": "3.8", "tand": "0", "sigma": "0"}),
             node("rf_metal", "m1", "Au", metal(kind)),
             node("rf_cover", "cov", "Cover", {"kind": "open"})]
    for L in (500, 1000):
        e, s = shapes(L)
        ports = []
        for i, x in enumerate((5, e - 5)):
            ports.append(node("rf_internal_port", f"p{i+1}", f"P{i+1}",
                              {"to": "Au", "return": "terminals", "returns": [f"T{2*i+1}", f"T{2*i+2}"], **square(x, 0)}))
        for i, x in enumerate((5, e - 5)):
            ports.append(node("rf_terminal", f"t{2*i+1}", f"T{2*i+1}", {"metal": "Au", **square(x, 75)}))
            ports.append(node("rf_terminal", f"t{2*i+2}", f"T{2*i+2}", {"metal": "Au", **square(x, -75)}))
        model(f"{out}/pads_{kind}_L{L}_c{cells}.qrf25", s, stack, [(1, 0, "Au")], ports, cells=cells,
              study={"from": "5[GHz]", "to": "100[GHz]", "points": "20"})
print("written", out)
