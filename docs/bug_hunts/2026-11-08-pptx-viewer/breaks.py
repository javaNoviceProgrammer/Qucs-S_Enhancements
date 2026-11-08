#!/usr/bin/env python3
"""breaks.py <worktree> <tree>: each fix of the session undone in turn, the
viewer's tests built and run through ctest; which of them fail. The file is
put back after each."""
import os, re, subprocess, sys

W, TREE = sys.argv[1], sys.argv[2]
Q = os.path.join(W, "qucs-s-26.1.1", "qucs")

BREAKS = [
    ("the SVG in a blip's extensions ignored", "pptx.cpp",
     "if (ext->attr(u\"uri\").compare(kSvgBlipExtension, Qt::CaseInsensitive) != 0) continue;",
     "continue;"),
    ("a broken SVG not fallen back from", "pptx.cpp",
     "    if (svg != nullptr && svg->drawable()) return svg;",
     "    if (svg != nullptr) return svg;"),
    ("a slide drawn whole at any size (no tiles)", "pptxdoc.cpp",
     "    if (qint64(full.width()) * full.height() <= WholeSlidePixels) return {QRect(QPoint(0, 0), full)};",
     "    return {QRect(QPoint(0, 0), full)};"),
    ("a zoom lets the slide shown slip", "pptxdoc.cpp",
     "    if (a_current != keep && keep < (a_presentation != nullptr ? a_presentation->slideCount() : 0)) {",
     "    if (false) {"),
    ("thumbnails as tall as the slide's aspect", "pptxdoc.cpp",
     "int(std::clamp(ThumbnailWidth * s.height() / s.width(), 20.0, 4.0 * ThumbnailWidth))",
     "int(std::max(20.0, ThumbnailWidth * s.height() / s.width()))"),
    ("pictures decoded far smaller than drawn", "pptx.cpp",
     "while (level < 8 && atLeast.isValid() && shrunk.width() / 2 >= std::max(1, atLeast.width()) && shrunk.height() / 2 >= std::max(1, atLeast.height())) {",
     "while (level < 8 && atLeast.isValid() && shrunk.width() / 2 >= 1 && shrunk.height() / 2 >= 1) {"),
    ("a tile drawn at the slide's top left", "pptx.cpp",
     "        p.translate(-area.topLeft());\n        p.scale(scale, scale);",
     "        p.scale(scale, scale);"),
    ("layouts let go and not made again", "pptx.cpp",
     "        if (a_doc != nullptr && qFuzzyCompare(a_width + 1, width + 1)) return a_doc.get();",
     "        if (a_doc != nullptr && qFuzzyCompare(a_width + 1, width + 1)) return a_doc.get();\n        if (a_width > -2) { static QTextDocument none; return &none; }"),
    ("slide sizes not held", "pptx.cpp",
     "    d.size = QSizeF(std::clamp(d.size.width(), 1.0, kMaxSlideSide), std::clamp(d.size.height(), 1.0, kMaxSlideSide));",
     ""),
]


def run():
    env = dict(os.environ, PATH=subprocess.run(["brew", "--prefix", "bison"], capture_output=True, text=True).stdout.strip() + "/bin:" + os.environ["PATH"])
    b = subprocess.run(["ninja", "-C", TREE, "test_pptx_doc"], capture_output=True, text=True, env=env)
    if b.returncode != 0:
        return "does not build: " + " ".join(l for l in b.stdout.splitlines() if "error" in l)[:300]
    t = subprocess.run(["ctest", "--test-dir", TREE, "-R", "test_pptx_doc", "--output-on-failure"], capture_output=True, text=True, env=env)
    fails = sorted(set(re.findall(r"FAIL!  : TestPptxDoc::(\w+)\(\)", t.stdout)))
    if "Passed" in t.stdout and not fails:
        return "NOT CAUGHT (all pass)"
    return "caught by " + ", ".join(fails) if fails else "failed: " + t.stdout[-300:]


for name, file, old, new in BREAKS:
    path = os.path.join(Q, file)
    text = open(path).read()
    if old not in text:
        print("%-45s: the text to undo is not there" % name, flush=True)
        continue
    try:
        open(path, "w").write(text.replace(old, new, 1))
        print("%-45s: %s" % (name, run()), flush=True)
    finally:
        open(path, "w").write(text)
print("restored:", run(), flush=True)
