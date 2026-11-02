#!/usr/bin/env python3
"""breaks.py: the GSSG coupled-modes round's fixes undone on purpose, one at a
time - the return conductors (marked, joined to the ground by a via, a
coplanar line's outermost two in either direction), the signals' lines
against them (infinity floating, Wheeler's R and its floor, a return no
line), the per-metre direction of a short line, the full wave's coplanar
port past a signal, with no ground plane, its differential pairs and their
mixed-mode S, and the tab's and the tools' parts - each built and its tests
run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/person/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/person/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-gssg-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

ST = "rf25/rf25_static.cpp"
FW = "rf25/rf25_fullwave.cpp"
MO = "rf25/rf25_model.cpp"
DOC = "rf25doc.cpp"
QC = "qucscontrol_rf25.cpp"
TS, TF, TD, TC = "test_rf25_static", "test_rf25_fullwave", "test_rf25_doc", "test_qucs_control"
GSSG_S = "aGssgsSignalsAreCoupledLinesAgainstItsGrounds"
CANNOT_S = "whatItCannotSolveIsRefused"
GSSG_F = "aGssgReturnsThroughItsGroundsWithNoGroundPlane"
PAIRS = "aDifferentialPairsModesAreItsMixedModeS"
CANNOT_F = "whatItCannotSolveIsSaid"
DRAWN = "aCoplanarPortsGroundsAreDrawnAsItsReturns"
PADS = "whatIsNoLineHasNoSParameters"
VIA_D = "aViaToTheGroundIsSolvedFullWaveAndDrawn"
TOOLS = "aGssgIsSolvedThroughTheToolsAsItsModes"

BREAKS = [
 # ---- the return conductors (G1)
 ("a Return Conductor's net not taken as a return",
  [(ST, "        if (!out.at(found)) marked << nets.at(found).name;\n        out[found] = true;",
    "        if (!out.at(found)) marked << nets.at(found).name;")],
  [(TS, GSSG_S), (TD, DRAWN)]),

 ("what a via joins to the ground no return",
  [(ST, "                    out[i] = true;\n                    joined << nets.at(i).name;", "                    joined << nets.at(i).name;")],
  [(TS, GSSG_S), (TS, CANNOT_S), (TD, VIA_D)]),

 ("a coplanar line's outermost two looked for along its longer side alone",
  [(ST, "        for (const bool alongX : {true, false}) {", "        for (const bool alongX : {all.width() >= all.height()}) {")],
  [(TS, GSSG_S), (TF, GSSG_F)]),

 ("the lines of every net, the returns' too",
  [(ST, "    out.nets = extraction.signalNets();\n    if (out.nets.isEmpty()) {",
    "    for (int i = 0; i < n; ++i) out.nets << i;\n    if (out.nets.isEmpty()) {")],
  [(TS, GSSG_S), (TC, TOOLS)]),

 # ---- the signals' lines against them (G1-G3, G6)
 ("infinity held at nought where it floats",
  [(ST, "    if (extraction.hasReturns() && !extraction.grounded) {\n        const Eigen::VectorXd one",
    "    if (false) {\n        const Eigen::VectorXd one")],
  [(TS, GSSG_S)]),

 ("Wheeler's rule wanting a ground plane",
  [(ST, "    if (uniform && (out.grounded || out.hasReturns())) {", "    if (uniform && out.grounded) {")],
  [(TS, GSSG_S)]),

 ("the returns' own resistance out of a signal's at direct current",
  [(ST, "pm.dcResistance.at(i) + pm.dcReturn + g[i][i]);", "pm.dcResistance.at(i) + 0 * pm.dcReturn + g[i][i]);")],
  [(TS, GSSG_S)]),

 ("a return given a line of its own",
  [(ST, "        if (extraction.returns.value(index, false)) {\n            out.problems << tr(\"%1 is a return conductor",
    "        if (false) {\n            out.problems << tr(\"%1 is a return conductor")],
  [(TS, GSSG_S)]),

 # ---- the per-metre direction of a short line
 ("the per-metre line along its longer side alone",
  [(ST, "    if (spansX != spansY && spansX != alongX) {", "    if (false) {")],
  [(TS, GSSG_S), (TC, TOOLS)]),

 ("two pads far apart a line",
  [(ST, "        if (widest <= (spansX ? all.width() : all.height())) alongX = spansX;",
    "        alongX = spansX || widest < 0;")],
  [(TD, PADS)]),

 # ---- the full wave's coplanar port past a signal (G5)
 ("a coplanar port's returns the nearest conductors, whatever they are",
  [(FW, "        const Beside beside = besideFaces(mesh, cellOf, face, on, along, &netOf, &isReturn);",
    "        const Beside beside = besideFaces(mesh, cellOf, face, on, along, &netOf, nullptr);")],
  [(TF, GSSG_F), (TF, PAIRS), (TC, TOOLS)]),

 ("a signal passed over not said of the feed",
  [(FW, "        if (!passed.isEmpty())\n            feed.unlike = tr(", "        if (false)\n            feed.unlike = tr(")],
  [(TF, GSSG_F)]),

 ("a port returning through what another drives, solved",
  [(FW, "                if (q != p && net >= 0 && ownNet.at(q) == net) {", "                if (false) {")],
  [(TF, PAIRS)]),

 # ---- with no ground plane (G4)
 ("every port refused with no ground plane, as before",
  [(FW, "            if (port.internal ? fromGround : !port.coplanar) grounded << out.portNames.at(p);",
    "            if (port.internal || !fromGround || true) grounded << out.portNames.at(p);")],
  [(TF, GSSG_F), (TC, TOOLS)]),

 ("a port returning through the ground driven against infinity",
  [(FW, "        if (!grounded.isEmpty()) {\n            out.problems << tr(\"There is no ground plane under the stack",
    "        if (false) {\n            out.problems << tr(\"There is no ground plane under the stack")],
  [(TF, GSSG_F), (TF, CANNOT_F)]),

 ("a via down to a ground that is none",
  [(FW, "    if (grounded > 0 && !c.layers.groundBelow) {", "    if (false) {")],
  [(TF, GSSG_F)]),

 # ---- the differential pairs and their modes (G5)
 ("a pair named from both ends two pairs",
  [(FW, "        if (ports.at(q).pair.trimmed() == name && q < p) continue;\n", "")],
  [(TF, PAIRS)]),

 ("a pair's two references not held alike",
  [(FW, "        if (std::abs(out.references.at(p) - out.references.at(q)) > 1e-9 * out.references.at(p)) {",
    "        if (false) {")],
  [(TF, PAIRS)]),

 ("the common mode a second difference",
  [(FW, "            t(row, minus) = sign * half;", "            t(row, minus) = -half * std::abs(sign);")],
  [(TF, GSSG_F), (TF, PAIRS)]),

 ("the modes by pair rather than each differential, then each common",
  [(FW, "    for (const double sign : {-1.0, 1.0})\n        for (int i = 0; i < solved.pairs.size(); ++i) {",
    "    for (int i = 0; i < solved.pairs.size(); ++i)\n        for (const double sign : {-1.0, 1.0}) {")],
  [(TF, GSSG_F), (TF, PAIRS)]),

 ("[Mixed-Mode Order] the common modes first",
  [(FW, "    for (const QChar mode : {QLatin1Char('D'), QLatin1Char('C')})",
    "    for (const QChar mode : {QLatin1Char('C'), QLatin1Char('D')})")],
  [(TF, PAIRS), (TC, TOOLS)]),

 ("the mixed-mode Touchstone's data the ports' own",
  [(FW, "    return touchstoneText(solved, modes, said, keywords, QStringLiteral(\"[End]\\n\"));",
    "    return touchstoneText(solved, solved.s, said, keywords, QStringLiteral(\"[End]\\n\"));")],
  [(TF, PAIRS)]),

 # ---- the tab
 ("the Return Conductors not handed to the full wave",
  [(DOC, "1e-6, &control, job->returns));", "1e-6, &control));")],
  [(TD, DRAWN)]),

 ("a port's Pair with not read",
  [(DOC, "        p.pair = textOf(port, QStringLiteral(\"pair\")).trimmed();", "        p.pair.clear();")],
  [(TC, TOOLS)]),

 ("the export node's Modes not read",
  [(DOC, "    out.mixed = textOf(*node, QStringLiteral(\"modes\")) == QLatin1String(\"mixed\");", "    out.mixed = false;")],
  [(TD, DRAWN)]),

 ("a mixed-mode export written port by port",
  [(DOC, "            return touchstone2Of(*a_fullWave, comments, true);", "            return touchstone2Of(*a_fullWave, comments);")],
  [(TC, TOOLS)]),

 ("a coplanar port's grounds drawn as the nearest conductors",
  [(DOC, "besideEdges(shapes, face, outward, layer, shapeReturns.isEmpty() ? nullptr : &shapeReturns)",
    "besideEdges(shapes, face, outward, layer, nullptr)")],
  [(TD, DRAWN)]),

 ("a uniform line's length its longer side",
  [(DOC, "        a_solvedLength = job->result && job->result->perMetre.fromMiddle && job->result->perMetre.length > 0",
    "        a_solvedLength = false && job->result->perMetre.fromMiddle && job->result->perMetre.length > 0")],
  [(TC, TOOLS)]),

 ("the tab's S summary of every net, the returns' too",
  [(DOC, "    const QList<int> signalNets = a_extraction->signalNets();\n    const int lines = int(signalNets.size());",
    "    QList<int> signalNets;\n    for (int i = 0; i < a_extraction->names.size(); ++i) signalNets << i;\n    const int lines = int(signalNets.size());")],
  [(TC, TOOLS)]),

 ("the coupled Touchstone's ports named of every net",
  [(DOC, "        for (const int i : a_extraction->signalNets()) lines << a_extraction->names.at(i);",
    "        for (int i = 0; i < a_extraction->names.size(); ++i) lines << a_extraction->names.at(i);")],
  [(TC, TOOLS)]),

 ("the coupled export's ports counted of every net",
  [(QC, "what.coupled ? 2 * int(model->extraction()->signalNets().size()) : 2;\n    out.insert(QStringLiteral(\"ports\"), ports);",
    "what.coupled ? 2 * int(model->extraction()->names.size()) : 2;\n    out.insert(QStringLiteral(\"ports\"), ports);")],
  [(TC, TOOLS)]),

 ("a Return Conductor named as no ground",
  [(MO, "    else if (type == QLatin1String(\"rf_return\")) n.label = numbered(QStringLiteral(\"G\"));",
    "    else if (type == QLatin1String(\"rf_return\")) n.label = numbered(QStringLiteral(\"R\"));")],
  [(TD, DRAWN)]),

 # ---- the tools (G3, G7)
 ("the coupled lines' matrices per metre left out",
  [(QC, "            coupled.insert(QStringLiteral(\"per metre\"), per);", "            (void)per;")],
  [(TC, TOOLS)]),

 ("what the lines return through left out",
  [(QC, "            if (!returning.isEmpty()) coupled.insert(QStringLiteral(\"returning through\"), returning);",
    "            (void)returning;")],
  [(TC, TOOLS)]),

 ("the coupled lines' S of a length unasked",
  [(QC, "                args.contains(QLatin1String(\"length\")) ? fem::coupledSParameters(cl, length, zref)",
    "                true ? fem::coupledSParameters(cl, length, zref)")],
  [(TC, TOOLS)]),

 ("the brief's modes without their n_eff",
  [(QC, "{\"name\", \"n_eff\", \"Z\", \"epsilon_eff\", \"alpha\"}", "{\"name\", \"Z\", \"epsilon_eff\", \"alpha\"}")],
  [(TC, TOOLS)]),

 ("the full-wave answer without the pairs' modes",
  [(QC, "    if (!mixed.isEmpty()) out.insert(QStringLiteral(\"mixed-mode\"), mixed);",
    "    if (false) out.insert(QStringLiteral(\"mixed-mode\"), mixed);")],
  [(TC, TOOLS)]),

 ("the modes' references the ports' own",
  [(QC, "                references.append(factor * w.references.at(pair.first));",
    "                references.append(w.references.at(pair.first) + 0 * factor);")],
  [(TC, TOOLS)]),

 ("mixed modes let into Touchstone 1.1",
  [(QC, "        if (args.value(QLatin1String(\"version\")).toString().trimmed() == QLatin1String(\"1.1\"))",
    "        if (false)")],
  [(TC, TOOLS)]),
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
