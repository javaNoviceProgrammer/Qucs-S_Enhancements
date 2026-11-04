"""Solves the acceptance models through the app's --mcp-server (rf25_solve) and writes each one's Touchstone
(rf25_export): python3 -B -I run.py <app> <models folder> <root for the server's own folders> name [name...].
Each answer is kept as <name>.json beside the model."""
import json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server

app, models, root = sys.argv[1:4]
server = Server(app, root, models)
try:
    for name in sys.argv[4:]:
        began = time.time()
        bad, answer, _, text = server.call("rf25_solve", {"path": name + ".qrf25", "timeout": 3600})
        took = time.time() - began
        json.dump(answer if isinstance(answer, dict) else {"text": text}, open(os.path.join(models, name + ".json"), "w"), indent=1)
        if bad:
            print(f"{name}: REFUSED in {took:.0f} s: {text[:600]}", flush=True)
            continue
        checks = answer.get("checks", {})
        matrix = answer.get("matrix", {})
        print(f"{name}: solved in {took:.0f} s, {answer.get('unknowns')}, held {matrix.get('held')}"
              f" (iterations {matrix.get('iterations')}, residual {matrix.get('residual')}), passive {checks.get('passive')}"
              f" (most returned {checks.get('most returned')}), reciprocal {checks.get('reciprocal')}", flush=True)
        for port in answer.get("ports", []):
            shares = ", ".join(f"{r['name']} {100 * r['share']:.2f} % at {r['phase degrees']:.2f} deg" for r in port.get("returns", []))
            print(f"   {port['name']}: {port['drives']}" + (f" | shares: {shares}" if shares else ""), flush=True)
        for w in answer.get("warnings", []):
            print(f"   warning: {w}", flush=True)
        bad, said, _, text = server.call("rf25_export", {"path": name + ".qrf25", "file": name + ".s2p"})
        print(f"   {'NOT WRITTEN: ' + text[:300] if bad else 'written ' + name + '.s2p'}", flush=True)
finally:
    server.close()
