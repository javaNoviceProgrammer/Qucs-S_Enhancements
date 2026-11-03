#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's phase C begun (3 November) undone on purpose,
one piece at a time - the prisms' shapes and rules, the space's levels and
maps, the faces' points, the scalar physics (a linear prism's lumping, ε0,
films, fluxes, held values, floating groups, reactions, the terminal sweep,
Picard's iterations, Joule heat, a crystal's axes in 3D, a total power, what is
refused), the study, the slices and nodal values, multigrid, the window
(Compute, the solution kept, its staleness, the legend, the cut's slice, F2,
the panel) and the schema - each built and its tests run. Sources are put
back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/person/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/qucs-s-26.1.1/qucs/"
B = "/Users/person/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/multiphysics3d-c-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

FE, SC, SO, RE = "multiphysics/fem3_fe.cpp", "multiphysics/fem3_scalar.cpp", "multiphysics/fem3_solver.cpp", "multiphysics/fem3_results.cpp"
AMG, ME, FM = "multiphysics/fem_amg.cpp", "multiphysics/fem3_mesh.cpp", "multiphysics/fem_model.cpp"
DOC, PAN, VIEW, APP = "multiphysics3ddoc.cpp", "multiphysics3dpanel.cpp", "view3d.cpp", "qucs.cpp"
TFE, TSO, TAMG, TD, TG = "test_fem3_fe", "test_fem3_solve", "test_fem_amg", "test_multiphysics3d_doc", "test_fem3_geometry"

