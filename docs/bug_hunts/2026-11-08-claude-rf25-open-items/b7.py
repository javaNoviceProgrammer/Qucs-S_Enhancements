import sys, json; sys.path.insert(0, "."); from mk import *
out = sys.argv[1]
stack = [node("rf_ground", "gnd", "Ground", {"kind": "open"}),
         node("rf_substrate", "sub", "Quartz", {"thickness": "500[um]", "epsr": "3.8", "tand": "0", "sigma": "0"}),
         node("rf_metal", "m1", "Au", {"thickness": "0[um]", "sigma": "1e12", "model": "sheet"}),
         node("rf_cover", "cov", "Cover", {"kind": "open"})]
L = 1000
shapes = [(1, (0, -25, L, 25)), (1, (0, 45, L, 205)), (1, (0, -205, L, -45)), (1, (L, -205, L + 20, 205))]
st = {"from": "10[GHz]", "to": "30[GHz]", "points": "3"}
model(f"{out}/short_beside.qrf25", shapes, stack, [(1, 0, "Au")], [edge("p1", "Au", 0, 0, "-x", ret="beside")], cells=3, study=st)
sq = lambda x, y: {"x": f"{x}[um]", "y": f"{y}[um]", "width": "20[um]", "height": "20[um]"}
ports = [node("rf_internal_port", "p1", "P1", {"to": "Au", "return": "terminals", "returns": ["T1", "T2"], **sq(20, 0)}),
         node("rf_terminal", "t1", "T1", {"metal": "Au", **sq(20, 125)}),
         node("rf_terminal", "t2", "T2", {"metal": "Au", **sq(20, -125)})]
model(f"{out}/short_tips.qrf25", shapes, stack, [(1, 0, "Au")], ports, cells=3, study=st)
# The open line, for comparison: the same, no bar.
model(f"{out}/open_tips.qrf25", shapes[:3], stack, [(1, 0, "Au")], ports, cells=3, study=st)
model(f"{out}/open_beside.qrf25", shapes[:3], stack, [(1, 0, "Au")], [edge("p1", "Au", 0, 0, "-x", ret="beside")], cells=3, study=st)
