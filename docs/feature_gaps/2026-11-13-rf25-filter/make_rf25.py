"""filter.py's bandstop filter as an RF 2.5D model: edge ports at the feeds' ends, returning through the ground
plane, de-embedded (calibrated); perfect metal, the alumina lossless; 4 to 16 GHz.
usage: python3 -I make_rf25.py <out folder> [cells across] [points]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk import node, model, edge
import filter as F

out = os.path.abspath(sys.argv[1]); cells = int(sys.argv[2]) if len(sys.argv) > 2 else 6
points = sys.argv[3] if len(sys.argv) > 3 else "121"
os.makedirs(out, exist_ok=True)
stack = [node("rf_ground", "gnd", "Ground", {"kind": "perfect"}),
         node("rf_substrate", "sub", "Alumina", {"thickness": f"{F.H}[um]", "epsr": str(F.EPS), "tand": "0", "sigma": "0"}),
         node("rf_metal", "m1", "M1", {"thickness": "0[um]", "sigma": "1e12", "model": "sheet"}),
         node("rf_cover", "cov", "Cover", {"kind": "open"})]
ports = [edge("p1", "M1", 0, 0, "-x", calibrate=True), edge("p2", "M1", F.LENGTH, 0, "+x", calibrate=True)]
model(f"{out}/filter_c{cells}.qrf25", [(1, F.outline())], stack, [(1, 0, "M1")], ports, cells=cells,
      study={"from": "4[GHz]", "to": "16[GHz]", "points": points})
print("written", out)