BREAKS = [
 # ---- the elements
 ("order 2's middle function along z halved",
  [(FE, "        l[1] = 4 * zeta * (1 - zeta);", "        l[1] = 2 * zeta * (1 - zeta);")],
  [(TFE, "prismShapes")]),
 ("order 1's derivative along z turned",
  [(FE, "        dl[0] = -1;\n        dl[1] = 1;", "        dl[0] = 1;\n        dl[1] = -1;")],
  [(TFE, "prismShapes")]),
 ("a prism rule's line weight dropped",
  [(FE, "r.points.push_back({p[0], p[1], q[0], p[2] * q[1]});", "r.points.push_back({p[0], p[1], q[0], p[2]});")],
  [(TFE, "prismRules")]),
 ("order 2's layer middles at the layer's bottom",
  [(FE, "                const int level = 2 * j + lz;", "                const int level = 2 * j + (lz == 1 ? 0 : lz);")],
  [(TFE, "spaces")]),
 ("the map's z its top's alone",
  [(FE, "    out.x = Vec3{p.x.x(), p.x.y(), z0 + zeta * (z1 - z0)};", "    out.x = Vec3{p.x.x(), p.x.y(), z0 + zeta * z1};")],
  [(TFE, "spaces")]),
 ("the map's Jacobian without its height",
  [(FE, "    out.det = p.det * (z1 - z0);", "    out.det = p.det;")],
  [(TFE, "spaces")]),
 ("a wall's area without its edge's length",
  [(FE, "            fp.weight = qs[1] * qz[1] * jacobian * (z1 - z0);", "            fp.weight = qs[1] * qz[1] * (z1 - z0);")],
  [(TFE, "facePoints")]),
 ("a floor's prism below read at its bottom",
  [(FE, "            fp.zeta = side == 0 ? 0.0 : 1.0;", "            fp.zeta = 0.0;")],
  [(TFE, "facePoints")]),
 ("a wall's edge run one way in every triangle",
  [(FE, "                const double u = forward ? s : 1 - s;", "                const double u = s;")],
  [(TFE, "facePoints")]),
 ("a wall's height one too low",
  [(ME, "                f.plan = int(ei);\n                f.height = j;", "                f.plan = int(ei);\n                f.height = j > 0 ? j - 1 : j;")],
  [(TFE, "facePoints")]),
 # ---- the physics
 ("a linear prism's vertical term consistent in the plane",
  [(SC, "                        if (a == b) v += C[8] * t.n[a] * dL[k] * dL[l];", "                        v += C[8] * t.n[a] * t.n[b] * dL[k] * dL[l];")],
  [(TSO, "thinLayersStayWithinBounds"), (TD, "aModelSolvedInItsTab")]),
 ("a linear prism's in-plane term consistent along z",
  [(SC, "                        if (k == l)\n                            v += L[k] * (gT[a][0]", "                        v += L[k] * L[l] * (gT[a][0]")],
  [(TSO, "tallPrismsStayWithinBounds")]),
 ("electrostatics without ε0",
  [(SC, "    a_scale = a_es ? Eps0 : 1.0;", "    a_scale = 1.0;")],
  [(TSO, "coaxialLiner")]),
 ("a film's h not in the matrix",
  [(SC, "for (int j = 0; j < m; ++j) Kf[std::size_t(i * m + j)] += fp.weight * h * fp.n[i] * fp.n[j];", "for (int j = 0; j < m; ++j) (void)j;")],
  [(TSO, "filmAndRadiation"), (TSO, "invariantInZ")]),
 ("a face's flux not added",
  [(SC, "                    for (int i = 0; i < m; ++i) Ff[std::size_t(i)] += fp.weight * g * fp.n[i];", "                    (void)g;")],
  [(TSO, "filmAndRadiation")]),
 ("a held value read with x and y swapped",
  [(SC, "values[std::size_t(slot)] = d.kind == K::X ? x.x : d.kind == K::Y ? x.y", "values[std::size_t(slot)] = d.kind == K::X ? x.y : d.kind == K::Y ? x.x")],
  [(TSO, "patchTests")]),
 ("a floating group's dofs each their own",
  [(SC, "        if (group[dof] && q.terminal[dof] >= 0) {", "        if (false) {")],
  [(TSO, "terminals")]),
 ("a reaction with its sources added",
  [(SC, "    const Eigen::VectorXd r = a.R * x + a.Rd * ud - a.Fd;", "    const Eigen::VectorXd r = a.R * x + a.Rd * ud + a.Fd;")],
  [(TSO, "terminals")]),
 ("the sweep's other terminals at 1 V too",
  [(SC, "            out[k] = q.terminal[std::size_t(dof)] == sweepTerminal ? 1.0 : 0.0;", "            out[k] = q.terminal[std::size_t(dof)] >= 0 ? 1.0 : 0.0;")],
  [(TSO, "terminals"), (TSO, "invariantInZ")]),
 ("Picard's iterations one",
  [(SC, "    const int rounds = a_self ? o.maxIterations : 1;", "    const int rounds = 1;")],
  [(TSO, "filmAndRadiation"), (TD, "aModelSolvedInItsTab")]),
 ("a crystal's axes the plane's",
  [(SC, "    static const int from[3] = {0, 2, 1};", "    static const int from[3] = {0, 1, 2};")],
  [(TSO, "crystalTensor")]),
 ("a total power not spread by volume",
  [(SC, "                scale = *p / total;", "                scale = *p;")],
  [(TSO, "thinLayersStayWithinBounds"), (TD, "aModelSolvedInItsTab")]),
 ("a field held nowhere solved",
  [(SC, "        if (it == 0 && q.fixedDof.empty() && !a.robin) {", "        if (false) {")],
  [(TSO, "refusals")]),
 ("a user's plane tensor taken in 3D",
  [(SC, "            if (aniso == QLatin1String(\"diagonal\") || aniso == QLatin1String(\"symmetric\"))\n                problem(",
    "            if (false)\n                problem(")],
  [(TSO, "refusals")]),
 # ---- the study and the variables
 ("coupled physics one round",
  [(SO, "    const int rounds = coupled ? o.maxIterations : 1;", "    const int rounds = 1;")],
  [(TSO, "jouleHeating")]),
 ("Joule heat J alone",
  [(SO, "                for (int i = 0; i < 3; ++i) s += ev.value(flux[i]) * ev.value(e[i]);", "                for (int i = 0; i < 3; ++i) s += ev.value(flux[i]);")],
  [(TSO, "jouleHeating")]),
 ("E as +∇V",
  [(SO, "tr(\"electric field, %1 component\").arg(QString::fromLatin1(axis[i])), K::Gradient, f, i, -1);",
    "tr(\"electric field, %1 component\").arg(QString::fromLatin1(axis[i])), K::Gradient, f, i, 1);")],
  [(TSO, "terminals")]),
 ("a point's ζ not scaled by its layer's height",
  [(SO, "    const double zeta = std::clamp((z - h[std::size_t(j)]) / (h[std::size_t(j) + 1] - h[std::size_t(j)]), 0.0, 1.0);",
    "    const double zeta = std::clamp(z - h[std::size_t(j)], 0.0, 1.0);")],
  [(TSO, "filmAndRadiation")]),
 ("a step of another kind solved as at rest",
  [(SO, "        if (step.type != QLatin1String(\"stationary\")) {", "        if (false) {")],
  [(TSO, "refusals")]),
 # ---- results
 ("a z slice read at its layer's bottom",
  [(RE, "        const double zeta = (at - h[std::size_t(j)]) / (h[std::size_t(j) + 1] - h[std::size_t(j)]);", "        const double zeta = 0;")],
  [(TSO, "slices")]),
 ("an x slice's quadrilateral its bottom twice",
  [(RE, "b1{B.x.x(), B.x.y(), z1}, a1{A.x.x(), A.x.y(), z1};", "b1{B.x.x(), B.x.y(), z0}, a1{A.x.x(), A.x.y(), z1};")],
  [(TSO, "slices")]),
 ("nodal values summed, not averaged",
  [(RE, "        if (count[n] > 0) out[n] = sum[n] / count[n];", "        if (count[n] > 0) out[n] = sum[n];")],
  [(TSO, "slices")]),
 # ---- multigrid and the system
 ("multigrid's tolerance not set",
  [(AMG, "            prm.solver.tol = d.tolerance;\n", "")],
  [(TAMG, "solvesALaplacian")]),
 ("no fallback where multigrid fails",
  [(AMG, "    // The incomplete Cholesky's conjugate gradients, from x as given.\n    r.fallback = true;",
    "    // The incomplete Cholesky's conjugate gradients, from x as given.\n    if (report) *report = r;\n    return false;\n    r.fallback = true;")],
  [(TAMG, "fallsBack")]),
 ("a direct solve asked, multigrid given",
  [(SO, "    const bool direct = solver == QLatin1String(\"direct\")", "    const bool direct = false")],
  [(TSO, "iterativeAndDirect")]),
 # ---- the window
 ("a solve's solution not kept",
  [(DOC, "    a_solution = r.solution;\n    a_solutionStale = false;", "    a_solutionStale = false;")],
  [(TD, "aModelSolvedInItsTab")]),
 ("an edit leaves the solution fresh",
  [(DOC, "    if (a_solution) a_solutionStale = true;\n", "")],
  [(TD, "aModelSolvedInItsTab")]),
 ("the legend never shown",
  [(DOC, "            legend.shown = true;", "            legend.shown = false;")],
  [(TD, "aModelSolvedInItsTab")]),
 ("the cut's slice not drawn",
  [(DOC, "                cut = slice(sol, e, axis, lo + (hi - lo) * a_clipSlider->value() / 100.0);", "                (void)lo;")],
  [(TD, "aModelSolvedInItsTab")]),
 ("F2 not computing a 3D model",
  [(APP, "      model3->compute();   // its first study (Multiphysics 3D > Compute)", "      (void)model3;")],
  [(TD, "aModelSolvedInItsTab")]),
 ("the panel showing the plane's properties",
  [(PAN, "            if (!(p.spaces & a_doc->model().space()) || !propertyVisible(*n, p, k)) continue;", "            if (!propertyVisible(*n, p, k)) continue;")],
  [(TD, "physicsThroughThePanel")]),
 ("the study hidden",
  [(PAN, "    return node.type == QLatin1String(\"results\");", "    return node.type == QLatin1String(\"results\") || node.type == QLatin1String(\"study\");")],
  [(TD, "aNewModel")]),
 ("the view's colours not blended",
  [(VIEW, "                row[x] = !t.shaded ? rgb", "                row[x] = true ? rgb")],
  [(TD, "theView")]),
 # ---- the schema
 ("the scalar physics the plane's alone",
  [(FM, "            if (type == QLatin1String(p) || type.startsWith(QLatin1String(p) + QLatin1Char('/'))) return true;", "            (void)p;")],
  [(TG, "schema"), (TD, "physicsThroughThePanel")]),
 ("a 3D physics made of order 2",
  [(FM, "k->property(QStringLiteral(\"order\"))) n.props.insert(QStringLiteral(\"order\"), QStringLiteral(\"1\"));",
    "k->property(QStringLiteral(\"order\"))) n.props.insert(QStringLiteral(\"order\"), QStringLiteral(\"2\"));")],
  [(TG, "schema"), (TD, "physicsThroughThePanel")]),
 ("a 3D file's physics read without its order",
  [(FM, "                && !c.props.contains(QStringLiteral(\"order\")))\n                c.props.insert(QStringLiteral(\"order\"), QStringLiteral(\"1\"));",
    "                && !c.props.contains(QStringLiteral(\"order\")))\n                (void)0;")],
  [(TG, "schema")]),
 ("the plane's tensors offered in 3D",
  [(FM, "                a.choiceSpaces = {Spaces::All, Spaces::TwoD, Spaces::TwoD};", "                a.choiceSpaces = {Spaces::All, Spaces::All, Spaces::All};")],
  [(TD, "physicsThroughThePanel")]),
 ("a step's adaptive refinement shown in 3D",
  [(FM, "                p.spaces = Spaces::TwoD;\n", "")],
  [(TD, "physicsThroughThePanel")]),
 ("a new 3D model without a study",
  [(FM, "    // (A study at rest, as a 2D model's: its physics solved by Compute.)\n    Node study = m.make(QStringLiteral(\"study\"));\n    m.add(QStringLiteral(\"model\"), study);\n",
    "    // (A study at rest, as a 2D model's: its physics solved by Compute.)\n")],
  [(TG, "schema"), (TD, "aNewModel")]),
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
