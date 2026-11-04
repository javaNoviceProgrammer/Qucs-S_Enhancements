#!/usr/bin/env python3
"""breaks.py: RF 2.5D > Settings… (rf25settings.cpp, rf25/rf25_fullwave.cpp)
broken on purpose, one break at a time - the solver not reading the dense
limit or the sweep's memory, the memory estimate with the LU's copy again,
Automatic's memory the old fixed one, the dialog not keeping or not giving
a setting, its Cancel keeping them, its count of frequencies at a time, the
environment not said, the menu item given a role, the refusal not naming
the setting - each built and its tests run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors (writing each break and putting
it back); BRK_ONLY=3,7 runs those alone; HUNT_RUNS=<dir> for logs;
BRK_BUILD=<dir> the build (build)."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = W + "/qucs-s-26.1.1/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-settings-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

F = "rf25/rf25_fullwave.cpp"
D = "rf25settings.cpp"
M = "qucs_rf25.cpp"
DOC = ("test_rf25_doc", "theSettingsHaveTheLuTheLimitAndTheMemory")
SETTINGS = ("test_rf25_fullwave", "theDenseLimitAndTheSweepMemoryAreTheSettings")
CANNOT = ("test_rf25_fullwave", "whatItCannotSolveIsSaid")

BREAKS = [
 # ---- the solver
 ("the dense limit setting not read: the build's own always",
  [(F, "control->mostUnknowns : denseLimit();", "control->mostUnknowns : kMostUnknowns;")],
  [SETTINGS]),
 ("the sweep's memory setting not read",
  [(F, "std::max(1, int(sweepMemory() / perFrequency))", "std::max(1, int(kSweepMemory / perFrequency))")],
  [SETTINGS]),
 ("a frequency's memory reckoned with the LU's copy again",
  [(F, "const double perFrequency = 16.0 * (double(nr) * nr + double(nc) * nc) + 1e6;",
       "const double perFrequency = 16.0 * (2.0 * double(nr) * nr + double(nc) * nc) + 1e6;")],
  [SETTINGS]),
 ("Automatic's memory the old fixed 1.5 GB",
  [(F, "    return std::max(kSweepMemory, physicalMemory() / 4);", "    return kSweepMemory;")],
  [SETTINGS]),
 ("the refusal not naming the setting",
  [(F, "cell - or hold it compressed (the study's Matrix), or raise the limit (RF 2.5D > \"\n                           \"Settings…).\")",
       "cell - or hold it compressed (the study's Matrix).\")")],
  [CANNOT]),
 # ---- the dialog
 ("OK not keeping the limit",
  [(D, "        settings.setValue(kLimitKey, a_limit->value());\n", "")],
  [DOC]),
 ("the settings not given the solver: the LU",
  [(D, "    setDenseLu(c == 1 ? DenseLu::Eigen : c == 2 ? DenseLu::Accelerate : DenseLu::Automatic);\n", "")],
  [DOC]),
 ("the settings not given the solver: the limit",
  [(D, "    setDenseLimit(limitWanted());\n", "")],
  [DOC]),
 ("the settings not given the solver: the memory",
  [(D, "    setSweepMemory(memoryWanted() * 1e9);\n", "")],
  [DOC]),
 ("a change kept as it is made, Cancel keeping it",
  [(D, "    connect(a_limit, &QSpinBox::valueChanged, this, &RF25SettingsDialog::showStatus);\n",
       "    connect(a_limit, &QSpinBox::valueChanged, this, &RF25SettingsDialog::showStatus);\n"
       "    connect(a_limit, &QSpinBox::valueChanged, this, &RF25SettingsDialog::apply);\n")],
  [DOC]),
 ("the limit box not showing the setting",
  [(D, "    a_limit->setValue(limitWanted());\n", "")],
  [DOC]),
 ("the frequencies at a time counted with the LU's copy",
  [(D, "    const double each = 16.0 * (runs * runs + cells * cells) + 1e6;",
       "    const double each = 16.0 * (2.0 * runs * runs + cells * cells) + 1e6;")],
  [DOC]),
 ("QUCS_FEM_SOLVER=eigen not said",
  [(D, "    if (qgetenv(\"QUCS_FEM_SOLVER\").trimmed().toLower() == \"eigen\") {",
       "    if (false) {")],
  [DOC]),
 ("the menu item given a role (on macOS taken into the application's menu)",
  [(M, "  settings->setMenuRole(QAction::NoRole);\n", "")],
  [DOC]),
]


def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if p not in back:
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
                    for k in ("QUCS_TEST_SLOW", "QUCS_RF25_LU", "QUCS_FEM_SOLVER"):
                        env.pop(k, None)
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
