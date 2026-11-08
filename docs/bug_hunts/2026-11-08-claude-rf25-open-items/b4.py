"""B4: the CPW (S 50 / G 20, 2 um... here thin, on 500 um quartz) drawn inside GSG pads with tapers against the same
line drawn as rectangles; GSG tips on the line 200 um in from the line's ends, 600 um apart."""
import sys; sys.path.insert(0, "."); from mk import *
out = sys.argv[1]; cells = int(sys.argv[2]) if len(sys.argv) > 2 else 6; L = int(sys.argv[3]) if len(sys.argv) > 3 else 1000
stack = [node("rf_ground", "gnd", "Ground", {"kind": "open"}),
         node("rf_substrate", "sub", "Quartz", {"thickness": "500[um]", "epsr": "3.8", "tand": "0", "sigma": "0"}),
         node("rf_metal", "m1", "Au", {"thickness": "0[um]", "sigma": "1e12", "model": "sheet"}),
         node("rf_cover", "cov", "Cover", {"kind": "open"})]
LP, LT = 60, 80
x0, x1 = LP + LT, LP + LT + L; e = x1 + LT + LP
sig = [(0, -40), (LP, -40), (x0, -25), (x1, -25), (x1 + LT, -40), (e, -40), (e, 40), (x1 + LT, 40), (x1, 25), (x0, 25), (LP, 40), (0, 40)]
gup = [(0, 70), (LP, 70), (x0, 45), (x1, 45), (x1 + LT, 70), (e, 70), (e, 205), (0, 205)]
gdn = [(x, -y) for x, y in reversed(gup)]
sq = lambda x, y: {"x": f"{x}[um]", "y": f"{y}[um]", "width": "10[um]", "height": "10[um]"}
def tips(xa, xb):
    ports = []
    for i, x in enumerate((xa, xb)):
        ports.append(node("rf_internal_port", f"p{i+1}", f"P{i+1}", {"to": "Au", "return": "terminals", "returns": [f"T{2*i+1}", f"T{2*i+2}"], **sq(x, 0)}))
    for i, x in enumerate((xa, xb)):
        ports.append(node("rf_terminal", f"t{2*i+1}", f"T{2*i+1}", {"metal": "Au", **sq(x, 125)}))
        ports.append(node("rf_terminal", f"t{2*i+2}", f"T{2*i+2}", {"metal": "Au", **sq(x, -125)}))
    return ports
st = {"from": "10[GHz]", "to": "30[GHz]", "points": "3"}
model(f"{out}/inpads_c{cells}_L{L}.qrf25", [(1, sig), (1, gup), (1, gdn)], stack, [(1, 0, "Au")], tips(x0 + 200, x1 - 200), cells=cells, study=st)
model(f"{out}/inplain_c{cells}_L{L}.qrf25", [(1, (x0, -25, x1, 25)), (1, (x0, 45, x1, 205)), (1, (x0, -205, x1, -45))], stack, [(1, 0, "Au")],
      tips(x0 + 200, x1 - 200), cells=cells, study=st)
