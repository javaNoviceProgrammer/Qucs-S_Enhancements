import sys, json; sys.path.insert(0, "."); from run import Server
folder, names = sys.argv[1], sys.argv[2].split(",")
s = Server(folder)
for name in names:
    bad, ans, text = s.call("rf25_solve", {"path": name + ".qrf25", "solver": "quasi_static", "timeout": 3600, "brief": True})
    print(name, "ERROR " + text[:300] if bad else json.dumps(ans.get("warnings", []))[:500])
s.close()
