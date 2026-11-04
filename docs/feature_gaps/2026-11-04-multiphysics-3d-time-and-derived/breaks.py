#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's heat in time, its Parametric Sweep and its
derived values (4 November) undone on purpose, one piece at a time: the
capacity's lumping along z, its consistency in the plane and at order 2, its
history, BDF2's weights, the midpoint rule's second half, the start's
snapshot and heat, a snapshot's globals, a terminal of t, ρ Cp required, a
sweep's meshes, parameters and units, the tab's Time and Point boxes, the
physics at rest at each step, the schema; a volume's and an edge's weights,
an edge's points, the extrema's corners, an average, a point's unit, the
output time asked, the instances' labels, the time shown after an
evaluation, the results in the tree, the points' columns, the edges offered, a results edit kept off the solution.
Each built and its tests run; sources put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/multiphysics3d-time-derived-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

SC, SV, RS = "multiphysics/fem3_scalar.cpp", "multiphysics/fem3_solver.cpp", "multiphysics/fem3_results.cpp"
PANEL = "multiphysics3dpanel.cpp"
FS, MODEL, DOC = "multiphysics/fem_solver.cpp", "multiphysics/fem_model.cpp", "multiphysics3ddoc.cpp"
TS, TD = "test_fem3_solve", "test_multiphysics3d_doc"

