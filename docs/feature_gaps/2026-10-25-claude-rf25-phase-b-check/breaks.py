#!/usr/bin/env python3
"""breaks.py: each fix of the RF 2.5D phase B check of 24 October undone on
purpose - a line's per metre of its cross-section, a lone conductor with no
ground no line, cells across a conductor and the mesh fitted to the solver,
the Touchstone's reference said, one count of the charge, a pair's span, a
pair shown by its modes, a brief answer, and a loop's resistance at direct
current of the current at direct current. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-phase-b-check-breaks")
os.makedirs(RUNS, exist_ok=True)

S, M, MH, MO = "rf25/rf25_static.cpp", "rf25/rf25_mesh.cpp", "rf25/rf25_mesh.h", "rf25/rf25_model.cpp"
D, SP, T, I = "rf25doc.cpp", "rf25/rf25_sparams.cpp", "qucscontrol_rf25.cpp", "rf25/rf25_inductance.cpp"
ST, ME, MOD, DOC, SPS, IND, CTL = ("test_rf25_static", "test_rf25_mesh", "test_rf25_model", "test_rf25_doc",
                                   "test_rf25_sparams", "test_rf25_inductance", "test_qucs_control")
MICRO = "aMicrostripsImpedanceIsThatOfTheClosedForm"
PAIR = "aCoupledPairsModesAreOfItsCrossSection"
LONE = "aLoneConductorWithNoGroundIsNoLine"
UNIFORM = "whatIsNoUniformLineIsSaidToBe"
LOSS = "theLossMatrixCouplesAsWellAsLoads"
LIMIT = "theQuasiStaticLimitIsSaidRatherThanAssumed"
BLANK = "aBlankModelIsWrittenAndRead"
ACROSS = "aConductorHasCellsAcrossIt"
FITTED = "aMeshTooLargeForTheSolverHasFewerAcross"
HALVED = "theCellsAtAnEdgeAreHalved"
READ = "theCellsAcrossAndTheStretchAreRead"
WRITTEN = "theSolvedModelIsWrittenAsATouchstone"
MADE = "theShapesAndTheMeshAreMadeOfTheLayoutRead"
MODES = "aPairIsShownByItsModes"
TOOL = "anRf25ModelIsSolvedThroughItsTool"
BAR = "aBarsResistanceAtDirectCurrentIsItsOwnOnAnyMesh"

BREAKS = [
 # ---- F1: a uniform line's per metre, of its cross-section
 ("per metre is the whole over the length again",
  [(S, "    const bool middle = pm.fromMiddle && index < pm.maxwell.size() && index < pm.maxwell0.size();",
    "    const bool middle = false && index < pm.maxwell.size() && index < pm.maxwell0.size();")],
  [(ST, MICRO), (CTL, TOOL)]),

 ("the cross-section's strips are not tiled along: one copy each",
  [(S, "                const int copies = int(std::ceil(reach / delta));", "                const int copies = 0;")],
  [(ST, MICRO)]),

 ("the copies summed only as far as the span twice",
  [(S, "        reach = 100 * std::max((hi - lo) * micrometre, top);", "        reach = 2 * std::max((hi - lo) * micrometre, top);")],
  [(ST, MICRO)]),

 ("a charge per metre puts no length of it on each copy",
  [(S, "                p(a, b) = sum * delta;", "                p(a, b) = sum;")],
  [(ST, MICRO)]),

 ("a net broader than its middle (an L) is taken as a line",
  [(S, "            if (first || mid.isEmpty() || std::abs(lo - midLo) > eps || std::abs(hi - midHi) > eps) uniform = false;",
    "            if (first || mid.isEmpty()) uniform = false;")],
  [(ST, UNIFORM)]),

 ("the cross-section is not compared along the line (a step passes)",
  [(S, "        for (const double at : {lineFrom + lineLength / 4, lineFrom + 3 * lineLength / 4}) {",
    "        for (const double at : std::initializer_list<double>{}) {")],
  [(ST, UNIFORM)]),

 ("G per metre is not of the cross-section",
  [(S, "            if (uniform) crossSection(fromNudged, &nudgedPerMetre, nullptr);",
    "            if (false) crossSection(fromNudged, &nudgedPerMetre, nullptr);")],
  [(SPS, LOSS)]),

 # ---- F6: a lone conductor with no ground
 ("a lone conductor with no ground is a line again",
  [(S, "    if (!extraction.grounded && extraction.c.size() == 1) {", "    if (false) {")],
  [(ST, LONE)]),

 ("the extraction takes every stack as grounded",
  [(S, "    out.grounded = layers0.groundBelow;", "    out.grounded = true;")],
  [(ST, LONE)]),

 ("the starter model has no ground again",
  [(MO, "    stack.children.push_back(ground);\n", "")],
  [(MOD, BLANK)]),

 # ---- F2, F3: the mesh
 ("no cells across a conductor by default",
  [(MH, "    int cellsAcross = 10;", "    int cellsAcross = 0;")],
  [(ME, ACROSS)]),

 ("a strip's cells are not stretched along it",
  [(M, "            if (strip && how.stretch > 1) along = std::min(size, how.stretch * across);",
    "            if (false) along = std::min(size, how.stretch * across);")],
  [(ME, ACROSS)]),

 ("a shape that is no rectangle goes by its bounds' short side",
  [(M, "            const double feature = rectangle ? narrow : (perimeter > 0 ? 2 * area / perimeter : narrow);",
    "            const double feature = narrow;")],
  [(ME, ACROSS)]),

 ("the outermost cells of a side are not halved",
  [(M, "    if (graded) at << from + d / 2;", "    if (false) at << from + d / 2;"),
   (M, "    if (graded) at << from + length - d / 2;", "    if (false) at << from + length - d / 2;")],
  [(ME, HALVED)]),

 ("a mesh too large for the solver is refused, not fitted",
  [(M, "    while (!fits && perFeature > 1) {", "    while (false) {")],
  [(ME, FITTED)]),

 ("the solver's cap back at 4000",
  [(MH, "constexpr int kMostCharges = 8000;", "constexpr int kMostCharges = 4000;")],
  [(ME, FITTED)]),

 ("the node's cells across is not read",
  [(M, "    if (across.isDouble()) how.cellsAcross = across.toInt();", "    if (false) how.cellsAcross = across.toInt();")],
  [(ME, READ)]),

 # ---- N1-N5
 ("the Touchstone's comment is into the line's own impedance (N1)",
  [(D, "    for (const QString& line : sParameterSummary(what.impedance)) comments << line;",
    "    for (const QString& line : sParameterSummary()) comments << line;")],
  [(DOC, WRITTEN)]),

 ("the status line gives the full-wave unknowns again (N2)",
  [(D, "        tail += tr(\"; %1 cell(s), %2\").arg(a_mesh->cells.size()).arg(chargeCostOf(int(a_mesh->cells.size())));",
    "        tail += tr(\"; %1 cell(s), %2\").arg(a_mesh->cells.size()).arg(a_mesh->cost.said);")],
  [(DOC, MADE)]),

 ("a pair's span is said as the widest conductor (N3)",
  [(SP, "        out.what = conductors > 1 ? tr(\"the conductors span %1 µm across\").arg(wide * 1e6, 0, 'g', 4)",
    "        out.what = false ? tr(\"the conductors span %1 µm across\").arg(wide * 1e6, 0, 'g', 4)")],
  [(SPS, LIMIT)]),

 ("the tab gives a pair's nets as lines of their own (N4)",
  [(D, "    if (a_extraction->names.size() > 1) {", "    if (false) {")],
  [(DOC, MODES)]),

 ("the tab's S of a pair is each net's two-port (N4)",
  [(D, "    if (nets > 1) {", "    if (false) {")],
  [(DOC, MODES)]),

 ("the tool gives a pair's nets as lines of their own (N4)",
  [(T, "        if (e->names.size() > 1) {", "        if (false) {")],
  [(CTL, TOOL)]),

 ("brief is not read (N5)",
  [(T, "    if (args.value(QLatin1String(\"brief\")).toBool()) {", "    if (false) {")],
  [(CTL, TOOL)]),

 # ---- found on the way: a loop's resistance at direct current
 ("a loop's resistance at direct current is the least-energy current's again",
  [(I, "    if (resistive) {", "    if (false) {")],
  [(IND, BAR)]),

 ("the tool gives no inductance at direct current",
  [(T, "    out.insert(QStringLiteral(\"L_dc\"), ind->lDc);\n", "")],
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
        print("%-66s %s" % (name[:66], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
