#!/usr/bin/env python3
"""breaks.py: the full-wave gap grading of 4 November undone on purpose, one
piece at a time - the tab's settings, the calibration strips meshed as the
structure, what the mesh and the calibration say of it, the grading itself -
each built and its tests run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (build-rf25);
BRK_SOURCE=<dir> a copy of qucs-s-26.1.1 to break instead of the checkout's
(where other work builds from the checkout meanwhile), BRK_BUILD its build."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rf25")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-fullwave-gaps-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

D = "rf25doc.cpp"
M = "rf25/rf25_mesh.cpp"
E = "rf25/rf25_deembed.cpp"
TAB = ("test_rf25_doc", "aQuasiStaticSolveSaysWhatItDoesAndStops")
COPLANAR = ("test_rf25_doc", "aCoplanarPortIsReadAndItsFeedCalibrated")
MATRIX = ("test_rf25_doc", "aFullWaveStudysMatrixIsItsOwn")
MEASURED = ("test_rf25_measured", "aCoplanarLineOnAluminaIsTheMeasuredOne")
GAPS = ("test_rf25_mesh", "aSideFacingANarrowGapIsGradedToAQuarterOfIt")

BREAKS = [
 ("the tab grading a quasi-static study's gaps alone, as before",
  [(D, "    how.gaps = true;\n    return how;",
       "    const Node* study = model.root.child(QStringLiteral(\"rf_study\"));\n"
       "    how.gaps = !(study != nullptr && textOf(*study, QStringLiteral(\"kind\")) == QLatin1String(\"full_wave\"));\n    return how;")],
  [TAB, COPLANAR]),
 ("the calibration strips meshed without the gaps, the structure with them",
  [(D, "        job->how = tabMeshSettings(a_model, &said);\n", "        job->how = tabMeshSettings(a_model, &said);\n        job->how.gaps = false;\n")],
  [COPLANAR]),
 ("the structure meshed without the gaps, its strips with them",
  [(D, "        const MeshSettings how = tabMeshSettings(job->model, &said);\n",
       "        MeshSettings how = tabMeshSettings(job->model, &said);\n        how.gaps = false;\n")],
  [TAB, COPLANAR]),
 ("the tab's settings not held compressed for a compressed study",
  [(D, "    how.compressed = meshedCompressed(model);\n    how.gaps = true;", "    how.gaps = true;")],
  [MATRIX]),
 ("the strips' grading not said",
  [(E, "        out.facing = std::max(out.facing, mesh.facing);\n", "")],
  [COPLANAR]),
 ("the mesh not keeping how many sides it graded",
  [(M, "    mesh.facing = facing;\n", "")],
  [COPLANAR]),
 ("no gap graded at all",
  [(M, "    if (how.edges && how.gaps) {", "    if (false && how.edges && how.gaps) {")],
  [MEASURED, GAPS, COPLANAR]),
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
                               QUCS_SETTINGS_DIR=os.path.join(RUNS, "settings"), QUCS_TRASH_DIR=os.path.join(RUNS, "trash"),
                               QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh", QTEST_FUNCTION_TIMEOUT="1800000")
                    env.pop("QUCS_TEST_SLOW", None)
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=1800, env=env, cwd=RUNS)
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


if __name__ == "__main__":
    sys.exit(main())
