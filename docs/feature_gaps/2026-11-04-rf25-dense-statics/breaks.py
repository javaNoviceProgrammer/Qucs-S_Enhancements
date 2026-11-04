#!/usr/bin/env python3
"""breaks.py: the dense solve's static parts shared out among a solve's
threads (rf25/rf25_fullwave.cpp's makeStatics(), its columns, sharedOut's
count) broken on purpose, one break at a time - the threads they are made
on, what is counted and where, the inductances' and the potentials'
columns - each built and its tests run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors (writing each break and
putting it back); BRK_ONLY=3,7 runs those alone; HUNT_RUNS=<dir> for logs;
BRK_BUILD=<dir> the build (build)."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = W + "/qucs-s-26.1.1/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-dense-statics-breaks")
CACHE = os.path.join(RUNS, "cache")

F = "rf25/rf25_fullwave.cpp"
STATICS = ("test_rf25_fullwave", "theStaticPartsAreTheSameOnAnyNumberOfThreads")
SHARED = ("test_rf25_fullwave", "aFrequencysFillIsTheSameOnAnyNumberOfThreads")
QS = ("test_rf25_fullwave", "atLowFrequencyItIsTheQuasiStaticExtraction")
JUNCTION = ("test_rf25_fullwave", "aJunctionIsReciprocalAndPassive")
FACES = ("test_rf25_fullwave", "aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent")
TWO = ("test_rf25_fullwave", "linesOnTwoMetalsAreSolvedTogether")
VIAG = ("test_rf25_fullwave", "aViaToTheGroundIsItsOwnInductance")
VIA2 = ("test_rf25_fullwave", "aViaBetweenTwoMetalsJoinsThem")
COMPRESSED = ("test_rf25_fullwave", "aCompressedSolveIsTheDenseOne")

CALL = "    if (!compress) {\n        makeStatics(c, threads);\n"
COLUMNS_COUNTED = "            staticInductanceColumn(c, c.lDirect, c.lImage, nr - 1 - (i - nc));\n    }, sharedStatics);\n"
MIRROR_COUNTED = "        mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));\n    }, sharedStatics);\n"
CROSSED = "const bool crossed = std::abs(c.dirX.at(a) * c.dirX.at(b) + c.dirY.at(a) * c.dirY.at(b)) < 1e-9;"
DIRECT = "        direct(a, b) = crossed ? 0.0 : alignedInductance(c.runs.at(a), c.runs.at(b));\n"
IMAGE = "        image(a, b) = crossed ? 0.0 : alignedInductance(c.runs.at(a), c.mirrored.at(b));\n"
POTENTIAL = "        p(i, j) = rectanglePotential(c.rect.at(j), c.centre.at(i), std::abs(c.cellZ.at(i) - c.cellZ.at(j)));\n"

BREAKS = [
 # ---- the threads they are made on, and what is counted
 ("the static parts made on one thread",
  [(F, CALL, CALL.replace("makeStatics(c, threads);", "makeStatics(c, 1);"))],
  [STATICS]),
 ("the static parts made on two threads where one is asked",
  [(F, CALL, CALL.replace("makeStatics(c, threads);", "makeStatics(c, std::max(2, threads));"))],
  [STATICS]),
 ("the static parts made for a compressed solve too",
  [(F, CALL, "    makeStatics(c, threads);\n    if (!compress) {\n")],
  [STATICS]),
 ("the static parts counted as a frequency's columns",
  [(F, COLUMNS_COUNTED, COLUMNS_COUNTED.replace("}, sharedStatics);", "});")),
   (F, MIRROR_COUNTED, MIRROR_COUNTED.replace("}, sharedStatics);", "});"))],
  [STATICS]),
 ("sharedOut counting every job as a frequency's column, whatever it is given",
  [(F, "            if (t > 0) ++counted;\n", "            if (t > 0) ++sharedColumns;\n")],
  [STATICS]),
 ("sharedOut counting no job",
  [(F, "            if (t > 0) ++counted;\n", "")],
  [STATICS, SHARED]),
 # ---- the inductances' columns
 ("a run's own static inductance with itself left unmade",
  [(F, "    for (int a = 0; a <= b; ++a) {\n        const bool crossed", "    for (int a = 0; a < b; ++a) {\n        const bool crossed")],
  [QS, COMPRESSED, STATICS]),
 ("the inductances' columns made below the diagonal, where the mirror writes",
  [(F, "    for (int a = 0; a <= b; ++a) {\n        const bool crossed", "    for (int a = b; a < c.nr; ++a) {\n        const bool crossed")],
  [QS, COMPRESSED, STATICS]),
 ("parallel runs' static inductance left nought, and those at right angles made",
  [(F, CROSSED, CROSSED.replace("c.dirX.at(a) * c.dirX.at(b) + c.dirY.at(a) * c.dirY.at(b)",
                                "c.dirX.at(a) * c.dirY.at(b) + c.dirY.at(a) * c.dirX.at(b)"))],
  [QS, COMPRESSED]),
 ("the direct static inductance the image's",
  [(F, DIRECT, DIRECT.replace("c.runs.at(b)", "c.mirrored.at(b)"))],
  [QS, COMPRESSED]),
 ("the image's static inductance the run's own",
  [(F, IMAGE, IMAGE.replace("c.mirrored.at(b)", "c.runs.at(b)"))],
  [QS, VIAG, COMPRESSED]),
 # ---- the potentials' columns
 ("the static potentials of two cells taken at one height",
  [(F, POTENTIAL, POTENTIAL.replace("c.cellZ.at(i) - c.cellZ.at(j)", "c.cellZ.at(i) - c.cellZ.at(i)"))],
  [TWO, FACES, VIA2]),
 ("the static potentials' columns cut at their diagonal",
  [(F, "    for (int i = 0; i < c.nc; ++i)\n" + POTENTIAL, "    for (int i = 0; i <= j; ++i)\n" + POTENTIAL)],
  [QS, JUNCTION, STATICS]),
]


def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))


def main():
    os.makedirs(CACHE, exist_ok=True)
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
            suites = sorted({t[0] for t in tests})
            targets = ["qucs/tests/" + s for s in suites]
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + targets, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=CACHE, ASAN_OPTIONS="detect_leaks=0",
                               QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh", QTEST_FUNCTION_TIMEOUT="900000")
                    for k in ("QUCS_TEST_SLOW", "QUCS_TEST_PART", "QUCS_RF25_LU", "QUCS_FEM_SOLVER"):
                        env.pop(k, None)
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=900, env=env, cwd=RUNS)
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
        if "BUILD FAILED" in said or "hung" in said or ("PASSED - NOT CAUGHT" in said and "caught)" not in said):
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":   # (read only, for BREAKS: nothing run)
    sys.exit(main())
