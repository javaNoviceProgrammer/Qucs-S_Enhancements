#!/usr/bin/env python3
"""A model's file written over while a solve of it runs, and a call sent at
once: does the tab reload the model out from under the job?
    racejob.py <app> <home> <model.qfem> <other.qfem> [hmax]
"""
import json, os, select, shutil, subprocess, sys, time
app, home, model, other = sys.argv[1:5]
hmax = sys.argv[5] if len(sys.argv) > 5 else None
for d in ["ws", "home", "settings", "cache"]: os.makedirs(os.path.join(home, d), exist_ok=True)
shutil.copy(model, os.path.join(home, "ws", "m.qfem"))
env = dict(os.environ, HOME=home+"/home", QUCS_SETTINGS_DIR=home+"/settings", QUCS_CACHE_DIR=home+"/cache",
           QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0")
p = subprocess.Popen([app, "--mcp-server", "--workspace", home+"/ws"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=open(home+"/stderr.txt","w"), env=env)
buf = [b""]
def send(o): p.stdin.write((json.dumps(o)+"\n").encode()); p.stdin.flush()
def call(i, name, args): send({"jsonrpc":"2.0","id":i,"method":"tools/call","params":{"name":name,"arguments":args}})
def drain(timeout):
    end = time.time()+timeout; got = []
    while time.time() < end:
        r,_,_ = select.select([p.stdout],[],[], max(0, end-time.time()))
        if not r: break
        c = os.read(p.stdout.fileno(), 1<<20)
        if not c: got.append("(stdout closed)"); break
        buf[0] += c
        while b"\n" in buf[0]:
            line, buf[0] = buf[0].split(b"\n",1)
            if line.strip(): got.append(line[:220].decode(errors="replace"))
    return got
send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"r","version":"1"}}})
drain(30); send({"jsonrpc":"2.0","method":"notifications/initialized"})
call(10, "fem_model", {"action":"open","path":"m.qfem"}); print("open:", drain(60)[:1])
if hmax:
    call(11, "fem_describe", {"what":"mesh"}); print("mesh:", [g[:200] for g in drain(30)][:1])
    call(12, "fem_edit", {"action":"set","tag":"mesh1","props":{"hmax":hmax}}); print("fine:", [g[:160] for g in drain(30)][:1])
# The solve goes out and is NOT waited for.
call(20, "fem_solve", {"timeout": 600})
time.sleep(float(os.environ.get("RACE_AFTER", "1.5")))
shutil.copyfile(other, os.path.join(home, "ws", "m.qfem"))
print("(the file written over while the solve runs)")
call(21, "fem_describe", {"what":"tree"})
for line in drain(180): print("  <-", line)
print("alive:", p.poll())
call(22, "get_state", {})
for line in drain(60): print("  <-", line[:160])
try:
    p.stdin.close(); p.wait(timeout=60)
except Exception: p.kill()
print("exit", p.returncode)
for line in open(os.path.join(home,"stderr.txt"), errors="replace"):
    if "Sanitizer" in line or "runtime error" in line or "crash" in line.lower(): print("  SAN", line.strip()[:200])
