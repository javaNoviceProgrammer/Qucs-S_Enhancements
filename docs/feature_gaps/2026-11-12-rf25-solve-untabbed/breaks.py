#!/usr/bin/env python3
"""breaks.py: RF 2.5D's solve lifted out of its tab (12 November) undone on
purpose, one piece at a time - the mesh made as the tab makes it, the stop,
the length a line is said to have, the folder a TRL calibration's standards are
found from, the mesh's note, why there is no mesh, a convergence check refused
- each built and its tests run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=3 runs that
alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the checkout's
build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-solve-untabbed-breaks")

S = "rf25/rf25_solve.cpp"
T = "rf25doc.cpp"
MINE = ("test_rf25_doc", "aModelSolvedWithoutItsTabIsTheTabsAnswer")
TRL = ("test_rf25_doc", "aTrlCalibrationTakesThePadsOut")
NOTE = ("test_rf25_doc", "anAdaptiveSweepSolvesFewOfItsFrequencies")

BREAKS = [
 ("a model solved without its tab meshed by its Mesh node alone",
  [(S, "    const MeshSettings how = tabMeshSettings(model, &said);\n    auto mesh = std::make_shared<Mesh>(",
       "    const MeshSettings how = meshSettingsOf(model.mesh(), model.parameters(), &said);\n    auto mesh = std::make_shared<Mesh>(")],
  [MINE]),
 ("its line's length the longer side solved, never the line's own",
  [(S, "        out.length = job.result && job.result->perMetre.fromMiddle && job.result->perMetre.length > 0 ? job.result->perMetre.length : job.length;",
       "        out.length = job.length;")],
  [MINE]),
 ("the tab's TRL standards found from no folder",
  [(T, "        const QString problem = setUpSolve(*job, a_model, *a_processed, *a_mesh, request, dir.absolutePath());",
       "        const QString problem = setUpSolve(*job, a_model, *a_processed, *a_mesh, request, QString());")],
  [TRL]),
 ("a solve's lines without its mesh's note first",
  [(S, "        if (job.wave->ok()) job.wave->lines.prepend(meshNoteOf(job.mesh));", "")],
  [NOTE]),
 ("no mesh, without a word of why",
  [(S, "        QStringList why{tr(\"There is no mesh to solve on: give the model a layout and a layer map first.\")};",
       "        QStringList why{tr(\"Nothing.\")};")],
  [MINE]),
 ("a convergence check taken as a solve",
  [(S, "    if (!request.levels.isEmpty()) {\n        out.problem = tr(", "    if (false) {\n        out.problem = tr(")],
  [MINE]),
 ("a stop set before it begins solved all the same",
  [(S, "    if (job.stop.load()) {\n        out.stopped = true;\n        return out;\n    }\n    runSolve(job);\n    out.stopped = job.stop.load();",
       "    runSolve(job);\n    out.stopped = false;")],
  [MINE]),
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
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn, "-platform", "offscreen"], stdout=out,
                                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=1200, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    # The suites built again from the sources as they are: a break's build
    # left behind fails a later run for nothing.
    subprocess.run(["ninja", "-C", B] + sorted({t[0] for _, _, tests in BREAKS for t in tests}),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
