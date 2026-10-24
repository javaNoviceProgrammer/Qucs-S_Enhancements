#!/usr/bin/env python3
"""The server's tool list, as JSON: list_tools.py <app> <home> > tools.json"""
import json, os, select, subprocess, sys, time
app, home = sys.argv[1], sys.argv[2]
for d in ["ws", "home", "settings", "cache"]:
    os.makedirs(os.path.join(home, d), exist_ok=True)
env = dict(os.environ, HOME=home + "/home", QUCS_SETTINGS_DIR=home + "/settings", QUCS_CACHE_DIR=home + "/cache",
           QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0")
p = subprocess.Popen([app, "--mcp-server", "--workspace", home + "/ws"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=open(home + "/stderr.txt", "w"), env=env)
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
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "l", "version": "1"}}})
recv(1, 60); send({"jsonrpc": "2.0", "method": "notifications/initialized"})
send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
print(json.dumps(recv(2, 60)["result"]["tools"]))
p.stdin.close(); p.wait(timeout=30)
