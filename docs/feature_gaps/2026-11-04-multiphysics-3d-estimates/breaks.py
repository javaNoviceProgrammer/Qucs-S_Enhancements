#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's estimates and memory limit (4 November) undone
on purpose, one piece at a time: the nonzeros' count, the spaces by order, a
study's peak, the fits, the limit and force, a solver for one solve, the
iterative solver offered, Build Mesh's estimate, the job's limit and force,
the limit's default and its Settings, fem_build's estimates, a refused solve
answered with an earlier one, a 2D model's fem_solve. Each built and its tests
run; sources put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/multiphysics3d-estimate-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

EST, SOL = "multiphysics/fem3_estimate.cpp", "multiphysics/fem3_solver.cpp"
DOC, MENU, FEM = "multiphysics3ddoc.cpp", "qucs_multiphysics3d.cpp", "qucscontrol_fem.cpp"
E, L = ("test_fem3_solve", "estimates"), ("test_multiphysics3d_doc", "estimatesAndTheLimit")

BREAKS = [
 # ---- the count
 ("each prism's pairs counted, not each row's once",
  [(EST, "        total += std::unique(row.begin(), row.end()) - row.begin();", "        total += qint64(row.size());")],
  [E]),
 ("a row's own degree of freedom not counted",
  [(EST, "        total += std::unique(row.begin(), row.end()) - row.begin();", "        total += std::unique(row.begin(), row.end()) - row.begin() - 1;")],
  [E]),
 ("each thread's chunk short of its last row",
  [(EST, "        pool.emplace_back([&part, &count, t, threads, n] { part[std::size_t(t)] = count(int(qint64(n) * t / threads), int(qint64(n) * (t + 1) / threads)); });",
    "        pool.emplace_back([&part, &count, t, threads, n] { part[std::size_t(t)] = count(int(qint64(n) * t / threads), int(qint64(n) * (t + 1) / threads) - 1); });")],
  [E]),
 ("one space for every order",
  [(EST, "        const int order = p->text(QStringLiteral(\"order\")) == QLatin1String(\"2\") ? 2 : 1;\n        if (!spaces[order]) {",
    "        const int order = 1;\n        if (!spaces[order]) {")],
  [E]),
 ("a study's peak its physics' sum",
  [(EST, "    for (const Estimate3& e : estimates) most = std::max(most, e.bytes());", "    for (const Estimate3& e : estimates) most += e.bytes();")],
  [E]),
 # ---- the fits
 ("multigrid's memory a nonzero halved",
  [(EST, "    return 85.0 * nonzeros;", "    return 42.5 * nonzeros;")],
  [E]),
 ("the Cholesky's factor of its rows alone, not their nonzeros",
  [(EST, "    return 74.3342 * std::pow(unknowns, 1.2483) * std::pow(nonzeros / unknowns, 0.4802);", "    return 74.3342 * std::pow(unknowns, 1.2483) * std::pow(20.0, 0.4802);")],
  [E]),
 # ---- the limit
 ("the limit not consulted",
  [(SOL, "        if (const QString past = pastTheLimit(out.estimates, asked.limit); !past.isEmpty()) {",
    "        if (const QString past = pastTheLimit(out.estimates, 0); !past.isEmpty()) {")],
  [E, L]),
 ("forced, refused all the same",
  [(SOL, "            if (!asked.force) {\n                out.error = past;", "            if (true) {\n                out.error = past;")],
  [E, L]),
 ("a solver for this solve alone not taken",
  [(SOL, "        if (!asked.solver.isEmpty()) o.solver = asked.solver;\n", "")],
  [E, L]),
 ("the iterative solver not offered where it would fit",
  [(EST, "    if (iterative < peak && iterative <= limit) ways <<", "    if (false) ways <<")],
  [E]),
 ("past the limit when under it",
  [(EST, "    if (peak <= limit) return {};", "    if (peak > limit) return {};")],
  [E, L]),
 # ---- the tab and the tools
 ("Build Mesh's estimate not said",
  [(DOC, "                    for (const Estimate3& e : a_estimates) say(tr(\"Estimated: %1\").arg(estimateText(e)));", "")],
  [L]),
 ("the limit not given to the job",
  [(DOC, "    job->asked.limit = memoryLimit();\n", "")],
  [L]),
 ("force not given to the job",
  [(DOC, "    a_asked.force = force;\n", "    a_asked.force = !force && force;\n")],
  [L]),
 ("the limit's default all of the memory",
  [(DOC, "    return physicalMemory() / 2;\n}", "    return physicalMemory();\n}")],
  [L]),
 ("the Settings' OK not kept",
  [(MENU, "    Multiphysics3DDoc::setMemoryLimitGB(limit->value());\n", "")],
  [L]),
 ("fem_build's mesh without its estimates",
  [(FEM, "        estimatesInto(o, d->estimates());\n        done(jsonResult(o));", "        done(jsonResult(o));")],
  [L]),
 ("a refused solve answered with the solution before it",
  [(FEM, "            if (!lines.isEmpty()) return done(errorResult(lines.join(QLatin1Char('\\n'))));\n            QJsonObject r = fem3Solved(d, args);",
    "            QJsonObject r = fem3Solved(d, args);")],
  [L]),
 ("a 2D model's fem_solve taking solver and force",
  [(FEM, "    for (const char* key : {\"solver\", \"force\"})\n        if (args.contains(QLatin1String(key)))",
    "    for (const char* key : {\"solver\", \"force\"})\n        if (false && args.contains(QLatin1String(key)))")],
  [L]),
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
