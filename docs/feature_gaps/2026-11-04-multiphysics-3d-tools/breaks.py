#!/usr/bin/env python3
"""breaks.py: Claude's fem tools on 3D models (4 November) undone on purpose,
one piece at a time: the tools' dispatch to a 3D model, a whole model's space
read, its points' unit, a line's points, an edge's level, a kind's integral,
a value's name, a reference, a sweep's points, a plot group's slices, clip and
surface, the camera, the mesh built, the format's kinds and an Extrude's
sketch, a 3D model kept 3D. Each built and its tests run; sources put back
whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/multiphysics3d-tools-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

FEM = "qucscontrol_fem.cpp"
TD = "test_multiphysics3d_doc"
A, P = (TD, "theToolsOnA3DModel"), (TD, "theToolsPlotAndExportA3DModel")

BREAKS = [
 # ---- the dispatch, the model
 ("fem_evaluate not sent to a 3D model",
  [(FEM, "    if (Multiphysics3DDoc* three = fem3DocumentFor(args, &error)) return fem3Evaluate(three, args);",
    "    if (Multiphysics3DDoc* three = false ? fem3DocumentFor(args, &error) : nullptr) return fem3Evaluate(three, args);")],
  [A]),
 ("a whole 3D model's format and space not read by fem_model new",
  [(FEM, "           || mo.value(QStringLiteral(\"format\")).toString().contains(QLatin1String(\" 3d \")) || first.value(QStringLiteral(\"space\")).toString() == QLatin1String(\"3D\");",
    "           ;")],
  [A]),
 ("a 3D model's space made 2D by fem_edit",
  [(FEM, "        if (!m.is3D()) return errorResult(tr(\"A 3D model stays 3D: a 2D one is the Multiphysics tab's, a .qfem. The model is unchanged.\"));", "")],
  [A]),
 ("its mesh not built by fem_build",
  [(FEM, "    if (mesh) doc->buildMesh();\n    else if (doc->geometryStale()) doc->requestGeometry();",
    "    if (mesh) {\n    } else if (doc->geometryStale()) doc->requestGeometry();")],
  [A]),
 # ---- fem_evaluate
 ("points taken in metres, not the geometry's unit",
  [(FEM, "                const std::optional<double> v = evaluateAt3(*s, e, Vec3{p.x * k, p.y * k, p.z * k});",
    "                const std::optional<double> v = evaluateAt3(*s, e, Vec3{p.x, p.y, p.z});")],
  [A]),
 ("a line's points short of its end",
  [(FEM, "                    const double t = double(i) / (n - 1);", "                    const double t = double(i) / n;")],
  [A]),
 ("'over' edge taken as faces",
  [(FEM, "            FNode node = doc->model().make(kind);\n            node.props.insert(QStringLiteral(\"level\"), over);",
    "            FNode node = doc->model().make(kind);\n            node.props.insert(QStringLiteral(\"level\"), over == QLatin1String(\"edge\") ? QStringLiteral(\"boundary\") : over);")],
  [A]),
 ("'kind' left out: an integral always",
  [(FEM, "            FNode node = doc->model().make(kind);", "            FNode node = doc->model().make(QStringLiteral(\"integral\"));")],
  [A]),
 ("a global's name left out",
  [(FEM, "            add(label, item.name, e.constant(), e.dim(), text, s->scope());", "            add(label, text, e.constant(), e.dim(), text, s->scope());")],
  [A]),
 ("a reference not measured",
  [(FEM, "    o.insert(QStringLiteral(\"values\"), valuesAgainst(values, want, expression, &worst, &worstName, &matched, &unmatched));",
    "    valuesAgainst(values, want, expression, &worst, &worstName, &matched, &unmatched);")],
  [A]),
 ("a sweep's point counted from 0",
  [(FEM, "    const std::vector<Instance3> at = all ? instances3(set, true) : instances3(set, false, args.value(QLatin1String(\"point\")).toInt(0), time, &why);",
    "    const std::vector<Instance3> at = all ? instances3(set, true) : instances3(set, false, args.value(QLatin1String(\"point\")).toInt(0) + 1, time, &why);")],
  [P]),
 # ---- fem_plot
 ("a slice's planes across z whatever is asked",
  [(FEM, "                plot(QStringLiteral(\"slice\"), {{QStringLiteral(\"plane\"), QString::fromLatin1(axis)},",
    "                plot(QStringLiteral(\"slice\"), {{QStringLiteral(\"plane\"), QStringLiteral(\"z\")},")],
  [P]),
 ("a plot group's clip not set",
  [(FEM, "                g.props.insert(QStringLiteral(\"clip\"), QString::fromLatin1(axis));", "")],
  [P]),
 ("'surface' false not heard",
  [(FEM, "        if (args.value(QLatin1String(\"surface\")).toBool(true)) plot(QStringLiteral(\"surface\"), {});", "        plot(QStringLiteral(\"surface\"), {});")],
  [P]),
 ("the camera's top the front",
  [(FEM, "        static const QHash<QString, View3D::Preset> presets{{QStringLiteral(\"top\"), View3D::Preset::Top},",
    "        static const QHash<QString, View3D::Preset> presets{{QStringLiteral(\"top\"), View3D::Preset::Front},")],
  [P]),
 ("a plot group's range not kept",
  [("multiphysics3ddoc.cpp", "        a_plotRange = {first->lo, first->hi};\n", "")],
  [P]),
 # ---- fem_describe's format
 ("a 3D model's format every kind",
  [(FEM, "        const bool three = space == QLatin1String(\"3D\") || (space.isEmpty() && fem3DocumentFor(args, &none) != nullptr);",
    "        const bool three = space == QLatin1String(\"3D\");")],
  [A]),
 ("an Extrude's sketch left out of a 3D model's format",
  [(FEM, "            if (three && !(k->spaces & Spaces::ThreeD) && !sketch.contains(k->type)) continue;", "            if (three && !(k->spaces & Spaces::ThreeD)) continue;")],
  [A]),
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
