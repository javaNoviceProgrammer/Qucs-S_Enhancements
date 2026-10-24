#!/usr/bin/env python3
"""breaks.py: each piece of the quasi-static extraction undone on purpose,
to see its test fail. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors alone; HUNT_RUNS=<dir> for the logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-static-breaks")
os.makedirs(RUNS, exist_ok=True)

G, S = "rf25/rf25_green.cpp", "rf25/rf25_static.cpp"
GREEN, STATIC = "test_rf25_green", "test_rf25_static"

BREAKS = [
 # ---- Bessel's J0
 ("J0 is its series everywhere, which cancels itself away above 8",
  [(G, "    if (ax < 8.0) {", "    if (true) {")],
  [(GREEN, "besselJ0IsItsOwn")]),

 # ---- the stack read as slabs
 ("a metal takes room in the stack, as if its thickness were dielectric",
  [(G, "        case Layer::Metal:\n        case Layer::Via:\n            break;   // no room of its own",
    "        case Layer::Metal:\n        case Layer::Via:\n            z += l.thickness;\n            break;")],
  [(GREEN, "theStackIsReadAsSlabs")]),

 ("a stack with no dielectric at all is solved anyway",
  [(G, '    if (out.slabs.isEmpty()) out.problems << tr("The stack has no dielectric to solve in.");', "")],
  [(GREEN, "theStackIsReadAsSlabs")]),

 ("a charge outside the stack is placed in it all the same",
  [(G, "    if (at < 0) {", "    if (false) {")],
  [(GREEN, "freeSpaceIsCoulombsLaw")]),

 # ---- the reflection coefficients
 ("an interface reflects the wrong way round",
  [(G, "        a_rUp << (here - above) / (here + above);", "        a_rUp << (above - here) / (here + above);")],
  [(GREEN, "oneInterfaceIsItsImage")]),

 ("a ground plane reflects as a dielectric would, not wholly",
  [(G, "    double rd = a_ground ? -1.0 : (a_rDown.isEmpty() ? 0.0 : a_rDown.last());\n    for (int i = a_rDown.size() - 2; i >= 0; --i) {\n        const double e = std::exp(-2 * k * a_hDown.at(i));",
    "    double rd = a_rDown.isEmpty() ? 0.0 : a_rDown.last();\n    for (int i = a_rDown.size() - 2; i >= 0; --i) {\n        const double e = std::exp(-2 * k * a_hDown.at(i));")],
  [(GREEN, "aGroundPlaneIsTheImageCharge")]),

 ("the reflection of the slabs above is not carried down through them",
  [(G, "        ru = (a_rUp.at(i) + ru * e) / (1 + a_rUp.at(i) * ru * e);", "        ru = a_rUp.at(i);")],
  [(GREEN, "theImagesSumBackToTheSpectralReflection"), (GREEN, "theImagesAndTheIntegralAgree")]),

 # ---- the images
 ("the images of the ground plane are left out of the expansion",
  [(G, '    Series down{{a_ground ? -1.0 : (a_rDown.isEmpty() ? 0.0 : a_rDown.last()), 0.0}};',
    '    Series down{{0.0, 0.0}};')],
  [(GREEN, "aGroundPlaneIsTheImageCharge"), (STATIC, "aPlateOverAGroundPlaneIsItsParallelPlateValuePlusFringing")]),

 ("an image's path is as long as one bounce, not the round trip",
  [(G, "    Series series = shifted(down, 2 * a_down) + shifted(up, 2 * a_up) + scaled(both, 2.0);",
    "    Series series = shifted(down, a_down) + shifted(up, a_up) + scaled(both, 2.0);")],
  [(GREEN, "aGroundPlaneIsTheImageCharge")]),

 ("the multiple bounces between the two interfaces are not summed",
  [(G, "    series = times(tidied(std::move(series), &a_truncated), overOneMinus(both, &a_truncated), &a_truncated);",
    "    series = tidied(std::move(series), &a_truncated);")],
  [(GREEN, "aSlabOverGroundIsItsImageSeries")]),

 ("the geometric series of 1/(1 - x) stops at its first term",
  [(G, "    for (int n = 0; n < kMostBounces; ++n) {", "    for (int n = 0; n < 0; ++n) {")],
  [(GREEN, "aSlabOverGroundIsItsImageSeries")]),

 # (Terms fall below the floor in two places - as a product is formed, and
 # again once equal paths have been added together, where they can cancel -
 # so the behaviour is both of them.)
 ("what the truncation threw away goes uncounted",
  [(G, "        if (std::abs(t.first) < kTermFloor) {\n            if (lost != nullptr) *lost += std::abs(t.first);",
    "        if (std::abs(t.first) < kTermFloor) {\n            if (false) *lost += std::abs(t.first);"),
   (G, "            if (std::abs(c) >= kTermFloor) out << QPair<double, double>{c, x.second + y.second};\n            else if (lost != nullptr) *lost += std::abs(c);",
    "            if (std::abs(c) >= kTermFloor) out << QPair<double, double>{c, x.second + y.second};")],
  [(GREEN, "theImagesAndTheIntegralAgree")]),

 ("the bounces running out go uncounted, as though nothing were missing",
  [(G, "    if (lost != nullptr)\n        for (const auto& t : std::as_const(power)) *lost += std::abs(t.first);", "")],
  [(GREEN, "whatTheTruncationDroppedIsCounted")]),

 ("a stack whose images cannot be enumerated is solved on them anyway",
  [(S, "    if (green.truncated() > kMostLost) {", "    if (false) {")],
  [(STATIC, "whatItCannotSolveIsRefused")]),

 # ---- the spectral integral, the reference the images are held to
 ("the free-space part is not taken out of the integrand",
  [(G, "    const auto f = [this, tail](double k) { return spectralReflection(k) - tail; };",
    "    const auto f = [this](double k) { return spectralReflection(k); };")],
  [(GREEN, "theImagesAndTheIntegralAgree")]),

 ("the rectangle's potential leaves out its out-of-plane term",
  [(S, "    if (z != 0 && r > 0 && x != 0 && y != 0) out -= z * std::atan(x * y / (z * r));", "")],
  [(STATIC, "theRectanglesClosedFormIsTheIntegral")]),

 ("the rectangle's potential is its integral, not its average",
  [(S, "    return area > 0 ? integral / area : 0;", "    return area > 0 ? integral : 0;")],
  [(STATIC, "theRectanglesClosedFormIsTheIntegral")]),

 ("one corner of the rectangle is added where it should be taken away",
  [(S, "    const double integral = rectangleTerm(x2, y2, z) - rectangleTerm(x1, y2, z) - rectangleTerm(x2, y1, z)\n                            + rectangleTerm(x1, y1, z);",
    "    const double integral = rectangleTerm(x2, y2, z) - rectangleTerm(x1, y2, z) + rectangleTerm(x2, y1, z)\n                            + rectangleTerm(x1, y1, z);")],
  [(STATIC, "theRectanglesClosedFormIsTheIntegral")]),

 # ---- the matrix
 ("every pair is taken at the cell's centre, its own term included",
  [(S, "            if (i == j || distance < kNearWidths * side) {", "            if (false) {")],
  [(STATIC, "aMicrostripsImpedanceIsThatOfTheClosedForm")]),

 ("the images of the stack are left out of the near terms",
  [(S, "    QList<Green::Image> terms{Green::Image{1.0, 0.0}};\n    terms += green.images();",
    "    QList<Green::Image> terms{Green::Image{1.0, 0.0}};")],
  [(STATIC, "aMicrostripsImpedanceIsThatOfTheClosedForm")]),

 ("the charge is placed at the bottom of the stack, not under its metal",
  [(S, "        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) z += l.thickness;", "")],
  [(STATIC, "aMicrostripsImpedanceIsThatOfTheClosedForm")]),

 ("C0 is solved on the stack as it is, not on one of vacuum",
  [(S, "    for (Layers::Slab& s : vacuum.slabs) s.epsr = 1;", "")],
  [(STATIC, "aMicrostripsImpedanceIsThatOfTheClosedForm")]),

 # ---- the matrices and what comes of them
 ("the mutual matrix keeps the sign the Maxwell one has",
  [(S, "                row << (i == j ? sum : -m.at(i).at(j));", "                row << (i == j ? sum : m.at(i).at(j));")],
  [(STATIC, "twoLinesCoupleAndTheCouplingFallsWithTheGap")]),

 ("a conductor's capacitance to everything else is its own term alone",
  [(S, "    double sum = 0;\n    for (int j = 0; j < c.at(i).size(); ++j) sum += c.at(i).at(j);\n    return sum;",
    "    return c.at(i).at(i);")],
  [(STATIC, "twoLinesCoupleAndTheCouplingFallsWithTheGap")]),

 ("a line's impedance is not the one the engine's own lineparams gives",
  [(S, "    out.z0 = 1.0 / (kLightSpeed * std::sqrt(out.c * out.c0));", "    out.z0 = std::sqrt(out.l / out.c);")],
  [(STATIC, "aMicrostripsImpedanceIsThatOfTheClosedForm")]),

 ("a length of nothing is taken as a line",
  [(S, "    if (!(length > 0)) {", "    if (false) {")],
  [(STATIC, "aMicrostripsImpedanceIsThatOfTheClosedForm")]),

 # ---- what it refuses
 ("cells on two metal layers are solved as though they were on one",
  [(S, "    if (layers.size() > 1) {", "    if (false) {")],
  [(STATIC, "whatItCannotSolveIsRefused")]),

 ("a conductor on a layer that is no metal is solved all the same",
  [(S, "    if (metal->kind != Layer::Metal) {", "    if (false) {")],
  [(STATIC, "whatItCannotSolveIsRefused")]),

 ("a mesh past what a dense matrix holds is solved until it swaps",
  [(S, "    if (n > 4000) {", "    if (false) {")],
  [(STATIC, "whatItCannotSolveIsRefused")]),

 ("a cell belongs to no conductor in particular",
  [(S, "            if (conductors.at(i).outline.containsPoint(cell.centre, Qt::OddEvenFill)) {",
    "            if (true) {")],
  [(STATIC, "theCellsAreGroupedIntoConductors")]),
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
                                           stderr=subprocess.STDOUT, timeout=600,
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
