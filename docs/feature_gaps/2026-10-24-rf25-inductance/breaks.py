#!/usr/bin/env python3
"""breaks.py: the partial inductances and the loop solved from them - the
closed form, the geometric mean distance, the signs, the runs the mesh's
shared edges give, the terminal stubs, the face a port drives, the energy
minimised under Kirchhoff's law, and what is said of it - undone on
purpose. Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks
the anchors; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-inductance-breaks")
os.makedirs(RUNS, exist_ok=True)

I, D, T, P = "rf25/rf25_inductance.cpp", "rf25doc.cpp", "qucscontrol_rf25.cpp", "rf25panel.cpp"
IND, DOC, CTL = "test_rf25_inductance", "test_rf25_doc", "test_qucs_control"
STRIP = "aStripsOwnInductanceIsTheTextbooks"
MUTUAL = "twoFilamentsMutualIsTheirDoubleIntegral"
PIECES = "aStripCutIntoPiecesAddsBackUpToItself"
NEAR = "aNearPairIsAveragedAcrossItsWidths"
CORNER = "aCurrentThatTurnsACornerCrowdsAtIt"
TWOWIRE = "aTwoWireLineIsTheDifferenceOfItsIntegrals"
GROVER = "aRectangularLoopIsGroversFormula"
RUNSOF = "theRunsOfCurrentAreTheCellsThatShareAnEdge"
BAR = "aStraightBarsSolvedInductanceIsItsOwn"
PLATE = "aWidePlateLosesLessThanANarrowStrip"
REFUSE = "whatHasNoLoopIsRefused"
LOOP = "theLoopsInductanceIsSolvedFromItsPorts"
TOOL = "anRf25ModelIsSolvedThroughItsTool"

BREAKS = [
 # ---- the closed form
 ("the antiderivative loses its root, so the integral is not one",
  [(I, "    return s * std::asinh(s / rho) - std::sqrt(s * s + rho * rho);", "    return s * std::asinh(s / rho);")],
  [(IND, STRIP)]),

 ("the four-term form is taken with three of them",
  [(I, "    return antiderivative(d + lb, rho) - antiderivative(d + lb - la, rho) - antiderivative(d, rho) + antiderivative(d - la, rho);",
    "    return antiderivative(d + lb, rho) - antiderivative(d + lb - la, rho) - antiderivative(d, rho);")],
  [(IND, STRIP)]),

 ("the middle two terms are added rather than taken away",
  [(I, "    return antiderivative(d + lb, rho) - antiderivative(d + lb - la, rho) - antiderivative(d, rho) + antiderivative(d - la, rho);",
    "    return antiderivative(d + lb, rho) + antiderivative(d + lb - la, rho) + antiderivative(d, rho) + antiderivative(d - la, rho);")],
  [(IND, STRIP)]),

 ("the permeability of free space is left out of it",
  [(I, "constexpr double kMu0 = 4e-7 * M_PI;", "constexpr double kMu0 = 1.0;")],
  [(IND, STRIP)]),

 ("it is mu0/4pi times nothing of the sort",
  [(I, "        return sign * kMu0 / (4 * M_PI) * parallelPair(la, lb, d, rho);",
    "        return sign * kMu0 * parallelPair(la, lb, d, rho);")],
  [(IND, STRIP)]),

 # ---- the geometric mean distance
 ("the geometric mean distance of a strip is its width",
  [(I, "    return width * std::exp(-1.5);", "    return width;")],
  [(IND, STRIP)]),

 ("the geometric mean distance is e^-3/2 the wrong way about",
  [(I, "    return width * std::exp(-1.5);", "    return width * std::exp(1.5);")],
  [(IND, STRIP)]),

 ("a strip of no width is given one",
  [(I, "    if (!(width > 0)) return 0;\n    return width * std::exp(-1.5);", "    return width * std::exp(-1.5);")],
  [(IND, STRIP)]),

 # (PIECES cannot see this one: it takes the whole strip's own term by the
 #  same call it is comparing against, so a break in it is self-consistent.)
 ("a run's own term is taken at the distance between its axes, which is none",
  [(I, "    if (same || apart <= 1e-9 * std::max(widest, la)) {", "    if (false) {")],
  [(IND, STRIP)]),

 ("two pieces of one strip are taken as being apart",
  [(I, "    if (same || apart <= 1e-9 * std::max(widest, la)) {", "    if (same) {")],
  [(IND, PIECES)]),

 # ---- the signs and the directions
 ("a current going the other way puts its flux the same way",
  [(I, "    const double sign = along > 0 ? 1.0 : -1.0;", "    const double sign = 1.0;")],
  [(IND, MUTUAL), (IND, TWOWIRE)]),

 ("runs across one another are given a mutual all the same",
  [(I, "    if (std::abs(along) < 1e-9) return 0;", "    if (false) return 0;")],
  [(IND, MUTUAL)]),

 ("the offset along the axis is dropped, so every pair is side by side",
  [(I, "    const double d = std::min(sFrom, sTo);", "    const double d = 0;")],
  [(IND, PIECES)]),

 ("the distance between the axes keeps the part along them",
  [(I, "    const Vector offset = bFrom - da * sFrom;", "    const Vector offset = bFrom;")],
  [(IND, MUTUAL)]),

 # ---- near and far
 ("every pair is taken between its middles, however near",
  [(I, "    if (apart > kNearWidths * widest || !(a.width > 0) || !(b.width > 0))",
    "    if (true)")],
  [(IND, NEAR)]),

 ("the quadrature across the widths does not weigh its points",
  [(I, "            sum += kGaussWeight[i] * kGaussWeight[j] * parallelPair(la, lb, d, rho);",
    "            sum += parallelPair(la, lb, d, rho);")],
  [(IND, NEAR)]),

 ("a width lies along its own run rather than across it",
  [(I, "    return {-d.y, d.x, 0};", "    return {d.x, d.y, 0};")],
  [(IND, NEAR)]),

 # ---- the matrix and the loop
 ("the partial inductance matrix is filled on one side only",
  [(I, "            out[i][j] = m;\n            out[j][i] = m;", "            out[i][j] = m;")],
  [(IND, PIECES)]),

 ("a loop is the sum of its own terms, its pairs left out",
  [(I, "    for (int i = 0; i < loop.size(); ++i)\n        for (int j = 0; j < loop.size(); ++j) sum += partialInductance(loop.at(i), loop.at(j));",
    "    for (int i = 0; i < loop.size(); ++i) sum += partialInductance(loop.at(i), loop.at(i));")],
  [(IND, TWOWIRE), (IND, GROVER)]),

 # ---- the runs the mesh gives
 ("a run's width is not the edge the two cells share",
  [(I, "        f.width = edge * micrometre;", "        f.width = 1e-6;")],
  [(IND, BAR)]),

 ("a run is put at the bottom of the stack whatever metal it is on",
  [(I, "        f.z0 = f.z1 = z;", "        f.z0 = f.z1 = 0;")],
  [(IND, RUNSOF)]),

 ("a run forgets the metal's thickness, so it has no resistance",
  [(I, "        f.thickness = metal != nullptr ? metal->thickness : 0.0;", "        f.thickness = 0.0;")],
  [(IND, RUNSOF)]),

 ("a run's resistance leaves out its own length",
  [(I, "    return length() / (sigma * area);", "    return 1.0 / (sigma * area);")],
  [(IND, BAR)]),

 ("a run's resistance is of its width alone, its thickness dropped",
  [(I, "    const double area = width * thickness;", "    const double area = width;")],
  [(IND, BAR)]),

 ("a metal that says no conductivity is given a resistance",
  [(I, "    if (!(sigma > 0) || !(area > 0)) return 0;", "    if (false) return 0;")],
  [(IND, RUNSOF)]),

 # ---- the terminals
 ("the stubs at the ends are left off, so the path is a half cell short at each",
  [(I, "    for (const int cell : sourceFace) {", "    for (const int cell : QList<int>()) {"),
   (I, "    for (const int cell : sinkFace) {", "    for (const int cell : QList<int>()) {")],
  [(IND, BAR)]),

 ("a stub starts where the port is rather than where the cell meets the face",
  [(I, "        QPointF at = outside;\n        if (unit != QPointF(0, 0)) {", "        QPointF at = outside;\n        if (false) {")],
  [(IND, BAR)]),

 ("only the one cell nearest the port is driven, not the face",
  [(I, "            if (std::max(tA, tB) < -tol || std::min(tA, tB) > len + tol) continue;\n                cells << it.key();",
    "            if (std::max(tA, tB) < -tol || std::min(tA, tB) > len + tol) continue;\n                if (cells.isEmpty()) cells << it.key();")],
  [(IND, BAR)]),

 ("the face is taken from any outline edge, near or far",
  [(I, "                if (layer.isEmpty() || far < closest) {", "                if (layer.isEmpty() || far > closest) {")],
  [(IND, BAR), (IND, CORNER)]),

 ("a stub's width is not the cell's own breadth",
  [(I, "        f.width = acrossOf(cell, along) * micrometre;", "        f.width = 1e-9;")],
  [(IND, BAR)]),

 ("both ends of the loop being on one face is answered rather than refused",
  [(I, "        if (a == b) {", "        if (false) {")],
  [(IND, REFUSE), (CTL, TOOL)]),

 ("two faces that merely meet at a corner are refused as one",
  [(I, "        QList<int> a = sourceFace, b = sinkFace;\n        std::sort(a.begin(), a.end());\n        std::sort(b.begin(), b.end());\n        if (a == b) {",
    "        QList<int> a = sourceFace, b = sinkFace;\n        for (const int c : b) a.removeAll(c);\n        if (a.isEmpty() || a.size() < sourceFace.size()) {")],
  [(IND, CORNER)]),

 # ---- the solve
 ("Kirchhoff's law is not written into the system",
  [(I, "        if (a != drop) {\n            m(row(a), b) += 1;\n            m(b, row(a)) += 1;\n        }",
    "        if (false) {\n            m(row(a), b) += 1;\n            m(b, row(a)) += 1;\n        }")],
  [(IND, BAR)]),

 ("the two ends of a branch are given the same sign in it",
  [(I, "            m(row(c), b) -= 1;\n            m(b, row(c)) -= 1;", "            m(row(c), b) += 1;\n            m(b, row(c)) += 1;")],
  [(IND, BAR)]),

 ("no current is injected at all",
  [(I, "    rhs(row(in)) = 1.0;", "    rhs(row(in)) = 0.0;")],
  [(IND, BAR)]),

 ("the energy minimised is the current's square, not its magnetic energy",
  [(I, "        for (int j = 0; j < nb; ++j) m(i, j) = lp.at(i).at(j);",
    "        for (int j = 0; j < nb; ++j) m(i, j) = i == j ? 1.0 : 0.0;")],
  [(IND, CORNER)]),

 ("a system that cannot be solved is solved anyway",
  [(I, "    if (!lu.isInvertible()) {", "    if (false) {")],
  [(IND, REFUSE)]),

 ("the inductance is the energy of one branch rather than of all the pairs",
  [(I, "        for (int j = 0; j < nb; ++j) energy += lp.at(i).at(j) * answer(i) * answer(j);",
    "        energy += lp.at(i).at(i) * answer(i) * answer(i);")],
  [(IND, BAR)]),

 ("the resistance at direct current forgets that it goes as the current squared",
  [(I, "        loss += out.runs.at(i).resistance() * answer(i) * answer(i);",
    "        loss += out.runs.at(i).resistance() * answer(i);")],
  [(IND, BAR)]),

 ("a mesh with no two cells sharing an edge is solved anyway",
  [(I, "    if (out.runs.isEmpty()) {", "    if (false) {")],
  [(IND, REFUSE)]),

 ("a mesh past what a dense matrix holds is set up all the same",
  [(I, "    if (out.branches + out.nodes > kMostRuns) {", "    if (false) {")],
  [(IND, REFUSE)]),

 ("Q is not w L over R",
  [(I, "    if (frequency > 0 && loss > 0) out.q = 2 * M_PI * frequency * energy / loss;",
    "    if (frequency > 0 && loss > 0) out.q = energy / loss;")],
  [(IND, BAR)]),

 # ---- what is said of it, and the tools
 ("the ports do not say what the loop is",
  [(D, "    if (at.size() >= 2) {", "    if (false) {")],
  [(DOC, LOOP), (CTL, TOOL)]),

 ("a model with one port is given a loop all the same",
  [(D, "    if (at.size() >= 2) {", "    if (at.size() >= 1) {")],
  [(DOC, LOOP)]),

 ("the ports are read as though the cells were in metres",
  [(D, "        at << QPointF(x, y) / 1e-6;", "        at << QPointF(x, y);")],
  [(DOC, LOOP)]),

 ("the loop is kept when the shapes it was of have gone",
  [(D, "    a_extraction.reset();\n    a_inductance.reset();\n    a_portsSaid.clear();\n    a_solvedLength = 0;\n    a_solveProblem.clear();\n    showOverlays();",
    "    a_extraction.reset();\n    a_portsSaid.clear();\n    a_solvedLength = 0;\n    a_solveProblem.clear();\n    showOverlays();")],
  [(DOC, LOOP)]),

 ("the tab says nothing of the loop",
  [(D, '    out << tr("The loop is %1 nH over %2 run(s) of current, and %3 Ohm at direct current.")',
    '    out << tr("%1 %2 %3")')],
  [(DOC, LOOP)]),

 ("the status line says nothing of it",
  [(D, '    else if (a_inductance && a_inductance->ok()) tail += tr("; the loop is %1 nH").arg(a_inductance->l * 1e9, 0, \'g\', 4);', "")],
  [(DOC, LOOP)]),

 ("the Results node says nothing of it",
  [(P, "            did += a_doc->inductanceSummary();", "")],
  [(DOC, LOOP)]),

 ("the tool answers a loop that came to nothing",
  [(T, "    if (!ind->ok())", "    if (false)")],
  [(CTL, TOOL)]),

 ("the tool's Q is at the study's frequency whatever it was asked",
  [(T, "    const double at = asked > 0 ? asked : ind->frequency;", "    const double at = ind->frequency;")],
  [(CTL, TOOL)]),

 ("the tool takes a frequency that is none",
  [(T, '        if (!given) return errorResult(tr("\'frequency\' is a frequency above zero (\\"10[GHz]\\")."));\n        asked = *given;',
    "        asked = given ? *given : 0;")],
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
        print("%-72s %s" % (name[:72], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
