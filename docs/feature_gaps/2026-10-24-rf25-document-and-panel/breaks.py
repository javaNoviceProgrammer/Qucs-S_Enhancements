#!/usr/bin/env python3
"""breaks.py: each part of this round undone on purpose, to see its test
fail. Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks the
anchors alone; HUNT_RUNS=<dir> says where the logs go."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build-rf25"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-breaks")
os.makedirs(RUNS, exist_ok=True)

DOC = ["test_rf25_doc"]

BREAKS = [
 ("the parts of the tree get no tags of their own",
  [("rf25/rf25_model.cpp",
    "        if (n.tag.isEmpty() && k != nullptr && k->fixed) {",
    "        if (false && n.tag.isEmpty() && k != nullptr && k->fixed) {")],
  DOC, ["test_rf25_doc:theTreeIsEditedInStepsOfUndo", "test_rf25_doc:aFileThatIsNoModelIsRefused"]),

 ("a tag two nodes share is left as the file had it",
  [("rf25/rf25_model.cpp",
    "        if (!n.tag.isEmpty() && tags.contains(n.tag)) {",
    "        if (false && !n.tag.isEmpty() && tags.contains(n.tag)) {")],
  ["test_rf25_model"], ["test_rf25_model:everyNodeHasATagOfItsOwn"]),

 ("the root's row is written back into the model as its title",
  [("rf25panel.cpp",
    "        if (k != nullptr && k->fixed) return;",
    "        if (false) return;")],
  DOC, ["test_rf25_doc:itIsSavedAndReadBackTheSame"]),

 ("a layer renamed is not renamed where the model names it",
  [("rf25panel.cpp", "    if (layer) m.renameLayer(old, n->label);", "    if (false) m.renameLayer(old, n->label);")],
  DOC, ["test_rf25_doc:aLayerRenamedIsRenamedWhereItIsNamed"]),

 ("two layers may share a name",
  [("rf25panel.cpp",
    "            if (c.tag != tag && c.label == name) {",
    "            if (false) {")],
  DOC, ["test_rf25_doc:aLayerRenamedIsRenamedWhereItIsNamed"]),

 ("the panel reads a property through the multiphysics table",
  [("rf25panel.cpp",
    "        auto* edit = new QLineEdit(textOf(node, key), a_form);\n        edit->setObjectName(QStringLiteral(\"rfEdit_\") + key);\n        if (!p.unit.isEmpty()) edit->setPlaceholderText(p.unit);",
    "        auto* edit = new QLineEdit(node.text(key), a_form);\n        edit->setObjectName(QStringLiteral(\"rfEdit_\") + key);\n        if (!p.unit.isEmpty()) edit->setPlaceholderText(p.unit);")],
  DOC, ["test_rf25_doc:aPropertyIsEditedAndUndone"]),

 ("a property's showIf is ignored: everything is shown",
  [("multiphysics/fem_model.cpp",
    "    if (p.showIf.isEmpty()) return true;",
    "    if (true) return true;")],
  DOC + ["test_multiphysics_doc"],
  ["test_rf25_doc:whatDependsOnAFlagIsShownWithIt", "test_multiphysics_doc:theSettingsForm"]),

 ("a save is stamped by the clock, not by the file it wrote",
  [("rf25doc.cpp", "    stampedFromFile();", "    setLastSaved(QDateTime::currentDateTime());")],
  DOC, ["test_rf25_doc:aFileChangedOnDiskIsReadAgainAndOneSavedHereIsNot"]),

 ("the suffix is not recognised: a model opens as text",
  [("qucs_rf25.cpp",
    'return QFileInfo(name).suffix().compare(QLatin1String("qrf25"), Qt::CaseInsensitive) == 0;',
    'return false;')],
  DOC, ["test_rf25_doc:aModelOpensFromADropAndFromTheProjectView", "test_rf25_doc:itIsSavedAndReadBackTheSame"]),

 ("the dock's new page has no name for a session to keep",
  [("qucs_workspace.cpp",
    '  case SidePage::Rf25: return QStringLiteral("rf 2.5d");',
    '  case SidePage::Rf25: return QString();')],
  DOC, ["test_rf25_doc:theDockReachesItsPagesByName"]),

 ("the layer map's colours are not used: every layer is drawn alike",
  [("rf25doc.cpp", "        if (at >= 0) {", "        if (false) {")],
  DOC, ["test_rf25_doc:theLayoutIsDrawnInTheStacksColours"]),

 ("the layout file is not read beside the model",
  [("rf25doc.cpp",
    "    const QDir dir = a_DocName.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(a_DocName).absoluteDir();\n    return QDir::cleanPath(dir.absoluteFilePath(named));",
    "    return named;")],
  DOC, ["test_rf25_doc:theLayoutIsDrawnInTheStacksColours"]),

 ("a layer added goes to the end, wherever it belongs in z",
  [("rf25/rf25_model.cpp",
    "    if (index < 0 && parent->type == QLatin1String(\"rf_stack\")) {",
    "    if (false) {")],
  DOC, ["test_rf25_doc:theTreeIsEditedInStepsOfUndo"]),

 ("the window's Edit > Undo does not know this document",
  [("qucs_actions.cpp",
    "  if (auto *rf25 = qobject_cast<RF25Doc *>(DocumentTab->currentWidget())) {\n    rf25->undo();",
    "  if (false) {\n    ((RF25Doc *) nullptr)->undo();")],
  DOC, ["test_rf25_doc:theWindowsEditActionsKnowIt"]),

 ("Zoom In falls through to the schematic's path",
  [("qucs_actions.cpp",
    "  if (isTextDocument(w) || isPdfDocument(w) || isSheetDocument(w) || isArchiveDocument(w) || isLayoutDocument(w)\n      || isImageDocument(w) || isMultiphysicsDocument(w) || isRf25Document(w) || isPresentationDocument(w)) {\n    docIn(w)->zoomBy(1.5f);",
    "  if (isTextDocument(w) || isPdfDocument(w) || isSheetDocument(w) || isArchiveDocument(w) || isLayoutDocument(w)\n      || isImageDocument(w) || isMultiphysicsDocument(w) || isPresentationDocument(w)) {\n    docIn(w)->zoomBy(1.5f);")],
  DOC, ["test_rf25_doc:theWindowsEditActionsKnowIt"]),

 ("a part of the tree itself may be deleted",
  [("rf25/rf25_model.cpp", "    if (k != nullptr && k->fixed) {\n        if (error != nullptr) *error = tr(\"%1 is a part of the tree itself",
    "    if (false) {\n        if (error != nullptr) *error = tr(\"%1 is a part of the tree itself")],
  DOC, ["test_rf25_doc:theTreeIsEditedInStepsOfUndo"]),
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
                    said.append("%s %s" % (fn[:30], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-60s %s" % (name[:60], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
