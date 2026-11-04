#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's exports and its functions of three arguments (4
November) undone on purpose, one piece at a time: a wedge's floor, a quadratic
wedge's middles and their values, a collection's names and times, a grid's z
and its columns' blank lines, the trilinear table's axes, a table that is not
a whole grid, the Export button. Each built and its tests run; sources put
back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/multiphysics3d-export-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

EXP, EXPR, MODEL, PANEL = "multiphysics/fem3_export.cpp", "multiphysics/fem_expr.cpp", "multiphysics/fem_model.cpp", "multiphysics3dpanel.cpp"
TS, TD = "test_fem3_solve", "test_multiphysics3d_doc"

BREAKS = [
 # ---- VTU
 ("a wedge's floor left counter-clockwise",
  [(EXP, "    if (turn > 0) return {c[0], c[2], c[1], c[3], c[5], c[4]};", "    if (false && turn > 0) return {c[0], c[2], c[1], c[3], c[5], c[4]};")],
  [(TS, "exports")]),
 ("a quadratic wedge's middles at their prism's first corner",
  [(EXP, "                if (sp) x = sp->map(p, xi, eta, zeta).x;", "                if (sp) x = sp->map(p, 0, 0, 0).x;")],
  [(TS, "exports")]),
 ("a quadratic wedge's middles' values read at the prism's first corner",
  [(EXP, "                ev.at(p, xi, eta, zeta, x);", "                ev.at(p, 0, 0, 0, x);")],
  [(TS, "exports")]),
 ("a quadratic wedge's middles in metres",
  [(EXP, "                points.push_back(Vec3{x.x / m.unitScale, x.y / m.unitScale, x.z / m.unitScale});", "                points.push_back(x);")],
  [(TS, "exports")]),
 ("a collection's parts named without its base",
  [(EXP, "               + \"\\\" file=\\\"\" + (base + end).toHtmlEscaped().toUtf8() + \"\\\"/>\\n\";", "               + \"\\\" file=\\\"\" + end.toHtmlEscaped().toUtf8() + \"\\\"/>\\n\";")],
  [(TS, "exports")]),
 ("a collection's times each part's number",
  [(EXP, "        const double step = at[i].snapshot >= 0 ? at[i].time : (swept ? double(at[i].point) : double(i));", "        const double step = double(i);")],
  [(TS, "exports")]),
 # ---- Field on a Grid
 ("a grid's z range not read",
  [(EXP, "        const QString texts[6] = {from.value(0), from.value(1), z.value(0), to.value(0), to.value(1), z.value(1)};",
    "        const QString texts[6] = {from.value(0), from.value(1), QString(), to.value(0), to.value(1), QString()};")],
  [(TS, "exports")]),
 ("a grid's columns without a blank line after each plane of z",
  [(EXP, "(n + 1) % (std::size_t(nx) * std::size_t(ny)) == 0 ? \"\\n\\n\" : \"\\n\";", "\"\\n\";")],
  [(TS, "exports")]),
 ("a NumPy grid's shape without z",
  [(EXP, "            shape.insert(shape.end(), {K, std::size_t(nz), std::size_t(ny), std::size_t(nx)});", "            shape.insert(shape.end(), {K * std::size_t(nz), std::size_t(ny), std::size_t(nx)});")],
  [(TS, "exports")]),
 # ---- functions of three arguments
 ("the trilinear table's y and z swapped",
  [(EXPR, "    locate(grid, v, &j, &s);\n    locate(grid3, w, &l, &t);\n    const std::size_t i1", "    locate(grid, w, &j, &s);\n    locate(grid3, v, &l, &t);\n    const std::size_t i1")],
  [(TS, "interpolationOfThree")]),
 ("the trilinear table's third axis its cell's floor alone",
  [(EXPR, "    const std::size_t i1 = std::min(i + 1, nx - 1), j1 = std::min(j + 1, ny - 1), l1 = std::min(l + 1, nz - 1);",
    "    const std::size_t i1 = std::min(i + 1, nx - 1), j1 = std::min(j + 1, ny - 1), l1 = l;")],
  [(TS, "interpolationOfThree")]),
 ("a table of three arguments not a whole grid taken",
  [(MODEL, "            if (table->x.empty() || at.size() != axis[0].size() * axis[1].size() * axis[2].size()) {",
    "            if (table->x.empty()) {")],
  [(TS, "interpolationOfThree")]),
 # ---- the tab
 ("the panel's Export button left out",
  [(PANEL, "    } else if (k && k->group == QLatin1String(\"export\")) {", "    } else if (k && k->group == QLatin1String(\"exportx\")) {")],
  [(TD, "exportsInTheTab")]),
 ("a 3D model's Export not offering VTU",
  [(MODEL, "        if (kind.type == QLatin1String(\"vtu\")) kind.spaces = Spaces::ThreeD;", "        if (kind.type == QLatin1String(\"vtu\")) kind.spaces = Spaces::TwoD;")],
  [(TD, "exportsInTheTab")]),
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
