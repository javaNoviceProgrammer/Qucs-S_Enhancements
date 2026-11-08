import sys, json, copy, os; sys.path.insert(0, "."); from run import Server
folder = sys.argv[1]
base = json.load(open(os.path.join(folder, "cpw_au_rlgc.qfem")))
lay, dist = base["components"][0]["mesh"]["features"]
sz = lambda lvl, sel, h: {"type": "size", "label": "s", "level": lvl, "selection": sel, "hmax": h}
variants = {
    "nolayer": [dist],
    "nolayer_sig0.2": [dist, sz("domain", {"only": ["sig"]}, "0.2[um]")],
    "nodist": [lay],
    "nodist_bnd0.2": [lay, sz("boundary", {"numbers": [12, 17]}, "0.2[um]")],
    "bare": [],
    "bare_bnd0.2": [sz("boundary", {"numbers": [12, 17]}, "0.2[um]")],
    "bare_sig0.2": [sz("domain", {"only": ["sig"]}, "0.2[um]")],
}
s = Server(folder)
for name, feats in variants.items():
    m = copy.deepcopy(base); m["components"][0]["mesh"]["features"] = feats
    p = os.path.join(folder, f"v_{name}.qfem"); json.dump(m, open(p, "w"), indent=1)
    bad, ans, text = s.call("fem_build", {"path": p, "what": "mesh", "timeout": 600})
    print(name, "ERROR " + text[:300] if bad else {k: ans.get(k) for k in ("triangles", "nodes")}, flush=True)
s.close()
