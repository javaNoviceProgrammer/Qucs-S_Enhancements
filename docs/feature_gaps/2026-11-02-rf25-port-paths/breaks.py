#!/usr/bin/env python3
"""breaks.py: G9's fix undone on purpose, one piece at a time - a port's own
path up from the ground and the field it has (its potential along the
runs, the vias' G_zz along it, its own and the other paths' G_zz and
potentials, in the dense solve and the compressed), how it is made (where
it stands, what it is as wide as, which way its current goes, what metal
it comes from), and what is said of the stack's second surface wave now
it is no cause - each built and its tests run. Sources are put back
whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-port-paths-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

FW = "rf25/rf25_fullwave.cpp"
DE = "rf25/rf25_deembed.cpp"
QC = "qucscontrol_rf25.cpp"
TF, TD, TC = "test_rf25_fullwave", "test_rf25_deembed", "test_qucs_control"
VIA = "aViaToTheGroundIsItsOwnInductance"
CLIMB = "aViaBetweenTwoMetalsJoinsThem"
MONO = "aPortsPathRadiatesAsAMonopole"
VAC = "aStripOverVacuumGivesNothingBack"
PAST = "pastTheSecondSurfaceWaveAStripIsPassive"
RECT = "anInternalPortOverARectangleDrivesItsCells"
COMP = "aCompressedSolveIsTheDenseOne"
LONG = "aLongStripsPhaseIsFollowedFromBelow"
THIRD = "aThirdStripSaysWhereTheStripsAreOneLine"
TOOL = "anRf25ModelIsSolvedFullWaveThroughItsTool"

BREAKS = [
 # ---- the paths' field
 ("no port has a path: addPortPaths adds nothing",
  [(FW, "    VectorC at(c.nc);\n    for (const PortPath& path : c.paths) {",
    "    if (count >= 0) return;\n    VectorC at(c.nc);\n    for (const PortPath& path : c.paths) {")],
  [(TF, MONO), (TF, VAC), (TF, PAST), (TF, VIA), (TF, CLIMB)]),
 ("a path's potential along the runs left out",
  [(FW, "        for (int a = 0; a < c.planar; ++a) rhs(a, path.column) -= path.current * (at[c.head.at(a)] - at[c.tail.at(a)]);\n", "")],
  [(TF, VIA), (TF, CLIMB)]),
 ("the vias' G_zz along a path left out",
  [(FW, "        for (const Via& via : c.vias) rhs(via.run, path.column) -= path.current * k.couplingOf(path.via, via);\n", "")],
  [(TF, VIA), (TF, CLIMB)]),
 ("a via's G_zz along a path averaged both ways round, its potential one way",
  [(FW, "rhs(via.run, path.column) -= path.current * k.couplingOf(path.via, via);",
    "rhs(via.run, path.column) -= path.current * 0.5 * (k.couplingOf(path.via, via) + k.couplingOf(via, path.via));")],
  [(TF, CLIMB)]),
 ("the paths' own G_zz left out of the ports' Z",
  [(FW, "* (k.couplingOf(a.via, b.via) + k.couplingOf(b.via, a.via) + along(i, j) + along(j, i));",
    "* (along(i, j) + along(j, i));")],
  [(TF, MONO), (TF, VIA), (TF, CLIMB)]),
 ("the paths' potentials at each other's ends left out",
  [(FW, "* (k.couplingOf(a.via, b.via) + k.couplingOf(b.via, a.via) + along(i, j) + along(j, i));",
    "* (k.couplingOf(a.via, b.via) + k.couplingOf(b.via, a.via));")],
  [(TF, VIA), (TF, CLIMB)]),
 ("the paths' own left out of the dense solve's Z",
  [(FW, "    out.z = c.drive.transpose() * pDrive / jw + pathZ - rhs.transpose() * out.current;",
    "    out.z = c.drive.transpose() * pDrive / jw - rhs.transpose() * out.current;")],
  [(TF, MONO), (TF, VIA)]),
 ("the compressed solve given no paths",
  [(FW, "    MatrixC pathZ = MatrixC::Zero(columns, columns);\n    addPortPaths(c, k, rhs, pathZ);",
    "    MatrixC pathZ = MatrixC::Zero(columns, columns);")],
  [(TF, COMP)]),
 # ---- how a path is made
 ("an internal port's path from the ground whatever metal it is driven from",
  [(FW, "            if (c.drive(i, k).real() < 0) {", "            if (false && c.drive(i, k).real() < 0) {")],
  [(TF, CLIMB)]),
 ("a path's current up whichever way its port drives it",
  [(FW, "            path.current = up ? share : -share;", "            path.current = share;")],
  [(TF, CLIMB)]),
 ("a path from a metal with no bottom end on it",
  [(FW, "            for (const QPair<int, double>& end : std::as_const(from))\n                path.ends << qMakePair(end.first, (up ? -1.0 : 1.0) * end.second / fromWeight);\n", "")],
  [(TF, CLIMB)]),
 ("an edge port's path its cells' boxes, not a ribbon along its face",
  [(FW, "? ribbon(shares.at(k) * faceWidth)", "? boxOf(mesh.cells.at(face.at(k)).outline.boundingRect())")],
  [(TF, RECT)]),
 ("an internal port's path over a rectangle the whole cell's box",
  [(FW, "boxOf(over ? box.intersected(rect) : box)", "boxOf(box)")],
  [(TF, VIA), (TF, CLIMB)]),
 ("a path standing 10 µm off its cell's centre",
  [(FW, "            path.via.at = c.centre.at(node);", "            path.via.at = c.centre.at(node) + QPointF(10e-6, 0);")],
  [(TF, VIA), (TF, CLIMB)]),
 ("a path's mean distance with itself a square's of its side",
  [(FW, "            path.via.gmd = it.value().second;", "            path.via.gmd = 0.447 * it.value().first;")],
  [(TF, VIA)]),
 ("the paths not said",
  [(FW, "    if (!c.paths.isEmpty())\n        out.lines << tr(\"The ports' currents", "    if (false)\n        out.lines << tr(\"The ports' currents")],
  [(TF, VAC)]),
 # ---- the second surface wave, no cause now
 ("past the second surface wave, a warning again",
  [(FW, "    if (out.secondWave > 0 && top >= out.secondWave)\n        out.lines << tr(\"From %1 GHz the stack carries",
    "    if (out.secondWave > 0 && top >= out.secondWave)\n        out.warnings << tr(\"From %1 GHz the stack carries")],
  [(TF, PAST), (TC, TOOL)]),
 ("past the second surface wave, nothing said",
  [(FW, "    if (out.secondWave > 0 && top >= out.secondWave)\n        out.lines", "    if (false)\n        out.lines")],
  [(TF, PAST)]),
 ("where the second surface wave begins left out of the answer",
  [(QC, "    if (w.secondWave > 0) out.insert(QStringLiteral(\"second surface wave from\"), w.secondWave);\n", "")],
  [(TC, TOOL)]),
 # ---- the strips that still disagree, at the tests' new frequencies
 ("strips that disagree said to agree",
  [(DE, "    if (offFrom == 0)\n", "    if (true)\n")],
  [(TD, LONG)]),
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
