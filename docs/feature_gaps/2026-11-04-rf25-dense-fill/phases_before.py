"""phases_before.py rf25_fullwave.cpp: the timed phases of 9edbb9d's solveAt, its fill one
thread's - Kernels, the inductance, the vias, P, A^T P A, the rest, the LU -
printed to the app's stderr a frequency at a time ("PHASES ..."). On a copy
of the source, never one to commit."""
import sys
p = sys.argv[1]
s = open(p).read()
def put(after, line):
    global s
    assert s.count(after) == 1, after[:60]
    s = s.replace(after, after + line)
put("    const Kernels k(c, f);\n    if (!k.problems.isEmpty()) {\n        out.problems << k.problems;\n        return out;\n    }\n    const Complex jw = k.jw;\n    const int nr = c.nr, nc = c.nc;\n",
    "    const double tK = clock.nsecsElapsed() * 1e-9;\n")
put("            m(a, b) = m(b, a) = jw * (dot * k.inductance(a, b, c.lDirect(a, b), c.lImage(a, b)));\n        }\n",
    "    const double tL = clock.nsecsElapsed() * 1e-9;\n")
put("                for (int j = 0; j < nv; ++j) portVia(q, j) += c.drive(i, q) * k.viaPotential(j, i);\n",
    "    const double tV = clock.nsecsElapsed() * 1e-9;\n")
put("        for (int j = i; j < nc; ++j) p(i, j) = p(j, i) = k.potential(i, j, c.pStatic(i, j), c.pStatic(j, i));\n",
    "    const double tP = clock.nsecsElapsed() * 1e-9;\n")
put("                       / jw;\n",
    "    const double tA = clock.nsecsElapsed() * 1e-9;\n")
put("    ownImpedances(c, f, [&m](int a, int b, Complex v) { m(a, b) += v; });\n",
    "    const double tO = clock.nsecsElapsed() * 1e-9;\n")
put("    const MatrixC pDrive = p * c.drive;\n",
    "    const double tD = clock.nsecsElapsed() * 1e-9;\n")
put("    out.luSeconds = double(clock.nsecsElapsed()) * 1e-9 - out.fillSeconds;\n",
    "    fprintf(stderr, \"PHASES f=%g nr=%d nc=%d nv=%d planar=%d paths=%d np=%d kernels=%.4f inductance=%.4f vias=%.4f P=%.4f ATPA=%.4f own=%.4f pdrive=%.4f rhs=%.4f lu=%.4f\\n\", f, nr, nc, int(c.vias.size()), c.planar, int(c.paths.size()), int(c.np), tK, tL - tK, tV - tL, tP - tV, tA - tP, tO - tA, tD - tO, out.fillSeconds - tD, out.luSeconds);\n")
open(p, 'w').write(s)
