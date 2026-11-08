"""QFEM: does a mesh/size feature change the mesh? The benchmark's CPW cross-section with size features added."""
import sys, json, copy, os; sys.path.insert(0, "."); from run import Server
folder = sys.argv[1]
base = json.load(open(os.path.join(folder, "cpw_au_rlgc.qfem")))
variants = {
    "none": [],
    "dom_sig_0.2um": [{"type": "size", "label": "s", "level": "domain", "selection": {"only": ["sig"]}, "hmax": "0.2[um]"}],
    "dom_air_5um": [{"type": "size", "label": "s", "level": "domain", "selection": {"only": ["air"]}, "hmax": "5[um]"}],
    "dom_quartz_num_5um": [{"type": "size", "label": "s", "level": "domain", "selection": {"numbers": [2]}, "hmax": "5[um]"}],
    "bnd_num_0.2um": [{"type": "size", "label": "s", "level": "boundary", "selection": {"numbers": [12, 17]}, "hmax": "0.2[um]"}],
}
s = Server(folder)
for name, feats in variants.items():
    m = copy.deepcopy(base)
    m["components"][0]["mesh"]["features"] = base["components"][0]["mesh"]["features"] + feats
    p = os.path.join(folder, f"size_{name}.qfem"); json.dump(m, open(p, "w"), indent=1)
    bad, ans, text = s.call("fem_build", {"path": p, "what": "mesh", "timeout": 600})
    print(name, "ERROR " + text[:400] if bad else {k: ans.get(k) for k in ("triangles", "nodes", "smallest angle")}, flush=True)
# global size, for comparison
for hmax in ("20[um]", "5[um]"):
    m = copy.deepcopy(base); m["components"][0]["mesh"].update({"custom": True, "hmax": hmax})
    p = os.path.join(folder, f"global_{hmax[:-4]}.qfem"); json.dump(m, open(p, "w"), indent=1)
    bad, ans, text = s.call("fem_build", {"path": p, "what": "mesh", "timeout": 600})
    print("global hmax", hmax, "ERROR " + text[:400] if bad else {k: ans.get(k) for k in ("triangles", "nodes")}, flush=True)
# and through fem_edit on the model as opened
p = os.path.join(folder, "size_none.qfem")
bad, ans, text = s.call("fem_describe", {"path": p, "what": "mesh"})
print("describe mesh:", text[:800])
s.close()
