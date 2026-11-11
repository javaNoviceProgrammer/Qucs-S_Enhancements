#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's solid mechanics (10 November) undone on
purpose, one piece at a time: the strain's columns, the isotropic stiffness,
the thermal strain, a face's normal, a roller, the reactions, a crystal's turn
into 3D's axes, a vector field's components, the body load, a held value, the
eigenvalues' shift and the mass, the piezoelectric stress and polarization,
the schema's order and z components. Each built and its suite run through
ctest; sources put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build-rel/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build-rel")
RUNS = os.environ.get("HUNT_RUNS", os.path.join(ROOT, "..", "multiphysics3d-solid-breaks"))
os.makedirs(RUNS, exist_ok=True)

SD, SOL, SC, RES, MOD = ("multiphysics/fem3_solid.cpp", "multiphysics/fem3_solver.cpp", "multiphysics/fem3_scalar.cpp",
                         "multiphysics/fem3_results.cpp", "multiphysics/fem_model.cpp")
S = "test_fem3_solid"

BREAKS = [
 ("the strain's yz column of v its y gradient",
  [(SD, "    B[3][1] = g[2];", "    B[3][1] = g[1];")], [S]),
 ("the isotropic λ without its (1 + ν)",
  [(SD, "const double lambda = e * n / ((1 + n) * (1 - 2 * n)), mu = e / (2 * (1 + n));",
    "const double lambda = e * n / (1 - 2 * n), mu = e / (2 * (1 + n));")], [S]),
 ("the thermal strain's sign turned",
  [(SD, "S0[k] = std::isfinite(a) && std::isfinite(t) ? (k < 3 ? 1.0 : 2.0) * a * t : 0.0;",
    "S0[k] = std::isfinite(a) && std::isfinite(t) ? -(k < 3 ? 1.0 : 2.0) * a * t : 0.0;")], [S]),
 ("a wall's normal inward on its long edge",
  [(SD, "        n[0] = gxi[0] + geta[0];\n        n[1] = gxi[1] + geta[1];", "        n[0] = -gxi[0] - geta[0];\n        n[1] = -gxi[1] - geta[1];")], [S]),
 ("a roller across an axis holding nothing",
  [(SD, "            fix[axis] = true;\n", "")], [S]),
 ("the reactions with the loads added, not taken",
  [(SD, "        const Eigen::VectorXd r = a.R * x + a.Rd * ud - a.Fd;", "        const Eigen::VectorXd r = a.R * x + a.Rd * ud + a.Fd;")], [S]),
 ("a crystal's turn the plane's, not 3D's",
  [(SD, "    static const double M[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};", "    static const double M[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};")], [S]),
 ("a vector field's components all its first at a point",
  [(SOL, "    const std::size_t offset = std::size_t(part) * std::size_t(sp.size());\n    u = 0;", "    const std::size_t offset = 0;\n    u = 0;")], [S]),
 ("a vector field's components all its first at the nodes",
  [(RES, "        const std::size_t offset = field.space ? std::size_t(part) * std::size_t(field.space->size()) : 0;", "        const std::size_t offset = 0;")], [S]),
 ("the body load's z left out",
  [(SD, "                    for (int a = 0; a < 3; ++a) {\n                        fb[a] = Material::at(&ev, bf[std::size_t(a)]);",
    "                    for (int a = 0; a < 3; ++a) {\n                        fb[a] = a == 2 ? 0.0 : Material::at(&ev, bf[std::size_t(a)]);")], [S]),
 ("a held value's components all its x's",
  [(SD, "        ud(Eigen::Index(k)) = face >= 0 ? evalAt(a_constraints[std::size_t(face)].value[a], sp.point(dof)) : 0.0;",
    "        ud(Eigen::Index(k)) = face >= 0 ? evalAt(a_constraints[std::size_t(face)].value[0], sp.point(dof)) : 0.0;")], [S]),
 ("the eigenvalue's shift taken the wrong way",
  [(SD, "        found.push_back({sigma + (1.0 / theta).real(), k});", "        found.push_back({sigma - (1.0 / theta).real(), k});")], [S]),
 ("the mass without its density",
  [(SD, "                const double w = qp[3] * std::abs(mp.det) * (std::isfinite(r) ? r : 0);", "                const double w = qp[3] * std::abs(mp.det) * (std::isfinite(r) ? 1000 : 0);")], [S]),
 ("the piezoelectric stress added, not taken",
  [(SD, "        out.stress[I] = v - SP[I];", "        out.stress[I] = v + SP[I];"),
   (SD, "                    sigma0[I] = SP[I];", "                    sigma0[I] = -SP[I];")], [S]),
 ("the polarization's term with its sign turned",
  [(SC, "                        Fe[std::size_t(i)] += w * (P[0] * gi[0] + P[1] * gi[1] + P[2] * gi[2]);",
    "                        Fe[std::size_t(i)] -= w * (P[0] * gi[0] + P[1] * gi[1] + P[2] * gi[2]);")], [S]),
 ("multigrid without the rigid-body modes",
  [(SD, "        sys.setNearNullspace(6, std::move(B), blocks ? 3 : 1);", "        (void)blocks;")], [S]),
 ("a 3D solid made of order 1",
  [(MOD, "n.props.insert(QStringLiteral(\"order\"), k->type == QLatin1String(\"solid\") ? QStringLiteral(\"2\") : QStringLiteral(\"1\"));",
    "n.props.insert(QStringLiteral(\"order\"), QStringLiteral(\"1\"));")], [S]),
 ("a displacement's w0 shown in 2D too",
  [(MOD, "          inSpaces(shownIf(spatial(QStringLiteral(\"w0\"), tr(\"Displacement w0\"), QStringLiteral(\"m\"), QStringLiteral(\"0[m]\")), QStringLiteral(\"z=true\")),\n                   Spaces::ThreeD)},",
    "          shownIf(spatial(QStringLiteral(\"w0\"), tr(\"Displacement w0\"), QStringLiteral(\"m\"), QStringLiteral(\"0[m]\")), QStringLiteral(\"z=true\"))},")], [S]),
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
    for i, (name, edits, suites) in enumerate(BREAKS, 1):
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
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + ["qucs/tests/" + s for s in suites], stdout=out,
                                  stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite in suites:
                    try:
                        r = subprocess.run(["ctest", "--test-dir", B, "-R", "^%s$" % suite, "--output-on-failure"], stdout=out,
                                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=1800)
                        said.append("%s %s" % (suite, "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % suite)
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
