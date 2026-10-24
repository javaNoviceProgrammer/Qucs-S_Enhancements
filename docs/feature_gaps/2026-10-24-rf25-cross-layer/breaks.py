#!/usr/bin/env python3
"""breaks.py: each part of the cross-layer Green's function and of the
extraction over several metals undone on purpose, to see its test fail.
Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors alone; HUNT_RUNS=<dir> for the logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-cross-breaks")
os.makedirs(RUNS, exist_ok=True)

G, S = "rf25/rf25_green.cpp", "rf25/rf25_static.cpp"
GREEN, STATIC = "test_rf25_green", "test_rf25_static"

BREAKS = [
 # ---- the cross-layer recursion
 ("the interface transmits what it reflects",
  [(G, "        const double t = upward ? 1 + interfaceAt(face) : 1 - interfaceAt(face);",
    "        const double t = upward ? interfaceAt(face) : -interfaceAt(face);")],
  [(GREEN, "aChargeSeenAcrossOneInterfaceIsItsTransmittedImage")]),

 ("a slab the wave only passes through reflects everything below it back, not the one face it crossed",
  [(G, "        amp *= upward ? 1 + interfaceAt(face) : 1 - interfaceAt(face);\n        const int next = j + step;\n        if (next != a_fld) {\n            const double hj = thicknessAt(next);\n            const double ahead = upward ? upwardAt(next, k) : downwardAt(next, k);\n            const double back = upward ? -interfaceAt(face) : interfaceAt(face);",
    "        amp *= upward ? 1 + interfaceAt(face) : 1 - interfaceAt(face);\n        const int next = j + step;\n        if (next != a_fld) {\n            const double hj = thicknessAt(next);\n            const double ahead = upward ? upwardAt(next, k) : downwardAt(next, k);\n            const double back = upward ? downwardAt(next, k) : upwardAt(next, k);")],
  [(GREEN, "theRecursionAgreesWithTheProblemSolvedOutright")]),

 ("the point below the charge is measured from the wrong face of its slab",
  [(G, '    const double arriveAt = upward ? a_z - a_layers.slabs.at(a_fld).z0 : a_layers.slabs.at(a_fld).z1 - a_z;',
    '    const double arriveAt = a_z - a_layers.slabs.at(a_fld).z0;')],
  [(GREEN, "theRecursionAgreesWithTheProblemSolvedOutright")]),

 # (Both the images and the kernel they are an expansion of, since the
 # behaviour is the one thing written out twice.)
 ("the charge leaving its slab downward is measured from the wrong face",
  [(G, "        // How far the charge is from the face it leaves by, and how far\n        // the point is from the face the wave arrives at.\n        const double leaveAt = upward ? hs - us : us;",
    "        const double leaveAt = hs - us;"),
   (G, "    const bool upward = a_fld > a_src;\n    const double leaveAt = upward ? hs - us : us;",
    "    const bool upward = a_fld > a_src;\n    const double leaveAt = hs - us;")],
  [(GREEN, "theChargeAndThePointMayChangePlaces")]),

 ("the wave that went the other way first and came back is left out",
  [(G, "    double amp = (std::exp(-k * leaveAt) + rdS * std::exp(-k * (2 * hs - leaveAt)))\n                 / (1 - ruS * rdS * std::exp(-2 * k * hs));",
    "    double amp = std::exp(-k * leaveAt) / (1 - ruS * rdS * std::exp(-2 * k * hs));")],
  [(GREEN, "theRecursionAgreesWithTheProblemSolvedOutright")]),

 ("what comes back off the far face of the point's own slab is left out",
  [(G, "    return amp * (std::exp(-k * arriveAt) + ahead * std::exp(-k * (2 * hf - arriveAt)))\n           / (1 - ahead * back * std::exp(-2 * k * hf));",
    "    return amp * std::exp(-k * arriveAt) / (1 - ahead * back * std::exp(-2 * k * hf));")],
  [(GREEN, "theRecursionAgreesWithTheProblemSolvedOutright")]),

 ("two metals with one dielectric between them lose the way straight from one to the other",
  [(G, "        if (a_z != a_zSource) series << QPair<double, double>{1.0, std::abs(a_z - a_zSource)};", "")],
  [(GREEN, "theImagesSumBackToTheSpectralReflection")]),

 ("a charge seen from another layer is given a free-space term of its own as well",
  [(G, "    return sameHeight() && rho > 0 ? 1.0 / (4 * M_PI * epsilon() * rho) : 0.0;",
    "    return rho > 0 ? 1.0 / (4 * M_PI * epsilon() * rho) : 0.0;")],
  [(GREEN, "aChargeSeenAcrossOneInterfaceIsItsTransmittedImage")]),

 # ---- the pruning, the lump and the table
 ("a term is judged by its strength alone, with no regard to how far it travelled",
  [(G, "    double worth(double a, double z) const { return std::abs(a) * zRef / std::max(z, zRef); }",
    "    double worth(double a, double z) const { Q_UNUSED(z); return std::abs(a); }")],
  [(GREEN, "whatTheTruncationDroppedIsCounted")]),

 ("the coarsening is counted as missing, which the lump stands for",
  [(G, "    a_truncated = budget.missing;\n    a_coarsened = budget.coarsened;",
    "    a_truncated = budget.missing + budget.coarsened;\n    a_coarsened = 0;")],
  [(GREEN, "theImagesAndTheIntegralAgree")]),

 ("the table is read straight between its samples, not through a cubic",
  [(G, "    const double product =\n        p1 + 0.5 * f * ((p2 - p0) + f * ((2 * p0 - 5 * p1 + 4 * p2 - p3) + f * (3 * (p1 - p2) + p3 - p0)));",
    "    const double product = p1 * (1 - f) + p2 * f;")],
  [(GREEN, "theTableAndTheSplitAreTheImagesAgain")]),

 # ---- the nets and the vias
 ("a via joins nothing: two metals it lands on stay apart",
  [(S, "                join(v, c);", "")],
  [(STATIC, "aViaJoinsTwoMetalsIntoOneNet")]),

 ("a via joins whatever shares its layer map, wherever it is",
  [(S, "                if (!conductors.at(c).outline.boundingRect().intersects(where)) continue;\n                if (unionOf({conductors.at(c).outline, conductors.at(v).outline}).size() > 1) continue;",
    "")],
  [(STATIC, "aViaJoinsTwoMetalsIntoOneNet")]),

 ("a via carries a charge of its own, as though it were a sheet",
  [(S, "        const Layer* layer = stack.named(cell.layer);\n        if (layer != nullptr && layer->kind == Layer::Via) continue;",
    "        const Layer* layer = stack.named(cell.layer);")],
  [(STATIC, "aViaJoinsTwoMetalsIntoOneNet")]),

 ("every metal is placed where the lowest one is",
  [(S, "        if (l.name == metal || l.tag == metal) return l.kind == Layer::Metal ? z : -1;",
    "        if (l.name == metal || l.tag == metal) return l.kind == Layer::Metal ? 0.0 : -1;")],
  [(STATIC, "twoPlatesFacingEachOtherAreAParallelPlateCapacitor")]),

 # ---- the matrix over several layers
 ("the images past the strongest few are left out of a near pair altogether",
  [(S, "                p(i, j) = sum / (4 * M_PI * g.epsilon()) + g.beyondStrongest(distance, int(terms.size()));",
    "                p(i, j) = sum / (4 * M_PI * g.epsilon());")],
  [(STATIC, "aPlateOverSeveralDielectricsIsTheirSeriesCapacitance")]),

 ("a cell on another layer is given the Green's function of its own",
  [(S, "            const Green& g = greenOf(ci.layer, cj.layer);", "            const Green& g = greenOf(ci.layer, ci.layer);")],
  [(STATIC, "twoPlatesFacingEachOtherAreAParallelPlateCapacitor")]),
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
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
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
                                           stderr=subprocess.STDOUT, timeout=600,
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
        print("%-72s %s" % (name[:72], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
