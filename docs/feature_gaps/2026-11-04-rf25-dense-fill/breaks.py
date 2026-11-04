#!/usr/bin/env python3
"""breaks.py: RF 2.5D's dense fill shared among a frequency's threads
(rf25/rf25_fullwave.cpp's solveAt and Kernels, rf25/rf25_fast.cpp's
onThreads) broken on purpose, one break at a time - the Green's functions
made side by side, the system's and P's columns, the mirror, the charges,
the threads handed down and said, the counts - each built and its tests
run. Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks the
anchors (writing each break and putting it back); BRK_ONLY=3,7 runs those
alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (build)."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = W + "/qucs-s-26.1.1/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-dense-fill-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

F = "rf25/rf25_fullwave.cpp"
FA = "rf25/rf25_fast.cpp"
SHARED = ("test_rf25_fullwave", "aFrequencysFillIsTheSameOnAnyNumberOfThreads")
QS = ("test_rf25_fullwave", "atLowFrequencyItIsTheQuasiStaticExtraction")
JUNCTION = ("test_rf25_fullwave", "aJunctionIsReciprocalAndPassive")
FACES = ("test_rf25_fullwave", "aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent")
TWO = ("test_rf25_fullwave", "linesOnTwoMetalsAreSolvedTogether")
VIAG = ("test_rf25_fullwave", "aViaToTheGroundIsItsOwnInductance")
VIA2 = ("test_rf25_fullwave", "aViaBetweenTwoMetalsJoinsThem")
COMPRESSED = ("test_rf25_fullwave", "aCompressedSolveIsTheDenseOne")
HMUL = ("test_rf25_fast", "anHMatrixMultipliesAsItsDenseMatrixDoes")

MIRROR = ("    const int mBlocks = (nr + kMirrorBlock - 1) / kMirrorBlock, pBlocks = (nc + kMirrorBlock - 1) / kMirrorBlock;\n"
          "    sharedOut(mBlocks + pBlocks, threads, [&m, &p, mBlocks](int i) {\n"
          "        MatrixC& x = i < mBlocks ? m : p;\n"
          "        const int b0 = (i < mBlocks ? i : i - mBlocks) * kMirrorBlock;\n"
          "        mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));\n"
          "    });\n")
CHARGES = "    sharedOut(nr, threads, [&c, &p, &m, jw](int b) { chargeColumn(c, p, jw, m, b); });\n"
UPPER = ("        if (i < nr)\n"
         "            systemColumn(c, k, m, nr - 1 - i);\n"
         "        else\n"
         "            potentialColumn(c, k, p, nc - 1 - (i - nr));\n")
COMPRESSED_KERNELS = ("    AtFrequency out;\n    const Kernels k(c, f, threads);\n    if (!k.problems.isEmpty()) {\n"
                      "        out.problems << k.problems;\n        return out;\n    }\n    const Complex jw = k.jw;\n"
                      "    const int nr = c.nr, nc = c.nc, nv = int(c.vias.size()), columns = c.np;\n")

BREAKS = [
 # ---- the Green's functions side by side (Kernels)
 ("the Green's functions made on one thread",
  [(F, "    onThreads(std::clamp(threads, 1, std::max(1, count)), [this, &heightPairs,", "    onThreads(1, [this, &heightPairs,")],
  [SHARED]),
 ("every via's kernels made for the stack's first via",
  [(F, "v = ViaGreen(c.layers, c.spans.at(w - pairs), c.spans,", "v = ViaGreen(c.layers, c.spans.at(0), c.spans,")],
  [SHARED, FACES, VIAG, VIA2]),
 ("the pairs of heights' Green's functions taken the other way round",
  [(F, "        WaveGreen& g = waves[std::size_t(w)];\n        if (!g.ok()) {",
       "        WaveGreen& g = waves[std::size_t(pairs - 1 - w)];\n        if (!g.ok()) {")],
  [FACES, TWO, SHARED]),
 ("every Green's function that cannot be made said, not the first",
  [(F, "            problems << g.problems();\n            return;\n        }\n        const int i = heightPairs",
       "            problems << g.problems();\n            continue;\n        }\n        const int i = heightPairs")],
  [SHARED]),
 ("the compressed solve's Green's functions made on one thread",
  [(F, COMPRESSED_KERNELS, COMPRESSED_KERNELS.replace("const Kernels k(c, f, threads);", "const Kernels k(c, f);"))],
  [SHARED]),
 # ---- the columns shared out, and the threads handed down
 ("the system's and P's columns made on one thread",
  [(F, "    onThreads(std::clamp(threads, 1, std::max(1, count)), [&next, &job, count](int t) {",
       "    onThreads(1, [&next, &job, count](int t) {")],
  [SHARED]),
 ("a frequency's fill given one thread",
  [(F, "                                                : solveAt(c, frequencies.at(k), inner);",
       "                                                : solveAt(c, frequencies.at(k), 1);")],
  [SHARED]),
 ("the threads said as the frequencies at a time",
  [(F, "    out.threads = side * inner;\n", "    out.threads = compress ? side * inner : side;\n")],
  [SHARED]),
 ("onThreads running the caller's share alone",
  [(FA, "    for (int t = 1; t < threads; ++t) {\n        QThread* thread = QThread::create([&job, t] { job(t); });",
        "    for (int t = 1; t < 1; ++t) {\n        QThread* thread = QThread::create([&job, t] { job(t); });")],
  [SHARED, HMUL, COMPRESSED]),
 # ---- the columns themselves
 ("a run's own term with itself left unmade",
  [(F, "        for (int a = 0; a <= b; ++a) {\n            const double dot = k.cosine(a, b);",
       "        for (int a = 0; a < b; ++a) {\n            const double dot = k.cosine(a, b);")],
  [QS, COMPRESSED, SHARED]),
 ("a via's coupling to a run, the run's head and tail the other way round",
  [(F, "m(a, b) = k.viaPotential(j, c.head.at(a)) - k.viaPotential(j, c.tail.at(a));",
       "m(a, b) = k.viaPotential(j, c.tail.at(a)) - k.viaPotential(j, c.head.at(a));")],
  [VIAG, VIA2, FACES]),
 ("a via's own term with itself left unmade",
  [(F, "    for (int i = 0; i <= j; ++i) {\n        const Complex ij = onto(i, j);",
       "    for (int i = 0; i < j; ++i) {\n        const Complex ij = onto(i, j);")],
  [VIAG, VIA2, FACES]),
 ("two vias' coupling taken one way round, not the two averaged",
  [(F, "m(c.vias.at(i).run, b) = 0.5 * (ij + (i == j ? ij : onto(j, i)));", "m(c.vias.at(i).run, b) = ij;")],
  [VIAG, VIA2, FACES, COMPRESSED, SHARED]),
 ("the charges' column, its own tail and head the other way round",
  [(F, "    const int tail = c.tail.at(b), head = c.head.at(b);", "    const int tail = c.head.at(b), head = c.tail.at(b);")],
  [QS, JUNCTION]),
 # ---- the part below the diagonal
 ("the mirror's first row of each block left out",
  [(F, "    for (int a = b0 + 1; a < x.rows(); ++a)", "    for (int a = b0 + 2; a < x.rows(); ++a)")],
  [JUNCTION, QS]),
 ("each block's columns past its first left unmirrored",
  [(F, "mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));", "mirrorColumns(x, b0, std::min(int(x.cols()), b0 + 1));")],
  [JUNCTION, QS]),
 ("P left unmirrored",
  [(F, "    sharedOut(mBlocks + pBlocks, threads, [&m, &p, mBlocks](int i) {", "    sharedOut(mBlocks + 0 * pBlocks, threads, [&m, &p, mBlocks](int i) {")],
  [JUNCTION, QS]),
 ("the charges added before the matrices are mirrored",
  [(F, MIRROR + CHARGES, CHARGES + MIRROR)],
  [JUNCTION, QS]),
 ("each column mirrored as it is made, the others' it reads perhaps still being made",
  [(F, UPPER, "        if (i < nr) {\n"
              "            systemColumn(c, k, m, nr - 1 - i);\n"
              "            mirrorColumns(m, nr - 1 - i, nr - i);\n"
              "        } else {\n"
              "            potentialColumn(c, k, p, nc - 1 - (i - nr));\n"
              "            mirrorColumns(p, nc - 1 - (i - nr), nc - (i - nr));\n"
              "        }\n"),
   (F, "    sharedOut(mBlocks + pBlocks, threads, [&m, &p, mBlocks](int i) {", "    if (false) sharedOut(mBlocks + pBlocks, threads, [&m, &p, mBlocks](int i) {")],
  [SHARED]),
 # ---- the counts
 ("a Green's function made on another thread not counted",
  [(F, "            if (t > 0) ++sharedGreens;\n", "")],
  [SHARED]),
 ("a column made on another thread not counted",
  [(F, "            if (t > 0) ++sharedColumns;\n", "")],
  [SHARED]),
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
                    for k in ("QUCS_TEST_SLOW", "QUCS_RF25_LU", "QUCS_FEM_SOLVER"):
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
        print("%-66s %s" % (name[:66], said))
        if "BUILD FAILED" in said or "hung" in said or ("PASSED - NOT CAUGHT" in said and "caught)" not in said):
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":   # (read only, for BREAKS: nothing run)
    sys.exit(main())
