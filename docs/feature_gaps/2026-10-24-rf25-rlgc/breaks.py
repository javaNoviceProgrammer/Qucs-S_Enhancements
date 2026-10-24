#!/usr/bin/env python3
"""breaks.py: each part of the line's losses - the surface impedance, how
the charge crowds, the dielectrics' share of the field, and what the four
of R, L, G and C make - undone on purpose. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-rlgc-breaks")
os.makedirs(RUNS, exist_ok=True)

S, D, T = "rf25/rf25_static.cpp", "rf25doc.cpp", "qucscontrol_rf25.cpp"
ST, DOC, CTL = "test_rf25_static", "test_rf25_doc", "test_qucs_control"
SLAB = "theSurfaceImpedanceIsThatOfASlab"
LOSS = "aLinesResistanceAndConductanceAreThoseOfItsLosses"
TOOL = "anRf25ModelIsSolvedThroughItsTool"

BREAKS = [
 # ---- the surface impedance
 ("a metal is a half space however thin it is",
  [(S, "    std::complex<double> coth;\n    if (std::abs(kt.real()) > 30) coth = 1.0;\n    else coth = std::cosh(kt) / std::sinh(kt);",
    "    const std::complex<double> coth = 1.0;")],
  [(ST, SLAB)]),

 ("a thick metal's coth is taken as a ratio of two runaways",
  [(S, "    if (std::abs(kt.real()) > 30) coth = 1.0;", "    if (false) coth = 1.0;")],
  [(ST, SLAB)]),

 ("the skin depth leaves out the permeability",
  [(S, "    const double depth = std::sqrt(2.0 / (omega * mu0 * sigma));\n    const std::complex<double> k{1.0 / depth, 1.0 / depth};",
    "    const double depth = std::sqrt(2.0 / (omega * sigma));\n    const std::complex<double> k{1.0 / depth, 1.0 / depth};")],
  [(ST, SLAB)]),

 ("a metal that says no conductivity is given one all the same",
  [(S, "    if (!(sigma > 0) || !(t > 0) || !(frequency > 0)) return {0, 0};", "    if (false) return {0, 0};")],
  [(ST, SLAB), (ST, LOSS)]),

 # ---- how the charge crowds
 ("the charge is taken as spread evenly, however it crowds",
  [(S, "                    if (area > 0) square += q(i) * q(i) / area;", "                    if (area > 0) square += total * total / area;")],
  [(ST, LOSS)]),

 ("a cell's crowding is of its charge, not its density",
  [(S, "                crowd->append(total != 0 ? square / (total * total) : 0.0);", "                crowd->append(square);")],
  [(ST, LOSS)]),

 ("the resistance is Rs over a width, with no length in it",
  [(S, "    out.r = out.surfaceResistance * length * extraction.crowding.value(index);",
    "    out.r = out.surfaceResistance * extraction.crowding.value(index);")],
  [(ST, LOSS)]),

 # ---- the dielectrics' share
 ("every dielectric's loss counts the same, whatever share of the field it holds",
  [(S, "                if (here != 0) loss[i] += tand * was * dCdEps / here;", "                if (here != 0) loss[i] += tand;")],
  [(ST, LOSS)]),

 ("the conductance is not w C tan delta",
  [(S, "    out.g = omega * out.c * extraction.lossTangent.value(index);", "    out.g = omega * out.c;")],
  [(ST, LOSS)]),

 # ---- what the four make
 ("the characteristic impedance is of the series arm alone",
  [(S, "    if (std::abs(shunt) > 0) out.zc = std::sqrt(series / shunt);", "    if (std::abs(shunt) > 0) out.zc = std::sqrt(series);")],
  [(ST, LOSS)]),

 ("the propagation constant is the sum of the arms, not their product",
  [(S, "    out.gamma = std::sqrt(series * shunt);", "    out.gamma = std::sqrt(series + shunt);")],
  [(ST, LOSS)]),

 ("the root is taken on the side that gains rather than loses",
  [(S, "    if (out.gamma.real() < 0) out.gamma = -out.gamma;", "    if (out.gamma.real() > 0) out.gamma = -out.gamma;")],
  [(ST, LOSS)]),

 ("nepers are reported as decibels",
  [(S, "    out.alphaDb = out.gamma.real() * 20.0 / std::log(10.0);", "    out.alphaDb = out.gamma.real();")],
  [(ST, LOSS)]),

 ("a frequency of none is still taken as one",
  [(S, "    if (!(frequency > 0)) return out;", "    if (false) return out;")],
  [(ST, LOSS)]),

 # ---- what is said of it
 ("the tab says nothing of R, L, G and C at the study's frequency",
  [(D, '            out << tr("%1 at %2 GHz: R %3 Ohm/m, L %4 nH/m, G %5 S/m, C %6 pF/m; %7 dB/m")',
    '            out << tr("%1 at %2 GHz: %3 %4 %5 %6 %7")')],
  [(DOC, "theModelIsSolvedAndSaysWhatItFound")]),

 ("the tool ignores the frequency it was given",
  [(T, "        frequency = given.constant();", "")],
  [(CTL, TOOL)]),

 ("the tool answers the losses as though there were none",
  [(T, "            if (lp.frequency > 0) {", "            if (false) {")],
  [(CTL, TOOL)]),
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
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
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
