#!/usr/bin/env python3
"""breaks.py: each fix of the bend-study round undone on purpose, to see
its test fail. One edit (or a few) per break, the named targets built, the
named tests run; every one of them must fail. The sources are put back
whatever happens. BRK_ONLY_CHECK=1 checks the anchors alone, building
nothing. HUNT_RUNS=<dir> says where the logs go.

BRK_SRC and BRK_BUILD say which tree to break and where to build it: the
round's own run used a copy of the tree, because another session was in the
middle of qucs/rf25/ and the checkout did not compile at the time."""
import os, subprocess, sys

Q = os.environ.get("BRK_SRC", "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/")
B = os.environ.get("BRK_BUILD", "/Users/meisam/git/Qucs-S_Enhancements/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/bend-study-breaks")
os.makedirs(RUNS, exist_ok=True)

# (file, old, new) edits; targets to build; "suite:function" tests
BREAKS = [
 ("1 the axes of a 1D plot group need not span what is written",
  [("multiphysicsdoc.cpp",
    "        if (!outer.isEmpty() && (spanned != every.size() || unevenModes)) {",
    "        if (false && !outer.isEmpty() && (spanned != every.size() || unevenModes)) {")],
  ["test_multiphysics_doc"], ["test_multiphysics_doc:aSweepOfUnevenModesSaysWhatIsInIt"]),

 ("1 the solutions are written against an index with nothing to say which is which",
  [("multiphysicsdoc.cpp",
    "            if (first.modeAnalysis)\n                column(first.quantumStates ? QStringLiteral(\"state\") : QStringLiteral(\"mode\"), QString(),",
    "            if (false)\n                column(first.quantumStates ? QStringLiteral(\"state\") : QStringLiteral(\"mode\"), QString(),")],
  ["test_multiphysics_doc"], ["test_multiphysics_doc:aSweepOfUnevenModesSaysWhatIsInIt"]),

 ("2 a mode step reports no group index",
  [("multiphysics/fem_waves.cpp",
    "    if (std::isfinite(a_groupIndex))\n        a_s.setGlobal(p + QStringLiteral(\"ng\")",
    "    if (false)\n        a_s.setGlobal(p + QStringLiteral(\"ng\")")],
  ["test_fem_waves"], ["test_fem_waves:theGroupIndex"]),

 ("2 the group index leaves the media's own dispersion out (ε at the one frequency)",
  [("multiphysics/fem_waves.cpp",
    "            const double w = omega * (1 + side * apart), k = a_k0 * (1 + side * apart);\n            dev.setFrequency(w / (2 * M_PI));",
    "            const double w = omega * (1 + side * apart), k = a_k0 * (1 + side * apart);\n            dev.setFrequency(omega / (2 * M_PI));")],
  ["test_fem_waves"], ["test_fem_waves:theGroupIndex"]),

 ("2 the energy counts ε alone, not ∂(ωε)/∂ω",
  [("multiphysics/fem_waves.cpp",
    "                for (int i = 0; i < 3; ++i)\n                    for (int j = 0; j < 3; ++j) into[std::size_t(3 * i + j)] += double(side) * w * m.epsT[i][j];",
    "                for (int i = 0; i < 3; ++i)\n                    for (int j = 0; j < 3; ++j) into[std::size_t(3 * i + j)] += double(side) * omega * m.epsT[i][j];")],
  ["test_fem_waves"], ["test_fem_waves:theGroupIndex"]),

 ("3 the azimuthal order m is a whole number of its own again, which no sweep reaches",
  [("multiphysics/fem_model.cpp",
    "          expr(QStringLiteral(\"m\"), tr(\"Azimuthal order m\"), QString(), QStringLiteral(\"1\"),",
    "          integer(QStringLiteral(\"m\"), tr(\"Azimuthal order m\"), 1,")],
  ["test_fem_resonator"], ["test_fem_resonator:theOrderIsSwept"]),

 ("4 a model given as JSON keeps a property no kind has",
  [("qucscontrol_fem.cpp",
    "            if (const QStringList unknown = unknownPropertiesOf(m->root); !unknown.isEmpty())\n                return errorResult(tr(\"'model': %1 Nothing was made",
    "            if (const QStringList unknown = QStringList(); !unknown.isEmpty())\n                return errorResult(tr(\"'model': %1 Nothing was made")],
  ["test_multiphysics_doc"], ["test_multiphysics_doc:anUnknownPropertyIsRefusedWhereItIsSet"]),

 ("4 a property no kind has cannot be taken out again",
  [("qucscontrol_fem.cpp",
    "            if (it.value().isNull() && n.props.contains(it.key())) {\n                n.props.remove(it.key());\n                continue;\n            }",
    "            if (false) {\n                n.props.remove(it.key());\n                continue;\n            }")],
  ["test_multiphysics_doc"], ["test_multiphysics_doc:anUnknownPropertyIsRefusedWhereItIsSet"]),

 ("4 a refusal does not say which name was meant",
  [("qucscontrol.cpp",
    "QStringList qucs_s::control::nearNamesOf(const QStringList& unknown, const QStringList& takes)\n{\n    QStringList near;",
    "QStringList qucs_s::control::nearNamesOf(const QStringList& unknown, const QStringList& takes)\n{\n    QStringList near;\n    if (true) return near;")],
  ["test_multiphysics_doc"], ["test_multiphysics_doc:anUnknownPropertyIsRefusedWhereItIsSet"]),

 ("5 an export does not make the folder its file is in",
  [("multiphysicsdoc.cpp",
    "    if (const QString folder = QFileInfo(path).absolutePath(); !folder.isEmpty() && !QFileInfo(folder).isDir())\n        if (!QDir().mkpath(folder))",
    "    if (const QString folder = QFileInfo(path).absolutePath(); false && !folder.isEmpty() && !QFileInfo(folder).isDir())\n        if (!QDir().mkpath(folder))")],
  ["test_multiphysics_doc"], ["test_multiphysics_doc:anExportMakesItsFolder"]),

 ("7 a decimal label is written to the step's places alone",
  [("numberformat.cpp",
    "        return fixed(value, decimals >= 0 ? decimals : placesFor(value, step));",
    "        return fixed(value, decimals >= 0 ? decimals : placesOf(step));")],
  ["test_number_format"], ["test_number_format:aLabelSaysWhereItsTickIs"]),

 ("7 a label off the grid is written to fifteen places",
  [("numberformat.cpp",
    "    const double tolerance = std::fabs(step) * 1e-3;",
    "    const double tolerance = 0.0;")],
  ["test_number_format"], ["test_number_format:aLabelSaysWhereItsTickIs"]),

 ("8 the legend's dash pattern is not fitted to its sample",
  [("diagrams/diagram.cpp",
    "    if (drawn > 0.0 && 2 * drawn > sample) {",
    "    if (false) {")],
  ["test_diagram_legend"], ["test_diagram_legend:theSampleShowsTheLineStyle"]),

 ("8 a dashed sample's caps are round again, which closes its gaps",
  [("diagrams/diagram.cpp",
    "        if (dashed) pen.setCapStyle(Qt::FlatCap);",
    "        if (false) pen.setCapStyle(Qt::FlatCap);")],
  ["test_diagram_legend"], ["test_diagram_legend:theSampleShowsTheLineStyle"]),

 ("8 a row with a marker gets no more room for its line",
  [("diagrams/diagram.cpp",
    "        if (row.marker != Graph::PointMarker::None) return legendSample + 12.0;",
    "        if (false) return legendSample + 12.0;")],
  ["test_diagram_legend"], ["test_diagram_legend:theSampleShowsTheLineStyle"]),

 ("6 the guard: a standalone display's plots are not drawn after a save",
  [("qucscontrol_results.cpp",
    "void reloadDiagram(Schematic* sch, Diagram* d)\n{\n    for (Graph* g : d->Graphs) g->lastLoaded = QDateTime();",
    "void reloadDiagram(Schematic* sch, Diagram* d)\n{\n    for (Graph* g : d->Graphs) g->Var = QStringLiteral(\"no.such.variable\");\n    for (Graph* g : d->Graphs) g->lastLoaded = QDateTime();")],
  ["test_diagram_tools"], ["test_diagram_tools:aStandaloneDisplayKeepsItsPlots"]),
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
                build = subprocess.run(["ninja", "-C", B] + targets, stdout=out, stderr=subprocess.STDOUT)
                if build.returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                caught = []
                for t in tests:
                    suite, fn = t.split(":")
                    r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn],
                                       stdout=out, stderr=subprocess.STDOUT,
                                       env=dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0"))
                    caught.append("%s %s" % (t, "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                results.append((name, "; ".join(caught)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-66s %s" % (name[:66], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
