#!/usr/bin/env python3
"""The review of 7 November (qucs-reviews/PdfViewer/2026-11-07-claude-pdf-
digitizer-check.md), rerun: Fig. 4(a), 4(b) and 5 of the scanned paper it used,
through Claude's tools over stdio with no window, with the references it gave.

    python3 retest_scanned_paper.py <qucs-s program> <the paper's PDF> [<folder for the pictures>]

The paper is not in the repository (it is not ours); the pictures, if asked
for, go where they are told, not here.
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
import time

FIGURES = {
    "Fig. 5": [([354, 156.88], 0.6, 0), ([534.5, 156.88], 2, 0), ([354, 56.6], 0.6, 1)],
    "Fig. 4(a)": [([75.13, 132.21], 0.6, 0), ([173.06, 132.21], 2, 0), ([75.13, 56.75], 0.6, 0.4)],
    "Fig. 4(b)": [([188.76, 132.21], 0.6, 0), ([286.57, 132.21], 2, 0), ([188.76, 78.96], 0.6, 0.6)],
}


def main():
    program, pdf = sys.argv[1], sys.argv[2]
    pictures = sys.argv[3] if len(sys.argv) > 3 else None
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_SETTINGS_DIR=os.path.join(tmp, "settings"))
        p = subprocess.Popen([program, "--mcp-server", "--workspace", tmp], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, env=env, text=True)
        n = 0

        def send(method, params=None):
            nonlocal n
            n += 1
            p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": n, "method": method, "params": params or {}}) + "\n")
            p.stdin.flush()
            while True:
                reply = json.loads(p.stdout.readline())
                if reply.get("id") == n:
                    return reply

        send("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "retest", "version": "1"}})
        p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        p.stdin.flush()

        def tool(name, **args):
            t = time.perf_counter()
            result = send("tools/call", {"name": name, "arguments": args}).get("result", {})
            ms = (time.perf_counter() - t) * 1000
            text = next((c["text"] for c in result.get("content", []) if c["type"] == "text"), "")
            images = [c for c in result.get("content", []) if c["type"] == "image"]
            return ms, (json.loads(text) if not result.get("isError") else text), images

        ms, found, _ = tool("find_plots", path=pdf, pages=3)
        print(f"find_plots page 3: {ms:.0f} ms, {len(found['plots'])} plots")
        for plot in found["plots"]:
            print("   ", plot["plot"], plot["box"], plot["calibration"].get("x_axis", {}).get("title", ""), plot.get("notes", [])[:1])
        for name, refs in FIGURES.items():
            references = [{"at": at, "x": x, "y": y} for at, x, y in refs]
            box = next(pl for pl in found["plots"] if pl["box"][0] - 1 <= refs[0][0][0] <= pl["box"][2] + 1)
            ms, d, images = tool("digitize_plot", path=pdf, page=3, plot=box["plot"], references=references, picture=pictures is not None)
            if isinstance(d, str):
                print(name, "refused:", d)
                continue
            print(f"\n{name}: {ms:.0f} ms, trust {d['trust']['level']}, {len(d['series'])} series")
            for s in d["series"]:
                ys = [y for y in s.get("y", []) if y is not None]
                xs = [x for x in s.get("x", []) if x is not None]
                print(f"   {s['name']:<12} {s['shape']:<8} {s['colour']} {s['points_in_all']:>4} points, "
                      f"x {min(xs):.3f}..{max(xs):.3f}, y up to {max(ys):.3f}" if xs else f"   {s['name']}: no points")
            for note in d.get("notes", []):
                print("   note:", note)
            for reason in d["trust"].get("reasons", []):
                print("   trust:", reason)
            if pictures and images:
                with open(os.path.join(pictures, name.replace(" ", "_").replace("(", "").replace(")", "").replace(".", "") + ".png"), "wb") as f:
                    f.write(base64.b64decode(images[0]["data"]))
        p.stdin.close()
        p.terminate()


main()
