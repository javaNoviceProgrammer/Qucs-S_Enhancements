#!/usr/bin/env python3
"""breaks.py: each part of the Workspaces round undone on purpose, to see
its test fail. The sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors alone. HUNT_RUNS=<dir> for the logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/workspaces-breaks")
os.makedirs(RUNS, exist_ok=True)

BREAKS = [
 ("the project open now is opened again, closing every tab",
  [("qucs.cpp",
    "  if (!ProjName.isEmpty() && QDir(QucsSettings.QucsWorkDir).canonicalPath() == real) {",
    "  if (false) {")],
  ["test_project_folders"], ["test_project_folders:theProjectOpenIsNotOpenedAgain"]),

 ("it leaves the dock where it was instead of showing the contents",
  [("qucs.cpp",
    "    showSidePage(SidePage::Content);\n    statusBar()->showMessage(tr(\"%1 is the project open already.\")",
    "    (void) 0;\n    statusBar()->showMessage(tr(\"%1 is the project open already.\")")],
  ["test_project_folders"], ["test_project_folders:theProjectOpenIsNotOpenedAgain"]),

 ("another project no longer closes what is open",
  [("qucs.cpp",
    "  if (!ProjName.isEmpty() && QDir(QucsSettings.QucsWorkDir).canonicalPath() == real) {",
    "  if (!ProjName.isEmpty()) {")],
  ["test_project_folders"], ["test_project_folders:theProjectOpenIsNotOpenedAgain"]),

 ("the workspace is not remembered",
  [("qucs_workspace.cpp",
    "  if (!now.isEmpty() && !QucsSettings.RecentWorkspaces.contains(now)) rememberWorkspace(now);",
    "  if (false) rememberWorkspace(now);")],
  ["test_project_folders"], ["test_project_folders:theWorkspacesPanelKeepsItsHistory"]),

 ("the history is oldest first",
  [("qucs_workspace.cpp",
    "    QucsSettings.RecentWorkspaces.prepend(path);",
    "    QucsSettings.RecentWorkspaces.append(path);")],
  ["test_project_folders"], ["test_project_folders:theWorkspacesPanelKeepsItsHistory"]),

 ("the workspace open now has no dot of its own",
  [("qucs_workspace.cpp",
    "    const bool open = !nowReal.isEmpty() ? QFileInfo(dir).canonicalFilePath() == nowReal : dir == now;",
    "    const bool open = false;")],
  ["test_project_folders"], ["test_project_folders:theWorkspacesPanelKeepsItsHistory"]),

 ("a workspace that is gone says nothing of it",
  [("qucs_workspace.cpp",
    "    const bool there = QFileInfo(dir).isDir();",
    "    const bool there = true;")],
  ["test_project_folders"], ["test_project_folders:theWorkspacesPanelKeepsItsHistory"]),

 ("the workspace open now can be forgotten",
  [("qucs_workspace.cpp",
    "    if (QDir(dir).canonicalPath() == QucsSettings.qucsWorkspaceDir.canonicalPath()) {\n      statusBar()->showMessage(tr(\"The workspace open now stays in the list.\"), 5000);\n      return;\n    }",
    "    if (false) {\n      statusBar()->showMessage(tr(\"The workspace open now stays in the list.\"), 5000);\n      return;\n    }")],
  ["test_project_folders"], ["test_project_folders:theWorkspacesPanelKeepsItsHistory"]),

 ("the history is not written to the settings",
  [("qucs_workspace.cpp",
    "  QSettings *settings = new QucsSettingsFile;\n  settings->setValue(QStringLiteral(\"RecentWorkspaces\")",
    "  QSettings *settings = new QucsSettingsFile;\n  if (false) settings->setValue(QStringLiteral(\"RecentWorkspaces\")")],
  ["test_project_folders"], ["test_project_folders:theWorkspacesPanelKeepsItsHistory"]),

 ("a page is kept by number again, so a session means another page",
  [("workspacesession.cpp",
    '                       {QStringLiteral("side page"), sidePage},',
    '                       {QStringLiteral("side page"), QString()},')],
  ["test_workspace_session"], ["test_workspace_session:theSidePageIsKeptByName"]),

 ("an old session's number is read as the new order",
  [("workspacesession.cpp",
    'static const QStringList was{QStringLiteral("projects"), QStringLiteral("content"), QStringLiteral("components"),',
    'static const QStringList was{QStringLiteral("workspaces"), QStringLiteral("projects"), QStringLiteral("content"),')],
  ["test_workspace_session"], ["test_workspace_session:theSidePageIsKeptByName"]),

 ("two pages answer to one name",
  [("qucs_workspace.cpp",
    '  case SidePage::Libraries: return QStringLiteral("libraries");',
    '  case SidePage::Libraries: return QStringLiteral("content");')],
  ["test_workspace_session"], ["test_workspace_session:theSidePageIsKeptByName"]),
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
                    said.append("%s %s" % (t, "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-58s %s" % (name[:58], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
