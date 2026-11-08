#!/usr/bin/env python3
"""breaks.py: the fix of the layout tab's crash at closing undone on purpose,
one menu at a time - each menu put back as it was (a QMenu on the slot's
stack shown with exec(), or one of the heap shown with exec() and freed
after it), built, and its test run; a break is caught when the test fails or
the program dies. Sources are put back whatever happens. BRK_ONLY_CHECK=1
checks the anchors; BRK_ONLY=1,3 runs those alone; BRK_BUILD=<dir> the build
tree (build-asan by default: ASan names the bad free); HUNT_RUNS=<dir> for
logs."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = W + "/qucs-s-26.1.1/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-asan")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/layout-close-crash-breaks")
os.makedirs(RUNS, exist_ok=True)

NEW = "        auto* menu = new QMenu(this);\n"
STACK = "        QMenu stack(this);\n        auto* menu = &stack;\n"
LAYOUT = ("test_layout_doc", "aTabClosedWithItsMenuUpTakesTheMenuWithIt")
MENU = "aTabClosedWithItsMenuUpTakesTheMenuWithIt"

def stack(f, shown):
    """\a f's menu on the stack again, shown with exec() at \a shown."""
    return [(f, NEW, STACK), (f, "qucs_s::popUp(menu, %s);" % shown, "menu->exec(%s);" % shown)]

def heap(f, shown, indent="        "):
    """\a f's menu shown with exec() at \a shown, freed after it."""
    return [(f, "qucs_s::popUp(menu, %s);" % shown, "menu->exec(%s);\n%smenu->deleteLater();" % (shown, indent))]

BREAKS = [
 ("the layout's layers' menu on the stack (the crash of 7 November)",
  [("layoutdoc.cpp", NEW + "        menu->addAction(tr(\"Show All\")", STACK + "        menu->addAction(tr(\"Show All\")"),
   ("layoutdoc.cpp", "qucs_s::popUp(menu, a_layers->viewport()->mapToGlobal(at));", "menu->exec(a_layers->viewport()->mapToGlobal(at));")],
  [LAYOUT]),
 ("the layout's canvas's menu on the stack",
  [("layoutdoc.cpp", NEW + "        QAction* copy = menu->addAction(tr(\"Copy\")", STACK + "        QAction* copy = menu->addAction(tr(\"Copy\")"),
   ("layoutdoc.cpp", "qucs_s::popUp(menu, at);", "menu->exec(at);")],
  [LAYOUT]),
 ("the picture's menu on the stack", stack("imagedoc.cpp", "at"), [("test_image_doc", MENU)]),
 ("the PDF page's menu on the stack", stack("pdfdoc.cpp", "at"), [("test_pdf_viewer", MENU)]),
 ("the slide's menu on the stack",
  [("pptxdoc.cpp", "    auto* menu = new QMenu(this);\n    fillMenu(menu);", "    QMenu stack(this);\n    auto* menu = &stack;\n    fillMenu(menu);"),
   ("pptxdoc.cpp", "qucs_s::popUp(menu, globalPos);", "menu->exec(globalPos);")],
  [("test_pptx_doc", MENU)]),
 ("the XML tree's menu shown with exec() and freed after it",
  heap("xmldoc.cpp", "a_tree->viewport()->mapToGlobal(pos)"), [("test_xml_doc", MENU)]),
 ("the History's commit menu shown with exec() and freed after it",
  heap("githistory.cpp", "a_view->viewport()->mapToGlobal(pos)"),
  [("test_git_integration", "theHistoryClosedWithItsMenuUpTakesTheMenuWithIt")]),
 ("the multiphysics view's menu on the stack",
  [("multiphysicsview.cpp", "    auto* menu = new QMenu(this);\n    menu->addAction(tr(\"Fit\")", "    QMenu stack(this);\n    auto* menu = &stack;\n    menu->addAction(tr(\"Fit\")"),
   ("multiphysicsview.cpp", "qucs_s::popUp(menu, event->globalPos());", "menu->exec(event->globalPos());")],
  [("test_multiphysics_view", "theViewFreedWithItsMenuUpTakesTheMenuWithIt")]),
]

def apply(edits, undo=False):
    for f, fixed, broken in edits:
        path = Q + f
        text = open(path, encoding="utf-8").read()
        a, b = (broken, fixed) if undo else (fixed, broken)
        if text.count(a) != 1:
            raise SystemExit("%s: the anchor is there %d times: %r" % (f, text.count(a), a[:80]))
        open(path, "w", encoding="utf-8").write(text.replace(a, b))

# (ctest's environment for the tests, qucs_add_test's: offscreen, no claude or gh of the machine's;
# run on the display, a test can open a native dialog nothing answers)
ENV = dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", QUCS_CLAUDE="/nonexistent/claude",
           QUCS_GH="/nonexistent/gh", QTEST_FUNCTION_TIMEOUT="300000")

def run(test, function, log):
    with open(log, "w") as out:
        if subprocess.call(["ninja", "-C", B, test], stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL) != 0:
            return "build failed"
        code = subprocess.call(["./" + test, function], cwd=B + "/qucs/tests", stdout=out, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL, timeout=900, env=ENV)
    return "passed" if code == 0 else "failed (%d)" % code

only = {int(x) for x in os.environ.get("BRK_ONLY", "").split(",") if x}
caught = 0
chosen = [(i, b) for i, b in enumerate(BREAKS, 1) if not only or i in only]
for i, (what, edits, tests) in chosen:
    apply(edits)
    try:
        if os.environ.get("BRK_ONLY_CHECK"):
            print("%d. %s: anchors there" % (i, what))
            continue
        results = [run(t, f, os.path.join(RUNS, "%d-%s.log" % (i, t))) for t, f in tests]
    finally:
        apply(edits, undo=True)
    ok = any(r.startswith("failed") for r in results)
    caught += ok
    print("%d. %s: %s - %s" % (i, what, ", ".join(results), "caught" if ok else "NOT CAUGHT"), flush=True)
if not os.environ.get("BRK_ONLY_CHECK"):
    print("%d of %d caught" % (caught, len(chosen)))
    # (the fixed sources built again, as they are)
    subprocess.call(["ninja", "-C", B] + sorted({t for _, _, ts in BREAKS for t, _ in ts}), stdout=subprocess.DEVNULL,
                    stdin=subprocess.DEVNULL)
    sys.exit(0 if caught == len(chosen) else 1)
