#!/usr/bin/env python3
"""The model's file written over while a run_script (which runs alone) is in
flight: the script's own calls must not see the new model, and the call
queued behind it must. alonereload.py <app> <home> <a.qfem> <b.qfem>"""
import json, os, select, shutil, subprocess, sys, time
app, home, a, b = sys.argv[1:5]
for d in ["ws", "home", "settings", "cache"]: os.makedirs(os.path.join(home, d), exist_ok=True)
shutil.copy(a, os.path.join(home, "ws", "m.qfem"))
env = dict(os.environ, HOME=home+"/home", QUCS_SETTINGS_DIR=home+"/settings", QUCS_CACHE_DIR=home+"/cache",
           QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0")
p = subprocess.Popen([app, "--mcp-server", "--workspace", home+"/ws"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=open(home+"/stderr.txt","w"), env=env)
buf=[b""]
def send(o): p.stdin.write((json.dumps(o)+"\n").encode()); p.stdin.flush()
def call(i, n, ar): send({"jsonrpc":"2.0","id":i,"method":"tools/call","params":{"name":n,"arguments":ar}})
def drain(t):
    end=time.time()+t; got=[]
    while time.time()<end:
        r,_,_=select.select([p.stdout],[],[],max(0,end-time.time()))
        if not r: break
        c=os.read(p.stdout.fileno(),1<<20)
        if not c: break
        buf[0]+=c
        while b"\n" in buf[0]:
            line,buf[0]=buf[0].split(b"\n",1)
            if line.strip(): got.append(line.decode(errors="replace"))
    return got
send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"a","version":"1"}}})
drain(30); send({"jsonrpc":"2.0","method":"notifications/initialized"})
call(10,"fem_model",{"action":"open","path":"m.qfem"}); drain(30)
# A script that reads the model, waits, reads it again - all inside one call.
script = ("var a = qucs.call('fem_describe', {what:'tree'}).geometry.objects.join(','); "
          "var t = Date.now(); while (Date.now() - t < 4000) {} "
          "var b = qucs.call('fem_describe', {what:'tree'}).geometry.objects.join(','); a + ' then ' + b")
call(20,"run_script",{"script":script,"timeout":30})
time.sleep(1.5)
shutil.copyfile(b, os.path.join(home,"ws","m.qfem")); print("(written over 1.5 s into the script)")
call(21,"fem_describe",{"what":"tree"})
for line in drain(60):
    m=json.loads(line)
    if m.get("id")==20:
        t=" ".join(c.get("text","") for c in m["result"]["content"] if c.get("type")=="text")
        print("  script  ->", t[:220])
    elif m.get("id")==21:
        t=" ".join(c.get("text","") for c in m["result"]["content"] if c.get("type")=="text")
        import re
        o=re.search(r'"objects":\[([^\]]*)\]', t)
        print("  queued call -> objects", o.group(1) if o else t[:120])
try: p.stdin.close(); p.wait(timeout=30)
except Exception: p.kill()
print("exit", p.returncode)
