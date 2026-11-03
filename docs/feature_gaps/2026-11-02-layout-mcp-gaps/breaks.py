#!/usr/bin/env python3
"""breaks.py: the layout tools' gaps of 2 November closed, then undone on
purpose one piece at a time - what is selected (read, set, brought into
view), the rulers (read, added and snapped, cleared, their angle), where a
cell is placed (its turn, mirror, array copies, depth, region, the cells
looked into), texts found by what they say, the layers' colours (one by one,
a .lyp beside the layout, the palette), the canvas (theme, contrast, grid
style), a reload, and export_image of a layout's view (its scale, the
layout drawn, transparency, what is refused) - each built and its tests
run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/layout-mcp-gaps-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

QL, QC, LD, LY = "qucscontrol_layout.cpp", "qucscontrol.cpp", "layoutdoc.cpp", "layout.cpp"
TL = "test_layout_doc"
TEXT, PLACE, SELECT = "find_shapesFindsTextsByWhatTheySay", "find_placementsFindsWhereACellIsPlaced", "show_layoutSelectsAndMeasures"
COLOUR, EXPORT = "show_layoutColoursTheLayersAndTheCanvas", "export_imageWritesTheLayoutsView"
OVERLAYS = "overlaysAreDrawnOverTheLayoutWithinTheirBudget"

BREAKS = [
 # ---- what is selected
 ("show_layout's answer without what is selected and measured",
  [(QL, "        // What is selected and measured there now - the user's or Claude's.\n        insertSelectionAndRulers(result, *layout, view);\n", "")],
  [(TL, SELECT)]),
 ("get_layout's tab without what is selected and measured",
  [(QL, "            // What the user (or Claude) selected and measured there.\n            insertSelectionAndRulers(shown, *layout, v);\n", "")],
  [(TL, SELECT)]),
 ("a point to select taken as the view's pixels",
  [(QL, "nothingThere = !view->selectAt(view->toPixel(at));", "nothingThere = !view->selectAt(at);")],
  [(TL, SELECT)]),
 ("what is selected out of sight left there",
  [(QL, "                if (!view->visibleRegion().intersects(b)) view->setView(b.center(), view->scale());", "                Q_UNUSED(b);")],
  [(TL, SELECT)]),
 ("what is selected brought into view at another zoom",
  [(QL, "                if (!view->visibleRegion().intersects(b)) view->setView(b.center(), view->scale());",
    "                if (!view->visibleRegion().intersects(b)) doc->zoomToSelection();")],
  [(TL, SELECT)]),
 ("'select' none clearing nothing",
  [(QL, "        if (selectNone) {\n            view->select(std::nullopt);\n        } else if", "        if (selectNone) {\n        } else if")],
  [(TL, SELECT)]),
 # ---- the rulers
 ("rulers not snapped",
  [(QL, "view->addRuler(QLineF(view->snapped(r.p1()), view->snapped(r.p2())));", "view->addRuler(r);")],
  [(TL, SELECT)]),
 ("rulers' clear clearing nothing",
  [(QL, "        if (clearRulers) view->clearRulers();\n", "")],
  [(TL, SELECT)]),
 ("a ruler's angle measured clockwise",
  [(QL, "    double angle = std::atan2(d.y(), d.x()) * 180 / M_PI;", "    double angle = std::atan2(-d.y(), d.x()) * 180 / M_PI;")],
  [(TL, SELECT)]),
 ("the view's snap within no distance",
  [(LD, "    return snap(*a_layout, queryHere(), point, tolerance() * 1.5);", "    return snap(*a_layout, queryHere(), point, 0.0);")],
  [(TL, SELECT)]),
 # ---- where a cell is placed
 ("a placement's turn read clockwise",
  [(QL, "double turn = std::atan2(t.m12(), t.m11()) * 180 / M_PI;", "double turn = std::atan2(-t.m12(), t.m11()) * 180 / M_PI;")],
  [(TL, PLACE)]),
 ("a mirrored placement not said so",
  [(QL, "                if (det < 0) o.insert(QStringLiteral(\"mirrored\"), true);", "                if (det > 1e300) o.insert(QStringLiteral(\"mirrored\"), true);")],
  [(TL, PLACE)]),
 ("the cells placed not looked into",
  [(LY, "            const bool below = level < std::min(query.depth, MaxLevels) && holds.at(inst.cell);",
    "            const bool below = level < 0 && holds.at(inst.cell);")],
  [(TL, PLACE)]),
 ("the depth not held to",
  [(LY, "            const bool below = level < std::min(query.depth, MaxLevels) && holds.at(inst.cell);",
    "            const bool below = level < MaxLevels && holds.at(inst.cell);")],
  [(TL, PLACE)]),
 ("an array's first copy alone",
  [(LY, "            r.forEach(one, everywhere ? QRectF() : vis, [&](QPointF off) { return at(off, copyAt(r, off)); });",
    "            Q_UNUSED(r);\n            at(QPointF(), 0);")],
  [(TL, PLACE)]),
 ("find_placements' region not held to",
  [(QL, "        if (region.size() == 4)\n            q.region = QRectF(QPointF(region.at(0).toDouble(), region.at(1).toDouble()), QPointF(region.at(2).toDouble(), region.at(3).toDouble()))\n                           .normalized();\n        constexpr quint64 kCountUpTo = 100000;\n        quint64 count = 0;\n        QJsonArray list;",
    "        constexpr quint64 kCountUpTo = 100000;\n        quint64 count = 0;\n        QJsonArray list;")],
  [(TL, PLACE)]),
 ("no cell taken as placing it: none looked into",
  [(LY, "            if (!holds.at(p)) {\n                holds[p] = true;", "            if (false) {\n                holds[p] = true;")],
  [(TL, PLACE)]),
 # ---- texts by what they say
 ("'text' finding every text and shape",
  [(QL, "            if (byText && (!f.isText() || !pattern.match(", "            if (false && (!f.isText() || !pattern.match(")],
  [(TL, TEXT)]),
 ("a text matched in one case only",
  [(QL, "                                          : QRegularExpression::escape(textWanted),\n                                     QRegularExpression::CaseInsensitiveOption);",
    "                                          : QRegularExpression::escape(textWanted),\n                                     QRegularExpression::NoPatternOption);")],
  [(TL, TEXT)]),
 ("'text' with 'texts' false taken",
  [(QL, "    if (byText && !texts) return done(errorResult(", "    if (false) return done(errorResult(")],
  [(TL, TEXT)]),
 # ---- the layers' colours, the canvas, a reload
 ("a layer's colour its fill alone",
  [(LD, "    styles[layer].frame = styles[layer].fill = colour;", "    styles[layer].fill = colour;")],
  [(TL, COLOUR)]),
 ("a colour that is none taken",
  [(QL, "        if (!it.value().isString() || !QColor::fromString(it.value().toString().trimmed()).isValid())\n            return done(",
    "        if (false)\n            return done(")],
  [(TL, COLOUR)]),
 ("a .lyp beside the layout not looked for",
  [(QL, "                const QString file = QFileInfo(lyp).isRelative() && QFileInfo::exists(beside) ? beside : lypGiven;",
    "                const QString file = lypGiven;\n                Q_UNUSED(beside);")],
  [(TL, COLOUR)]),
 ("the palette not taken again",
  [(QL, "                doc->usePalette();\n", "")],
  [(TL, COLOUR)]),
 ("the theme not set",
  [(QL, "            doc->setCanvasTheme(theme == QLatin1String(\"light\")", "            (void)(theme == QLatin1String(\"light\")")],
  [(TL, COLOUR)]),
 ("the colours' contrast not set",
  [(QL, "        if (contrast.isBool()) doc->setColorsAdapted(contrast.toBool());\n", "")],
  [(TL, COLOUR)]),
 ("the grid's style not set",
  [(QL, "            doc->setGridStyle(gridStyle == QLatin1String(\"lines\")", "            (void)(gridStyle == QLatin1String(\"lines\")")],
  [(TL, COLOUR)]),
 ("'reload' reading nothing again",
  [(QL, "        if (open != nullptr && !open->isLoading()) open->reload();\n", "")],
  [(TL, COLOUR)]),
 ("get_layout without a layer's colour",
  [(QL, "            if (i < styles.size() && styles.at(i).frame.isValid()) l.insert(QStringLiteral(\"colour\"), styles.at(i).frame.name());\n", "")],
  [(TL, COLOUR)]),
 # ---- export_image of a layout
 ("a layout's picture at its view's pixels whatever the scale",
  [(QL, "    double factor = scale.isDouble() ? scale.toDouble() : 2.0;", "    double factor = scale.isDouble() ? scale.toDouble() : 1.0;")],
  [(TL, EXPORT)]),
 ("the picture without the layout",
  [(LD, "    if (a_layout && a_cell >= 0) {\n        if (a_grid) paintGrid(p, c);\n        qucs_s::layout::render(p, *a_layout, renderOptions());\n    }\n    paintOver(p, c);",
    "    if (a_layout && a_cell >= 0) {\n        if (a_grid) paintGrid(p, c);\n    }\n    paintOver(p, c);")],
  [(TL, EXPORT)]),
 ("transparent ignored",
  [(QL, "            QPainter painter(&image);\n            view->paintOnto(painter, !transparent);", "            QPainter painter(&image);\n            view->paintOnto(painter, true);")],
  [(TL, EXPORT)]),
 ("a layout exported as a schematic",
  [(QC, "    if (isLayoutExport(args)) return exportLayout(args);\n", "")],
  [(TL, EXPORT)]),
 ("EPS taken for a layout",
  [(QL, "        if (*format == gx::Format::Eps || *format == gx::Format::PdfTex) return errorResult(wanted);\n", "")],
  [(TL, EXPORT)]),
 ("a schematic's 'diagram' taken for a layout",
  [(QL, "    if (args.contains(QLatin1String(\"diagram\")) || args.contains(QLatin1String(\"selection\")) || args.contains(QLatin1String(\"slides\")))",
    "    if (args.contains(QLatin1String(\"selection\")) || args.contains(QLatin1String(\"slides\")))")],
  [(TL, EXPORT)]),
 ("grayscale taken for a layout",
  [(QL, "    if (colours != QLatin1String(\"colour\") && colours != QLatin1String(\"color\"))\n        return errorResult(tr(\"A layout is written in its layers' colours",
    "    if (false)\n        return errorResult(tr(\"A layout is written in its layers' colours")],
  [(TL, EXPORT)]),
 # ---- the view's painting, split in two
 ("the view not drawing what goes over the layout",
  [(LD, "    if (!drawn) p.drawImage(QPointF(0, 0), a_cache);\n    paintOver(p, c);\n}", "    if (!drawn) p.drawImage(QPointF(0, 0), a_cache);\n}")],
  [(TL, OVERLAYS)]),
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
                                                    ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude",
                                                    QUCS_GH="/nonexistent/gh"))
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
