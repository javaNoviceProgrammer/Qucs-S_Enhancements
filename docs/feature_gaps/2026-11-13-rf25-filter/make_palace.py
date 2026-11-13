"""filter.py's bandstop filter in 3D for Palace's driven solve. Half model: the line's axis (y = 0) a magnetic wall.
x along the line, y across, z up; the alumina from 0 to 254 um on a perfect ground plane (the box's floor), the
filter a perfect sheet on it, air above to the box's top. The board as large as the box; its ends, far side and top
a first-order absorbing boundary. Each end a lumped port from the ground plane up to the strip's end, on the end face
(x 0 or the length, y 0 to the strip's half width, z 0 to 254 um): 100 Ohm in the half, the whole line's 50.
Needs gmsh's Python API on PYTHONPATH (its Homebrew lib folder).
usage: python3 make_palace.py <out folder> [edge size um] [far size um] [name] [cross] [full]
  cross: one pair of stubs alone (filter.cross()) in place of the filter; line: the line alone (filter.line()).
  full:  the whole model - both sides of the axis, no magnetic wall, 50 Ohm ports - in place of the half.
  lumped: lumped ports on the end faces in place of wave ports - which lose most of the power into the absorbing
          boundary beside them (a plain 50 Ohm line: S21 -4.5 dB, S11 -11 to -19 dB), kept only to show that."""
import json, os, sys

import gmsh

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import filter as F

out = os.path.abspath(sys.argv[1])
hm = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
far = float(sys.argv[3]) if len(sys.argv) > 3 else 400.0
name = sys.argv[4] if len(sys.argv) > 4 else f"filter_h{hm:g}"
alone = "cross" in sys.argv[5:] or "line" in sys.argv[5:]
bare = "line" in sys.argv[5:]
lumped = "lumped" in sys.argv[5:]
full = "full" in sys.argv[5:]
YM, AIR = F.W / 2 + F.STUB + 2000.0, 3000.0
L, H = (F.CROSS if alone else F.LENGTH), F.H
POLY = (F.line(half=not full) if bare else
        (F.cross() if alone else F.outline()) if full else (F.cross(half=True) if alone else F.outline(half=True)))
Y0 = -YM if full else 0.0
os.makedirs(out, exist_ok=True)


