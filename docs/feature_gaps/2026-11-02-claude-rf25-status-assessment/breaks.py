#!/usr/bin/env python3
"""breaks.py: the RF 2.5D assessment's workflow items of 2 November undone
on purpose, one piece at a time - the solve's time left and its Stop beside
the bar, the solution kept beside the model (its stamps, what it is read
against, when it is written), the equivalent circuit (its fit, its forms,
the frequencies it leaves out, its SPICE and Qucs-S writing, the node, the
panel, the tool), the pen's corners and one-stroke arrows, and the solver
held to a published measurement - each built and its tests run. Sources are
put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-assessment-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

RD, KP, CI, MD, PN = "rf25doc.cpp", "rf25/rf25_kept.cpp", "rf25/rf25_circuit.cpp", "rf25/rf25_model.cpp", "rf25panel.cpp"
QR, MK, LD, FW, GR = "qucscontrol_rf25.cpp", "markup.cpp", "layoutdoc.cpp", "rf25/rf25_fullwave.cpp", "rf25/rf25_green.cpp"
TD, TS, TC, TP, TL, TM = "test_rf25_doc", "test_rf25_sparams", "test_qucs_control", "test_pptx_doc", "test_layout_doc", "test_rf25_measured"
KEPT, EC, LEFT, PROG = "theSolutionIsKeptBesideTheModel", "anEquivalentCircuitIsWrittenFromTheSolve", "theTimeLeftIsSaidRoundly", "aSolveShowsHowFarItHasGot"
PI, ONE, LONG, NONE, SUB = ("aPiOfKnownBranchesIsFoundAgain", "aOnePortIsOneBranch", "aLongLineIsSaidToBeNoLumpedCircuit",
                            "whatIsNoCircuitIsSaid", "theSpiceSubcircuitIsTheCircuit")
TOOL, STROKES, LAYOUT, LINE = ("anRf25ModelIsSolvedFullWaveThroughItsTool", "strokesAreReadAsDrawn", "theRawInputDrawsOnALayout",
                               "aCoplanarLineOnAluminaIsTheMeasuredOne")
QUASI, LOSSY = "anRf25ModelIsSolvedThroughItsTool", "aSlightlyLossyCapacitorIsACapacitor"

BREAKS = [
 # ---- the solve's time left, and its Stop beside the bar
 ("the time left not in the notes",
  [(RD, "\n                     + (left.isEmpty() ? QString() : tr(\" (%1)\").arg(left));", ";")],
  [(TD, PROG)]),
 ("the time left not in the status line",
  [(RD, "                + (left.isEmpty() ? QString() : tr(\", %1\").arg(left));", "                ;")],
  [(TD, PROG)]),
 ("the time left not in the bar's tool tip",
  [(RD, "a_progressBar->setToolTip(left.isEmpty() ? c.said : tr(\"%1 - %2\").arg(c.said, left));", "a_progressBar->setToolTip(c.said);")],
  [(TD, PROG)]),
 ("seconds said as they are, not to five",
  [(RD, "return tr(\"about %1 s left\").arg(5 * qRound(seconds / 5));", "return tr(\"about %1 s left\").arg(qRound(seconds));")],
  [(TD, LEFT)]),
 ("hours said as minutes",
  [(RD, "    if (seconds < 90 * 60) return tr(\"about %1 min left\")", "    if (true) return tr(\"about %1 min left\")")],
  [(TD, LEFT)]),
 ("the Stop beside the bar never shown",
  [(RD, "        a_progressBar->show();\n        a_progressStop->show();", "        a_progressBar->show();")],
  [(TD, PROG)]),
 ("the Stop beside the bar stopping nothing",
  [(RD, "        if (isSolving()) stopSolving();\n    });\n    statusRow->addWidget(a_progressStop);",
    "        if (false) stopSolving();\n    });\n    statusRow->addWidget(a_progressStop);")],
  [(TD, PROG)]),
 # ---- the solution kept beside the model
 ("a solve not kept beside the model",
  [(RD, "    if (!getDocChanged()) keepSolution();\n", "")],
  [(TD, KEPT), (TC, TOOL)]),
 ("a save not keeping the solve of the model saved",
  [(RD, "    // The solution shown, where it is of the model as saved, beside it.\n    keepSolution();\n", "")],
  [(TD, KEPT)]),
 ("what was kept not shown",
  [(RD, "            a_fullWave = kept.fullWave;\n", "")],
  [(TD, KEPT)]),
 ("a kept solution of another version of the model taken",
  [(RD, "    else if (kept->modelHash != solveStampOf(a_model))", "    else if (false)"),
   (RD, "        } else if (solveStampOf(a_model) != kept.modelHash || layoutStamp() != kept.layoutHash) {",
    "        } else if (layoutStamp() != kept.layoutHash) {")],
  [(TD, KEPT)]),
 ("a kept solution of another layout file taken",
  [(RD, "    else if (kept->layoutHash != layoutStamp())", "    else if (false)"),
   (RD, "        } else if (solveStampOf(a_model) != kept.modelHash || layoutStamp() != kept.layoutHash) {",
    "        } else if (solveStampOf(a_model) != kept.modelHash) {")],
  [(TD, KEPT)]),
 ("a kept solution of a mesh of other cells taken",
  [(RD, "        } else if (int(a_mesh->cells.size()) != kept.cells) {", "        } else if (false) {")],
  [(TD, KEPT)]),
 ("an export added clearing the solve",
  [(KP, "    if (Node* exports = copy.root.child(QStringLiteral(\"rf_exports\"))) exports->children.clear();\n", "")],
  [(TD, KEPT), (TD, EC)]),
 ("an edit changing nothing solved clearing the solve all the same",
  [(RD, "    const bool unchanged = (a_fullWave || a_extraction) && !a_solvedModel.isEmpty()",
    "    const bool unchanged = false && !a_solvedModel.isEmpty()")],
  [(TD, KEPT), (TD, EC)]),
 ("a file of another format read",
  [(KP, "    if (format != kFormat)\n", "    if (false)\n")],
  [(TD, KEPT)]),
 ("the stamp of the cells not kept",
  [(KP, "<< qint32(kept.cells) << kept.solvedBy", "<< qint32(0) << kept.solvedBy")],
  [(TD, KEPT)]),
 ("the full-wave answer not saying where it is kept",
  [(QR, "    if (!model.keptFile().isEmpty()) out.insert(QStringLiteral(\"kept beside the model\"), QFileInfo(model.keptFile()).fileName());", "")],
  [(TC, TOOL)]),
 ("the quasi-static answer not saying where it is kept",
  [(QR, "    if (!model->keptFile().isEmpty()) out.insert(QStringLiteral(\"kept beside the model\"), QFileInfo(model->keptFile()).fileName());", "")],
  [(TC, QUASI)]),
 # ---- the equivalent circuit
 ("the circuit's Y of the wrong sign",
  [(CI, "((one - m) * lu.inverse())", "((m - one) * lu.inverse())")],
  [(TS, PI), (TS, ONE)]),
 ("the least misfit taken whatever its values",
  [(CI, "        if (b.error <= std::max(1.25 * best, best + 2e-3)) return b;", "        if (b.error <= best) return b;")],
  [(TS, LOSSY)]),
 ("a frequency of no Y fitted all the same",
  [(CI, "        if (!lu.isInvertible()) {", "        if (false) {")],
  [(TS, LONG)]),
 ("two ports joined by a wire fitted",
  [(CI, "    if (between && least > 1e6) {", "    if (false) {")],
  [(TS, NONE)]),
 ("the SPICE subcircuit without its reference",
  [(CI, "QStringLiteral(\".subckt %1 %2 ref\")", "QStringLiteral(\".subckt %1 %2\")")],
  [(TS, SUB), (TD, EC)]),
 ("the part without its ground",
  [(CI, "    void ground(int x, int y) { components << QStringLiteral(\"  <GND * 1 %1 %2 0 0 0 0>\").arg(x).arg(y); }",
    "    void ground(int, int) {}")],
  [(TD, EC)]),
 ("a subcircuit's name as given, spaces and all",
  [(RD, "    sub.replace(QRegularExpression(QStringLiteral(\"[^A-Za-z0-9_]\")), QStringLiteral(\"_\"));\n", "")],
  [(TD, EC)]),
 ("a circuit node's file by default a SPICE one",
  [(RD, "QStringLiteral(\"%1_%2.sch\").arg(stem, tag)", "QStringLiteral(\"%1_%2.cir\").arg(stem, tag)")],
  [(TD, EC)]),
 ("no Equivalent circuit among the exports",
  [(MD, "{QStringLiteral(\"rf_touchstone\"), QStringLiteral(\"rf_circuit\")}", "{QStringLiteral(\"rf_touchstone\")}")],
  [(TD, EC)]),
 ("the panel's Write not for a circuit",
  [(PN, "if (n->type == QLatin1String(\"rf_touchstone\") || n->type == QLatin1String(\"rf_circuit\")) {",
    "if (n->type == QLatin1String(\"rf_touchstone\")) {")],
  [(TD, EC)]),
 ("the tool not writing the file asked",
  [(QR, "        file.write(text.toUtf8());\n        file.close();\n        out.insert(QStringLiteral(\"file\"), where);",
    "        file.close();\n        out.insert(QStringLiteral(\"file\"), where);")],
  [(TC, TOOL)]),
 ("the tool not saying how far its S is",
  [(QR, "    out.insert(QStringLiteral(\"S differs by at most\"), c->worst);\n", "")],
  [(TC, TOOL)]),
 ("rf25_circuit looking for a solve before the model's mesh is made",
  [(QR, "rf25_circuit fits a circuit to the full-wave solve of one.\")\n                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr(\"nothing is open\")));\n    // Its shapes and its mesh first: a solution kept beside the model is\n    // taken once they are made, a moment after the model is opened.\n    if (!model->waitForLayout(300000)) return errorResult(tr(\"Its layout is still being read after %1 s.\").arg(300));\n    if (!model->waitForPrepared(300000))\n        return errorResult(tr(\"Its shapes and its mesh are still being made after %1 s.\").arg(300));",
    "rf25_circuit fits a circuit to the full-wave solve of one.\")\n                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr(\"nothing is open\")));")],
  [(TC, TOOL)]),
 ("rf25_export looking for a solve before the model's mesh is made",
  [(QR, "rf25_export writes the S parameters of one.\")\n                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr(\"nothing is open\")));\n    // Its shapes and its mesh first: a solution kept beside the model is\n    // taken once they are made, a moment after the model is opened.\n    if (!model->waitForLayout(300000)) return errorResult(tr(\"Its layout is still being read after %1 s.\").arg(300));\n    if (!model->waitForPrepared(300000))\n        return errorResult(tr(\"Its shapes and its mesh are still being made after %1 s.\").arg(300));",
    "rf25_export writes the S parameters of one.\")\n                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr(\"nothing is open\")));")],
  [(TC, TOOL)]),
 ("rf25_export pointing a circuit node to rf25_circuit only once solved",
  [(QR, "!node.isEmpty() && n != nullptr && n->type == QLatin1String(\"rf_circuit\"))",
    "!node.isEmpty() && n != nullptr && n->type == QLatin1String(\"rf_circuit\") && model->fullWave())")],
  [(TC, TOOL)]),
 ("rf25_export taking an Equivalent circuit node for a Touchstone one",
  [(QR, "!node.isEmpty() && n != nullptr && n->type == QLatin1String(\"rf_circuit\"))\n        return errorResult(tr(\"%1 is an Equivalent",
    "false)\n        return errorResult(tr(\"%1 is an Equivalent")],
  [(TC, TOOL)]),
 # ---- the pen: corners and one-stroke arrows
 ("both sides of a V drawn at one go not an arrow",
  [(MK, "&& away <= 0.45 * reach && back <= 2 * reach) {", "&& away <= 0.45 * reach && back <= 0.6 * reach) {")],
  [(TP, STROKES)]),
 ("an arrow's head as far from the point as its shaft",
  [(MK, "&& away <= 0.45 * reach && back <= 2 * reach) {", "&& away <= 1.5 * reach && back <= 2 * reach) {")],
  [(TP, STROKES)]),
 ("no corners",
  [(MK, "    if (arms.size() == 3) {", "    if (false) {")],
  [(TP, STROKES), (TL, LAYOUT)]),
 ("a tick a corner",
  [(MK, "if (std::min(a.length(), b.length()) >= 0.4 * std::max(a.length(), b.length()) && angle >= 45 && angle <= 135) {",
    "if (angle >= 45 && angle <= 135) {")],
  [(TP, STROKES)]),
 ("a hairpin a corner",
  [(MK, "&& angle >= 45 && angle <= 135) {", "&& angle <= 135) {")],
  [(TP, STROKES)]),
 ("a bend of 150 degrees a corner",
  [(MK, "&& angle >= 45 && angle <= 135) {", "&& angle >= 45) {")],
  [(TP, STROKES)]),
 ("a corner not taken for an arrow's head",
  [(MK, " && out.at(j) != Gesture::Mark && out.at(j) != Gesture::Corner) continue;", " && out.at(j) != Gesture::Mark) continue;")],
  [(TP, STROKES)]),
 ("a corner's point not said on a page",
  [(MK, "            o.insert(QStringLiteral(\"point\"), QJsonArray{rounded(tips.at(i).x()), rounded(tips.at(i).y())});\n", "")],
  [(TP, STROKES)]),
 ("what a corner is at not in its sentence",
  [(MK, "{\"points at\", \"pointing at\"}, {\"at\", \"at\"}};", "{\"points at\", \"pointing at\"}};")],
  [(TP, STROKES)]),
 ("a corner's point not said on a layout",
  [(LD, "                o.insert(QStringLiteral(\"point\"), QJsonArray{std::round(tips.at(i).x() * 1000) / 1000, std::round(tips.at(i).y() * 1000) / 1000});\n", "")],
  [(TL, LAYOUT)]),
 ("what a corner on a layout is at not said",
  [(LD, "                    o.insert(QStringLiteral(\"at\"), at);\n                o.insert(QStringLiteral(\"point\")",
    "                    {}\n                o.insert(QStringLiteral(\"point\")")],
  [(TL, LAYOUT)]),
 # ---- the solver held to a measurement: the physics it checks
 ("the substrate's permittivity taken 6 % higher",
  [(GR, "                s.epsr = l.epsr > 0 ? l.epsr : 1;", "                s.epsr = l.epsr > 0 ? 1.06 * l.epsr : 1;")],
  [(TM, LINE)]),
 ("a two-faced metal's own impedance halved",
  [(FW, "        add(a, a, (own + edge) * squares);", "        add(a, a, (0.5 * own + edge) * squares);")],
  [(TM, LINE)]),
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
                                                    ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude",
                                                    QUCS_GH="/nonexistent/gh"))
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
