#!/usr/bin/env python3
"""breaks.py: each piece of the vias undone on purpose: a via's kernels (the
integrals along whole slabs, G_zz's closed forms, the potential's lines,
sign and tail, the slabs a via must span), the solve (the potential taken
at the via's mean distance, its runs, its own ends and the ports, a via to
the ground's tail, where it stands, how its cells are grouped, its bar's
resistance, its drawing), the stack's via to the ground and phase B's
refusal of it, and the tab's drawing. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those
alone; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-vias-breaks")
os.makedirs(RUNS, exist_ok=True)

W = "rf25/rf25_wave.cpp"
F = "rf25/rf25_fullwave.cpp"
M = "rf25/rf25_model.cpp"
S = "rf25/rf25_static.cpp"
D = "rf25doc.cpp"
TW, TF, TM, TS, TD = "test_rf25_wave", "test_rf25_fullwave", "test_rf25_model", "test_rf25_static", "test_rf25_doc"
KERN = "aViasKernelsAreItsLinesIntegratedAlongIt"
FORMS = "theTwoFormsOfAViasCouplingAgree"
LEFT = "whatAViasClosedFormsLeaveIsSmallAndTabulated"
WHOLE = "aViaThatIsNotWholeSlabsIsSaid"
GROUND = "aViaToTheGroundIsItsOwnInductance"
BAR = "aViasResistanceIsItsBars"
METALS = "aViaBetweenTwoMetalsJoinsThem"
STACK = "aStackThatCannotBeSaysWhy"
REFUSED = "whatItCannotSolveIsRefused"
TAB = "aViaToTheGroundIsSolvedFullWaveAndDrawn"

BREAKS = [
 ("the slab's own thickness left out of Int Int I_v",
  [(W, "                const double same = n.slab == m.slab ? n.z1 - n.z0 : 0.0;\n                const Complex currents",
    "                const double same = 0.0;\n                const Complex currents")],
  [(TW, KERN)]),

 ("G_zz over the field's permittivity, not the source's",
  [(W, "                sum += kMu0 * currents / a_eps.at(m.slab);", "                sum += kMu0 * currents / a_eps.at(n.slab);")],
  [(TW, FORMS)]),

 ("the potential's TE line left out",
  [(W, "                   - (voltage(false, h, m.z1) - voltage(false, h, m.z0)) / jkzZh;", "                   ;")],
  [(TW, KERN)]),

 ("the potential's sign turned",
  [(W, "        full[c++] = -kJ * w * kMu0 / (kr * kr) * sum;", "        full[c++] = kJ * w * kMu0 / (kr * kr) * sum;")],
  [(TW, KERN), (TW, FORMS)]),

 ("a vertical current's image in a ground taken as the opposite current",
  [(W, "                    terms << Term{-fresnelDown(n.slab) * half, k, n.z0, n.z1, 2 * n.z0 - m.z1, 2 * n.z0 - m.z0};",
    "                    terms << Term{fresnelDown(n.slab) * half, k, n.z0, n.z1, 2 * n.z0 - m.z1, 2 * n.z0 - m.z0};")],
  [(TW, KERN), (TW, LEFT)]),

 ("its image in a dielectric face not taken out",
  [(W, "                    terms << Term{-fresnelUp(n.slab) * half, k, n.z0, n.z1, 2 * n.z1 - m.z1, 2 * n.z1 - m.z0};",
    "                    terms << Term{Complex(0, 0), k, n.z0, n.z1, 2 * n.z1 - m.z1, 2 * n.z1 - m.z0};")],
  [(TW, KERN), (TW, LEFT)]),

 ("the direct term carried across a face as it is",
  [(W, "                    terms << Term{carried * half, k, n.z0, n.z1, m.z0, m.z1};",
    "                    terms << Term{half, k, n.z0, n.z1, m.z0, m.z1};")],
  [(TW, LEFT)]),

 ("the potential's tail of the wrong sign",
  [(W, "            t.coefficient = -kJ * a_omega * kMu0 * fresnelUp(top.slab) / 2.0;",
    "            t.coefficient = kJ * a_omega * kMu0 * fresnelUp(top.slab) / 2.0;")],
  [(TW, KERN), (TW, LEFT)]),

 ("the second antiderivative's series a tenth out",
  [(W, "        return a * a * (0.5 - x * (1.0 / 6 - x * (1.0 / 24",
    "        return a * a * (0.45 - x * (1.0 / 6 - x * (1.0 / 24")],
  [(TW, LEFT)]),

 ("a via that ends half way up a slab taken",
  [(W, "            if (lo != slab.z0 || hi != slab.z1) return false;   // (a part of a slab)\n", "")],
  [(TW, WHOLE)]),

 ("a via that reaches beyond the stack taken",
  [(W, "        return !parts->isEmpty() && std::abs(covered - (s.z1 - s.z0)) <= 2 * hair * parts->size();",
    "        return !parts->isEmpty();")],
  [(TW, WHOLE)]),

 ("a via's potential taken at its axis, not its mean distance",
  [(F, "        return viaGreens.at(v.span).potential(c.hNode.at(cell), std::max(distance(c.centre.at(cell), v.at), v.gmd));",
    "        return viaGreens.at(v.span).potential(c.hNode.at(cell), distance(c.centre.at(cell), v.at));")],
  [(TF, GROUND)]),

 ("the ports' share of a via's potential left out",
  [(F, "                for (int j = 0; j < nv; ++j) portVia(q, j) += c.drive(i, q) * potentialOf(c.vias.at(j), i);",
    "                for (int j = 0; j < nv; ++j) (void)j;")],
  [(TF, GROUND)]),

 ("the ports' share in their voltage but not in the vias' drive",
  [(F, "        for (int q = 0; q < c.np; ++q) rhs(c.vias.at(j).run, q) -= portVia(q, j);", "        (void)j;")],
  [(TF, GROUND), (TF, METALS)]),

 ("the ports' share of the wrong sign",
  [(F, "                for (int j = 0; j < nv; ++j) portVia(q, j) += c.drive(i, q) * potentialOf(c.vias.at(j), i);",
    "                for (int j = 0; j < nv; ++j) portVia(q, j) -= c.drive(i, q) * potentialOf(c.vias.at(j), i);")],
  [(TF, GROUND)]),

 ("a run in the plane and a via not coupled",
  [(F, "            m(a, b.run) = m(b.run, a) = v;", "            (void)v;")],
  [(TF, GROUND)]),

 ("a run in the plane and a via coupled the wrong way round",
  [(F, "            const Complex v = potentialOf(b, c.head.at(a)) - potentialOf(b, c.tail.at(a));",
    "            const Complex v = potentialOf(b, c.tail.at(a)) - potentialOf(b, c.head.at(a));")],
  [(TF, GROUND)]),

 ("a via's potential at its own ends left out",
  [(F, "            v += potentialOf(b, head);\n            if (tail >= 0) v -= potentialOf(b, tail);\n", "")],
  [(TF, GROUND)]),

 ("a via to the ground given the first cell for its tail",
  [(F, "        c.tail[a] = ends.at(a).first < 0 ? -1 : nodeOf.value(ends.at(a).first);",
    "        c.tail[a] = nodeOf.value(ends.at(a).first);")],
  [(TF, GROUND)]),

 ("a via to the ground stood where its cells are, not at its cell's centre",
  [(F, "        v.at = (g.tail < 0 ? mesh.cells.at(g.head).centre : g.weighted / g.area) * micrometre;",
    "        v.at = (g.weighted / g.area) * micrometre;")],
  [(TF, GROUND)]),

 ("each via cell a run of its own",
  [(F, "        const QString key = QStringLiteral(\"%1|%2|%3\").arg(cell.layer).arg(tail).arg(head);",
    "        const QString key = QStringLiteral(\"%1|%2|%3|%4\").arg(cell.layer).arg(tail).arg(head).arg(index);")],
  [(TF, GROUND)]),

 ("a via's self term across its breadth, not its mean distance",
  [(F, "    if (apart <= 1e-6 * std::max(widest, gmd)) return linePair(a0, a1, b0, b1, gmd);",
    "    if (apart <= 1e-6 * std::max(widest, gmd)) return linePair(a0, a1, b0, b1, widest);")],
  [(TF, GROUND)]),

 ("a via's own resistance left out",
  [(F, "        if (v.side > 0 && run.sigma > 0) m(v.run, v.run) += surfaceImpedance(run.sigma, v.side / 4, f) * run.length() / (4 * v.side);",
    "        (void)run;")],
  [(TF, BAR)]),

 ("a via's current not drawn",
  [(F, "        out.insert(it.key(), std::abs(current.at(r)) / (4 * solved.geometry.at(r).width));", "        (void)r;")],
  [(TF, GROUND), (TD, TAB)]),

 ("the stack's ground not a via's end",
  [(M, "                    if (o.kind == Layer::Ground\n", "                    if (false && o.kind == Layer::Ground\n")],
  [(TM, STACK), (TD, TAB)]),

 ("a via to the ground taken from any metal",
  [(M, "                        if (o.kind == Layer::Metal && o.z0 < metal.z0) ++between;", "                        (void)o;")],
  [(TM, STACK)]),

 ("the word ground not the ground",
  [(M, "            && (l.name == name || l.tag == name || name.trimmed().compare(QLatin1String(\"ground\"), Qt::CaseInsensitive) == 0))",
    "            && (l.name == name || l.tag == name))")],
  [(TM, STACK), (TF, GROUND)]),

 ("phase B solving a via to the ground as a floating pad",
  [(S, "        if (stack.isGround(via->from) || stack.isGround(via->to)) {", "        if (false) {")],
  [(TS, REFUSED), (TD, TAB)]),

 ("the tab drawing the vias under the metals",
  [(D, "            for (const QList<LayoutView::Overlay>* each : {&layer, &vias})", "            for (const QList<LayoutView::Overlay>* each : {&vias, &layer})")],
  [(TD, TAB)]),
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
