#!/usr/bin/env python3
"""Claude's digitizer tools over stdio, against the program itself, with no
window: each call's time as the client sees it, and what came back.

    python3 mcp_times.py <qucs-s program> <pdf> [<pdf> ...]

A temporary workspace and settings folder: nothing of the user's is touched.
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
import time


def main():
    program, pdfs = sys.argv[1], sys.argv[2:]
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_SETTINGS_DIR=os.path.join(tmp, "settings"))
        p = subprocess.Popen([program, "--mcp-server", "--workspace", tmp], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, env=env, text=True)
        number = 0

        def send(method, params=None):
            nonlocal number
            number += 1
            p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": number, "method": method, "params": params or {}}) + "\n")
            p.stdin.flush()
            while True:
                line = p.stdout.readline()
                if not line:
                    raise SystemExit("the server went away")
                reply = json.loads(line)
                if reply.get("id") == number:
                    return reply

        send("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "times", "version": "1"}})
        p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        p.stdin.flush()

        def tool(name, **args):
            t = time.perf_counter()
            r = send("tools/call", {"name": name, "arguments": args})
            ms = (time.perf_counter() - t) * 1000
            result = r.get("result", {})
            text = next((c["text"] for c in result.get("content", []) if c["type"] == "text"), "")
            images = [c for c in result.get("content", []) if c["type"] == "image"]
            return ms, text, images, result.get("isError", False)

        for pdf in pdfs:
            print("==", os.path.basename(pdf))
            ms, text, _, err = tool("find_plots", path=pdf)
            o = json.loads(text) if not err else {}
            print(f"find_plots       {ms:7.0f} ms  {len(o.get('plots', []))} plots" if not err else text)
            for plot in o.get("plots", [])[:2]:
                cold = True
                for _ in range(2):
                    ms, text, images, err = tool("digitize_plot", path=pdf, page=plot["page"], plot=plot["plot"], picture=True)
                    if err:
                        print(text)
                        break
                    d = json.loads(text)
                    size = len(base64.b64decode(images[0]["data"])) // 1024 if images else 0
                    print(f"digitize_plot    {ms:7.0f} ms  {'first' if cold else 'again'}  route {d.get('route')}, "
                          f"{len(d.get('series', []))} series, trust {d.get('trust', {}).get('level')}, "
                          f"picture {size} kB, answer {len(text) // 1024} kB, ms {d.get('ms')}")
                    cold = False
        p.stdin.close()
        p.terminate()


main()
