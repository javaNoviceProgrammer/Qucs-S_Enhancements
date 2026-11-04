"""phases_before.py rf25_fullwave.cpp: ac3f5fc's fullWaveOf timed - from its
start to the dense solve's static parts, the parts themselves on one thread
(the inductances' zeros, the inductances, the potentials), the frequencies'
end and its own - and a hash of each static matrix whole, printed to the
app's stderr ("STATICS ...", "FREQS ...", "END ..."). On a copy of the
source, never one to commit."""
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
put("    if (!compress) {\n        c.lDirect = Eigen::MatrixXd::Zero(nr, nr);\n        c.lImage = Eigen::MatrixXd::Zero(nr, nr);\n",
    "    if (!compress) {\n        const double s0 = " + NOW + ";\n"
    "        c.lDirect = Eigen::MatrixXd::Zero(nr, nr);\n        c.lImage = Eigen::MatrixXd::Zero(nr, nr);\n"
    "        const double s1 = " + NOW + ";\n")
put("                c.lImage(a, b) = c.lImage(b, a) = alignedInductance(c.runs.at(a), c.mirrored.at(b));\n            }\n",
    "                c.lImage(a, b) = c.lImage(b, a) = alignedInductance(c.runs.at(a), c.mirrored.at(b));\n            }\n"
    "        const double s2 = " + NOW + ";\n")
put("                c.pStatic(i, j) = rectanglePotential(c.rect.at(j), c.centre.at(i), std::abs(zOf.at(i) - zOf.at(j)));\n",
    "                c.pStatic(i, j) = rectanglePotential(c.rect.at(j), c.centre.at(i), std::abs(zOf.at(i) - zOf.at(j)));\n"
    "        const double s3 = " + NOW + ";\n        {\n" + HASH +
    "            fprintf(stderr, \"STATICS threads=%d nr=%d nc=%d planar=%d since=%.4f zero=%.4f L=%.4f P=%.4f total=%.4f "
    "hashL=%016llx hashI=%016llx hashP=%016llx\\n\", threads, nr, nc, c.planar, s0, s1 - s0, s2 - s1, s3 - s2, s3 - s0, "
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
