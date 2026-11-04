#!/usr/bin/env python3
"""probe.py: what each distance of fgc_S62's Green's functions' tables takes,
and the four tables made on 1, 4, 8, 16 and 24 threads - a test function
written into tests/test_rf25_wave.cpp for the run and taken out again, and
(PROBE_ORDER=1) a switch in rf25/rf25_wave.cpp's nodesOf for the nearest
first (RF25_FAR_FIRST=0) against the furthest (1), the two run in turn,
PROBE_ROUNDS times, PROBE_REPS solves each, at 8 and 24 threads. The stack
is the model's: 500 µm of silicon (11.7, 6.41 S/m) under 20 µm of polyimide
(3.12, tan 0.002) and the metal's 1.5 µm, no ground; the walls' via through
the metal, its pairs of heights 520 and 521.5 µm; 10 GHz (PROBE_GHZ), out
to 1.1 mm. Sources are put back whatever happens. BRK_BUILD=<dir> the build
(build); HUNT_RUNS=<dir> the cache and logs."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = W + "/qucs-s-26.1.1/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-table-threads-probe")

PROBE = r'''
    // (A probe, never committed: each table distance's time, fgc_S62's stack.)
    void probeNodeCosts()
    {
        if (qEnvironmentVariableIsEmpty("RF25_PROBE")) QSKIP("probe");
        rf::Layers l;
        const double eps0 = 8.8541878128e-12, mu0 = 4e-7 * M_PI;
        const double z[4] = {0, 500e-6, 520e-6, 521.5e-6};
        const double er[3] = {11.7, 3.12, 1.0}, td[3] = {0, 0.002, 0}, sg[3] = {6.41026, 0, 0};
        for (int i = 0; i < 3; ++i) {
            rf::Layers::Slab s;
            s.z0 = z[i];
            s.z1 = z[i + 1];
            s.epsr = er[i];
            s.tand = td[i];
            s.sigma = sg[i];
            s.name = QStringLiteral("D%1").arg(i);
            l.slabs << s;
        }
        l.groundBelow = false;
        const int ghz = qEnvironmentVariableIntValue("RF25_PROBE_GHZ");
        const double f = ghz > 0 ? 1e9 * ghz : 10e9, rhoMax = 1.1e-3, w = 2 * M_PI * f;
        double kMax = std::abs(std::sqrt(Complex(w * w * mu0 * eps0, 0)));
        for (int i = 0; i < 3; ++i)
            kMax = std::max(kMax, std::abs(std::sqrt(w * w * mu0 * (eps0 * Complex(er[i], -er[i] * td[i]) - Complex(0, sg[i] / w)))));
        const auto distances = [kMax, rhoMax](double reach) {
            double low = 1e-3 / kMax;
            if (reach > 0) low = std::min(low, 0.01 * reach);
            const double most = 2 * M_PI / (60 * kMax), ratio = std::pow(10.0, 1.0 / 40);
            QList<double> wanted{0.0};
            for (double r = low;; r = std::min(r * ratio, r + most)) {
                wanted << r;
                if (r >= rhoMax && wanted.size() >= 8) break;
            }
            return wanted;
        };
        const rf::ViaGreen::Span span{520e-6, 521.5e-6};
        const rf::ViaGreen v(l, span, {span}, {520e-6, 521.5e-6}, f, rhoMax);
        QVERIFY2(v.ok(), qPrintable(v.problems().join(" ")));
        const rf::WaveGreen g(l, 521.5e-6, 520e-6, f, rhoMax), g0(l, 520e-6, 520e-6, f, rhoMax), g1(l, 521.5e-6, 521.5e-6, f, rhoMax);
        const int reps = qEnvironmentVariableIntValue("RF25_PROBE_REPS") > 0 ? qEnvironmentVariableIntValue("RF25_PROBE_REPS") : 1;
        const auto run = [&distances](const char* name, double reach, const std::function<void(double)>& node) {
            const QList<double> at = distances(reach);
            double total = 0;
            QList<double> each;
            for (int i = 0; i < at.size(); ++i) {
                QElapsedTimer t;
                t.start();
                node(at.at(i));
                each << t.nsecsElapsed() / 1e3;
                total += each.last();
            }
            QString tenths;
            for (int d = 0; d < 10; ++d) {
                double sum = 0;
                int count = 0;
                for (int i = d * int(at.size()) / 10; i < (d + 1) * int(at.size()) / 10; ++i, ++count) sum += each.at(i);
                tenths += QStringLiteral(" %1").arg(count > 0 ? sum / count : 0, 0, 'f', 0);
            }
            qWarning("%s: %d distances, %.1f ms, reach %g; mean us a distance by tenth, nearest first:%s", name,
                     int(at.size()), total / 1e3, reach, qPrintable(tenths));
        };
        if (reps == 1) {
            run("via", v.reach(), [&v](double r) { v.couplingRemainderByIntegral(0, r); });
            run("pair 520/521.5", g.reach(), [&g](double r) { g.scalarRemainderByIntegral(r); });
            run("pair 520/520", g0.reach(), [&g0](double r) { g0.scalarRemainderByIntegral(r); });
            run("pair 521.5/521.5", g1.reach(), [&g1](double r) { g1.scalarRemainderByIntegral(r); });
        }
        for (int rep = 0; rep < reps; ++rep)
            for (const int threads : {1, 4, 8, 16, 24}) {
                if (reps > 1 && threads != 8 && threads != 24) continue;
                rf::forgetGreens();
                QElapsedTimer t;
                t.start();
                const rf::ViaGreen vt(l, span, {span}, {520e-6, 521.5e-6}, f, rhoMax);
                const int elsewhere = vt.prepare(threads);
                const double viaMs = t.nsecsElapsed() / 1e6;
                t.restart();
                int waves = 0;
                for (const auto& hz : {std::pair<double, double>{520e-6, 520e-6}, {521.5e-6, 520e-6}, {521.5e-6, 521.5e-6}}) {
                    const rf::WaveGreen gt(l, hz.first, hz.second, f, rhoMax);
                    waves += gt.prepare(threads);
                }
                qWarning("prepare on %d threads: via %.1f ms (%d elsewhere of %d), three pairs %.1f ms (%d elsewhere)", threads,
                         viaMs, elsewhere, vt.tableSize(), t.nsecsElapsed() / 1e6, waves);
            }
    }
'''

EDITS = [
    ("tests/test_rf25_wave.cpp", "#include <QThread>\n", "#include <QThread>\n#include <QElapsedTimer>\n#include <functional>\n"),
    ("tests/test_rf25_wave.cpp", "        QVERIFY2(same, \"read from eight threads at once as from one\");\n    }\n};\n",
     "        QVERIFY2(same, \"read from eight threads at once as from one\");\n    }\n" + PROBE + "};\n"),
]
ORDER = [
    ("rf25/rf25_wave.cpp", "            const int i = count - 1 - k;\n",
     "            static const bool farFirst = qEnvironmentVariableIntValue(\"RF25_FAR_FIRST\") != 0;\n"
     "            const int i = farFirst ? count - 1 - k : k;\n"),
]


def main():
    os.makedirs(os.path.join(RUNS, "cache"), exist_ok=True)
    order = os.environ.get("PROBE_ORDER") == "1"
    back = {}
    try:
        for path, old, new in EDITS + (ORDER if order else []):
            p = Q + path
            s = open(p, encoding="utf-8").read()
            if s.count(old) != 1:
                raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
            back.setdefault(p, s)
            open(p, "w", encoding="utf-8").write(s.replace(old, new))
        if subprocess.run(["ninja", "-C", B, "qucs/tests/test_rf25_wave"]).returncode != 0:
            return 1
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"), RF25_PROBE="1",
                   QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
        if os.environ.get("PROBE_GHZ"):
            env["RF25_PROBE_GHZ"] = os.environ["PROBE_GHZ"]
        exe = os.path.join(B, "qucs/tests/test_rf25_wave")
        if not order:
            subprocess.run([exe, "probeNodeCosts"], env=env, cwd=RUNS)
            return 0
        env["RF25_PROBE_REPS"] = os.environ.get("PROBE_REPS", "5")
        for _ in range(int(os.environ.get("PROBE_ROUNDS", "4"))):
            for far in ("0", "1"):
                print("RF25_FAR_FIRST=%s" % far, flush=True)
                subprocess.run([exe, "probeNodeCosts"], env=dict(env, RF25_FAR_FIRST=far), cwd=RUNS)
        return 0
    finally:
        for p, s in back.items():
            open(p, "w", encoding="utf-8").write(s)


if __name__ == "__main__":   # (read only, for its text: nothing run)
    sys.exit(main())
