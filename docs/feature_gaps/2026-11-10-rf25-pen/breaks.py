#!/usr/bin/env python3
"""breaks.py: the pen on RF 2.5D's tab (10 November) undone on purpose, one
piece at a time - its button at the toolbar's end, taking it up and its menu,
the pen and the port tools one at a time, and what reaches Claude: a prompt's
note, read_drawings, show_layout's pen, drawings and addressed, and send_input's
target layout - each built and its tests run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=3,7
runs those alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the
checkout's build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-pen-breaks")

D = "rf25doc.cpp"
A = "qucs.cpp"
R = "qucscontrol_pptx.cpp"
S = "qucscontrol_layout.cpp"
I = "qucscontrol_input.cpp"
PEN = ("test_qucs_control", "anRf25ModelsPenIsReadThroughTheTools")

BREAKS = [
 # ---- The tab
 ("the pen not in the toolbar",
  [(D, "    row->addWidget(a_penButton);\n", "")], [PEN]),
 ("the pen taken up, the view not drawing",
  [(D, "        a_view->setMode(on ? LayoutView::Mode::Draw : LayoutView::Mode::Select);\n        a_view->setFocus();\n    });\n    a_penButton->setColorChooser",
       "        a_view->setFocus();\n    });\n    a_penButton->setColorChooser")], [PEN]),
 ("a port tool taken up, the pen left pressed",
  [(D, "        a_penButton->setChecked(mode == LayoutView::Mode::Draw);\n", "")], [PEN]),
 ("the pen taken up, a port tool left pressed",
  [(D, "        if (on && a_placing != Placing::None) setPlacing(Placing::None);\n", "")], [PEN]),
 ("the pen's menu not told of the drawing",
  [(D, "    connect(a_view, &LayoutView::drawingChanged, this, &RF25Doc::updatePen);\n", "")], [PEN]),
 # ---- What reaches Claude
 ("a prompt not told what was drawn",
  [(A, "    if (auto *model = dynamic_cast<RF25Doc *>(doc)) {", "    if (auto *model = (RF25Doc *)nullptr) {")], [PEN]),
 ("read_drawings not reading a model",
  [(R, "        auto* model = dynamic_cast<RF25Doc*>(doc);   // (its", "        RF25Doc* model = nullptr;   // (its")], [PEN]),
 ("show_layout not taking a model's pen up",
  [(S, "            if (pen.isBool()) model->penButton()->setChecked(pen.toBool());\n", "")], [PEN]),
 ("show_layout not clearing a model's drawings",
  [(S, "            const bool cleared = drawings == QLatin1String(\"clear\") && view->clearDrawing(true);\n            int marked",
       "            const bool cleared = false;\n            int marked")], [PEN]),
 ("show_layout marking a stroke a model has not",
  [(S, "                    if (view->drawing().find(view->drawingPart(), id) == nullptr) missing << QString::number(id);\n                if (!missing.isEmpty()) {\n                    for (const qucs_s::markup::Stroke& s : view->drawing().on(view->drawingPart())) there << QString::number(s.id);\n                    done(errorResult(tr(\"The cell shown has no stroke %1 (its strokes: %2).\")\n                                         .arg(",
       "                    if (false) missing << QString::number(id);\n                if (!missing.isEmpty()) {\n                    for (const qucs_s::markup::Stroke& s : view->drawing().on(view->drawingPart())) there << QString::number(s.id);\n                    done(errorResult(tr(\"The cell shown has no stroke %1 (its strokes: %2).\")\n                                         .arg(")], [PEN]),
 ("show_layout not saying a model's drawings",
  [(S, "            // What the user drew with the pen, read as on a layout's tab.\n            if (!view->drawing().isEmpty()) insertDrawings(result, view->readDrawing(false));\n", "")], [PEN]),
 ("send_input not drawing on a model",
  [(I, "            model = dynamic_cast<RF25Doc*>(a_app->getDoc());", "            model = nullptr;")], [PEN]),
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


def main():
    os.makedirs(os.path.join(RUNS, "cache"), exist_ok=True)
    if os.environ.get("BRK_ONLY_CHECK"):
        for name, edits, tests in BREAKS:
            apply(edits, None)
        print("%d breaks, every anchor once" % len(BREAKS))
        return 0
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
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
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    home = os.path.join(RUNS, "home")
                    os.makedirs(home, exist_ok=True)
                    env = dict(os.environ, HOME=home, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out, stderr=subprocess.STDOUT,
                                           stdin=subprocess.DEVNULL, timeout=900, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-62s %s" % (name[:62], said))
        if "BUILD FAILED" in said or "hung" in said or "caught)" not in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
