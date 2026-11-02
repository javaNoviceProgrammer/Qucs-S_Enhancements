#!/usr/bin/env python3
"""breaks.py: the RF 2.5D tab's solve bar undone on purpose, one piece at a
time - when it is shown and hidden, what it counts in each phase of a solve,
its place and size beside the status line, a solve started again under it,
and the two things the solver tells it (FullWaveControl's begins and a
test's allowance) - each built and its test run. Sources are put back
whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
BRK_SRC=<qucs dir> and BRK_BUILD=<build dir> for another checkout (the
default is the one this file is in, and its build/); HUNT_RUNS=<dir> for
logs."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.environ.get("BRK_SRC", os.path.join(ROOT, "qucs-s-26.1.1", "qucs")) + "/"
B = os.environ.get("BRK_BUILD", os.path.join(ROOT, "build"))
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-progress-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

D = "rf25doc.cpp"
FW = "rf25/rf25_fullwave.cpp"
T = "test_rf25_doc"
BAR = "aSolveShowsHowFarItHasGot"

BREAKS = [
 # ---- shown and hidden
 ("never shown",
  [(D, "    if (a_progressBar->isHidden() && a_solvingSince.elapsed() >= 250) a_progressBar->show();\n", "")],
  [(T, BAR)]),

 ("shown at once, flashing up for a solve of milliseconds",
  [(D, "a_solvingSince.elapsed() >= 250) a_progressBar->show();", "a_solvingSince.elapsed() >= 0) a_progressBar->show();")],
  [(T, BAR)]),

 ("left shown when the solve ends",
  [(D, "        a_progress->stop();\n        a_progressBar->hide();\n", "        a_progress->stop();\n")],
  [(T, BAR)]),

 ("left shown by a Stop, till a tick of its timer",
  [(D, "void RF25Doc::stopSolving()\n{\n    letSolveGo();\n    showProgress();\n}",
    "void RF25Doc::stopSolving()\n{\n    letSolveGo();\n}")],
  [(T, BAR)]),

 ("its timer not started, as it was not for the quasi-static solve",
  [(D, "    if (!a_progress->isActive()) a_progress->start();\n", "")],
  [(T, BAR)]),

 ("its timer's tick leaving it as it was",
  [(D, "    connect(a_progress, &QTimer::timeout, this, [this] {\n        showProgress();\n",
    "    connect(a_progress, &QTimer::timeout, this, [this] {\n")],
  [(T, BAR)]),

 # ---- what it counts
 ("the quasi-static extraction counted out of one, not busy",
  [(D, "    a_progressBar->setRange(0, of);", "    a_progressBar->setRange(0, std::max(1, of));")],
  [(T, BAR)]),

 ("the full-wave sweep busy, not counted",
  [(D, "        of = frequenciesToSolve();\n        done = std::min(frequenciesSolved(), of);",
    "        done = std::min(frequenciesSolved(), of);")],
  [(T, BAR)]),

 ("the sweep counted out of one frequency more than it has",
  [(D, "        of = frequenciesToSolve();\n", "        of = frequenciesToSolve() + 1;\n")],
  [(T, BAR)]),

 ("the sweep's count standing still",
  [(D, "        done = std::min(frequenciesSolved(), of);", "        done = 0;")],
  [(T, BAR)]),

 ("its tool tip not saying how far",
  [(D, "    a_progressBar->setToolTip(said);\n", "")],
  [(T, BAR)]),

 ("the calibration busy throughout, its strips not counted",
  [(D, "        if (begun > 0) of = planned", "        if (false) of = planned")],
  [(T, BAR)]),

 ("the calibration counted out of the strips begun alone",
  [(D, "        if (begun > 0) of = planned + std::max(0, 2 * job->feedsToCalibrate - begun) * planned / begun;",
    "        if (begun > 0) of = planned;")],
  [(T, BAR)]),

 ("the feeds to calibrate not counted",
  [(D, "                job->feedsToCalibrate = int(keys.size());\n", "")],
  [(T, BAR)]),

 ("the strips' frequencies solved not counted",
  [(D, "            quiet.done = &job->calibrated;\n", "")],
  [(T, BAR)]),

 # ---- what the solver tells it
 ("the solver telling no one how many frequencies it begins",
  [(FW, "    if (control != nullptr && control->begins) control->begins(nf);\n", "")],
  [(T, BAR)]),

 ("the solver ignoring a test's allowance",
  [(FW, "            if (!allowedOne(control)) return;\n", "")],
  [(T, BAR)]),

 # ---- a solve started again under it
 ("an edit's mesh starting the solve again hides it a moment",
  [(D, "    const bool wanted = a_solveJob != nullptr || a_solveWhenPrepared;\n    letSolveGo();",
    "    const bool wanted = a_solveJob != nullptr || a_solveWhenPrepared;\n    stopSolving();")],
  [(T, BAR)]),

 ("a solve asked for again lets the one before go with its bar",
  [(D, "void RF25Doc::solveWith(const SolveRequest& request)\n{\n    letSolveGo();",
    "void RF25Doc::solveWith(const SolveRequest& request)\n{\n    stopSolving();")],
  [(T, BAR)]),

 # ---- its place and size
 ("as tall as it would be, the status line taller and the splitter shorter",
  [(D, "    a_progressBar->setMaximumHeight(a_status->fontMetrics().height() + 4);\n", "")],
  [(T, BAR)]),

 ("as wide as it likes, asking the window for its width",
  [(D, "    a_progressBar->setFixedWidth(160);", "    a_progressBar->setMinimumWidth(1600);")],
  [(T, BAR)]),

 ("before the status line, not at its end",
  [(D, "    statusRow->addWidget(a_status, 1);\n    a_progressBar = new QProgressBar(this);",
    "    a_progressBar = new QProgressBar(this);"),
   (D, "    a_progressBar->hide();\n    statusRow->addWidget(a_progressBar);\n",
    "    a_progressBar->hide();\n    statusRow->addWidget(a_progressBar);\n    statusRow->addWidget(a_status, 1);\n")],
  [(T, BAR)]),

 # ---- the status line, which the tools read
 ("the status line's count said otherwise",
  [(D, "        tail += tr(\"; solving %1 of %2 frequencies\")", "        tail += tr(\"; solving %1/%2 frequencies\")")],
  [(T, BAR)]),
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
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        if which and i not in which:
            continue
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
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=CACHE,
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
        print("%-66s %s" % (name[:66], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
