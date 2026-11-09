"""RSS of a headless server before a solve, during it, and after rf25_stop."""
import sys, time, subprocess, json; sys.path.insert(0, "."); from run import Server
folder, name = sys.argv[1], sys.argv[2]
s = Server(folder)
def rss(): return int(subprocess.run(["ps", "-o", "rss=", "-p", str(s.p.pid)], capture_output=True, text=True).stdout.strip() or 0) / 1024
s.call("open_document", {"path": name + ".qrf25"})
print(f"before: {rss():.0f} MB", flush=True)
bad, ans, text = s.call("rf25_solve", {"path": name + ".qrf25", "solver": "full_wave", "timeout": 25})
print(f"during (after 25 s): {rss():.0f} MB", flush=True)
s.call("rf25_stop", {"path": name + ".qrf25", "timeout": 600})
for t in (2, 10):
    time.sleep(t)
    s.call("get_state", {})   # (a turn of the event loop)
    print(f"after stop +{t}s: {rss():.0f} MB", flush=True)
s.close()
