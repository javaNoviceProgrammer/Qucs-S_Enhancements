#!/usr/bin/env python3
"""breaks.py: RF 2.5D's adaptive sweep and mesh convergence check (11
November, F1 of the open items of 8 November) undone on purpose, one piece at
a time - the rational fit, the sweep's choice and checks of its points, the
currents carried, the calibration read between its frequencies, the study and
the tool asking for it, the kept solution, the mesh's note, the convergence
verdicts, the check's meshes refined as a whole and what it leaves - each
built and its tests run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=3 runs that
alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the checkout's
build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-convergence-breaks")

A = "rf25/rf25_adaptive.cpp"
D = "rf25doc.cpp"
K = "rf25/rf25_kept.cpp"
C = "qucscontrol_rf25.cpp"
U = "test_rf25_adaptive"
FIT = (U, "aRationalFunctionIsFoundAgain")
FEW = (U, "anAdaptiveSweepSolvesFewAndReadsTheRest")
LINE = (U, "aLineOfManyResonancesIsReadWithinTheTolerance")
CURRENTS = (U, "theCurrentsAreCarriedByTheFit")
SHORT = (U, "aShortSweepIsSolvedWhole")
STOP = (U, "aStoppedOrFailedSweepSaysSo")
CAL = (U, "aCalibrationIsReadBetweenItsFrequencies")
CONV = (U, "aValueConvergingIsExtrapolated")
NOT = (U, "aValueNotConvergingIsSaid")
REFINE = (U, "aMeshIsRefinedAsAWhole")
TAB = ("test_rf25_doc", "anAdaptiveSweepSolvesFewOfItsFrequencies")
CHECK = ("test_rf25_doc", "aMeshConvergenceCheckSaysHowFarTheMeshIsFromTheLimit")
TOOLS = ("test_qucs_control", "anRf25ModelsSweepAndMeshAreCheckedThroughTheTools")

BREAKS = [
 ("the fit's weights not the Loewner matrix's least singular vector",
  [(A, "        w = svd.matrixV().col(m - 1);", "        w = svd.matrixV().col(0);")], [FIT]),
 ("no more than two support points",
  [(A, "|| int(chosen.size()) >= top) break;", "|| int(chosen.size()) >= std::min(top, 2)) break;")], [FIT]),
 ("the points solved last not held to what the fit said of them",
  [(A, "            all = all && e <= how.tolerance;", "            all = true;")], [LINE]),
 ("the currents not carried by the fit, the first support's taken",
  [(A, "for (int r = 0; r < std::min(column.size(), from.size()); ++r) column[r] += c.at(j) * from.at(r);",
       "for (int r = 0; r < std::min(column.size(), from.size()); ++r) column[r] = from.at(r);")], [CURRENTS]),
 ("a short sweep not solved whole",
  [(A, "    if (n <= std::max(how.fewest, how.first)) {", "    if (n <= how.first) {")], [SHORT]),
 ("a stop not heeded between rounds",
  [(A, "        if (stop()) return out;\n", "")], [STOP]),
 ("the fit's frequencies solved not said in the answer",
  [(A, "    wave.solvedFrequencies = out.solved;\n", "")], [FEW, TAB]),
 ("a calibration not read between its frequencies",
  [(A, "    if (!calibration.ok() || calibration.frequencies.size() >= sweep.size()) return calibration;",
       "    if (true) return calibration;")], [CAL, TAB]),
 ("Richardson's order of halving cells, whatever the sizes",
  [(A, "        return (std::pow(h2, p) - std::pow(h3, p)) / (std::pow(h1, p) - std::pow(h2, p));", "        return std::pow(0.5, p);")], [CONV]),
 ("the limit extrapolated as if the cells halved",
  [(A, "        out.limit = v3 + (v3 - v2) * std::pow(h3, order) / (std::pow(h2, order) - std::pow(h3, order));",
       "        out.limit = v3 + (v3 - v2) / (std::pow(2.0, order) - 1);")], [CONV]),
 ("changes that turn back taken as converging",
  [(A, "    if (out.ratio < 1 && turned) {", "    if (false && turned) {")], [NOT]),
 ("changes shrinking slower than the root of the cells taken as converging",
  [(A, "    if (order >= 0.5) {", "    if (order > 0) {")], [NOT]),
 ("a mesh refined by its cells across alone",
  [("rf25/rf25_mesh.cpp", "    if (how.maximum > 0) out.maximum = how.maximum / factor;\n", "")], [REFINE, CHECK]),
 ("the check's meshes not refined as a whole",
  [(D, "        MeshSettings how = refinedBy(job.how, factor);", "        MeshSettings how = job.how;")], [CHECK]),
 ("the strips calibrated at every frequency of an adaptive sweep",
  [(D, "        if (!a.whole) solvedAt = a.solved;\n", "")], [TAB]),
 ("the study's Add points where it changes not read",
  [(D, "(request.adaptive < 0 ? studyAdaptive : request.adaptive > 0)", "(request.adaptive > 0)")], [TAB]),
 ("the mesh not said first in a full-wave solve's lines",
  [(D, "            if (job->wave->ok()) job->wave->lines.prepend(meshNoteOf(job->mesh));\n", "")], [TAB]),
 ("the frequencies an adaptive sweep solved not kept beside the model",
  [(K, "            put(s, kept.fullWave->solvedFrequencies);\n", "")], [TAB]),
 ("a convergence check taken as the model's solve",
  [(D, "        a_convergenceLayout = job->layoutStamp;\n        showProblems();\n        showProgress();\n        emit solvedChanged();\n        return;\n",
       "        a_convergenceLayout = job->layoutStamp;\n")], [CHECK]),
 ("the bar not saying which of the check's meshes it is at",
  [(D, "    if (job != nullptr && !job->levels.isEmpty()) {\n        const int at", "    if (false) {\n        const int at")], [CHECK]),
 ("a check kept past an edit of the model",
  [(D, "        a_convergence.reset();\n    if (!unchanged) {", "        ;\n    if (!unchanged) {")], [CHECK]),
 ("rf25_converge's meshes taken in the order given",
  [(C, "        std::sort(request.levels.begin(), request.levels.end());\n", "")], [TOOLS]),
 ("rf25_solve's 'adaptive' not passed on",
  [(C, "        request.adaptive = args.value(QLatin1String(\"adaptive\")).toBool() ? 1 : 0;", "        request.adaptive = -1;")], [TOOLS]),
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
