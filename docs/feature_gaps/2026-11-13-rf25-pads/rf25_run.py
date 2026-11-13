"""Solves RF 2.5D models through a headless `qucs-s --mcp-server` (not the open window, whose background
rf25_solve crashed on 7 Nov) and exports each full-wave solve as Touchstone.
usage: python3 -I rf25_run.py <models folder> full|qs name [name...]
  full: rf25_solve full wave, answer kept as <name>.json, S written to <name>.s2p
  qs:   quasi-static at 5..50 GHz, the line values written to <name>.qs.json
The server's settings, workspace and HOME are in <project>/.rf25server, so nothing goes to the home folder."""
import json, os, subprocess, sys, time

APP = os.environ.get("QUCS_APP", "/Applications/qucs-s.app/Contents/MacOS/qucs-s")
models = os.path.abspath(sys.argv[1]); mode = sys.argv[2]; names = sys.argv[3:]
base = os.path.join(os.path.dirname(models), ".rf25server")
for d in ("settings", "home"): os.makedirs(os.path.join(base, d), exist_ok=True)
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_SETTINGS_DIR=os.path.join(base, "settings"),
           QUCS_NO_SHELL_ENV="1", HOME=os.path.join(base, "home"))
p = subprocess.Popen([APP, "--mcp-server", "--workspace", models], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=open(os.path.join(base, "server.log"), "a"), env=env, text=True)
def send(o): p.stdin.write(json.dumps(o) + "\n"); p.stdin.flush()
def recv(i):
    while True:
        line = p.stdout.readline()
        if not line: raise SystemExit("server ended")
        m = json.loads(line)
        if m.get("id") == i: return m
n = 0
def call(tool, args):
    global n; n += 1
    send({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = recv(n).get("result", {})
    text = "\n".join(c.get("text", "") for c in r.get("content", []))
    try: ans = json.loads(text)
    except Exception: ans = {"text": text}
    return bool(r.get("isError")), ans, text
send({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}}}); recv(0)
send({"jsonrpc": "2.0", "method": "notifications/initialized"})
bad, st, _ = call("get_state", {})
if os.path.realpath(st.get("workspace", "")) != os.path.realpath(models):
    p.kill(); raise SystemExit(f"server workspace {st.get('workspace')} is not {models}")
try:
    for name in names:
        t0 = time.time()
        if mode == "full":
            if os.path.exists(os.path.join(models, name + ".s2p")): continue   # solved before
            bad, ans, text = call("rf25_solve", {"path": name + ".qrf25", "solver": "full_wave", "timeout": 3600, "brief": True})
            json.dump(ans, open(os.path.join(models, name + ".json"), "w"), indent=1)
            if bad: print(f"{name}: REFUSED {text[:400]}", flush=True); continue
            ch = ans.get("checks", {}); mx = ans.get("matrix", {})
            print(f"{name}: {time.time() - t0:.0f} s, unknowns {ans.get('unknowns')}, held {mx.get('held') if isinstance(mx, dict) else mx},"
                  f" passive {ch.get('passive')}, reciprocal {ch.get('reciprocal')}", flush=True)
            bad, _, text = call("rf25_export", {"path": name + ".qrf25", "file": name + ".s2p", "version": "2.0"})
            if bad: print(f"   NOT WRITTEN {text[:300]}", flush=True)
        else:
            out = {}
            for f in (5, 10, 15, 20, 25, 30, 35, 40, 45, 50):
                bad, ans, text = call("rf25_solve", {"path": name + ".qrf25", "solver": "quasi_static", "frequency": f"{f}[GHz]", "brief": True})
                if bad: print(f"{name} {f} GHz: REFUSED {text[:300]}", flush=True); break
                out[f] = ans
            json.dump(out, open(os.path.join(models, name + ".qs.json"), "w"), indent=1)
            print(f"{name}: quasi-static {time.time() - t0:.0f} s", flush=True)
finally:
    p.stdin.close()
    try: p.wait(timeout=60)
    except Exception: p.kill()
