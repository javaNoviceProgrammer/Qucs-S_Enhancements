#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's plot groups (4 November) undone on purpose, one
piece at a time: the prisms' split into tetrahedra, the marching
quadrilateral's split, a crossing's place, a slice's values, the arrows' grid,
the default plot groups, the Show box's groups, a group's clip and its cut
face, the view's cut, a slice's planes, the Plot button, a plot's problems
said, a group removed, and the schema. Each built and its tests run; sources
put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/multiphysics3d-plot-group-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

RS, MODEL, DOC = "multiphysics/fem3_results.cpp", "multiphysics/fem_model.cpp", "multiphysics3ddoc.cpp"
PANEL, VIEW = "multiphysics3dpanel.cpp", "view3d.cpp"
TS, TD = "test_fem3_solve", "test_multiphysics3d_doc"

BREAKS = [
 # ---- isosurfaces and slices
 ("a prism split by its own corners, not the least global number",
  [(RS, "    int least = 0;\n    for (int i = 1; i < 6; ++i)\n        if (g[std::size_t(i)] < g[std::size_t(least)]) least = i;",
    "    int least = 0;")],
  [(TS, "isosurfacesAndSlices")]),
 ("a quadrilateral's two triangles overlapping",
  [(RS, "    out.push_back({q0, q1, q2});\n    out.push_back({q0, q2, q3});", "    out.push_back({q0, q1, q2});\n    out.push_back({q0, q1, q3});")],
  [(TS, "isosurfacesAndSlices")]),
 ("a crossing at its edge's middle",
  [(RS, "        const double u = a == b ? 0.5 : a / (a - b);", "        const double u = 0.5;")],
  [(TS, "isosurfacesAndSlices")]),
 ("a slice's values at its prism's first corner",
  [(RS, "                    ev.at(p, tri[std::size_t(k)].ref[0], tri[std::size_t(k)].ref[1], tri[std::size_t(k)].ref[2]);",
    "                    ev.at(p, 0, 0, 0);")],
  [(TS, "isosurfacesAndSlices")]),
 ("arrows' points in the geometry's unit, not metres",
  [(RS, "                if (!ev.at(Vec3{k * at.x, k * at.y, k * at.z})) continue;", "                if (!ev.at(at)) continue;")],
  [(TS, "isosurfacesAndSlices")]),
 ("arrows at their cells' corners",
  [(RS, "const Vec3 at{box.x0 + (i + 0.5) * (box.x1 - box.x0) / nx,", "const Vec3 at{box.x0 + i * (box.x1 - box.x0) / nx,")],
  [(TS, "isosurfacesAndSlices")]),
 # ---- the tab
 ("no default plot groups after a solve",
  [(DOC, "    if (a_show != Show::Plot) a_show = Show::Solution;\n    addDefaultPlots();", "    if (a_show != Show::Plot) a_show = Show::Solution;")],
  [(TD, "plotGroupsInTheTab")]),
 ("the Show box without the plot groups",
  [(DOC, "    if (!a_focus.isEmpty() && !a_model.find(a_focus)) a_focus.clear();\n    fillShowChoices();",
    "    if (!a_focus.isEmpty() && !a_model.find(a_focus)) a_focus.clear();")],
  [(TD, "plotGroupsInTheTab")]),
 ("a group's clip not set on the view",
  [(DOC, "        if (a_clipBox->currentData().toInt() < 0) a_view->setClip(axis, share);", "")],
  [(TD, "plotGroupsInTheTab")]),
 ("a group's cut face not drawn",
  [(DOC, "        if (*clipAxis >= 0)\n            for (const SliceTriangle& st : slice(*sol, first->e, *clipAxis, clipAt)) {",
    "        if (false)\n            for (const SliceTriangle& st : slice(*sol, first->e, *clipAxis, clipAt)) {")],
  [(TD, "plotGroupsInTheTab")]),
 ("the view's cut without its hair of slack",
  [(VIEW, "        clipValue = lo + (hi - lo) * a_clipAt + 1e-9 * (hi - lo);", "        clipValue = lo + (hi - lo) * a_clipAt;")],
  [(TD, "aFaceOnTheCutIsWhole")]),
 ("a slice's planes at their cells' lower sides",
  [(DOC, "                    for (int i = 0; i < n; ++i) at.push_back(a0 + (i + 0.5) * (a1 - a0) / n);",
    "                    for (int i = 0; i < n; ++i) at.push_back(a0 + i * (a1 - a0) / n);")],
  [(TD, "plotGroupsInTheTab")]),
 ("a plot's expression that does not read not said",
  [(DOC, "        if (!e.isValid()) {\n            a_plotProblems << tr(\"%1: %2\").arg(p.name(), e.error());\n            continue;\n        }\n        QString warning;",
    "        if (!e.isValid()) continue;\n        QString warning;")],
  [(TD, "plotGroupsInTheTab")]),
 ("a group removed still shown",
  [(DOC, "        if (!g || g->type != QLatin1String(\"plotgroup3d\")) a_show = a_solution ? Show::Solution : Show::Geometry;", "")],
  [(TD, "plotGroupsInTheTab")]),
 ("the panel's Plot button left out",
  [(PANEL, "    } else if (n->type == QLatin1String(\"plotgroup3d\")) {\n        auto* plot = new QPushButton(tr(\"Plot\"), a_form);",
    "    } else if (n->type == QLatin1String(\"plotgroup3dx\")) {\n        auto* plot = new QPushButton(tr(\"Plot\"), a_form);")],
  [(TD, "plotGroupsInTheTab")]),
 # ---- the schema
 ("a 3D plot group not among the results' children",
  [(MODEL, "         {QStringLiteral(\"plotgroup\"), QStringLiteral(\"plotgroup1d\"), QStringLiteral(\"cutline\"), QStringLiteral(\"cutpoint\"),\n          QStringLiteral(\"plotgroup3d\")},",
    "         {QStringLiteral(\"plotgroup\"), QStringLiteral(\"plotgroup1d\"), QStringLiteral(\"cutline\"), QStringLiteral(\"cutpoint\")},")],
  [(TD, "plotGroupsInTheTab")]),
 ("a 3D plot group offering the plane's contours",
  [(MODEL, "             {QStringLiteral(\"surface\"), QStringLiteral(\"slice\"), QStringLiteral(\"isosurface\"), QStringLiteral(\"arrowvolume\"), QStringLiteral(\"meshplot\")},",
    "             {QStringLiteral(\"surface\"), QStringLiteral(\"slice\"), QStringLiteral(\"isosurface\"), QStringLiteral(\"arrowvolume\"), QStringLiteral(\"meshplot\"),\n              QStringLiteral(\"contour\")},"),
   (MODEL, "        if (kind.type == QLatin1String(\"surface\") || kind.type == QLatin1String(\"meshplot\")) kind.spaces = Spaces::All;",
    "        if (kind.type == QLatin1String(\"surface\") || kind.type == QLatin1String(\"meshplot\") || kind.type == QLatin1String(\"contour\")) kind.spaces = Spaces::All;")],
  [(TD, "plotGroupsInTheTab")]),
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
                if subprocess.run(["ninja", "-C", B] + ["qucs/tests/" + s for s in suites], stdout=out,
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
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
