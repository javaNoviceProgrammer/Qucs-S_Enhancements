"""phases_after.py rf25_fullwave.cpp: the same of solveAt shared out among a
frequency's threads - Kernels, the part above the diagonal, the mirror,
the charges, the rest, the LU. On a copy of the source, never one to
commit."""
import sys
p = sys.argv[1]
s = open(p).read()
def put(after, line):
    global s
    assert s.count(after) == 1, after[:70]
    s = s.replace(after, after + line)
put("    const Complex jw = k.jw;\n    const int nr = c.nr, nc = c.nc, nv = int(c.vias.size());\n",
    "    const double tK = clock.nsecsElapsed() * 1e-9;\n")
put("            potentialColumn(c, k, p, nc - 1 - (i - nr));\n    });\n",
    "    const double tU = clock.nsecsElapsed() * 1e-9;\n")
put("        mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));\n    });\n",
    "    const double tM = clock.nsecsElapsed() * 1e-9;\n")
put("    sharedOut(nr, threads, [&c, &p, &m, jw](int b) { chargeColumn(c, p, jw, m, b); });\n",
    "    const double tC = clock.nsecsElapsed() * 1e-9;\n")
put("    out.luSeconds = double(clock.nsecsElapsed()) * 1e-9 - out.fillSeconds;\n",
    "    fprintf(stderr, \"PHASES f=%g threads=%d nr=%d nc=%d kernels=%.4f upper=%.4f mirror=%.4f charges=%.4f rest=%.4f lu=%.4f\\n\", f, threads, nr, nc, tK, tU - tK, tM - tU, tC - tM, out.fillSeconds - tC, out.luSeconds);\n")
open(p, 'w').write(s)
