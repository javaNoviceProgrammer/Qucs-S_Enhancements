#!/usr/bin/env python3
"""breaks.py: each piece of phase D's de-embedding undone on purpose: the
calibration (the strips' length, E E' and the normalisation of E, the
phase followed from its anchor), the de-embedding (its algebra, the shift,
a given line), the solve's report of each port's face, the currents drawn
of the ports as solved, Touchstone 2.0, and the tab, the model and the
tools that carry it. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-deembed-breaks")
os.makedirs(RUNS, exist_ok=True)

E = "rf25/rf25_deembed.cpp"
F = "rf25/rf25_fullwave.cpp"
D = "rf25doc.cpp"
M = "rf25/rf25_model.cpp"
C = "qucscontrol_rf25.cpp"
TE, TD, TC = "test_rf25_deembed", "test_rf25_doc", "test_qucs_control"
THRU = "aLineDeembeddedHalfWayIsAThru"
OPEN = "theFeedIsALineAndAnOpenEndIsItsOwn"
PHASE = "aLongStripsPhaseIsFollowedFromBelow"
GIVEN = "aReferencePlaneMovesAlongAGivenLine"
PHYS = "deembeddingKeepsWhatPhysicsAllows"
TS2 = "aTouchstone2CarriesEachPortsReference"
WAVE = "aFullWaveStudySolvesThePortsAndDrawsTheCurrents"
MCP = "anRf25ModelIsSolvedFullWaveThroughItsTool"

BREAKS = [
 ("the calibration strips four heights long, the ports' near fields meeting",
  [(E, "    const double length = chosen > 0 ? chosen * micrometre : 8 * h + 2 * width * micrometre;   // m",
    "    const double length = chosen > 0 ? chosen * micrometre : 4 * h;   // m")],
  [(TE, THRU)]),

 ("E E' taken from the strips the wrong way round",
  [(E, "        const Chain m = times(times(t1, inverse(t2)), t1);", "        const Chain m = times(times(t2, inverse(t1)), t1);")],
  [(TE, THRU)]),

 ("E split with an ideal transformer at the face, its D not one",
  [(E, "        const Chain e = {(m11 + 1.0) / 2.0, m[1] / (m11 + 1.0), m[2] / 2.0, Complex(1, 0)};",
    "        const Chain e = {(m11 + 1.0) / 2.0 * std::sqrt(2.0), m[1] / (m11 + 1.0) / std::sqrt(2.0), m[2] / 2.0 * std::sqrt(2.0), Complex(1 / std::sqrt(2.0), 0)};")],
  [(TE, OPEN)]),

 ("the strips' phase not followed from the frequency before",
  [(E, "            wave += Complex(0, 2 * M_PI * std::round((predicted - wave.imag()) / (2 * M_PI)));\n", "")],
  [(TE, PHASE)]),

 ("no anchor below the sweep",
  [(E, "    if (anchored) at.prepend(anchor);", "    (void)anchored;")],
  [(TE, PHASE)]),

 ("a line that gives back more than it gets not said",
  [(E, "        if (wave.real() < -5e-3) gains << at.at(k);", "        (void)gains;")],
  [(TE, PHASE)]),

 ("(A - Z C) without its Z C",
  [(E, "        MatrixC left = -(z * c.asDiagonal());", "        MatrixC left = MatrixC::Zero(np, np);")],
  [(TE, THRU)]),

 ("the reference plane moved out, not in",
  [(E, "            const Chain total = each.shift != 0 ? times(e, lineOf(gamma, impedance, each.shift)) : e;",
    "            const Chain total = each.shift != 0 ? times(e, lineOf(gamma, impedance, -each.shift)) : e;")],
  [(TE, THRU)]),

 ("a given line's permittivity left out of its phase",
  [(E, "            Complex gamma(0, w * std::sqrt(std::max(each.eps, 0.0)) / c0), impedance(each.impedance, 0);",
    "            Complex gamma(0, w / c0), impedance(each.impedance, 0);")],
  [(TE, GIVEN)]),

 ("the discontinuity's shunt read off its series term",
  [(E, "                said.shuntC = e[2].imag() / w;", "                said.shuntC = e[1].imag() / w;")],
  [(TC, MCP)]),

 ("a port's face not reported",
  [(F, "            out.faceWidths[p] = faceWidth;\n", "")],
  [(TE, THRU), (TD, WAVE)]),

 ("the currents drawn of the de-embedded Z",
  [(F, "    const QList<QList<QList<Complex>>>& zAt = solved.zSolved.isEmpty() ? solved.z : solved.zSolved;",
    "    const QList<QList<QList<Complex>>>& zAt = solved.z;")],
  [(TE, THRU)]),

 ("Touchstone 2.0 without its [Reference]",
  [(F, "    keywords += QStringLiteral(\"[Number of Frequencies] %1\\n[Reference] %2\\n[Network Data]\\n\")",
    "    keywords += QStringLiteral(\"[Number of Frequencies] %1\\n[Network Data]\\n%2\")")],
  [(TE, TS2)]),

 ("the port's calibrate not read",
  [(D, "            p.calibrate = flagOf(port, QStringLiteral(\"calibrate\"));", "            p.calibrate = false;")],
  [(TC, MCP)]),

 ("the tab's solve not de-embedded",
  [(D, "            *job->wave = deembedded(*job->wave, each);", "            (void)each;")],
  [(TD, WAVE)]),

 ("the export's version not read",
  [(D, "    out.version2 = textOf(*node, QStringLiteral(\"version\")) == QLatin1String(\"2.0\");", "    out.version2 = false;")],
  [(TD, WAVE)]),

 ("the export's comment of a de-embedded plane not said",
  [(D, "                planes << tr(\"%1's %2 µm %3 from its edge %4, its own discontinuity taken out\")",
    "                planes << tr(\"%1's at the centres of the cells it drives%2%3%4\")")],
  [(TD, WAVE)]),

 ("every edge port calibrated unasked",
  [(M, "          boolean(QStringLiteral(\"calibrate\"), tr(\"Take out its own discontinuity\"), false,",
    "          boolean(QStringLiteral(\"calibrate\"), tr(\"Take out its own discontinuity\"), true,")],
  [(TD, WAVE)]),

 ("rf25_export's version not read",
  [(C, "        what.version2 = version == QLatin1String(\"2.0\");", "        what.version2 = false;")],
  [(TC, MCP)]),

 ("rf25_solve's answer without the de-embedding",
  [(C, "            port.insert(QStringLiteral(\"de-embedded\"), de);", "            (void)de;")],
  [(TC, MCP)]),
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
