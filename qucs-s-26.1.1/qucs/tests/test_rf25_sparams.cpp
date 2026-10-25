/*
 * test_rf25_sparams.cpp - the quasi-static extraction as a circuit
 * (rf25/rf25_sparams.h): one net as the multiphysics engine's own line,
 * all of them as coupled lines, and the S-parameters of each.
 *
 * The end of phase B of docs/proposals/2026-10-23-rf-2p5d-moment-method.md,
 * whose test list asks for "a 50 Ohm microstrip's S11 below -30 dB and its
 * phase against beta l" and for "reciprocity (S = S^T) and passivity on
 * every result".
 */
#include <QtTest>

#include "multiphysics/fem_export.h"
#include "rf25/rf25_sparams.h"

#include <cmath>

namespace rf = qucs_s::rf25;
namespace fem = qucs_s::fem;

using cd = std::complex<double>;

namespace {

/// A stack of a substrate of \a epsr and \a height (m) on a ground plane,
/// with one metal on top of it and air above. \a tand and \a sigma give
/// the substrate its loss and the metal its conductivity.
rf::Stack grounded(double epsr, double height, double tand = 0, double sigma = 0, double thick = 0)
{
    rf::Stack s;
    rf::Layer ground;
    ground.kind = rf::Layer::Ground;
    ground.name = QStringLiteral("GND");
    ground.tag = QStringLiteral("gnd");
    rf::Layer sub;
    sub.kind = rf::Layer::Substrate;
    sub.name = QStringLiteral("SUB");
    sub.tag = QStringLiteral("sub");
    sub.thickness = height;
    sub.epsr = epsr;
    sub.tand = tand;
    rf::Layer metal;
    metal.kind = rf::Layer::Metal;
    metal.name = QStringLiteral("M1");
    metal.tag = QStringLiteral("m1");
    metal.thickness = thick;
    metal.sigma = sigma;
    s.layers << ground << sub << metal;
    return s;
}

rf::Conductor strip(double x, double y, double w, double h)
{
    rf::Conductor c;
    c.layer = QStringLiteral("M1");
    c.outline = QPolygonF(QRectF(x, y, w, h));
    return c;
}

rf::Mesh meshed(const QList<rf::Conductor>& conductors, double cell)
{
    rf::MeshSettings how;
    how.maximum = cell * 1e-6;
    how.edges = true;
    return rf::meshOf(conductors, how, rf::Stack(), 0);
}

double dB(cd s) { return 20 * std::log10(std::max(1e-300, std::abs(s))); }

} // namespace

class TestRf25Sparams : public QObject
{
    Q_OBJECT

private slots:

