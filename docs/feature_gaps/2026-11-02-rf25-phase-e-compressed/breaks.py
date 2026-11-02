#!/usr/bin/env python3
"""breaks.py: phase E's compressed solve undone on purpose, one piece at a
time - the cross approximation, its recompression and the H-matrix's
blocks; GMRES; the system's terms (L's two kernels, A^T P A, the vias');
the ports' Z taken variationally and GMRES's residual; the near field it
is preconditioned with; the automatic choice and the memory bound; the
oblique runs' cosine; and the mesh's, the tab's, the tools' and the
model's parts - each built and its tests run. Sources are put back
whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/person/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/person/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-phase-e-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

FA = "rf25/rf25_fast.cpp"
FW = "rf25/rf25_fullwave.cpp"
IN = "rf25/rf25_inductance.cpp"
ME = "rf25/rf25_mesh.cpp"
MO = "rf25/rf25_model.cpp"
DOC = "rf25doc.cpp"
QC = "qucscontrol_rf25.cpp"
FT, TF, TI, TM, TD, TC, TE = ("test_rf25_fast", "test_rf25_fullwave", "test_rf25_inductance", "test_rf25_mesh",
                              "test_rf25_doc", "test_qucs_control", "test_rf25_deembed")
CROSS = "aCrossApproximationIsItsBlockToTheTolerance"
HMAT = "anHMatrixMultipliesAsItsDenseMatrixDoes"
GMRES = "gmresSolvesToItsResidual"
DENSE = "aCompressedSolveIsTheDenseOne"
PAST = "pastTheDenseLimitItIsHeldCompressed"
CANNOT = "whatItCannotSolveIsSaid"
OBLIQUE = "anObliquePairIsTheCosineTimesItsAlignedOne"
MESHED = "aMeshForACompressedSolveHoldsMoreCells"
MATRIX = "aFullWaveStudysMatrixIsItsOwn"
TOOL = "anRf25ModelIsSolvedFullWaveThroughItsTool"
STRIPS = "stripsTooLargeAreCutAndSaid"

BREAKS = [
 # ---- the cross approximation and the H-matrix
 ("ACA's row without what is held already taken off",
  [(FA, "        for (std::size_t l = 0; l < us.size(); ++l) r -= us[l][row] * vs[l];", "        (void)row;")],
  [(FT, CROSS), (FT, HMAT), (TF, DENSE)]),

 ("ACA's column without what is held already taken off",
  [(FA, "        for (std::size_t l = 0; l < us.size(); ++l) c -= vs[l][pivot] * us[l];", "        (void)pivot;")],
  [(FT, CROSS), (FT, HMAT)]),

 ("ACA done at its first small outer product",
  [(FA, "            if (++small >= 2) break;", "            if (++small >= 1) break;")],
  [(FT, CROSS), (FT, HMAT), (TF, DENSE)]),

 ("recompressed to a hundred times the tolerance",
  [(FA, "    while (keep > 1 && tail + s[keep - 1] * s[keep - 1] <= tolerance * tolerance * total) {",
    "    while (keep > 1 && tail + s[keep - 1] * s[keep - 1] <= 1e4 * tolerance * tolerance * total) {")],
  [(FT, CROSS), (FT, HMAT)]),

 ("a symmetric matrix's lower blocks left out",
  [(FA, "                if (a_symmetric && b.row != b.column) backward();", "                (void)backward;")],
  [(FT, HMAT), (TF, DENSE)]),

 ("every pair of clusters low rank, the touching too",
  [(FA, "    if (apart > 0 && std::min(r.box.diameter(), c.box.diameter()) <= a_eta * apart) {",
    "    if (apart >= 0 && std::min(r.box.diameter(), c.box.diameter()) <= 1e9 * (a_eta * apart + 1)) {")],
  [(TF, DENSE)]),

 # ---- GMRES
 ("GMRES's update not preconditioned",
  [(FA, "        preconditioned(basis.leftCols(k) * y, z);", "        z = basis.leftCols(k) * y;")],
  [(FT, GMRES), (TF, DENSE)]),

 ("GMRES's rotations' sine not conjugated",
  [(FA, "                h(i + 1, k) = -std::conj(sn[std::size_t(i)]) * h(i, k) + cs[std::size_t(i)] * h(i + 1, k);",
    "                h(i + 1, k) = -sn[std::size_t(i)] * h(i, k) + cs[std::size_t(i)] * h(i + 1, k);")],
  [(TF, DENSE)]),

 # ---- the system's terms
 ("L's kernel along y left out of the system",
  [(FW, "    for (const bool alongX : {true, false}) {\n        const QList<int>& runs = alongX ? z.xRun : z.yRun;",
    "    for (const bool alongX : {true}) {\n        const QList<int>& runs = alongX ? z.xRun : z.yRun;")],
  [(TF, DENSE)]),

 ("an oblique run in L's kernel along x alone",
  [(FW, "        if (std::abs(c.dirY.at(a)) > 1e-9) {\n            z.yRun << a;",
    "        if (std::abs(c.dirY.at(a)) > 1e-9 && std::abs(c.dirX.at(a)) <= 1e-9) {\n            z.yRun << a;")],
  [(TF, DENSE)]),

 ("A^T P A without the vias' charges",
  [(FW, "    for (int a = 0; a < c.nr; ++a) {\n        if (c.tail.at(a) >= 0) q[c.tail.at(a)] += x[a];",
    "    for (int a = 0; a < c.planar; ++a) {\n        if (c.tail.at(a) >= 0) q[c.tail.at(a)] += x[a];")],
  [(TF, DENSE)]),

 ("a via's coupling to the runs one way round alone",
  [(FW, "    phi.multiplyTransposed(s, t);", "    (void)s;")],
  [(TF, DENSE)]),

 ("between two vias their potentials' halves each whole",
  [(FW, "        if (c.tail.at(via.run) >= 0) s[c.tail.at(via.run)] += 0.5 * x[via.run];\n        s[c.head.at(via.run)] -= 0.5 * x[via.run];",
    "        if (c.tail.at(via.run) >= 0) s[c.tail.at(via.run)] += x[via.run];\n        s[c.head.at(via.run)] -= x[via.run];")],
  [(TF, DENSE)]),

 # ---- Z, and GMRES's residual
 ("the ports' Z of the currents as they are, not variationally",
  [(FW, "    out.z = c.drive.transpose() * pd / jw - (rhs.transpose() * current + current.transpose() * rhs - current.transpose() * applied);",
    "    out.z = c.drive.transpose() * pd / jw - rhs.transpose() * current + 0.0 * applied.transpose() * current;")],
  [(TF, DENSE)]),

 ("GMRES to a residual of 1e-6",
  [(FW, "constexpr double kResidual = 1e-10;", "constexpr double kResidual = 1e-6;")],
  [(TF, DENSE)]),

 # ---- the near field it is preconditioned with
 ("no preconditioner",
  [(FW, "    const Operator precondition = [&lu](const VectorC& x, VectorC& y) { y = lu.solve(x); };",
    "    const Operator precondition = [&lu](const VectorC& x, VectorC& y) { (void)lu; y = x; };")],
  [(TF, DENSE)]),

 ("the near field's P its diagonal alone",
  [(FW, "    for (const QPair<int, int>& q : z.nearCells) {\n        const Complex v = k.potential(q.first, q.second);",
    "    for (const QPair<int, int>& q : z.nearCells) {\n        if (q.first != q.second) continue;\n        const Complex v = k.potential(q.first, q.second);")],
  [(TF, DENSE)]),

 ("the near field without a via's potential at the cells beside it",
  [(FW, "        for (const QPair<int, int>& q : z.nearViaCells) terms.emplace_back(q.second, q.first, k.viaPotential(q.first, q.second));",
    "        (void)0;")],
  [(TF, DENSE)]),

 # ---- the automatic choice and the memory bound
 ("dense past the limit unasked",
  [(FW, "                          || (asked == FullWaveControl::Matrix::Automatic && nr + nc > most);",
    "                          || (asked == FullWaveControl::Matrix::Automatic && false);")],
  [(TF, PAST), (TE, STRIPS)]),

 ("a dense solve past the limit set up when asked",
  [(FW, "    if (!compress && nr + nc > most) {", "    if (!compress && nr + nc > 4 * most) {")],
  [(TF, CANNOT), (TE, STRIPS)]),

 ("the memory a frequency would take not judged",
  [(FW, "        if (z.estimate > budget) {", "        if (false) {")],
  [(TF, CANNOT)]),

 # ---- the oblique runs' cosine
 ("an oblique pair's aligned inductance its partial one",
  [(IN, "    if (std::abs(std::abs(along) - 1) < 1e-12) return std::abs(partialInductance(a, b));",
    "    if (true) return std::abs(partialInductance(a, b));")],
  [(TI, OBLIQUE), (TF, DENSE)]),

 ("the dense solve's oblique pairs as they were",
  [(FW, "                c.lDirect(a, b) = c.lDirect(b, a) = alignedInductance(c.runs.at(a), c.runs.at(b));",
    "                c.lDirect(a, b) = c.lDirect(b, a) = std::abs(partialInductance(c.runs.at(a), c.runs.at(b)));")],
  [(TF, DENSE)]),

 # ---- the mesh
 ("a compressed solve's mesh held to the charge solve's 8000",
  [(ME, "    return most > 0 ? most : (compressed ? kMostCompressedCells : kMostCharges);",
    "    return most > 0 ? most : kMostCharges;")],
  [(TM, MESHED)]),

 ("a compressed solve's mesh's cost said dense",
  [(ME, "    mesh.cost = how.compressed ? costOf(unknowns + charges, true) : costOf(unknowns);", "    mesh.cost = costOf(unknowns);")],
  [(TM, MESHED), (TD, MATRIX)]),

 ("the mesh's Most cells not read",
  [(ME, "    if (most.isDouble()) how.most = most.toInt();", "    if (false) how.most = most.toInt();")],
  [(TM, MESHED)]),

 # ---- the tab
 ("the study's Matrix not handed to the solve",
  [(DOC, "            control.matrix = job->matrix;", "            (void)job->matrix;")],
  [(TD, MATRIX)]),

 ("a full-wave study's mesh cut for the charge solve",
  [(DOC, "        how.compressed = meshedCompressed(job->model);", "        how.compressed = false;")],
  [(TD, MATRIX)]),

 ("the study's Matrix read as automatic whatever it says",
  [(DOC, "    if (matrix == QLatin1String(\"dense\")) return FullWaveControl::Matrix::Dense;",
    "    if (false) return FullWaveControl::Matrix::Dense;")],
  [(TD, MATRIX)]),

 # ---- the tools
 ("rf25_solve's 'matrix' not read",
  [(QC, "    if (args.contains(QLatin1String(\"matrix\"))) {\n        request.matrix",
    "    if (false) {\n        request.matrix")],
  [(TC, TOOL)]),

 ("the answer without what the compressed matrix kept",
  [(QC, "    if (w.compressed) {\n        matrix.insert(QStringLiteral(\"tolerance\"), w.tolerance);",
    "    if (false) {\n        matrix.insert(QStringLiteral(\"tolerance\"), w.tolerance);")],
  [(TC, TOOL)]),

 ("'matrix' let through for the quasi-static solve",
  [(QC, "        if (!fullWave) return errorResult(tr(\"'matrix' is the full-wave solve's: the quasi-static one is dense.\"));",
    "        (void)fullWave;")],
  [(TC, TOOL)]),

 # ---- the model
 ("the study's Matrix shown for a quasi-static study too",
  [(MO, "                  \"kind=full_wave\")},", "                  \"\")},")],
  [(TD, MATRIX)]),
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
                                                    ASAN_OPTIONS="detect_leaks=0"))
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
