#!/usr/bin/env python3
"""breaks.py: RF 2.5D's Optimization (12 November) undone on purpose, one
piece at a time - its kind, its plan, each candidate, how a stop reaches the
search, its result kept, its page, Apply Optimum - each built and its tests
run. Sources are put back whatever happens, and the suites built again after.
BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=3 runs that
alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the checkout's
build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-optimization-breaks")

O = "rf25/rf25_optimize.cpp"
M = "rf25/rf25_model.cpp"
T = "rf25doc.cpp"
PLAN = ("test_rf25_optimize", "anOptimizationsPlan")
STRIP = ("test_rf25_doc", "theShippedMicrostripReachesItsWidth")
STUB = ("test_rf25_doc", "theShippedStubFindsItsNotch")

BREAKS = [
 ("the kind's study kept",
  [(M, "                if (p.key == QLatin1String(\"study\")) continue;", "")], [PLAN]),
 ("an objective's Over read as its worst whatever it says",
  [(O, "        if (!overOf(cellText(r.at(4)), Reading::Over::Worst, &reading.over))", "        if (!overOf(QString(), Reading::Over::Worst, &reading.over))")], [PLAN]),
 ("an objective's window not read",
  [(O, "        if (!frequencyOf(cellText(r.at(5)), *ps, &reading.from, &bad) || !frequencyOf(cellText(r.at(6)), *ps, &reading.to, &bad)",
       "        if (!frequencyOf(QString(), *ps, &reading.from, &bad) || !frequencyOf(QString(), *ps, &reading.to, &bad)")], [PLAN]),
 ("a candidate solved with the model's own parameters",
  [(O, "        const Model candidate = withParameters(model, c.overrides);\n        SolveJob job;\n        control.begin(&job);\n        const ModelSolution solution",
       "        const Model candidate = model;\n        SolveJob job;\n        control.begin(&job);\n        const ModelSolution solution")], [STRIP]),
 ("a stop told to the solves alone, not to the search",
  [(O, "        if ((c.stopped && c.stopped()) || (c.progress && !c.progress(0.0, tr(\"solving\")))) {", "        if (c.stopped && c.stopped()) {"),
   (O, "        if (solution.stopped || control.stop.load() || (c.progress && !c.progress(1.0, tr(\"solved\")))) {", "        if (solution.stopped || control.stop.load()) {")],
  [STRIP]),
 ("Apply Optimum not writing the parameters' rows",
  [(O, "    parameters->props.insert(QStringLiteral(\"rows\"), out);", "")], [STRIP]),
 ("an optimization's result not kept in its node",
  [(T, "        node->props.insert(QStringLiteral(\"result\"), run->optimization ? run->found.toJson() : run->result.toJson());",
       "        node->props.insert(QStringLiteral(\"result\"), run->optimization ? QJsonObject() : run->result.toJson());")], [STRIP]),
 ("each solve not told to the page",
  [(T, "            if (page) page->addRow(row, columns);", "")], [STRIP]),
 ("an optimization's page not shown",
  [(T, "        a_optimizationPage->showResult(nameOf(*node), optimizationResult(tag), analysisOld(tag));\n        a_optimizationPage->show();",
       "        a_optimizationPage->showResult(nameOf(*node), optimizationResult(tag), analysisOld(tag));")], [STRIP]),
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
