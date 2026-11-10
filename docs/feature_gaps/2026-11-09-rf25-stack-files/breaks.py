#!/usr/bin/env python3
"""breaks.py: RF 2.5D's stack written to and read from HFSS 3D Layout's XML
stackup and EMX's process file (9 November) undone on purpose, one piece at a
time - each writer's layers, rooms, grounds, lids, vias and names; the EMX
reader's units, offsets, flips, plane, lid, vias and regions; the HFSS reader's
plane, lid, vias, streams and permittivity; the model's stack and layer map put
in place; the tools, the guard and the menu - each built and its tests run.
Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks the anchors and
writes nothing; BRK_ONLY=3,7 runs those alone; HUNT_RUNS=<dir> for logs;
BRK_BUILD=<dir> the build (the checkout's build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-stack-breaks")

F = "rf25/rf25_stackfile.cpp"
I = "rf25/rf25_hfss_import.cpp"
D = "rf25doc.cpp"
P = "rf25panel.cpp"
G = "qucscontrol_resources.cpp"
S = "test_rf25_stack"
HOUT = (S, "aStackIsWrittenAsAnHfssStackup")
HBACK = (S, "anHfssStackupReadsBackTheSameStack")
ENDS = (S, "groundsAndLidsGoThereAndBack")
EOUT = (S, "aStackIsWrittenAsAnEmxProcess")
EBACK = (S, "anEmxProcessReadsBackTheSameStack")
GRAMMAR = (S, "anEmxProcessIsReadAsItsGrammarSays")
VARIED = (S, "variationsUnitsFlipsAndHiddenParts")
OTHER = (S, "aStackupOfAnotherHandReads")
PERM = (S, "theControlFileImportKeepsEachPermittivity")
REFUSED = (S, "whatCannotBeReadIsRefused")
ALONE = (S, "theStackReplacesTheModelsAlone")
KEPT = (S, "whatNeitherFileSaysOfAMetalIsKeptByName")
TOOLS = ("test_qucs_control", "anRf25StackGoesOutAndBackThroughItsToolsAndItsMenu")
GUARD = ("test_qucs_control", "guardsAskAboutCommandsAndFilesOutside")
DR = "rf25/rf25_stackdrawing.cpp"
C = "qucscontrol_rf25.cpp"
DW = "test_rf25_stack_drawing"
DRAWN = (DW, "whatIsDrawn")
COVERS = (DW, "groundsAndCovers")
COLS = (DW, "viasStandInColumns")
LAID = (DW, "itIsLaidOutOnThePage")
APART = (DW, "aTallStackPutsItsKeyApart")
PDF = (DW, "thePdfIsWrittenAndNotRead")
PAINT = (DW, "aPageIsPainted")

BREAKS = [
 # ---- Writing
 ("a metal's room of the air, not the dielectric above it",
  [(F, "        if (above.kind == Layer::Dielectric) return &above;", "        if (false) return &above;")], [HOUT, EOUT]),
 ("HFSS: the conductors bottom first",
  [(F, "        conductors.prepend(QStringLiteral(", "        conductors.append(QStringLiteral(")], [HOUT]),
 ("HFSS: a metal's room not among the dielectrics",
  [(F, "        if (r.thickness > 0)\n            dielectrics.prepend(", "        if (false)\n            dielectrics.prepend(")], [HOUT, HBACK]),
 ("HFSS: a via's StartLayer the lower",
  [(F, ".arg(QStringLiteral(\"%1\"), xml(m), xml(v.name), xml(upper), xml(lower));", ".arg(QStringLiteral(\"%1\"), xml(m), xml(v.name), xml(lower), xml(upper));")], [HOUT, HBACK]),
 ("HFSS: a perfect plane of copper",
  [(F, "            g.material = material(g.name, 1, 0, 1e30);", "            g.material = material(g.name, 1, 0, 5.8e7);")], [HOUT, HBACK]),
 ("HFSS: a metal plane four skin depths",
  [(F, "            g.thickness = 5 * std::sqrt(", "            g.thickness = 4 * std::sqrt(")], [ENDS]),
 ("HFSS: a lid with no gap under it",
  [(F, "            gap.material = airMaterial();\n            z += l.thickness;\n            rows << gap;", "            gap.material = airMaterial();\n            z += l.thickness;")], [ENDS]),
 ("EMX: a substrate's conductivity left out",
  [(F, "            if (l.sigma > 0) line += QStringLiteral(\" 1 %1\").arg(num(l.sigma));", "            if (false) line += QStringLiteral(\" 1 %1\").arg(num(l.sigma));")], [EOUT, EBACK]),
 ("EMX: a dielectric's loss left out",
  [(F, "            QString line = QStringLiteral(\"layer %1 %2 name %3\").arg(um(l.thickness), num(l.epsr), id);\n            if (l.tand > 0)",
       "            QString line = QStringLiteral(\"layer %1 %2 name %3\").arg(um(l.thickness), num(l.epsr), id);\n            if (false)")], [EOUT, EBACK]),
 ("EMX: an open ground not written as air below",
  [(F, "    if (open) {\n        // EMX has its plane always", "    if (false) {\n        // EMX has its plane always")], [ENDS]),
 ("EMX: its own words left as names",
  [(F, "    if (reservedForEmx(id.toLower())) id += QStringLiteral(\"_\");", "")], [EOUT]),
 ("EMX: a via to the ground not to the backside",
  [(F, "        const QString from = stack.isGround(v.from) ? QStringLiteral(\"backside\")", "        const QString from = stack.isGround(v.from) ? QStringLiteral(\"ground\")")], [EOUT]),
 ("EMX: no define of the layer map",
  [(F, "            defines << QStringLiteral(\"define %1 = %2\")", "            defines << QStringLiteral(\"# define %1 = %2\")")], [EOUT, EBACK]),
 # ---- Reading EMX
 ("EMX: offsets not added up",
  [(F, "                offset = k == QLatin1String(\"offset\") ? offset + v : v;", "                offset = v;")], [GRAMMAR]),
 ("EMX: a sheet resistance taken as a conductivity",
  [(F, "                    m.sigma = 1 / (sigma.first * m.thickness);", "                    m.sigma = sigma.first;")], [GRAMMAR, VARIED]),
 ("EMX: a layer's ohm-cm taken as S/m",
  [(F, "                    d.sigma = sigma.first > 0 ? 100 / sigma.first : 0;", "                    d.sigma = sigma.first;")], [GRAMMAR, VARIED]),
 ("EMX: a length's unit not read",
  [(F, "        if (const std::optional<double> unit = lengthUnitOf(c.peek())) {\n            c.next();\n            return v * *unit;",
       "        if (const std::optional<double> unit = lengthUnitOf(c.peek())) {\n            c.next();\n            return v;")], [VARIED, GRAMMAR]),
 ("EMX: a flip's conductors not turned",
  [(F, "                        conductors[i].z0 = a + b - conductors.at(i).z0 - conductors.at(i).thickness;", "")], [VARIED]),
 ("EMX: the 'via' of ohms/via begins a statement",
  [(F, "keywords.contains(t.text.toLower()) && !afterUnit) {", "keywords.contains(t.text.toLower())) {")], [VARIED]),
 ("EMX: an open ground's plane kept",
  [(F, "        plane.clear();\n        first = 1;", "        first = 1;")], [ENDS]),
 ("EMX: a via from the upper",
  [(F, "        if (b == QLatin1String(\"ground\") || (a != QLatin1String(\"ground\") && elevationOf(a) > elevationOf(b))) std::swap(a, b);", "")], [GRAMMAR]),
 ("EMX: rf25_open_ground not known again",
  [(F, "bottom.name.compare(QLatin1String(\"rf25_open_ground\"), Qt::CaseInsensitive) == 0", "bottom.name.compare(QLatin1String(\"rf25_open_ground\"), Qt::CaseInsensitive) == 1")], [ENDS]),
 ("EMX: a conducting top no lid",
  [(F, "            if (d.sigma > 0) {\n                EdbLayer l;", "            if (false) {\n                EdbLayer l;")], [VARIED, ENDS]),
 ("EMX: a name defined nowhere taken as nought",
  [(F, "        if (!hidden) stop(t, ", "        if (false) stop(t, ")], [REFUSED]),
 ("EMX: a difference read as exact",
  [(F, "            r.exact = r.exact && b.exact && op == QLatin1String(\"+\");", "            r.exact = r.exact && b.exact;")], [GRAMMAR]),
 ("EMX: random not nought",
  [(F, "            noteOnce(QStringLiteral(\"random\"), tr(\"Its random variables are taken as nought, as EMX takes them outside a Monte Carlo or corner run.\"));\n            return 0;",
       "            noteOnce(QStringLiteral(\"random\"), tr(\"Its random variables are taken as nought, as EMX takes them outside a Monte Carlo or corner run.\"));\n            return 1;")], [VARIED]),
 # ---- Reading HFSS
 ("HFSS: Permeability read as the permittivity (the bug fixed)",
  [(I, "            } else if (!material.isEmpty() && name != QLatin1String(\"Double\")) {",
       "            } else if (name == QLatin1String(\"Permittivity\") || name == QLatin1String(\"DielectricLossTangent\") || name == QLatin1String(\"Conductivity\")) {")], [PERM, HBACK]),
 ("HFSS: two streams to one target two metals (the bug fixed)",
  [(I, "                    if (!known) out->layers << l;", "                    out->layers << l;")], [OTHER]),
 ("HFSS: no plane at the bottom",
  [(I, "        if (lowest.elevation <= bottom + 1e-12) {", "        if (false) {")], [HBACK, OTHER]),
 ("HFSS: no lid",
  [(I, "            if (overVacuum) lid = highest.name;", "")], [ENDS]),
 ("HFSS: a via from the upper",
  [(I, "                if (bGround || (!aGround && metalsOrder.indexOf(a) > metalsOrder.indexOf(b))) std::swap(a, b);", "")], [HBACK, OTHER]),
 ("HFSS: layers by number not mapped",
  [(I, "        out.rows.append(QJsonArray{r.layer, r.datatype.isEmpty() ? QStringLiteral(\"0\") : r.datatype, to});", "")], [OTHER]),
 ("a lid's gap not its height",
  [(I, "            height = d.thickness;\n            gapName = d.name;", "            height = 0;\n            gapName = d.name;")], [ENDS]),
 # ---- The model, the tools, the guard, the menu
 ("the layer map's old rows for the file's layers kept",
  [(F, "                if (keys.contains(keyOf(row)) || (row.size() > 2 && names.contains(row.at(2).toString()))) continue;", "")], [ALONE]),
 ("the stack read but not put in the model",
  [(D, "    setModel(changed, tr(\"Import Stack\"));", "")], [TOOLS]),
 ("the menu's name with no suffix left so",
  [(P, "    if (!stackFormatOf(file))\n        file += ", "    if (false)\n        file += ")], [TOOLS]),
 ("the menu's items not on the Stack",
  [(P, "    if (n->type == QLatin1String(\"rf_stack\")) {\n        QAction* in", "    if (n->type == QLatin1String(\"rf_stackx\")) {\n        QAction* in")], [TOOLS]),
 # ---- The check of 9 November: S1 and S2
 ("S1: faces not said on export",
  [(F, "    if (faces.isEmpty()) return {};", "    return {};")], [KEPT]),
 ("S1: a metal's model not kept on import",
  [(F, "                    n.props.insert(k, was->props.value(k));", "")], [KEPT]),
 ("S1: EMX's spelling of a name not matched",
  [(F, "before.value(n.label, beforeLoose.value(looseName(n.label)))", "before.value(n.label)")], [KEPT]),
 ("S1: what was kept not said",
  [(F, "        if (!keptFaces.isEmpty())", "        if (false)")], [KEPT]),
 ("S2: a dielectric's EMX name not said",
  [(F, "        if (!id.isEmpty() && id != l.name && l.kind != Layer::Metal && l.kind != Layer::Via)", "        if (false)")], [KEPT]),
 ("S2: the plane's lost name not said",
  [(F, "    if (!open && ground->name != QLatin1String(\"GND\"))", "    if (false)")], [KEPT]),
 ("the guard not told what the stack export writes",
  [(G, "    } else if (tool == QLatin1String(\"rf25_stack_export\")) {", "    } else if (tool == QLatin1String(\"rf25_stack_exportx\")) {")], [GUARD]),
 # ---- The recheck: metals with no namesake
 ("the recheck: metals come in as sheets unsaid",
  [(F, "        if (!sheets.isEmpty())", "        if (false)")], [KEPT]),
 ("the recheck: a metal with a namesake said to come in as a sheet",
  [(F, "            } else if (!n.props.contains(QLatin1String(\"model\"))", "            } if (!n.props.contains(QLatin1String(\"model\"))")], [KEPT]),
 # ---- The drawing (10 November)
 ("drawing: a metal's room not said to be of the dielectric above",
  [(DR, "(fill != nullptr ? tr(\"%1 (%2)\").arg(epsWords(fill->epsr), fill->name)", "(fill != nullptr ? epsWords(fill->epsr)")], [DRAWN]),
 ("drawing: the substrate's conductivity not said",
  [(DR, "            if (l.sigma > 0) b.words << sigmaWords(l.sigma)", "            if (false) b.words << sigmaWords(l.sigma)")], [DRAWN]),
 ("drawing: a via from its lower metal's bottom",
  [(DR, "joined << Joined{&v, a, b, a != nullptr ? a->z1 : 0, b->z0};", "joined << Joined{&v, a, b, a != nullptr ? a->z0 : 0, b->z0};")], [DRAWN, COLS]),
 ("drawing: a via named downwards not turned up",
  [(DR, "if (b == nullptr || (a != nullptr && a->z0 > b->z0)) std::swap(a, b);", "if (b == nullptr) std::swap(a, b);")], [COLS]),
 ("drawing: boxes of a column may overlap",
  [(DR, "[&](const Item& it) { return z0 < it.z1 - eps && it.z0 < z1 - eps; });", "[&](const Item& it) { return false && z0 < it.z1 - eps && it.z0 < z1 - eps; });")], [COLS]),
 ("drawing: a metal no via joins not drawn",
  [(DR, "        columns[chosen] << metalItem(l);", "")], [COLS]),
 ("drawing: the gap under a lid left out",
  [(DR, "            if (l.variant != QLatin1String(\"perfect\") || !(l.thickness > 0)) continue;", "            continue;")], [COVERS]),
 ("drawing: an open ground's level drawn solid",
  [(DR, "i == 0 && d.ground.isEmpty()}", "false}")], [COVERS]),
 ("drawing: heights in proportion, not by the cube root",
  [(DR, "k * std::cbrt(std::max(0.0, (b.z1 - b.z0) * 1e6))", "k * (std::max(0.0, (b.z1 - b.z0) * 1e6))")], [LAID]),
 ("drawing: no least height",
  [(DR, "return std::max(thinnest, k * std::cbrt(", "return std::max(0.0, k * std::cbrt(")], [LAID]),
 ("drawing: the key never apart",
  [(DR, "const bool keyApart = drawH < std::max(220.0, n * 7.0);", "const bool keyApart = false;")], [APART]),
 ("drawing: the plane not painted",
  [(DR, "p.setBrush(plane ? QColor(70, 70, 70)", "p.setBrush(plane ? QColor(255, 255, 255)")], [PAINT]),
 ("drawing: a PDF read as a process file",
  [(F, "    if (format == StackFormat::Pdf) {", "    if (false) {")], [PDF]),
 ("drawing: a PDF with no suffix not known by its first bytes",
  [(F, "text.startsWith(\"%PDF\") ? StackFormat::Pdf : ", "")], [PDF]),
 ("drawing: the model writes the text, not the PDF's bytes",
  [(D, "    const QByteArray bytes = w.bytes();", "    const QByteArray bytes = w.text.toUtf8();")], [TOOLS]),
 ("drawing: the tool's format said as emx",
  [(C, "format == StackFormat::Emx ? QStringLiteral(\"emx\") : QStringLiteral(\"pdf\")", "QStringLiteral(\"emx\")")], [TOOLS]),
 ("drawing: the PDF filter's suffix not given a bare name",
  [(P, ": filter.contains(QLatin1String(\"*.pdf\")) ? QStringLiteral(\".pdf\")", ": false ? QStringLiteral(\".pdf\")")], [TOOLS]),
 ("the Stack's form without its buttons",
  [(P, "    if (n->type == QLatin1String(\"rf_stack\")) {\n        auto* addRow", "    if (false) {\n        auto* addRow")], [TOOLS]),
 ("the Stack's Export button not connected",
  [(P, "        connect(out, &QPushButton::clicked, this, &RF25Panel::exportStackFile);", "")], [TOOLS]),
]


def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is None:
            continue
        if p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))


def main():
    os.makedirs(os.path.join(RUNS, "cache"), exist_ok=True)
    if os.environ.get("BRK_ONLY_CHECK"):
        for name, edits, tests in BREAKS:
            apply(edits, None)
        print("%d breaks, every anchor once" % len(BREAKS))
        return 0
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        if which and i not in which:
            continue
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            log = os.path.join(RUNS, "brk%03d.log" % i)
            suites = sorted({t[0] for t in tests})
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    home = os.path.join(RUNS, "home")
                    os.makedirs(home, exist_ok=True)
                    env = dict(os.environ, HOME=home, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out, stderr=subprocess.STDOUT,
                                           stdin=subprocess.DEVNULL, timeout=900, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-62s %s" % (name[:62], said))
        if "BUILD FAILED" in said or "hung" in said or "caught)" not in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
