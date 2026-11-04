#!/usr/bin/env python3
"""breaks.py: the round of 4 November - CI's reds fixed, decision 8's devices
held to what is published, multigrid in any unit and with a symmetric smoother
- undone on purpose, one piece at a time: send_input's points mapped as its
events go, an export's Write said again when its form is made again, the
right-hand side scaled to a norm of one, the smoother, a right-hand side not
finite, a terminal on a face between two solved domains, order 2's curved
floors, and the device examples. Each built and its tests run; sources and
examples put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/multiphysics3d-devices-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

INPUT, PANEL = "qucscontrol_input.cpp", "rf25panel.cpp"
AMG, SC, FE = "multiphysics/fem_amg.cpp", "multiphysics/fem3_scalar.cpp", "multiphysics/fem3_fe.cpp"
EX = "../examples/multiphysics3d/"
TPP, TRF, TAMG, TDEV = "test_pptx_doc", "test_rf25_doc", "test_fem_amg", "test_fem3_devices"

BREAKS = [
 # ---- CI's reds
 ("send_input: a view's points as mapped before its events go",
  [(INPUT, "            const QList<QPoint> local = mapped ? mapped() : asGiven;", "            const QList<QPoint> local = asGiven;")],
  [(TPP, "theRawInputDrawsOnASlide")]),
 ("an export's Write: what it said lost when its form is made again",
  [(PANEL, "        said->setText(a_exportSaid);\n        said->setVisible(!a_exportSaid.isEmpty());", "        said->hide();")],
  [(TRF, "theSolvedModelIsWrittenAsATouchstone")]),
 # ---- multigrid
 ("multigrid: the right-hand side not brought to a norm of one",
  [(AMG, "    const double norm = b.norm();", "    const double norm = b.norm() > 0 ? 1.0 : 0.0;")],
  [(TAMG, "solvesAtAnyScale"), (TDEV, "photodiodeFingers")]),
 ("multigrid's smoother ILU(0) again",
  [(AMG, '        return v == "spai0" ? 0 : v == "ilu0" ? 2 : 1;', '        return v == "spai0" ? 0 : v == "gauss_seidel" ? 1 : 2;')],
  [(TDEV, "vcselArray")]),
 ("multigrid: a right-hand side not finite taken",
  [(AMG, "    if (n == 0 || norm == 0 || !std::isfinite(norm)) {", "    if (n == 0 || norm == 0) {")],
  [(TAMG, "fallsBack")]),
 # ---- what the devices lean on
 ("a terminal on a face between two solved domains not held",
  [(SC, "            for (int b : picked)\n                if (b >= 0 && b < int(F)) a_conditions[std::size_t(b)] = c;",
    "            for (int b : picked)\n                if (b >= 0 && b < int(F) && !(topo.faces[std::size_t(b)].back >= 0 && topo.faces[std::size_t(b)].front >= 0\n"
    "                                               && f.domains[std::size_t(topo.faces[std::size_t(b)].back)] && f.domains[std::size_t(topo.faces[std::size_t(b)].front)]))\n"
    "                    a_conditions[std::size_t(b)] = c;")],
  [(TDEV, "photodiodeFingers")]),
 ("order 2's floors weighed as straight triangles",
  [(FE, "            fp.weight = q[2] * std::abs(p.det);",
    "            const auto c0 = a_plan->map(face.plan, 0, 0).x, c1 = a_plan->map(face.plan, 1, 0).x, c2 = a_plan->map(face.plan, 0, 1).x;\n"
    "            fp.weight = q[2] * std::abs((c1.x() - c0.x()) * (c2.y() - c0.y()) - (c1.y() - c0.y()) * (c2.x() - c0.x()));")],
  [(TDEV, "vcselArray")]),
 # ---- the examples
 ("the TSV example's currents on every domain",
  [(EX + "tsv.qfem3d", '                    "order": "2",\n                    "selection": {"only": ["via"]},\n', '                    "order": "2",\n')],
  [(TDEV, "theExamples")]),
 ("the photodiode example's electrode on its comb's top",
  [(EX + "msm_photodiode.qfem3d", '"selection": {"of": ["combA"], "side": "bottom"}', '"selection": {"of": ["combA"], "side": "top"}')],
  [(TDEV, "theExamples")]),
 ("the VCSEL example's heat on the middle VCSEL alone",
  [(EX + "vcsel_array.qfem3d", '"selection": {"of": ["vcsel11", "vcsel12", "vcsel13", "vcsel21", "vcsel22", "vcsel23", "vcsel31", "vcsel32", "vcsel33"], "side": "top"}',
    '"selection": {"of": ["vcsel22"], "side": "top"}')],
  [(TDEV, "theExamples")]),
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
