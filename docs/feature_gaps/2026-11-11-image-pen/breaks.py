#!/usr/bin/env python3
"""breaks.py: the pen on a picture's tab (11 November) undone on purpose, one
piece at a time - its button, taking it up, drawing before the selection and
the view's drag, the strokes painted (zoomed, turned, clipped), a double
click, the middle button, Ctrl+Z, Escape, the eraser, a copy, a print, the
file left as it is, a reload, the menu, frames - and what reaches Claude: a
prompt's note, read_drawings, show_picture's pen, drawings and addressed,
send_input's target picture, the tools in every turn. Each built and its tests
run, in ctest's environment for test_image_doc (offscreen, no claude, no gh).
Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks the anchors and
writes nothing; BRK_ONLY=3,7 runs those alone; HUNT_RUNS=<dir> for logs;
BRK_BUILD=<dir> the build (the checkout's build-rel). Run as a script only."""
import json, os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/image-pen-breaks")

D = "imagedoc.cpp"
A = "qucs.cpp"
C = "qucscontrol.cpp"
R = "qucscontrol_pptx.cpp"
P = "qucscontrol_picture.cpp"
I = "qucscontrol_input.cpp"
PEN = ("test_image_doc", "thePenDrawsOnThePicture")
CLAUDE = ("test_image_doc", "whatThePenDrewIsReadByClaude")
FRAMES = ("test_image_doc", "severalPicturesAreDrawnOnOneByOne")

