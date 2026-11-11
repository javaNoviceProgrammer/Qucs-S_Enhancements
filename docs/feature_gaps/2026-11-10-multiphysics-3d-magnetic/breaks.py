#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's magnetic fields (10 November) undone on
purpose, one piece at a time: the curl-curl term, the gauge's rows, the
source, a magnet's Br, the B-H curve's tangent, insulation on inner faces, a
potential's projection, a coil's scale, the inductance sweep, the Lorentz
force, the energy, the edge elements' order in the schema. Each built and its
suite run through ctest; sources put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build-rel/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build-rel")
RUNS = os.environ.get("HUNT_RUNS", os.path.join(ROOT, "..", "multiphysics3d-magnetic-breaks"))
os.makedirs(RUNS, exist_ok=True)

MG, MOD, MAG2 = "multiphysics/fem3_magnetic.cpp", "multiphysics/fem_model.cpp", "multiphysics/fem_magnetic.cpp"
S = "test_fem3_magnetic"

BREAKS = [
 ("the curl-curl term's tangent the identity's",
  [(MG, "                for (int j = 0; j < nb; ++j) Ke[std::size_t(i * nb + j)] += w * (b.c[j][0] * tc[0] + b.c[j][1] * tc[1] + b.c[j][2] * tc[2]);",
    "                for (int j = 0; j < nb; ++j) Ke[std::size_t(i * nb + j)] += w * (b.c[j][0] * b.c[i][0] + b.c[j][1] * b.c[i][1] + b.c[j][2] * b.c[i][2]) / 1.25663706127e-6;")], [S]),
 ("the gauge's z gradient left out",
  [(MG, "        g[k][2] = s.dzeta[k] / h;", "        g[k][2] = 0;")], [S]),
 ("the source's z component left out",
  [(MG, "                Fe[std::size_t(i)] += w * (Jv[0] * b.v[i][0] + Jv[1] * b.v[i][1] + Jv[2] * b.v[i][2] - (H[0] * b.c[i][0] + H[1] * b.c[i][1] + H[2] * b.c[i][2]));",
    "                Fe[std::size_t(i)] += w * (Jv[0] * b.v[i][0] + Jv[1] * b.v[i][1] - (H[0] * b.c[i][0] + H[1] * b.c[i][1] + H[2] * b.c[i][2]));")], [S]),
 ("a magnet's Br added to B, not taken",
  [(MG, "                    for (int j = 0; j < 3; ++j) H[i] += nu[3 * i + j] * (B[j] - Br[j]);", "                    for (int j = 0; j < 3; ++j) H[i] += nu[3 * i + j] * (B[j] + Br[j]);")], [S]),
 ("the B-H curve's tangent its secant alone",
  [(MG, "                        Tg[3 * i + j] = (i == j ? nub : 0.0) + (bn > 0 ? (slope - nub) * B[i] * B[j] / (bn * bn) : 0.0);",
    "                        Tg[3 * i + j] = (i == j ? nub : 0.0) + 0 * slope;")], [S]),
 ("insulation on the faces between its domains too",
  [(MG, "        if (c.kind == FaceCondition::Insulation && in(tf.back) && in(tf.front)) continue;\n", "")], [S]),
 ("a potential's projection n × A0 its sign turned",
  [(MG, "                    rhs(li) += fp.weight * (ni[0] * na0[0] + ni[1] * na0[1] + ni[2] * na0[2]);", "                    rhs(li) -= fp.weight * (ni[0] * na0[0] + ni[1] * na0[1] + ni[2] * na0[2]);")], [S]),
 ("a coil's scale without its turns' current",
  [(MG, "        c.scale = c.perAmp * i0;", "        c.scale = c.perAmp;")], [S]),
 ("the inductance sweep's coils all the first",
  [(MG, "        for (int c = 0; c < nc; ++c) unit[std::size_t(c)] = sourcesOf(c);", "        for (int c = 0; c < nc; ++c) unit[std::size_t(c)] = sourcesOf(0);")], [S]),
 ("the Lorentz force's y component J's z times B's x the wrong way",
  [(MG, "                coil.force[1] += w * (j[2] * st.B[0] - j[0] * st.B[2]);", "                coil.force[1] += w * (j[0] * st.B[2] - j[2] * st.B[0]);")], [S]),
 ("the energy density without its half",
  [(MG, "        out.energy += 0.5 * out.H[i] * (out.B[i] - Br[i]);", "        out.energy += out.H[i] * (out.B[i] - Br[i]);")], [S]),
 ("the External Current Density's Jez shown in 2D too",
  [(MOD, "          inSpaces(spatial(QStringLiteral(\"Jez\"), tr(\"Jez\"), QStringLiteral(\"A/m^2\"), QStringLiteral(\"0[A/m^2]\")), Spaces::ThreeD)},",
    "          spatial(QStringLiteral(\"Jez\"), tr(\"Jez\"), QStringLiteral(\"A/m^2\"), QStringLiteral(\"0[A/m^2]\"))},")], [S]),
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
