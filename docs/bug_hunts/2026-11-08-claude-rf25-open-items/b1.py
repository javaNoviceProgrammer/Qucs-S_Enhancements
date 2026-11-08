import sys, json; sys.path.insert(0, "."); from mk import *; from run import Server
out = sys.argv[1]
stack = [node("rf_ground", "gnd", "Ground", {"kind": "perfect"}),
         node("rf_substrate", "sub", "Board", {"thickness": "1600[um]", "epsr": "2.2", "tand": "0", "sigma": "0"}),
         node("rf_metal", "m1", "Cu", {"thickness": "0[um]", "sigma": "1e12", "model": "sheet"}),
         node("rf_cover", "cov", "Cover", {"kind": "open"})]
rects = [(0,0,3600,3900),(1600,-2100,4200,0),(-950,-1600,1550,0),(2150,-5200,2250,-2100)]
ports = [edge("p1", "Cu", -950, -800, "-x"), edge("p2", "Cu", 4200, -1050, "+x")]
model(f"{out}/split4.qrf25", [(1, r) for r in rects], stack, [(1, 0, "Cu")], ports, cells=3,
      study={"from": "1[GHz]", "to": "3[GHz]", "points": "3"})
# the same as one polygon drawn by hand (the union's outline)
poly = [(-950,-1600),(1550,-1600),(1550,0),(1600,0),(1600,-2100),(2150,-2100),(2150,-5200),(2250,-5200),(2250,-2100),(4200,-2100),(4200,0),(3600,0),(3600,3900),(0,3900),(0,0),(-950,0)]
model(f"{out}/one4.qrf25", [(1, poly)], stack, [(1, 0, "Cu")], ports, cells=3, study={"from": "1[GHz]", "to": "3[GHz]", "points": "3"})
if len(sys.argv) > 2:
    s = Server(out)
    for name in ("split4", "one4"):
        bad, ans, text = s.call("rf25_solve", {"path": name + ".qrf25", "solver": "full_wave", "frequency": "2[GHz]", "timeout": 600})
        print(name, "ERROR" if bad else "", text[:3000] if bad else "")
        if not bad:
            print(json.dumps({k: ans[k] for k in ans if k not in ("frequencies",)}, indent=0)[:2500])
    s.close()

# C chamfered at its far lower-left corner: a slanted side, so the mesh cannot cut it into rectangles.
Cch = [(-950,-1200),(-550,-1600),(1550,-1600),(1550,0),(-950,0)]
rects2 = [(0,0,3600,3900),(1600,-2100,4200,0),(2150,-5200,2250,-2100)]
model(f"{out}/split4ch.qrf25", [(1, r) for r in rects2] + [(1, Cch)], stack, [(1, 0, "Cu")], ports, cells=3,
      study={"from": "1[GHz]", "to": "3[GHz]", "points": "3"})
polych = [(-950,-1200),(-550,-1600),(1550,-1600),(1550,0),(1600,0),(1600,-2100),(2150,-2100),(2150,-5200),(2250,-5200),(2250,-2100),(4200,-2100),(4200,0),(3600,0),(3600,3900),(0,3900),(0,0),(-950,0)]
model(f"{out}/one4ch.qrf25", [(1, polych)], stack, [(1, 0, "Cu")], ports, cells=3, study={"from": "1[GHz]", "to": "3[GHz]", "points": "3"})

# The merge turned off: the chamfered split joined by the mesh alone; and a line cut along a slant, which it cannot join.
nomerge = {"merge": False}
model(f"{out}/nomerge_ch.qrf25", [(1, r) for r in rects2] + [(1, Cch)], stack, [(1, 0, "Cu")], ports, cells=3,
      study={"from": "1[GHz]", "to": "3[GHz]", "points": "3"}, processing=nomerge)
slantL = [(0, 0), (10000, 0), (12000, 4000), (0, 4000)]
slantR = [(10000, 0), (30000, 0), (30000, 4000), (12000, 4000)]
lp = [edge("p1", "Cu", 0, 2000, "-x"), edge("p2", "Cu", 30000, 2000, "+x")]
model(f"{out}/nomerge_slant.qrf25", [(1, slantL), (1, slantR)], stack, [(1, 0, "Cu")], lp, cells=3,
      study={"from": "1[GHz]", "to": "3[GHz]", "points": "3"}, processing=nomerge)
model(f"{out}/slant_merged.qrf25", [(1, slantL), (1, slantR)], stack, [(1, 0, "Cu")], lp, cells=3,
      study={"from": "1[GHz]", "to": "3[GHz]", "points": "3"})
