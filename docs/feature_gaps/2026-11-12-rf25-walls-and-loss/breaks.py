#!/usr/bin/env python3
"""breaks.py: the walls' current, charge and loss of RF 2.5D's two-face
metal (12 November, phase 2 of its accuracy limits) undone on purpose, one
piece at a time - each built and its tests run. Sources are put back whatever
happens, and the suites built again after. BRK_ONLY_CHECK=1 checks the
anchors and writes nothing; BRK_ONLY=3 runs that alone; HUNT_RUNS=<dir> for
logs; BRK_BUILD=<dir> the build (the checkout's build-rel). Run as a script
only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-walls-and-loss-breaks")
GUILESS = {"test_rf25_accuracy", "test_rf25_edge"}   # (no -platform for these)

FW = "rf25/rf25_fullwave.cpp"
ST = "rf25/rf25_static.cpp"
IN = "rf25/rf25_inductance.cpp"
ED = "rf25/rf25_edge.cpp"
PERFECT = ("test_rf25_accuracy", "aPerfectMetalsInductanceIsTheBoundaryElementSolves")
LINES = ("test_rf25_accuracy", "theFiveLinesAreTheir2DSolves")
CPW = ("test_rf25_accuracy", "theBenchmarksCoplanarLineIsPalacesAndQfems")
FACES = ("test_rf25_fullwave", "aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent")
COMB = ("test_rf25_fullwave", "theSideWallsChargeIsInAThickCombsCapacitance")
COMBQ = ("test_rf25_fullwave", "aThickCombsQuasiStaticSolveIsItsFullWave")
UPRIGHT = ("test_rf25_inductance", "anUprightStripBesideAFlatOneIsTheirDoubleIntegral")
DENSE = ("test_rf25_fullwave", "aCompressedSolveIsTheDenseOne")
EDGE = ("test_rf25_edge", "athighFrequencyTheEdgeCrowds")
EDGELOW = ("test_rf25_edge", "atALowFrequencyTheCurrentIsEven")

BREAKS = [
 ("no current down the walls",
  [(FW, "    wallsOf(c);\n", "")], [PERFECT, CPW]),
 ("no charge on the walls in the full wave",
  [(FW, "        for (int i = 0; i < nc; ++i) c.charges[i] = walls.value(cellOf.at(i));\n", "")], [CPW, COMB]),
 ("no charge on the walls in the quasi-static solve",
  [(ST, "                                   wallCharges(thick, micrometre, [&thick](int m) { return heightOf(thick.stack, thick.mesh.cells.at(m).layer); }));",
        "                                   {});")], [COMBQ]),
 ("charge on every wall, facing a gap or not",
  [(ED, "    if (facing.isEmpty()) return out;\n", "    for (int m = 0; m < thick.mesh.cells.size(); ++m) facing[m] |= 15;\n")], [FACES]),
 ("the edge band's loss not taken",
  [(FW, "                    if (r > 0) self = Complex(r, self.imag());\n", "")], [LINES]),
 ("an edge run's loss crowding()'s, its walls counted twice",
  [(FW, "        if (!parts.isEmpty()) {\n            // A run along an edge that carries current down its wall too",
        "        if ((false)) {\n            // A run along an edge that carries current down its wall too")], [LINES]),
 ("an upright strip's pair by the averaged distance",
  [(IN, "    if (a.rise > 0 || b.rise > 0) {", "    if ((false)) {")], [UPRIGHT]),
 ("the mean of ln r taken whole, its sign lost",
  [(IN, "    return (F(x0) - F(x1)) / (a1 - a0);", "    return std::abs(F(x0) - F(x1)) / std::abs(a1 - a0);")], [UPRIGHT]),
 ("a short upright pair by its geometric mean distance alone",
  [(IN, "        return sign * kMu0 / (4 * M_PI) * (smooth - K * meanLogBetween(sa, sb));",
        "        return sign * kMu0 / (4 * M_PI) * parallelPair(la, lb, d, std::exp(meanLogBetween(sa, sb)));")], [UPRIGHT]),
 ("the edge solved without its mirror at mid-thickness",
  [(ED, "            const double lnr = meanLog(a, b) + meanLog(a, mirroredX(b)) + meanLog(a, mirroredY(b)) + meanLog(a, mirroredX(mirroredY(b)));",
        "            const double lnr = meanLog(a, b) + meanLog(a, mirroredX(b));")], [EDGE, LINES]),
 ("the compressed solve's inductance without the walls",
  [(FW, "    double direct = 0, image = 0;\n    staticPair(c, a, b, &direct, &image);\n    return inductance(a, b, direct, image);",
        "    return inductance(a, b, alignedInductance(c.runs.at(a), c.runs.at(b)), alignedInductance(c.runs.at(a), c.mirrored.at(b)));")], [DENSE, COMB]),
 ("the compressed solve's potential without the walls' charge",
  [(FW, "    return potential(i, j, staticPotential(c, i, j), i == j ? 0.0 : staticPotential(c, j, i));",
        "    const double dz = std::abs(c.cellZ.at(i) - c.cellZ.at(j));\n    return potential(i, j, rectanglePotential(c.rect.at(j), c.centre.at(i), dz), i == j ? 0.0 : rectanglePotential(c.rect.at(i), c.centre.at(j), dz));")], [COMB, COMBQ, DENSE]),
]


def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is None:
            continue
        if p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))


def main():
    os.makedirs(os.path.join(RUNS, "cache"), exist_ok=True)
    if os.environ.get("BRK_ONLY_CHECK"):
        for name, edits, tests in BREAKS:
            apply(edits, None)
        print("%d breaks, every anchor once" % len(BREAKS))
        return 0
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        if which and i not in which:
            continue
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            log = os.path.join(RUNS, "brk%03d.log" % i)
            suites = sorted({t[0] for t in tests})
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-j12", "-C", B] + suites, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    home = os.path.join(RUNS, "home")
                    os.makedirs(home, exist_ok=True)
                    env = dict(os.environ, HOME=home, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
                    try:
                        args = [os.path.join(B, "qucs/tests", suite), fn] + ([] if suite in GUILESS else ["-platform", "offscreen"])
                        r = subprocess.run(args, stdout=out,
                                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=1200, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    # The suites built again from the sources as they are: a break's build
    # left behind fails a later run for nothing.
    subprocess.run(["ninja", "-j12", "-C", B] + sorted({t[0] for _, _, tests in BREAKS for t in tests}),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-62s %s" % (name[:62], said))
        if "BUILD FAILED" in said or "hung" in said or "caught)" not in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
