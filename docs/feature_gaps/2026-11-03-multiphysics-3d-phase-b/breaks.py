#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's first steps (phase B of the proposal of 2
November) undone on purpose, one piece at a time - the schema's spaces (a
kind's, a choice's, a model's, the .qfem3d's format), the layered solids'
Form Union (cells, domains joined by the solids that hold them, faces joined
on one surface, edges broken where they meet, numbering, normals), the
selections in 3D, the layer stack over a layout (µm, its window, the clip),
booleans, the swept mesh (divisions in z, counts, sizes, faces, nodes), and
the document, panel, view and window, and what the DMG's check of 58c0fd7
found (the document tools and the Files panel knowing a 3D model, the 2D
tools refusing one) - each built and its tests run. Sources are put back
whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/multiphysics3d-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

FM, FG3, FM3 = "multiphysics/fem_model.cpp", "multiphysics/fem3_geometry.cpp", "multiphysics/fem3_mesh.cpp"
DOC, PAN, VIEW, WIN = "multiphysics3ddoc.cpp", "multiphysics3dpanel.cpp", "view3d.cpp", "qucs_multiphysics3d.cpp"
DOC2, QF = "multiphysicsdoc.cpp", "qucscontrol_fem.cpp"
DOC2P = "multiphysicspanel.cpp"
QC, FB = "qucscontrol.cpp", "filebrowser.cpp"
TC, TFB = "test_qucs_control", "test_file_browser"
TG, TM, TD, T2 = "test_fem3_geometry", "test_fem3_mesh", "test_multiphysics3d_doc", "test_multiphysics_doc"

