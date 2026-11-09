#!/usr/bin/env python3
"""The review of 7 November (qucs-reviews/PdfViewer/2026-11-07-claude-pdf-
digitizer-check.md) and its recheck of 8 November (2026-11-08-claude-pdf-
digitizer-recheck.md), rerun: Fig. 4(a), 4(b) and 5 of the scanned paper they
used, through Claude's tools over stdio with no window - with the references
the review gave, then the recheck's runs: colours and names, one colour with
x_range and y_range, two seeds on one curve. Each line is checked to come back
in order of x, and (the second recheck, 2026-11-08-claude-pdf-digitizer-
recheck2.md) for turning back in y between points a sliver apart: a steep side
shuffled.

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

BLUE4, BLACK = "#3550a4", "#000000"
# The recheck's runs: (what, figure, arguments besides the plot; references
# given unless the run has x_range).
RUNS = [
    ("Fig. 5, three colours", "Fig. 5", dict(colours=["#e11e25", "#425396", BLACK])),
    ("Fig. 4(a), names by colour", "Fig. 4(a)", dict(colours=[BLUE4, BLACK], names=["Model", "COMSOL"])),
    ("Fig. 4(b), names by colour", "Fig. 4(b)", dict(colours=[BLUE4, BLACK], names=["Model", "COMSOL"])),
    ("Fig. 5, blue alone, ranges", "Fig. 5", dict(colours=["#425396"], x_range=[0.6, 2], y_range=[0, 1])),
    ("Fig. 4(a), two seeds", "Fig. 4(a)", dict(colours=[BLUE4], seeds=[[140.9, 113.3], [88.42, 94.5]])),
]

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
        runs = [(name, name, {}) for name in FIGURES] + RUNS
        for title, name, extra in runs:
            refs = FIGURES[name]
            box = next(pl for pl in found["plots"] if pl["box"][0] - 1 <= refs[0][0][0] <= pl["box"][2] + 1)
            args = dict(path=pdf, page=3, plot=box["plot"], picture=pictures is not None, max_points=100000)
            if "x_range" not in extra:
                args["references"] = [{"at": at, "x": x, "y": y} for at, x, y in refs]
            args.update(extra)
            ms, d, images = tool("digitize_plot", **args)
            name = title
            if isinstance(d, str):
                print(name, "refused:", d)
                continue
            print(f"\n{name}: {ms:.0f} ms, trust {d['trust']['level']}, {len(d['series'])} series")
            # (Points of the page a unit of each axis is, from the references.)
            (a0, x0, y0), (a1, x1, _), (a2, _, y2) = refs
            sx, sy = abs((a1[0] - a0[0]) / (x1 - x0)), abs((a2[1] - a0[1]) / (y2 - y0))
            for s in d["series"]:
                ys = [y for y in s.get("y", []) if y is not None]
                xs = [x for x in s.get("x", []) if x is not None]
                line = s["shape"] == "line" and len(xs) == len(ys)
                back = sum(1 for a, b in zip(xs, xs[1:]) if b < a) if line else 0
                # y turning back by more than 1.5 pt each way, within a piece,
                # between points a sliver (under 0.5 pt) apart in x: a steep
                # side shuffled, not a peak's top, which moves on in x.
                starts = set(s.get("pieces_start_at", []))
                turns = sum(1 for i in range(1, len(ys) - 1) if i not in starts and i + 1 not in starts
                            and (ys[i] - ys[i - 1]) * (ys[i + 1] - ys[i]) < 0
                            and abs(ys[i] - ys[i - 1]) * sy > 1.5 and abs(ys[i + 1] - ys[i]) * sy > 1.5
                            and abs(xs[i] - xs[i - 1]) * sx < 0.5 and abs(xs[i + 1] - xs[i]) * sx < 0.5) if line else 0
                print(f"   {s['name']:<12} {s['shape']:<8} {s['colour']} {s['points_in_all']:>4} points, "
                      f"x {min(xs):.3f}..{max(xs):.3f}, y up to {max(ys):.3f}" + (f", {len(starts)} gaps" if starts else "")
                      + (f", NOT IN ORDER OF X ({back} steps back)" if back else "") + (f", TURNS BACK {turns} times" if turns else "")
                      if xs else f"   {s['name']}: no points")
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
