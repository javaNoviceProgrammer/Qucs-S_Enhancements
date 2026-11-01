#!/usr/bin/env python3
"""breaks.py: each piece of phase C's second round - the full-wave solve -
undone on purpose: the vector potential's image and its retardation, the
stack's remainders and how they are integrated over a cell, P made
symmetric, the charge's incidence, the sign of jw L, a run's resistance and
the sheet's two faces, S of Z, a port's shares, the Green's function of
each pair of heights, the current returned, what is refused - and phase B's
near cells judged by their area again. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=6,21 runs those alone; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/person/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/person/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-fullwave-breaks")
os.makedirs(RUNS, exist_ok=True)

F = "rf25/rf25_fullwave.cpp"
S = "rf25/rf25_static.cpp"
T = "test_rf25_fullwave"
SHEET = "theSheetAndTheScatteringMatrixAreTheirClosedForms"
LOW = "atLowFrequencyItIsTheQuasiStaticExtraction"
WAVE = "aLinesWaveIsItsOwnLineDispersed"
LOSS = "aSubstratesAndAMetalsLossesAreTheirs"
TWO = "linesOnTwoMetalsAreSolvedTogether"
JUNCTION = "aJunctionIsReciprocalAndPassive"
CURRENT = "theCurrentIsWhatChargesTheLineBeyond"
REFUSE = "whatItCannotSolveIsSaid"

BREAKS = [
 ("the vector potential's image in the ground left out",
  [(F, "                    l = 2.0 * direct.coefficient / kMu0 * lDirect(a, b) + 2.0 * image.coefficient / kMu0 * lImage(a, b);",
    "                    l = 2.0 * direct.coefficient / kMu0 * lDirect(a, b);")],
  [(T, LOW), (T, WAVE)]),

 ("the image's partial inductances taken at the runs themselves",
  [(F, "        mirrored[a].z0 = mirrored[a].z1 = -runs.at(a).z0;", "        mirrored[a].z0 = mirrored[a].z1 = runs.at(a).z0;")],
  [(T, LOW)]),

 ("the scalar potential's stack remainder left out",
  [(F, "            return v + (centred ? g.scalarRemainder(rho) : remainderOver(g, rect.at(j), centre.at(i), scale));",
    "            return v;")],
  [(T, LOW)]),

 ("the remainder at the charged cell's centre wherever it is not the cell's own",
  [(F, "            const bool centred = i != j && longest * longest < kCentreBound * scale * scale;",
    "            const bool centred = i != j;")],
  [(T, LOW)]),

 ("Gauss's rule of one point, the centre, for every cell",
  [(F, "    return rules.at(std::clamp(n, 1, kMostGauss));", "    return rules.at(1);")],
  [(T, LOW)]),

 ("P left as it is, not made symmetric",
  [(F, "                const Complex v = i == j ? coefficient(i, i, g) : 0.5 * (coefficient(i, j, g) + coefficient(j, i, g));\n"
       "                p(i, j) = p(j, i) = v;",
    "                p(i, j) = coefficient(i, j, g);\n                p(j, i) = coefficient(j, i, g);")],
  # (Not the line's test: a line is the same from either end, and reciprocal
  # by that whatever P is.)
  [(T, JUNCTION)]),

 ("the charge's incidence: a run's two ends taken alike",
  [(F, "                m(a, b) += (p(tail.at(a), tail.at(b)) - p(tail.at(a), head.at(b)) - p(head.at(a), tail.at(b))",
    "                m(a, b) += (p(tail.at(a), tail.at(b)) + p(tail.at(a), head.at(b)) + p(head.at(a), tail.at(b))")],
  [(T, LOW)]),

 ("jw L with the wrong sign",
  [(F, "                m(a, b) = m(b, a) = jw * l;", "                m(a, b) = m(b, a) = -jw * l;")],
  [(T, LOW), (T, WAVE)]),

 ("the scalar term's retardation left out",
  [(F, "            const Complex v = t.coefficient / (2 * M_PI) * (pStatic(i, j) + retardation(t.k, std::hypot(rho, t.dz)));",
    "            const Complex v = t.coefficient / (2 * M_PI) * pStatic(i, j);")],
  [(T, WAVE), (T, JUNCTION)]),

 ("the vector terms' retardation left out",
  [(F, "                    const Complex smooth = direct.coefficient / (2 * M_PI) * retardation(direct.k, std::hypot(rho, direct.dz))\n"
       "                                           + image.coefficient / (2 * M_PI) * retardation(image.k, std::hypot(rho, image.dz))\n"
       "                                           + g.vectorRemainder(rho);",
    "                    const Complex smooth = g.vectorRemainder(rho);")],
  [(T, WAVE), (T, JUNCTION)]),

 ("the vector potential's stack remainder left out",
  [(F, "                                           + g.vectorRemainder(rho);", "                                           + 0.0;")],
  [(T, WAVE)]),

 ("a run's resistance across it, not along",
  [(F, "            if (run.width > 0) m(a, a) += sheetImpedance(run.sigma, run.thickness, f) * run.length() / run.width;",
    "            if (run.width > 0) m(a, a) += sheetImpedance(run.sigma, run.thickness, f) * run.width / run.length();")],
  [(T, LOSS)]),

 ("the sheet's current on one face only",
  [(F, "    return 0.5 * surfaceImpedance(sigma, thickness / 2, frequency);", "    return surfaceImpedance(sigma, thickness, frequency);")],
  [(T, SHEET)]),

 ("S as (Z + Z0)(Z - Z0)^-1",
  [(F, "    const MatrixC s = Eigen::PartialPivLU<MatrixC>(b).solve(a);", "    const MatrixC s = Eigen::PartialPivLU<MatrixC>(a).solve(b);")],
  [(T, SHEET)]),

 ("a port's shares of its face left in micrometres, not summing to one",
  [(F, "    for (double& l : lengths) l /= sum;\n", "")],
  [(T, LOW)]),

 ("the first pair of heights' Green's function for every pair of cells",
  [(F, "                const WaveGreen& g = greens.at(pairOf.at(hNode.at(i) * nh + hNode.at(j)));",
    "                const WaveGreen& g = greens.first();")],
  [(T, TWO)]),

 ("the current of no frequency kept",
  [(F, "        if (fi + 1 == frequencies.size()) {", "        if (fi < 0) {")],
  [(T, CURRENT)]),

 ("a via let through",
  [(F, "        if (f.z0 != f.z1) {", "        if (false) {")],
  [(T, REFUSE)]),

 ("a stack with no ground solved",
  [(F, "    if (!layers.groundBelow) {", "    if (false) {")],
  [(T, REFUSE)]),

 ("two ports on the same face let through",
  [(F, "            if (other == sorted) {", "            if (false) {")],
  [(T, REFUSE)]),

 ("a frequency of nought or less let through",
  [(F, "        if (!(f > 0) || !std::isfinite(f)) {", "        if (!std::isfinite(f)) {")],
  [(T, REFUSE)]),

 ("phase B's near cells judged by their area again",
  [(S, "            const double side = longestSide(ci, cj) * unit;",
    "            const double side = std::max(std::sqrt(ci.area), std::sqrt(cj.area)) * unit;")],
  [(T, LOW)]),
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
