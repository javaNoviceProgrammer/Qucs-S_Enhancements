#!/usr/bin/env python3
"""breaks.py: the recheck of 75ff6f9's fixes undone on purpose, one at a time
- the sheet's edge loss where the metal is thin beside its skin depth, and a
metal solved as its two faces: its top face and the room under it, its
walls, its faces' impedances and edge, its ports and vias, what is handed
back, and the counts - each built and its tests run. Sources are put back
whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-recheck-75ff6f9-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

FW = "rf25/rf25_fullwave.cpp"
ME = "rf25/rf25_mesh.cpp"
ST = "rf25/rf25_static.cpp"
MO = "rf25/rf25_model.cpp"
TF, TS, TM = "test_rf25_fullwave", "test_rf25_static", "test_rf25_mesh"
FACES_MESH = "aMetalSolvedAsItsTwoFacesIsTwoSheetsAndItsWalls"
FACES_STATIC = "aMetalSolvedAsItsTwoFacesHoldsItsWallsField"
FACES_FW = "aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent"
CPW = "aCoplanarPortReturnsThroughItsGrounds"
VIA = "aViaBetweenTwoMetalsJoinsThem"
CANNOT = "whatItCannotSolveIsSaid"
LOSSES = "aSubstratesAndAMetalsLossesAreTheirs"
EDGE = "aMetalsLossIsItsThickEdgesWhereTheCurrentCrowds"

BREAKS = [
 # ---- the sheet's edge loss, thin beside its skin depth
 ("the edge cells' crowding not faded where Pearl's length is long",
  [(FW, "    const double crowd = 1 + (thick - 1) / (1 + pearl * pearl);", "    const double crowd = thick + 0 * pearl;")],
  [(TF, LOSSES), (TF, EDGE)]),

 ("the slab's resistance beyond Rs/2 crowded as well",
  [(FW, "    return (crowd * half + std::max(0.0, sheet.real() - half)) / sheet.real();", "    return crowd + 0 * half;")],
  [(TF, LOSSES)]),

 # ---- a metal solved as its two faces: the stack, the shapes, the cells
 ("the metal's \"Solved as\" not read",
  [(MO, "            layer.twoSheets = textOf(c, QStringLiteral(\"model\")) == QLatin1String(\"faces\");",
    "            layer.twoSheets = false;")],
  [(TM, FACES_MESH)]),

 ("the room between the faces filled with what is under the metal",
  [(ME, "        for (int j = i + 1; j < stack.layers.size(); ++j) {\n            const Layer& above = stack.layers.at(j);",
    "        for (int j = i - 1; j >= 0; --j) {\n            const Layer& above = stack.layers.at(j);"),
   (ME, "            if (above.kind != Layer::Dielectric) continue;\n            room.epsr = above.epsr;",
    "            if (above.kind != Layer::Dielectric && above.kind != Layer::Substrate) continue;\n            room.epsr = above.epsr;")],
  [(TM, FACES_MESH), (TS, FACES_STATIC)]),

 ("the room the air whatever is over the metal",
  [(ME, "            room.epsr = above.epsr;", "            room.epsr = 1;")],
  [(TM, FACES_MESH)]),

 ("the top face at the bottom's height",
  [(ME, "        room.thickness = metal.thickness;", "        room.thickness = 0;")],
  [(TM, FACES_MESH), (TS, FACES_STATIC)]),

 ("no walls between the faces",
  [(ME, "            out.mesh.cells << wall;", "            (void)wall;")],
  [(TM, FACES_MESH), (TF, FACES_FW)]),

 ("the faces not one conductor to the extraction",
  [(ME, "        out.conductors << walls;", "        (void)walls;")],
  [(TM, FACES_MESH), (TS, FACES_STATIC)]),

 ("a via up from the metal left at its bottom face",
  [(ME, "        if (from != nullptr && topOf.contains(from->name) && upper(from, to, via.to)) via.from = topOf.value(from->name);",
    "        if (false) via.from = topOf.value(from->name);")],
  [(TM, FACES_MESH), (TF, VIA)]),

 ("the panel's count of a two-face metal a sheet's",
  [(ME, "        if (it.value() >= 2) unknowns += twoFaces(it.key().section(QLatin1Char('|'), 0, 0)) ? 2 : 1;",
    "        if (it.value() >= 2) unknowns += 1;")],
  [(TM, FACES_MESH)]),

 ("the mesh refusing what only its top faces make too many",
  [(ME, "    if (mesh.cells.size() > kMostCharges) mesh.problems << chargeCostOf(int(mesh.cells.size()));",
    "    if (charges > kMostCharges) mesh.problems << chargeCostOf(charges);")],
  [(TM, FACES_MESH)]),

 # ---- the extraction
 ("the extraction solving a two-face metal as a sheet",
  [(ST, "    if (!thick.any) return extractSheets(mesh, conductors, stack, micrometre, {});",
    "    if (true) return extractSheets(mesh, conductors, stack, micrometre, {});")],
  [(TS, FACES_STATIC)]),

 ("a top face's charge apart from its cell's in how the whole's crowds",
  [(ST, "                    ofCell[bottomOf.value(which.at(i), which.at(i))] += q(i);", "                    ofCell[which.at(i)] += q(i);")],
  [(TS, FACES_STATIC)]),

 ("a top face's strip apart from its cell's in how the middle's crowds",
  [(ST, "                    ofCell[bottomOf.value(cell, cell)] += q(a);", "                    ofCell[cell] += q(a);")],
  [(TS, FACES_STATIC)]),

 ("the top face's rectangles in Wheeler's boxes too",
  [(ST, "            if (metal != nullptr && metal->face == Layer::Top) continue;", "            if (false) continue;")],
  [(TS, FACES_STATIC)]),

 ("a top face named in its net's name",
  [(ST, "            if (layer == nullptr || layer->face != Layer::Top) named << l;", "            named << l;")],
  [(TS, FACES_STATIC)]),

 # ---- the full wave
 ("the full wave solving a two-face metal as a sheet",
  [(FW, "    const Thickened thick = thickened(givenStack, givenConductors, given);",
    "    Stack plain = givenStack;\n    for (Layer& l : plain.layers) l.twoSheets = false;\n    const Thickened thick = thickened(plain, givenConductors, given);")],
  [(TF, FACES_FW)]),

 ("the faces uncoupled through the metal",
  [(FW, "        m(a, twin) += (across + edge) * squares;", "        m(a, twin) += edge * squares;")],
  [(TF, FACES_FW)]),

 ("each face the sheet's impedance",
  [(FW, "        m(a, a) += (own + edge) * squares;", "        m(a, a) += (sheet + edge) * squares;")],
  [(TF, FACES_FW)]),

 ("the two-face edge stopped where a sheet's is",
  [(FW, "    const double stop = thickness / (4 * M_PI * std::exp(M_PI)) * (faces ? 3 : 1);",
    "    const double stop = thickness / (4 * M_PI * std::exp(M_PI)) * (faces ? 1 : 1);")],
  [(TF, FACES_FW)]),

 ("the edge's excess reactance the slab's, not a thick metal's",
  [(FW, "        const Complex edge = (crowd - 1) * sheet.real() * Complex(1, 1);", "        const Complex edge = (crowd - 1) * sheet;")],
  [(TF, FACES_FW)]),

 ("a port's face driven on its bottom alone",
  [(FW, "        if (top >= 0 && nodeOf.contains(top)) {", "        if (false) {")],
  [(TF, FACES_FW), (TF, CPW)]),

 ("an internal port on a metal's bottom face toward one above",
  [(FW, "        if (!(metalHeight(stack, other) > metalHeight(stack, layer->name))) return metal;", "        return metal;")],
  [(TF, VIA)]),

 ("the feed's vias looked for among the walls",
  [(FW, "            feedVias(&feed, givenConductors, givenStack, on,", "            feedVias(&feed, conductors, stack, on,")],
  [(TF, CPW)]),

 ("the top faces' current left out of what is handed back",
  [(FW, "                    if (t >= 0) i += (ends.at(t).first", "                    if (t < -1) i += (ends.at(t).first")],
  [(TF, FACES_FW), (TF, CPW)]),

 ("the walls' runs handed back as the mesh's",
  [(FW, "            if ((a < c.planar && thick.bottomOf.contains(ends.at(a).second)) || walled.contains(a)) continue;",
    "            if (a < c.planar && thick.bottomOf.contains(ends.at(a).second)) continue;")],
  [(TF, FACES_FW)]),

 ("the full wave's unknowns at most 3000 again",
  [(FW, "constexpr int kMostUnknowns = 6000;", "constexpr int kMostUnknowns = 3000;")],
  [(TF, CANNOT)]),
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
