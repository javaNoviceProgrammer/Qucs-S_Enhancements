#!/usr/bin/env python3
"""breaks.py: RF 2.5D's shapes drawn on its tab and its circuit fitted together
on S (11 November, F4 and F6 of the open items of 8 November) undone on
purpose, one piece at a time - the file begun and named, the map's row, Undo
of the file and the model, the snapping, the polygon's keys and clicks,
Delete, the tools one at a time, the view kept, the layer chosen, the exact
pick of a shape, rf25_draw's answer; the joint fit kept, its candidates, its
choice of the fewest values, the symmetric two-port, what it says - each
built and its tests run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=3 runs that
alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the checkout's
build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-draw-and-fit-breaks")

F = "rf25/rf25_circuit.cpp"
R = "rf25doc_draw.cpp"
D = "rf25doc.cpp"
E = "layoutedit.cpp"
C = "qucscontrol_rf25.cpp"
JOINT = ("test_rf25_sparams", "theCircuitIsFittedTogetherOnS")
CAP = ("test_rf25_sparams", "aSlightlyLossyCapacitorIsACapacitor")
PI = ("test_rf25_sparams", "aPiOfKnownBranchesIsFoundAgain")
CIRCUIT = ("test_qucs_control", "anRf25ModelIsSolvedFullWaveThroughItsTool")
DRAW = ("test_rf25_doc", "aModelsShapesAreDrawnOnItsTab")
TOOL = ("test_qucs_control", "anRf25ModelIsDrawnThroughItsTool")

BREAKS = [
 # F6, the circuit fitted together on S
 ("the joint fit never kept",
  [(F, "        if (kept != nullptr && kept->rms < 0.98 * c.branchwiseRms) {", "        if (false) {")], [JOINT]),
 ("the joint fit's choice not of the fewest values nearly as good",
  [(F, "            if (rms > std::max(1.25 * best, best + 1e-3)) continue;", "            if (rms > best) continue;")], [CAP]),
 ("only the branches' own forms fitted together",
  [(F, "    if (chosen.values.isEmpty()) return out;\n", "    return out;\n")], [JOINT]),
 ("a symmetric two-port's branches to the ground not kept alike",
  [(F, "        bool symmetric = n == 2 && c.toGround[0].form == c.toGround[1].form;", "        bool symmetric = false;")], [JOINT]),
 ("S's imaginary part left out of the joint misfit",
  [(F, "                r(at++) = std::isfinite(d.imag()) ? d.imag() : 1e6;", "                r(at++) = 0;")], [JOINT]),
 ("the branches' own rms not kept beside the joint one",
  [(F, "            c.branchwiseRms = was.branchwiseRms;", "            c.branchwiseRms = c.rms;")], [JOINT]),
 ("the joint fit not said",
  [(F, "    if (c.joint)\n        c.lines << tr(\"Its values fitted together", "    if (false)\n        c.lines << tr(\"Its values fitted together")], [JOINT]),
 ("rf25_circuit not saying how it was fitted",
  [(C, "    out.insert(QStringLiteral(\"fitted\"), c->joint", "    out.insert(QStringLiteral(\"fitted_\"), c->joint")], [CIRCUIT]),
 # F4, the shapes drawn
 ("no file named by the model's Layout when one is begun",
  [(R, "        m.layout().props.insert(QStringLiteral(\"file\"), name);\n", "")], [DRAW, TOOL]),
 ("no row of the map added for a layer drawn on",
  [(R, "        rows.append(QJsonArray{QString::number(next), QStringLiteral(\"0\"), layer.name});\n", "")], [DRAW, TOOL]),
 ("Undo not taking a begun file away",
  [(R, "    } else {\n        QFile::remove(path);\n    }", "    }")], [DRAW]),
 ("Undo not putting the model back",
  [(R, "    if (model.toObject() != a_model.toObject()) applyModel(model);\n    refreshLayout();", "    refreshLayout();")], [DRAW]),
 ("a drawing not a step of Undo",
  [(R, "    if (undoable) {\n        QPointer<RF25Doc> self = this;", "    if (false) {\n        QPointer<RF25Doc> self = this;")], [DRAW, TOOL]),
 ("a rectangle's corners not snapped",
  [(D, "const QRectF r = QRectF(drawSnapped(box.topLeft()), drawSnapped(box.bottomRight())).normalized();",
       "const QRectF r = QRectF(box.topLeft(), box.bottomRight()).normalized();")], [DRAW]),
 ("a corner not snapped to a shape's",
  [(R, "        if (QLineF(s, onGrid).length() > 0.5 * dbu) return s;\n", "")], [DRAW]),
 ("Backspace not taking a corner back",
  [(R, "                a_polygon.removeLast();\n", "")], [DRAW]),
 ("the application's shortcuts not told the keys are the tab's",
  [(R, "        if (polygon || selected) {\n            key->accept();", "        if (false) {\n            key->accept();")], [DRAW]),
 ("a double click not closing a polygon",
  [(R, "event->type() == QEvent::MouseButtonDblClick && a_drawTool == DrawTool::Polygon",
       "event->type() == QEvent::MouseButtonDblClick && a_drawTool == DrawTool::None")], [DRAW]),
 ("a click on the first corner not closing a polygon",
  [(R, "    if (a_polygon.size() >= 3 && QLineF(p, a_polygon.first()).length() <= reach) {", "    if (false) {")], [DRAW]),
 ("Delete not deleting the shape selected",
  [(R, "            if (!deleteSelectedShape(&why)) a_status->setText(why);\n            return true;\n        }\n    }",
       "            return true;\n        }\n    }")], [DRAW]),
 ("a deletion by a shape's own box taking any shape of its layer",
  [(E, "        if (exact && !same(el.box)) return;\n", "")], [DRAW]),
 ("a port tool not putting the drawing tool down",
  [(D, "    if (placing != Placing::None && a_drawTool != DrawTool::None) dropDrawTool();\n", "")], [DRAW]),
 ("Escape not putting the drawing tool down",
  [(D, "        if ((mode == LayoutView::Mode::Select || mode == LayoutView::Mode::Draw) && a_drawTool != DrawTool::None) dropDrawTool();\n", "")], [DRAW]),
 ("the view fitted to the layout read after each drawing",
  [(D, "    if (again || a_keepView) a_view->setView(center, scale);", "    if (again) a_view->setView(center, scale);")], [DRAW]),
 ("the via chosen in the stack not the layer drawn on",
  [(R, "    if (drawable(stack.named(a_focus))) return stack.named(a_focus)->name;\n", "")], [DRAW]),
 ("a via drawn on a metal",
  [(R, "        if (via && l->kind != Layer::Via) {", "        if (false) {")], [DRAW]),
 ("rf25_draw answering before the layout is read again",
  [(C, "    const bool laidOut = model->waitForLayout(120000);\n    if (model->closedWhenFree()) return closedWhileWaited(heldName);\n    const bool prepared",
       "    const bool laidOut = true;\n    const bool prepared")], [TOOL]),
 ("rf25_draw not saying what the processing leaves",
  [(C, "        out.insert(QStringLiteral(\"processed shapes by layer\"), layers);\n", "")], [TOOL]),
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
                                           stdin=subprocess.DEVNULL, timeout=1200, env=env, cwd=RUNS)
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
