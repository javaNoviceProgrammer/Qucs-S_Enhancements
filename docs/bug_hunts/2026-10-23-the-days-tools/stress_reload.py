#!/usr/bin/env python3
"""Calls pipelined (sent without waiting) while the open document's file is
written over again and again: the per-call reload of 2c0de7e under load.
    stress_reload.py <app> <home> <a.qfem> <b.qfem> <rounds>
"""
import json, os, select, shutil, subprocess, sys, threading, time
app, home, a, b, rounds = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
for d in ["ws", "home", "settings", "cache"]: os.makedirs(os.path.join(home, d), exist_ok=True)
shutil.copy(a, os.path.join(home, "ws", "m.qfem"))
env = dict(os.environ, HOME=home+"/home", QUCS_SETTINGS_DIR=home+"/settings", QUCS_CACHE_DIR=home+"/cache",
           QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1")
p = subprocess.Popen([app, "--mcp-server", "--workspace", home+"/ws"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=open(home+"/stderr.txt","w"), env=env)
def send(o): p.stdin.write((json.dumps(o)+"\n").encode()); p.stdin.flush()
def call(i, name, args): send({"jsonrpc":"2.0","id":i,"method":"tools/call","params":{"name":name,"arguments":args}})
buf=[b""]; seen={}
def reader():
    while True:
        try: c = os.read(p.stdout.fileno(), 1<<20)
        except Exception: return
        if not c: return
        buf[0]+=c
        while b"\n" in buf[0]:
            line, buf[0] = buf[0].split(b"\n",1)
            if not line.strip(): continue
            try: m=json.loads(line)
            except ValueError: continue
            if m.get("id") is not None: seen[m["id"]] = ("error" in m) or (m.get("result",{}).get("isError") is True)
threading.Thread(target=reader, daemon=True).start()
send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"s","version":"1"}}})
time.sleep(2)
send({"jsonrpc":"2.0","method":"notifications/initialized"})
call(9, "fem_model", {"action":"open","path":"m.qfem"}); time.sleep(2)
stop = [False]
def writer():
    k = 0
    while not stop[0]:
        shutil.copyfile(b if k % 2 else a, os.path.join(home, "ws", "m.qfem"))
        k += 1
        time.sleep(0.03)
threading.Thread(target=writer, daemon=True).start()
n = 100
work = [("fem_describe", {"what":"tree"}), ("get_state", {}), ("list_documents", {}),
        ("fem_describe", {"what":"geometry"}), ("fem_evaluate", {"expression":"1[m]"}),
        ("fem_edit", {"action":"set","tag":"r1","props":{"label":"x"}}), ("get_schematic", {})]
for r in range(rounds):
    for name, args in work:
        n += 1; call(n, name, args)
    time.sleep(0.05)
sent = n - 100
deadline = time.time() + 120
while len(seen) < sent + 1 and time.time() < deadline and p.poll() is None:
    time.sleep(0.5)
stop[0] = True
print("sent %d, answered %d, errors %d, alive %s" % (sent, len(seen)-1, sum(1 for k,v in seen.items() if v and k>100), p.poll()))
time.sleep(1)
try:
    p.stdin.close(); p.wait(timeout=60)
except Exception: p.kill()
print("exit", p.returncode)
for line in open(os.path.join(home,"stderr.txt"), errors="replace"):
    if "Sanitizer" in line or "runtime error" in line or "ASSERT" in line: print("  !!", line.strip()[:200])
