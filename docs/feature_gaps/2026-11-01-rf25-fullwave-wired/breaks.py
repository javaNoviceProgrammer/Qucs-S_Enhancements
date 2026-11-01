#!/usr/bin/env python3
"""breaks.py: each piece of the full-wave solve's wiring undone on purpose:
a port's facing and metal, an internal port's other metal, each port's own
reference, the frequencies side by side, stopped and counted, the current
density and the Touchstone, the document's study, results, export,
currents and ports, the MCP tools' solver and their quasi-static and
export paths, and the layout view's fill and legend. Sources are put back
whatever happens. BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs
those alone; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-fullwave-wired-breaks")
os.makedirs(RUNS, exist_ok=True)

F = "rf25/rf25_fullwave.cpp"
I = "rf25/rf25_inductance.cpp"
D = "rf25doc.cpp"
C = "qucscontrol_rf25.cpp"
L = "layoutdoc.cpp"
TF, TD, TC, TL = "test_rf25_fullwave", "test_rf25_doc", "test_qucs_control", "test_layout_doc"
PORTS = "itsPortsAreTheModelsOwn"
REF = "eachPortIsReferredToItsOwnImpedance"
SWEEP = "aSweepIsSolvedSideBySideAndCanBeStopped"
DRAW = "theCurrentDensityAndTheTouchstoneAreOfTheSolve"
WAVE = "aFullWaveStudySolvesThePortsAndDrawsTheCurrents"
MCP = "anRf25ModelIsSolvedFullWaveThroughItsTool"
OVER = "overlaysAreDrawnOverTheLayoutWithinTheirBudget"

BREAKS = [
 ("a face's inward normal taken for its outward one",
  [(I, "                if (c.outline.containsPoint(middle + normal * (1e-3 * len), Qt::OddEvenFill)) normal = -normal;",
    "                if (!c.outline.containsPoint(middle + normal * (1e-3 * len), Qt::OddEvenFill)) normal = -normal;")],
  [(TF, PORTS)]),

 ("an edge port's metal not kept to",
  [(I, "        if (!layer.isEmpty() && c.layer != layer) continue;\n        for (int i = 0, n = int(c.outline.size()); i < n; ++i) {",
    "        for (int i = 0, n = int(c.outline.size()); i < n; ++i) {")],
  [(TF, PORTS)]),

 ("an internal port between two metals driven against the ground",
  [(F, "                c.drive(nodeOf.value(from), p) -= 1;", "                (void)from;")],
  [(TF, PORTS)]),

 ("each port's own reference left out of the power waves",
  [(F, "        for (int j = 0; j < n; ++j) s(i, j) *= root(j) / root(i);\n", "")],
  [(TF, REF)]),

 ("(Z + R)'s inverse taken on the left, as though R were one number",
  [(F, "    MatrixC s = Eigen::PartialPivLU<MatrixC>(b.transpose()).solve(a.transpose()).transpose();",
    "    MatrixC s = Eigen::PartialPivLU<MatrixC>(b).solve(a);")],
  [(TF, REF)]),

 ("every thread writing the first frequency's slot",
  [(F, "            solved[std::size_t(k)] = solveAt(c, frequencies.at(k));", "            solved[0] = solveAt(c, frequencies.at(k));")],
  [(TF, SWEEP)]),

 ("the stop flag not heeded",
  [(F, "            if (failed || (control != nullptr && control->stop != nullptr && control->stop->load())) return;",
    "            if (failed) return;")],
  [(TF, SWEEP)]),

 ("the frequencies done not counted",
  [(F, "            if (control != nullptr && control->done != nullptr) ++*control->done;\n", "")],
  [(TF, SWEEP)]),

 ("a cell's current density the sum of its runs, not their mean",
  [(F, "        const Complex jx = it->nx > 0 ? it->x / it->nx : Complex(0, 0);", "        const Complex jx = it->x;")],
  [(TF, DRAW)]),

 ("a two-port written S11 S12 S21 S22",
  [(F, "            else line += pair(s[0][0]) + pair(s[1][0]) + pair(s[0][1]) + pair(s[1][1]);",
    "            else line += pair(s[0][0]) + pair(s[0][1]) + pair(s[1][0]) + pair(s[1][1]);")],
  [(TF, DRAW)]),

 ("a many-port's rows wrapped three to a line",
  [(F, "                if (j > 0 && j % 4 == 0) {", "                if (j > 0 && j % 3 == 0) {")],
  [(TF, DRAW)]),

 ("the document never taking the study for a full-wave one",
  [(D, "    return study != nullptr && textOf(*study, QStringLiteral(\"kind\")) == QLatin1String(\"full_wave\");", "    return false;")],
  [(TD, WAVE), (TC, MCP)]),

 ("a full-wave solve kept past the edit that made it stale",
  [(D, "    a_extraction.reset();\n    a_fullWave.reset();\n    a_waveNotes.clear();\n    a_inductance.reset();",
    "    a_extraction.reset();\n    a_waveNotes.clear();\n    a_inductance.reset();")],
  [(TD, WAVE)]),

 ("the export writing a full-wave solve's lines, which it has none of",
  [(D, "    if (a_fullWave && a_fullWave->ok()) {\n        if (!(what.impedance > 0))", "    if (false) {\n        if (!(what.impedance > 0))")],
  [(TD, WAVE), (TC, MCP)]),

 ("the currents not drawn",
  [(D, "    if (a_showCurrents && a_fullWave && a_fullWave->ok() && a_mesh) {", "    if (false) {")],
  [(TD, WAVE)]),

 ("a port's de-embedding passed over in silence",
  [(D, "            if (ok && deembed != 0)", "            if (false)")],
  [(TD, WAVE)]),

 ("an edge port's metal not read from the model",
  [(D, "            p.metal = textOf(port, QStringLiteral(\"conductor\")).trimmed();\n", "")],
  [(TD, WAVE)]),

 ("an edge port's facing not read from the model",
  [(D, "            const QString facing = textOf(port, QStringLiteral(\"facing\"));", "            const QString facing;")],
  [(TD, WAVE)]),

 ("rf25_solve not reading its 'solver'",
  [(C, "        request.solver = args.value(QLatin1String(\"solver\")).toString().trimmed();",
    "        request.solver = QStringLiteral(\"quasi_static\");")],
  [(TC, MCP)]),

 ("rf25_inductance solving by the study's solver",
  [(C, "    model->solveWith(quasiStatic);", "    model->solve();")],
  [(TC, MCP)]),

 ("rf25_export refusing a full-wave solve",
  [(C, "    if (!full && (!model->extraction() || !model->extraction()->ok()))", "    if (!model->extraction() || !model->extraction()->ok())")],
  [(TC, MCP)]),

 ("the matched drive's source and ends left out of its loop",
  [(F, "        for (int q = 0; q < np; ++q) a(q, q) += solved.references.at(q);\n", "")],
  [(TF, DRAW)]),

 ("the matched drive's source an EMF of a volt, not a wave of a watt",
  [(F, "        e(port) = 2 * std::sqrt(solved.references.at(port));", "        e(port) = 1.0;")],
  [(TF, DRAW)]),

 ("the tab drawing the currents of an ampere with the other ports open",
  [(D, "        const QHash<int, double> density = currentDensity(*a_fullWave, k, port, true);",
    "        const QHash<int, double> density = currentDensity(*a_fullWave, k, port, false);")],
  [(TD, WAVE)]),

 ("an overlay's fill not painted",
  [(L, "            p.setBrush(overlay.fill.isValid() ? QBrush(overlay.fill) : QBrush(Qt::NoBrush));", "            p.setBrush(Qt::NoBrush);")],
  [(TL, OVER)]),

 ("the legend not painted",
  [(L, "    if (!a_legend.isEmpty()) {", "    if (false) {")],
  [(TL, OVER)]),
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
        print("%-66s %s" % (name[:66], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
