#!/usr/bin/env python3
"""breaks.py: the ports on the layout undone on purpose, one at a time -
drawn by their names, placed by hand with the Edge Port and Internal Port
tools, an internal port over a rectangle, a node named past the highest of
its stem - each built and its tests run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those
alone; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/person/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/person/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-ports-breaks")
os.makedirs(RUNS, exist_ok=True)

V = "layoutdoc.cpp"
D = "rf25doc.cpp"
I = "rf25/rf25_inductance.cpp"
FW = "rf25/rf25_fullwave.cpp"
M = "rf25/rf25_model.cpp"
TL, TD, TF, TM = "test_layout_doc", "test_rf25_doc", "test_rf25_fullwave", "test_rf25_model"
PICK = "aPickingModeSaysWhatIsPickedAndSelectsNothing"
DRAWN = "thePortsAreDrawnByTheirNames"
HAND = "aPortIsPlacedByHandOnTheLayout"
RECT = "anInternalPortOverARectangleDrivesItsCells"
NAMED = "aNewNodeIsNamedPastTheHighestOfItsStem"

BREAKS = [
 # ---- the view: labels and the picking modes
 ("an overlay's labels not written",
  [(V, "    if (overlay.labels.isEmpty()) return;\n    const QColor ink", "    return;\n    const QColor ink")],
  [(TL, PICK)]),
 ("a click in PickPoint selecting, not picked",
  [(V, "            if (a_mode == Mode::PickPoint || a_mode == Mode::PickBox) emit picked(toLayout(event->position()));",
    "            if (false) emit picked(toLayout(event->position()));")],
  [(TL, PICK)]),
 ("a drag in PickBox panning, not a box",
  [(V, "    if (event->button() == Qt::LeftButton && a_mode == Mode::PickBox) a_drag = Drag::Box;",
    "    if (false) a_drag = Drag::Box;")],
  [(TL, PICK)]),
 ("Escape leaving a picking mode as it is",
  [(V, "        } else if (a_mode != Mode::Select) {\n            setMode(Mode::Select);",
    "        } else if (a_mode == Mode::Ruler) {\n            setMode(Mode::Select);")],
  [(TL, PICK), (TD, HAND)]),
 # ---- the ports drawn
 ("an edge port's face found facing any way",
  [(I, "            if (facingNorm > 0 && (normal.x() * facing.x() + normal.y() * facing.y()) / facingNorm < M_SQRT1_2) continue;",
    "            (void)facingNorm;")],
  [(TD, DRAWN)]),
 ("a coplanar port's grounds not found beside it",
  [(I, "            if (t1 <= tol) sides[0] << std::make_pair(-t1, -t0);", "            if (false) sides[0] << std::make_pair(-t1, -t0);")],
  [(TD, DRAWN)]),
 ("the ports drawn whether shown or not",
  [(D, "    if (a_showPorts && a_processed) {\n        const bool dark", "    if (a_processed) {\n        const bool dark")],
  [(TD, DRAWN)]),
 ("a port put not drawn till the next preparing",
  [(D, "    // (A port added, moved or taken away is drawn at once, before any\n    //  preparing it may bring.)\n    showOverlays();\n",
    "")],
  [(TD, HAND)]),
 # ---- placed by hand
 ("an edge port put facing no way",
  [(D, "    if (std::abs(outward.x()) > 0.999) facing = outward.x() > 0 ? QStringLiteral(\"+x\") : QStringLiteral(\"-x\");",
    "    if (false) facing = outward.x() > 0 ? QStringLiteral(\"+x\") : QStringLiteral(\"-x\");")],
  [(TD, HAND)]),
 ("a second edge port put on an edge one drives",
  [(D, "        if (faceEdge(metals, p.at, p.metal, p.facing, &theirs) && theirs == edge)",
    "        if (false && faceEdge(metals, p.at, p.metal, p.facing, &theirs) && theirs == edge)")],
  [(TD, HAND)]),
 ("an edge port put however far the click is from an edge",
  [(D, "    if (std::hypot(at.x() - onEdge.x(), at.y() - onEdge.y()) > reach)", "    if (false)")],
  [(TD, HAND)]),
 ("an internal port's rectangle not written",
  [(D, "        port.props.insert(QStringLiteral(\"width\"), QStringLiteral(\"%1[um]\").arg(r.width(), 0, 'g', 9));", "")],
  [(TD, HAND)]),
 ("the two tools in hand together",
  [(D, "        a_edgePortButton->setChecked(placing == Placing::EdgePort);", "        (void)0;")],
  [(TD, HAND)]),
 # ---- an internal port over a rectangle
 ("a rectangle's port driving the one cell under its point",
  [(FW, "            const bool over = port.size.width() > 0 && port.size.height() > 0;", "            const bool over = false;")],
  [(TF, RECT)]),
 ("a rectangle's cells driven alike, not by their share of it",
  [(FW, "    for (std::pair<int, double>& each : out) each.second /= total;", "    for (std::pair<int, double>& each : out) each.second = 1.0 / out.size();")],
  [(TF, RECT)]),
 # ---- names
 ("a node named among its own kind alone",
  [(M, "            if (node.label.startsWith(stem)) {", "            if (node.type == QStringLiteral(\"rf_metal\") && node.label.startsWith(stem)) {")],
  [(TM, NAMED), (TD, DRAWN)]),
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
        print("%-66s %s" % (name[:66], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
