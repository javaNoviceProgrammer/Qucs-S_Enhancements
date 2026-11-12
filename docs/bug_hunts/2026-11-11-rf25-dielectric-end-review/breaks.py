#!/usr/bin/env python3
"""breaks.py: the fixes of RF 2.5D's dielectric that ends after its test
against the 2D engine (11 November) undone on purpose, one piece at a time -
the normal field's Green's functions and their table, the rectangles'
closed form and the solid angle, the field averaged over a face's panel,
the panels' grading and coarsening, the walls' rows, and the line per metre
where the layer drawn everywhere does not solve - each built and its tests
run. Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks the
anchors and writes nothing; BRK_ONLY=3 runs that alone; HUNT_RUNS=<dir> for
logs; BRK_BUILD=<dir> the build (the checkout's build-rel). Run as a script
only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-dielectric-review-breaks")

G = "rf25/rf25_green.cpp"
D = "rf25/rf25_dielectric.cpp"
S = "rf25/rf25_static.cpp"
FIELD = ("test_rf25_dielectric", "aNormalFieldIsItsPotentialsSlope")
PAIRS = ("test_rf25_dielectric", "aRectanglePairsFieldIsItsClosedForm")
THIN = ("test_rf25_dielectric", "aThinFilmIsItsStacksOwn")
COARSE = ("test_rf25_dielectric", "panelsNextToTheConductorsAreNotCoarsened")
ENDS = ("test_rf25_dielectric", "aSubstrateThatEndsIsSolvedByItsRegion")
FLUSH = ("test_rf25_doc", "aStripFlushWithItsMesaIsThe2DEnginesLine")
BOTH = ("test_rf25_doc", "aSubstrateThatEndsIsSolvedByBothSolvers")

BREAKS = [
 ("the normal field's paths in one slab not signed",
  [(G, "        const auto sign = [this](double s) { return a_side != 0 ? s : 1.0; };", "        const auto sign = [](double) { return 1.0; };")],
  [FIELD, THIN]),
 ("the normal field's round trip across slabs signed as the way straight",
  [(G, "sBack = a_side != 0 ? -sIn : 1.0;", "sBack = sIn;")], [FIELD]),
 ("the normal field without the charge itself on its own plane",
  [(G, "        else if (a_side != 0) series << QPair<double, double>{double(a_side), 0.0};", "")], [THIN, ENDS]),
 ("the normal field's table scaled by rho cubed alone",
  [(G, "            if (i.z > 0 && std::abs(i.a) > 1e-12) a_fieldScale = std::min(a_fieldScale, i.z);", "            a_fieldScale = 0;")], [FIELD]),
 ("two rectangles' mean field sampled, not in closed form",
  [(D, "                    if (pi.isRect && pj.isRect && apart <= kFarSides * std::max(pi.size, pj.size)) {", "                    if (false) {"),
   (D, "                    if (pi.isRect && apartFrom(centre) <= kFarSides * std::max(pi.size, size)) {", "                    if (false) {")],
  [THIN]),
 ("a metal's cell's mean field over a face's panel sampled",
  [(D, "                    if (pi.isRect && apartFrom(centre) <= kFarSides * std::max(pi.size, size)) {", "                    if (false) {")], [THIN]),
 ("the rectangles' antiderivative without its -w R",
  [(D, "    double g = u * v * std::atan(u * v / (w * r)) - w * r;", "    double g = u * v * std::atan(u * v / (w * r));")], [PAIRS]),
 ("the solid angle's edges without the point's height",
  [(D, "        sum += std::atan2(p0 * lp, r02 + ad * rp) - std::atan2(p0 * lm, r02 + ad * rm);", "        sum += std::atan2(p0 * lp, r02) - std::atan2(p0 * lm, r02);")], [PAIRS]),
 ("an edge of the region refining as near a conductor, wherever it runs",
  [(D, "                for (const QLineF& e : edges) apply(e, kEdgeShare * conductors, false);", "                for (const QLineF& e : edges) apply(e, 0, false);")], [THIN]),
 ("the panels next to a metal coarsened with the rest",
  [(D, "                    const bool next = ofMetal && std::hypot(dp, beyond) <= 2 * floor0;", "                    const bool next = false;")],
  [COARSE]),
 ("a wall's rows beginning as fine as the finest cell, not a quarter",
  [(D, "                const double first = std::clamp(finest / 4, t / 400, t / 4);", "                const double first = std::clamp(finest, t / 48, t / 4);")], [FLUSH]),
 ("a wall's strips beside a line as long as its cells, not as wide",
  [(D, "            const double smallestWall = std::max(std::min(t, 2 * finest) / 2, 1e-9);",
       "            const double smallestWall = std::max(std::min(t, 2 * coarsestFinest) / 2, 1e-9);")],
  [FLUSH]),
 ("a line per metre with the layer drawn everywhere unsolved: the layer twice",
  [(S, "        if (!whole.ok() || whole.delta.size() == 0) {", "        if (false) {"),
   (S, "        endOnly = dielectric.delta - whole.delta;",
       "        endOnly = whole.ok() && whole.delta.size() > 0 ? Eigen::MatrixXd(dielectric.delta - whole.delta) : dielectric.delta;")],
  [BOTH]),
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
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    home = os.path.join(RUNS, "home")
                    os.makedirs(home, exist_ok=True)
                    env = dict(os.environ, HOME=home, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn, "-platform", "offscreen"], stdout=out,
                                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=1200, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
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
