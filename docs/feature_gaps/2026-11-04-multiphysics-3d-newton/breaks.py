#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's user's tensor, Newton's method and parallel
assembly (4 November) undone on purpose, one piece at a time: a tensor's keys,
its zz and cross terms, its diagonal's cross terms; Newton's derivatives of
the coefficient, the source and a film, its step, its matrix read by columns,
its solver; the parts' join, a part's last element, the faces' triplets. Each
built and its tests run; sources put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/multiphysics3d-newton-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

SC, SOL, AMG, MOD = "multiphysics/fem3_scalar.cpp", "multiphysics/fem3_solver.cpp", "multiphysics/fem_amg.cpp", "multiphysics/fem_model.cpp"
TS = "test_fem3_solve"
T, N, P, F = (TS, "usersTensor"), (TS, "newtonsMethod"), (TS, "parallelAssembly"), (TS, "filmAndRadiation")
D = ("test_multiphysics3d_doc", "physicsThroughThePanel")

BREAKS = [
 # ---- a user's tensor
 ("a user's tensor read by its isotropic key",
  [(SC, "                        keys[i][j] = key;\n                        t.c[i][j] = feat->text(key);", "                        keys[i][j] = key;\n                        t.c[i][j] = feat->text(property);")],
  [T]),
 ("its xz read as its xy",
  [(SC, "                        const QString key = tensorPrefix + QLatin1Char(axes[std::min(i, j)]) + QLatin1Char(axes[std::max(i, j)]);",
    "                        const QString key = tensorPrefix + QLatin1Char(axes[std::min(i, j)]) + QLatin1Char(axes[std::max(i, j) == 2 && std::min(i, j) == 0 ? 1 : std::max(i, j)]);")],
  [T]),
 ("its zz the plane's only",
  [(MOD, "            inThreeD(shownIf(spatial(key + QStringLiteral(\"zz\"), symbol + QStringLiteral(\", zz\"), unit, def), user)),", "")],
  [D]),
 ("a diagonal tensor's cross terms read too",
  [(SC, "                        if (i != j && !full) continue;\n", "")],
  [T]),
 # ---- Newton's method
 ("the coefficient's derivative left out",
  [(SC, "                    for (int k = 0; k < 9; ++k) dc[k] = c[std::size_t(k)].isConstant() ? 0 : (ev.eval(c[std::size_t(k)]) - c0[k]) / du;",
    "                    for (int k = 0; k < 9; ++k) dc[k] = 0 * c0[k];")],
  [N]),
 ("its derivative's term transposed",
  [(SC, "                            Je[std::size_t(i * per + j)] += w * (a_scale * flux * s.n[j] - (std::isfinite(dq) ? dq : 0) * s.n[i] * s.n[j]);",
    "                            Je[std::size_t(j * per + i)] += w * (a_scale * flux * s.n[j] - (std::isfinite(dq) ? dq : 0) * s.n[i] * s.n[j]);")],
  [N]),
 ("a film's derivative left out",
  [(SC, "                    const double d = ((h1 - h0) * u - (g1 - g0)) / du;", "                    const double d = 0 * ((h1 - h0) * u - (g1 - g0)) / du;")],
  [F]),
 ("Newton's step the residual's sign turned",
  [(SC, "            const Eigen::VectorXd r = b - a.A * x;", "            const Eigen::VectorXd r = a.A * x - b;")],
  [N, F]),
 ("Newton's matrix without its derivatives",
  [(SC, "            const Eigen::SparseMatrix<double> jm = a.A + a.J;", "            const Eigen::SparseMatrix<double> jm = a.A;")],
  [N]),
 ("multigrid reading Newton's matrix by its columns",
  [(AMG, "    if (d.nonsymmetric) {\n        rows = d.a;", "    if (false) {\n        rows = d.a;"),
   (AMG, "    const int* outer = d.nonsymmetric ? rows.outerIndexPtr() : d.a.outerIndexPtr();\n    const int* inner = d.nonsymmetric ? rows.innerIndexPtr() : d.a.innerIndexPtr();\n    const double* values = d.nonsymmetric ? rows.valuePtr() : d.a.valuePtr();",
    "    const int* outer = d.a.outerIndexPtr();\n    const int* inner = d.a.innerIndexPtr();\n    const double* values = d.a.valuePtr();")],
  [N]),
 ("Newton's matrix by conjugate gradients",
  [(SOL, "        a_amg->setNonsymmetric(true);\n", "")],
  [N]),
 ("Picard's iterations whatever is asked",
  [(SC, "    const bool newton = a_self && o.nonlinear != QLatin1String(\"picard\");\n    a_converged = !a_self;", "    const bool newton = false;\n    a_converged = !a_self;")],
  [N, F]),
 # ---- parallel assembly
 ("the parts' records joined from the last",
  [(SC, "    for (Part& p : parts) {\n        for (const auto& [i, v] : p.f) out.F(i) += v;", "    for (auto pi = parts.rbegin(); pi != parts.rend(); ++pi) {\n        Part& p = *pi;\n        for (const auto& [i, v] : p.f) out.F(i) += v;")],
  [P]),
 ("a part's last element left out",
  [(SC, "        const int from = int(qint64(E) * k / partCount), to = int(qint64(E) * (k + 1) / partCount);",
    "        const int from = int(qint64(E) * k / partCount), to = int(qint64(E) * (k + 1) / partCount) - (k + 1 < partCount ? 1 : 0);")],
  [P]),
 ("the faces' triplets left out of the matrix",
  [(SC, "        lists.push_back(&faces);\n", "")],
  [F]),
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
