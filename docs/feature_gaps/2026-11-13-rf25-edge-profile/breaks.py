#!/usr/bin/env python3
"""breaks.py: RF 2.5D's edge-singular charge and current (13 November, phase 5
of its accuracy limits) undone on purpose, one piece at a time - each built
and its tests run. Sources are put back whatever happens, and the suites
built again after. BRK_ONLY_CHECK=1 checks the anchors and writes nothing;
BRK_ONLY=3 runs that alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the
build (the checkout's build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-edge-profile-breaks")
GUILESS = {"test_rf25_accuracy", "test_rf25_edge", "test_rf25_returns"}   # (no -platform for these)

ED = "rf25/rf25_edge.cpp"
ST = "rf25/rf25_static.cpp"
FW = "rf25/rf25_fullwave.cpp"
THIN = ("test_rf25_accuracy", "aThinCoplanarLinesZ0ConvergesOnPalaces")
PLATE = ("test_rf25_accuracy", "aSquarePlatesChargeIsNearerItsLimitWithItsEdgesProfile")
CPW = ("test_rf25_accuracy", "theBenchmarksCoplanarLineIsPalacesAndQfems")
FIVE = ("test_rf25_accuracy", "theFiveLinesAreTheir2DSolves")
PERFECT = ("test_rf25_accuracy", "aPerfectMetalsInductanceIsTheBoundaryElementSolves")
PORTS = ("test_rf25_fullwave", "itsPortsAreTheModelsOwn")
FACES = ("test_rf25_fullwave", "aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent")
PLATES = ("test_rf25_static", "twoPlatesFacingEachOtherAreAParallelPlateCapacitor")
TIED = ("test_rf25_returns", "whatAPortThatNamesItsReturnsCannotBeIsSaid")
ENDED = ("test_rf25_doc", "aSubstrateThatEndsIsSolvedByBothSolvers")

BREAKS = [
 ("no charge profile at the edges",
  [(ED, "        out.insert(m, profiled(parts, it.value(), 0.5));", "        out.insert(m, parts);")], [THIN, PLATE]),
 ("the charge's profile as d^-1/3",
  [(ED, "        out.insert(m, profiled(parts, it.value(), 0.5));", "        out.insert(m, profiled(parts, it.value(), 1.0 / 3));")], [THIN, PLATE]),
 ("a corner's charge d^-1/2 each way",
  [(ED, "    if ((lowY || highY) && (lowX || highX)) alpha = std::min(alpha, 0.35);\n", "")], [PLATE]),
 ("even cuts, not graded toward the edge",
  [(ED, "        for (int k = 0; k <= kProfileStrips; ++k) cuts << w * double(k * k) / (kProfileStrips * kProfileStrips);",
        "        for (int k = 0; k <= kProfileStrips; ++k) cuts << w * double(k) / kProfileStrips;")], [THIN, PLATE]),
 ("a profile beside facing plates",
  [(ED, "        if (facedNear(m)) continue;\n", "")], [PLATES]),
 ("the metal's own other face taken as facing",
  [(ED, "            if (c.layer == cell.layer || c.layer == own) continue;", "            if (c.layer == cell.layer) continue;")], [FACES, CPW]),
 ("the quasi-static's coincident image left even",
  [(ST, "        const bool coincident = std::abs(std::abs(t.z) - dz) <= 1e-9 * std::max(dz, 1e-9);\n        if (!coincident) continue;",
        "        const bool coincident = std::abs(std::abs(t.z) - dz) <= 1e-9 * std::max(dz, 1e-9);\n        if (!coincident || direct) continue;")], [PORTS, CPW]),
 ("no current profile on the runs",
  [(FW, "        const auto pieces = edgeProfiles() ? edgeProfile(run.width, mask & 1, mask & 2, faces ? 1.0 / 3 : 0.5)",
        "        const auto pieces = false ? edgeProfile(run.width, mask & 1, mask & 2, faces ? 1.0 / 3 : 0.5)")], [CPW, PERFECT]),
 ("a face's current d^-1/2",
  [(FW, "edgeProfile(run.width, mask & 1, mask & 2, faces ? 1.0 / 3 : 0.5)", "edgeProfile(run.width, mask & 1, mask & 2, 0.5)")], [FIVE]),
 ("an ended layer's profile of the background, quasi-static",
  [(ST, "                                         withDielectric && !insideOf.isEmpty() ? localCorrection",
        "                                         false ? localCorrection")], [ENDED]),
 ("an ended layer's profile of the background, full wave",
  [(FW, "        if (any) c.profileScale = scale;", "        (void)any;")], [ENDED]),
 ("only a profiled cell scaled where a layer ends",
  [(FW, "            // (Every cell's, a profiled cell's pair with one that is not\n            //  scaled by both: Context::profileScale.)\n",
        "            if (c.charges.at(i).isEmpty()) continue;\n")], [ENDED]),
 ("two returns alike tied by LU alone",
  [(FW, "            if (!z.allFinite() || !x.allFinite()) {\n                Eigen::CompleteOrthogonalDecomposition",
        "            if ((false)) {\n                Eigen::CompleteOrthogonalDecomposition")], [TIED]),
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
