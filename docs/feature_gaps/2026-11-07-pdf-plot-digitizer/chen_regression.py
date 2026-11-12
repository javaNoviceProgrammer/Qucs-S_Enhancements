#!/usr/bin/env python3
"""B9 of the PDF viewer's open items of 9 November: the eight vector plots of
Chen et al. (1996), digitized through Claude's tools over stdio with no window,
checked against what they should give:

- each plot's curves, one series each, no more (a short piece no join could
  place is allowed, named "unassigned piece");
- every x axis linear, from 0, its labels at the frame's corners read;
- no arrow or ellipse among the series: they are under 'annotations';
- each curve named from the arrow pointing at it, as the benchmark's curves
  were named by hand (the table below, from those assignments): the dose,
  energy, tilt or pre-amorphization the paper writes at the arrow's tail.

    python3 chen_regression.py <qucs-s program> <Chen1996_In_Ge_implant.pdf>

The paper is not in the repository (it is not ours). Exits 1 on a failure.
"""
import json
import os
import subprocess
import sys
import tempfile

# Page, plot: the curves (measured + model), the words naming each pair, and
# how many annotations - arrows, ellipses - the page draws.
PLOTS = {
    (14, 2): (10, ["2x10^15", "5x10^14", "1x10^14", "2x10^13", "5x10^12"], (5, 0)),
    (15, 1): (4, ["0o /0o", "6o /30o"], (2, 0)),
    (16, 1): (8, ["10keV", "20keV", "40keV", "80keV"], (4, 0)),
    (17, 1): (10, ["2x10^15", "5x10^14", "1x10^14", "2x10^13", "5x10^12"], (5, 0)),
    (18, 1): (6, ["w/o preamorphization", "2x10^13", "5x10^14"], (3, 3)),
    (19, 1): (4, ["2x10^12", "2x10^14"], (2, 0)),
    (19, 2): (4, ["2x1012", "2x1014"], (2, 0)),
    (20, 1): (4, ["1x1013", "1x10^12"], (2, 0)),
}


def main():
    program, pdf = sys.argv[1], sys.argv[2]
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_SETTINGS_DIR=os.path.join(tmp, "settings"))
        p = subprocess.Popen([program, "--mcp-server", "--workspace", tmp], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, env=env, text=True)
        n = 0

        def send(method, params):
            nonlocal n
            n += 1
            p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": n, "method": method, "params": params}) + "\n")
            p.stdin.flush()
            while True:
                r = json.loads(p.stdout.readline())
                if r.get("id") == n:
                    return r

        send("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "chen", "version": "1"}})
        for (page, plot), (curves, words, (arrows, ellipses)) in PLOTS.items():
            r = send("tools/call", {"name": "digitize_plot", "arguments": {"path": pdf, "page": page, "plot": plot, "max_points": 0}})["result"]
            text = next(c["text"] for c in r["content"] if c["type"] == "text")
            where = f"page {page}, plot {plot}"
            if r.get("isError"):
                failures.append(f"{where}: refused: {text[:200]}")
                continue
            d = json.loads(text)
            x = d["calibration"]["x_axis"]
            if x.get("scale") != "linear" or abs(x.get("from", 1)) > 0.001:
                failures.append(f"{where}: x axis {x.get('scale')} from {x.get('from')}")
            series = [s for s in d["series"] if "unassigned piece" not in s["name"]]
            if len(series) != curves:
                failures.append(f"{where}: {len(series)} series for {curves} curves: {[s['name'] for s in series]}")
            if any(s["colour"] in ("#000000", "#010202") for s in d["series"]):
                failures.append(f"{where}: a black series (an arrow, an ellipse)")
            kinds = [a["kind"] for a in d.get("annotations", [])]
            if (kinds.count("arrow"), kinds.count("ellipse")) != (arrows, ellipses):
                failures.append(f"{where}: annotations {kinds}")
            for w in words:
                named = [s["name"] for s in series if w in s["name"]]
                if len(named) != 2:
                    failures.append(f"{where}: '{w}' names {named}, not a measured and a model curve")
            print(f"{where}: {len(series)} curves, {len(d['series']) - len(series)} unassigned, {kinds.count('arrow')} arrows, "
                  f"{kinds.count('ellipse')} ellipses, trust {d['trust']['level']}")
        p.stdin.close()
        p.wait(timeout=30)
    for f in failures:
        print("FAILED:", f)
    print("all as they should be" if not failures else f"{len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
