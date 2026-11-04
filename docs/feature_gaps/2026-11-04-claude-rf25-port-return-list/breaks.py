#!/usr/bin/env python3
"""breaks.py: a port's returns given as a list (4 November) undone on purpose, one
piece at a time - the columns against each return, both faces of a metal
solved as its two, the refusals, the loop check and the benign loop it lets
through, the shares and where they are taken, the model's kinds and names,
the tab's reading, drawing and Shift-drag, the kept solution, and the tools'
answers - each built and its tests run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=3,7
runs those alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the
checkout's build); BRK_SOURCE=<dir> a copy of qucs-s-26.1.1 to break instead
of the checkout's. Run as a script only: importing it does nothing."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-port-return-list-breaks")

F = "rf25/rf25_fullwave.cpp"
M = "rf25/rf25_model.cpp"
D = "rf25doc.cpp"
P = "rf25panel.cpp"
K = "rf25/rf25_kept.cpp"
C = "qucscontrol_rf25.cpp"
E = "qucscontrol_fem.cpp"
R = "test_rf25_returns"
COPLANAR = (R, "aPortReturningThroughTheFacesBesideItIsTheCoplanarOne")
TIPS = (R, "aProbesTipsShareItsCurrent")
SERIES = (R, "aPortNamedAmongTheReturnsIsInSeries")
METAL = (R, "aTerminalOnAnotherMetalIsItsFromMetalWithoutAPath")
REFUSED = (R, "whatAPortThatNamesItsReturnsCannotBeIsSaid")
COMPRESSED = (R, "aCompressedSolveOfTheTipsIsTheDenseOne")
TIPS_GSSG = (R, "aGssgsSignalsReturnThroughTheSameTwoTips")
MODEL = ("test_rf25_model", "aPortsReturnsAreTheTerminalsAndPortsItNames")
DOC = ("test_rf25_doc", "aPortReturnsThroughTheTerminalsItNames")
TOOLS = ("test_qucs_control", "anRf25PortsReturnsAreNamedThroughTheTools")
GSSG = ("test_rf25_fullwave", "aGssgReturnsThroughItsGroundsWithNoGroundPlane")
PAIRS = ("test_rf25_fullwave", "aDifferentialPairsModesAreItsMixedModeS")

BREAKS = [
 ("a return's column the port's own cells alone, the return's not taken off",
  [(F, "            columns << Eigen::VectorXcd(owns.at(p) - back);", "            columns << Eigen::VectorXcd(owns.at(p));")],
  [COPLANAR, TIPS, SERIES, METAL]),
 ("a face terminal's cells on the bottom face alone, not half on the top (put())",
  [(F, "                for (int k = 0; k < face.size(); ++k) put(back, face.at(k), shares.at(k));",
       "                for (int k = 0; k < face.size(); ++k) back(nodeOf.value(face.at(k))) += shares.at(k);")],
  [COPLANAR]),
 ("a return on the port's own conductor not refused",
  [(F, "            for (const int net : netsUnder(back))\n                if (own.contains(net)) {",
       "            for (const int net : netsUnder(back))\n                if (false && own.contains(net)) {")],
  [REFUSED, TOOLS]),
 ("a terminal on another port's cells taken as it is",
  [(F, "                        if (back(i) != Complex(0, 0) && owns.at(q)(i) != Complex(0, 0)) {",
       "                        if (false && back(i) != Complex(0, 0) && owns.at(q)(i) != Complex(0, 0)) {")],
  [SERIES]),
 ("ports that return through each other not refused",
  [(F, "                if (std::abs(intoPort.at(q)) > 1e-6 && !involved.contains(out.portNames.at(q))) involved << out.portNames.at(q);",
       "                if (false && std::abs(intoPort.at(q)) > 1e-6 && !involved.contains(out.portNames.at(q))) involved << out.portNames.at(q);")],
  [SERIES]),
 ("a loop no port's current sees refused too (a GSSG's two signals' ties)",
  [(F, "                if (std::abs(intoPort.at(q)) > 1e-6 && !involved.contains(out.portNames.at(q))) involved << out.portNames.at(q);",
       "                if (!involved.contains(out.portNames.at(q))) involved << out.portNames.at(q);")],
  [GSSG, PAIRS, TIPS_GSSG]),
 ("the shares at the first frequency, not the top",
  [(F, "    const int topAt = int(std::max_element(frequencies.begin(), frequencies.end()) - frequencies.begin());",
       "    const int topAt = 0;")],
  [TIPS]),
 ("the shares of the columns' voltages, not their currents",
  [(F, "            if (k == topAt) shareOut(x * z);", "            if (k == topAt) shareOut(x);")],
  [TIPS, COPLANAR]),
 ("a port that names its returns wanting a ground plane",
  [(F, "            if (port.terminals) continue;\n            if (port.internal ? fromGround : !port.coplanar) grounded << out.portNames.at(p);",
       "            if (port.internal ? fromGround : !port.coplanar) grounded << out.portNames.at(p);")],
  [TIPS, COMPRESSED]),
 ("its calibration by strips not refused",
  [(F, "        if (port.wantsStrips()) {\n            out.problems << tr(\"%1 returns through the terminals it names",
       "        if (false && port.wantsStrips()) {\n            out.problems << tr(\"%1 returns through the terminals it names")],
  [REFUSED]),
 ("a return named twice not refused",
  [(F, "            if (named.contains(t.name)) {", "            if (false && named.contains(t.name)) {")],
  [REFUSED]),
 ("the ports' paths never made (the tests' switch stuck off)",
  [(F, "    for (int k = 0; k < c.np && (control == nullptr || control->portPaths); ++k) {",
       "    for (int k = 0; k < c.np && false; ++k) {")],
  [METAL]),
 ("the ports' paths made where the tests ask for none",
  [(F, "    for (int k = 0; k < c.np && (control == nullptr || control->portPaths); ++k) {",
       "    for (int k = 0; k < c.np; ++k) {")],
  [METAL]),
 ("a point terminal off its metal taken as the cell nearest it",
  [(F, "                if (!over) {\n                    bool onShape = false;", "                if (false) {\n                    bool onShape = false;")],
  [REFUSED]),
 ("no warning where a port that names its returns gives back",
  [(F, "    if (!naming.isEmpty() && !c.layers.groundBelow && out.gain > 1e-6) {",
       "    if (false && !naming.isEmpty() && !c.layers.groundBelow && out.gain > 1e-6) {")],
  [TIPS]),
 ("the edge port's Return without its terminals",
  [(M, "{QStringLiteral(\"ground\"), QStringLiteral(\"beside\"), QStringLiteral(\"terminals\")},",
       "{QStringLiteral(\"ground\"), QStringLiteral(\"beside\")},")],
  [MODEL]),
 ("the model's names of the returns unchecked",
  [(M, "            else if (!terminals.contains(r) && !driven.contains(r))",
       "            else if (false && !terminals.contains(r) && !driven.contains(r))")],
  [MODEL, DOC]),
 ("a renamed terminal left where the returns name it",
  [(M, "        for (QString& r : returns)\n            if (r == from) r = to;", "")],
  [MODEL, DOC]),
 ("a terminal's metal left as it was when its layer is renamed",
  [(M, "        rename(p, QStringLiteral(\"metal\"));\n", "")],
  [MODEL]),
 ("a list of names written as a text not read",
  [(M, "    for (const QString& name : v.toString().split(QRegularExpression(QStringLiteral(\"[,;\\\\s]+\")), Qt::SkipEmptyParts))\n        out << name;",
       "")],
  [MODEL, DOC]),
 ("the tab not reading a port's returns",
  [(D, "            p.returns = returnsOf(port, model, problem);", "            p.returns = {};")],
  [DOC, TOOLS]),
 ("the terminals not drawn",
  [(D, "            terminals.labels << LayoutView::Overlay::Label{place, portName(node), QPointF()};", "")],
  [DOC, TOOLS]),
 ("the Internal Port tool's Shift-drag a port of its own",
  [(D, "        if ((shift ? placeTerminal(box, &why) : placeInternalPort(box, &why)).isEmpty() && !why.isEmpty()) a_status->setText(why);",
       "        if (placeInternalPort(box, &why).isEmpty() && !why.isEmpty()) a_status->setText(why);")],
  [DOC]),
 ("the Touchstone's reference plane of a port that names its returns not said",
  [(D, "            if (!d.done && a_fullWave->namesReturns.value(p))", "            if (false && !d.done && a_fullWave->namesReturns.value(p))")],
  [DOC]),
 ("the Returns menu offering the port itself",
  [(P, "                    if (!n.enabled || portName(n) == self) continue;", "                    if (!n.enabled) continue;")],
  [DOC]),
 ("the kept solution without its returns",
  [(K, "            put(s, kept.fullWave->returnsOf);\n", "")],
  [DOC]),
 ("rf25_solve without each port's returns",
  [(C, "        if (!w.returnsOf.value(p).isEmpty()) {\n            QJsonArray returns;", "        if (false) {\n            QJsonArray returns;")],
  [TOOLS]),
 ("fem_describe of an RF 2.5D model its multiphysics kinds",
  [(E, "            if (rf25) {\n                const QString group", "            if (false) {\n                const QString group")],
  [TOOLS]),
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
        for i, (name, edits, tests) in enumerate(BREAKS, 1):
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
            log = os.path.join(RUNS, "brk%02d.log" % i)
            suites = sorted({t[0] for t in tests})
            targets = ["qucs/tests/" + s for s in suites]
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + targets, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_SETTINGS_DIR=os.path.join(RUNS, "settings"),
                               QUCS_TRASH_DIR=os.path.join(RUNS, "trash"), QUCS_CLAUDE="/nonexistent/claude",
                               QUCS_GH="/nonexistent/gh", QTEST_FUNCTION_TIMEOUT="1800000")
                    env.pop("QUCS_TEST_SLOW", None)
                    env.pop("QUCS_TEST_PART", None)
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=1800, env=env, cwd=RUNS)
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
        print("%-70s %s" % (name[:70], said))
        if "BUILD FAILED" in said or "hung" in said or "caught)" not in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
