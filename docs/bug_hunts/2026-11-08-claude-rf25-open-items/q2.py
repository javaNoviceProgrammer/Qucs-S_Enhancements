import sys, json; sys.path.insert(0, "."); from run import Server
folder, names, f = sys.argv[1], sys.argv[2].split(","), sys.argv[3]
s = Server(folder)
for name in names:
    bad, ans, text = s.call("rf25_solve", {"path": name + ".qrf25", "solver": "full_wave", "frequency": f, "timeout": 3600})
    if bad: print(name, "ERROR", text[:800]); continue
    print(name, "S dB", [[round(v, 4) for v in r] for r in ans["s matrix"]["dB"]], "warnings:", json.dumps(ans.get("warnings", []))[:600])
s.close()
