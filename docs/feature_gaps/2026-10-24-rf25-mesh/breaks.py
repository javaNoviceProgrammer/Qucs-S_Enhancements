#!/usr/bin/env python3
"""breaks.py: each part of the mesh undone on purpose, to see its test
fail. Sources are put back whatever happens. One of these takes the guard
off a loop that would then try to build ten billion cells, so every test is
run under a timeout and a hang counts as caught - that is the very failure
the guard is there to prevent.
BRK_ONLY_CHECK=1 checks the anchors alone; HUNT_RUNS=<dir> for the logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-mesh-breaks")
os.makedirs(RUNS, exist_ok=True)
T = "test_rf25_mesh"

BREAKS = [
 ("the wavelength does not decide the cell size",
  [("rf25/rf25_mesh.cpp", "        size = wavelength / how.perWavelength / perUnit;",
    "        size = 1;")],
  ["theCellSizeComesFromTheWavelength"]),

 ("the largest cell allowed is not a limit",
  [("rf25/rf25_mesh.cpp", "        if (size <= 0 || most < size) {", "        if (size <= 0) {")],
  ["theCellSizeComesFromTheWavelength"]),

 ("a mesh with nothing to go on guesses a size",
  [("rf25/rf25_mesh.cpp",
    '        mesh.problems << tr("Nothing says how large a cell may be: give the study a frequency, or the mesh a largest "\n                            "cell.");\n        return mesh;',
    '        size = 1;')],
  ["theCellSizeComesFromTheWavelength"]),

 ("the grid is not cut against the conductor",
  [("rf25/rf25_mesh.cpp",
    "                const QList<QPolygonF> pieces = clipTo({conductor.outline}, QPolygonF(square));",
    "                const QList<QPolygonF> pieces = QList<QPolygonF>{QPolygonF(square)};")],
  ["theConductorsAreCutIntoCells"]),   # (the L-shaped conductor sees it; a square on the grid cannot)

 ("the edge refinement runs whether it was asked for or not",
  [("rf25/rf25_mesh.cpp", "    if (how.edges) {", "    if (true) {")],
  ["theCellsAtAnEdgeAreDividedAgain"]),

 ("every cell is divided again, not only those at an edge",
  [("rf25/rf25_mesh.cpp",
    "            if (!cell.onEdge || box.width() <= size / 4 || box.height() <= size / 4) {",
    "            if (box.width() <= size / 4 || box.height() <= size / 4) {")],
  ["theCellsAtAnEdgeAreDividedAgain"]),

 ("a cell at an edge is not marked as one",
  [("rf25/rf25_mesh.cpp",
    "                    cell.onEdge = std::abs(area - dx * dy) > dx * dy * 1e-6\n                                  || i == 0 || j == 0 || i == across - 1 || j == up - 1;",
    "                    cell.onEdge = false;")],
  ["theCellsAtAnEdgeAreDividedAgain"]),

 ("every edge is counted as an unknown, not only a shared one",
  [("rf25/rf25_mesh.cpp", "        if (it.value() >= 2) ++unknowns;", "        ++unknowns;")],
  ["theUnknownsAreCountedAndTheirCostSaid"]),

 ("a mesh beyond what this build holds is not called too big",
  [("rf25/rf25_mesh.cpp", "    cost.tooBig = unknowns > kMostUnknowns;", "    cost.tooBig = false;")],
  ["theUnknownsAreCountedAndTheirCostSaid"]),

 ("a conductor that would take ten billion cells is cut all the same",
  [("rf25/rf25_mesh.cpp", "        if (double(across) * double(up) > 4.0 * kMostUnknowns) {", "        if (false) {")],
  ["theUnknownsAreCountedAndTheirCostSaid"]),

 ("too few cells to a wavelength is taken as given",
  [("rf25/rf25_mesh.cpp", "    if (how.perWavelength < 4) {", "    if (false) {")],
  ["theMeshSettingsAreReadAndChecked"]),

 ("a largest cell below zero is taken as given",
  [("rf25/rf25_mesh.cpp", "    if (how.maximum < 0) {", "    if (false) {")],
  ["theMeshSettingsAreReadAndChecked"]),
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
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B, T], stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for fn in tests:
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", T), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=180,
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0"))
                        said.append("%s %s" % (fn[:30], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung (caught: that is what the check prevents)" % fn[:30])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-58s %s" % (name[:58], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
