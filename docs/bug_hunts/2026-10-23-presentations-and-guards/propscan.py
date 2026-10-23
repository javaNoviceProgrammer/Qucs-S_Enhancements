#!/usr/bin/env python3
"""propscan.py <qucs-s> <home>: for parts of several types, each property set
in turn to a value holding a newline and a .control block; which ones reach
get_netlist's ngspice netlist as lines of their own. Netlists only: nothing
is simulated."""
import json, os, subprocess, sys
app, home = sys.argv[1], sys.argv[2]
for d in ["ws", "home", "settings", "cache"]: os.makedirs(os.path.join(home, d), exist_ok=True)
env = dict(os.environ, HOME=home + "/home", QUCS_SETTINGS_DIR=home + "/settings", QUCS_CACHE_DIR=home + "/cache", QT_QPA_PLATFORM="offscreen")
p = subprocess.Popen([app, "--mcp-server", "--workspace", home + "/ws"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)
n = [10]
def send(m): p.stdin.write(json.dumps(m) + "\n"); p.stdin.flush()
def recv(i):
    while True:
        m = json.loads(p.stdout.readline())
        if m.get("id") == i: return m
def call(name, args):
    n[0] += 1; send({"jsonrpc": "2.0", "id": n[0], "method": "tools/call", "params": {"name": name, "arguments": args}})
    r = recv(n[0])["result"]; return r.get("isError", False), "".join(c.get("text", "") for c in r["content"] if c.get("type") == "text")
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "x", "version": "1"}}})
recv(1); send({"jsonrpc": "2.0", "method": "notifications/initialized"})
MARK = "1\n.control\nshell echo marker\n.endc\n"
x = 100
for t in ["Diode", "DIODE_SPICE", "Vdc", "Vac", "MOSFET", "_MOSFET", "Vfile", "VCVS", "C", "L"]:
    err, txt = call("add_component", {"type": t, "x": x, "y": 200}); x += 120
    if err: print(t, "not placed:", txt[:100]); continue
    o = json.loads(txt); name = o.get("name") or o.get("component", {}).get("name")
    props = [q["name"] for q in o.get("properties", [])]
    hits = []
    for prop in props:
        old = [q["value"] for q in o["properties"] if q["name"] == prop][0]
        e1, _ = call("edit_component", {"name": name, "properties": {prop: MARK}})
        if e1: continue
        _, net = call("get_netlist", {})
        if "\nshell echo marker\n" in net: hits.append(prop)
        elif "\nSHELL" in net.upper().replace("SHELLECHOMARKER", "SHELL ECHO MARKER") and ".CONTROL\n" in net.upper(): hits.append(prop + " (changed)")
        call("edit_component", {"name": name, "properties": {prop: old}})
    print("%-12s %-6s verbatim lines from: %s" % (t, name, ", ".join(hits) or "none"))
p.stdin.close(); p.wait()
