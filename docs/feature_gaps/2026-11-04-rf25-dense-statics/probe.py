"""probe.py rf25_fullwave.cpp: makeStatics() prints how
long it took and how many of its jobs threads other than the caller's did
("PROBE ..."): how the tests' margins were measured. On a copy of the
source, never one to commit."""
import sys
p = sys.argv[1]
s = open(p).read()


def put(old, new):
    global s
    assert s.count(old) == 1, old[:70]
    s = s.replace(old, new)


put("void makeStatics(Context& c, int threads)\n{\n    const int nr = c.nr, nc = c.nc;\n",
    "void makeStatics(Context& c, int threads)\n{\n    QElapsedTimer probe;\n    probe.start();\n"
    "    const long long probeWas = sharedStatics.load();\n    const int nr = c.nr, nc = c.nc;\n")
put("        mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));\n    }, sharedStatics);\n}\n",
    "        mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));\n    }, sharedStatics);\n"
    "    fprintf(stderr, \"PROBE threads=%d nr=%d nc=%d jobs=%d others=%lld seconds=%.6f\\n\", threads, nr, nc, "
    "nc + nr + 2 * blocks, sharedStatics.load() - probeWas, probe.nsecsElapsed() * 1e-9);\n}\n")
open(p, 'w').write(s)
