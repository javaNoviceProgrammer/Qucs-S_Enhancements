"""Phase 6 of RF 2.5D's accuracy limits, Palace's side: make_rf25.py's GSG pads and line in 3D, for Palace's driven
solve. Half model: the line's axis (y = 0) a magnetic wall, the CPW mode alone. x along the line, y across it, z up;
the quartz from -500 to 0 um, open below, air 1000 um above and below, a first-order absorbing box 600 um beyond the
pads' ends and 1000 um out from the axis.
  pec: the metal a perfect sheet at z = 0 (PEC on interior faces).
  au:  the metal 2 um of gold cut out of the mesh, its faces a Conductivity boundary (as the benchmark's comparison 1).
Each end a lumped port across the gap between the signal pad and its ground pad, on the pads' first 10 um (x 0..10,
y 40..70, at z = 0): 100 Ohm in the half, the whole structure's 50.
The linear solve AMS-preconditioned GMRES: Palace's default, a sparse direct coarse solve, peaked at 29 GB on
the smallest of these (2.3 million unknowns).
Needs gmsh's Python API on PYTHONPATH (its Homebrew lib folder).
usage: python3 make_palace.py <out folder> <pec|au> <L um> [edge size um] [far size um]"""
import json, math, os, sys

import gmsh

out, kind, L = os.path.abspath(sys.argv[1]), sys.argv[2], float(sys.argv[3])
hm = float(sys.argv[4]) if len(sys.argv) > 4 else (3.0 if kind == "pec" else 2.0)
far = float(sys.argv[5]) if len(sys.argv) > 5 else 150.0
LP, LT, H, EPS, SIGMA = 60.0, 80.0, 500.0, 3.8, 4.1e7
T = 2.0 if kind == "au" else 0.0
XM, YM, AIR = 600.0, 1000.0, 1000.0
x0, x1 = LP + LT, LP + LT + L
e = x1 + LT + LP
SIG = [(0, 0), (e, 0), (e, 40), (x1 + LT, 40), (x1, 25), (x0, 25), (LP, 40), (0, 40)]
GND = [(0, 70), (LP, 70), (x0, 45), (x1, 45), (x1 + LT, 70), (e, 70), (e, 205), (0, 205)]
PORTS = [(0.0, 10.0), (e - 10.0, e)]
os.makedirs(out, exist_ok=True)


