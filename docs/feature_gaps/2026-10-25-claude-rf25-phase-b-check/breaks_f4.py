#!/usr/bin/env python3
"""breaks_f4.py: each part of the phase B check's F4 - a line's R by
Wheeler's rule on its own thick cross-section, the ground's share, the
floor at direct current - undone on purpose. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-phase-b-check-f4-breaks")
os.makedirs(RUNS, exist_ok=True)

S, MO, SP, T = "rf25/rf25_static.cpp", "rf25/rf25_model.cpp", "rf25/rf25_sparams.cpp", "qucscontrol_rf25.cpp"
ST, SPS, CTL = "test_rf25_static", "test_rf25_sparams", "test_qucs_control"
WHEELER = "aLinesResistanceIsWheelersOnItsThickness"
LOSSES = "aLinesResistanceAndConductanceAreThoseOfItsLosses"
GROUND = "theGroundIsReadForWhatItIsNotWhereItIs"
ROUGH = "aMetalsRoughnessRaisesItsLoss"
D, DOC = "rf25doc.cpp", "test_rf25_doc"
STRAIGHT = "aSolveStraightAfterAnEditIsOfTheEdit"
PAIR = "twoLinesAreCoupledLinesWithTheirModes"
TOOL = "anRf25ModelIsSolvedThroughItsTool"

BREAKS = [
 ("R from the sheet's crowding again",
  [(S, "    if (index < wheeler.size()) {", "    if (false) {")],
  [(ST, WHEELER), (ST, LOSSES)]),

 ("the walls left out: two sheets",
  [(S, "        for (int i = 0; i + 1 < zs.size(); ++i) {\n            faces << Face2D{box.net, false,",
    "        for (int i = 0; false && i + 1 < zs.size(); ++i) {\n            faces << Face2D{box.net, false,")],
  [(ST, WHEELER)]),

 ("the ground's image left out",
  [(S, "    return (integral(true) - integral(false)) / (2 * M_PI * kEps0 * (f.b - f.a));",
    "    return (-integral(false)) / (2 * M_PI * kEps0 * (f.b - f.a));")],
  [(ST, WHEELER)]),

 ("the corners not refined below the thickness",
  [(S, "                while (box.across.at(1) - box.across.at(0) > t / 2)", "                while (false)"),
   (S, "                while (box.across.at(box.across.size() - 1) - box.across.at(box.across.size() - 2) > t / 2)",
    "                while (false)")],
  [(ST, WHEELER)]),

 ("one side of the difference only",
  [(S, "(inductance2D(boxes, count, recede, dn) - inductance2D(boxes, count, recede, -dn)) / (2 * dn * mu0);",
    "(inductance2D(boxes, count, recede, dn) - inductance2D(boxes, count, -1, 0.0)) / (2 * dn * mu0);")],
  [(ST, WHEELER)]),

 ("a conductor's bottom face does not recede",
  [(S, "            z0 += dn;\n            z1 -= dn;", "            z1 -= dn;")],
  [(ST, WHEELER)]),

 ("no floor at direct current",
  [(S, "    for (int i = 0; i < n; ++i) r[i][i] = std::max(r[i][i], pm.dcResistance.at(i) + g[i][i]);\n", "")],
  [(ST, WHEELER), (CTL, TOOL)]),

 ("a metal's slab resistance of the wrong conductivity",
  [(S, "        const double rs = surfaceImpedance(extraction.sigma.value(k), extraction.thickness.value(k), frequency).real();",
    "        const double rs = surfaceImpedance(2 * extraction.sigma.value(k), extraction.thickness.value(k), frequency).real();")],
  [(ST, WHEELER)]),

 # (Not "a metal of no thickness taken as a thick one": the corners'
 # halving down to half of no thickness never ends, and a break that eats
 # the memory proves nothing.)
 ("a metal of no thickness not said",
  [(S, "            out.lines << tr(\"%1 has no thickness: R is from how the charge crowds on a sheet of none, which grows without \"",
    "            if (false) out.lines << tr(\"%1 has no thickness: R is from how the charge crowds on a sheet of none, which grows without \"")],
  [(ST, WHEELER)]),

 ("a metal ground's surface resistance nothing",
  [(S, "    if (extraction.groundKind == QLatin1String(\"metal\") && extraction.groundValue > 0 && frequency > 0) {",
    "    if (false) {")],
  [(ST, WHEELER)]),

 ("a metal ground's conductivity not read",
  [(MO, "            else if (layer.variant == QLatin1String(\"metal\"))\n                read(QStringLiteral(\"sigma\"), &layer.sigma);\n", "")],
  [(ST, GROUND)]),

 ("a pair's R diagonal again",
  [(SP, "    if (wheeler.size() == n) {", "    if (false) {")],
  [(SPS, PAIR)]),

 ("a metal's roughness not in Wheeler's R",
  [(S, "        const double rs = surfaceImpedance(extraction.sigma.value(k), extraction.thickness.value(k), frequency).real()\n                          * roughnessFactor(extraction.roughness.value(k), extraction.sigma.value(k), frequency);",
    "        const double rs = surfaceImpedance(extraction.sigma.value(k), extraction.thickness.value(k), frequency).real();")],
  [(ST, ROUGH)]),

 ("Hammerstad and Bekkadal's factor misread",
  [(S, "    return 1 + 2 / M_PI * std::atan(1.4 * ratio * ratio);", "    return 1 + 2 / M_PI * std::atan(1.4 * ratio);")],
  [(ST, ROUGH)]),

 # ---- found on the way: a solve after an edit came to nothing
 ("a Solve after an edit starts on the mesh before it again",
  [(D, "    if (a_prepareSoon->isActive() || isPreparing()) {\n        a_solveWhenPrepared = true;",
    "    if (false) {\n        a_solveWhenPrepared = true;")],
  [(DOC, STRAIGHT)]),

 ("a solve an edit's mesh overtakes is dropped again",
  [(D, "    const bool wanted = a_solveJob != nullptr || a_solveWhenPrepared;", "    const bool wanted = a_solveWhenPrepared;")],
  [(DOC, STRAIGHT)]),

 ("waiting for a solve does not wait for one waiting for the mesh",
  [(D, "    while ((a_solveJob || a_solveWhenPrepared) && t.elapsed() < ms)", "    while (a_solveJob && t.elapsed() < ms)")],
  [(DOC, STRAIGHT)]),

 ("the tool gives no R at direct current",
  [(T, "                if (lp.rDc > 0) line.insert(QStringLiteral(\"R_dc\"), lp.rDc);\n", "")],
  [(CTL, TOOL)]),
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