    // One net as the engine's own line: the same numbers lineParameters()
    // gave, and at a frequency a lossless line whose Zc is its Z0 and
    // whose propagation is jw/v.
    void aNetIsTheEnginesOwnLine()
    {
        const double w = 200.0, h = 100e-6, epsr = 9.8, length = 800e-6;
        const QList<rf::Conductor> line{strip(0, 0, 800, w)};
        const rf::Extraction e = rf::extract(meshed(line, 50), line, grounded(epsr, h));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));

        const double f = 10e9;
        // A lossy stack, so that every term is a different number and a
        // crossing of two of them cannot hide.
        const rf::Extraction loss = rf::extract(meshed(line, 50), line, grounded(epsr, h, 0.002, 5.8e7, 3e-6));
        QVERIFY2(loss.ok(), qPrintable(loss.problems.join(" | ")));
        const rf::LineParameters lp = rf::lineParameters(loss, 0, length, f);
        QVERIFY(lp.ok());
        QVERIFY2(lp.r > 0 && lp.g > 0, "the fixture must lose, or R and G cannot be told apart");
        QVERIFY2(lp.r > 1e3 * lp.g, "R and G differ by orders of magnitude here, so a crossing shows at once");
        const fem::LineRLGC l = rf::asLine(loss, 0, length, f);
        QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
        QCOMPARE(l.frequency, f);
        QCOMPARE(l.C, lp.c);
        QCOMPARE(l.L, lp.l);
        QCOMPARE(l.R, lp.r);
        QCOMPARE(l.G, lp.g);
        QCOMPARE(l.Zc, lp.zc);
        QCOMPARE(l.gamma, lp.gamma);
        QCOMPARE(l.alphaDb, lp.alphaDb);
        // The engine's epsEff is what beta says, which on a lossy line is
        // not the quasi-static C/C0 - and the velocity with it.
        const double k0 = 2 * M_PI * f / rf::kLightSpeed;
        const double n = l.gamma.imag() / k0;
        QVERIFY2(std::abs(l.epsEff - n * n) < 1e-9 * n * n,
                 qPrintable(QStringLiteral("%1 vs (beta/k0)^2 %2").arg(l.epsEff).arg(n * n)));
        QVERIFY2(std::abs(l.velocity - 2 * M_PI * f / l.gamma.imag()) < 1e-6 * l.velocity, "v is w over beta");
        const double quasiStatic = rf::lineParameters(loss, 0, length).epsilonEff;
        QVERIFY2(std::abs(l.epsEff - quasiStatic) > 1e-6 * quasiStatic,
                 qPrintable(QStringLiteral("beta says %1 and C/C0 says %2: a lossy line's are not the same")
                                .arg(l.epsEff).arg(quasiStatic)));

        // A perfect metal on a lossless substrate: Zc is the real Z0 and
        // gamma is exactly jw sqrt(LC), which is the check that the
        // bridge did not quietly lose the quasi-static relations.
        const rf::Extraction clean = e;
        const fem::LineRLGC q = rf::asLine(clean, 0, length, f);
        QVERIFY(q.error.isEmpty());
        QCOMPARE(q.R, 0.0);
        QCOMPARE(q.G, 0.0);
        const double z0 = std::sqrt(q.L / q.C);
        QVERIFY2(std::abs(q.Zc.real() - z0) < 1e-9 * z0, qPrintable(QStringLiteral("%1 vs %2").arg(q.Zc.real()).arg(z0)));
        QVERIFY2(std::abs(q.Zc.imag()) < 1e-9 * z0, "a lossless line's Zc is real");
        QCOMPARE(q.gamma.real(), 0.0);
        const double beta = 2 * M_PI * f * std::sqrt(q.L * q.C);
        QVERIFY2(std::abs(q.gamma.imag() - beta) < 1e-9 * beta, "a lossless line's gamma is jw sqrt(LC)");
        // And a lossless line's eps_eff from beta *is* the quasi-static
        // C/C0, to a part in a million - the same relation the lossy one
        // above departs from.
        const rf::LineParameters qs = rf::lineParameters(clean, 0, length);
        QVERIFY2(std::abs(q.epsEff - qs.epsilonEff) < 1e-6 * qs.epsilonEff,
                 qPrintable(QStringLiteral("%1 vs %2").arg(q.epsEff).arg(qs.epsilonEff)));

        // What it refuses: a net that is not there, a length of none.
        QVERIFY(!rf::asLine(e, 9, length, f).error.isEmpty());
        QVERIFY(!rf::asLine(e, 0, 0, f).error.isEmpty());
    }

    // The proposal's own test: a microstrip solved for its Z0, then fed
    // into ports of that impedance, reflects next to nothing and delays
    // by beta l.
    void aMatchedLineReflectsNothingAndDelaysByBetaL()
    {
        const double h = 100e-6, epsr = 9.8, length = 2e-3;
        // 200 µm on 100 µm of alumina: near enough 50 Ohm.
        const QList<rf::Conductor> line{strip(0, 0, 2000, 200)};
        const rf::Extraction e = rf::extract(meshed(line, 60), line, grounded(epsr, h));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
        const double f = 5e9;
        const fem::LineRLGC l = rf::asLine(e, 0, length, f);
        QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
        const double z0 = l.Zc.real();
        QVERIFY2(z0 > 20 && z0 < 100, qPrintable(QStringLiteral("Z0 %1").arg(z0)));

        // Matched: S11 below -30 dB, as the proposal asks.
        const std::array<cd, 4> s = fem::lineSParameters(l, length, z0);
        QVERIFY2(dB(s[0]) < -30, qPrintable(QStringLiteral("S11 %1 dB").arg(dB(s[0]))));
        QVERIFY2(dB(s[3]) < -30, qPrintable(QStringLiteral("S22 %1 dB").arg(dB(s[3]))));
        // Lossless, so all of it comes out.
        QVERIFY2(std::abs(std::abs(s[1]) - 1.0) < 1e-6, qPrintable(QStringLiteral("|S21| %1").arg(std::abs(s[1]))));
        // And its phase is exactly -beta l, which is the one number a
        // transmission line is for.
        const double beta = l.gamma.imag();
        const double wanted = -beta * length;
        double phase = std::arg(s[1]);
        while (phase - wanted > M_PI) phase -= 2 * M_PI;
        while (wanted - phase > M_PI) phase += 2 * M_PI;
        QVERIFY2(std::abs(phase - wanted) < 1e-6,
                 qPrintable(QStringLiteral("arg S21 %1 vs -beta l %2").arg(phase).arg(wanted)));

        // Reciprocal, and passive.
        QVERIFY2(std::abs(s[1] - s[2]) < 1e-12 * std::abs(s[1]), "S21 is S12");
        QVERIFY2(std::norm(s[0]) + std::norm(s[1]) <= 1.0 + 1e-9, "a lossless line gives nothing away");

        // A quarter wave of it, which is a transformer: through ports of
        // z its reflection is exactly (Z0^2 - z^2)/(Z0^2 + z^2), and that
        // closed form is the sharpest check there is on both the beta
        // this extraction found and the ABCD the engine turns into S.
        // beta l = pi/2 at f = 1/(4 l sqrt(LC)).
        const double quarter = 1.0 / (4 * length * std::sqrt(l.L * l.C));
        const fem::LineRLGC qw = rf::asLine(e, 0, length, quarter);
        QVERIFY(qw.error.isEmpty());
        QVERIFY2(std::abs(qw.gamma.imag() * length - M_PI / 2) < 1e-6, "a quarter wave is a quarter wave");
        const double z = 25.0;
        const std::array<cd, 4> m = fem::lineSParameters(qw, length, z);
        const double zq = qw.Zc.real();
        const double wantedS11 = (zq * zq - z * z) / (zq * zq + z * z);
        QVERIFY2(std::abs(m[0].real() - wantedS11) < 1e-6,
                 qPrintable(QStringLiteral("S11 %1 vs %2").arg(m[0].real()).arg(wantedS11)));
        QVERIFY2(std::abs(m[0].imag()) < 1e-9, "a quarter wave's reflection is real");

        // And a half wave of it is transparent, whatever the ports are:
        // all of it comes out, turned over.
        const fem::LineRLGC hw = rf::asLine(e, 0, length, 2 * quarter);
        const std::array<cd, 4> t = fem::lineSParameters(hw, length, 13.0);
        QVERIFY2(std::abs(t[0]) < 1e-9, qPrintable(QStringLiteral("S11 %1").arg(std::abs(t[0]))));
        QVERIFY2(std::abs(t[1] + 1.0) < 1e-6, qPrintable(QStringLiteral("S21 %1 + j%2").arg(t[1].real()).arg(t[1].imag())));
    }

    // A lossy line: what comes out is exp(-alpha l) of what went in, and
    // the loss is the R and G the extraction found.
    void aLossyLinesTransmissionIsItsAttenuation()
    {
        const double h = 100e-6, epsr = 9.8, length = 10e-3;
        const QList<rf::Conductor> line{strip(0, 0, 2000, 200)};
        const rf::Extraction e = rf::extract(meshed(line, 60), line, grounded(epsr, h, 0.002, 5.8e7, 3e-6));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
        const double f = 20e9;
        const fem::LineRLGC l = rf::asLine(e, 0, length, f);
        QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
        QVERIFY2(l.R > 0 && l.G > 0, "a lossy stack loses");
        QVERIFY(l.alphaDb > 0);
        const std::array<cd, 4> s = fem::lineSParameters(l, length, l.Zc.real());
        // Matched to its own (complex) Zc's real part, so the through is
        // the attenuation and little else.
        const double wanted = std::exp(-l.gamma.real() * length);
        QVERIFY2(std::abs(std::abs(s[1]) - wanted) < 2e-3 * wanted,
                 qPrintable(QStringLiteral("|S21| %1 vs exp(-alpha l) %2").arg(std::abs(s[1])).arg(wanted)));
        // It loses, so it cannot be passive-bounded at 1.
        QVERIFY2(std::norm(s[0]) + std::norm(s[1]) < 1.0, "a lossy line keeps some of it");
        // And the longer it is the more it loses.
        const fem::LineRLGC twice = rf::asLine(e, 0, 2 * length, f);
        const std::array<cd, 4> s2 = fem::lineSParameters(twice, 2 * length, twice.Zc.real());
        QVERIFY2(std::abs(s2[1]) < std::abs(s[1]), "twice the line loses more");
    }

    // The sweep: the frequencies a study asks for, and what it costs.
    void aSweepIsOneSolveAndManyFrequencies()
    {
        QString why;
        const QList<double> linear = rf::sweepFrequencies(1e9, 5e9, 5, false, &why);
        QVERIFY(why.isEmpty());
        QCOMPARE(linear.size(), 5);
        QCOMPARE(linear.first(), 1e9);
        QCOMPARE(linear.last(), 5e9);
        QVERIFY2(std::abs(linear.at(2) - 3e9) < 1.0, "the middle of a linear sweep is its middle");

        const QList<double> log = rf::sweepFrequencies(1e8, 1e10, 3, true, &why);
        QVERIFY(why.isEmpty());
        QCOMPARE(log.size(), 3);
        QVERIFY2(std::abs(log.at(1) - 1e9) < 1.0, "the middle of a decade-wide log sweep is its geometric mean");

        // One point is the top, as a single-frequency study asks for.
        QCOMPARE(rf::sweepFrequencies(1e9, 4e9, 1, false, &why), QList<double>{4e9});
        // What is no sweep.
        QVERIFY(rf::sweepFrequencies(1e9, 4e9, 0, false, &why).isEmpty());
        QVERIFY(!why.isEmpty());
        QVERIFY(rf::sweepFrequencies(4e9, 1e9, 5, false, &why).isEmpty());
        QVERIFY(!why.isEmpty());
        QVERIFY(rf::sweepFrequencies(0, 1e10, 5, true, &why).isEmpty());
        QVERIFY2(!why.isEmpty(), "a log sweep cannot start at nothing");
        QVERIFY(rf::sweepFrequencies(-1e9, 1e10, 5, false, &why).isEmpty());
        QVERIFY(!why.isEmpty());

        // And the sweep itself: C does not move with frequency and R
        // rises as its root, which is what makes one solve enough.
        const QList<rf::Conductor> line{strip(0, 0, 2000, 200)};
        const rf::Extraction e = rf::extract(meshed(line, 60), line, grounded(9.8, 100e-6, 0.002, 5.8e7, 3e-6));
        QVERIFY(e.ok());
        const QList<double> four = rf::sweepFrequencies(1e9, 100e9, 3, true, &why);
        const fem::LineSweep sweep = rf::lineSweep(e, 0, 2e-3, four);
        QVERIFY2(sweep.error.isEmpty(), qPrintable(sweep.error));
        QCOMPARE(int(sweep.points.size()), 3);
        for (const fem::LineRLGC& p : sweep.points) {
            QCOMPARE(p.C, sweep.points.front().C);
            QCOMPARE(p.L, sweep.points.front().L);
        }
        // A hundredfold in frequency is a hundredfold in G, which is
        // w C tan delta and nothing else.
        const double gRatio = sweep.points.back().G / sweep.points.front().G;
        QVERIFY2(std::abs(gRatio - 100.0) < 1.0, qPrintable(QStringLiteral("G rose %1 times").arg(gRatio)));
        // R is not so simple, and that is the point of a slab's surface
        // impedance rather than a half space's. Where the metal is many
        // skin depths thick at both ends of the sweep, R goes as the root
        // of the frequency - tenfold over a hundredfold:
        const rf::Extraction fat = rf::extract(meshed(line, 60), line, grounded(9.8, 100e-6, 0.002, 5.8e7, 30e-6));
        QVERIFY(fat.ok());
        const fem::LineSweep fatSweep = rf::lineSweep(fat, 0, 2e-3, four);
        QVERIFY(fatSweep.error.isEmpty());
        const double fatRatio = fatSweep.points.back().R / fatSweep.points.front().R;
        QVERIFY2(std::abs(fatRatio - 10.0) < 0.1, qPrintable(QStringLiteral("30 µm: R rose %1 times").arg(fatRatio)));
        // Where it is thin beside one at both ends, R is the direct
        // current's and hardly moves at all:
        const rf::Extraction foil = rf::extract(meshed(line, 60), line, grounded(9.8, 100e-6, 0.002, 5.8e7, 0.1e-6));
        QVERIFY(foil.ok());
        const fem::LineSweep foilSweep = rf::lineSweep(foil, 0, 2e-3, four);
        QVERIFY(foilSweep.error.isEmpty());
        const double foilRatio = foilSweep.points.back().R / foilSweep.points.front().R;
        QVERIFY2(foilRatio > 1.0 && foilRatio < 1.3, qPrintable(QStringLiteral("0.1 µm: R rose %1 times").arg(foilRatio)));
        // And the 3 µm metal of this sweep is between the two - one and a
        // half skin depths at 1 GHz, fourteen at 100 - so it rises by
        // more than ten, which is the crossover a half-space formula
        // cannot see.
        const double rRatio = sweep.points.back().R / sweep.points.front().R;
        QVERIFY2(rRatio > 10.2 && rRatio < 12.0, qPrintable(QStringLiteral("3 µm: R rose %1 times").arg(rRatio)));
        // A frequency of none has no S-parameters, and says so rather
        // than answering a line with no impedance.
        QVERIFY(!rf::lineSweep(e, 0, 2e-3, {0.0}).error.isEmpty());
        QVERIFY(!rf::lineSweep(e, 0, 2e-3, {}).error.isEmpty());
    }

    // The study's own frequencies, read as the mesh reads its own: from,
    // to, points and spacing.
    void theStudysOwnFrequenciesAreRead()
    {
        rf::Model m = rf::Model::blank();
        // What a blank model asks for: 0.1 to 40 GHz in 41 linear points.
        QString why;
        QList<double> f = rf::studySweep(m, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(f.size(), 41);
        QVERIFY2(std::abs(f.first() - 1e8) < 1.0, qPrintable(QString::number(f.first())));
        QVERIFY2(std::abs(f.last() - 4e10) < 1.0, qPrintable(QString::number(f.last())));
        QVERIFY2(std::abs(f.at(20) - 2.005e10) < 1e6, qPrintable(QString::number(f.at(20))));

        // Logarithmic, which the linear reading cannot be mistaken for:
        // three points over a decade put the middle at the geometric mean.
        rf::Node* study = m.root.child(QStringLiteral("rf_study"));
        QVERIFY(study != nullptr);
        study->props.insert(QStringLiteral("from"), QStringLiteral("1[GHz]"));
        study->props.insert(QStringLiteral("to"), QStringLiteral("100[GHz]"));
        study->props.insert(QStringLiteral("points"), 3);
        study->props.insert(QStringLiteral("scale"), QStringLiteral("log"));
        f = rf::studySweep(m, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(f.size(), 3);
        QVERIFY2(std::abs(f.at(1) - 1e10) < 1e4, qPrintable(QString::number(f.at(1))));
        // The same study read as linear would put it at 50.5 GHz, which
        // is what this would be if the spacing were not read at all.
        QVERIFY2(std::abs(f.at(1) - 50.5e9) > 1e9, "the study's spacing was not read");

        // Written the other way about, a sweep still runs upwards.
        study->props.insert(QStringLiteral("from"), QStringLiteral("100[GHz]"));
        study->props.insert(QStringLiteral("to"), QStringLiteral("1[GHz]"));
        f = rf::studySweep(m, &why);
        QCOMPARE(f.size(), 3);
        QVERIFY2(f.first() < f.last(), "a sweep runs upwards");

        // A frequency that is no number. Said of a *linear* study, since a
        // logarithmic one would refuse a fallback of nothing for its own
        // reason and so could not see this one.
        study->props.insert(QStringLiteral("scale"), QStringLiteral("linear"));
        study->props.insert(QStringLiteral("from"), QStringLiteral("1[GHz]"));
        study->props.insert(QStringLiteral("to"), QStringLiteral("sideways"));
        QVERIFY2(rf::studySweep(m, &why).isEmpty(), "it swept a frequency that is no number");
        QVERIFY2(why.contains(QStringLiteral("to")), qPrintable(why));
        // And of a logarithmic one, which refuses it as well.
        study->props.insert(QStringLiteral("scale"), QStringLiteral("log"));
        QVERIFY(rf::studySweep(m, &why).isEmpty());
        QVERIFY2(!why.isEmpty(), "it said nothing of the nonsense it was given");
        rf::Model bare;
        QVERIFY(rf::studySweep(bare, &why).isEmpty());
        QVERIFY(!why.isEmpty());

        // And the number reader behind it, on a property of a node: a
        // unit, a parameter, nothing, and nonsense.
        bool ok = false;
        QCOMPARE(rf::numberOf(*study, QStringLiteral("from"), m, 7.0, &ok), 1e9);
        QVERIFY(ok);
        QCOMPARE(rf::numberOf(*study, QStringLiteral("to"), m, 7.0, &ok), 7.0);
        QVERIFY2(!ok, "nonsense is not the same as not given");
        study->props.insert(QStringLiteral("to"), QString());
        QCOMPARE(rf::numberOf(*study, QStringLiteral("to"), m, 7.0, &ok), 7.0);
        QVERIFY2(ok, "not given is not the same as nonsense");
    }

    // Two lines as the engine's coupled lines: its own mode solver on the
    // matrices this extraction filled, and the 4-port those make.
    void twoLinesAreCoupledLinesWithTheirModes()
    {
        const double h = 100e-6, epsr = 9.8, length = 2e-3, f = 5e9;
        // A pair 100 µm apart, edge coupled.
        const QList<rf::Conductor> pair{strip(0, 0, 2000, 200), strip(0, 300, 2000, 200)};
        const rf::Extraction e = rf::extract(meshed(pair, 60), pair, grounded(epsr, h));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
        QCOMPARE(e.names.size(), 2);

        const fem::CoupledLines c = rf::asCoupledLines(e, length, f);
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        QCOMPARE(int(c.modes.size()), 2);
        QVERIFY2(c.symmetricPair, "two identical strips are a symmetric pair");
        QCOMPARE(c.modes[0].name, QStringLiteral("even"));
        QCOMPARE(c.modes[1].name, QStringLiteral("odd"));
        // Every mode travels, and its impedance is positive.
        for (const fem::LineMode& m : c.modes) {
            QVERIFY2(m.gamma.imag() > 0, "a mode propagates forward");
            QVERIFY2(m.Z.real() > 0, qPrintable(QStringLiteral("%1: Z %2").arg(m.name).arg(m.Z.real())));
            QVERIFY2(m.nEff > 1 && m.nEff < std::sqrt(epsr), qPrintable(QStringLiteral("%1: n_eff %2").arg(m.name).arg(m.nEff)));
        }
        // The common mode keeps more of its field in the substrate than
        // the differential one, so it is the slower of the two.
        QVERIFY2(c.modes[0].nEff > c.modes[1].nEff,
                 qPrintable(QStringLiteral("even %1 vs odd %2").arg(c.modes[0].nEff).arg(c.modes[1].nEff)));
        // The odd mode's impedance is the lower: the two strips are a
        // volt apart across a smaller gap than either is from ground.
        QVERIFY2(c.modes[1].Z.real() < c.modes[0].Z.real(),
                 qPrintable(QStringLiteral("odd %1 vs even %2").arg(c.modes[1].Z.real()).arg(c.modes[0].Z.real())));

        // L is C0^-1/c^2: the check is that C0 L = 1/c^2, which is what
        // the relation means for a matrix.
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) {
                double sum = 0;
                for (int k = 0; k < 2; ++k) sum += c.C0[std::size_t(i)][std::size_t(k)] * c.L[std::size_t(k)][std::size_t(j)];
                const double wanted = i == j ? 1.0 / (rf::kLightSpeed * rf::kLightSpeed) : 0.0;
                QVERIFY2(std::abs(sum - wanted) < 1e-6 / (rf::kLightSpeed * rf::kLightSpeed),
                         qPrintable(QStringLiteral("C0 L (%1,%2) = %3").arg(i).arg(j).arg(sum)));
            }
        // R is diagonal: the proximity effect is not in an electrostatic
        // solve, and the matrix says so rather than guessing at it.
        QVERIFY(c.R[0][1] == 0.0 && c.R[1][0] == 0.0);

        // Per metre, and the same per metre the single line reports: the
        // modes are blind to the scaling (C and C0 move together, so
        // eps_eff does not), so this is what pins it.
        const fem::LineRLGC one = rf::asLine(e, 0, length, f);
        QVERIFY(one.error.isEmpty());
        double row = 0;
        for (int k = 0; k < 2; ++k) row += c.C[0][std::size_t(k)];
        // A microstrip pair is some hundreds of pF/m. Without the length
        // in it the same numbers come out a thousand times smaller, which
        // is why this is an absolute bound and not a ratio.
        QVERIFY2(row > 1e-11 && row < 1e-9, qPrintable(QStringLiteral("%1 F/m is no microstrip").arg(row)));
        // Near the single line's own C per metre, but not equal to it:
        // symmetrising shifts a row sum by half the asymmetry of the
        // collocation solve, which two strips meshed differently have.
        QVERIFY2(std::abs(row - one.C) < 0.1 * one.C,
                 qPrintable(QStringLiteral("%1 F/m across the row vs %2 F/m of the line").arg(row).arg(one.C)));

        // And exactly symmetric, not nearly: the solve's own asymmetry is
        // numerical, and a generalized self-adjoint eigenproblem wants
        // the matrices symmetric to the bit.
        for (const fem::CoupledLines::Matrix* m : {&c.C, &c.C0, &c.L, &c.G, &c.R})
            QVERIFY2((*m)[0][1] == (*m)[1][0], "a matrix came through asymmetric");

        // Lossy, so that each net's resistance is a number and not zero:
        // it is the surface resistance times the length times how its own
        // charge crowds, which is what the single line reports.
        const rf::Extraction lossy = rf::extract(meshed(pair, 60), pair, grounded(epsr, h, 0.002, 5.8e7, 3e-6));
        QVERIFY2(lossy.ok(), qPrintable(lossy.problems.join(" | ")));
        const fem::CoupledLines cl = rf::asCoupledLines(lossy, length, f);
        QVERIFY2(cl.error.isEmpty(), qPrintable(cl.error));
        const fem::LineRLGC each = rf::asLine(lossy, 0, length, f);
        QVERIFY(each.error.isEmpty());
        QVERIFY2(each.R > 0, "the fixture must lose");
        QVERIFY2(std::abs(cl.R[0][0] - each.R) < 1e-9 * each.R,
                 qPrintable(QStringLiteral("%1 Ohm/m in the matrix vs %2 of the line").arg(cl.R[0][0]).arg(each.R)));
        QVERIFY(cl.R[0][1] == 0.0);

        // Asked for no frequency at all, a lossless line still has modes:
        // its own are taken at 1 GHz, as the engine's quasi-TEM ones are,
        // since a propagation of nothing has no eigenvectors.
        const fem::CoupledLines still = rf::asCoupledLines(e, length);
        QVERIFY2(still.error.isEmpty(), qPrintable(still.error));
        QCOMPARE(int(still.modes.size()), 2);
        QCOMPARE(still.frequency, 0.0);
        QVERIFY(!still.rlgc);
        for (const fem::LineMode& m : still.modes) {
            QVERIFY2(m.gamma.imag() > 0, "a lossless mode still propagates");
            QVERIFY2(m.nEff > 1 && m.nEff < std::sqrt(epsr), qPrintable(QStringLiteral("%1").arg(m.nEff)));
            QVERIFY2(m.Z.real() > 0, "and still has an impedance");
        }
        // Its R and G are nothing, which is what "no frequency" means.
        QVERIFY(still.R[0][0] == 0.0 && still.G[0][0] == 0.0);

        // The 4-port: reciprocal, passive, and the through is bigger than
        // the coupling.
        const std::vector<std::vector<cd>> s = fem::coupledSParameters(c, length, 50.0);
        QCOMPARE(int(s.size()), 4);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                QVERIFY2(std::abs(s[std::size_t(i)][std::size_t(j)] - s[std::size_t(j)][std::size_t(i)]) < 1e-9,
                         qPrintable(QStringLiteral("S(%1,%2) is not S(%2,%1)").arg(i).arg(j)));
        for (int j = 0; j < 4; ++j) {
            double power = 0;
            for (int i = 0; i < 4; ++i) power += std::norm(s[std::size_t(i)][std::size_t(j)]);
            QVERIFY2(power <= 1.0 + 1e-6, qPrintable(QStringLiteral("port %1 gives out %2").arg(j).arg(power)));
        }
        // Port 3 is line 1's far end, port 2 is line 2's near end.
        QVERIFY2(std::abs(s[2][0]) > std::abs(s[1][0]), "more goes along a line than across to its neighbour");
        QVERIFY2(std::abs(s[1][0]) > 1e-4, "but something does cross");
    }

    // Moved apart, two lines stop coupling - in the matrix and in the
    // S-parameters both.
    void theCouplingFallsAsTheLinesAreMovedApart()
    {
        const double h = 100e-6, epsr = 9.8, length = 2e-3, f = 5e9;
        double lastCoupling = 1.0, lastTerm = 1e300;
        for (const double gap : {100.0, 400.0, 1600.0}) {
            const QList<rf::Conductor> pair{strip(0, 0, 2000, 200), strip(0, 200 + gap, 2000, 200)};
            const rf::Extraction e = rf::extract(meshed(pair, 60), pair, grounded(epsr, h));
            QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
            const fem::CoupledLines c = rf::asCoupledLines(e, length, f);
            QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
            // The off-diagonal capacitance (negative, in the Maxwell form).
            const double term = std::abs(c.C[0][1]);
            QVERIFY2(term < lastTerm, qPrintable(QStringLiteral("gap %1: C12 %2 is not below %3").arg(gap).arg(term).arg(lastTerm)));
            lastTerm = term;
            const std::vector<std::vector<cd>> s = fem::coupledSParameters(c, length, 50.0);
            const double coupling = std::abs(s[1][0]);
            QVERIFY2(coupling < lastCoupling, qPrintable(QStringLiteral("gap %1: coupled %2 is not below %3").arg(gap).arg(coupling).arg(lastCoupling)));
            lastCoupling = coupling;
        }
    }

    // Each dielectric's loss as a matrix rather than one number per net:
    // a layer's loss loads a conductor and couples two, and the coupled
    // lines need the second term.
    void theLossMatrixCouplesAsWellAsLoads()
    {
        const double h = 100e-6, epsr = 9.8, length = 2e-3, f = 20e9;
        const QList<rf::Conductor> pair{strip(0, 0, 2000, 200), strip(0, 300, 2000, 200)};
        const rf::Extraction e = rf::extract(meshed(pair, 60), pair, grounded(epsr, h, 0.002));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
        QCOMPARE(e.dielectricLoss.size(), 2);
        // The diagonal loads, and its ratio to the capacitance is the
        // effective loss tangent the single line reports.
        for (int i = 0; i < 2; ++i) {
            QVERIFY2(e.dielectricLoss.at(i).at(i) > 0, "a lossy substrate loads the conductor over it");
            QVERIFY2(e.lossTangent.at(i) > 0.3 * 0.002 && e.lossTangent.at(i) < 0.002,
                     qPrintable(QStringLiteral("net %1: tand %2").arg(i).arg(e.lossTangent.at(i))));
        }
        // And the off-diagonal couples: the term is there, it is of the
        // opposite sign to the diagonal (as the capacitance's is), and it
        // is smaller.
        const double off = e.dielectricLoss.at(0).at(1);
        QVERIFY2(off < 0, qPrintable(QStringLiteral("the coupling term is %1").arg(off)));
        QVERIFY2(std::abs(off) < e.dielectricLoss.at(0).at(0), "a pair couples less than it loads");
        QVERIFY2(std::abs(off - e.dielectricLoss.at(1).at(0)) < 1e-6 * std::abs(off), "the loss matrix is symmetric");

        // G is w times it, which the coupled lines carry across whole - of
        // the line's cross-section, per metre as C is, its ends' share not
        // in it; so near the whole over the length, but not it (the
        // coupling per metre is a little more: at the ends the field
        // goes to the ground rather than to the other line).
        const fem::CoupledLines c = rf::asCoupledLines(e, length, f);
        QVERIFY(c.error.isEmpty());
        const double omega = 2 * M_PI * f;
        QVERIFY(e.perMetre.fromMiddle);
        QCOMPARE(e.perMetre.dielectricLoss.size(), 2);
        const double offPerMetre = e.perMetre.dielectricLoss.at(0).at(1);
        QVERIFY2(std::abs(c.G[0][1] - omega * offPerMetre) < 1e-6 * std::abs(omega * offPerMetre),
                 "the conductance matrix is w times the loss matrix, per metre");
        QVERIFY2(offPerMetre < 0 && std::abs(offPerMetre - off / length) < 0.2 * std::abs(off / length)
                     && std::abs(offPerMetre - off / length) > 1e-4 * std::abs(off / length),
                 qPrintable(QStringLiteral("per metre %1, the whole over the length %2").arg(offPerMetre).arg(off / length)));
        QVERIFY2(c.G[0][1] < 0, "and its coupling term survives the crossing");

        // A stack that declares no loss has none of it.
        const rf::Extraction dry = rf::extract(meshed(pair, 60), pair, grounded(epsr, h));
        QVERIFY(dry.ok());
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) QVERIFY(dry.dielectricLoss.at(i).at(j) == 0.0);
    }

    // The vacuum matrix is kept whole, not reduced to one number a net:
    // every dielectric replaced by vacuum, so eps_eff is C/C0 and L is
    // C0^-1/c^2 for a matrix as well as for a scalar.
    void theVacuumMatrixIsKeptWhole()
    {
        const double h = 100e-6, epsr = 9.8;
        const QList<rf::Conductor> pair{strip(0, 0, 2000, 200), strip(0, 300, 2000, 200)};
        const rf::Extraction e = rf::extract(meshed(pair, 60), pair, grounded(epsr, h));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
        QCOMPARE(e.maxwell0.size(), 2);
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(e.maxwell0.at(i).size(), 2);
            // A conductor in vacuum holds less charge at a volt than the
            // same one over a dielectric, and more than nothing.
            QVERIFY(e.maxwell0.at(i).at(i) > 0);
            QVERIFY2(e.maxwell0.at(i).at(i) < e.maxwell.at(i).at(i), "vacuum holds less than alumina");
            // And it is still the mutual c0 the earlier rounds reported.
            double sum = 0;
            for (int j = 0; j < 2; ++j) sum += e.maxwell0.at(i).at(j);
            QVERIFY2(std::abs(sum - e.c0.at(i).at(i)) < 1e-12 * std::abs(sum), "c0's diagonal is maxwell0's row sum");
        }
    }

    // Where the quasi-static answer stops being the whole story, said
    // rather than assumed.
    void theQuasiStaticLimitIsSaidRatherThanAssumed()
    {
        const double h = 100e-6, epsr = 9.8;
        const QList<rf::Conductor> line{strip(0, 0, 2000, 200)};
        const rf::Mesh mesh = meshed(line, 60);
        const rf::Stack stack = grounded(epsr, h);
        const rf::QuasiStaticLimit limit = rf::quasiStaticLimit(stack, mesh, 1e-6, 1e8, 2e-3);
        // The substrate is 100 µm and the strip 200 µm wide across a
        // 2000 µm length, so the depth is not what sets it: the width is.
        QCOMPARE(limit.epsilonMax, epsr);
        QVERIFY2(std::abs(limit.transverse - 200e-6) < 1e-9, qPrintable(QStringLiteral("%1 m").arg(limit.transverse)));
        QVERIFY2(limit.what.contains(QStringLiteral("the conductor is 200")), qPrintable(limit.what));
        // Of a pair, what the two span - not the width of either (the
        // phase B check's N3: two 100 µm strips said "the widest is 250").
        {
            const QList<rf::Conductor> pair{strip(0, 0, 2000, 100), strip(0, 150, 2000, 100)};
            const rf::QuasiStaticLimit both = rf::quasiStaticLimit(stack, meshed(pair, 60), 1e-6, 1e8, 2e-3, 2);
            QVERIFY2(both.what.contains(QStringLiteral("the conductors span 250")), qPrintable(both.what));
            QVERIFY2(!both.what.contains(QStringLiteral("widest")), qPrintable(both.what));
        }
        // A tenth of a wavelength in alumina at 200 µm: about 48 GHz.
        const double wanted = rf::kLightSpeed / (10 * 200e-6 * std::sqrt(epsr));
        QVERIFY2(std::abs(limit.frequency - wanted) < 1e-6 * wanted, qPrintable(QStringLiteral("%1 Hz").arg(limit.frequency)));
        QVERIFY2(limit.frequency > 40e9 && limit.frequency < 60e9, qPrintable(QStringLiteral("%1 Hz").arg(limit.frequency)));
        // And the line's own first resonance, which is not a limit of the
        // model: 1e8 m/s over twice 2 mm is 25 GHz.
        QVERIFY2(std::abs(limit.resonance - 25e9) < 1.0, qPrintable(QStringLiteral("%1 Hz").arg(limit.resonance)));

        // A deep stack under a narrow strip: the depth sets it, and the
        // limit falls as the stack deepens.
        const QList<rf::Conductor> narrow{strip(0, 0, 2000, 20)};
        const rf::QuasiStaticLimit deep = rf::quasiStaticLimit(grounded(epsr, 500e-6), meshed(narrow, 60));
        QVERIFY2(std::abs(deep.transverse - 500e-6) < 1e-9, qPrintable(QStringLiteral("%1 m").arg(deep.transverse)));
        QVERIFY2(deep.what.contains(QStringLiteral("deep")), qPrintable(deep.what));
        QVERIFY2(deep.frequency < limit.frequency, "a deeper stack gives out sooner");
        // No length, no resonance to report.
        QCOMPARE(deep.resonance, 0.0);
    }

    // The writers the engine already has, on an RF 2.5D answer: a
    // Touchstone file of a line and of a coupled pair.
    void theEnginesWritersTakeAnRf25Answer()
    {
        const double h = 100e-6, epsr = 9.8, length = 2e-3;
        const QList<rf::Conductor> line{strip(0, 0, 2000, 200)};
        const rf::Extraction e = rf::extract(meshed(line, 60), line, grounded(epsr, h, 0.002, 5.8e7, 3e-6));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
        const QList<double> f = rf::sweepFrequencies(1e9, 10e9, 5, false);
        const fem::LineSweep sweep = rf::lineSweep(e, 0, length, f);
        QVERIFY2(sweep.error.isEmpty(), qPrintable(sweep.error));
        const QString s2p = fem::touchstone(sweep, length, 50.0, {QStringLiteral("An RF 2.5D model, quasi-static")});
        QVERIFY2(s2p.contains(QStringLiteral("# Hz S RI R 50")), qPrintable(s2p.left(400)));
        QVERIFY(s2p.contains(QStringLiteral("! An RF 2.5D model, quasi-static")));
        // One line of nine numbers a frequency: f and four complex terms.
        QStringList data;
        for (const QString& row : s2p.split(QLatin1Char('\n')))
            if (!row.trimmed().isEmpty() && !row.startsWith(QLatin1Char('!')) && !row.startsWith(QLatin1Char('#'))) data << row;
        QCOMPARE(data.size(), 5);
        for (const QString& row : data) QCOMPARE(row.split(QLatin1Char(' '), Qt::SkipEmptyParts).size(), 9);
        // The first field is the frequency it was asked for.
        QVERIFY2(std::abs(data.first().split(QLatin1Char(' '), Qt::SkipEmptyParts).first().toDouble() - 1e9) < 1.0,
                 qPrintable(data.first()));

        // And the pair as a 4-port: twenty-five numbers a frequency.
        const QList<rf::Conductor> pair{strip(0, 0, 2000, 200), strip(0, 300, 2000, 200)};
        const rf::Extraction two = rf::extract(meshed(pair, 60), pair, grounded(epsr, h, 0.002, 5.8e7, 3e-6));
        QVERIFY2(two.ok(), qPrintable(two.problems.join(" | ")));
        const fem::CoupledSweep cs = rf::coupledSweep(two, length, f);
        QVERIFY2(cs.error.isEmpty(), qPrintable(cs.error));
        QCOMPARE(int(cs.points.size()), 5);
        const QString s4p = fem::touchstoneCoupled(cs, length, 50.0, false);
        QVERIFY2(s4p.contains(QStringLiteral("# Hz S RI R 50")), qPrintable(s4p.left(400)));
        QVERIFY(!s4p.isEmpty());
    }

    // What the coupled bridge refuses.
    void whatCannotBeMadeIntoLinesIsRefused()
    {
        const QList<rf::Conductor> line{strip(0, 0, 2000, 200)};
        const rf::Extraction e = rf::extract(meshed(line, 60), line, grounded(9.8, 100e-6));
        QVERIFY(e.ok());
        // A length of nothing.
        QVERIFY(!rf::asCoupledLines(e, 0, 5e9).error.isEmpty());
        // Nothing extracted at all.
        rf::Extraction empty;
        empty.problems << QStringLiteral("nothing here");
        QVERIFY(!rf::asCoupledLines(empty, 1e-3, 5e9).error.isEmpty());
        QVERIFY(!rf::asLine(empty, 0, 1e-3, 5e9).error.isEmpty());
        QVERIFY(!rf::coupledSweep(empty, 1e-3, {5e9}).error.isEmpty());
        // A sweep of no frequencies, and of a frequency that is none.
        QVERIFY(!rf::coupledSweep(e, 1e-3, {}).error.isEmpty());
        QVERIFY(!rf::coupledSweep(e, 1e-3, {0.0}).error.isEmpty());
        // And the engine's own mode solver refusing matrices that are no
        // shape - the one guard that is this bridge's to get right.
        fem::CoupledLines bad;
        bad.names << QStringLiteral("A") << QStringLiteral("B");
        QVERIFY(!fem::coupledModes(bad, 2 * M_PI * 1e9));
        QVERIFY(!bad.error.isEmpty());
        fem::CoupledLines again = rf::asCoupledLines(e, 1e-3, 5e9);
        QVERIFY(!fem::coupledModes(again, 0));
        // Asked again with a frequency, it answers that many modes - not
        // those of the run before with these piled on top.
        const int was = int(again.modes.size());
        QVERIFY(was > 0);
        QVERIFY(fem::coupledModes(again, 2 * M_PI * 1e10));
        QCOMPARE(int(again.modes.size()), was);
    }
};

QTEST_MAIN(TestRf25Sparams)
#include "test_rf25_sparams.moc"
