#!/usr/bin/env python3
"""breaks.py: RF 2.5D's dense LU on Accelerate (rf25/rf25_dense.cpp) broken on
purpose, one break at a time - the blocked LU's pivots, swaps, U12 and
trailing update, the 1-norm rcond reads, a solve's padding, the real solve,
the choice between Accelerate and Eigen and what overrides it, the build's
define, the counts, the full wave's seconds and whose LU it says, the static
solve's nets and its hint, and rf25_solve's answer - each built and its
tests run. Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks
the anchors (writing each break and putting it back); BRK_ONLY=3,7 runs
those alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (build)."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = W + "/qucs-s-26.1.1/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-dense-lu-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

D = "rf25/rf25_dense.cpp"
F = "rf25/rf25_fullwave.cpp"
ST = "rf25/rf25_static.cpp"
R = "qucscontrol_rf25.cpp"
C = "CMakeLists.txt"
MAC = ("test_rf25_dense", "macOSFactorizesWithAccelerate")
RAND = ("test_rf25_dense", "aRandomMatrixIsSolvedAlike")
MOM = ("test_rf25_dense", "aMomentMatrixIsSolvedAlike")
RHS = ("test_rf25_dense", "anyNumberOfRightHandSidesIsSolved")
ENV = ("test_rf25_dense", "theEnvironmentChoosesOverTheProcess")
FULL = ("test_rf25_fullwave", "theSystemIsFactorizedAlikeEitherWay")
STAT = ("test_rf25_static", "theChargesAreSolvedAlikeEitherWay")
COUPLE = ("test_rf25_static", "twoLinesCoupleAndTheCouplingFallsWithTheGap")
TOOL = ("test_qucs_control", "anRf25ModelIsSolvedFullWaveThroughItsTool")

BREAKS = [
 # ---- the blocked LU
 ("the panel's pivots left counted from its own first row",
  [(D, "        for (LapackInt i = k; i < k + width; ++i) pivots[i] += k;\n", "")],
  [RAND, RHS]),
 ("the panel's swaps not made to the columns before it",
  [(D, "        if (k > 0) zlaswp_(&k, a, &n, &first, &last, pivots, &unit);\n", "")],
  [RAND, MOM]),
 ("the panel's swaps not made to the columns after it",
  [(D, "            zlaswp_(&right, a + std::size_t(k + width) * std::size_t(n), &n, &first, &last, pivots, &unit);\n", "")],
  [RAND]),
 ("U12 not solved for (no ztrsm)",
  [(D, "            ztrsm_(\"L\", \"L\", \"N\", \"U\", &width, &right, &one, panel, &n, a12, &n);\n", "")],
  [RAND, MOM]),
 ("the trailing matrix not updated (no zgemm)",
  [(D, "            zgemm_(\"N\", \"N\", &right, &right, &width, &minusOne, panel + width, &n, a12, &n, &one, a12 + width, &n);\n", "")],
  [RAND, MOM]),
 ("the trailing update added, not taken away",
  [(D, "&right, &right, &width, &minusOne, panel + width", "&right, &right, &width, &one, panel + width")],
  [RAND]),
 # ---- what rcond and a solve read
 ("the 1-norm taken of the factors, not of the matrix",
  [(D, "                d->norm = d->lu.cwiseAbs().colwise().sum().maxCoeff();\n                d->info = factorize(n, d->lu.data(), d->pivots.data());\n",
       "                d->info = factorize(n, d->lu.data(), d->pivots.data());\n                d->norm = d->lu.cwiseAbs().colwise().sum().maxCoeff();\n")],
  [RAND, MOM]),
 ("a solve's padding handed back with it (eight columns for two)",
  [(D, "        if (x.cols() != columns) x.conservativeResize(Eigen::NoChange, columns);\n", "")],
  [RHS, FULL]),
 ("the right-hand side not put into the padded columns",
  [(D, "        x.leftCols(columns) = b;\n", "")],
  [RHS]),
 ("the real solve the transpose's",
  [(D, "    dgetrs_(\"N\", &n, &columns, lu, &n, pivots, b, &n, &info);", "    dgetrs_(\"T\", &n, &columns, lu, &n, pivots, b, &n, &info);")],
  [RAND, STAT]),
 # ---- which one
 ("QUCS_RF25_LU=eigen not read",
  [(D, "    if (asked == \"eigen\") return DenseLu::Eigen;\n", "")],
  [ENV]),
 ("QUCS_FEM_SOLVER=eigen not read",
  [(D, "    if (qgetenv(\"QUCS_FEM_SOLVER\").trimmed().toLower() == \"eigen\") return DenseLu::Eigen;\n", "")],
  [ENV]),
 ("Accelerate only when asked by name, Automatic taken for Eigen",
  [(D, "    return denseLu() != DenseLu::Eigen && accelerateLuHere();", "    return denseLu() == DenseLu::Accelerate && accelerateLuHere();")],
  [MAC, FULL, STAT, TOOL]),
 ("the build's check not turning Accelerate's LAPACK on",
  [(C, "    set_property(SOURCE rf25/rf25_dense.cpp APPEND PROPERTY COMPILE_DEFINITIONS QUCS_ACCELERATE_LAPACK)\n", "")],
  [MAC]),
 ("Accelerate's factorizations not counted",
  [(D, "            ++byAccelerate;\n", "")],
  [RAND, STAT]),
 # ---- the full wave
 ("the full wave's LU seconds not taken",
  [(F, "    out.luSeconds = double(clock.nsecsElapsed()) * 1e-9 - out.fillSeconds;\n", "")],
  [FULL]),
 ("whose LU it was not said",
  [(F, "    out.accelerated = lu.accelerated();\n", "")],
  [FULL, TOOL]),
 ("the frequencies' seconds not added up",
  [(F, "            out.fillSeconds += a.fillSeconds;\n            out.luSeconds += a.luSeconds;\n", "")],
  [FULL]),
 # ---- the static solve
 ("every net held at a volt in every column",
  [(ST, "        for (int i = 0; i < n; ++i) held(i, netOf.at(i)) = 1.0;\n", "        held.setOnes();\n")],
  [COUPLE, STAT]),
 ("the hint taken of the coefficients handed over",
  [(ST, "        const double hint = p.diagonal().maxCoeff() / std::max(1e-300, p.diagonal().minCoeff());\n", ""),
   (ST, "        const Eigen::MatrixXd charges = lu.solve(held);\n",
        "        const Eigen::MatrixXd charges = lu.solve(held);\n"
        "        const double hint = p.size() > 0 ? p.diagonal().maxCoeff() / std::max(1e-300, p.diagonal().minCoeff()) : 0.0;\n")],
  [STAT]),
 # ---- the tool
 ("rf25_solve's answer giving Eigen for every LU",
  [(R, "        matrix.insert(QStringLiteral(\"LU by\"), w.accelerated ? QStringLiteral(\"Accelerate\") : QStringLiteral(\"Eigen\"));",
       "        matrix.insert(QStringLiteral(\"LU by\"), QStringLiteral(\"Eigen\"));")],
  [TOOL]),
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
