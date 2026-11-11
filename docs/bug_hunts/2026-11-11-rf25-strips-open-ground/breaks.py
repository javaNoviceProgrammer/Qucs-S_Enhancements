#!/usr/bin/env python3
"""breaks.py: the calibration strips of a coplanar feed with no ground plane
sized by its gaps (11 November) undone on purpose, one piece at a time -
the gaps' length, a ground plane telling the two apart, the gaps counted, and
what it says - each built and its test run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=3
runs that alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the
checkout's build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-strips-breaks")

E = "rf25/rf25_deembed.cpp"
GAPS = ("test_rf25_deembed", "aCoplanarFeedWithNoGroundPlaneIsCalibratedByItsGaps")

BREAKS = [
 ("the strips eight heights long whatever the ground",
  [(E, "    const double first = chosen > 0 ? chosen : byGaps ? 8 * span + 2 * width : byHeight;", "    const double first = chosen > 0 ? chosen : byHeight;")], [GAPS]),
 ("a ground plane not told from none",
  [(E, "    for (const Layer& l : stack.layers) plane = plane || (l.kind == Layer::Ground && l.variant != QLatin1String(\"open\"));\n", "")], [GAPS]),
 ("the gaps not counted, the signal alone",
  [(E, "    const double span = width + feed.gap[0] + feed.gap[1];", "    const double span = width;")], [GAPS]),
 ("a thin substrate's height bounding the strips",
  [(E, "    const bool byGaps = feed.coplanar && !plane && span > width;", "    const bool byGaps = feed.coplanar && !plane && span > width && 8 * span + 2 * width < byHeight;")], [GAPS]),
 ("why the strips are so long not said",
  [(E, "        if (byGaps && chosen <= 0)\n            out.lines", "        if (false)\n            out.lines")], [GAPS]),
]


def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is None:
            continue
        if p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))


def main():
    os.makedirs(os.path.join(RUNS, "cache"), exist_ok=True)
    if os.environ.get("BRK_ONLY_CHECK"):
        for name, edits, tests in BREAKS:
            apply(edits, None)
        print("%d breaks, every anchor once" % len(BREAKS))
        return 0
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        if which and i not in which:
            continue
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            log = os.path.join(RUNS, "brk%03d.log" % i)
            suites = sorted({t[0] for t in tests})
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    home = os.path.join(RUNS, "home")
                    os.makedirs(home, exist_ok=True)
                    env = dict(os.environ, HOME=home, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out, stderr=subprocess.STDOUT,
                                           stdin=subprocess.DEVNULL, timeout=900, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-62s %s" % (name[:62], said))
        if "BUILD FAILED" in said or "hung" in said or "caught)" not in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
