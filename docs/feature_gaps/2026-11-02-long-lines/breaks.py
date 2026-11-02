#!/usr/bin/env python3
"""breaks.py: the labels and the chips that give way to long lines undone
on purpose, one piece at a time - ElidedLabel's least width, its cutting, its
drawing, its tool tip, its rich text and word wrap; ElidedToolButton's least
width, size policy, drawing and cut; each label of the tabs and of the
Claude Code panel, and its two chips, made plain again; and a layout's box
dragged up or to the left as it was - each built and its tests run.
Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/long-lines-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

EL = "elidedlabel.cpp"
T = "test_long_lines"
UNIT = "aLongLineIsCutAndWholeInItsTip"
WIN = "aTabsLongLineLeavesTheWindowAsItWas"
CHIP = "aLongNameIsCutInItsButton"
NAME = "aDocumentsLongNameLeavesTheWindowAsItWas"

def plain(path, member, parent, name):
    """The label \\a member of \\a path made a plain QLabel again."""
    return ("%s a QLabel again" % name,
            [(path, "    %s = new ElidedLabel(%s);" % (member, parent), "    %s = new QLabel(%s);" % (member, parent))],
            [(T, WIN)])

BREAKS = [
 # ---- ElidedLabel's least width
 ("the least width the whole text's, as a QLabel asks",
  [(EL, "    return QSize(std::min(whole.width(), least), whole.height());\n}\n\nbool ElidedLabel::event",
    "    return whole;\n}\n\nbool ElidedLabel::event")],
  [(T, UNIT), (T, WIN)]),

 ("the least width a few characters, whatever the text",
  [(EL, "    return QSize(std::min(whole.width(), least), whole.height());\n}\n\nbool ElidedLabel::event",
    "    return QSize(least, whole.height());\n}\n\nbool ElidedLabel::event")],
  [(T, UNIT)]),

 ("the least width without the margins and frame around the text",
  [(EL, "    const int least = width() - room().width() + fontMetrics().averageCharWidth() * 4;",
    "    const int least = fontMetrics().averageCharWidth() * 4;")],
  [(T, UNIT)]),

 # ---- its cutting and drawing
 ("never cut",
  [(EL, "    return !wordWrap() && !text().isEmpty() && QLabel::sizeHint().width() > width();", "    return false;")],
  [(T, UNIT), (T, WIN)]),

 ("drawn whole, not cut",
  [(EL, "    if (!elided()) {\n        QLabel::paintEvent(event);", "    if (true) {\n        QLabel::paintEvent(event);")],
  [(T, UNIT)]),

 ("cut to the label's width, not the room inside its margins",
  [(EL, "fontMetrics().elidedText(plain(), a_mode, std::max(0, room().width()))",
    "fontMetrics().elidedText(plain(), a_mode, std::max(0, width()))")],
  [(T, UNIT)]),

 ("drawn from the label's edge, not inside its margins",
  [(EL, "    style()->drawItemText(&painter, room(), ", "    style()->drawItemText(&painter, rect(), ")],
  [(T, UNIT)]),

 # ---- its tool tip
 ("the whole text not in the tool tip",
  [(EL, "    if (event->type() == QEvent::ToolTip && elided()) {", "    if (false) {")],
  [(T, UNIT)]),

 ("the tool tip without the label's own",
  [(EL, "        if (!toolTip().isEmpty())\n", "        if (false)\n")],
  [(T, UNIT)]),

 # ---- rich text and word wrap
 ("rich text cut as its markup",
  [(EL, "    QString out = rich() ? QTextDocumentFragment::fromHtml(text()).toPlainText() : text();",
    "    QString out = text();")],
  [(T, UNIT)]),

 ("cut with word wrap too",
  [(EL, "    return !wordWrap() && !text().isEmpty() && QLabel::sizeHint().width() > width();",
    "    return !text().isEmpty() && QLabel::sizeHint().width() > width();")],
  [(T, UNIT)]),

 ("word wrap's least width a few characters too",
  [(EL, "    if (wordWrap()) return whole;\n", "")],
  [(T, UNIT)]),

 # ---- the document chips' button
 ("the chip's least width its whole name",
  [(EL, "    return QSize(std::min(whole.width(), least), whole.height());\n}\n\nvoid ElidedToolButton",
    "    return whole;\n}\n\nvoid ElidedToolButton")],
  [(T, CHIP), (T, NAME)]),

 ("the chip's width fixed, as a tool button's is",
  [(EL, "    policy.setHorizontalPolicy(QSizePolicy::Maximum);\n", "")],
  [(T, CHIP), (T, NAME)]),

 ("the chip drawn whole, not cut",
  [(EL, "    if (!elided()) {\n        QToolButton::paintEvent(event);", "    if (true) {\n        QToolButton::paintEvent(event);")],
  [(T, CHIP)]),

 ("the chip's name cut at its end, its kind lost",
  [(EL.replace(".cpp", ".h"), "explicit ElidedToolButton(QWidget* parent = nullptr, Qt::TextElideMode mode = Qt::ElideMiddle);",
    "explicit ElidedToolButton(QWidget* parent = nullptr, Qt::TextElideMode mode = Qt::ElideRight);")],
  [(T, CHIP)]),

 # ---- the labels
 plain("rf25doc.cpp", "a_status", "this", "the RF 2.5D status (the screenshots' line)"),
 plain("rf25doc.cpp", "a_layoutLabel", "bar", "the RF 2.5D layout line"),
 plain("layoutdoc.cpp", "a_cellLabel", "a_toolbar", "the layout viewer's cell"),
 plain("layoutdoc.cpp", "a_info", "infoBar", "the layout viewer's info line (ignored width)"),
 plain("multiphysicsdoc.cpp", "a_status", "this", "the multiphysics status"),
 plain("imagedoc.cpp", "a_factsLabel", "a_infoBar", "the image's facts (ignored width)"),
 plain("xmldoc.cpp", "a_status", "a_bar", "the XML status"),
 plain("zipdoc.cpp", "a_status", "this", "the archive's status"),
 plain("claudecodepanel.cpp", "a_stateText", "a_header", "the Claude Code state"),
 plain("claudecodepanel.cpp", "a_modelLabel", "a_header", "the Claude Code model"),
 ("the Claude Code document chip a QToolButton again",
  [("claudecodepanel.cpp", "    a_attach = new ElidedToolButton(a_composer);", "    a_attach = new QToolButton(a_composer);")],
  [(T, NAME)]),
 ("the Claude Code pin chip a QToolButton again",
  [("claudecodepanel.cpp", "    a_pin = new ElidedToolButton(a_composer);", "    a_pin = new QToolButton(a_composer);")],
  [(T, NAME)]),

 # ---- what fixing it showed: a box dragged up or to the left
 ("a layout's box dragged up or left two pixels short, as QRect::normalized() leaves it",
  [("layoutdoc.cpp", "        a_zoomBox = QRect(QPoint(std::min(a_pressPos.x(), pos.x()), std::min(a_pressPos.y(), pos.y())),\n"
                     "                          QPoint(std::max(a_pressPos.x(), pos.x()), std::max(a_pressPos.y(), pos.y())));",
    "        a_zoomBox = QRect(a_pressPos, pos).normalized();")],
  [("test_layout_doc", "aPickingModeSaysWhatIsPickedAndSelectsNothing"), ("test_rf25_doc", "aPortIsPlacedByHandOnTheLayout")]),
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
