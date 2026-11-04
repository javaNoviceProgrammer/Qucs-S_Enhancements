"""phases_after.py rf25_fullwave.cpp: the same of fullWaveOf with its static
parts shared out (makeStatics(): the allocation, the columns, the mirror) -
from its start to them, them, the frequencies' end and its own - and a hash
of each static matrix whole, printed to the app's stderr ("STATICSIN ...",
"STATICS ...", "FREQS ...", "END ..."). On a copy of the source, never one
to commit."""
import sys
p = sys.argv[1]
s = open(p).read()


def put(old, new):
    global s
    assert s.count(old) == 1, old[:70]
    s = s.replace(old, new)


NOW = "phaseClock.nsecsElapsed() * 1e-9"
HASH = ("            const auto hashOf = [](const Eigen::MatrixXd& x) {\n"
        "                unsigned long long h = 1469598103934665603ull;\n"
        "                const unsigned long long* w = reinterpret_cast<const unsigned long long*>(x.data());\n"
        "                for (Eigen::Index n = 0; n < x.size(); ++n) h = (h ^ w[n]) * 1099511628211ull;\n"
        "                return h;\n"
        "            };\n")
put("                    const FullWaveControl* control, const QList<ReturnMark>& returns)\n{\n    FullWave out;\n",
    "                    const FullWaveControl* control, const QList<ReturnMark>& returns)\n{\n    FullWave out;\n"
    "    QElapsedTimer phaseClock;\n    phaseClock.start();\n")
put("void makeStatics(Context& c, int threads)\n{\n    const int nr = c.nr, nc = c.nc;\n"
    "    c.lDirect.resize(nr, nr);\n    c.lImage.resize(nr, nr);\n    c.pStatic.resize(nc, nc);\n",
    "void makeStatics(Context& c, int threads)\n{\n    QElapsedTimer phaseClock;\n    phaseClock.start();\n"
    "    const int nr = c.nr, nc = c.nc;\n"
    "    c.lDirect.resize(nr, nr);\n    c.lImage.resize(nr, nr);\n    c.pStatic.resize(nc, nc);\n"
    "    const double s1 = " + NOW + ";\n")
put("            staticInductanceColumn(c, c.lDirect, c.lImage, nr - 1 - (i - nc));\n    }, sharedStatics);\n",
    "            staticInductanceColumn(c, c.lDirect, c.lImage, nr - 1 - (i - nc));\n    }, sharedStatics);\n"
    "    const double s2 = " + NOW + ";\n")
put("        mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));\n    }, sharedStatics);\n}\n",
    "        mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));\n    }, sharedStatics);\n"
    "    const double s3 = " + NOW + ";\n"
    "    fprintf(stderr, \"STATICSIN threads=%d nr=%d nc=%d alloc=%.4f columns=%.4f mirror=%.4f\\n\", threads, nr, nc, s1, "
    "s2 - s1, s3 - s2);\n}\n")
put("    if (!compress) {\n        makeStatics(c, threads);\n",
    "    if (!compress) {\n        const double s0 = " + NOW + ";\n        makeStatics(c, threads);\n"
    "        const double s3 = " + NOW + ";\n        {\n" + HASH +
    "            fprintf(stderr, \"STATICS threads=%d nr=%d nc=%d planar=%d since=%.4f total=%.4f "
    "hashL=%016llx hashI=%016llx hashP=%016llx\\n\", threads, nr, nc, c.planar, s0, s3 - s0, "
    "hashOf(c.lDirect), hashOf(c.lImage), hashOf(c.pStatic));\n        }\n")
put("    for (QThread* thread : std::as_const(pool)) {\n        thread->wait();\n        delete thread;\n    }\n"
    "    if (control != nullptr && control->stop != nullptr && control->stop->load()) {\n        int done = 0;",
    "    for (QThread* thread : std::as_const(pool)) {\n        thread->wait();\n        delete thread;\n    }\n"
    "    fprintf(stderr, \"FREQS since=%.4f\\n\", " + NOW + ");\n"
    "    if (control != nullptr && control->stop != nullptr && control->stop->load()) {\n        int done = 0;")
put("                     .arg(c.nh * (c.nh + 1) / 2).arg(c.rhoMax * 1e3, 0, 'g', 3);\n    return out;\n}\n",
    "                     .arg(c.nh * (c.nh + 1) / 2).arg(c.rhoMax * 1e3, 0, 'g', 3);\n"
    "    fprintf(stderr, \"END since=%.4f\\n\", " + NOW + ");\n    return out;\n}\n")
open(p, 'w').write(s)
