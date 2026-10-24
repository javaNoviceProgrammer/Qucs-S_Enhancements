#!/usr/bin/env python3
"""A batch nested in itself N deep sent as raw text, so the probe's own JSON
reader is not what gives way: nestprobe.py <app> <home> <depth>..."""
import json, os, select, subprocess, sys, time
app, home = sys.argv[1], sys.argv[2]
for d in ["ws", "home", "settings", "cache"]:
    os.makedirs(os.path.join(home, d), exist_ok=True)
env = dict(os.environ, HOME=home + "/home", QUCS_SETTINGS_DIR=home + "/settings", QUCS_CACHE_DIR=home + "/cache",
           QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0")
p = subprocess.Popen([app, "--mcp-server", "--workspace", home + "/ws"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=open(home + "/stderr.txt", "w"), env=env)
buf = [b""]
def send_raw(s): p.stdin.write(s.encode() + b"\n"); p.stdin.flush()
def recv(i, timeout):
    end = time.time() + timeout
    while True:
        while b"\n" in buf[0]:
            line, buf[0] = buf[0].split(b"\n", 1)
            if b'"id":%d' % i in line.replace(b" ", b"") or b'"id": %d' % i in line:
                return line[:400].decode(errors="replace")
        left = end - time.time()
        if left <= 0: return "TIMEOUT"
        r, _, _ = select.select([p.stdout], [], [], left)
        if r:
            c = os.read(p.stdout.fileno(), 1 << 20)
            if not c: return "DEAD"
            buf[0] += c
send_raw(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                     "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "n", "version": "1"}}}))
recv(1, 60)
send_raw(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}))
n = 10
for depth in [int(x) for x in sys.argv[3:]]:
    s = '{"tool":"get_state","arguments":{}}'
    for _ in range(depth):
        s = '{"tool":"batch","arguments":{"calls":[' + s + ']}}'
    n += 1
    t0 = time.time()
    send_raw('{"jsonrpc":"2.0","id":%d,"method":"tools/call","params":{"name":"batch","arguments":{"calls":[%s]}}}' % (n, s))
    r = recv(n, 60)
    print("depth %-7d %.1f s -> %s" % (depth, time.time() - t0, r[:180]))
    if r in ("TIMEOUT", "DEAD"): break
    n += 1
    send_raw('{"jsonrpc":"2.0","id":%d,"method":"tools/call","params":{"name":"get_state","arguments":{}}}' % n)
    print("   alive:", recv(n, 30)[:90])
try:
    p.stdin.close(); p.wait(timeout=30)
except Exception:
    p.kill()
print("exit", p.returncode)
