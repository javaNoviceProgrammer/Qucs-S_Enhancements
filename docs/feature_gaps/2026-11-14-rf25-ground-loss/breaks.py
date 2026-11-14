#!/usr/bin/env python3
"""breaks.py: a metal or impedance ground's loss in the full-wave solve (14 November) undone on purpose, one piece
at a time - each built and its tests run. Sources are put back whatever happens, and the suites built again after.
BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=2 runs that alone; HUNT_RUNS=<dir> for logs;
BRK_BUILD=<dir> the build (the checkout's build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-ground-loss-breaks")
GUILESS = {"test_rf25_accuracy", "test_rf25_edge", "test_rf25_returns"}   # (no -platform for these)

FW = "rf25/rf25_fullwave.cpp"
MO = "rf25/rf25_model.cpp"
LOSS = ("test_rf25_fullwave", "aMetalGroundsLossIsCountedWhereAsked")
READ = ("test_rf25_model", "aGroundsLossInTheFullWaveIsReadWhereItCanLose")

BREAKS = [
 ("the ground's loss not added in the dense solve",
  [(FW, "    groundImpedances(c, f, [&m](int a, int b, Complex v) { m(a, b) += v; });\n", "")], [LOSS]),
 ("the ground's loss not in the compressed solve's operator",
  [(FW, "            withGround.emplace_back(a, b, v);\n", "")], [LOSS]),
 ("the kernel's H one run's height, not the two's",
  [(FW, "const double across = m.x() * ri.v.x() + m.y() * ri.v.y(), h = ri.z + rj.z;",
        "const double across = m.x() * ri.v.x() + m.y() * ri.v.y(), h = ri.z;")], [LOSS]),
 ("runs that do not run side by side sharing too, each its whole length",
  [(FW, "                    if (!(side > 0)) continue;\n", ""),
   (FW, "                    const double k = along * side * over / (ri.w * rj.w);",
        "                    const double k = along * std::max(ri.l, rj.l) * over / (ri.w * rj.w);")], [LOSS]),
 ("a lossy ground not counted said nowhere",
  [(FW, "        else if (lossy)\n            out.lines << tr(\"The ground plane is %1, and its loss is not counted here",
        "        else if ((false))\n            out.lines << tr(\"The ground plane is %1, and its loss is not counted here")], [LOSS]),
 ("an impedance ground's option not read",
  [(MO, "                                   && (layer.variant == QLatin1String(\"metal\") || layer.variant == QLatin1String(\"impedance\"));",
        "                                   && layer.variant == QLatin1String(\"metal\");")], [READ]),
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
                if subprocess.run(["ninja", "-j12", "-C", B] + suites, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    home = os.path.join(RUNS, "home")
                    os.makedirs(home, exist_ok=True)
                    env = dict(os.environ, HOME=home, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
                    try:
                        args = [os.path.join(B, "qucs/tests", suite), fn] + ([] if suite in GUILESS else ["-platform", "offscreen"])
                        r = subprocess.run(args, stdout=out,
                                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=1200, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    # The suites built again from the sources as they are: a break's build
    # left behind fails a later run for nothing.
    subprocess.run(["ninja", "-j12", "-C", B] + sorted({t[0] for _, _, tests in BREAKS for t in tests}),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
