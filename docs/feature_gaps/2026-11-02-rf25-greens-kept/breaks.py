#!/usr/bin/env python3
"""breaks.py: the Green's functions' tables kept between solves, undone on
purpose one piece at a time - the grid that is one sequence for every
extent, what a table is kept by, what of a kept table a kernel is handed,
keeping nothing when asked, the counts, the budget and what it forgets, and
a kernel read from many threads at once - each built and its tests run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-greens-kept-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

W = "rf25/rf25_wave.cpp"
TW, TF = "test_rf25_wave", "test_rf25_fullwave"
KEPT = "aKeptTableIsTheOneItsKernelWouldMake"
SOLVE = "aSolveReadsTheSameWhateverWasSolvedBefore"
THREADS = "aKernelReadFromManyThreadsReadsAsFromOne"

BREAKS = [
 # ---- the grid
 ("a wave kernel's grid from how far it is wanted again",
  [(W, "    double low = 1e-3 / a_kMax;\n    if (a_reach > 0) low = std::min(low, 0.01 * a_reach);\n    const double most = 2 * M_PI / (60 * a_kMax);\n    KeyWriter key('W'",
    "    double low = std::min(1e-3 * a_rhoMax, 1e-3 / a_kMax);\n    if (a_reach > 0) low = std::min(low, 0.01 * a_reach);\n    const double most = 2 * M_PI / (60 * a_kMax);\n    KeyWriter key('W'")],
  [(TW, KEPT), (TF, SOLVE)]),
 # ---- what a table is kept by
 ("kept by the stack alone, not the frequency or the grid",
  [(W, "for (const double v : {layers.epsrBelow, layers.epsrAbove, frequency, low, most}) add(v);",
    "for (const double v : {layers.epsrBelow, layers.epsrAbove}) add(v);")],
  [(TW, KEPT), (TF, SOLVE)]),
 ("a wave kernel kept without its heights",
  [(W, "    key.add(a_zField);\n    key.add(a_zSource);\n", "")],
  [(TW, KEPT)]),
 ("a via's kernel kept without the vias it couples to",
  [(W, "    for (const QList<Segment>& other : std::as_const(a_others)) parts(other);\n", "")],
  [(TW, KEPT)]),
 # ---- what is handed over
 ("the whole kept table handed over, not the kernel's own part of it",
  [(W, "    *rho = kept->rho.mid(0, n);\n    *values = kept->values.mid(0, n * width);",
    "    *rho = kept->rho;\n    *values = kept->values;")],
  [(TW, KEPT)]),
 ("kept though nothing is to be",
  [(W, "        if (s.budget > 0) {\n            kept = s.kept.value(key);", "        if (true) {\n            kept = s.kept.value(key);")],
  [(TW, KEPT), (TF, SOLVE)]),
 # ---- the counts
 ("what was built and reused not counted",
  [(W, "    s.built += std::max(0, n - had);\n    s.reused += std::min(had, n);\n", "")],
  [(TW, KEPT), (TF, SOLVE)]),
 # ---- the budget
 ("past the budget nothing forgotten",
  [(W, "    while (s.bytes > s.budget || (s.budget <= 0 && !s.kept.isEmpty())) {",
    "    while (false) {")],
  [(TW, KEPT)]),
 ("past the budget the most lately used forgotten",
  [(W, "(oldest == s.kept.end() || it.value()->used < oldest.value()->used)",
    "(oldest == s.kept.end() || it.value()->used > oldest.value()->used)")],
  [(TW, KEPT)]),
 # ---- reading a kernel from many threads
 ("a wave kernel read through its mutable lists, which a reader detaches",
  [(W, "    const QList<double>& nodes = a_rho;\n    const QList<Pair>& table = a_table;",
    "    QList<double>& nodes = a_rho;\n    QList<Pair>& table = a_table;")],
  [(TW, THREADS)]),
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
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=CACHE,
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