BREAKS = [
 # ---- the capacity
 ("order 1's capacity consistent along z",
  [(SC, "if (massed && k == l) Me[std::size_t(i * per + j)] += w * capv * t.n[a] * t.n[b] * L[k];",
    "if (massed) Me[std::size_t(i * per + j)] += w * capv * t.n[a] * t.n[b] * L[k] * L[l];")],
  [(TS, "tallPrismsInTime")]),
 ("order 1's capacity lumped in the plane too",
  [(SC, "if (massed && k == l) Me[std::size_t(i * per + j)] += w * capv * t.n[a] * t.n[b] * L[k];",
    "if (massed && k == l && a == b) Me[std::size_t(i * per + j)] += w * capv * t.n[a] * L[k];")],
  [(TS, "heatInTimeInvariantInZ")]),
 ("order 2's capacity lumped",
  [(SC, "                        if (massed) Me[std::size_t(i * per + j)] += w * capv * s.n[i] * s.n[j];",
    "                        if (massed && i == j) Me[std::size_t(i * per + j)] += w * capv * s.n[i];")],
  [(TS, "heatInTimeInvariantInZ")]),
 ("the capacity's history left out of the vector",
  [(SC, "                    if (history) Fe[std::size_t(i)] += v * (*history)[std::size_t(dofs[std::size_t(j)])];", "")],
  [(TS, "energyInTime"), (TS, "slabInTime")]),
 ("ρ Cp not required for a step in time",
  [(SC, "            if (f.domains[std::size_t(d)] && !coef.capacity[std::size_t(d)].isValid()) {",
    "            if (false && f.domains[std::size_t(d)] && !coef.capacity[std::size_t(d)].isValid()) {")],
  [(TS, "refusals")]),
 ("the start's heat not measured",
  [(SC, "        if (!a_assembled) {\n            // (A step in time's start", "        if (false) {\n            // (A step in time's start")],
  [(TS, "energyInTime")]),
 ("a terminal of t read at t = 0",
  [(SC, "d.kind == K::Time ? a_s.time() : NaN;", "d.kind == K::Time ? 0.0 : NaN;")],
  [(TS, "terminalInTime")]),
 # ---- the steps
 ("BDF2's second history weight ω/(1 + ω)",
  [(SV, "const double a1 = (1 + omega), a2 = omega * omega / (1 + omega);", "const double a1 = (1 + omega), a2 = omega / (1 + omega);")],
  [(TS, "energyInTime")]),
 ("the implicit midpoint rule's second half dropped",
  [(SV, "                for (std::size_t k = 0; k < v.size() && k < un.size(); ++k) v[k] = 2 * v[k] - un[k];", "")],
  [(TS, "energyInTime")]),
 ("the physics at rest not solved at each step",
  [(SV, "        const bool ok = solveTogether(problems, subset, [&](int i) {", "        const bool ok = solveTogether(problems, stepped.empty() ? subset : stepped, [&](int i) {")],
  [(TS, "terminalInTime")]),
 ("no snapshot at the start",
  [(SV, "    setSolvedGlobals(problems, subset);\n    s.addSnapshot();\n    s.timeDependent = true;",
    "    setSolvedGlobals(problems, subset);\n    s.timeDependent = true;")],
  [(TS, "heatInTimeInvariantInZ"), (TD, "aModelInTime")]),
 ("a snapshot's globals not taken when chosen",
  [(SV, "    for (const auto& [name, g] : s.globals) {\n        a_globals[name] = g;\n        a_globalScope->setConstant(name, g.value, g.dim);\n    }", "")],
  [(TS, "heatInTimeInvariantInZ"), (TD, "aModelInTime")]),
 # ---- the sweep
 ("a sweep's length on the model's own mesh",
  [(SV, "        if (plan->reshapes(changed)) {\n            if (!meshOf(*ps, nullptr, sub(0, 0.3), &m)) {",
    "        if (false) {\n            if (!meshOf(*ps, nullptr, sub(0, 0.3), &m)) {")],
  [(TS, "aSweep")]),
 ("a sweep's points solved with the model's own parameters",
  [(SV, "        StudyResult3 r = solveStudy3(model, ps, m, study->tag, sub(0.3, 1));",
    "        StudyResult3 r = solveStudy3(model, parameters, m, study->tag, sub(0.3, 1));")],
  [(TS, "aSweep"), (TD, "aSweptModel")]),
 ("a sweep's plain numbers not in their row's unit",
  [(FS, "            for (double& v : *values) v = v * u->scale + u->offset;", "")],
  [(TS, "aSweep")]),
 ("the sweep a step of the study",
  [(SV, "        if (!step.enabled || step.type == QLatin1String(\"sweep\")) continue;", "        if (!step.enabled) continue;")],
  [(TS, "aSweep")]),
 # ---- the tab and the schema
 ("the tab's plot of the time it last drew",
  [(DOC, "            if (a_plotCache.solution != a_solution || a_plotCache.snapshot != sol.selected() || a_plotCache.text != text)",
    "            if (a_plotCache.solution != a_solution || a_plotCache.text != text)")],
  [(TD, "aModelInTime")]),
 ("the tab's Point box choosing nothing",
  [(DOC, "    a_solution = a_solutions[std::size_t(index)];\n    if (time >= 0", "    if (time >= 0")],
  [(TD, "aSweptModel")]),
 ("no Time Dependent step in 3D",
  [(MODEL, 'return type == QLatin1String("study") || type == QLatin1String("stationary") || type == QLatin1String("transient")\n               || type == QLatin1String("sweep");',
    'return type == QLatin1String("study") || type == QLatin1String("stationary")\n               || type == QLatin1String("sweep");')],
  [(TD, "physicsThroughThePanel")]),
 ("Newton offered for a step in time",
  [(MODEL, '        if (kind.type == QLatin1String("transient"))\n            for (P& p : kind.properties)\n                if (p.key == QLatin1String("nonlinear")) p.choiceSpaces = {Spaces::All, Spaces::TwoD, Spaces::All};', "")],
  [(TD, "physicsThroughThePanel")]),
 # ---- derived values
 ("a volume's quadrature without its Jacobian",
  [(RS, "                take(q[3] * std::abs(map.det), ev.eval(e));", "                take(q[3], ev.eval(e));")],
  [(TS, "derivedValues")]),
 ("an edge's points in the geometry's unit, not metres",
  [(RS, "const Vec3 x{k * (a.x + t * (b.x - a.x)), k * (a.y + t * (b.y - a.y)), k * (a.z + t * (b.z - a.z))};",
    "const Vec3 x{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};")],
  [(TS, "derivedValues")]),
 ("an edge's Gauss weights 4/18 in the middle",
  [(RS, "constexpr double GaussWeight[3] = {5.0 / 18, 8.0 / 18, 5.0 / 18};", "constexpr double GaussWeight[3] = {5.0 / 18, 4.0 / 18, 5.0 / 18};")],
  [(TS, "derivedValues")]),
 ("the extrema without the cells' corners",
  [(RS, "            for (const auto& c : corner) {\n                ev.at(p, c[0], c[1], c[2]);\n                extreme(ev.eval(e));\n            }\n", "")],
  [(TS, "derivedValues")]),
 ("an average the integral",
  [(RS, "add(text, o.integral / o.measure, e.dim(), text);", "add(text, o.integral, e.dim(), text);")],
  [(TS, "derivedValues")]),
 ("a point's coordinates in metres",
  [(RS, "                    options.lengthUnit = k;\n", "")],
  [(TS, "derivedValues")]),
 ("an output time asked: the last given",
  [(RS, "        snapshot = s.snapshotAt(*t);", "        (void)t;")],
  [(TS, "derivedValues")]),
 ("an instance's label without its time",
  [(RS, '            if (snapshot >= 0 && s.timeDependent) label << QStringLiteral("t = %1").arg(formatQuantity(in.time, Dim::of(0, 0, 1)));', "")],
  [(TS, "derivedValues"), (TD, "derivedValuesInTheTab")]),
 ("the time shown not shown again after an evaluation",
  [(DOC, "    if (a_solution && shown >= 0) a_solution->select(shown);", "")],
  [(TD, "derivedValuesInTheTab")]),
 ("a 3D model's results hidden from its tree",
  [(PANEL, "    for (const Node& c : node.children) {\n        auto* item = new QTreeWidgetItem(parent);",
    "    for (const Node& c : node.children) {\n        if (c.type == QLatin1String(\"results\")) continue;\n        auto* item = new QTreeWidgetItem(parent);")],
  [(TD, "aNewModel"), (TD, "derivedValuesInTheTab")]),
 ("a point's editor two columns",
  [(PANEL, "    const int n = int(columns.size());", "    const int n = 2;")],
  [(TD, "derivedValuesInTheTab")]),
 ("a results edit leaves the solution stale",
  [(DOC, "    if (read || before.solved != after.solved) {\n        ++a_solvedRevision;", "    if (true) {\n        ++a_solvedRevision;")],
  [(TD, "derivedValuesInTheTab")]),
 ("edges not offered as a level in 3D",
  [(MODEL, "        level.choiceSpaces = {Spaces::All, Spaces::All, Spaces::ThreeD};", "        level.choiceSpaces = {Spaces::All, Spaces::All, Spaces::TwoD};")],
  [(TD, "derivedValuesInTheTab")]),
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