BREAKS = [
 # ---- The tab
 ("the pen not in the toolbar",
  [(D, "    bar->addWidget(a_penButton);\n", "")], [PEN]),
 ("the button not taking the pen up",
  [(D, "    connect(a_penButton, &QToolButton::toggled, a_view, &ImageView::setPenActive);\n", "")], [PEN]),
 ("the pen not first on a press: the selection's drag",
  [(D, "    // erases, before it selects or moves the view (beside it, nothing).\n    if (a_pen && event->button() == Qt::LeftButton) {",
       "    // erases, before it selects or moves the view (beside it, nothing).\n    if (a_pen && event->button() == Qt::LeftButton && !a_selecting) {")], [PEN]),
 ("the strokes not painted",
  [(D, "            qucs_s::markup::paint(p, strokes, [&v](QPointF pt) { return v.map(pt); }, a_zoom);\n", "")], [PEN]),
 ("the strokes not turned with the picture",
  [(D, "            const QTransform v = toViewportTransform();\n            qucs_s::markup::paint",
       "            const QTransform v = QTransform::fromScale(a_zoom, a_zoom) * QTransform::fromTranslate(pictureRect().left(), pictureRect().top());\n            qucs_s::markup::paint")], [PEN]),
 ("the strokes drawn past the picture's edge",
  [(D, "            p.save();\n            p.setClipRect(visible);\n            const QTransform v", "            p.save();\n            const QTransform v")], [PEN]),
 ("a stroke as broad in the picture's pixels as on the screen",
  [(D, "            a_stroke = qucs_s::markup::begin(pen, a_part, toPicture(event->position()), a_zoom);",
       "            a_stroke = qucs_s::markup::begin(pen, a_part, toPicture(event->position()), 1);")], [PEN]),
 ("a press beside the picture drawing",
  [(D, "        if (isEmpty() || a_inking || a_erasing || !pictureRect().contains(event->position())) return;",
       "        if (isEmpty() || a_inking || a_erasing) return;")], [PEN]),
 ("a double click with the pen up a zoom",
  [(D, "    // With the pen up a double click is two presses of it, not a zoom.\n    if (a_pen && event->button() == Qt::LeftButton) {\n        mousePressEvent(event);\n        return;\n    }\n", "")], [PEN]),
 ("the middle button drawing with the pen up",
  [(D, "    // erases, before it selects or moves the view (beside it, nothing).\n    if (a_pen && event->button() == Qt::LeftButton) {",
       "    // erases, before it selects or moves the view (beside it, nothing).\n    if (a_pen && event->button() != Qt::RightButton) {")], [PEN]),
 ("Ctrl+Z not the pen's",
  [(D, "    if (a_pen && event->matches(QKeySequence::Undo)) {\n        if (!a_inking && !a_erasing) undoDrawing();\n        return;\n    }\n", "")], [PEN, CLAUDE]),
 ("Escape not putting the pen down",
  [(D, "        } else {\n            setPenActive(false);\n        }\n        return;", "        }\n        return;")], [PEN, CLAUDE]),
 ("Escape left to the window's shortcuts",
  [(D, "((key->key() == Qt::Key_Escape && (!a_selection.isEmpty() || a_pen || a_inking))",
       "((key->key() == Qt::Key_Escape && !a_selection.isEmpty())")], [PEN]),
 ("the eraser not erasing",
  [(D, "    while (a_drawing.eraseAt(a_part, toPicture(pos), qucs_s::markup::EraserReach / a_zoom)) erased = true;",
       "    (void)pos;")], [PEN]),
 ("a copy without the strokes",
  [(D, "        drawStrokes(image, selection.isEmpty() ? QRectF(QPointF(0, 0), QSizeF(image.size())) : QRectF(selection), 1);\n", "")], [PEN]),
 ("a print without the strokes",
  [(D, "    qucs_s::markup::paint(*painter, a_view->drawing().on(partOf(a_frame)), [](QPointF pt) { return pt; }, 1);\n", "")], [PEN]),
 ("the strokes written into the file (Save As)",
  [(D, "    return a_frames.isEmpty() ? QImage() : a_frames.at(a_frame);\n}\n\nvoid ImageDoc::copyImage()",
       "    if (a_frames.isEmpty()) return QImage();\n    QImage drawn = a_frames.at(a_frame);\n    drawStrokes(drawn, QRectF(QPointF(0, 0), QSizeF(drawn.size())), 1);\n    return drawn;\n}\n\nvoid ImageDoc::copyImage()")], [PEN]),
 ("the strokes dropped when the file is read again",
  [(D, "void ImageDoc::show(Picture& picture, bool keepView)\n{\n", "void ImageDoc::show(Picture& picture, bool keepView)\n{\n    a_view->clearDrawing();\n")], [PEN]),
 ("the pen's menu not told of the drawing",
  [(D, "    connect(a_view, &ImageView::drawingChanged, this, &ImageDoc::updatePen);\n", "")], [PEN]),
 ("the menu's Clear the Drawings on This Picture clearing every frame's",
  [(D, "[this] { a_view->clearDrawing(a_view->part()); });", "[this] { a_view->clearDrawing(); });")], [FRAMES]),
 ("a frame shown, the strokes kept on the one before",
  [(D, "    a_view->setImage(a_frames.at(frame));\n    a_view->setPart(partOf(frame));\n", "    a_view->setImage(a_frames.at(frame));\n")], [FRAMES]),
 ("an animation's frames drawn on one by one",
  [(D, "    return QString::number(isAnimated() || a_svg != nullptr ? 1 : frame + 1);", "    return QString::number(a_svg != nullptr ? 1 : frame + 1);")], [FRAMES]),
 # ---- What reaches Claude
 ("a prompt not told what was drawn on a picture",
  [(A, "    if (auto *picture = qobject_cast<ImageDoc *>(documentWidget(doc))) {", "    if (auto *picture = (ImageDoc *)nullptr) {")], [CLAUDE]),
 ("a prompt not told the pen is still up",
  [(A, "penSaid(picture->drawingSentences(), picture->penButton(), \"show_picture\")", "penSaid(picture->drawingSentences(), nullptr, \"show_picture\")")], [CLAUDE]),
 ("a prompt telling the strokes marked addressed",
  [(D, "        if (r.value(QStringLiteral(\"addressed\")).toBool())\n            addressed <<", "        if (false)\n            addressed <<")], [CLAUDE]),
 ("a prompt not saying what a picture's measures are",
  [(D, "    if (!lines.isEmpty())\n        lines << QStringLiteral(\"(In the picture's pixels", "    if (false)\n        lines << QStringLiteral(\"(In the picture's pixels")], [CLAUDE]),
 ("an arrow's tip and tail not read",
  [(D, "                o.insert(QStringLiteral(\"tip\"), at(tips.at(i)));\n", "")], [CLAUDE]),
 ("a stroke's frame not read",
  [(D, "            if (byFrame) o.insert(QStringLiteral(\"frame\"), part.toInt());\n", "")], [FRAMES]),
 ("read_drawings not reading a picture",
  [(R, "        auto* picture = dynamic_cast<ImageDoc*>(doc);\n        if (layout == nullptr", "        ImageDoc* picture = nullptr;\n        if (layout == nullptr")], [CLAUDE]),
 ("read_drawings not saying a picture's measures",
  [(R, "                                                     : picture != nullptr\n", "                                                     : false\n")], [CLAUDE]),
 ("show_picture not taking the pen up",
  [(P, "        picture->penButton()->setChecked(pen.toBool());\n", "")], [CLAUDE]),
 ("show_picture not clearing the drawings",
  [(P, "        done << (view->clearDrawing() ?", "        done << (false ?")], [CLAUDE]),
 ("show_picture marking a stroke the picture has not",
  [(P, "            if (view->drawing().find(view->part(), id) == nullptr) missing << QString::number(id);", "            if (false) missing << QString::number(id);")], [CLAUDE]),
 ("show_picture not marking strokes addressed",
  [(P, "view->setAddressed(addressedIds, true));", "0);")], [CLAUDE]),
 ("show_picture not saying what was drawn",
  [(P, "        insertDrawings(o, picture->readDrawing(false));\n", "")], [CLAUDE]),
 ("send_input not reaching a picture",
  [(I, "    } else if (target.compare(QLatin1String(\"picture\"), Qt::CaseInsensitive) == 0) {", "    } else if (target.compare(QLatin1String(\"no picture\"), Qt::CaseInsensitive) == 0) {")], [CLAUDE]),
 ("send_input's points not a picture's pixels",
  [(I, "            if (pictureView) return pictureView->toViewport(p).toPoint();\n", "            if (pictureView) return p.toPoint();\n")], [CLAUDE]),
 ("send_input not saying what it drew on a picture",
  [(I, "(pictureView && pictureDoc) || page", "page")], [CLAUDE]),
 ("show_picture not in every turn while a picture's pen is up",
  [(C, "            out << QStringLiteral(\"show_picture\");\n", "")], [CLAUDE]),
 ("read_drawings not in every turn while a picture is drawn on",
  [(C, " || (picture != nullptr && !picture->view()->drawing().isEmpty())) {", ") {")], [CLAUDE]),
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


def ctestEnvironment(suite):
    # The suite's ENVIRONMENT as ctest gives it (qucs_add_test): offscreen,
    # claude and gh out of reach, the watchdog's timeout.
    shown = subprocess.run(["ctest", "--test-dir", B, "-R", "^%s$" % suite, "--show-only=json-v1"], capture_output=True, text=True,
                           stdin=subprocess.DEVNULL, check=True).stdout
    env = dict(os.environ)
    for test in json.loads(shown).get("tests", []):
        for prop in test.get("properties", []):
            if prop.get("name") == "ENVIRONMENT":
                for item in prop.get("value", []):
                    k, _, v = item.partition("=")
                    env[k] = v
    if env.get("QT_QPA_PLATFORM") != "offscreen":
        raise SystemExit("%s: ctest gives no offscreen platform; not run on the display" % suite)
    return env


def failedAt(log):
    lines = [l.strip() for l in open(log, encoding="utf-8", errors="replace") if l.startswith("FAIL!") or l.strip().startswith("Loc:")]
    for i, l in enumerate(lines):
        if l.startswith("FAIL!") and i + 1 < len(lines) and lines[i + 1].startswith("Loc:"):
            return lines[i + 1].rsplit("(", 1)[-1].rstrip(")]")
    return "?"


def main():
    os.makedirs(RUNS, exist_ok=True)
    if os.environ.get("BRK_ONLY_CHECK"):
        for name, edits, tests in BREAKS:
            apply(edits, None)
        print("%d breaks, every anchor once" % len(BREAKS))
        return 0
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    envs = {}
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
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL).returncode != 0:
                    results.append((i, name, "BUILD FAILED - see " + log))
                    continue
            said = []
            for suite, fn in tests:
                if suite not in envs:
                    envs[suite] = ctestEnvironment(suite)
                one = os.path.join(RUNS, "brk%03d-%s.log" % (i, fn))
                with open(one, "w") as out:
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out, stderr=subprocess.STDOUT,
                                           stdin=subprocess.DEVNULL, timeout=600, env=envs[suite], cwd=RUNS)
                        caught = r.returncode != 0
                        said.append("%s %s" % (fn[:28], "failed at line %s (caught)" % failedAt(one) if caught else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:28])
            results.append((i, name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for i, name, said in results:
        print("%2d %-60s %s" % (i, name[:60], said))
        if "BUILD FAILED" in said or "hung" in said or "(caught)" not in said:
            bad += 1
    print("\n%d of %d uncaught (a break with two tests is caught when either fails)" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
