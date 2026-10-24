#!/usr/bin/env python3
"""breaks.py: each part of the solve's wiring - the button, the background
run, what is thrown away, what is said, and the tool - undone on purpose,
to see its test fail. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors alone; HUNT_RUNS=<dir> for the logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-solve-breaks")
os.makedirs(RUNS, exist_ok=True)

D, P, T = "rf25doc.cpp", "rf25panel.cpp", "qucscontrol_rf25.cpp"
DOC, CTL = "test_rf25_doc", "test_qucs_control"
SOLVED = "theModelIsSolvedAndSaysWhatItFound"
REFUSED = "aSolveWithNothingToSolveOnIsRefused"
TOOL = "anRf25ModelIsSolvedThroughItsTool"

BREAKS = [
 # ---- the solve itself
 ("the button does not solve",
  [(D, "        if (isSolving()) stopSolving();\n        else solve();", "        if (isSolving()) stopSolving();")],
  [(DOC, SOLVED)]),

 ("a solve with no mesh under it runs anyway",
  [(D, '    if (!a_mesh || a_mesh->cells.isEmpty())\n        return nothing(tr("There is no mesh to solve on: give the model a layout and a layer map first."));',
    "")],
  [(DOC, REFUSED)]),

 ("the length a line is taken over is the shorter side, not the longer",
  [(D, "    job->length = std::max(bounds.width(), bounds.height()) * 1e-6;",
    "    job->length = std::min(bounds.width(), bounds.height()) * 1e-6;")],
  [(DOC, SOLVED)]),

 ("the solve keeps the cells of the run before it",
  [(D, "    job->mesh = *a_mesh;\n    job->conductors = a_processed->conductors;", "    job->mesh = Mesh();\n    job->conductors = a_processed->conductors;")],
  [(DOC, SOLVED), (CTL, TOOL)]),

 # ---- what is thrown away when the model changes
 ("a capacitance of shapes that have gone is kept",
  [(D, "    stopSolving();\n    a_extraction.reset();\n    a_solvedLength = 0;\n    a_solveProblem.clear();", "")],
  [(DOC, SOLVED)]),

 ("the button says Stop for ever once a solve has run",
  [(D, "void RF25Doc::solveDone()\n{\n    const std::shared_ptr<SolveJob> job = a_solveJob;\n    a_solveJob.reset();\n    if (a_solveButton != nullptr) a_solveButton->setText(tr(\"Solve\"));",
    "void RF25Doc::solveDone()\n{\n    const std::shared_ptr<SolveJob> job = a_solveJob;\n    a_solveJob.reset();")],
  [(DOC, SOLVED)]),

 # ---- what it says
 ("the tab's notes say nothing of what it solved to",
  [(D, '        lines << tr("What it solves to:");', "        lines.clear();")],
  [(DOC, SOLVED)]),

 ("the status line says nothing of the line",
  [(D, '        tail += tr("; %1").arg(lineSummary().value(0, tr("solved")));', "        tail += QString();")],
  [(DOC, SOLVED)]),

 ("a line's impedance is left out of what it says",
  [(D, '        out << tr("%1: %2 pF/m, eps_eff %3, Z0 %4 Ohm over %5 µm")',
    '        out << tr("%1: %2 pF/m, eps_eff %3, %4 %5")')],
  [(DOC, SOLVED)]),

 ("the length a line was taken over is not the one the solve used",
  [(D, "double RF25Doc::solvedLength() const\n{\n    return a_solvedLength;\n}",
    "double RF25Doc::solvedLength() const\n{\n    return 0;\n}")],
  [(DOC, SOLVED)]),

 # ---- the panel's Results node
 ("the Results node says nothing of the solve",
  [(P, '    } else if (n->type == QLatin1String("rf_results")) {', "    } else if (false) {")],
  [(DOC, SOLVED), (DOC, REFUSED)]),

 ("a model with nothing solved says nothing rather than saying so",
  [(P, '            did << tr("Nothing is solved yet: the tab\'s Solve finds the capacitance of the conductors.");', "")],
  [(DOC, SOLVED)]),

 ("the panel is not told when a solve ends",
  [(P, "    a_connections << connect(doc, &RF25Doc::solvedChanged, this, [this] { scheduleSettings(); });", "")],
  [(DOC, SOLVED)]),

 # ---- the tool
 ("the tool does not wait for the shapes and the mesh",
  [(T, '    if (!model->waitForPrepared(timeout * 1000))\n        return errorResult(tr("Its shapes and its mesh are still being made after %1 s.").arg(timeout));',
    "")],
  [(CTL, TOOL)]),

 ("the tool answers a model that is none",
  [(T, "    if (model == nullptr)", "    if (false)")],
  [(CTL, TOOL)]),

 ("the tool ignores the length it was given",
  [(T, "        length = given.constant();", "")],
  [(CTL, TOOL)]),

 ("a length that is no length is taken as one",
  [(T, "        if (!given.isValid() || !given.isConstant() || !(given.constant() > 0))", "        if (false)")],
  [(CTL, TOOL)]),

 ("the tool answers the capacitance without the line it makes",
  [(T, "        const LineParameters lp = lineParameters(*e, i, length);", "        const LineParameters lp = lineParameters(*e, i, 0);")],
  [(CTL, TOOL)]),

 ("the tool answers nothing of what it did on the way",
  [(T, '    out.insert(QStringLiteral("what it did"), said);', "")],
  [(CTL, TOOL)]),
]

def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is not None and p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))

def main():
    only = os.environ.get("BRK_ONLY_CHECK")
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            if only:
                results.append((name, "anchors ok"))
                continue
            log = os.path.join(RUNS, "brk%02d.log" % i)
            suites = sorted({s for s, _ in tests})
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out,
                                  stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=900,
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen",
                                                    ASAN_OPTIONS="detect_leaks=0"))
                        said.append("%s %s" % (fn[:30], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:30])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-62s %s" % (name[:62], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
