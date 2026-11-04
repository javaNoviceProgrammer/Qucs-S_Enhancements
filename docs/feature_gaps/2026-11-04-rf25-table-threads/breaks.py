#!/usr/bin/env python3
"""breaks.py: a Green's function's table made on several threads
(rf25/rf25_wave.cpp's nodesOf, tableOf and prepare(threads), rf25/
rf25_fullwave.cpp's Kernels) broken on purpose, one break at a time - the
distances shared, each into its own place, the kept table grown, its lock
and its bytes, the counts, and the frequency's Green's functions made one
after another - each built and its tests run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors (writing each break and putting
it back); BRK_ONLY=3,7 runs those alone; HUNT_RUNS=<dir> for logs;
BRK_BUILD=<dir> the build (build)."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = W + "/qucs-s-26.1.1/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-table-threads-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

WV = "rf25/rf25_wave.cpp"
F = "rf25/rf25_fullwave.cpp"
TABLES = ("test_rf25_wave", "aTableIsTheSameOnAnyNumberOfThreads")
KEPT = ("test_rf25_wave", "aKeptTableIsTheOneItsKernelWouldMake")
INTEGRAL = ("test_rf25_wave", "theTableIsTheIntegral")
VIATABLE = ("test_rf25_wave", "whatAViasClosedFormsLeaveIsSmallAndTabulated")
SHARED = ("test_rf25_fullwave", "aFrequencysFillIsTheSameOnAnyNumberOfThreads")
BEFORE = ("test_rf25_fullwave", "aSolveReadsTheSameWhateverWasSolvedBefore")
FACES = ("test_rf25_fullwave", "aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent")
TWO = ("test_rf25_fullwave", "linesOnTwoMetalsAreSolvedTogether")
VIAG = ("test_rf25_fullwave", "aViaToTheGroundIsItsOwnInductance")
VIA2 = ("test_rf25_fullwave", "aViaBetweenTwoMetalsJoinsThem")

NODE = "            node(at.at(from + i), out + qsizetype(i) * width);\n"
WAVE_RETURN = ("    for (int i = 0; i < a_rho.size(); ++i) a_table << Pair{values.at(2 * i), values.at(2 * i + 1)};\n"
               "    return elsewhere;\n")
VIA_RETURN = "    a_table = table;\n    a_rho = rho;\n    return elsewhere;\n"

BREAKS = [
 # ---- the distances shared among the threads (nodesOf)
 ("a table's distances made on one thread",
  [(WV, "    onThreads(std::clamp(threads, 1, count), [&at, &node,", "    onThreads(1, [&at, &node,")],
  [TABLES, SHARED]),
 ("keeping nothing, a table's distances made on one thread",
  [(WV, "nodesOf(wanted, 0, width, threads, node, made.data());", "nodesOf(wanted, 0, width, 1, node, made.data());")],
  [TABLES]),
 ("a kept table's distances made on one thread",
  [(WV, "elsewhere = nodesOf(wanted, had, width, threads, node,", "elsewhere = nodesOf(wanted, had, width, 1, node,")],
  [TABLES, SHARED]),
 ("each distance made at the next one's",
  [(WV, NODE, "            node(at.at(from + std::min(i + 1, count - 1)), out + qsizetype(i) * width);\n")],
  [INTEGRAL, KEPT, VIATABLE]),
 ("the nearest distance to make left unmade",
  [(WV, "        for (int k = next++; k < count; k = next++) {\n", "        for (int k = next++; k < count - 1; k = next++) {\n")],
  [INTEGRAL, KEPT, TABLES]),
 # ---- the kept table grown
 ("a kept table grown over its kept distances",
  [(WV, "node, kept->values.data() + qsizetype(had) * width);", "node, kept->values.data());")],
  [KEPT, TABLES, BEFORE]),
 ("the kernel's lock let go while its distances are made",
  [(WV, "    QMutexLocker growing(&kept->lock);\n", "    QMutexLocker growing(&kept->lock);\n    growing.unlock();\n")],
  [TABLES]),
 ("the store's bytes of the table before it grew",
  [(WV, "    const double now = 8.0 * kept->rho.size() + 16.0 * kept->values.size();\n",
        "    const double now = 8.0 * had + 16.0 * had * width;\n")],
  [KEPT]),
 # ---- the counts
 ("what other threads made not returned (the wave kernel's)",
  [(WV, WAVE_RETURN, WAVE_RETURN.replace("return elsewhere;", "return 0;"))],
  [TABLES]),
 ("what other threads made not returned (a via's)",
  [(WV, VIA_RETURN, VIA_RETURN.replace("return elsewhere;", "return 0;"))],
  [TABLES]),
 ("the caller's own distances counted as made elsewhere",
  [(WV, "            if (t > 0) ++elsewhere;\n", "            ++elsewhere;\n")],
  [TABLES, SHARED]),
 # ---- the frequency's Green's functions (Kernels)
 ("a frequency's tables made on one thread",
  [(F, "            sharedGreens += g.prepare(threads);\n", "            sharedGreens += g.prepare();\n"),
   (F, "        sharedGreens += v.prepare(threads);\n", "        sharedGreens += v.prepare();\n")],
  [SHARED]),
 ("every via's kernels made for the stack's first via",
  [(F, "        ViaGreen v(c.layers, s, c.spans, c.heights, f, c.rhoMax);\n",
       "        ViaGreen v(c.layers, c.spans.at(0), c.spans, c.heights, f, c.rhoMax);\n")],
  [VIA2, FACES, VIAG]),
 ("the pairs of heights' Green's functions read the other way round",
  [(F, "            pairOf[i * nh + j] = pairOf[j * nh + i] = int(greens.size());\n",
       "            pairOf[i * nh + j] = pairOf[j * nh + i] = nh * (nh + 1) / 2 - 1 - int(greens.size());\n")],
  [TWO, FACES, SHARED]),
 ("every Green's function that cannot be made said, not the first",
  [(F, "                problems << g.problems();\n                return;\n",
       "                problems << g.problems();\n                continue;\n")],
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
                    for k in ("QUCS_TEST_SLOW", "QUCS_RF25_LU", "QUCS_FEM_SOLVER", "QUCS_TEST_PART"):
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
