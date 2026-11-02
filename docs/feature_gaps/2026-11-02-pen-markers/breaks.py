#!/usr/bin/env python3
"""breaks.py: the viewers' pens undone on purpose, one piece at a time - a
stroke's breadth and colour, the eraser, undo, the pen in hand and its
button, the layout's and the slides' drawing, their pictures and prints,
and what Claude's tools say of it - each built and its tests run. Sources
are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/person/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/qucs-s-26.1.1/qucs/"
B = "/Users/person/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/pen-markers-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

MC, MH = "markup.cpp", "markup.h"
LD, PD, PH = "layoutdoc.cpp", "pptxdoc.cpp", "pptxdoc.h"
QP, QL = "qucscontrol_pptx.cpp", "qucscontrol_layout.cpp"
TP, TL = "test_pptx_doc", "test_layout_doc"
MODEL = "aDrawingIsUndoneChangeByChange"
PICK = "thePenIsPickedFromItsMenuAndKept"
SLIDE = "aPenDrawsOnTheSlide"
CLAUDEP = "claudeSeesWhatThePenDrew"
CELL = "aPenDrawsOnTheCellShown"
CLAUDEL = "claudeSeesWhatThePenDrewOnALayout"

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
  [(QP, "    if (!deck->view()->drawing().isEmpty())\n        o.insert(QStringLiteral(\"drawings\")", "    if (false)\n        o.insert(QStringLiteral(\"drawings\")")],
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
  [(QL, "        if (!view->drawing().isEmpty()) result.insert(QStringLiteral(\"drawings\")", "        if (false) result.insert(QStringLiteral(\"drawings\")")],
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
