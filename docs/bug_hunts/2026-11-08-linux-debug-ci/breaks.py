#!/usr/bin/env python3
"""breaks.py: the fixes of 2026-11-08-linux-debug-ci.md undone on purpose, one
at a time - each put back as it was, built, and its test run through ctest
(a scratch CTestTestfile with qucs_add_test's environment: offscreen, no
claude or gh of the machine's); a break is caught when the test fails. Sources
are put back whatever happens. BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=1,3
runs those alone; BRK_BUILD=<dir> the build tree (build, a Release one, by
default); HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = W + "/qucs-s-26.1.1/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/linux-debug-ci-breaks")
os.makedirs(RUNS, exist_ok=True)

TIE_MESH = ("multiphysics/fem_mesh.cpp",
            "return x.priority != y.priority ? x.priority > y.priority : x.tri < y.tri;",
            "return x.priority > y.priority;")
# (The adaptive step's own sort, fem_solver.cpp, broken the same way: undone, adaptiveStepsStillWork
# still makes 1107 with libc++ and with libstdc++ - no tie there on these meshes - so it is not a break
# a test can catch, and not listed.)

BREAKS = [
 ("the Route panel left to ~QWidget's children (UBSan's downcast on Linux)",
  [("layoutdoc.cpp", "    delete a_routerPanel;\n    a_routerPanel = nullptr;\n}", "    // (the panel left to ~QWidget: breaks.py)\n}")],
  [("test_layout_routing", "thePanelGoesBeforeItsDocument")]),
 ("the digitizer's PDFs left to the program's end (the abort after the last test on Linux)",
  [("qucscontrol_digitizer.cpp",
    "        qAddPostRoutine([] {\n            delete openedCache();\n            openedCache() = nullptr;\n        });\n",
    "        // (left to the program's end: breaks.py)\n")],
  [("test_plot_digitizer_app", "keptAndReadBack")]),
 ("the mesher's candidates sorted by priority alone (the meshes of each standard library)",
  [TIE_MESH], [("test_fem_adapt", "adaptiveStepsStillWork")]),
 ("settled on the last mesh allowed, said as not settled",
  [("multiphysics/fem_adapt.cpp", "            if (checking)\n                stop(", "            if (false)\n                stop(")],
  [("test_fem_adapt", "settledOnTheLastMeshIsSaid")]),
 ("the OpenGL check's surface freed before its QRhi (Application Settings ended the program on Linux)",
  [("rhicanvas.cpp",
    "    std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());\n"
    "    QRhiGles2InitParams params;\n"
    "    params.fallbackSurface = surface.get();\n"
    "    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &params));\n",
    "    std::unique_ptr<QRhi> rhi;\n"
    "    QRhiGles2InitParams params;\n"
    "    std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());\n"
    "    params.fallbackSurface = surface.get();\n"
    "    rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));\n")],
  [("test_gpu_probe", "theOpenGLCheckEndsCleanly")]),
]

def apply(edits, undo=False):
    # (Every anchor checked first, both ways: an edit that could not be undone
    # would leave the source broken.)
    for f, fixed, broken in edits:
        text = open(Q + f, encoding="utf-8").read()
        a = broken if undo else fixed
        if text.count(a) != 1 or not broken or (not undo and text.count(broken) != 0):
            raise SystemExit("%s: the anchor is there %d times: %r" % (f, text.count(a), a[:80]))
    for f, fixed, broken in edits:
        path = Q + f
        text = open(path, encoding="utf-8").read()
        a, b = (broken, fixed) if undo else (fixed, broken)
        if text.count(a) != 1:
            raise SystemExit("%s: the anchor is there %d times: %r" % (f, text.count(a), a[:80]))
        open(path, "w", encoding="utf-8").write(text.replace(a, b))

ENV = "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;" \
      "QUCS_CLAUDE=/nonexistent/claude;QUCS_GH=/nonexistent/gh"

def run(test, function, log):
    d = os.path.join(RUNS, "%s-%s" % (test, function))
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "CTestTestfile.cmake"), "w") as f:
        f.write('add_test(t "%s/qucs/tests/%s" "%s")\n' % (B, test, function))
        f.write('set_tests_properties(t PROPERTIES ENVIRONMENT "%s" TIMEOUT 1800)\n' % ENV)
    with open(log, "w") as out:
        if subprocess.call(["ninja", "-C", B, test], stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL) != 0:
            return "build failed"
        code = subprocess.call(["ctest", "--test-dir", d, "--output-on-failure"], stdout=out, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL, env=dict(os.environ, HOME=d))
    return "passed" if code == 0 else "failed (%d)" % code

only = {int(x) for x in os.environ.get("BRK_ONLY", "").split(",") if x}
caught = 0
asked = 0
for i, (what, edits, tests) in enumerate(BREAKS, 1):
    if only and i not in only:
        continue
    asked += 1
    apply(edits)
    try:
        if os.environ.get("BRK_ONLY_CHECK"):
            print("%d. %s: anchors there" % (i, what))
            continue
        results = [run(t, fn, os.path.join(RUNS, "%d-%s-%s.log" % (i, t, fn))) for t, fn in tests]
    finally:
        apply(edits, undo=True)
    ok = all(r.startswith("failed") for r in results)
    caught += ok
    print("%d. %s: %s - %s" % (i, what, ", ".join(results), "caught" if ok else "NOT CAUGHT"))
    sys.stdout.flush()
if not os.environ.get("BRK_ONLY_CHECK"):
    print("%d of %d caught" % (caught, asked))
    # The tree as it was, built again.
    for t in sorted({t for _, _, ts in BREAKS for t, _ in ts}):
        subprocess.call(["ninja", "-C", B, t], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