BREAKS = [
 # ---- the schema
 ("Model::add takes a kind of another space",
  [(FM, "    if (!(k->spaces & spaceUnder(*parent))) {", "    if (false) {")],
  [(TG, "schema")]),

 ("a 3D model written as a 2D one's format",
  [(FM, "    o.insert(QStringLiteral(\"format\"), is3D() ? FormatName3D : FormatName);", "    o.insert(QStringLiteral(\"format\"), FormatName);")],
  [(TG, "schema"), (TD, "theExamples")]),

 ("a component's space 3D not read",
  [(FM, "        else if (space == QLatin1String(\"3d\")) comp.props.insert(QStringLiteral(\"space\"), QStringLiteral(\"3D\"));",
    "        else if (space == QLatin1String(\"3d\")) {}")],
  [(TG, "schema")]),

 ("the kinds under a node not filtered by space",
  [(FM, "        if (ck && (ck->spaces & space)) out << type;", "        if (ck) out << type;")],
  [(TG, "schema"), (TD, "aNewModel")]),

 ("every choice offered in every space",
  [(FM, "    return index < 0 || index >= choiceSpaces.size() || (choiceSpaces.at(index) & space) != 0;", "    return true;")],
  [(TG, "schema")]),

 ("an extrude's and a stack's objects not named",
  [(FM, "        if (f.type == QLatin1String(\"extrude\") || f.type == QLatin1String(\"stack\")) {", "        if (false) {")],
  [(TG, "schema")]),

 ("an extrude's sketch of the 3D space",
  [(FM, "    return parent.type == QLatin1String(\"extrude\") ? unsigned(Spaces::Plane) : space();", "    return space();")],
  [(TG, "schema"), (TD, "solidsThroughThePanel")]),

 # ---- cells and domains
 ("cells joined up through any solid",
  [(FG3, "            if (a >= 0 && a == sigOf(d, k + 1)) cells.join(std::size_t(d) * K + k, std::size_t(d) * K + k + 1);",
    "            if (a >= 0 && sigOf(d, k + 1) >= 0) cells.join(std::size_t(d) * K + k, std::size_t(d) * K + k + 1);")],
  [(TG, "stackedBoxes"), (TG, "layerStack")]),

 ("cells not joined across the plan's boundaries",
  [(FG3, "            if (a >= 0 && a == sigOf(bd.down, k)) cells.join(std::size_t(bd.up) * K + k, std::size_t(bd.down) * K + k);",
    "            (void)a;")],
  [(TG, "lShape"), (TG, "layerStack"), (TG, "booleans")]),

 ("a domain's owner the largest solid that holds it",
  [(FG3, "                double ownerVolume = INFINITY;", "                double ownerVolume = -INFINITY;"),
   (FG3, "                    if (v <= ownerVolume * (1 + 1e-12)) {", "                    if (v >= ownerVolume) {")],
  [(TG, "overlap"), (TG, "layerStack")]),

 # ---- faces
 ("walls not joined up the slabs",
  [(FG3, "            if (isAtom[a] && isAtom[a + 1] && atomSides[a] == atomSides[a + 1]) atoms.join(a, a + 1);", "            (void)a;")],
  [(TG, "layerStack")]),

 ("walls not joined across a point on one line",
  [(FG3, "                    if (sa == sc) atoms.join(a, c);", "                    (void)sc;")],
  [(TG, "lShape"), (TG, "booleans")]),

 ("floors not joined",
  [(FG3, "            if (isAtom[a] && isAtom[c] && atomSides[a] == atomSides[c]) atoms.join(a, c);", "            (void)c;")],
  [(TG, "lShape"), (TG, "booleans")]),

 ("a wall's triangles turned the other way",
  [(FG3, "        const bool turned = up != f.back;", "        const bool turned = up == f.back;")],
  [(TG, "aBox"), (TG, "lShape")]),

 ("a planar wall's normal reversed",
  [(FG3, "                        f.normal = n > 0 ? Vec3{along.y() / n, -along.x() / n, 0} : Vec3{};",
    "                        f.normal = n > 0 ? Vec3{-along.y() / n, along.x() / n, 0} : Vec3{};")],
  [(TG, "aBox")]),

 ("a face's normal out of its domain the wrong way",
  [(FG3, "    const double sign = domain == f.front ? -1 : 1;", "    const double sign = domain == f.front ? 1 : -1;")],
  [(TG, "aBox"), (TG, "aCylinder")]),

 # ---- edges, points, numbering
 ("edges not broken where three meet",
  [(FG3, "        if (ar.size() != 2) {\n            isBreak[c] = 1;\n            continue;\n        }", "        if (ar.size() != 2) {\n            continue;\n        }")],
  [(TG, "aBox"), (TG, "stackedBoxes")]),

 ("numbered from the left first",
  [(FG3, "    const double ka[6] = {a.z0, a.x0, a.y0, ma.z, ma.x, ma.y};\n    const double kb[6] = {b.z0, b.x0, b.y0, mb.z, mb.x, mb.y};",
    "    const double ka[6] = {a.x0, a.z0, a.y0, ma.x, ma.z, ma.y};\n    const double kb[6] = {b.x0, b.z0, b.y0, mb.x, mb.z, mb.y};")],
  [(TG, "aBox")]),

 # ---- selections
 ("the front side the back's",
  [(FG3, "            else if (side == QLatin1String(\"front\")) dirs.push_back({0, -1, 0});",
    "            else if (side == QLatin1String(\"front\")) dirs.push_back({0, 1, 0});")],
  [(TG, "aBox")]),

 ("a box's z left out",
  [(FG3, "            box.add(Vec3{num(b.at(0)), num(b.at(1)), num(b.at(2))});\n            box.add(Vec3{num(b.at(3)), num(b.at(4)), num(b.at(5))});",
    "            box.add(Vec3{num(b.at(0)), num(b.at(1)), -1e300});\n            box.add(Vec3{num(b.at(3)), num(b.at(4)), 1e300});")],
  [(TG, "selections")]),

 ("an exterior face's facing by its own normal, not out of its domain",
  [(FG3, "                    const Vec3 out_ = t.normalAt(i, f.back >= 0 ? f.back : f.front);", "                    const Vec3 out_ = f.normal;")],
  [(TG, "selections"), (TG, "aCylinder")]),

 ("in a plane: its lower side alone there",
  [(FG3, "                return std::abs(lo - at) <= eps && std::abs(hi - at) <= eps;", "                return std::abs(lo - at) <= eps;")],
  [(TG, "lShape"), (TG, "selections")]),

 # ---- the layer stack
 ("a stack's rows in the geometry's unit, not µm",
  [(FG3, "    double micrometres(const QString& text, const QString& what) { return evaluate(text, 1e-6, true, what) * 1e-6 / a_scale; }",
    "    double micrometres(const QString& text, const QString& what) { return evaluate(text, a_scale, true, what); }")],
  [(TG, "layerStack")]),

 ("the auto window without its margin",
  [(FG3, "            window = QRectF(QPointF(x0 - s.margin, y0 - s.margin), QPointF(x1 + s.margin, y1 + s.margin));",
    "            window = QRectF(QPointF(x0, y0), QPointF(x1, y1));")],
  [(TG, "layerStack")]),

 ("a layer's row not cut to the window",
  [(FG3, "            GObject o = l ? clipped(l->loops, um, window) : GObject();",
    "            GObject o = l ? clipped(l->loops, um, QRectF(-1e9, -1e9, 2e9, 2e9)) : GObject();")],
  [(TG, "layerStack")]),

 # ---- booleans, extrudes, errors
 ("a difference's tools not taken away",
  [(FG3, "            for (const Solid& t : tools)\n                if (t.holds(held)) return false;", "            for (const Solid& t : tools)\n                (void)t;")],
  [(TG, "booleans")]),

 ("an empty result not refused",
  [(FG3, "    for (int s = 0; s < S; ++s)\n        if (!(t->objectVolume[std::size_t(s)] > 0)) {",
    "    for (int s = 0; s < 0; ++s)\n        if (!(t->objectVolume[std::size_t(s)] > 0)) {")],
  [(TG, "booleans")]),

 ("an extrude's solids not of its name",
  [(FG3, "            s.group = group;", "            s.group = o.name;")],
  [(TG, "extrudeSketch")]),

 ("a failed build's error put over by the union's",
  [(FG3, "            if (!out.errors.contains(tag)) out.errors.insert(tag, why);", "            out.errors.insert(tag, why);")],
  [(TG, "errors")]),

 ("a solid thinner than the tolerance not refused as such",
  [(FG3, "        if (piece.k1 <= piece.k0) thin << piece.tag;", "        (void)thin;")],
  [(TG, "errors")]),

 # ---- the swept mesh
 ("divisions in z not graded",
  [(FM3, "    const double g = std::max(1.0, growth) - 1;", "    const double g = 1e9;")],
  [(TM, "divisions"), (TM, "counts")]),

 ("a count in z overruled by the size",
  [(FM3, "        const int n = given > 0 ? given : std::max(1, int(std::ceil(total - 1e-6)));",
    "        const int n = std::max(1, int(std::ceil(total - 1e-6)));")],
  [(TM, "divisions")]),

 ("a level asked fine not graded from",
  [(FM3, "                h = std::min(h, hLevel[std::size_t(i)] + g * std::abs(z - levels[std::size_t(i)]));", "                (void)g;")],
  [(TM, "divisions")]),

 ("a wall's quadrilaterals turned the other way",
  [(FM3, "            const bool turned = t.cell(bd.up, k) != t.faces[std::size_t(face)].back;",
    "            const bool turned = t.cell(bd.up, k) == t.faces[std::size_t(face)].back;")],
  [(TM, "aBox")]),

 ("a prism's top at its bottom's height",
  [(FM3, "                mesh->prisms.push_back({at(tri[0], j), at(tri[1], j), at(tri[2], j), at(tri[0], j + 1), at(tri[1], j + 1), at(tri[2], j + 1)});",
    "                mesh->prisms.push_back({at(tri[0], j), at(tri[1], j), at(tri[2], j), at(tri[0], j), at(tri[1], j), at(tri[2], j)});")],
  [(TM, "aBox")]),

 ("a domain's size not taken to its slabs",
  [(FM3, "                        lower(s.domainMax[std::size_t(pd)], h);\n                        lower(zmax[std::size_t(k)], h);",
    "                        lower(s.domainMax[std::size_t(pd)], h);")],
  [(TM, "sizes")]),

 ("a domain's size not taken to the plan",
  [(FM3, "                        lower(s.domainMax[std::size_t(pd)], h);\n                        lower(zmax[std::size_t(k)], h);",
    "                        lower(zmax[std::size_t(k)], h);")],
  [(TM, "sizes")]),

 ("a point's node at the bottom",
  [(FM3, "        mesh->pointNode.push_back(n >= 0 ? at(n, mesh->levelHeight[std::size_t(p.level)]) : -1);",
    "        mesh->pointNode.push_back(n >= 0 ? at(n, 0) : -1);")],
  [(TM, "aBox")]),

 ("a count across a domain given to each of its slabs whole",
  [(FM3, "                    zcount[std::size_t(k)] = std::max(zcount[std::size_t(k)], std::max(1, int(std::lround(n * thick / height))));",
    "                    zcount[std::size_t(k)] = std::max(zcount[std::size_t(k)], std::max(1, n + 0 * int(thick / height)));")],
  [(TM, "counts")]),

 # ---- the document, the panel, the view, the window
 ("an edit not building the geometry again",
  [(DOC, "    const bool geometry = read || before.geometry != after.geometry;", "    const bool geometry = read;")],
  [(TD, "solidsThroughThePanel")]),

 ("a feature's solids not drawn when it is chosen",
  [(DOC, "            if (t->domains[std::size_t(d)].objects.intersects(solids)) a_highlighted << d;", "            (void)d;")],
  [(TD, "solidsThroughThePanel")]),

 ("a selection not drawn when its node is chosen",
  [(DOC, "        a_highlighted = resolveSelection3(*t, a_model, level, n->selection());", "        a_highlighted.clear();")],
  [(TD, "theExamples")]),

 ("the 3D tree showing a study and the results",
  [(PAN, "    return node.type == QLatin1String(\"results\") || node.type == QLatin1String(\"study\");", "    return false;")],
  [(TD, "aNewModel")]),

 ("the view's farthest triangle drawn",
  [(VIEW, "                if (z <= d) continue;", "                if (z >= d && d > -std::numeric_limits<float>::infinity()) continue;")],
  [(TD, "theView")]),

 ("the view's cut ignored",
  [(VIEW, "    const bool clipping = a_clipAxis >= 0 && !b.isEmpty();", "    const bool clipping = false;")],
  [(TD, "theView")]),

 ("the 3D tab at the dock's end",
  [(WIN, "  const int at = after >= 0 ? after + 1 : TabView->count();", "  const int at = TabView->count();")],
  [(TD, "inTheWindow")]),

 ("a .qfem taken for a 3D model's file",
  [(WIN, "  return QFileInfo(name).suffix().compare(QLatin1String(\"qfem3d\"), Qt::CaseInsensitive) == 0;",
    "  return QFileInfo(name).suffix().startsWith(QLatin1String(\"qfem\"));")],
  [(TD, "inTheWindow")]),

 ("the 2D document opening a 3D model",
  [(DOC2, "    if (m->is3D()) {\n        misc::reportError(", "    if (false) {\n        misc::reportError(")],
  [(TD, "eachItsOwn")]),

 ("the 3D document opening a 2D model",
  [(DOC, "    if (!m->is3D()) {", "    if (false) {")],
  [(TD, "eachItsOwn")]),

 ("the format's 3D kinds not said to be",
  [(QF, "            if (!(k->spaces & Spaces::TwoD)) ko.insert(QStringLiteral(\"space\"), QStringLiteral(\"3D\"));\n", "")],
  [(T2, "claudesFormatOfAGroup")]),

 ("the 2D panel offering a 3D model's kinds",
  [(DOC2P, "            for (const QString& type : childKinds(*n, a_doc->model().spaceUnder(*n))) {", "            for (const QString& type : k->children) {")],
  [(TD, "theTwoDPanelKeepsToTwoD")]),

 ("the 2D panel offering a 3D selection's edges",
  [(DOC2P, "            if (p.choiceIn(i, space) || node.text(key) == p.choices[i]) box->addItem(", "            if (space || node.text(key) == p.choices[i]) box->addItem(")],
  [(TD, "theTwoDPanelKeepsToTwoD")]),

 ("the 3D panel's selections without edges",
  [(PAN, "            if (p.choiceIn(i, space) || node.text(key) == p.choices[i]) box->addItem(", "            if ((p.choiceIn(i, space) && i < 3) || node.text(key) == p.choices[i]) box->addItem(")],
  [(TD, "aNewModel")]),

 # ---- the DMG's check of 58c0fd7
 ("a 3D model's tab said to be a schematic",
  [(QC, '    if (QucsApp::isMultiphysics3DDocument(QucsApp::documentWidget(doc))) return QStringLiteral("3D multiphysics model");\n', '')],
  [(TC, "theWorkspaceIsListed")]),

 (".qfem3d files left out of list_documents",
  [(QC, '        {QStringLiteral("qfem3d"), QStringLiteral("3D multiphysics model")},\n', '')],
  [(TC, "theWorkspaceIsListed")]),

 ("the 3D kind refused as list_documents' filter",
  [(QC, '                                   QStringLiteral("3D multiphysics model"), QStringLiteral("presentation")};',
    '                                   QStringLiteral("presentation")};')],
  [(TC, "theWorkspaceIsListed")]),

 ("a 3D model saved as a name without its suffix",
  [(QC, '                                                   {QStringLiteral("3D multiphysics model"), QStringLiteral(".qfem3d")}};',
    '                                                   };')],
  [(TC, "theWorkspaceIsListed")]),

 ("a 3D model saved under the 2D model's suffix",
  [(QC, ',\n                                                      {QStringLiteral("3D multiphysics model"), QStringLiteral("qfem3d")}};', '};')],
  [(TC, "theWorkspaceIsListed")]),

 ("the Files panel not knowing a .qfem3d",
  [(FB, '    {"qfem3d", QT_TRANSLATE_NOOP("FileBrowser", "Qucs-S 3D multiphysics model"), "3D", 0x6a3fa8, Kind::Mesh, true},\n', '')],
  [(TFB, "filesAreKnownByKind")]),

 ("fem_model new makes a 2D tab of a 3D model",
  [(QF, '            if (m->is3D())\n                return errorResult(', '            if (false)\n                return errorResult(')],
  [(T2, "aThreeDModelIsRefusedByTheTwoDTools")]),

 ("fem_model set puts a 3D model in a 2D tab",
  [(QF, '        if (m->is3D())\n            return errorResult(tr("\'model\' is a 3D model (its format or its space): Multiphysics 3D opens it, as a .qfem3d - "\n'
        '                                  "fem_model sets',
    '        if (false)\n            return errorResult(tr("\'model\' is a 3D model (its format or its space): Multiphysics 3D opens it, as a .qfem3d - "\n'
        '                                  "fem_model sets')],
  [(T2, "aThreeDModelIsRefusedByTheTwoDTools")]),

 ("fem_edit makes a 2D model's space 3D",
  [(QF, '    if (m.is3D())\n        return errorResult(tr("A 2D model', '    if (false)\n        return errorResult(tr("A 2D model')],
  [(T2, "aThreeDModelIsRefusedByTheTwoDTools")]),

 ("list_documents' schema without the 3D kind",
  [(QC, '"RF 2.5D model", "3D multiphysics model", "presentation"]', '"RF 2.5D model", "presentation"]')],
  [(TC, "theWorkspaceIsListed")]),
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
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=CACHE,
                                                    ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude",
                                                    QUCS_GH="/nonexistent/gh"))
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
