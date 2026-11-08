import sys, json; sys.path.insert(0, "."); from run import Server
folder, names, f = sys.argv[1], sys.argv[2].split(","), sys.argv[3]
extra = json.loads(sys.argv[4]) if len(sys.argv) > 4 else {}
s = Server(folder)
for name in names:
    a = {"path": name + ".qrf25", "solver": "full_wave", "frequency": f, "timeout": 3600}; a.update(extra)
    bad, ans, text = s.call("rf25_solve", a)
    if bad: print(name, "ERROR", text[:1500]); continue
    sm = ans.get("s matrix", {})
    print(name, "S dB", [[round(v, 4) for v in r] for r in sm.get("dB", [])], "deg", [[round(v, 2) for v in r] for r in sm.get("degrees", [])])
    print("  unknowns", ans.get("unknowns"), "matrix", ans.get("matrix"))
    for l in ans.get("what it did", []): print("  -", l[:400])
    for k in ("mesh", "warnings", "warning", "notes"):
        if k in ans: print("  ", k, json.dumps(ans[k])[:1500])
s.close()
