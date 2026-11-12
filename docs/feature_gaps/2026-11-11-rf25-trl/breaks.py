#!/usr/bin/env python3
"""breaks.py: RF 2.5D's multiline TRL calibration (11 November, F3 of the open
items of 8 November) undone on purpose, one piece at a time - the error
boxes' terms from the eigenvectors, the thru and the reflect, the line each
frequency takes and its gamma unwrapped, the correction's scale, the
renormalization, what it refuses and says, and the solve's standards and
settings - each built and its tests run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=3
runs that alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the
checkout's build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-trl-breaks")

T = "rf25/rf25_trl.cpp"
D = "rf25doc.cpp"
M = "rf25/rf25_model.cpp"
U = "test_rf25_trl"
DEV = (U, "aDeviceBehindTwoFixturesIsFoundAgain")
BEST = (U, "eachFrequencyTakesTheLineThatTellsBest")
SHORT = (U, "aShortReflectIsTold")
RENORM = (U, "anSIsRenormalized")
REFUSE = (U, "whatCannotBeCalibratedIsSaid")
PADS = ("test_rf25_doc", "aTrlCalibrationTakesThePadsOut")

BREAKS = [
 ("e00 from the other eigenvector",
  [(T, "        const Complex e00 = r2, a = e00 - r1;", "        const Complex e00 = r1, a = e00 - r2;")], [DEV]),
 ("e33 of the wrong sign",
  [(T, "        const Complex e33 = -q2, b = q1 - q2;", "        const Complex e33 = q2, b = q1 - q2;")], [DEV]),
 ("the reflect's sign not told by its kind",
  [(T, "        if ((reflection * expect).real() < 0) {", "        if (false) {")], [SHORT]),
 ("every frequency taking the first line",
  [(T, "            if (weights.at(i) > weights.at(taken) + 1e-12) taken = i;\n", "")], [BEST]),
 ("gamma's phase not unwrapped",
  [(T, "                const double turns = std::round((expected - g0.imag()) * l / (2 * M_PI));", "                const double turns = 0;")], [DEV]),
 ("e10 e32 the thru's S21 alone",
  [(T, "        const Complex e10e32 = t[2] * (1.0 - x);", "        const Complex e10e32 = t[2];")], [DEV]),
 ("the correction's shared scale left out",
  [(T, "        const M2 td = e10e32 * (ka.inverse() * matrixOf(tm) * kb.inverse());", "        const M2 td = ka.inverse() * matrixOf(tm) * kb.inverse();")], [DEV]),
 ("lines that hardly tell not said",
  [(T, "        if (weights.at(taken) < kPoorPhase) {", "        if (false) {")], [DEV, BEST]),
 ("S renormalized into where it came from",
  [(T, "    const M2 out = (z - to * one) * (z + to * one).inverse();", "    const M2 out = (z - from * one) * (z + from * one).inverse();")], [RENORM]),
 ("a line no longer than the thru taken",
  [(T, "        if (!(st.longer.at(i) > 0)) {", "        if (false) {")], [REFUSE]),
 ("the solve not calibrated",
  [(D, "            if (job->trl && job->wave->ok() && !job->stop.load()) job->wave = trlOn(*job, *job->wave);\n", "")], [PADS]),
 ("each line taken as a millimetre longer",
  [(D, "                t.lines << qMakePair(file, longer);", "                t.lines << qMakePair(file, 1e-3);")], [PADS]),
 ("the reflect taken for a short",
  [(D, "            t.shortReflect = textOf(n, QStringLiteral(\"reflect_kind\")) == QLatin1String(\"short\");", "            t.shortReflect = true;")], [PADS]),
 ("the lines' own impedance not said",
  [(D, "        out->warnings << tr(\"The calibrated S is in the lines' own impedance", "        (void)tr(\"The calibrated S is in the lines' own impedance")], [PADS]),
 ("a calibration with no thru not said",
  [(M, "        if (textOf(p, QStringLiteral(\"thru\")).trimmed().isEmpty()) said << tr(\"The TRL calibration names no thru.\");\n", "")], [PADS]),
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
