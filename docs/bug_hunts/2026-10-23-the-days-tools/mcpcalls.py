#!/usr/bin/env python3
"""mcpcalls.py <app> <home> <calls.json> [deck...]: each call's answer (or a
timeout) printed with its time; the server's RSS after each."""
import json, os, select, shutil, subprocess, sys, time
app, home, calls = sys.argv[1], sys.argv[2], json.load(open(sys.argv[3]))
for d in ["ws/out", "home", "settings", "cache"]:
    os.makedirs(os.path.join(home, d), exist_ok=True)
for deck in sys.argv[4:]:
    shutil.copy(deck, os.path.join(home, "ws", os.path.basename(deck)))
env = dict(os.environ, HOME=home + "/home", QUCS_SETTINGS_DIR=home + "/settings", QUCS_CACHE_DIR=home + "/cache", QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0")
p = subprocess.Popen([app, "--mcp-server", "--workspace", home + "/ws"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(home + "/stderr.txt", "w"), env=env)
buf = [b""]
def send(m): p.stdin.write((json.dumps(m) + "\n").encode()); p.stdin.flush()
def recv(i, timeout):
    end = time.time() + timeout
    while True:
        while b"\n" in buf[0]:
            line, buf[0] = buf[0].split(b"\n", 1)
            try: m = json.loads(line)
            except ValueError: continue
            if m.get("id") == i: return m
        left = end - time.time()
        if left <= 0: return "timeout"
        r, _, _ = select.select([p.stdout], [], [], left)
        if r:
            c = os.read(p.stdout.fileno(), 1 << 20)
            if not c: return "dead"
            buf[0] += c
def rss():
    try: return int(subprocess.run(["ps", "-o", "rss=", "-p", str(p.pid)], capture_output=True, text=True).stdout.strip() or 0) // 1024
    except ValueError: return 0
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "probe", "version": "1"}}})
recv(1, 60); send({"jsonrpc": "2.0", "method": "notifications/initialized"})
n = 10
for c in calls:
    name, args = c[0], c[1]
    if name == "__run":
        cmd = args["cmd"].replace("WS", os.path.join(home, "ws"))
        subprocess.run(cmd, shell=True, check=False)
        time.sleep(args.get("wait", 0)); print("(ran %s)" % cmd[:120]); continue
    if name == "__copy":
        shutil.copyfile(args["from"], os.path.join(home, "ws", args["to"])); time.sleep(args.get("wait", 0)); print("(copied %s over %s)" % (os.path.basename(args["from"]), args["to"])); continue
    timeout = c[2] if len(c) > 2 else 60
    n += 1; t0 = time.time()
    send({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": name, "arguments": args}})
    r = recv(n, timeout)
    dt = time.time() - t0
    if isinstance(r, str):
        print("%-14s %s -> %s after %.1f s (rss %d MB)" % (name, json.dumps(args)[:200], r.upper(), dt, rss())); break
    res = r.get("result") or {}
    txt = " ".join(x.get("text", "") for x in res.get("content", []) if x.get("type") == "text").replace("\n", os.environ.get("NL", " "))
    imgs = sum(1 for x in res.get("content", []) if x.get("type") == "image")
    print("%-14s %s -> %s%s %.1f s rss %d MB: %s" % (name, json.dumps(args)[:200], "ERR " if res.get("isError") or "error" in r else "", ("[%d img] " % imgs) if imgs else "", dt, rss(), (txt or json.dumps(r.get("error")))[:int(os.environ.get("W", "400"))]))
try:
    p.stdin.close(); p.wait(timeout=30)
except Exception:
    p.kill()
print("exit", p.returncode)
