"""The request's second test: the GSG landing's L-2L eps_eff and Z0 (the mean of the two pad-box orders) against the
edge port's, both solved by this build; and the reviewer's own numbers of 4 November beside them.
python3 -B -I compare.py <models folder> <a> <b> [ref_a ref_b]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l2l_extract import l2l
folder = sys.argv[1]
os.chdir(folder)
a, b = sys.argv[2], sys.argv[3]
ra, rb = (sys.argv[4], sys.argv[5]) if len(sys.argv) > 5 else ("edge_L500", "edge_L1000")
tips = l2l(a + ".s2p", b + ".s2p", 0.5e-3, 1.0e-3)
edge = l2l(ra + ".s2p", rb + ".s2p", 0.5e-3, 1.0e-3)
reviewer = {10: (2.451, 77.6), 30: (2.429, 77.6), 60: (2.409, 78.7)}
print(f"{'GHz':>5} | {ra[:-5] + ' eps':>10} {'Z0':>7} | {a[:-5] + ' eps':>10} {'Z0':>7} | {'d eps %':>8} {'d Z0 %':>7} | reviewer's edge port")
for t, e in zip(tips, edge):
    f = round(t["f"])
    zt = (t["Z1"].real + t["Z2"].real) / 2
    ze = (e["Z1"].real + e["Z2"].real) / 2
    mark = f"{reviewer[f][0]:.3f} {reviewer[f][1]:.1f}" if f in reviewer else ""
    if f in (2, 10, 20, 30, 40, 50, 60) or mark:
        print(f"{f:5d} | {e['eeff']:10.4f} {ze:7.2f} | {t['eeff']:10.4f} {zt:7.2f} | {100 * (t['eeff'] / e['eeff'] - 1):+8.3f} {100 * (zt / ze - 1):+7.3f} | {mark}")
worst_e = max(abs(t["eeff"] / e["eeff"] - 1) for t, e in zip(tips, edge))
worst_z = max(abs((t["Z1"].real + t["Z2"].real) / (e["Z1"].real + e["Z2"].real) - 1) for t, e in zip(tips, edge))
print(f"over the sweep: eps_eff within {100 * worst_e:.3f} %, Z0 within {100 * worst_z:.3f} %")