def inside(poly, x, y):
    c = False
    for (ax, ay), (bx, by) in zip(poly, poly[1:] + poly[:1]):
        if (ay > y) != (by > y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
            c = not c
    return c


gmsh.initialize()
gmsh.option.setNumber("General.Verbosity", 2)
gmsh.model.add("filter")
occ = gmsh.model.occ
box = occ.addBox(0, Y0, 0, L, YM - Y0, H + AIR)
sub = occ.addBox(0, Y0, 0, L, YM - Y0, H)
p = [occ.addPoint(x, y, H) for x, y in POLY]
lines = [occ.addLine(a, b) for a, b in zip(p, p[1:] + p[:1])]
sheet = occ.addPlaneSurface([occ.addCurveLoop(lines)])


def endRect(x):
    y0 = -F.W / 2 if full else 0.0
    q = [occ.addPoint(x, y0, 0), occ.addPoint(x, F.W / 2, 0), occ.addPoint(x, F.W / 2, H), occ.addPoint(x, y0, H)]
    return occ.addPlaneSurface([occ.addCurveLoop([occ.addLine(a, b) for a, b in zip(q, q[1:] + q[:1])])])


_, pieces = occ.fragment([(3, box)], [(3, sub), (2, sheet)] + ([(2, endRect(0.0)), (2, endRect(L))] if lumped else []))
occ.synchronize()
# The metal: what the sheet became (fragment()'s map), not the faces whose centre is within its outline - the rest of
# the alumina's top is a comb whose centre falls in a stub, and was taken for metal: the board a parallel plate.
sheet_faces = {t for d, t in pieces[2] if d == 2}

sub_vols, air_vols = [], []
for _, v in gmsh.model.getEntities(3):
    (sub_vols if occ.getCenterOfMass(3, v)[2] < H else air_vols).append(v)
groups = {"metal": [], "ground": [], "outer": [], "sym": [], "port1": [], "port2": []}
eps = 1e-6
for _, s in gmsh.model.getEntities(2):
    x, y, z = occ.getCenterOfMass(2, s)
    bb = gmsh.model.getBoundingBox(2, s)
    flat = lambda k, v: abs(bb[k] - v) < eps and abs(bb[k + 3] - v) < eps
    if not full and flat(1, 0.0):
        groups["sym"].append(s)
    elif (flat(0, 0.0) or flat(0, L)) and (not lumped or bb[1] > -F.W / 2 - eps and bb[4] < F.W / 2 + eps and bb[5] < H + eps):
        groups["port1" if flat(0, 0.0) else "port2"].append(s)
    elif flat(0, 0.0) or flat(0, L):
        groups["outer"].append(s)
    elif flat(2, 0.0):
        groups["ground"].append(s)
    elif flat(1, YM) or flat(1, Y0) or flat(2, H + AIR):
        groups["outer"].append(s)
    elif s in sheet_faces:
        groups["metal"].append(s)
gmsh.model.addPhysicalGroup(3, sub_vols, 1, "substrate")
gmsh.model.addPhysicalGroup(3, air_vols, 2, "air")
tags = {"metal": 1, "ground": 2, "outer": 3, "sym": 5, "port1": 6, "port2": 7}
for key, faces in groups.items():
    if not faces and not (full and key == "sym"):
        raise SystemExit("no faces for " + key)
    if not faces:
        continue
    gmsh.model.addPhysicalGroup(2, faces, tags[key], key)

edges = set()
for s in groups["metal"]:
    for _, c in gmsh.model.getBoundary([(2, s)], oriented=False):
        edges.add(abs(c))
gmsh.option.setNumber("Mesh.MeshSizeExtendFromBoundary", 0)
gmsh.option.setNumber("Mesh.MeshSizeFromPoints", 0)
gmsh.option.setNumber("Mesh.MeshSizeFromCurvature", 0)
gmsh.option.setNumber("Mesh.MeshSizeMax", far)
f = gmsh.model.mesh.field
f.add("Distance", 1)
f.setNumbers(1, "CurvesList", sorted(edges))
f.setNumber(1, "Sampling", 400)
f.add("Threshold", 2)
f.setNumber(2, "InField", 1)
f.setNumber(2, "SizeMin", hm)
f.setNumber(2, "SizeMax", far)
f.setNumber(2, "DistMin", hm)
f.setNumber(2, "DistMax", 2000)
f.setAsBackgroundMesh(2)
gmsh.option.setNumber("Mesh.Algorithm", 6)
gmsh.option.setNumber("Mesh.Algorithm3D", 10)
gmsh.option.setNumber("Mesh.MshFileVersion", 2.2)
gmsh.model.mesh.generate(3)
gmsh.write(os.path.join(out, name + ".msh"))
tets = sum(len(t) for t in gmsh.model.mesh.getElements(3)[1])
gmsh.finalize()

R = 50.0 if full else 100.0
config = {"Problem": {"Type": "Driven", "Verbose": 2, "Output": "postpro_" + name},
          "Model": {"Mesh": name + ".msh", "L0": 1.0e-6},
          "Domains": {"Materials": [{"Attributes": [1], "Permittivity": F.EPS, "Permeability": 1.0},
                                    {"Attributes": [2], "Permittivity": 1.0, "Permeability": 1.0}]},
          "Boundaries": {"PEC": {"Attributes": [1, 2]}, **({} if full else {"PMC": {"Attributes": [5]}}),
                         "Absorbing": {"Attributes": [3], "Order": 1},
                         **({"LumpedPort": [{"Index": 1, "R": R, "Excitation": True, "Elements": [{"Attributes": [6], "Direction": "+Z"}]},
                                            {"Index": 2, "R": R, "Elements": [{"Attributes": [7], "Direction": "+Z"}]}]} if lumped else
                            {"WavePort": [{"Index": 1, "Attributes": [6], "Mode": 1, "Target": 2.56, "Excitation": True},
                                          {"Index": 2, "Attributes": [7], "Mode": 1, "Target": 2.56}]})},
          "Solver": {"Order": 2, "Device": "CPU",
                     "Driven": {"Samples": [{"Type": "Linear", "MinFreq": 4.0, "MaxFreq": 16.0, "FreqStep": 0.02}],
                                "Save": [], "AdaptiveTol": 1.0e-4},
                     "Linear": {"Type": "AMS", "KSPType": "GMRES", "Tol": 1.0e-8, "MaxIts": 1000}}}
json.dump(config, open(os.path.join(out, name + ".json"), "w"), indent=1)
print(f"{name}: {tets} tetrahedra, faces " + ", ".join(f"{k} {len(v)}" for k, v in groups.items()))
