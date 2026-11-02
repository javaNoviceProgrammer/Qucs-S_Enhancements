#!/usr/bin/env python3
"""breaks.py: the phase E check's fixes undone on purpose, one piece at a
time - the third calibration strip and its residual, the shorter strips
and which strips each frequency takes, the line's impedance by a half
wave, a feed's straight run and its port's reach, a de-embedding that
gives back, the stack's second surface wave and what is said past it, and
the answer's and the port's parts - each built and its tests run. Sources
are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/person/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/person/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-phase-e-check-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

DE = "rf25/rf25_deembed.cpp"
FW = "rf25/rf25_fullwave.cpp"
DOC = "rf25doc.cpp"
QC = "qucscontrol_rf25.cpp"
TD, TF, TDOC, TC = "test_rf25_deembed", "test_rf25_fullwave", "test_rf25_doc", "test_qucs_control"
THIRD = "aThirdStripSaysWhereTheStripsAreOneLine"
HALF = "aHalfWaveTakesItsImpedanceFromTheThird"
FEED = "aFeedShorterThanItsPortsFieldIsSaid"
GIVES = "aDeembeddingThatGivesBackIsSaid"
LONG = "aLongStripsPhaseIsFollowedFromBelow"
THRU = "aThruWhosePlanesMeetIsAThru"
CPW = "aCoplanarPortsFeedIsItsGroundsAndItsVias"
CUT = "theSecondSurfaceWaveIsTE1sCutoff"
PAST = "pastTheSecondSurfaceWaveItIsSaid"
SOLVES = "aFullWaveStudySolvesThePortsAndDrawsTheCurrents"
TOOL = "anRf25ModelIsSolvedFullWaveThroughItsTool"

BREAKS = [
 # ---- the third strip
 ("the third strip not looked at: the residual the shorter's alone",
  [(DE, "        out.residual << std::max(apart(l1, lineOf(gamma, z, length), z), apart(l3, lineOf(gamma, z, third), z));",
    "        out.residual << apart(l1, lineOf(gamma, z, length), z);")],
  [(TD, THIRD)]),

 ("the residual in ohms beside the cosh and the sinh",
  [(DE, "    const Chain na = {a[0], a[1] / z, a[2] * z, a[3]}, nb = {b[0], b[1] / z, b[2] * z, b[3]};",
    "    const Chain na = a, nb = b;\n    (void)z;")],
  [(TD, LONG)]),

 ("the third strip as long as the longer",
  [(DE, "        out.third = kPhi * between;", "        out.third = 2 * between;")],
  [(TD, THIRD), (TD, HALF)]),

 # ---- the shorter strips
 ("no shorter strips solved",
  [(DE, "    for (int halving = 1; halving <= 3 && chosen <= 0; ++halving) {",
    "    for (int halving = 1; halving <= 0 && chosen <= 0; ++halving) {")],
  [(TD, THIRD)]),

 ("each frequency from the strips that agree best, not the longest that agree",
  [(DE, "            if (r <= kStripsAgree) return i;\n", "")],
  [(TD, THIRD)]),

 # ---- the line's impedance
 ("the impedance always the shorter strip's",
  [(DE, "        const Complex z = nearHalf && std::abs(std::sinh(gamma * third)) > std::abs(std::sinh(wave)) ? z3 : z1;",
    "        const Complex z = z1;\n        (void)nearHalf;\n        (void)z3;")],
  [(TD, HALF)]),

 ("the impedance the third's wherever it is the longer",
  [(DE, "        const Complex z = nearHalf && std::abs(std::sinh(gamma * third)) > std::abs(std::sinh(wave)) ? z3 : z1;",
    "        const Complex z = std::abs(std::sinh(gamma * third)) > std::abs(std::sinh(wave)) ? z3 : z1;\n        (void)nearHalf;")],
  [(TD, THRU)]),

 # ---- a feed's straight run and its port's reach
 ("a feed's straight run not looked at",
  [(FW, "            out.feeds[p].length = feedLength(", "            out.feeds[p].length = -1 + 0 * feedLength(")],
  [(TD, FEED), (TD, CPW), (TC, TOOL)]),

 ("a feed's straight run never stopped",
  [(FW, "        if (!in(own, at(lo + width / 2)) || in(own, at(lo - beyond)) || in(own, at(lo + width + beyond)))",
    "        if (false)")],
  [(TD, FEED)]),

 ("a coplanar feed's grounds taken for its widening",
  [(FW, "                if (!span.cells[k].isEmpty() && span.near[k] > 0) beyond = std::min(beyond, 0.5 * span.near[k]);",
    "                if (!span.cells[k].isEmpty() && span.near[k] > 0) beyond = std::max(beyond, 2 * span.near[k]);")],
  [(TD, CPW)]),

 ("a feed shorter than its port's field not said",
  [(DE, "        if (said.reach > 0 && said.feedLength >= 0 && said.feedLength < said.reach)", "        if (false)")],
  [(TD, FEED)]),

 # ---- a de-embedding that gives back
 ("a de-embedding that gives back where the structure does not, not said",
  [(DE, "    if (out.gain > 1e-6 && worst >= 0 && solvedGiven.value(worst) < 0.5 * out.gain)", "    if (false)")],
  [(TD, GIVES)]),

 ("where it gives back the most not kept",
  [(DE, "            out.gainAt = f;\n            worst = k;", "            worst = k;")],
  [(TD, GIVES)]),

 # ---- the second surface wave
 ("the second surface wave never found",
  [(FW, "        if ((before > 0) != (now > 0)) {", "        if (false) {")],
  [(TF, CUT), (TF, PAST), (TC, TOOL)]),

 ("over a ground, TE1 taken for the slab's half wave",
  [(FW, "        return grounded ? a : c;", "        return c;")],
  [(TF, CUT)]),

 ("past it, nothing said",
  [(FW, "    if (out.secondWave > 0 && top >= out.secondWave) {", "    if (false) {")],
  [(TF, PAST), (TC, TOOL)]),

 # ---- the answer, the port and the tab
 ("the warnings left out of the answer",
  [(QC, "    if (!w.warnings.isEmpty()) out.insert(QStringLiteral(\"warnings\"), QJsonArray::fromStringList(w.warnings));\n", "")],
  [(TC, TOOL)]),

 ("what the strips said left out of the answer",
  [(QC, "                de.insert(QStringLiteral(\"strips agree to\"), d.agreedTo);\n", "")],
  [(TC, TOOL)]),

 ("the strips' agreement not kept at the port",
  [(DE, "                    said.agreedTo = cal.agreedTo;\n", "")],
  [(TC, TOOL)]),

 ("the strips' length asked not read",
  [(DOC, "            p.strips = numberOf(port, QStringLiteral(\"strips\"), model, 0, &okStrips);",
    "            p.strips = 0 * numberOf(port, QStringLiteral(\"strips\"), model, 0, &okStrips);")],
  [(TDOC, SOLVES)]),

 ("the strips' length handed over in metres",
  [(DOC, "calibrate(job->stack, job->how, job->top, feed, job->frequencies, 1e-6, &quiet, port.strips / 1e-6)",
    "calibrate(job->stack, job->how, job->top, feed, job->frequencies, 1e-6, &quiet, port.strips)")],
  [(TDOC, SOLVES)]),
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
