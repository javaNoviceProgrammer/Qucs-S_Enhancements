#!/usr/bin/env python3
"""breaks.py: each part of this round undone on purpose, to see its test
fail. Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks the
anchors alone; HUNT_RUNS=<dir> says where the logs go."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/icons-breaks")
os.makedirs(RUNS, exist_ok=True)

BREAKS = [
 ("a workspace row has no icon",
  [("qucs_workspace.cpp", "        item->setIcon(folder);", "        if (false) item->setIcon(folder);")],
  ["test_project_folders"], ["test_project_folders:theWorkspacesPanelKeepsItsHistory"]),

 ("the open workspace, double-clicked, stays where it is",
  [("qucs_workspace.cpp",
    "      showSidePage(SidePage::Projects);\n      statusBar()->showMessage(tr(\"%1 is the workspace already.\")",
    "      (void) 0;\n      statusBar()->showMessage(tr(\"%1 is the workspace already.\")")],
  ["test_project_folders"], ["test_project_folders:theWorkspacesPanelKeepsItsHistory"]),

 ("a project row says nothing of its state",
  [("qucs.cpp",
    "        if (role == RowStateRole)\n            return state == NoProject ? QString()",
    "        if (false)\n            return state == NoProject ? QString()")],
  ["test_qucs_control", "test_project_folders"],
  ["test_qucs_control:theOpenRowIsSaidAndTheOpenProjectIsNotOpenedAgain",
   "test_project_folders:theOpenProjectHasAGreenDot"]),

 ("a project that is not there is said to be closed",
  [("qucs.cpp",
    '                 : qucs_s::workspace::isDanglingLink(path) ? QStringLiteral("missing")\n',
    '')],
  ["test_project_folders"], ["test_project_folders:theOpenProjectHasAGreenDot"]),

 ("a workspace row says nothing of its state",
  [("qucs_workspace.cpp",
    '    item->setData(QucsFileSystemModel::RowStateRole,\n                  !there ? QStringLiteral("missing") : open ? QStringLiteral("open") : QStringLiteral("closed"));',
    '    if (false) item->setData(QucsFileSystemModel::RowStateRole, QString());')],
  ["test_qucs_control"], ["test_qucs_control:theOpenRowIsSaidAndTheOpenProjectIsNotOpenedAgain"]),

 ("a workspace whose folder has gone is said to be closed",
  [("qucs_workspace.cpp",
    '                  !there ? QStringLiteral("missing") : open ? QStringLiteral("open") : QStringLiteral("closed"));',
    '                  open ? QStringLiteral("open") : QStringLiteral("closed"));')],
  ["test_qucs_control"], ["test_qucs_control:theOpenRowIsSaidAndTheOpenProjectIsNotOpenedAgain"]),

 ("get_ui leaves a plain list's states out",
  [("qucscontrol.cpp",
    '            if (!states.isEmpty()) o.insert(QStringLiteral("state"), states);',
    '            if (false) o.insert(QStringLiteral("state"), states);')],
  ["test_qucs_control"], ["test_qucs_control:theOpenRowIsSaidAndTheOpenProjectIsNotOpenedAgain"]),

 ("get_ui leaves a view's row states out",
  [("qucscontrol.cpp",
    '                if (const QString state = index.data(QucsFileSystemModel::RowStateRole).toString(); !state.isEmpty())\n                    row.insert(QStringLiteral("state"), state);',
    '                if (false)\n                    row.insert(QStringLiteral("state"), QString());')],
  ["test_qucs_control"], ["test_qucs_control:theOpenRowIsSaidAndTheOpenProjectIsNotOpenedAgain"]),

 ("open_project answers as for a fresh open, and refuses over unsaved changes",
  [("qucscontrol_design.cpp",
    '    if (!a_app->ProjName.isEmpty() && QDir(QucsSettings.QucsWorkDir).canonicalPath() == real) {',
    '    if (false) {')],
  ["test_qucs_control"], ["test_qucs_control:theOpenRowIsSaidAndTheOpenProjectIsNotOpenedAgain"]),
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
    results = []
    for i, (name, edits, targets, tests) in enumerate(BREAKS, 1):
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            if only:
                results.append((name, "anchors ok"))
                continue
            log = os.path.join(RUNS, "brk%02d.log" % i)
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + targets, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for t in tests:
                    suite, fn = t.split(":")
                    r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out, stderr=subprocess.STDOUT,
                                       env=dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0"))
                    said.append("%s %s" % (fn[:28], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-62s %s" % (name[:62], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
