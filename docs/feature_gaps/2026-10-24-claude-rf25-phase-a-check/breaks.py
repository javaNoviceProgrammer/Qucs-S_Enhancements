#!/usr/bin/env python3
"""breaks.py: each fix of the RF 2.5D phase A check of 24 October undone on
purpose - the ground under the stack and both ends of it, a new model's
stack drawn, the model framed by show_layout and its tree's menu reached by
context_menu, a bad value said once, the tree's columns named, a new model
saved into the workspace, what a file has that is not read said, and a file
read again shown at once. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-phase-a-check-breaks")
os.makedirs(RUNS, exist_ok=True)

M, G, D, P = "rf25/rf25_model.cpp", "rf25/rf25_green.cpp", "rf25doc.cpp", "rf25panel.cpp"
L, U, C, A = "qucscontrol_layout.cpp", "qucscontrol_ui.cpp", "qucscontrol.cpp", "qucs.cpp"
MOD, ST, DOC, CTL = "test_rf25_model", "test_rf25_static", "test_rf25_doc", "test_qucs_control"
UNDER = "aGroundIsUnderTheStackWhereverItIsPut"
ADDED = "aLayerAddedGoesWhereItBelongs"
ONCE = "aBadValueIsSaidOnce"
NOTED = "whatAFileHasThatIsNotReadIsSaid"
SOLVED = "theGroundIsReadForWhatItIsNotWhereItIs"
DRAWN = "aNewModelsStackIsDrawnAtOnce"
NAMED = "theTreesColumnsAreNamed"
SAVED = "anUntitledModelIsSavedIntoTheWorkspace"
FOLLOWS = "aFileReadAgainIsShownInTheFormAtOnce"
FRAMED = "anRf25ModelIsFramedAndItsTreeMenuReached"

BREAKS = [
 # ---- B1: the ground under the stack, and both ends of it
 ("the ground is put at the running height again",
  [(M, "        if (l.kind == Layer::Ground) {\n            l.z0 = l.z1 = 0;", "        if (l.kind == Layer::Ground) {\n            l.z0 = l.z1 = z;")],
  [(MOD, UNDER)]),

 ("the substrate must be the very first layer, the ground counted",
  [(M, "        if (l.kind == Layer::Substrate && i != firstPlaced)", "        if (l.kind == Layer::Substrate && i != 0)")],
  [(MOD, UNDER)]),

 ("the cover must be the very last layer, the ground counted",
  [(M, "        if (l.kind == Layer::Cover && i != lastPlaced)", "        if (l.kind == Layer::Cover && i != out.layers.size() - 1)")],
  [(MOD, UNDER)]),

 ("the solver reads the ground by its place again",
  [(G, "            if (l.variant != QLatin1String(\"open\")) out.groundBelow = true;",
    "            if (out.slabs.isEmpty()) out.groundBelow = true;\n            else out.problems << tr(\"A ground plane above the stack is not solved yet: put it at the bottom.\");")],
  [(ST, SOLVED)]),

 ("an open ground is solved as a perfect plane",
  [(G, "            if (l.variant != QLatin1String(\"open\")) out.groundBelow = true;", "            out.groundBelow = true;")],
  [(ST, SOLVED)]),

 ("a metal lid is read as the air it is not",
  [(G, "            if (l.variant == QLatin1String(\"perfect\"))\n                out.problems", "            if (false)\n                out.problems")],
  [(ST, SOLVED)]),

 ("the stack does not say what kind of ground it has",
  [(M, "            layer.variant = textOf(c, QStringLiteral(\"kind\"));\n            if (layer.variant == QLatin1String(\"impedance\"))",
    "            if (textOf(c, QStringLiteral(\"kind\")) == QLatin1String(\"impedance\"))")],
  [(MOD, UNDER), (ST, SOLVED)]),

 ("a ground added goes at the end, after the cover",
  [(M, "        if (node.type == QLatin1String(\"rf_ground\")) {\n            at = 0;", "        if (node.type == QLatin1String(\"rf_ground\")) {\n            at = int(list.size());")],
  [(MOD, ADDED)]),

 ("a substrate added goes under the ground",
  [(M, "            at = groundFirst ? 1 : 0;", "            at = 0;")],
  [(MOD, ADDED)]),

 ("the ground's help says it goes last in the tree again",
  [(M, "         tr(\"What closes the stack at the bottom: a plane under the substrate, wherever it is in the tree. One at \"\n            \"most. Open is no plane at all - the substrate on air.\")});",
    "         tr(\"What closes the stack at the bottom. One at most, and last in the tree (it sits under the substrate).\")});")],
  [(MOD, ADDED)]),

 # ---- B2: a new model's stack drawn
 ("a new model's stack is drawn only at the first edit",
  [(D, "    a_stack->setStack(a_model.stack());\n    showProblems();\n    if (app != nullptr)", "    showProblems();\n    if (app != nullptr)")],
  [(DOC, DRAWN)]),

 # ---- B3: the model framed, and screenshot pointing there
 ("show_layout takes no RF 2.5D model",
  [(L, "        if (model != nullptr) {\n            // Which cell, and which layers,", "        if (false) {\n            // Which cell, and which layers,")],
  [(CTL, FRAMED)]),

 ("show_layout leaves the model's region where it was",
  [(L, "            if (region.size() == 4)\n                view->showRegion(", "            if (false)\n                view->showRegion(")],
  [(CTL, FRAMED)]),

 ("show_layout does not draw the mesh it was asked to",
  [(L, "            if (args.contains(QLatin1String(\"mesh\"))) model->setMeshShown(args.value(QLatin1String(\"mesh\")).toBool());", "")],
  [(CTL, FRAMED)]),

 ("show_layout sets a model's cell as though it were a layout's",
  [(L, "            for (const char* key : {\"cell\", \"layers\", \"depth\"})", "            for (const char* key : std::initializer_list<const char*>{})")],
  [(CTL, FRAMED)]),

 ("screenshot says only that a model is no schematic",
  [(C, "            const bool layoutLike = dynamic_cast<RF25Doc*>(doc) != nullptr || qobject_cast<LayoutDoc*>(QucsApp::documentWidget(doc)) != nullptr;",
    "            const bool layoutLike = false;")],
  [(CTL, FRAMED)]),

 # ---- N1: a bad value said once
 ("every check is made of a value however it read",
  [(M, "        const auto read = [&unread, i](const char* key) { return !unread.at(i).contains(QLatin1String(key)); };",
    "        const auto read = [&unread, i](const char* key) { Q_UNUSED(key); Q_UNUSED(unread); Q_UNUSED(i); return true; };")],
  [(MOD, ONCE)]),

 ("the reader forgets which values would not read",
  [(M, "            out.problems << tr(\"%1: %2 is not a number (%3).\").arg(where, key, why);\n            failed.insert(key);",
    "            out.problems << tr(\"%1: %2 is not a number (%3).\").arg(where, key, why);")],
  [(MOD, ONCE)]),

 # ---- N2: the tree's columns named
 ("the tree's columns are Qt's own 1 and 2 again",
  [(P, "    a_tree->setHeaderLabels({tr(\"Node\"), tr(\"Status\")});\n", "")],
  [(DOC, NAMED)]),

 # ---- N3: a new document saved into the workspace
 ("an untitled document's Save starts in the home folder again",
  [(A, "        if(lastDirOpenSave.isEmpty())  s = QucsSettings.qucsWorkspaceDir.absolutePath();",
    "        if(lastDirOpenSave.isEmpty())  s = QDir::homePath();")],
  [(DOC, SAVED)]),

 # ---- N4: what a file has that is not read
 ("a node's keys it does not read are passed over again",
  [(M, "    if (notes != nullptr) {\n        static const QStringList keys", "    if (false) {\n        static const QStringList keys")],
  [(MOD, NOTED)]),

 ("a property no kind has is passed over again",
  [(M, "                if (kind->property(it.key()) == nullptr)\n                    *notes", "                if (false)\n                    *notes")],
  [(MOD, NOTED)]),

 ("a node's children say nothing of what they hold",
  [(M, "        if (c.isObject()) n.children.push_back(nodeFromJson(c.toObject(), notes));",
    "        if (c.isObject()) n.children.push_back(nodeFromJson(c.toObject(), nullptr));")],
  [(MOD, NOTED)]),

 ("the read's notes are not handed back",
  [(M, "        *error = notes.join(QStringLiteral(\"; \"));\n    }\n    return m;", "        error->clear();\n    }\n    return m;")],
  [(MOD, NOTED)]),

 ("many notes are said as many as there are",
  [(M, "        if (notes.size() > 8) {", "        if (false) {")],
  [(MOD, NOTED)]),

 # ---- N5: the tree's menu reached
 ("context_menu's node opens nothing",
  [(U, "        open = [target, tag] { return target && target->openNodeMenu(tag); };", "        open = [] { return false; };")],
  [(CTL, FRAMED)]),

 ("the panel finds the node and opens no menu on it",
  [(P, "    emit a_tree->customContextMenuRequested(at.center());\n    return true;", "    return true;")],
  [(CTL, FRAMED)]),

 ("a node is found by its tag alone, never by its name",
  [(U, "                if (qucs_s::rf25::nameOf(n).compare(asked, Qt::CaseInsensitive) == 0) named << n.tag;", "")],
  [(CTL, FRAMED)]),

 # ---- N6: a file read again shown at once
 ("the panel leaves a read's form to the next turn again",
  [(P, "        if (a_doc && a_doc->lastChangeWasRead()) {", "        if (false) {")],
  [(DOC, FOLLOWS)]),

 ("the document does not say a change was a read",
  [(D, "    a_lastChangeRead = read;", "    a_lastChangeRead = false;")],
  [(DOC, FOLLOWS)]),
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
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            if only:
                results.append((name, "anchors ok"))
                continue
            log = os.path.join(RUNS, "brk%02d.log" % i)
            suites = sorted({s for s, _ in tests})
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out,
                                  stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=900,
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen",
                                                    ASAN_OPTIONS="detect_leaks=0"))
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
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
