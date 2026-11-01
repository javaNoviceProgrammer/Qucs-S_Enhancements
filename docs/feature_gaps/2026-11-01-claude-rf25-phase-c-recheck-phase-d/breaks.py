#!/usr/bin/env python3
"""breaks.py: the recheck of 1 November's fixes undone on purpose, one at a
time - coplanar ports calibrated by strips of their feed with its grounds
and vias, the sheet's edge cells losing as the thickness lets the current
crowd there, de-embedding in waves, a gaining line moved along as losing
nothing, and no resonance of what is no line - each built and its tests
run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-phase-c-recheck-breaks")
os.makedirs(RUNS, exist_ok=True)

FW = "rf25/rf25_fullwave.cpp"
DE = "rf25/rf25_deembed.cpp"
D = "rf25doc.cpp"
C = "qucscontrol_rf25.cpp"
TF, TDE, TD, TC = "test_rf25_fullwave", "test_rf25_deembed", "test_rf25_doc", "test_qucs_control"
CPWCAL = "aCoplanarPortsFeedIsItsGroundsAndItsVias"
CANNOT = "whatACoplanarFeedsStripsCannotCopyIsSaid"
CUT = "stripsTooLargeAreCutAndSaid"
MEET = "aThruWhosePlanesMeetIsAThru"
HALF = "aLineDeembeddedHalfWayIsAThru"
GAIN = "aLineReadAsGainingIsMovedAlongAsLosingNothing"
CPWTAB = "aCoplanarPortIsReadAndItsFeedCalibrated"
EDGE = "aMetalsLossIsItsThickEdgesWhereTheCurrentCrowds"
LOSSES = "aSubstratesAndAMetalsLossesAreTheirs"
CPW = "aCoplanarPortReturnsThroughItsGrounds"
TOOL = "anRf25ModelIsSolvedThroughItsTool"

BREAKS = [
 # ---- coplanar ports calibrated by strips of their feed
 ("a coplanar feed's strips without its grounds",
  [(DE, "            if (feed.ground[0] > 0) rect(metal, 0, -feed.gap[0] - feed.ground[0], l, -feed.gap[0]);",
    "            if (false) rect(metal, 0, -feed.gap[0] - feed.ground[0], l, -feed.gap[0]);"),
   (DE, "            if (feed.ground[1] > 0) rect(metal, 0, width + feed.gap[1], l, width + feed.gap[1] + feed.ground[1]);",
    "            if (false) rect(metal, 0, width + feed.gap[1], l, width + feed.gap[1] + feed.ground[1]);")],
  [(TDE, CPWCAL), (TD, CPWTAB)]),

 ("a coplanar feed's strips without its vias",
  [(DE, "                    for (double c = centre; c <= l - centre + 1e-6 * l; c += feed.pitch)\n",
    "                    for (double c = centre; c < 0; c += feed.pitch)\n")],
  [(TDE, CPWCAL)]),

 ("the longer strip twice the shorter's length, not its periods",
  [(DE, "            const double l = 2 * offset + between * (i + 1);", "            const double l = (2 * offset + between) * (i + 1);")],
  [(TDE, CPWCAL)]),

 ("the port's discontinuity left at the feed's first via",
  [(DE, "        error << (offset > 0 ? times(e, lineOf(lossless, z, -offset * micrometre)) : e);", "        (void)lossless;\n        error << e;")],
  [(TDE, CPWCAL)]),

 ("the feed's vias found nowhere",
  [(FW, "    const Layer* metal = stack.named(feed->metal);\n    if (metal == nullptr) return;",
    "    const Layer* metal = stack.named(feed->metal);\n    if (metal != nullptr) return;")],
  [(TDE, CPWCAL)]),

 ("rows staggered by a third of a period taken for strips' rows",
  [(FW, "        if (std::abs(halves - std::round(halves)) > 1e-3) {", "        if (false) {")],
  [(TDE, CANNOT)]),

 ("a via on the signal not seen",
  [(FW, "        if (middle > 0 && middle < w) {", "        if (false) {")],
  [(TDE, CANNOT)]),

 ("a coplanar feed keyed apart from its mirror image",
  [(FW, "    return std::min(text(false), text(true));", "    return text(false);")],
  [(TDE, CPWCAL)]),

 ("a feed's vias keyed in the order they were found",
  [(FW, "        each.sort();\n", "")],
  [(TDE, CPWCAL)]),

 ("the tab's coplanar port not calibrated",
  [(D, "                std::shared_ptr<Calibration>& calibration = feeds[feed.key()];",
    "                if (port.coplanar) continue;\n                std::shared_ptr<Calibration>& calibration = feeds[feed.key()];")],
  [(TD, CPWTAB)]),

 ("strips too large not cut",
  [(DE, "            if (fw.tooLarge && attempt < 2 && chosen <= 0) {", "            if (false) {")],
  [(TDE, CUT)]),

 # ---- the sheet's edge cells
 ("no crowding at a conductor's edge",
  [(FW, "    return std::max(1.0, free >= 2 ? 2 / (M_PI * M_PI) * log : log / 4);", "    (void)log;\n    return 1.0;")],
  [(TF, EDGE)]),

 ("no edge found: no side of a run free",
  [(FW, "            if (!inside) ++c.freeSides[a];", "            (void)inside;")],
  [(TF, EDGE)]),

 ("every side of every run taken as free",
  [(FW, "                if (shape.layer == layer && shape.outline.boundingRect().contains(probe)",
    "                if (false && shape.layer == layer && shape.outline.boundingRect().contains(probe)")],
  [(TF, EDGE)]),

 ("a thin metal's own resistance not spreading the current",
  [(FW, "    const double spread = 2 * std::max(0.0, sheet.real() - faces) / (omega * mu0);",
    "    const double spread = 0 * faces;")],
  [(TF, LOSSES)]),

 ("the crowding stopped at the thickness itself, not t/(4 pi e^pi)",
  [(FW, "    const double stop = thickness / (4 * M_PI * std::exp(M_PI));", "    const double stop = thickness;")],
  [(TF, EDGE)]),

 # ---- de-embedding in waves
 ("de-embedded through Z again",
  [(DE, "        const MatrixC s = Eigen::PartialPivLU<MatrixC>(grown).solve(x);",
    "        const MatrixC zs = matrixOf(solved.z.at(k));\n"
    "        MatrixC left = -(zs * c.asDiagonal());\n        left.diagonal() += a;\n"
    "        MatrixC right = zs * d.asDiagonal();\n        right.diagonal() -= b;\n"
    "        const MatrixC inner = Eigen::PartialPivLU<MatrixC>(left).solve(right);\n"
    "        (void)grown;\n"
    "        const MatrixC s = matrixOf(scatteringOf(rowsOf(inner), out.references));")],
  [(TDE, MEET)]),

 ("the error box's two ends swapped",
  [(DE, "            e11(p) = (a(p) + b(p) / r - c(p) * r - d(p)) / den;", "            e11(p) = (-a(p) + b(p) / r - c(p) * r + d(p)) / den;"),
   (DE, "            e22(p) = (-a(p) + b(p) / r - c(p) * r + d(p)) / den;", "            e22(p) = (a(p) + b(p) / r - c(p) * r - d(p)) / den;")],
  [(TDE, HALF), (TDE, CPWCAL)]),

 # ---- a gaining line moved along as losing nothing
 ("a line read as gaining moved along as read",
  [(DE, "                        gamma = Complex(0, gamma.imag());", "                        (void)0;")],
  [(TDE, GAIN)]),

 # ---- no resonance of what is no line
 ("the resonance of what is no line said at nought",
  [(C, "        if (limit.resonance > 0) said.insert(QStringLiteral(\"first half-wave resonance\"), limit.resonance);",
    "        said.insert(QStringLiteral(\"first half-wave resonance\"), limit.resonance);")],
  [(TC, TOOL)]),
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
