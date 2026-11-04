#!/usr/bin/env python3
"""breaks.py: the fixes of the thin-layer review of 3 November undone on
purpose, one at a time - the image series' bins, their cubic, the squaring,
the floor, the bound and the stop, the integral's grading and its averaged
end, the extraction's stop and its steps, rf25_solve's joining, rf25_stop,
rf25_inductance's joining, the gaps' grading, the charges counted and the
loss solves spared - each built and its tests run. Sources are put back
whatever happens. BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs
those alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (build-rf25)."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = W + "/qucs-s-26.1.1/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rf25")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-thin-layer-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

G = "rf25/rf25_green.cpp"
S = "rf25/rf25_static.cpp"
M = "rf25/rf25_mesh.cpp"
D = "rf25doc.cpp"
R = "qucscontrol_rf25.cpp"
FILM = ("test_rf25_green", "aThinFilmOfHighPermittivityIsBuiltInBoundedWork")
ACROSS = ("test_rf25_green", "aChargeSeenAcrossAThinFilmIsBuiltToo")
CAVITY = ("test_rf25_green", "aCavityThatReflectsNearlyEverythingConverges")
SETTLE = ("test_rf25_green", "aStackThatDoesNotSettleSaysWhyAndAStopStops")
INTEGRAL = ("test_rf25_green", "theIntegralIsGradedAndItsSeriesSummed")
LINE = ("test_rf25_static", "aCoplanarLineOnThinFilmLithiumNiobate")
STOPS = ("test_rf25_static", "anExtractionStopsWhenItIsStopped")
LOSS = ("test_rf25_sparams", "theLossMatrixCouplesAsWellAsLoads")
GAPS = ("test_rf25_mesh", "aSideFacingANarrowGapIsGradedToAQuarterOfIt")
TAB = ("test_rf25_doc", "aQuasiStaticSolveSaysWhatItDoesAndStops")
HOWFAR = ("test_rf25_doc", "aSolveShowsHowFarItHasGot")
TOOL = ("test_qucs_control", "anRf25SolveGoingOnIsJoinedAndStopped")

BREAKS = [
 # ---- the image series
 ("every length kept apart, no bins (the series path by path)",
  [(G, "        if (a_bins.empty() || z <= (a_lost != nullptr ? a_lost->zExact : 0.0)) {",
       "        if (true || a_bins.empty() || z <= (a_lost != nullptr ? a_lost->zExact : 0.0)) {")],
  [FILM, ACROSS]),
 ("a crowded bin's terms to two nodes by a straight line, not four by a cubic",
  [(G, "            double w = 1;\n            for (int n = 0; n < 4; ++n)\n                if (n != m) w *= (z - at[n]) / (at[m] - at[n]);\n",
       "            double w = m == 1 ? (at[2] - z) / (at[2] - at[1]) : m == 2 ? (z - at[1]) / (at[2] - at[1]) : 0.0;\n")],
  [FILM, ACROSS]),
 ("the geometric series a bounce at a time, two thousand of them",
  [(G, "    Series out{{1.0, 0.0}}, power = x;\n",
       "    Series out{{1.0, 0.0}}, power{{1.0, 0.0}};\n    for (int n = 0; n < 2000; ++n) {\n        power = times(power, x, lost);\n"
       "        if (power.isEmpty()) return tidied(std::move(out), lost);\n        double biggest = 0;\n"
       "        for (const auto& t : std::as_const(power)) biggest = std::max(biggest, std::abs(t.first));\n"
       "        out += power;\n        if (biggest < kTermFloor) return tidied(std::move(out), lost);\n    }\n"
       "    if (lost != nullptr)\n        for (const auto& t : std::as_const(power)) lost->lose(t.first, t.second);\n"
       "    return tidied(std::move(out), lost);\n")],
  [CAVITY]),
 ("the floor at 1e-13 again",
  [(G, "constexpr double kTermFloor = 1e-16;", "constexpr double kTermFloor = 1e-13;")],
  [FILM]),
 ("no bound on what the expansion may cost",
  [(G, "        if ((mostPairs > 0 && pairs > mostPairs) || (stop != nullptr && stop->load())) over = true;",
       "        if (stop != nullptr && stop->load()) over = true;")],
  [SETTLE]),
 ("the stop not looked at as the expansion goes",
  [(G, "        if ((mostPairs > 0 && pairs > mostPairs) || (stop != nullptr && stop->load())) over = true;",
       "        if (mostPairs > 0 && pairs > mostPairs) over = true;")],
  [SETTLE, STOPS]),
 ("the layer likeliest why not named",
  [(G, "    if (worst < 0) return QString();", "    if (true) return QString();")],
  [SETTLE]),
 # ---- the integral
 ("the far branch's intervals one panel each",
  [(G, "        sum += over(a, std::min(b, kMax));", "        sum += gauss(integrand, a, std::min(b, kMax));")],
  [INTEGRAL]),
 ("the plain sum where the intervals run out",
  [(G, "    integral = covered || partial.size() < 8 ? sum : averaged(partial);",
       "    integral = covered || partial.size() < 8 || partial.size() == 4000 ? sum : averaged(partial);")],
  [INTEGRAL, FILM]),
 # ---- the extraction
 ("the tab's solve not handed its stop",
  [(D, "extract(job->mesh, job->conductors, job->stack, 1e-6, job->returns, &control));",
       "extract(job->mesh, job->conductors, job->stack, 1e-6, job->returns, nullptr));")],
  [TAB]),
 ("the matrix filled to its end when stopped",
  [(S, "        // (Stopped: the rows not filled are nothing, and the caller,\n        //  seeing it stopped, reads none of them.)\n"
       "        if (control != nullptr && control->stopped()) {",
       "        if (false) {")],
  [STOPS]),
 ("the stack's Green's functions not handed the stop",
  [(S, "                    control->at(ExtractControl::Greens, built, all);\n                    g.stopWith(control->stop);",
       "                    control->at(ExtractControl::Greens, built, all);")],
  [STOPS, TAB]),
 ("the vacuum's images said as the stack's",
  [(S, "control->images != nullptr && some == &greens) control->images->store(count);",
       "control->images != nullptr) control->images->store(count);")],
  [LINE]),
 ("a uniform line solved whole again for each lossy layer",
  [(S, "            if (!uniform) solve(fromNudged, &nudgedMaxwell);", "            solve(fromNudged, &nudgedMaxwell);")],
  [LINE, LOSS]),
 # ---- the tab
 ("a quasi-static solve's step not said (\"Solving...\")",
  [(D, "    } else if (job != nullptr) {\n        // The quasi-static extraction: what it is at,",
       "    } else if (false && job != nullptr) {\n        // The quasi-static extraction: what it is at,")],
  [TAB, HOWFAR]),
 ("a quasi-static solve's texts not refreshed as it goes",
  [(D, "        // (A quasi-static solve's too, now that it says what it is doing.)\n        if (!a_solveJob) return;",
       "        if (!a_solveJob || !a_solveJob->fullWave) return;")],
  [TAB]),
 ("the status line's \"; solving\" again",
  [(D, "        tail += tr(\"; %1\").arg(extractStepSaid(a_solveJob->step.load(), a_solveJob->stepDone.load(), a_solveJob->stepAll.load(),\n"
       "                                               a_solveJob->images.load()));",
       "        tail += tr(\"; solving\");")],
  [TAB, HOWFAR]),
 ("the status line counting cells, not charges",
  [(D, ": chargeCostOf(a_mesh->charges > 0 ? a_mesh->charges : int(a_mesh->cells.size())));",
       ": chargeCostOf(int(a_mesh->cells.size())));")],
  [TAB]),
 ("the mesh's charges not counted",
  [(M, "    mesh.charges = charges;\n", "")],
  [GAPS, TAB]),
 # ---- the mesh
 ("no side graded past the edges' one halving",
  [(M, "for (double w = d / 2; finestLow > 0 && w > finestLow", "for (double w = d / 2; false && finestLow > 0 && w > finestLow"),
   (M, "for (double w = d / 2; finestHigh > 0 && w > finestHigh", "for (double w = d / 2; false && finestHigh > 0 && w > finestHigh")],
  [GAPS, LINE]),
 ("gaps graded to half of them, not a quarter",
  [(M, "constexpr double kGapShare = 0.25;", "constexpr double kGapShare = 0.5;")],
  [GAPS, LINE]),
 ("the tab's quasi-static mesh not graded at gaps",
  [(D, "        how.gaps = !studyFullWave(job->model);\n", "")],
  [TAB]),
 # ---- the tools
 ("rf25_solve stopping its solve at its timeout",
  [(R, "    if (!model->waitForSolved(timeout * 1000)) {\n        // It goes on: stopped here,",
       "    if (!model->waitForSolved(timeout * 1000)) {\n        model->stopSolving();\n        // It goes on: stopped here,")],
  [TOOL]),
 ("rf25_solve starting again what it would join",
  [(R, "    if (model->isSolving() && !joined)\n        return errorResult(tr(\"%1 is solving already, as it was asked otherwise",
       "    if (false)\n        return errorResult(tr(\"%1 is solving already, as it was asked otherwise"),
   (R, "    if (!joined) {\n        *startedHere = true;", "    if (true) {\n        *startedHere = true;")],
  [TOOL]),
 ("rf25_solve starting a solve asked otherwise beside the one going on",
  [(R, "    if (model->isSolving() && !joined)\n        return errorResult(tr(\"%1 is solving already, as it was asked otherwise",
       "    if (false)\n        return errorResult(tr(\"%1 is solving already, as it was asked otherwise")],
  [TOOL]),
 ("rf25_solve's timeout blaming the mesh whatever it is at",
  [(R, "        if (step == ExtractControl::Greens)\n", "        if (false)\n")],
  [TOOL]),
 ("rf25_inductance beginning a solve of its own",
  [(R, "    if (!model->isSolving()) model->solveWith(quasiStatic);", "    model->solveWith(quasiStatic);")],
  [TOOL]),
 ("rf25_stop not stopping",
  [(R, "    const QString was = model->solvingSaid();\n    model->stopSolving();\n", "    const QString was = model->solvingSaid();\n")],
  [TOOL]),
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
            suites = sorted({t[0] for t in tests})
            targets = ["qucs/tests/" + s for s in suites]
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + targets, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=CACHE, ASAN_OPTIONS="detect_leaks=0",
                               QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh", QTEST_FUNCTION_TIMEOUT="900000")
                    env.pop("QUCS_TEST_SLOW", None)
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=900, env=env, cwd=RUNS)
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
        if "BUILD FAILED" in said or "hung" in said or ("PASSED - NOT CAUGHT" in said and "caught)" not in said):
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":   # (read only, for BREAKS: nothing run)
    sys.exit(main())
