#!/usr/bin/env python3
"""breaks.py: the viewers' pens undone on purpose, one piece at a time - a
stroke's breadth and colour, the eraser, undo, the pen in hand and its
button, the layout's and the slides' drawing, their pictures and prints,
what Claude's tools say of it, and how a stroke is read for Claude without
a picture (its gesture, the slide's words and parts, the layout's shapes,
read_drawings, the panel's prompts) - each built and its tests run.
Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/pen-markers-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

MC, MH = "markup.cpp", "markup.h"
LD, PD, PH, PX = "layoutdoc.cpp", "pptxdoc.cpp", "pptxdoc.h", "pptx.cpp"
QP, QL, QC = "qucscontrol_pptx.cpp", "qucscontrol_layout.cpp", "qucscontrol.cpp"
CP, APP = "claudecodepanel.cpp", "qucs.cpp"
TP, TL, TC = "test_pptx_doc", "test_layout_doc", "test_claude_code"
MODEL = "aDrawingIsUndoneChangeByChange"
PICK = "thePenIsPickedFromItsMenuAndKept"
SLIDE = "aPenDrawsOnTheSlide"
CLAUDEP = "claudeSeesWhatThePenDrew"
CELL = "aPenDrawsOnTheCellShown"
CLAUDEL = "claudeSeesWhatThePenDrewOnALayout"
READ = "strokesAreReadAsDrawn"
PARTS = "slidePartsAreWhereTheyAreDrawn"
LREAD = "whatThePenDrewOnALayoutIsRead"
PROMPT = "whatThePenDrewGoesWithThePrompt"

BREAKS = [
 # ---- a stroke
 ("a highlighter's colour not let through",
  [(MC, "    if (s.highlighter) s.color.setAlphaF(s.color.alphaF() * HighlighterCover);\n", "")],
  [(TP, MODEL)]),
 ("a stroke as broad in its measures as the pen in pixels, whatever the zoom",
  [(MC, "    s.width = pen.drawingWidth() / std::max(pixelsPerUnit, 1e-300);", "    s.width = pen.drawingWidth();")],
  [(TP, MODEL), (TL, CELL)]),
 ("a highlighter as broad as the pen",
  [(MH, "return tool == Tool::Highlighter ? highlighterWidth : width; }", "return width; }")],
  [(TP, MODEL)]),
 ("a stroke under a pixel broad not drawn at one",
  [(MC, "std::clamp(s.width * pixelsPerUnit, 1.0, BroadestPixels)", "std::min(s.width * pixelsPerUnit, BroadestPixels)")],
  [(TL, CELL)]),
 ("the eraser's reach without the stroke's own half width",
  [(MC, "        const double near = reach + s.width / 2;", "        const double near = reach;")],
  [(TP, MODEL)]),
 ("an eraser's drag not kept for undo",
  [(MC, "void Drawing::changed(const QList<Stroke>& before)\n{\n    a_undo << before;\n    if (a_undo.size() > MostUndone) a_undo.removeFirst();\n}",
    "void Drawing::changed(const QList<Stroke>& before)\n{\n    Q_UNUSED(before);\n}")],
  [(TP, MODEL), (TP, SLIDE), (TL, CELL)]),
 ("a clearing not kept for undo",
  [(MC, "    remember();\n    a_strokes = kept;", "    a_strokes = kept;")],
  [(TP, MODEL)]),
 # ---- the pen in hand and its button
 ("the pen in hand not kept",
  [(MC, "    settings.setValue(QLatin1String(kToolKey), nameOf(pen.tool));\n", "")],
  [(TP, PICK)]),
 ("picking a pen does not take it up",
  [(MC, "        if (!isChecked()) setChecked(true);\n", "")],
  [(TP, PICK)]),
 ("the other viewers' buttons not told",
  [(MC, "    emit Pens::instance()->changed();\n", "")],
  [(TP, PICK)]),
 ("a colour picked with the eraser in hand leaves the eraser",
  [(MC, "                pen.color = color;\n                if (pen.tool == Tool::Eraser) pen.tool = Tool::Pen;\n", "                pen.color = color;\n")],
  [(TP, PICK)]),
 ("the highlighter's widths set the pen's",
  [(MC, "if (pen.tool == Tool::Highlighter) pen.highlighterWidth = kHighlighterWidths[i];", "if (false) pen.highlighterWidth = kHighlighterWidths[i];")],
  [(TP, PICK)]),
 ("the icon not in the pen's colour",
  [(MC, "        QColor tip = a_pen.color;", "        QColor tip = ink;")],
  [(TP, PICK)]),
 # ---- the layout
 ("every cell's strokes drawn on whichever is shown",
  [(LD, "        QList<markup::Stroke> strokes = a_drawing.on(drawingPart());", "        QList<markup::Stroke> strokes = a_drawing.strokes();")],
  [(TL, CELL)]),
 ("the middle button draws",
  [(LD, "    if (event->button() == Qt::LeftButton && a_mode == Mode::Draw && !drawingPart().isEmpty()) {",
    "    if (a_mode == Mode::Draw && !drawingPart().isEmpty()) {")],
  [(TL, CELL)]),
 ("the drawing dropped when the file is read again",
  [(LD, "void LayoutView::setLayout(std::shared_ptr<const Layout> layout)\n{\n", "void LayoutView::setLayout(std::shared_ptr<const Layout> layout)\n{\n    a_drawing = markup::Drawing();\n")],
  [(TL, CELL)]),
 ("a layout printed without the drawing",
  [(LD, "    markup::paint(*painter, a_view->drawing().on(a_view->drawingPart()), [&o](QPointF pt) { return o.toDevice.map(pt); }, scale);\n", "")],
  [(TL, CELL)]),
 ("Escape leaves the pen up",
  [(LD, "        } else if (a_mode != Mode::Select) {\n            setMode(Mode::Select);", "        } else if (a_mode != Mode::Select && a_mode != Mode::Draw) {\n            setMode(Mode::Select);")],
  [(TL, CELL)]),
 # ---- the slides
 ("a stroke kept on the slide after the one drawn on",
  [(PH, "return QString::number(index + 1); }", "return QString::number(index + 2); }")],
  [(TP, SLIDE), (TP, CLAUDEP)]),
 ("drawn past the slide's edge",
  [(PD, "            p.setClipRect(shown);\n", "")],
  [(TP, SLIDE)]),
 ("a double click with the pen up plays the show",
  [(PD, "    if (a_pen) {\n        mousePressEvent(event);\n        return;\n    }\n", "")],
  [(TP, SLIDE)]),
 ("the slide's picture without the drawing",
  [(PD, "    if (!image.isNull() && !strokes.isEmpty()) {", "    if (false) {")],
  [(TP, SLIDE)]),
 ("a slide printed without the drawing",
  [(PD, "        markup::paint(*painter, a_view->drawing().on(SlideView::partOf(i)), [&at, scale](QPointF pt) { return at.topLeft() + pt * scale; }, scale);\n", "")],
  [(TP, SLIDE)]),
 ("the slides' Escape left to the window",
  [(PD, "        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier && (a_pen || a_inking)) {", "        if (false) {")],
  [(TP, SLIDE)]),
 # ---- Claude's tools
 ("the presentation's drawings not said",
  [(QP, "    if (!deck->view()->drawing().isEmpty()) insertDrawings(o, deck->readDrawing(false));", "    if (false) insertDrawings(o, deck->readDrawing(false));")],
  [(TP, CLAUDEP)]),
 ("screenshot's slide drawn without the drawing",
  [(QP, "    const QImage image = drawnToRead(*presentation, slide - 1, region, &scale, deck->view()->drawing().on(qucs_s::pptx::SlideView::partOf(slide - 1)));",
    "    const QImage image = drawnToRead(*presentation, slide - 1, region, &scale);")],
  [(TP, CLAUDEP)]),
 ("show_pptx's clearing not done",
  [(QP, "done << (deck->view()->clearDrawing(-1) ?", "done << (false ?")],
  [(TP, CLAUDEP)]),
 ("show_layout's clearing not done",
  [(QL, "    const bool cleared = drawings == QLatin1String(\"clear\") && view->clearDrawing(true);", "    const bool cleared = false;")],
  [(TL, CLAUDEL)]),
 ("the layout's drawings not said",
  [(QL, "        if (!view->drawing().isEmpty()) insertDrawings(result, view->readDrawing(false));", "        if (false) insertDrawings(result, view->readDrawing(false));")],
  [(TL, CLAUDEL)]),
 # ---- a stroke read: its gesture, what it marks, in a sentence
 ("a ring taken for a line or a mark",
  [(MC, "    if (chord <= 0.25 * size && areaOf(p) >= 0.2 * box.width() * box.height()) return Gesture::Ring;", "    if (false) return Gesture::Ring;")],
  [(TP, READ), (TL, LREAD), (TP, CLAUDEP)]),
 ("an arrow drawn at one go not seen",
  [(MC, "    if (far > 0 && far < last && reach >= 0.6 * size", "    if (false && far > 0 && far < last && reach >= 0.6 * size")],
  [(TP, READ), (TL, LREAD)]),
 ("the pen's wobble as it lifts taken for an arrowhead",
  [(MC, "back >= std::max(0.05 * reach, 2 * stroke.width)", "back > 0")],
  [(TP, READ)]),
 ("a head drawn after its line not joined to it",
  [(MC, "            if (std::min(toEnd, toStart) > 0.2 * length) continue;", "            continue;")],
  [(TP, READ), (TL, LREAD)]),
 ("a dot at a line's end taken for its head",
  [(MC, "            if (out.at(j) != Gesture::Line && out.at(j) != Gesture::Mark) continue;", "            if (out.at(j) == Gesture::Ring || out.at(j) == Gesture::Arrowhead) continue;")],
  [(TP, READ)]),
 ("an arrow drawn from its tip pointing at its tail",
  [(MC, "            if (out.at(i) == Gesture::Line) tip[i] = toEnd <= toStart ? shaft.last() : shaft.first();", "            if (out.at(i) == Gesture::Line) tip[i] = shaft.last();")],
  [(TP, READ)]),
 ("some of a paragraph's words said to be all of it",
  [(MC, "    if (words.isEmpty() || words.size() == t.words.size()) return QJsonObject{{t.kind, shortened(t.text)}};", "    if (true) return QJsonObject{{t.kind, shortened(t.text)}};")],
  [(TP, READ)]),
 ("a word a ring holds a twentieth of taken as held",
  [(MC, "if (insideShare(ring, t.words.at(w).second.boundingRect()) >= 0.6) in << w;", "if (insideShare(ring, t.words.at(w).second.boundingRect()) >= 0.05) in << w;")],
  [(TP, READ)]),
 ("an underline read as a strike, a strike as an underline",
  [(MC, "            if (below ? foot : middle) picked << w;", "            if (below ? middle : foot) picked << w;")],
  [(TP, READ)]),
 ("an arrow points only at what its tip is on",
  [(MC, "    for (int k = 1; t < 0 && ahead > 0 && k <= 16; ++k)", "    for (int k = 1; false; ++k)")],
  [(TP, READ)]),
 ("a highlighter's words not read as highlighted",
  [(MC, "            if (s.highlighter || g == Gesture::Mark) {", "            if (g == Gesture::Mark) {")],
  [(TP, READ)]),
 ("what a stroke marks left out of its sentence",
  [(MC, "        if (reading.contains(QLatin1String(key))) s +=", "        if (false) s +=")],
  [(TP, READ), (TL, LREAD)]),
 ("the strokes left out of the sentences not counted",
  [(MC, "    if (from > 0) lines << QStringLiteral(\"(%1 earlier stroke(s) left out: read_drawings has them all)\").arg(from);\n", "")],
  [(TP, READ)]),
 ("'a orange pen'",
  [(MC, "QStringLiteral(\"aeiou\").contains(colour.left(1)) ? QStringLiteral(\"an\") : QStringLiteral(\"a\")", "QStringLiteral(\"a\")")],
  [(TP, READ)]),
 ("a stroke's every point given, not simplified",
  [(MC, "        QPolygonF few = simplified(stroke.points, tolerance);", "        QPolygonF few = stroke.points;")],
  [(TP, READ)]),
 # ---- a slide's parts
 ("words placed at the frame's top whatever its anchor",
  [(PX, "        else if (anchor == QLatin1String(\"b\")) y += inner.height() - h;\n        qreal x = inner.left();\n        if (!wrap || inner.width() <= 0) {\n            const qreal w = doc->idealWidth();",
    "        qreal x = inner.left();\n        if (!wrap || inner.width() <= 0) {\n            const qreal w = doc->idealWidth();")],
  [(TP, PARTS)]),
 ("a bullet read as a word",
  [(PX, "            const int skip = !para.hanging && !para.bullet.isEmpty() ? int(para.bullet.size()) + 1 : 0;", "            const int skip = 0;")],
  [(TP, PARTS)]),
 ("a picture by its name, not its alt text",
  [(PX, "    if (const Node* cNvPr = nv != nullptr ? nv->child(u\"cNvPr\") : nullptr)\n        e.label = cNvPr->attr(u\"descr\").trimmed().isEmpty() ? cNvPr->attr(u\"name\").trimmed() : cNvPr->attr(u\"descr\").trimmed();",
    "    if (const Node* cNvPr = nv != nullptr ? nv->child(u\"cNvPr\") : nullptr)\n        e.label = cNvPr->attr(u\"name\").trimmed();")],
  [(TP, PARTS)]),
 ("a group's children where their own space puts them",
  [(PX, "            inner = g * t;\n", "")],
  [(TP, PARTS)]),
 ("a table's cells not read",
  [(PX, "                if (!c.covered && c.text != nullptr && !c.text->empty()) texts(*c.text, rects.at(i), t, SlidePart::Cell);", "                Q_UNUSED(c);")],
  [(TP, PARTS)]),
 ("a shape's parts not turned with it",
  [(PX, "    const QTransform t = e.transform() * toSlide;\n    const QRectF box(0, 0, e.rect.width(), e.rect.height());\n    // A text frame's",
    "    const QTransform t = QTransform::fromTranslate(e.rect.x(), e.rect.y()) * toSlide;\n    const QRectF box(0, 0, e.rect.width(), e.rect.height());\n    // A text frame's")],
  [(TP, PARTS)]),
 ("a chart by its frame's name, not its title",
  [(PX, "e.chart != nullptr && e.chart->hasTitle ? e.chart->title : e.label", "e.label")],
  [(TP, PARTS)]),
 # ---- a layout's reading
 ("the layers hidden read",
  [(LD, "    for (int i = 0; i < a_styles.size() && i < layers.size(); ++i) layers[i] = a_styles.at(i).visible;\n    for (const QString& part : a_drawing.parts()) {",
    "    for (const QString& part : a_drawing.parts()) {")],
  [(TL, LREAD)]),
 ("the texts read when hidden",
  [(LD, "        base.labels = a_labels;\n", "")],
  [(TL, LREAD)]),
 ("read below the depth drawn",
  [(LD, "        base.depth = a_depth;\n        base.layers = layers;", "        base.layers = layers;")],
  [(TL, LREAD)]),
 ("an arrow's tip takes in everything within its reach",
  [(LD, "const double reach = markup::reachOf(s), close = std::max(s.width / 2, 1e-9);", "const double reach = markup::reachOf(s), close = reach;")],
  [(TL, LREAD)]),
 ("the cells placed at an arrow's tip not counted",
  [(LD, "        placedAt(layout, base.cell, pt, g);\n", "")],
  [(TL, LREAD)]),
 ("the cells placed in a ring not counted",
  [(LD, "        if (markup::insideShare(ring, box) >= 0.5) ++g.cells[layout.cells.at(placed).name];", "        Q_UNUSED(box); Q_UNUSED(placed);")],
  [(TL, LREAD)]),
 ("a highlighter read at its points alone",
  [(LD, "gatheredUnder(layout, base, markup::alongOf(s), close)", "gatheredUnder(layout, base, s.points, close)")],
  [(TL, LREAD)]),
 ("each copy of a repeated shape counted once",
  [(LD, "const QString key = QStringLiteral(\"%1 %2 %3 %4 %5 %6\").arg(f.cell).arg(f.shape).arg(f.label).arg(f.copy).arg(f.transform.dx()).arg(f.transform.dy());",
    "const QString key = QStringLiteral(\"%1 %2 %3\").arg(f.cell).arg(f.shape).arg(f.label);")],
  [(TL, LREAD)]),
 # ---- read_drawings, and the panel's prompts
 ("read_drawings without the points unless asked",
  [(QP, "    const bool points = !args.contains(QLatin1String(\"points\")) || args.value(QLatin1String(\"points\")).toBool();", "    const bool points = args.value(QLatin1String(\"points\")).toBool();")],
  [(TP, CLAUDEP), (TL, CLAUDEL)]),
 ("read_drawings of a document the pen does not draw on not refused",
  [(QP, "        if (qobject_cast<LayoutDoc*>(QucsApp::documentWidget(doc)) == nullptr && dynamic_cast<PptxDoc*>(doc) == nullptr)\n            return errorResult(", "        if (false)\n            return errorResult(")],
  [(TP, CLAUDEP)]),
 ("every stroke in a tab's answer",
  [(QP, "    constexpr int most = 40;", "    constexpr int most = 4000;")],
  [(TP, CLAUDEP)]),
 ("read_drawings unbounded",
  [(QP, "    constexpr int most = 200;", "    constexpr int most = 20000;")],
  [(TP, CLAUDEP)]),
 ("read_drawings not in every turn while something is drawn",
  [(QC, "            out << QStringLiteral(\"read_drawings\");\n", "")],
  [(TP, CLAUDEP)]),
 ("the app's reading of a deck for a prompt empty",
  [(APP, "      return {revision, revision == known ? QString() : qucs_s::markup::sentencesOf(deck->readDrawing(false), true)};", "      return {revision, QString()};")],
  [(TP, CLAUDEP)]),
 ("a layout's strokes told as a slide's",
  [(APP, "qucs_s::markup::sentencesOf(layout->view()->readDrawing(false), false)};", "qucs_s::markup::sentencesOf(layout->view()->readDrawing(false), true)};")],
  [(TL, CLAUDEL)]),
 ("the panel tells the same drawing at every prompt",
  [(CP, "        if (revision != known && !read.isEmpty()) {", "        if (!read.isEmpty()) {")],
  [(TC, PROMPT)]),
 ("the panel tells a drawing went that it never told of",
  [(CP, "        } else if (revision != known && a_drawingsTold.remove(attached) > 0) {", "        } else if (revision != known) {")],
  [(TC, PROMPT)]),
 ("a new conversation not told afresh",
  [(CP, "    a_session->reset();\n    a_drawingsTold.clear();\n    // With the saved permissions", "    a_session->reset();\n    // With the saved permissions")],
  [(TC, PROMPT)]),
 ("the drawing told with the document chip off",
  [(CP, "    if (!attached.isEmpty() && a_drawings) {\n        const quint64 known = a_drawingsTold.value(attached, 0);",
    "    if (a_drawings && a_document) {\n        const QString attached = a_document();\n        const quint64 known = a_drawingsTold.value(attached, 0);")],
  [(TC, PROMPT)]),
 ("the panel asks for a reading as if it had told nothing",
  [(CP, "        const auto [revision, read] = a_drawings(attached, known);", "        const auto [revision, read] = a_drawings(attached, 0);")],
  [(TC, PROMPT)]),
 ("the app reads a drawing that did not change",
  [(APP, "      return {revision, revision == known ? QString() : qucs_s::markup::sentencesOf(layout->view()->readDrawing(false), false)};",
    "      return {revision, qucs_s::markup::sentencesOf(layout->view()->readDrawing(false), false)};")],
  [(TL, CLAUDEL)]),
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
