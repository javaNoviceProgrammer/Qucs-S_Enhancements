import sys, time, subprocess; sys.path.insert(0, "."); from run import Server
folder, name = sys.argv[1], sys.argv[2]
s = Server(folder)
s.call("open_document", {"path": name + ".qrf25"})
s.call("rf25_solve", {"path": name + ".qrf25", "solver": "full_wave", "timeout": 25})
s.call("rf25_stop", {"path": name + ".qrf25", "timeout": 600})
time.sleep(5); s.call("get_state", {})
out = subprocess.run(["heap", "-s", str(s.p.pid)], capture_output=True, text=True).stdout
for l in out.splitlines():
    if "All zones" in l or "Physical footprint" in l or "zone" in l.lower() and "bytes" in l: print(l[:160])
v = subprocess.run(["vmmap", "--summary", str(s.p.pid)], capture_output=True, text=True).stdout
for l in v.splitlines():
    if "Physical footprint" in l or "MALLOC" in l[:30] or "TOTAL" in l[:10] or "DefaultMallocZone" in l: print(l[:170])
s.close()
