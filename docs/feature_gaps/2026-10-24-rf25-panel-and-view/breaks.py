#!/usr/bin/env python3
"""breaks.py: each part of the panel's report and the view's overlays undone
on purpose, to see its test fail. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors alone; HUNT_RUNS=<dir> for the logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-panel-breaks")
os.makedirs(RUNS, exist_ok=True)

# (name, [(file, old, new)], [(suite, test)])
BREAKS = [
 # ---- the layout processed from the one in memory (rf25_layout.cpp)
 ("the cell the model names is ignored",
  [("rf25/rf25_layout.cpp",
    "    const int cell = wanted.isEmpty() ? read.mainCell() : read.cellIndex(wanted);",
    "    const int cell = read.mainCell();")],
  [("test_rf25_layout", "aLayoutInMemoryIsProcessedWithoutReadingItAgain")]),

 ("the shapes left out by the map go uncounted",
  [("rf25/rf25_layout.cpp", "    out.report.ignored = ignored;", "    out.report.ignored = 0;")],
  [("test_rf25_layout", "aLayoutInMemoryIsProcessedWithoutReadingItAgain"),
   ("test_rf25_doc", "theShapesAndTheMeshAreMadeOfTheLayoutRead")]),

 # ---- the overlays themselves (kept) (layoutdoc.cpp, layoutdoc.h)
 ("the overlays are not drawn at all",
  [("layoutdoc.cpp", "    if (!a_overlays.isEmpty()) {", "    if (false) {")],
  [("test_layout_doc", "overlaysAreDrawnOverTheLayoutWithinTheirBudget"),
   ("test_rf25_doc", "theProcessedShapesAndTheMeshAreDrawnOverTheLayout")]),

 ("a shape out of sight is drawn all the same",
  [("layoutdoc.cpp", "                if (!box.intersects(seen)) continue;", "                if (false) continue;")],
  [("test_layout_doc", "overlaysAreDrawnOverTheLayoutWithinTheirBudget"),
   ("test_rf25_doc", "theProcessedShapesAndTheMeshAreDrawnOverTheLayout")]),

 ("the budget is no limit",
  [("layoutdoc.cpp",
    "                if (a_overlayDrawn >= MostOverlayShapes) {\n                    ++a_overlayLeftOut;\n                    continue;\n                }",
    "                if (false) { ++a_overlayLeftOut; continue; }")],
  [("test_layout_doc", "overlaysAreDrawnOverTheLayoutWithinTheirBudget")]),

 ("setting the overlays does not repaint",
  [("layoutdoc.cpp",
    "    a_overlays = overlays;\n    // (Drawn over the layout's picture, which is unchanged: no invalidate.)\n    update();",
    "    a_overlays = overlays;")],
  [("test_layout_doc", "overlaysAreDrawnOverTheLayoutWithinTheirBudget")]),

 ("the overlays are drawn into the layout's own picture, which is cached",
  [("layoutdoc.cpp", "    a_overlayDrawn = 0;\n    a_overlayLeftOut = 0;",
    "    a_overlayDrawn = 0;\n    a_overlayLeftOut = 0;\n    a_dirty = true;")],
  [("test_layout_doc", "overlaysAreDrawnOverTheLayoutWithinTheirBudget")]),

 # ---- what sets the work off (rf25doc.cpp)
 ("nothing is made of the layout once it is read",
  [("rf25doc.cpp",
    "    // What the solver would be given, of the layout as it has just been\n    // read. It is the work that follows a read, and it waits for none of\n    // the 300 ms a burst of edits is given.\n    prepare();",
    "")],
  [("test_rf25_doc", "theShapesAndTheMeshAreMadeOfTheLayoutRead")]),

 ("an edit of the model makes nothing again",
  [("rf25doc.cpp", "    if (!a_job) schedulePrepare();", "")],
  [("test_rf25_doc", "theMeshIsMadeAgainWithoutReadingTheLayoutAgain")]),

 ("a run waiting its moment is never started for a test or a tool",
  [("rf25doc.cpp", "    if (a_prepareSoon->isActive()) prepare();", "")],
  [("test_rf25_doc", "theMeshIsMadeAgainWithoutReadingTheLayoutAgain")]),

 ("the model that changed while a run went on is not made again",
  [("rf25doc.cpp", "    if (a_prepareAgain) prepare();", "")],
  [("test_rf25_doc", "theMeshIsMadeAgainWithoutReadingTheLayoutAgain")]),

 ("the layout read again leaves the shapes of the one before",
  [("rf25doc.cpp", "        prepare();   // (nothing read: it clears the shapes and the mesh)", "")],
  [("test_rf25_doc", "theShapesAndTheMeshAreMadeOfTheLayoutRead")]),

 # ---- what it says (rf25doc.cpp, rf25panel.cpp)
 ("the tab's notes say nothing of what the layout gives the solver",
  [("rf25doc.cpp", '        lines << tr("What the layout gives the solver:");', '        lines.clear();')],
  [("test_rf25_doc", "theShapesAndTheMeshAreMadeOfTheLayoutRead")]),

 ("the status line says nothing of the cells or their cost",
  [("rf25doc.cpp",
    '        tail += tr("; %1 cell(s), %2").arg(a_mesh->cells.size()).arg(a_mesh->cost.said);',
    '        tail += QString();')],
  [("test_rf25_doc", "theShapesAndTheMeshAreMadeOfTheLayoutRead")]),

 ("what stands in the way of a solve is not said",
  [("rf25doc.cpp", "    if (a_processed) cannot += a_processed->problems;\n    if (a_mesh) cannot += a_mesh->problems;",
    "    if (false) cannot += a_processed->problems;")],
  [("test_rf25_doc", "theMeshIsMadeAgainWithoutReadingTheLayoutAgain")]),

 ("the panel says nothing under the node that decided it",
  [("rf25panel.cpp", '        report->setObjectName(QStringLiteral("rfNodeReport"));',
    '        report->setObjectName(QStringLiteral("rfSomethingElse"));')],
  [("test_rf25_doc", "theShapesAndTheMeshAreMadeOfTheLayoutRead")]),

 ("every node is given the report, not only the ones that decide it",
  [("rf25panel.cpp", '    if (n->type == QLatin1String("rf_processing")) {\n        if (processed) {\n            did = processed->report.lines;',
    '    if (true) {\n        if (processed) {\n            did = processed->report.lines;')],
  [("test_rf25_doc", "theShapesAndTheMeshAreMadeOfTheLayoutRead")]),

 ("the panel keeps the report of the run before",
  [("rf25panel.cpp", "    a_connections << connect(doc, &RF25Doc::preparedChanged, this, [this] { scheduleSettings(); });", "")],
  [("test_rf25_doc", "theMeshIsMadeAgainWithoutReadingTheLayoutAgain")]),

 # ---- what is drawn, and in what colour (rf25doc.cpp)
 ("the cells are drawn whether the mesh is asked for or not",
  [("rf25doc.cpp", "    if (a_showMesh && a_mesh) {", "    if (a_mesh) {")],
  [("test_rf25_doc", "theProcessedShapesAndTheMeshAreDrawnOverTheLayout")]),

 ("the shapes are drawn whether they are asked for or not",
  [("rf25doc.cpp", "    if (a_showProcessed && a_processed) {", "    if (a_processed) {")],
  [("test_rf25_doc", "theProcessedShapesAndTheMeshAreDrawnOverTheLayout")]),

 ("the mesh's overlay holds the conductors, not the cells",
  [("rf25doc.cpp", "        for (const MeshCell& c : a_mesh->cells) byLayer[c.layer] << c.outline;",
    "        for (const Conductor& c : a_processed->conductors) byLayer[c.layer] << c.outline;")],
  [("test_rf25_doc", "theProcessedShapesAndTheMeshAreDrawnOverTheLayout")]),

 ("the outline is the very colour of the fill it lies on",
  [("rf25doc.cpp", "            overlay.line = outlineColor(colourOf(it.key()));", "            overlay.line = colourOf(it.key());")],
  [("test_rf25_doc", "theProcessedShapesAndTheMeshAreDrawnOverTheLayout")]),

 ("the cells take a layer's colour instead of the canvas's ink",
  [("rf25doc.cpp", "        QColor ink = a_view->colors().ink;", "        QColor ink = colourOf(QStringLiteral(\"M1\"));")],
  [("test_rf25_doc", "theProcessedShapesAndTheMeshAreDrawnOverTheLayout")]),

 ("a dark canvas gets the same outline as a light one",
  [("rf25doc.cpp",
    "    return a_view != nullptr && a_view->colors().background.lightness() < 128 ? fill.lighter(180) : fill.darker(220);",
    "    return fill.darker(220);")],
  [("test_rf25_doc", "theProcessedShapesAndTheMeshAreDrawnOverTheLayout")]),

 ("the canvas gone dark leaves the outlines of the light one",
  [("rf25doc.cpp", "    connect(a_view, &LayoutView::colorsChanged, this, &RF25Doc::showOverlays);", "")],
  [("test_rf25_doc", "theProcessedShapesAndTheMeshAreDrawnOverTheLayout")]),
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
                                           stderr=subprocess.STDOUT, timeout=300,
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
        print("%-62s %s" % (name[:62], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
