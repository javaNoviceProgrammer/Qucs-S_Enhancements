"""De-embedding from 30 to 150 GHz: plain lines at two lengths, each with its edge ports calibrated (de-embedded at
their faces) and not. What de-embedding should leave is a uniform line: its gamma and Z the same at either length,
L-2L's from the uncalibrated pair, and the 2D solves'.
  ms:  a microstrip 95 um wide on 100 um of alumina (eps_r 9.8) over a ground plane, perfect metal: 1000, 2000 um.
  cpw: the benchmark's coplanar line (S 50, G 20, grounds 160 um, 2 um of gold solved as Automatic, on 500 um of
       quartz open below): 500, 1000 um.
usage: python3 -I make_lines.py <out folder> [cells across]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk import node, model, edge

out = os.path.abspath(sys.argv[1]); cells = int(sys.argv[2]) if len(sys.argv) > 2 else 6
os.makedirs(out, exist_ok=True)
study = {"from": "30[GHz]", "to": "150[GHz]", "points": "61"}
ms = [node("rf_ground", "gnd", "Ground", {"kind": "perfect"}),
      node("rf_substrate", "sub", "Alumina", {"thickness": "100[um]", "epsr": "9.8", "tand": "0", "sigma": "0"}),
      node("rf_metal", "m1", "M1", {"thickness": "0[um]", "sigma": "1e12", "model": "sheet"}),
      node("rf_cover", "cov", "Cover", {"kind": "open"})]
cpw = [node("rf_ground", "gnd", "Ground", {"kind": "open"}),
       node("rf_substrate", "sub", "Quartz", {"thickness": "500[um]", "epsr": "3.8", "tand": "0", "sigma": "0"}),
       node("rf_metal", "m1", "M1", {"thickness": "2[um]", "sigma": "4.1e7", "model": "automatic"}),
       node("rf_cover", "cov", "Cover", {"kind": "open"})]
for cal in (True, False):
    tag = "cal" if cal else "raw"
    for L in (1000, 2000):
        ports = [edge("p1", "M1", 0, 0, "-x", calibrate=cal), edge("p2", "M1", L, 0, "+x", calibrate=cal)]
        model(f"{out}/ms_L{L}_{tag}_c{cells}.qrf25", [(1, (0, -47.5, L, 47.5))], ms, [(1, 0, "M1")], ports, cells=cells, study=study)
    for L in (500, 1000):
        ports = [edge("p1", "M1", 0, 0, "-x", ret="beside", calibrate=cal), edge("p2", "M1", L, 0, "+x", ret="beside", calibrate=cal)]
        shapes = [(1, (0, -25, L, 25)), (1, (0, 45, L, 205)), (1, (0, -205, L, -45))]
        model(f"{out}/cpw_L{L}_{tag}_c{cells}.qrf25", shapes, cpw, [(1, 0, "M1")], ports, cells=cells, study=study)
print("written", out)