def inside(poly, x, y):
    c = False
    for (ax, ay), (bx, by) in zip(poly, poly[1:] + poly[:1]):
        if (ay > y) != (by > y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
            c = not c
    return c


gmsh.initialize()
gmsh.option.setNumber("General.Verbosity", 2)
gmsh.model.add("pads")
occ = gmsh.model.occ
zb, zt = -H - AIR, T + AIR
box = occ.addBox(-XM, 0, zb, e + 2 * XM, YM, zt - zb)
sub = occ.addBox(-XM, 0, -H, e + 2 * XM, YM, H)


def polygon(pts, z):
    p = [occ.addPoint(x, y, z) for x, y in pts]
    lines = [occ.addLine(a, b) for a, b in zip(p, p[1:] + p[:1])]
    return occ.addPlaneSurface([occ.addCurveLoop(lines)])


tools = [(3, sub)]
for poly in (SIG, GND):
    s = polygon(poly, 0.0)
    if kind == "au":
        ext = occ.extrude([(2, s)], 0, 0, T)
        tools += [d for d in ext if d[0] == 3]
    else:
        tools.append((2, s))
for xa, xb in PORTS:
    tools.append((2, occ.addRectangle(xa, 40, 0, xb - xa, 30)))
occ.fragment([(3, box)], tools)
occ.synchronize()

# The volumes: the metal (gold, cut out), the quartz, the air.
metal_vols, sub_vols, air_vols = [], [], []
for _, v in gmsh.model.getEntities(3):
    x, y, z = occ.getCenterOfMass(3, v)
    if kind == "au" and 0 < z < T and (inside(SIG, x, y) or inside(GND, x, y)):
        metal_vols.append(v)
    elif z < 0:
        sub_vols.append(v)
    else:
        air_vols.append(v)
kept = set(sub_vols + air_vols)

# The faces: each kept face sorted by where it is.
groups = {"metal": [], "outer": [], "sym": [], "port1": [], "port2": []}
eps = 1e-6
for _, s in gmsh.model.getEntities(2):
    up, _ = gmsh.model.getAdjacencies(2, s)
    if not any(v in kept for v in up):
        continue
    x, y, z = occ.getCenterOfMass(2, s)
    bb = gmsh.model.getBoundingBox(2, s)
    if bb[1] > -eps and bb[4] < eps:
        groups["sym"].append(s)
    elif abs(bb[0] + XM) < eps and abs(bb[3] + XM) < eps or abs(bb[0] - e - XM) < eps and abs(bb[3] - e - XM) < eps \
            or abs(bb[1] - YM) < eps and abs(bb[4] - YM) < eps or abs(bb[2] - zb) < eps and abs(bb[5] - zb) < eps \
            or abs(bb[2] - zt) < eps and abs(bb[5] - zt) < eps:
        groups["outer"].append(s)
    elif abs(z) < eps and 40 < y < 70 and 0 < x < 10:
        groups["port1"].append(s)
    elif abs(z) < eps and 40 < y < 70 and e - 10 < x < e:
        groups["port2"].append(s)
    elif kind == "au" and any(v in metal_vols for v in up):
        groups["metal"].append(s)
    elif kind == "pec" and abs(z) < eps and (inside(SIG, x, y) or inside(GND, x, y)):
        groups["metal"].append(s)
if kind == "au":
    occ.remove([(3, v) for v in metal_vols])
    occ.synchronize()
gmsh.model.addPhysicalGroup(3, sub_vols, 1, "substrate")
gmsh.model.addPhysicalGroup(3, air_vols, 2, "air")
tags = {"metal": 1, "outer": 3, "sym": 5, "port1": 6, "port2": 7}
for name, faces in groups.items():
    if not faces:
        raise SystemExit("no faces for " + name)
    gmsh.model.addPhysicalGroup(2, faces, tags[name], name)

# Sizes: fine along the metal's edges, graded out.
edges = set()
for s in groups["metal"] + groups["port1"] + groups["port2"]:
    for _, c in gmsh.model.getBoundary([(2, s)], oriented=False):
        edges.add(abs(c))
gmsh.option.setNumber("Mesh.MeshSizeExtendFromBoundary", 0)
gmsh.option.setNumber("Mesh.MeshSizeFromPoints", 0)
gmsh.option.setNumber("Mesh.MeshSizeFromCurvature", 0)
gmsh.option.setNumber("Mesh.MeshSizeMax", far)
f = gmsh.model.mesh.field
f.add("Distance", 1)
f.setNumbers(1, "CurvesList", sorted(edges))
f.setNumber(1, "Sampling", 200)
f.add("Threshold", 2)
f.setNumber(2, "InField", 1)
f.setNumber(2, "SizeMin", hm)
f.setNumber(2, "SizeMax", far)
f.setNumber(2, "DistMin", hm)
f.setNumber(2, "DistMax", 900)
f.setAsBackgroundMesh(2)
gmsh.option.setNumber("Mesh.Algorithm", 6)
gmsh.option.setNumber("Mesh.Algorithm3D", 10)
gmsh.option.setNumber("Mesh.MshFileVersion", 2.2)
gmsh.model.mesh.generate(3)
name = f"pads_{kind}_L{L:g}"
gmsh.write(os.path.join(out, name + ".msh"))
tets = sum(len(t) for t in gmsh.model.mesh.getElements(3)[1])
gmsh.finalize()

mats = [{"Attributes": [1], "Permittivity": EPS, "Permeability": 1.0}, {"Attributes": [2], "Permittivity": 1.0, "Permeability": 1.0}]
bounds = {"PMC": {"Attributes": [5]}, "Absorbing": {"Attributes": [3], "Order": 1},
          "LumpedPort": [{"Index": 1, "R": 100.0, "Excitation": True, "Elements": [{"Attributes": [6], "Direction": "+Y"}]},
                         {"Index": 2, "R": 100.0, "Elements": [{"Attributes": [7], "Direction": "+Y"}]}]}
if kind == "au":
    bounds["Conductivity"] = [{"Attributes": [1], "Conductivity": SIGMA, "Thickness": T}]
else:
    bounds["PEC"] = {"Attributes": [1]}
config = {"Problem": {"Type": "Driven", "Verbose": 2, "Output": "postpro_" + name},
          "Model": {"Mesh": name + ".msh", "L0": 1.0e-6},
          "Domains": {"Materials": mats},
          "Boundaries": bounds,
          "Solver": {"Order": 2, "Device": "CPU",
                     "Driven": {"Samples": [{"Type": "Linear", "MinFreq": 1.0, "MaxFreq": 100.0, "FreqStep": 1.0}],
                                "Save": [], "AdaptiveTol": 1.0e-3},
                     "Linear": {"Type": "AMS", "KSPType": "GMRES", "Tol": 1.0e-8, "MaxIts": 1000}}}
json.dump(config, open(os.path.join(out, name + ".json"), "w"), indent=1)
print(f"{name}: {tets} tetrahedra, faces " + ", ".join(f"{k} {len(v)}" for k, v in groups.items()))
