#!/usr/bin/env python3
"""breaks.py: each part of the layer map and the processing undone on
purpose, to see its test fail. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors alone; HUNT_RUNS=<dir> for the logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-layout-breaks")
os.makedirs(RUNS, exist_ok=True)
T = "test_rf25_layout"

BREAKS = [
 ("shapes are merged even where the user turned merging off",
  [("rf25/rf25_layout.cpp", "        } else if (how.merge && shapes.size() > 1) {",
    "        } else if (shapes.size() > 1) {")],
  ["touchingShapesAreMergedIntoOne"]),

 ("touching shapes are left as they were drawn",
  [("rf25/rf25_layout.cpp", "            shapes = unionOf(shapes);", "            shapes = shapes;")],
  ["touchingShapesAreMergedIntoOne"]),

 ("a via farm is united without being closed first, so nothing joins",
  [("rf25/rf25_layout.cpp",
    "            const QList<QPolygonF> grown = offsetOf(shapes, viaDistance / 2);",
    "            const QList<QPolygonF> grown = shapes;")],
  ["aViaFarmIsMergedAndItsAreaKept"]),

 ("the closing is never undone, so the vias come out fat",
  [("rf25/rf25_layout.cpp",
    "            QList<QPolygonF> one = offsetOf(unionOf(grown), -viaDistance / 2);",
    "            QList<QPolygonF> one = unionOf(grown);")],
  ["aViaFarmIsMergedAndItsAreaKept"]),

 ("the merged vias keep whatever area the closing gave them",
  [("rf25/rf25_layout.cpp",
    "            for (int step = 0; how.keepArea && step < 5 && after > wanted * 1.01 && !one.isEmpty(); ++step) {",
    "            for (int step = 0; false && step < 5; ++step) {")],
  ["aViaFarmIsMergedAndItsAreaKept"]),

 ("vias further apart than the distance are joined anyway",
  [("rf25/rf25_layout.cpp", "offsetOf(shapes, viaDistance / 2);", "offsetOf(shapes, viaDistance * 4);")],
  ["aViaFarmIsMergedAndItsAreaKept"]),

 ("a sliver's width is not looked at",
  [("rf25/rf25_layout.cpp",
    "                if (narrowness(s) < sliverWidth || areaOf(s) < sliverWidth * sliverWidth) ++dropped;",
    "                if (false) ++dropped;")],
  ["sliversAreDroppedAndSaid"]),

 ("the points are not moved to the grid",
  [("rf25/rf25_layout.cpp",
    "                    const QPointF to(std::round(p.x() / grid) * grid, std::round(p.y() / grid) * grid);",
    "                    const QPointF to(p.x(), p.y());")],
  ["snappingHealingAndFill"]),

 ("a point repeated along the way is not counted as damage",
  [("rf25/rf25_layout.cpp",
    "                if (repeated > 0 || one.size() != 1 || one.first().size() != tidy.size()) ++out.report.healed;",
    "                if (one.size() != 1 || one.first().size() != tidy.size()) ++out.report.healed;")],
  ["snappingHealingAndFill"]),

 ("the point that closes a ring is counted as damage",
  [("rf25/rf25_layout.cpp",
    "                while (tidy.size() > 1 && tidy.last() == tidy.first()) tidy.removeLast();",
    "                if (false) tidy.removeLast();")],
  ["touchingShapesAreMergedIntoOne"]),   # (only this one sees it: the other's shapes are built by hand)

 ("dummy fill is dropped whether it was asked for or not",
  [("rf25/rf25_layout.cpp", "        if (isFill && how.removeFill) {", "        if (isFill) {")],
  ["snappingHealingAndFill"]),

 ("fill that is kept is not marked as floating metal",
  [("rf25/rf25_layout.cpp", "            c.fill = isFill;", "            c.fill = false;")],
  ["snappingHealingAndFill"]),

 ("a layer sent to two layers of the stack goes unsaid",
  [("rf25/rf25_layout.cpp",
    '        if (map.toLayer.contains(key))\n            map.problems << tr("The layer map sends %1 to two layers of the stack (%2 and %3).")',
    '        if (false)\n            map.problems << tr("The layer map sends %1 to two layers of the stack (%2 and %3).")')],
  ["theLayerMapSendsShapesWhereItSays"]),

 ("a row that names no layer of the stack goes unsaid",
  [("rf25/rf25_layout.cpp",
    '        if (stack.named(to) == nullptr)\n            map.problems << tr("The layer map sends %1 to %2, which is no layer of the stack.").arg(key, to);',
    '        if (false)\n            map.problems << tr("The layer map sends %1 to %2, which is no layer of the stack.").arg(key, to);')],
  ["theLayerMapSendsShapesWhereItSays"]),

 ("the fill note on a row is ignored",
  [("rf25/rf25_layout.cpp",
    '        if (note.compare(QLatin1String("fill"), Qt::CaseInsensitive) == 0) map.fill.insert(to);',
    '        if (false) map.fill.insert(to);')],
  ["theLayerMapSendsShapesWhereItSays"]),

 ("a length that is no number is taken as nothing instead of being said",
  [("rf25/rf25_layout.cpp",
    '            *problems << tr("Processing: %1 is not a number (%2).").arg(key, e.isValid() ? tr("it is not a constant") : e.error());',
    '            (void) key;')],
  ["theProcessingIsReadFromItsNode"]),

 ("a cell the file has not goes unsaid",
  [("rf25/rf25_layout.cpp",
    '        out.problems << tr("%1 has no cell %2.").arg(QFileInfo(path).fileName(), wanted);',
    '        (void) wanted;')],
  ["aLayoutFileIsReadAndProcessed"]),

 ("shapes on layers the map does not name are not counted",
  [("rf25/rf25_layout.cpp", "            ++ignored;", "            (void) 0;")],
  ["aLayoutFileIsReadAndProcessed"]),

 ("a layout whose every shape was dropped looks like a success",
  [("rf25/rf25_layout.cpp",
    '    if (out.conductors.isEmpty() && out.report.read > 0)\n        out.problems << tr("Every shape was dropped: nothing is left to solve on.");',
    '    if (false)\n        out.problems << tr("Every shape was dropped: nothing is left to solve on.");')],
  ["whatCannotBeProcessedIsSaid"]),
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
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B, T], stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for fn in tests:
                    r = subprocess.run([os.path.join(B, "qucs/tests", T), fn], stdout=out, stderr=subprocess.STDOUT,
                                       env=dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0"))
                    said.append("%s %s" % (fn[:30], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-58s %s" % (name[:58], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
