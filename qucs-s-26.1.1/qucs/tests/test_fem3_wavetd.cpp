/*
 * test_fem3_wavetd.cpp - light through a 3D model's devices in the time
 *                        domain by the finite element method: ports with a
 *                        PML behind them (in the frequency domain too), the
 *                        steps' transforms the frequency domain's at the
 *                        frequencies asked, a guide's phase, a junction's
 *                        S against the frequency domain's
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_export.h"
#include "fem3_geometry.h"
#include "fem3_mesh.h"
#include "fem3_solver.h"
#include "fem3_wave.h"
#include "fem_solver.h"

#include <QJsonArray>
#include <QtTest>

#include <cmath>
#include <complex>

using namespace qucs_s::fem;

namespace {

using cd = std::complex<double>;
constexpr double C0 = 299792458.0;

bool slowBuild()
{
    if (qEnvironmentVariableIsSet("QUCS_TEST_SLOW")) return true;
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
    return false;
}

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
}

QJsonObject objects(const QStringList& names)
{
    QJsonArray of;
    for (const QString& n : names) of.append(n);
    return QJsonObject{{QStringLiteral("objects"), of}};
}

Model model3d(double h)
{
    Model m = Model::blank3d();
    m.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QString::number(h, 'g', 17));
    return m;
}

void addBox(Model& m, const QString& label, double x0, double x1, double y0, double y1, double z0, double z1)
{
    Node n = m.make(QStringLiteral("box"));
    n.label = label;
    n.props.insert(QStringLiteral("position"), pairOf(x0, y0));
    n.props.insert(QStringLiteral("size"), pairOf(x1 - x0, y1 - y0));
    n.props.insert(QStringLiteral("z"), QString::number(z0, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(z1 - z0, 'g', 17));
    m.add(m.geometry().tag, n);
}

void addMaterial(Model& m, const QString& label, const QString& n, const QStringList& on, const QString& kappa = {})
{
    Node mat = m.make(QStringLiteral("material"));
    mat.label = label;
    mat.props.insert(QStringLiteral("n"), n);
    if (!kappa.isEmpty()) mat.props.insert(QStringLiteral("kappa"), kappa);
    mat.props.insert(QStringLiteral("selection"), objects(on));
    m.add(m.materials().tag, mat);
}

void addSize(Model& m, double h, const QStringList& on)
{
    Node sz = m.make(QStringLiteral("mesh/size"));
    sz.props.insert(QStringLiteral("level"), QStringLiteral("domain"));
    sz.props.insert(QStringLiteral("hmax"), QString::number(h, 'g', 17));
    sz.props.insert(QStringLiteral("selection"), objects(on));
    m.add(m.mesh().tag, sz);
}

struct Built {
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<const Topology3> topology;
    std::shared_ptr<const Mesh3> mesh;
};

Built build(const Model& m)
{
    Built b;
    b.parameters = evaluateParameters(m);
    GeometryBuild3 g = buildGeometry3(m, *b.parameters);
    if (!g.ok()) qFatal("geometry: %s", qPrintable(QStringList(g.errors.values()).join(QLatin1String("; "))));
    b.topology = g.topology;
    Mesh3Build mb = buildMesh3(m, *b.parameters, g.topology);
    if (!mb.mesh) qFatal("mesh: %s", qPrintable(mb.error));
    b.mesh = mb.mesh;
    return b;
}

std::vector<int> facesAt(const Model& m, const Built& b, double x)
{
    QString why;
    const QVector<int> f = resolveSelection3(*b.topology, m, Level::Boundary, QJsonObject{{QStringLiteral("plane"), QJsonObject{{QStringLiteral("x"), x}}}}, &why);
    return std::vector<int>(f.begin(), f.end());
}

std::vector<int> domainsOf(const Model& m, const Built& b, const QStringList& names)
{
    QString why;
    const QVector<int> d = resolveSelection3(*b.topology, m, Level::Domain, objects(names), &why);
    return std::vector<int>(d.begin(), d.end());
}

std::vector<Wave3Port> ports(const Model& m, const Built& b, double x1, double x2, const QString& modes, int counted, bool feedBoth)
{
    Wave3Port p1, p2;
    p1.name = QStringLiteral("1");
    p1.faces = facesAt(m, b, x1);
    p1.modes = {modes};
    p1.absorbed = counted;
    p2 = p1;
    p2.name = QStringLiteral("2");
    p2.faces = facesAt(m, b, x2);
    p2.excite = feedBoth;
    return {p1, p2};
}

/// A hollow metal guide 0.9 x 0.4 µm from x = 0 to \a L, a PML \a d long at
/// each end - the ports on the faces between -, filled with \a n (κ \a kappa).
Model hollowGuide(double L, double d, double h, const QString& n = QStringLiteral("1"), const QString& kappa = {})
{
    Model m = model3d(h);
    const double a = 0.9;
    addBox(m, QStringLiteral("air"), 0, L, -a / 2, a / 2, 0, 0.4);
    addBox(m, QStringLiteral("pa"), -d, 0, -a / 2, a / 2, 0, 0.4);
    addBox(m, QStringLiteral("pb"), L, L + d, -a / 2, a / 2, 0, 0.4);
    addMaterial(m, QStringLiteral("air"), n, {QStringLiteral("air"), QStringLiteral("pa"), QStringLiteral("pb")}, kappa);
    return m;
}

/// Silica's index, Malitson's Sellmeier.
QString silica()
{
    return QStringLiteral("sqrt(1 + 0.6961663*(lambda0/1[um])^2/((lambda0/1[um])^2 - 0.0684043^2) + "
                          "0.4079426*(lambda0/1[um])^2/((lambda0/1[um])^2 - 0.1162414^2) + "
                          "0.8974794*(lambda0/1[um])^2/((lambda0/1[um])^2 - 9.896161^2))");
}

/// A silicon wire 0.5 x 0.22 µm in oxide (0.6 µm round it, \a w2 wide past
/// x = \a Lstep), from x = 0 to \a L, PMLs \a d thick round it and at its
/// ends - the ports on the faces between the device and the ends' PMLs.
Model pmlWire(double L, double d, double h, double hCore, double Lstep = -1, double w2 = 0.5)
{
    const double Y = 0.85, Z0 = -0.6, Z1 = 0.82;
    Model m = model3d(h);
    addBox(m, QStringLiteral("ox"), -d, L + d, -Y, Y, Z0, Z1);
    if (Lstep > 0) {
        addBox(m, QStringLiteral("core"), -d, Lstep, -0.25, 0.25, 0, 0.22);
        addBox(m, QStringLiteral("core2"), Lstep, L + d, -w2 / 2, w2 / 2, 0, 0.22);
    } else
        addBox(m, QStringLiteral("core"), -d, L + d, -0.25, 0.25, 0, 0.22);
    addBox(m, QStringLiteral("p1"), -d, L + d, -Y - d, -Y, Z0, Z1);
    addBox(m, QStringLiteral("p2"), -d, L + d, Y, Y + d, Z0, Z1);
    addBox(m, QStringLiteral("p3"), -d, L + d, -Y - d, Y + d, Z0 - d, Z0);
    addBox(m, QStringLiteral("p4"), -d, L + d, -Y - d, Y + d, Z1, Z1 + d);
    addBox(m, QStringLiteral("ea"), -d, 0, -Y - d, Y + d, Z0 - d, Z1 + d);
    addBox(m, QStringLiteral("eb"), L, L + d, -Y - d, Y + d, Z0 - d, Z1 + d);
    addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"),
                {QStringLiteral("ox"), QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3"), QStringLiteral("p4"), QStringLiteral("ea"),
                 QStringLiteral("eb")});
    QStringList cores{QStringLiteral("core")};
    if (Lstep > 0) cores << QStringLiteral("core2");
    addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), cores);
    addSize(m, hCore, cores);
    return m;
}

QStringList wirePmls()
{
    return {QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3"), QStringLiteral("p4"), QStringLiteral("ea"), QStringLiteral("eb")};
}

/// test_fem3_wave's junction - a wire 0.5 then 1 µm wide (half of it, metal
/// at y = 0 and round its window) from x = 0 to L, on to 2L - its guides
/// going on into PMLs \a d long behind its ports.
Model halfJunctionWithPmls(double L, double d, double hOx, double hSi)
{
    Model m = model3d(hOx);
    const double W = 1.6, clad = 0.6;
    addBox(m, QStringLiteral("ox"), -d, 2 * L + d, 0, W / 2, -clad, 0.22 + clad);
    addBox(m, QStringLiteral("c1"), -d, L, 0, 0.25, 0, 0.22);
    addBox(m, QStringLiteral("c2"), L, 2 * L + d, 0, 0.5, 0, 0.22);
    addBox(m, QStringLiteral("ea"), -d, 0, 0, W / 2, -clad, 0.22 + clad);
    addBox(m, QStringLiteral("eb"), 2 * L, 2 * L + d, 0, W / 2, -clad, 0.22 + clad);
    addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("ox"), QStringLiteral("ea"), QStringLiteral("eb")});
    addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), {QStringLiteral("c1"), QStringLiteral("c2")});
    addSize(m, hSi, {QStringLiteral("c1"), QStringLiteral("c2")});
    return m;
}

double maxDiff(const Eigen::MatrixXcd& a, const Eigen::MatrixXcd& b, int cols)
{
    double d = 0;
    for (int i = 0; i < a.rows(); ++i)
        for (int j = 0; j < cols; ++j) d = std::max(d, std::abs(a(i, j) - b(i, j)));
    return d;
}

} // namespace

class TestFem3WaveTd : public QObject
{
    Q_OBJECT

private slots:
    // The hollow guide with a PML behind each port, in the frequency domain:
    // each port a source (a sheet of current, its mode both ways) and a
    // monitor splitting its field into what comes and what goes - S21 =
    // e^-jβL, S11 nought but for what the PMLs send back (R^(β/k0)).
    void aPortWithAPmlBehindItSplitsItsField()
    {
        const double L = 2, d = 0.5, lambda = 1.55, a = 0.9;
        const double k0 = 2 * M_PI / lambda, beta = std::sqrt(k0 * k0 - std::pow(M_PI / a, 2));
        Model m = hollowGuide(L, d, slowBuild() ? 0.2 : 0.1);
        const Built b = build(m);
        Wave3Settings st;
        st.ports = ports(m, b, 0, L, QStringLiteral("1"), 4, true);
        st.pmlDomains = domainsOf(m, b, {QStringLiteral("pa"), QStringLiteral("pb")});
        const Wave3Result r = solveWave3(m, b.parameters, *b.topology, b.mesh, st, C0 / (lambda * 1e-6));
        QVERIFY2(r.ok(), qPrintable(r.error));
        const cd want = std::exp(cd(0, -beta * L));
        const double err = std::abs(std::arg(r.S(1, 0) / want));
        qInfo("hollow guide, PMLs behind its ports: phase error %.2e rad, |S21| %.6f, |S11| %.2e, |S12 - S21| %.2e, balance %.6f", err, std::abs(r.S(1, 0)),
              std::abs(r.S(0, 0)), std::abs(r.S(0, 1) - r.S(1, 0)), r.balance[0]);
        for (const QString& l : r.log) qInfo("  %s", qPrintable(l));
        QVERIFY2(err < (slowBuild() ? 2e-3 : 3e-4), qPrintable(QString::number(err)));
        QVERIFY(std::abs(std::abs(r.S(1, 0)) - 1) < 2e-3);
        QVERIFY(std::abs(r.S(0, 0)) < 3e-3);
        QVERIFY(std::abs(r.S(0, 1) - r.S(1, 0)) < 1e-3);
    }

    // The time domain: its transforms the frequency domain's at each
    // frequency asked - the same mesh, PMLs, ports and sources (their
    // profiles the band's middle's) -, to the transforms' truncation.
    void theStepsAreTheFrequencyDomain()
    {
        const double L = 2, d = 0.5;
        Model m = hollowGuide(L, d, slowBuild() ? 0.2 : 0.1);
        const Built b = build(m);
        Wave3Settings st;
        st.ports = ports(m, b, 0, L, QStringLiteral("1"), 4, true);
        st.pmlDomains = domainsOf(m, b, {QStringLiteral("pa"), QStringLiteral("pb")});
        // (Settled further than by default: the transforms' truncation, 2e-9,
        // below what a PML without its points' recursions moves S by, 6e-7 -
        // such a PML, damped hard, nearly as absorbing.)
        st.decay = 1e-8;
        const std::vector<double> lambdas{1.5e-6, 1.55e-6, 1.6e-6};
        std::vector<double> f;
        for (double l : lambdas) f.push_back(C0 / l);
        QElapsedTimer t;
        t.start();
        const Wave3TimeResult td = solveWave3Time(m, b.parameters, *b.topology, b.mesh, st, f);
        QVERIFY2(td.ok(), qPrintable(td.error));
        qInfo("time domain: %lld ms, %d steps, settled %d", t.elapsed(), td.steps, int(td.settled));
        for (const QString& l : td.log) qInfo("  %s", qPrintable(l));
        for (const QString& w : td.warnings) qInfo("  warning: %s", qPrintable(w));
        st.sourceFrequency = 0.5 * (f.front() + f.back());
        for (std::size_t k = 0; k < f.size(); ++k) {
            const Wave3Result fd = solveWave3(m, b.parameters, *b.topology, b.mesh, st, f[k]);
            QVERIFY2(fd.ok(), qPrintable(fd.error));
            const double diff = maxDiff(td.at[k].S, fd.S, 2);
            qInfo("%.0f nm: time domain S21 %.6f%+.6fj S11 %.2e, frequency domain S21 %.6f%+.6fj S11 %.2e; |ΔS| %.2e", lambdas[k] * 1e9, td.at[k].S(1, 0).real(),
                  td.at[k].S(1, 0).imag(), std::abs(td.at[k].S(0, 0)), fd.S(1, 0).real(), fd.S(1, 0).imag(), std::abs(fd.S(0, 0)), diff);
            QVERIFY2(diff < 1e-7, qPrintable(QString::number(diff)));
        }
    }

    // A silicon wire in PMLs, its ends' too: the time domain against the
    // frequency domain (the same ports) at 1500, 1550 and 1600 nm, and S21
    // against e^-jβL of its port's index - the frequency domain's port that
    // absorbs its modes gave 3.3e-4 rad at this mesh.
    void aWireInPmlsInTheTimeDomain()
    {
        if (slowBuild()) QSKIP("a sanitized build: 110,000 unknowns' steps take most of an hour");
        const double L = 0.6, d = 0.3;
        Model m = pmlWire(L, d, 0.25, 0.1);
        const Built b = build(m);
        Wave3Settings st;
        st.ports = ports(m, b, 0, L, QStringLiteral("TE0"), 10, true);
        st.pmlDomains = domainsOf(m, b, wirePmls());
        const std::vector<double> lambdas{1.5e-6, 1.55e-6, 1.6e-6};
        std::vector<double> f;
        for (double l : lambdas) f.push_back(C0 / l);
        const Wave3TimeResult td = solveWave3Time(m, b.parameters, *b.topology, b.mesh, st, f);
        QVERIFY2(td.ok(), qPrintable(td.error));
        for (const QString& l : td.log) qInfo("  %s", qPrintable(l));
        for (const QString& w : td.warnings) qInfo("  warning: %s", qPrintable(w));
        st.sourceFrequency = 0.5 * (f.front() + f.back());
        const Wave3Result fd = solveWave3(m, b.parameters, *b.topology, b.mesh, st, f[1]);
        QVERIFY2(fd.ok(), qPrintable(fd.error));
        for (std::size_t k = 0; k < f.size(); ++k) {
            const cd n = td.at[k].portModes[0].neff;
            const cd want = std::exp(cd(0, -2 * M_PI / (lambdas[k] * 1e6) * L) * n);
            const double err = std::abs(std::arg(td.at[k].S(1, 0) / want));
            const double diff = k == 1 ? maxDiff(td.at[k].S, fd.S, 2) : 0.0;   // (5e-8)
            qInfo("%.0f nm: neff %.6f, |S21| %.6f, phase error %.2e rad, |S11| %.2e, balance %.6f; against the frequency domain |ΔS| %.2e", lambdas[k] * 1e9,
                  n.real(), std::abs(td.at[k].S(1, 0)), err, std::abs(td.at[k].S(0, 0)), td.at[k].balance[0], diff);
            QVERIFY2(diff < 1e-6, qPrintable(QString::number(diff)));
            QVERIFY2(err < 0.02, qPrintable(QString::number(err)));
            QVERIFY(std::abs(td.at[k].S(0, 0)) < 3e-3);
        }
    }

    // The model's own way: an Electromagnetic Waves, Time Domain physics, its
    // Optical Ports on the faces between the guide and the PMLs behind them,
    // a Wavelength Domain step - one run for both wavelengths, its globals
    // (ewtd.S21, ewtd.T), its field, its Touchstone file, its estimate; a
    // port on the outside refused, as is a time domain past the memory limit.
    void aWavelengthStepSolvesTheTimeDomain()
    {
        const double L = 2, d = 0.5, a = 0.9;
        Model m = hollowGuide(L, d, slowBuild() ? 0.2 : 0.12);
        Node ewtd = m.make(QStringLiteral("waves3t"));
        QCOMPARE(ewtd.tag, QStringLiteral("ewtd"));
        ewtd.props.insert(QStringLiteral("portresolution"), QStringLiteral("15"));
        // (A 3D model's physics made of order 1: this one quadratic, as asked.)
        QCOMPARE(ewtd.text(QStringLiteral("order")), QStringLiteral("1"));
        ewtd.props.insert(QStringLiteral("order"), QStringLiteral("2"));
        const QString tag = ewtd.tag;
        QVERIFY(m.add(m.component().tag, ewtd));
        for (int k = 0; k < 2; ++k) {
            Node port = m.make(QStringLiteral("waves3t/port"));
            port.props.insert(QStringLiteral("name"), QString::number(k + 1));
            port.props.insert(QStringLiteral("absorbed"), 4);
            port.props.insert(QStringLiteral("modes"), QStringLiteral("1"));
            port.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("plane"), QJsonObject{{QStringLiteral("x"), k == 0 ? 0.0 : L}}}});
            QString why;
            QVERIFY2(m.add(tag, port, -1, &why), qPrintable(why));
        }
        Node pml = m.make(QStringLiteral("waves3t/pml"));
        pml.props.insert(QStringLiteral("selection"), objects({QStringLiteral("pa"), QStringLiteral("pb")}));
        QVERIFY(m.add(tag, pml));
        for (Node& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                c.children.clear();
                Node step = m.make(QStringLiteral("wavelength"));
                step.props.insert(QStringLiteral("wavelengths"), QStringLiteral("1500[nm], 1600[nm]"));
                c.children.push_back(step);
            }
        QVERIFY(nodeKind(QStringLiteral("waves3t")) && (nodeKind(QStringLiteral("waves3t"))->spaces & Spaces::ThreeD));
        QVERIFY(!(nodeKind(QStringLiteral("waves3t"))->spaces & Spaces::TwoD));
        auto ps = evaluateParameters(m);
        const GeometryBuild3 g = buildGeometry3(m, *ps);
        QVERIFY(g.ok());
        {
            SolveAsked3 asked;
            asked.limit = 1048576;
            const StudyRun3 run = runStudy3(m, ps, g.topology, nullptr, QString(), {}, asked);
            QVERIFY(!run.ok());
            QVERIFY2(run.error.contains(QLatin1String("past the limit")), qPrintable(run.error));
        }
        const StudyRun3 run = runStudy3(m, ps, g.topology, nullptr, QString(), {});
        QVERIFY2(run.ok(), qPrintable(run.error));
        QVERIFY(run.log.join(QLatin1Char('\n')).contains(QLatin1String("Estimated: ewtd")));
        QVERIFY2(run.log.join(QLatin1Char('\n')).contains(QLatin1String("Time-domain solve")), qPrintable(run.log.join(QLatin1Char('\n'))));
        const std::shared_ptr<Solution3> s = run.solutions.back();
        QVERIFY(s->spectral);
        QCOMPARE(s->snapshotCount(), 2);
        QCOMPARE(int(s->optics.size()), 1);
        QVERIFY(s->optics.front().fullWave);
        for (int k = 0; k < 2; ++k) {
            s->select(k);
            const double lambda = C0 / s->snapshotFrequency(k);
            const double k0 = 2 * M_PI / (lambda * 1e6), beta = std::sqrt(k0 * k0 - std::pow(M_PI / a, 2));
            const cd s21(*s->global(QStringLiteral("ewtd.S21")), *s->global(QStringLiteral("ewtd.S21_im")));
            const cd want = std::exp(cd(0, -beta * L));
            qInfo("at %.0f nm: |S21| %.6f, phase error %.2e rad, T %.6f, neff %.6f (analytic %.6f)", lambda * 1e9, std::abs(s21), std::abs(std::arg(s21 / want)),
                  *s->global(QStringLiteral("ewtd.T")), s->optics.front().results[std::size_t(k)].portModes[0].neff.real(), beta / k0);
            for (const QString& l : run.log)
                if (k == 0 && (l.contains(QLatin1String("Time-domain")) || l.contains(QLatin1String("settled")))) qInfo("  %s", qPrintable(l));
            QVERIFY(std::abs(std::abs(s21) - 1) < 2e-3);
            QVERIFY(std::abs(std::arg(s21 / want)) < (slowBuild() ? 5e-3 : 1e-3));
            QVERIFY(*s->global(QStringLiteral("ewtd.T")) > 0.995);
            // Its field: |E| in the guide, of 1 W fed - as the frequency
            // domain's, E of a 1 W mode (TE10 of 0.9 x 0.4 µm: some 4e7 V/m
            // at its peak).
            const Expression e = Expression::compile(QStringLiteral("ewtd.normE"), s->pointScope());
            QVERIFY(e.isValid());
            PointEvaluator3 ev(*s);
            QVERIFY(ev.at(Vec3{1e-6, 0, 0.2e-6}));
            const double E = ev.eval(e);
            qInfo("|E| at the guide's centre %.3g V/m", E);
            QVERIFY(E > 1e7 && E < 1e9);
        }
        Node ts = m.make(QStringLiteral("touchstone"));
        const Export3 out = export3(run.solutions, ts, QStringLiteral("guide"));
        QVERIFY2(out.main.error.isEmpty(), qPrintable(out.main.error));
        QCOMPARE(out.main.suffix, QStringLiteral("s2p"));
        // A port on the geometry's outside: refused, its PML behind asked for.
        {
            Model outer = m;
            Node* phys = outer.find(tag);
            // (The guide's far end with no PML: the port on its outer face.)
            for (Node& c : phys->children) {
                if (c.type == QLatin1String("waves3t/port") && c.text(QStringLiteral("name")) == QLatin1String("2"))
                    c.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("plane"), QJsonObject{{QStringLiteral("x"), L + d}}}});
                if (c.type == QLatin1String("waves3t/pml")) c.props.insert(QStringLiteral("selection"), objects({QStringLiteral("pa")}));
            }
            auto ps2 = evaluateParameters(outer);
            const GeometryBuild3 g2 = buildGeometry3(outer, *ps2);
            const StudyRun3 bad = runStudy3(outer, ps2, g2.topology, nullptr, QString(), {});
            QVERIFY(!bad.ok());
            qInfo("a port outside: %s", qPrintable(bad.error));
            QVERIFY2(bad.error.contains(QLatin1String("PML behind")), qPrintable(bad.error));
        }
    }

    // A dispersive material: silica's Sellmeier filling the guide, its ε
    // fitted over the band (ε∞ and a pole), and a lossy one (κ 1e-3 at every
    // wavelength: a conductivity and a pole) - the time domain against the
    // frequency domain, which takes the material's ε at each wavelength: the
    // fit's error alone between them. A material that does not vary keeps
    // its ε.
    void aDispersiveMaterialIsFitted()
    {
        const double L = 2, d = 0.5;
        const std::vector<double> lambdas{1.5e-6, 1.55e-6, 1.6e-6};
        std::vector<double> f;
        for (double l : lambdas) f.push_back(C0 / l);
        auto compare = [&](const QString& n, const QString& kappa, double tolerance, const char* what) {
            Model m = hollowGuide(L, d, slowBuild() ? 0.2 : 0.12, n, kappa);
            const Built b = build(m);
            Wave3Settings st;
            st.ports = ports(m, b, 0, L, QStringLiteral("1"), 4, false);
            st.pmlDomains = domainsOf(m, b, {QStringLiteral("pa"), QStringLiteral("pb")});
            const Wave3TimeResult td = solveWave3Time(m, b.parameters, *b.topology, b.mesh, st, f);
            QVERIFY2(td.ok(), qPrintable(td.error));
            const QString log = td.log.join(QLatin1Char('\n'));
            for (const QString& l : td.log)
                if (l.contains(QLatin1String("fitted"))) qInfo("%s: %s", what, qPrintable(l));
            QVERIFY2(log.contains(QLatin1String("fitted")), qPrintable(log));
            st.sourceFrequency = 0.5 * (f.front() + f.back());
            for (std::size_t k = 0; k < f.size(); ++k) {
                const Wave3Result fd = solveWave3(m, b.parameters, *b.topology, b.mesh, st, f[k]);
                QVERIFY2(fd.ok(), qPrintable(fd.error));
                const double diff = maxDiff(td.at[k].S, fd.S, 1);
                qInfo("%s at %.0f nm: S21 %.6f%+.6fj (|S21| %.6f), the frequency domain's %.6f%+.6fj; |ΔS| %.2e", what, lambdas[k] * 1e9, td.at[k].S(1, 0).real(),
                      td.at[k].S(1, 0).imag(), std::abs(td.at[k].S(1, 0)), fd.S(1, 0).real(), fd.S(1, 0).imag(), diff);
                QVERIFY2(diff < tolerance, qPrintable(QString::number(diff)));
            }
        };
        compare(silica(), QString(), 1e-4, "silica");
        if (slowBuild()) return;   // (a sanitized build: each case minutes)
        compare(QStringLiteral("1.444"), QStringLiteral("1e-3"), 1e-4, "lossy");
    }

    // The junction of test_fem3_wave (0.5 into 1 µm) in the time domain, its
    // ports with PMLs behind them: |S21|² as the frequency domain's ports
    // that absorb their modes gave there (0.8999 at this mesh; the expansion
    // 0.9006), reciprocal, its power all out.
    void aJunctionInTheTimeDomain()
    {
        if (slowBuild()) QSKIP("a sanitized build: 97,000 unknowns' 1,400 steps take hours");
        const double L = 1, d = 0.4;
        Model m = halfJunctionWithPmls(L, d, 0.2, 0.1);
        const Built b = build(m);
        Wave3Settings st;
        st.ports = ports(m, b, 0, 2 * L, QStringLiteral("TE0"), 10, true);
        st.pmlDomains = domainsOf(m, b, {QStringLiteral("ea"), QStringLiteral("eb")});
        const Wave3TimeResult td = solveWave3Time(m, b.parameters, *b.topology, b.mesh, st, {C0 / 1.55e-6});
        QVERIFY2(td.ok(), qPrintable(td.error));
        for (const QString& l : td.log) qInfo("  %s", qPrintable(l));
        const Wave3Result& r = td.at.front();
        const double t = std::norm(r.S(1, 0));
        qInfo("junction in the time domain: |S21|² %.6f (the frequency domain 0.8999, the expansion 0.9006), |S11|² %.2e, |S22|² %.2e, |S21 - S12| %.1e, "
              "balance %.6f %.6f",
              t, std::norm(r.S(0, 0)), std::norm(r.S(1, 1)), std::abs(r.S(1, 0) - r.S(0, 1)), r.balance[0], r.balance[1]);
        QVERIFY2(std::abs(t / 0.9006 - 1) < 4e-3, qPrintable(QString::number(t)));
        QVERIFY(std::abs(r.S(1, 0) - r.S(0, 1)) < 1e-3);
        QVERIFY(std::abs(r.balance[0] - 1) < 2e-3 && std::abs(r.balance[1] - 1) < 2e-3);
    }
};

QTEST_GUILESS_MAIN(TestFem3WaveTd)
#include "test_fem3_wavetd.moc"
