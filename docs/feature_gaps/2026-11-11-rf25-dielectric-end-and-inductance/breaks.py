#!/usr/bin/env python3
"""breaks.py: RF 2.5D's dielectric that ends and its inductance past the dense
limit (11 November, F2 and F5 of the open items of 8 November) undone on
purpose, one piece at a time - the compressed loop's scale, kernels and
limit, a via's own cells; the region's background and permittivity, the
faces under a metal, convex pieces, the distance never nought, a wall's
direct path and images, the static solve's correction and its line per
metre, the full wave's correction and background, the processing and the
tab - each built and its tests run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=3 runs that
alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the checkout's
build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-dielectric-end-breaks")

I = "rf25/rf25_inductance.cpp"
D = "rf25/rf25_dielectric.cpp"
S = "rf25/rf25_static.cpp"
F = "rf25/rf25_fullwave.cpp"
L = "rf25/rf25_layout.cpp"
DOC = "rf25doc.cpp"
DRAW = "rf25doc_draw.cpp"
LOOP = ("test_rf25_inductance", "aLoopPastTheDenseLimitIsSolvedCompressed")
POLY = ("test_rf25_dielectric", "aPolygonsIntegralIsItsClosedForm")
WALL = ("test_rf25_dielectric", "aWallsPotentialIsItsGreensFunctionSummedOverIt")
ENDS = ("test_rf25_dielectric", "aSubstrateThatEndsIsSolvedByItsRegion")
FLUSH = ("test_rf25_dielectric", "aPlateOnAMesaFlushWithItFringesAsInAir")
OXIDE = ("test_rf25_dielectric", "anOxideOverTheSubstratesFace")
BOTH = ("test_rf25_doc", "aSubstrateThatEndsIsSolvedByBothSolvers")
TOOL = ("test_qucs_control", "anRf25ModelIsDrawnThroughItsTool")

BREAKS = [
 # F5, the loop past the dense limit
 ("the compressed loop's L not in its own scale",
  [(I, "            li /= scale;\n", "")], [LOOP]),
 ("the compressed loop's y kernel left out",
  [(I, "        part(ky, yRun, dy);\n", "")], [LOOP]),
 ("the compressed loop's vias' kernel left out",
  [(I, "        part(kz, zRun, dz);\n", "")], [LOOP]),
 ("a set dense limit not heeded",
  [(I, "    return set > 0 ? set : std::min(kMostRuns, denseLimit());", "    return std::min(kMostRuns, denseLimit());")], [LOOP]),
 ("a via's cells joined in the plane",
  [(I, "        if (const Layer* l = stack.named(ca.layer); l != nullptr && l->kind == Layer::Via) continue;\n", "")], [LOOP]),
 # F2, the dielectric that ends
 ("Wilton's closed form's out-of-plane term left out",
  [(D, "            sum -= ad * (std::atan2(p0 * lp, r02 + ad * rp) - std::atan2(p0 * lm, r02 + ad * rm));", "            ;")], [POLY]),
 ("the background not the layer as beyond its shapes",
  [(D, "        plan.background.slabs[slab].epsr = e.beyond;\n", "")], [ENDS]),
 ("the region's permittivity the wrong way round",
  [(D, "        r.delta = eps - e.beyond;", "        r.delta = e.beyond - eps;")], [ENDS]),
 ("a face under a metal not that metal's cells",
  [(D, "                    if (std::abs(c.z - z) <= 2 * side2 + 1e-9 * std::max(t, 1e-9))", "                    if (false)")], [ENDS]),
 ("a non-convex piece kept as it is",
  [(D, "                        if (!convex && sq.width() > 0.1 * smallest * 1e6) {", "                        if (false) {"),
   (D, "                            if (isConvex(piece)) addFace(piece, side, minimum);", "                            addFace(piece, side, minimum);")], [ENDS, FLUSH]),
 ("a horizontal distance of nought read as such",
  [(D, "    // to itself, at ten rows.)\n    const double far = std::max(std::hypot(at[0] - p.centre[0], at[1] - p.centre[1]), 1e-12);",
       "    // to itself, at ten rows.)\n    const double far = std::hypot(at[0] - p.centre[0], at[1] - p.centre[1]);")], [WALL]),
 ("a wall's direct path not recognised first",
  [(D, "        if (std::abs(std::abs(a.z) - d1) <= tol) {\n            for (int k = 0; k < t2.size() && match < 0; ++k)", "        if (false) {\n            for (int k = 0; k < t2.size() && match < 0; ++k)")], [WALL]),
 ("a wall's images as lines, not whole",
  [(D, "        if (dLow < 0 || dHigh < 0) {", "        if (true) {")], [WALL]),
 ("the static solve's potential coefficients not corrected",
  [(S, "        if (withDielectric && corrected) p += dielectric.delta;\n", "")], [BOTH]),
 ("a line per metre corrected by the region whole, not the end's difference",
  [(S, "        endOnly = whole.ok() && whole.delta.size() > 0 ? Eigen::MatrixXd(dielectric.delta - whole.delta) : dielectric.delta;",
       "        endOnly = dielectric.delta;")], [BOTH]),
 ("the full wave's potential coefficients not corrected",
  [(F, "    return c.corrected ? base + c.dielectric.at(i, j) : base;", "    return base;")], [BOTH]),
 ("the full wave's Green's functions not the background's",
  [(F, "        c.layers = dielectricPlanned.background;\n", "")], [BOTH]),
 ("a dielectric's shapes processed as conductors",
  [(L, "        if (of->kind == Layer::Dielectric || of->kind == Layer::Substrate) {\n            const QList<QPolygonF> joined",
       "        if (false) {\n            const QList<QPolygonF> joined")], [BOTH]),
 ("the ended layers not handed to the quasi-static solve",
  [(DOC, "        control.extents = job->extents;\n", "")], [BOTH, TOOL]),
 ("a dielectric not drawn on",
  [(DRAW, "        return l != nullptr && (l->kind == Layer::Metal || l->kind == Layer::Via || l->kind == Layer::Dielectric || l->kind == Layer::Substrate);",
       "        return l != nullptr && (l->kind == Layer::Metal || l->kind == Layer::Via);"),
   (DRAW, "        if (l->kind != Layer::Metal && l->kind != Layer::Via && l->kind != Layer::Dielectric && l->kind != Layer::Substrate) {",
       "        if (l->kind != Layer::Metal && l->kind != Layer::Via) {")], [BOTH, TOOL]),
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
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out, stderr=subprocess.STDOUT,
                                           stdin=subprocess.DEVNULL, timeout=1200, env=env, cwd=RUNS)
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
