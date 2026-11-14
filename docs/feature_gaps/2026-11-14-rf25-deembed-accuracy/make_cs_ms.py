"""make_lines.py's microstrip alone: its cross-section for Palace's 2D mode solve (BoundaryMode), Z0 and eps_eff at a
frequency. Half model, x across from the axis (a magnetic wall), y up: the alumina from 0 to 100 um on a perfect
ground, the strip (half of it, 47.5 um) a perfect line at y = 100, air above, a first-order absorbing box.
Needs gmsh's Python API on PYTHONPATH.
usage: python3 make_cs_ms.py <out folder> [edge size um] [GHz ...]"""
import json, os, sys
import gmsh
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
class F:
    H, W, EPS = 100.0, 95.0, 9.8   # the microstrip of make_lines.py

out = os.path.abspath(sys.argv[1]); hm = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
ghz = [float(g) for g in sys.argv[3:]] or [4.0, 10.0, 16.0]
X, TOP, H, half = 3000.0, 3000.0, F.H, F.W / 2
os.makedirs(out, exist_ok=True)
gmsh.initialize(); gmsh.option.setNumber("General.Verbosity", 2)
occ = gmsh.model.occ
whole = occ.addRectangle(0, 0, 0, X, H + TOP)
sub = occ.addRectangle(0, 0, 0, X, H)
a, b = occ.addPoint(0, H, 0), occ.addPoint(half, H, 0)
strip = occ.addLine(a, b)
_, pieces = occ.fragment([(2, whole)], [(2, sub), (1, strip)])
occ.synchronize()
strip_curves = {t for d, t in pieces[2] if d == 1}
subs, airs = [], []
for _, s in gmsh.model.getEntities(2):
    (subs if occ.getCenterOfMass(2, s)[1] < H else airs).append(s)
groups = {"strip": [], "ground": [], "outer": [], "sym": []}
e = 1e-6
for _, c in gmsh.model.getEntities(1):
    bb = gmsh.model.getBoundingBox(1, c)
    if c in strip_curves: groups["strip"].append(c)
    elif abs(bb[1]) < e and abs(bb[4]) < e: groups["ground"].append(c)
    elif abs(bb[0]) < e and abs(bb[3]) < e: groups["sym"].append(c)
    elif abs(bb[0] - X) < e and abs(bb[3] - X) < e or abs(bb[1] - H - TOP) < e and abs(bb[4] - H - TOP) < e: groups["outer"].append(c)
gmsh.model.addPhysicalGroup(2, subs, 1, "substrate"); gmsh.model.addPhysicalGroup(2, airs, 2, "air")
for k, (name, cs) in enumerate(groups.items()):
    gmsh.model.addPhysicalGroup(1, cs, {"strip": 1, "ground": 2, "outer": 3, "sym": 5}[name], name)
gmsh.option.setNumber("Mesh.MeshSizeExtendFromBoundary", 0); gmsh.option.setNumber("Mesh.MeshSizeFromPoints", 0)
f = gmsh.model.mesh.field
f.add("Distance", 1); f.setNumbers(1, "PointsList", [b]); f.setNumbers(1, "CurvesList", sorted(strip_curves)); f.setNumber(1, "Sampling", 400)
f.add("Threshold", 2); f.setNumber(2, "InField", 1); f.setNumber(2, "SizeMin", hm); f.setNumber(2, "SizeMax", 150)
f.setNumber(2, "DistMin", hm); f.setNumber(2, "DistMax", 1500); f.setAsBackgroundMesh(2)
gmsh.option.setNumber("Mesh.MshFileVersion", 2.2)
gmsh.model.mesh.generate(2); gmsh.write(os.path.join(out, "cs.msh")); gmsh.finalize()
for g in ghz:
    cfg = {"Problem": {"Type": "BoundaryMode", "Verbose": 1, "Output": f"postpro/f{g:g}"},
           "Model": {"Mesh": "cs.msh", "L0": 1.0e-6},
           "Domains": {"Materials": [{"Attributes": [1], "Permittivity": F.EPS, "Permeability": 1.0},
                                     {"Attributes": [2], "Permittivity": 1.0, "Permeability": 1.0}]},
           "Boundaries": {"PEC": {"Attributes": [1, 2]}, "PMC": {"Attributes": [5]}, "Absorbing": {"Attributes": [3], "Order": 1},
                          "Postprocessing": {"Impedance": [{"Index": 1, "VoltagePath": [[1.0, 0.0], [1.0, H]]}]}},
           "Solver": {"Order": 3, "BoundaryMode": {"Freq": g, "N": 1, "Save": 0, "Target": 2.6, "Tol": 1e-9}, "Linear": {"Tol": 1e-10}}}
    json.dump(cfg, open(os.path.join(out, f"f{g:g}.json"), "w"), indent=1)
print("written", out)
