#!/usr/bin/env python3
"""breaks.py: the 2D optimizer's driver opened to another kind of model (12
November) undone on purpose, one piece at a time - the candidate's solve as a
callback, what it says and stops by, the notes only the solver knows, the plan
read by the shared pieces, Apply Optimum into parameter rows, parameters
evaluated from rows alone - each built and its tests run. Sources are put back
whatever happens. BRK_ONLY_CHECK=1 checks the anchors and writes nothing;
BRK_ONLY=3 runs that alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build
(the checkout's build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/optimizer-driver-breaks")

O = "multiphysics/fem_optimize.cpp"
M = "multiphysics/fem_model.cpp"
MINE = ("test_fem_optimize", "anotherModelsPlanRunsByItsOwnSolve")
HELD = ("test_fem_optimize", "heldParametersAreWrittenIn")
MOVES = ("test_fem_optimize", "aValueThatMovesWithAVariedParameter")
STOP = ("test_fem_optimize", "aStopKeepsTheBest")
EXAMPLE = ("test_fem_optimize", "theShippedExample")

BREAKS = [
 ("what a candidate's solve says not told",
  [(O, "    if (!m.warnings.isEmpty()) {\n        std::lock_guard<std::mutex> lock(a_mutex);", "    if (false) {\n        std::lock_guard<std::mutex> lock(a_mutex);")],
  [MINE]),
 ("the solver's last notes left out",
  [(O, "    if (a_lastNotes)\n        for (const QString& n : a_lastNotes())", "    if (false)\n        for (const QString& n : a_lastNotes())")],
  [MINE, HELD]),
 ("a candidate never told the run was asked to stop",
  [(O, "    candidate.stopped = [this] { return a_stop.load(); };", "    candidate.stopped = [] { return false; };")],
  [MINE]),
 ("a 2D candidate's stop between its studies not looked at",
  [(O, "        if (asked.stopped && asked.stopped()) {\n            m.stopped = true;\n            return m;\n        }\n        StudyRun run",
       "        if (false) {\n            m.stopped = true;\n            return m;\n        }\n        StudyRun run")],
  [STOP]),
 ("the parameters held at a start of their own not written in",
  [(O, "        if (plan != nullptr)\n            for (const QJsonValue& row", "        if (false)\n            for (const QJsonValue& row")],
  [HELD]),
 ("a value that moves with a varied parameter not looked for",
  [(O, "const std::shared_ptr<ParameterScope> moved = evaluate({{v.name, valueText(other, plan.dims[j])}});",
       "const std::shared_ptr<ParameterScope> moved = nullptr;")],
  [MINE, MOVES]),
 ("an objective's row of no expression refused, not skipped",
  [(O, "    if (o.expression.isEmpty()) {\n        why.clear();\n        return false;\n    }",
       "    if (o.expression.isEmpty()) {\n        why = QStringLiteral(\"no expression\");\n        return false;\n    }")],
  [MINE]),
 ("a parameter's unit not kept when its value is written in",
  [(O, "    for (const Model::Parameter& p : rows) {\n        if (p.name.trimmed() != name) continue;\n        const QRegularExpressionMatch m = withUnit.match(p.expression);",
       "    for (const Model::Parameter& p : QList<Model::Parameter>()) {\n        if (p.name.trimmed() != name) continue;\n        const QRegularExpressionMatch m = withUnit.match(p.expression);")],
  [MINE, EXAMPLE]),
 ("parameters from rows alone evaluated with their overrides ignored",
  [(M, "        const QString text = overrides.contains(name) ? overrides.value(name) : p.expression;\n        const Expression e = Expression::compile(text, ps->scope);",
       "        const QString text = p.expression;\n        const Expression e = Expression::compile(text, ps->scope);")],
  [MINE, EXAMPLE]),
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
