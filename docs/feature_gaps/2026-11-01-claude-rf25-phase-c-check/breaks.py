#!/usr/bin/env python3
"""breaks.py: the phase C check's fixes undone on purpose, one at a time -
a conductor cut into the rectangles it is made of and their lines shared
where they meet (F1), touching shapes one net, no line of what is no
uniform line (F2), and the coplanar edge port (F6) - each built and its
tests run. Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-phase-c-check-breaks")
os.makedirs(RUNS, exist_ok=True)

MESH = "rf25/rf25_mesh.cpp"
STATIC = "rf25/rf25_static.cpp"
SP = "rf25/rf25_sparams.cpp"
FW = "rf25/rf25_fullwave.cpp"
D = "rf25doc.cpp"
TM, TS, TSP, TF, TD, TC = "test_rf25_mesh", "test_rf25_static", "test_rf25_sparams", "test_rf25_fullwave", "test_rf25_doc", "test_qucs_control"
TEE = "aTeeIsItsStripsMeetingEdgeToEdge"
CELLS = "aConductorHasCellsAcrossIt"
NETS = "touchingShapesAreOneNet"
NOLINE = "whatIsNoUniformLineIsSaidToBe"
LIMIT = "theQuasiStaticLimitIsSaidRatherThanAssumed"
CPW = "aCoplanarPortReturnsThroughItsGrounds"
CPWTAB = "aCoplanarPortIsReadAndSaysWhatItCannotHave"
PADS = "whatIsNoLineHasNoSParameters"
TOOL = "anRf25ModelIsSolvedFullWaveThroughItsTool"

BREAKS = [
 # ---- F1: a conductor cut into its rectangles, meeting edge to edge
 ("no conductor cut into the rectangles it is made of",
  [(MESH, "    if (std::abs(sum - areaOf(outline)) > 1e-6 * std::max(1e-30, areaOf(outline))) return {};\n    return chosen;",
    "    (void)sum;\n    return {};")],
  [(TM, TEE), (TM, CELLS)]),

 ("the slabs that give more rectangles chosen",
  [(MESH, "    const QList<QRectF>& chosen = byY.size() < byX.size() ||", "    const QList<QRectF>& chosen = byY.size() > byX.size() ||")],
  [(TM, TEE)]),

 ("where two rectangles meet, their lines not shared",
  [(MESH, "                gained = (m.alongX ? shareLines(a.xs, b.xs, m.lo, m.hi, tol) : shareLines(a.ys, b.ys, m.lo, m.hi, tol)) || gained;",
    "                (void)a;\n                (void)b;")],
  [(TM, TEE)]),

 ("a joint graded as though it were an edge",
  [(MESH, "            for (int k = 0; k < 4; ++k) pieces[a].grade[k] = met.at(a).at(k) < side[k] - tol;",
    "            for (int k = 0; k < 4; ++k) pieces[a].grade[k] = side[k] > 0 || met.at(a).at(k) >= 0;")],
  [(TM, TEE)]),

 # ---- touching shapes one net
 ("touching shapes of one metal left two nets",
  [(STATIC, "            join(a, b);\n        }\n    }\n    // The nets, in the order their first conductor appears.",
    "            (void)0;\n        }\n    }\n    // The nets, in the order their first conductor appears.")],
  [(TS, NETS)]),

 # ---- F2: no line of what is no uniform line
 ("what is no uniform line said to have values per metre",
  [(STATIC, "so there are no values per metre: its capacitance stands whole, and its ports' S are \"\n                         \"the full-wave solve's.\");",
    "so its per-metre values are the whole over \"\n                         \"its length.\");")],
  [(TS, NOLINE)]),

 ("the limit of what is no line set by its width",
  [(SP, "        wide = (line ? std::min(box.width(), box.height()) : std::max(box.width(), box.height())) * micrometre;",
    "        wide = std::min(box.width(), box.height()) * micrometre;")],
  [(TSP, LIMIT), (TD, PADS)]),

 ("modes per metre of what is no uniform line",
  [(SP, "    if (!extraction.perMetre.fromMiddle) {\n        out.error = tr(\"it is no uniform line",
    "    if (false) {\n        out.error = tr(\"it is no uniform line")],
  [(TD, PADS)]),

 ("the tab's S parameters of what is no line, unsaid",
  [(D, "    if (!a_extraction->perMetre.fromMiddle) {\n        out << tr(\"No uniform line, so no S parameters as one",
    "    if (false) {\n        out << tr(\"No uniform line, so no S parameters as one")],
  [(TD, PADS)]),

 # ---- F6: the coplanar edge port
 ("an edge port's Return not read",
  [(D, "            p.coplanar = textOf(port, QStringLiteral(\"return\")) == QLatin1String(\"beside\");", "            p.coplanar = false;")],
  [(TD, CPWTAB), (TC, TOOL)]),

 ("the conductor beside a port on one side not found",
  [(FW, "        if (t1 <= lo + tol) below << std::make_tuple(lo - t1, lo - t0, c);", "        if (false) below << std::make_tuple(lo - t1, lo - t0, c);")],
  [(TF, CPW)]),

 ("a coplanar column without its ground's share",
  [(FW, "            for (int k = 0; k < ground.size(); ++k) column(nodeOf.value(ground.at(k))) -= back.at(k);", "            (void)back;")],
  [(TF, CPW)]),

 ("a coplanar port's currents left in its columns",
  [(FW, "            a.current = a.current * (x * z);\n", "")],
  [(TF, CPW)]),

 ("a coplanar port's grounds not tied: its first column alone",
  [(FW, "    for (int k = 0; k < c.np; ++k) ofPort(k, c.portOf.at(k)) = 1;",
    "    for (int k = 0; k < c.np; ++k) ofPort(k, c.portOf.at(k)) = (k == 0 || c.portOf.at(k) != c.portOf.at(k - 1)) ? 1 : 0;")],
  [(TF, CPW)]),

 ("the solve's lines silent on a coplanar port's return",
  [(FW, "    out.lines << (coplanar ? tr(", "    out.lines << (false ? tr(")],
  [(TF, CPW)]),

 ("a coplanar port calibrated as though its feed were a strip",
  [(D, "                if (port.coplanar) {\n                    job->notes << tr(\"%1 returns through the conductors beside it, and strips of a coplanar feed",
    "                if (false) {\n                    job->notes << tr(\"%1 returns through the conductors beside it, and strips of a coplanar feed")],
  [(TD, CPWTAB)]),
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
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen",
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
