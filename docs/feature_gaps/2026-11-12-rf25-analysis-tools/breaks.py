#!/usr/bin/env python3
"""breaks.py: Claude's tools and the panel for RF 2.5D's analyses (12
November) undone on purpose, one piece at a time - rf25_edit, rf25_sweep,
rf25_optimize, rf25_stop, the format's text, the panel's Run, Stop and Apply -
each built and its tests run. Sources are put back whatever happens, and the
suites built again after. BRK_ONLY_CHECK=1 checks the anchors and writes
nothing; BRK_ONLY=3 runs that alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir>
the build (the checkout's build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-analysis-tools-breaks")

C = "qucscontrol_rf25.cpp"
F = "qucscontrol_fem.cpp"
P = "rf25panel.cpp"
TOOLS = ("test_qucs_control", "anRf25ModelIsOptimizedThroughItsTools")
PANEL = ("test_rf25_doc", "thePanelRunsAndAppliesAnAnalysis")

BREAKS = [
 ("rf25_edit taking a property its kind has not",
  [(C, "            if (kind == nullptr || kind->property(it.key()) == nullptr) {", "            if (false) {")], [TOOLS]),
 ("a node not named by its kind",
  [(C, "    return byKind.size() == 1 ? byKind.first() : nullptr;", "    return nullptr;")], [TOOLS]),
 ("rf25_sweep's rows not cut to 'rows'",
  [(C, "    for (int i = 0; i < r.points.size() && i < rows; ++i) {", "    for (int i = 0; i < r.points.size(); ++i) {")], [TOOLS]),
 ("rf25_sweep not waiting for the sweep it starts",
  [(C, "        const int timeout = waitsFor(QStringLiteral(\"rf25_sweep\"), args);\n        const bool ended = doc->waitForAnalysis(timeout * 1000);",
       "        const int timeout = waitsFor(QStringLiteral(\"rf25_sweep\"), args);\n        const bool ended = doc->waitForAnalysis(0);")], [TOOLS]),
 ("rf25_optimize's apply not applying",
  [(C, "        if (!doc->applyOptimum(tag, args.value(QLatin1String(\"point\")).toInt(0), &written, &why)) return errorResult(why);",
       "        written << QStringLiteral(\"nothing\");")], [TOOLS]),
 ("rf25_stop not stopping an analysis",
  [(C, "    if (model->isAnalysing()) {\n        QString said;", "    if (false) {\n        QString said;")], [TOOLS]),
 ("the format's text without the analyses",
  [(F, "        \"rf_analyses holds rf_sweep", "        \"rf_analyses keeps rf_sweep")], [TOOLS]),
 ("the panel's Run not offered",
  [(P, "            run->setObjectName(QStringLiteral(\"rfRunAnalysisAction\"));", "")], [PANEL]),
 ("the panel's Apply Optimum always offered",
  [(P, "            apply->setEnabled(kept && !running);", "            apply->setEnabled(true);")], [PANEL]),
 ("the panel's Stop not offered while it runs",
  [(P, "            stop->setObjectName(QStringLiteral(\"rfStopAnalysisAction\"));", "")], [PANEL]),
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
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn, "-platform", "offscreen"], stdout=out,
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
    subprocess.run(["ninja", "-C", B] + sorted({t[0] for _, _, tests in BREAKS for t in tests}),
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
