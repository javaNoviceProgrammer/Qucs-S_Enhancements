#!/usr/bin/env python3
"""breaks.py: the fixes of the check of 3 November undone on purpose, one
piece at a time - fem_model's keys, the fem_ tools' word for a 3D model,
untitled documents' names and close_document's answer, a value past the
prefixes and a misfit of 100 %, show_layout's grid and texts, and the two
examples' keys - each built and its tests run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = W + "/qucs-s-26.1.1/qucs/"
B = W + "/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/mcp-check-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

F = "qucscontrol_fem.cpp"
C = "qucscontrol.cpp"
R = "rf25/rf25_circuit.cpp"
L = "qucscontrol_layout.cpp"
KEYS = ("test_multiphysics_doc", "aModelsUnknownKeysAreRefused")
FILE3D = ("test_multiphysics_doc", "aThreeDModelsFileIsSaidToBeOne")
NAMES = ("test_qucs_control", "untitledDocumentsKeepTheirNames")
TOOL = ("test_qucs_control", "anRf25ModelIsSolvedFullWaveThroughItsTool")
VALUES = ("test_rf25_sparams", "theSpiceSubcircuitIsTheCircuit")
OPEN = ("test_rf25_sparams", "anOpenBetweenThePortsIsWrittenPlainly")
GRID = ("test_layout_doc", "show_layoutColoursTheLayersAndTheCanvas")
TEXTS = ("test_layout_doc", "show_layoutShowsACellRegionAndLayers")

BREAKS = [
 # ---- 1: fem_model's keys
 ("fem_model new: the model's keys not checked",
  [(F, "            if (const QStringList unknown = unknownModelKeysOf(args.value(QLatin1String(\"model\")).toObject()); !unknown.isEmpty())\n"
       "                return errorResult(tr(\"'model': %1. Nothing was made",
    "            if (const QStringList unknown = unknownModelKeysOf(args.value(QLatin1String(\"model\")).toObject()); false && !unknown.isEmpty())\n"
    "                return errorResult(tr(\"'model': %1. Nothing was made")],
  [KEYS]),
 ("fem_model set: the model's keys not checked",
  [(F, "        if (const QStringList unknown = unknownModelKeysOf(args.value(QLatin1String(\"model\")).toObject()); !unknown.isEmpty())\n"
       "            return errorResult(tr(\"'model': %1. The model is unchanged",
    "        if (const QStringList unknown = unknownModelKeysOf(args.value(QLatin1String(\"model\")).toObject()); false && !unknown.isEmpty())\n"
    "            return errorResult(tr(\"'model': %1. The model is unchanged")],
  [KEYS]),
 ("a component's key at the top passed in silence",
  [(F, "        if (key != QLatin1String(\"tag\") && model.contains(key))", "        if (false && model.contains(key))")],
  [KEYS]),
 ("no near name said",
  [(F, "near.isEmpty() ? QString() : QObject::tr(\" - meant %1?\")", "true ? QString() : QObject::tr(\" - meant %1?\")")],
  [KEYS]),
 ("the component's keys not checked",
  [(F, "    for (const QJsonValue& c : components) check(c.toObject(), component, QObject::tr(\"its component has no\"));\n", "")],
  [KEYS]),
 ("the results' keys not checked",
  [(F, "    check(model.value(QLatin1String(\"results\")).toObject(), results, QObject::tr(\"its results have no\"));\n", "")],
  [KEYS]),
 ("a model's description taken for unknown",
  [(F, "QStringLiteral(\"format\"),     QStringLiteral(\"title\"),      QStringLiteral(\"description\"),",
    "QStringLiteral(\"format\"),     QStringLiteral(\"title\"),")],
  [KEYS]),
 ("a component's thickness taken for unknown",
  [(F, "QStringLiteral(\"tag\"),       QStringLiteral(\"label\"),      QStringLiteral(\"unit\"),\n"
       "                                       QStringLiteral(\"thickness\"), ",
    "QStringLiteral(\"tag\"),       QStringLiteral(\"label\"),      QStringLiteral(\"unit\"),\n"
    "                                       ")],
  [KEYS]),
 ("the reverse recovery's global graph its \"solutions\" again",
  [("../examples/multiphysics/diode_reverse_recovery.qfem", "{\"type\": \"globalgraph\", \"expression\": \"semi.I_a\", \"unit\": \"mA\"}",
    "{\"type\": \"globalgraph\", \"expression\": \"semi.I_a\", \"unit\": \"mA\", \"solutions\": \"all\"}")],
  [KEYS]),
 ("the point source's point graph its \"dataset\" again",
  [("../examples/multiphysics/point_source_wave.qfem", "\"data\": \"cpt1\",", "\"dataset\": \"cpt1\",")],
  [KEYS]),
 # ---- 2: a 3D model to the fem_ tools
 ("a closed .qfem3d \"not a multiphysics model (.qfem)\"",
  [(F, "            if (QucsApp::isMultiphysics3DFile(file)) {\n                *error = notA2DModel(QFileInfo(file).fileName());\n"
       "                return nullptr;\n            }\n", "")],
  [FILE3D]),
 ("an open 3D model \"not a multiphysics model\"",
  [(F, "QucsApp::isMultiphysics3DDocument(QucsApp::documentWidget(doc)) ? notA2DModel(titleOf(doc))",
    "false ? notA2DModel(titleOf(doc))")],
  [FILE3D]),
 ("every other document called a 3D model",
  [(F, "QucsApp::isMultiphysics3DDocument(QucsApp::documentWidget(doc)) ? notA2DModel(titleOf(doc))",
    "true ? notA2DModel(titleOf(doc))")],
  [FILE3D]),
 # ---- 3, 4: untitled documents' names, close_document
 ("an untitled one's number its place again",
  [(C, "    const int n = w->property(kNumber).toInt();\n    return n <= 1 ? title",
    "    int n = 1;\n    for (QWidget* ow : std::as_const(same)) {\n        if (ow == w) break;\n        ++n;\n    }\n    return n <= 1 ? title")],
  [NAMES]),
 ("a new one numbered past the others, not the lowest free",
  [(C, "        int n = 1;\n        while (taken.contains(n)) ++n;", "        int n = int(taken.size()) + 1;\n        while (taken.contains(n)) ++n;")],
  [NAMES]),
 ("get_state without the tab's words",
  [(C, "            d.insert(QStringLiteral(\"tab\"), shownTitleOf(doc));", "            {}")],
  [NAMES]),
 ("close_document not saying which",
  [(C, "    if (others.isEmpty()) return textResult(tr(\"%1 is closed.\").arg(title));",
    "    return textResult(tr(\"%1 is closed.\").arg(title));")],
  [NAMES]),
 ("a tab's words not taken for the one tab",
  [(C, "    if (tabbed.size() == 1) return tabbed.first();\n", "")],
  [NAMES]),
 ("a tab's words of several: the first taken",
  [(C, "    if (tabbed.size() == 1) return tabbed.first();\n", "    if (!tabbed.isEmpty()) return tabbed.first();\n")],
  [NAMES]),
 ("\"untitled (5)\" not open: those open not said",
  [(C, "        if (!others.isEmpty()) {\n            *error = tr(\"%1 is not open; open without a file",
    "        if (false) {\n            *error = tr(\"%1 is not open; open without a file")],
  [NAMES]),
 # ---- 5: rf25_circuit's values
 ("a value past tera prefixed again",
  [(R, "if (!std::isfinite(a) || a >= 999.95e12 || a < 0.99995e-18) return",
    "if (!std::isfinite(a) || a < 0.99995e-18) return")],
  [VALUES, OPEN]),
 ("nought and below atto prefixed again (\"0 aH\")",
  [(R, "if (!std::isfinite(a) || a >= 999.95e12 || a < 0.99995e-18) return",
    "if (!std::isfinite(a) || a >= 999.95e12) return")],
  [VALUES]),
 ("no R an open",
  [(R, "    return element.startsWith(QLatin1Char('R')) && std::abs(value) >= 1e15;",
    "    return element.startsWith(QLatin1Char('Z')) && std::abs(value) >= 1e15;")],
  [VALUES, OPEN]),
 ("elementSaid without \"(an open)\"",
  [(R, " + (isOpenInEffect(element, value) ? tr(\" (an open)\") : QString());", ";")],
  [VALUES, OPEN]),
 ("the fit's lines their values as before",
  [(R, "    for (const auto& [name, value] : b.values) values << elementSaid(name, value);",
    "    for (const auto& [name, value] : b.values)\n"
    "        values << QStringLiteral(\"%1 %2\").arg(name, valueSaid(value, name.startsWith(QLatin1Char('R')) ? QStringLiteral(\"Ohm\") : QStringLiteral(\"F\")));")],
  [OPEN]),
 ("rf25_circuit's \"as written\" its values as before",
  [("qucscontrol_rf25.cpp", "            said << elementSaid(element, value);",
    "            said << QStringLiteral(\"%1 %2\").arg(element, valueSaid(value, element.startsWith(QLatin1Char('R')) ? QStringLiteral(\"Ohm\") : QStringLiteral(\"F\")));")],
  [TOOL]),
 ("a misfit of 100 % \"1e+02 %\" again",
  [(R, "percent >= 10 ? QString::number(std::round(percent), 'f', 0) : QString::number(percent, 'g', 2)",
    "QString::number(percent, 'g', 2)")],
  [OPEN]),
 # ---- 6: show_layout
 ("the grid \"a point every\" whatever it draws",
  [(L, "    case LayoutView::GridStyle::Lines: what = QObject::tr(\"a line every %1 µm across and up\").arg(each); break;\n"
       "    case LayoutView::GridStyle::Crosses: what = QObject::tr(\"a cross every %1 µm\").arg(each); break;\n",
    "    case LayoutView::GridStyle::Lines:\n    case LayoutView::GridStyle::Crosses:\n")],
  [GRID]),
 ("texts hidden by the tab not said",
  [(L, "        if (!view->labelsShown() && layout->labelCount > 0)\n", "        if (false && layout->labelCount > 0)\n")],
  [TEXTS]),
 ("'texts' the canvas's alone, the button left as it was",
  [(L, "        if (textsGiven) doc->setLabelsShown(texts);", "        if (textsGiven) view->setLabelsShown(texts);")],
  [TEXTS]),
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
        print("%-62s %s" % (name[:62], said))
        if "BUILD FAILED" in said or "hung" in said or ("PASSED - NOT CAUGHT" in said and "caught)" not in said):
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":   # (read only, for BREAKS: nothing run)
    sys.exit(main())
