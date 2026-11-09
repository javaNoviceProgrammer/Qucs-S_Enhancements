/*
 * test_fem3_wave.cpp - light through a 3D model's devices by the finite
 *                      element method in full, against what is known: a
 *                      hollow metal guide's phase, a silicon wire's through
 *                      PMLs, a slab's step against the 2D full-wave solve,
 *                      a junction and a sidewall's step against the
 *                      eigenmode expansion; the model's own way - a
 *                      Wavelength Domain step, its globals, its field, its
 *                      Touchstone file, its memory limit
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
#include <set>

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

/// A 3D model in µm, its mesh \a h at most.
Model model3d(double h)
{
    Model m = Model::blank3d();
    m.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QString::number(h, 'g', 17));
    return m;
}

/// A box from (x0, y0, z0) to (x1, y1, z1).
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

/// A model built and meshed.
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

/// The faces of \a b's geometry in the plane x = \a x.
std::vector<int> facesAt(const Model& m, const Built& b, double x)
{
    QString why;
    const QVector<int> f = resolveSelection3(*b.topology, m, Level::Boundary, QJsonObject{{QStringLiteral("plane"), QJsonObject{{QStringLiteral("x"), x}}}}, &why);
    return std::vector<int>(f.begin(), f.end());
}

/// Two ports at x = 0 and x = \a L, each taking \a modes and absorbing \a absorbed.
std::vector<Wave3Port> endPorts(const Model& m, const Built& b, double L, const QString& modes, int absorbed, bool feedBoth = true)
{
    Wave3Port p1, p2;
    p1.name = QStringLiteral("1");
    p1.faces = facesAt(m, b, 0);
    p1.modes = {modes};
    p1.absorbed = absorbed;
    p2 = p1;
    p2.name = QStringLiteral("2");
    p2.faces = facesAt(m, b, L);
    p2.excite = feedBoth;
    return {p1, p2};
}

/// The domains of objects \a names.
std::vector<int> domainsOf(const Model& m, const Built& b, const QStringList& names)
{
    QString why;
    const QVector<int> d = resolveSelection3(*b.topology, m, Level::Domain, objects(names), &why);
    return std::vector<int>(d.begin(), d.end());
}

/// A wire's junction (or a sidewall's step) in a metal window: oxide 0.6 µm
/// below and above, silicon 0.22 µm thick, widths \a w1 then \a w2 from x =
/// 0 to \a L and on to 2L; half of it (y ≥ 0, metal at y = 0: TE0's
/// tangential E is odd about it, nought there) - its even modes the whole
/// one's.
Model halfJunction(double w1, double w2, double L, double hOx, double hSi)
{
    Model m = model3d(hOx);
    const double W = 1.6, clad = 0.6;
    addBox(m, QStringLiteral("ox"), 0, 2 * L, 0, W / 2, -clad, 0.22 + clad);
    addBox(m, QStringLiteral("c1"), 0, L, 0, w1 / 2, 0, 0.22);
    addBox(m, QStringLiteral("c2"), L, 2 * L, 0, w2 / 2, 0, 0.22);
    addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("ox")});
    addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), {QStringLiteral("c1"), QStringLiteral("c2")});
    addSize(m, hSi, {QStringLiteral("c1"), QStringLiteral("c2")});
    return m;
}

Wave3Result solve(const Model& m, const Built& b, const Wave3Settings& st, double lambda = 1.55e-6)
{
    return solveWave3(m, b.parameters, *b.topology, b.mesh, st, C0 / lambda);
}

} // namespace

class TestFem3Wave : public QObject
{
    Q_OBJECT

private slots:
    // A hollow metal guide, 0.9 x 0.4 µm, 2 µm long, at 1550 nm: TE10 (E
    // vertical), β = sqrt(k0² - (π/a)²) - S21 = e^-jβL, S11 = 0. Quadratic
    // elements at 0.1 µm: the phase within 1e-4 rad (9.4e-5), nothing back
    // (4.8e-5); linear ones converge as h² (6.3e-2, 1.6e-2 at 0.2, 0.1 µm).
    // The port's mode its index to 1e-5 of the analytic one; S reciprocal.
    void aHollowGuidesPhaseConverges()
    {
        const double a = 0.9, L = 2, lambda = 1.55;
        const double k0 = 2 * M_PI / lambda, beta = std::sqrt(k0 * k0 - std::pow(M_PI / a, 2));
        auto run = [&](int order, double h, cd* s21, cd* s11, cd* s12, double* neff) {
            Model m = model3d(h);
            addBox(m, QStringLiteral("air"), 0, L, -a / 2, a / 2, 0, 0.4);
            addMaterial(m, QStringLiteral("air"), QStringLiteral("1"), {QStringLiteral("air")});
            const Built b = build(m);
            Wave3Settings st;
            st.order = order;
            st.ports = endPorts(m, b, L, QStringLiteral("1"), 6);
            const Wave3Result r = solve(m, b, st);
            QVERIFY2(r.ok(), qPrintable(r.error));
            *s21 = r.S(1, 0);
            *s11 = r.S(0, 0);
            *s12 = r.S(0, 1);
            *neff = r.portModes[0].neff.real();
        };
        const cd want = std::exp(cd(0, -beta * L));
        cd s21, s11, s12;
        double neff = 0;
        run(2, slowBuild() ? 0.2 : 0.1, &s21, &s11, &s12, &neff);
        const double err2 = std::abs(std::arg(s21 / want));
        qInfo("hollow guide, quadratic: phase error %.2e rad, |S11| %.2e, |S21| %.8f, neff %.7f (analytic %.7f)", err2, std::abs(s11), std::abs(s21), neff,
              beta / k0);
        QVERIFY(std::abs(neff - beta / k0) < 1e-5);
        QVERIFY(std::abs(s21 - s12) < 1e-8);
        QVERIFY(std::abs(std::abs(s21) - 1) < 1e-5);
        QVERIFY2(err2 < (slowBuild() ? 2e-3 : 2e-4), qPrintable(QString::number(err2)));
        QVERIFY(std::abs(s11) < (slowBuild() ? 1e-3 : 1e-4));
        if (slowBuild()) return;
        double err1[2];
        for (int k = 0; k < 2; ++k) {
            run(1, k == 0 ? 0.2 : 0.1, &s21, &s11, &s12, &neff);
            err1[k] = std::abs(std::arg(s21 / want));
        }
        qInfo("hollow guide, linear: phase error %.2e at 0.2 µm, %.2e at 0.1", err1[0], err1[1]);
        QVERIFY(err1[1] < err1[0] / 3 && err1[1] > err2 * 10);
    }

    // The hollow guide ending in a PML 0.5 µm long, one port: its TE10 goes
    // in and dies there. A PML stretched for a plane wave of k0 (R 1e-6)
    // takes a mode of β = 0.508 k0 as R^(β/k0) - some 1e-3 back; metal at
    // its end would send it all back.
    void aPmlTakesAGuidesWave()
    {
        const double a = 0.9, L = 1, d = 0.5;
        Model m = model3d(slowBuild() ? 0.2 : 0.1);
        addBox(m, QStringLiteral("air"), 0, L, -a / 2, a / 2, 0, 0.4);
        addBox(m, QStringLiteral("pml"), L, L + d, -a / 2, a / 2, 0, 0.4);
        addMaterial(m, QStringLiteral("air"), QStringLiteral("1"), {QStringLiteral("air"), QStringLiteral("pml")});
        const Built b = build(m);
        Wave3Settings st;
        st.ports = {endPorts(m, b, L, QStringLiteral("1"), 6).front()};
        st.pmlDomains = domainsOf(m, b, {QStringLiteral("pml")});
        const Wave3Result r = solve(m, b, st);
        QVERIFY2(r.ok(), qPrintable(r.error));
        qInfo("hollow guide into a PML: |S11| %.2e", std::abs(r.S(0, 0)));
        QVERIFY2(std::abs(r.S(0, 0)) < 5e-3, qPrintable(QString::number(std::abs(r.S(0, 0)))));
    }

    // A silicon wire (0.5 x 0.22 µm in oxide, 0.6 µm of it round the core)
    // in PMLs 0.4 µm thick, 1 µm long: S21 = e^-jβL with its port's index -
    // |S21| to 1e-4, the phase within 0.02 rad at this mesh (0.15 µm, the
    // core's 0.08), little back. Its ports' faces on the PMLs left out by
    // default (metal behind them), said; through them - their cuts with the
    // PMLs, their own modes crowding the search - the same S within 2e-3.
    void aWireThroughPmlsKeepsItsPhase()
    {
        if (slowBuild()) QSKIP("a sanitized build: a cut with PMLs takes minutes");
        const double L = 1, Y = 0.85, Z0 = -0.6, Z1 = 0.82, d = 0.4;
        Model m = model3d(0.15);
        addBox(m, QStringLiteral("ox"), 0, L, -Y, Y, Z0, Z1);
        addBox(m, QStringLiteral("core"), 0, L, -0.25, 0.25, 0, 0.22);
        addBox(m, QStringLiteral("p1"), 0, L, -Y - d, -Y, Z0, Z1);
        addBox(m, QStringLiteral("p2"), 0, L, Y, Y + d, Z0, Z1);
        addBox(m, QStringLiteral("p3"), 0, L, -Y - d, Y + d, Z0 - d, Z0);
        addBox(m, QStringLiteral("p4"), 0, L, -Y - d, Y + d, Z1, Z1 + d);
        const QStringList pmls{QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3"), QStringLiteral("p4")};
        addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), QStringList{QStringLiteral("ox")} + pmls);
        addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), {QStringLiteral("core")});
        addSize(m, 0.08, {QStringLiteral("core")});
        const Built b = build(m);
        Wave3Settings st;
        st.pmlDomains = domainsOf(m, b, pmls);
        QCOMPARE(int(st.pmlDomains.size()), 4);
        st.ports = endPorts(m, b, L, QStringLiteral("TE0"), 10, false);
        const Wave3Result r = solve(m, b, st);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const cd n = r.portModes[0].neff;
        const cd want = std::exp(cd(0, -2 * M_PI / 1.55 * L) * n);
        const double err = std::abs(std::arg(r.S(1, 0) / want));
        qInfo("wire in PMLs: neff %.6f, |S21| %.6f, phase error %.2e rad, |S11| %.2e", n.real(), std::abs(r.S(1, 0)), err, std::abs(r.S(0, 0)));
        QVERIFY(std::abs(n.real() - 2.4454) < 1e-3);
        QVERIFY(std::abs(std::abs(r.S(1, 0)) - 1) < 1e-4);
        QVERIFY2(err < 0.02, qPrintable(QString::number(err)));
        QVERIFY(std::abs(r.S(0, 0)) < 5e-3);
        QVERIFY(r.log.join(QLatin1Char('\n')).contains(QLatin1String("on a PML left out")));
        // Through the PMLs (their cuts coarser: their modes are many).
        st.portsThroughPmls = true;
        st.portResolution = 15;
        const Wave3Result t = solve(m, b, st);
        QVERIFY2(t.ok(), qPrintable(t.error));
        qInfo("through the PMLs: |S21 - S21'| %.2e, |S11'| %.2e", std::abs(t.S(1, 0) - r.S(1, 0)), std::abs(t.S(0, 0)));
        QVERIFY(!t.log.join(QLatin1Char('\n')).contains(QLatin1String("on a PML left out")));
        QVERIFY(std::abs(t.S(1, 0) - r.S(1, 0)) < 2e-3);
    }

    // A slab's step against the 2D full-wave solver: silicon 0.22 µm thick
    // into 0.5 µm, in oxide that absorbs beyond |z| = 0.5 µm (κ 0.3), metal
    // at ±1.5 µm, 9 µm either side, TE - a slice 0.08 µm across, metal at
    // its sides (the slab mode's E normal to them). The 2D solve's (in the
    // plane x, z, h 0.02 µm in silicon): |S21|² 0.946712, |S11|² 0.00250; this
    // one 0.946943 and 0.00245 at 0.08 µm in silicon (0.946795, 0.00252 at
    // 0.06 in a slice 0.2 across).
    void aSlabStepAgreesWithTheFullWave2D()
    {
        if (slowBuild()) QSKIP("a sanitized build: 335,000 unknowns take minutes");
        const double L = 9, W = 0.08;
        Model m = model3d(0.25);
        addBox(m, QStringLiteral("lossb"), 0, 2 * L, -W / 2, W / 2, -1.5, -0.5);
        addBox(m, QStringLiteral("losst"), 0, 2 * L, -W / 2, W / 2, 0.5, 1.5);
        addBox(m, QStringLiteral("ox"), 0, 2 * L, -W / 2, W / 2, -0.5, 0.5);
        addBox(m, QStringLiteral("c1"), 0, L, -W / 2, W / 2, -0.11, 0.11);
        addBox(m, QStringLiteral("c2"), L, 2 * L, -W / 2, W / 2, -0.25, 0.25);
        addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("ox")});
        addMaterial(m, QStringLiteral("lossy"), QStringLiteral("1.444"), {QStringLiteral("lossb"), QStringLiteral("losst")}, QStringLiteral("0.3"));
        addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), {QStringLiteral("c1"), QStringLiteral("c2")});
        addSize(m, 0.15, {QStringLiteral("ox")});
        addSize(m, 0.08, {QStringLiteral("c1"), QStringLiteral("c2")});
        const Built b = build(m);
        Wave3Settings st;
        st.ports = endPorts(m, b, 2 * L, QStringLiteral("TE0"), 20, false);
        const Wave3Result r = solve(m, b, st);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const double t = std::norm(r.S(1, 0)), back = std::norm(r.S(0, 0));
        qInfo("slab step: |S21|² %.6f (2D 0.946712), |S11|² %.5f (2D 0.00250), balance %.6f; %d unknowns", t, back, r.balance[0], r.unknowns);
        QVERIFY2(std::abs(t / 0.946712 - 1) < 1e-3, qPrintable(QString::number(t)));
        QVERIFY2(std::abs(back / 0.00250 - 1) < 0.05, qPrintable(QString::number(back)));
        // (The guides lossy - their tails in the absorber -, their modes'
        // |S|² near their power.)
        QVERIFY(r.balance[0] > 0.94 && r.balance[0] < 1);
    }

    // A wire's junction, 0.5 into 1 µm, in the eigenmode expansion's window
    // (1.6 x 1.42 µm, metal): TE0's |S21|² what the expansion's two
    // projections reach with 200 modes, 0.9006 - here 0.8999 (half the
    // geometry, 0.15 µm and 0.08 in silicon), 0.8997 and 0.8995 finer;
    // reciprocal; every propagating mode's power 1.
    void aJunctionAgreesWithTheExpansion()
    {
        if (slowBuild()) QSKIP("a sanitized build: 185,000 unknowns take minutes");
        const double L = 1.5;
        const Model m = halfJunction(0.5, 1.0, L, 0.15, 0.08);
        const Built b = build(m);
        Wave3Settings st;
        st.ports = endPorts(m, b, 2 * L, QStringLiteral("TE0"), 20);
        const Wave3Result r = solve(m, b, st);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const double t = std::norm(r.S(1, 0));
        qInfo("junction: |S21|² %.6f (the expansion 0.9006), |S11|² %.2e, |S22|² %.2e, balance %.7f %.7f, |S21 - S12| %.1e", t, std::norm(r.S(0, 0)),
              std::norm(r.S(1, 1)), r.balance[0], r.balance[1], std::abs(r.S(1, 0) - r.S(0, 1)));
        QVERIFY2(std::abs(t / 0.9006 - 1) < 2.5e-3, qPrintable(QString::number(t)));
        QVERIFY(std::abs(r.S(1, 0) - r.S(0, 1)) < 1e-9);
        QVERIFY(std::abs(r.balance[0] - 1) < 1e-5 && std::abs(r.balance[1] - 1) < 1e-5);
    }

    // A sidewall's step, 0.49 into 0.51 µm (a grating's tooth): its TE0
    // reflection |S11| 0.0030 by the expansion (converged there 0.00296 to
    // 0.00302), here 0.0032 (3.16e-3 to 3.21e-3 from 0.15 to 0.08 µm) - the
    // expansion converging slowly at a step's corner (its 2D grating's κ 4 to
    // 8 % short of the full-wave solve's); the guide without the step
    // reflects some 1e-4 at this mesh.
    void aSidewallStepReflectsAsTheExpansionSays()
    {
        if (slowBuild()) QSKIP("a sanitized build: 154,000 unknowns take minutes");
        const double L = 1.5;
        auto back = [&](double w2) {
            const Model m = halfJunction(0.49, w2, L, 0.15, 0.08);
            const Built b = build(m);
            Wave3Settings st;
            st.ports = endPorts(m, b, 2 * L, QStringLiteral("TE0"), 20, false);
            const Wave3Result r = solve(m, b, st);
            if (!r.ok()) qFatal("%s", qPrintable(r.error));
            return std::abs(r.S(0, 0));
        };
        const double step = back(0.51), none = back(0.49);
        qInfo("sidewall step: |S11| %.3e (the expansion 0.0030), without it %.2e", step, none);
        QVERIFY2(step > 0.0029 && step < 0.0035, qPrintable(QString::number(step)));
        QVERIFY(none < 3e-4);
    }

    // A crossing of two silicon wires at right angles, half of it (metal at
    // y = 0), three ports - the second wire's end facing +y among them: what
    // goes straight on, back and up the other arm, every port fed in turn.
    // Reciprocal, every mode's power 1 (lossless, its ports absorbing all
    // that propagates). At this coarse mesh some 0.75 straight on and 0.08 up
    // the arm (0.736 and 0.0835 at 0.15 µm, the core 0.08; the expansion,
    // which cannot carry light up the arm across its axis, 0.805).
    void aCrossingSendsLightUpItsArm()
    {
        if (slowBuild()) QSKIP("a sanitized build: three ports' modes and 100,000 unknowns take minutes");
        Model m = model3d(0.25);
        addBox(m, QStringLiteral("ox"), 0, 5, 0, 2.5, -0.6, 0.82);
        addBox(m, QStringLiteral("h"), 0, 5, 0, 0.25, 0, 0.22);
        addBox(m, QStringLiteral("v"), 2.25, 2.75, 0.25, 2.5, 0, 0.22);
        addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("ox")});
        addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), {QStringLiteral("h"), QStringLiteral("v")});
        addSize(m, 0.12, {QStringLiteral("h"), QStringLiteral("v")});
        const Built b = build(m);
        Wave3Settings st;
        st.ports = endPorts(m, b, 5, QStringLiteral("TE0"), 20);
        Wave3Port up = st.ports[1];
        up.name = QStringLiteral("3");
        QString why;
        const QVector<int> top = resolveSelection3(*b.topology, m, Level::Boundary, QJsonObject{{QStringLiteral("plane"), QJsonObject{{QStringLiteral("y"), 2.5}}}}, &why);
        up.faces.assign(top.begin(), top.end());
        QVERIFY(!up.faces.empty());
        st.ports.push_back(up);
        const Wave3Result r = solve(m, b, st);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(int(r.S.rows()), 3);
        qInfo("crossing: |S21|² %.4f, |S11|² %.2e, |S31|² %.4f; balance %.6f %.6f %.6f; |S31 - S13| %.1e", std::norm(r.S(1, 0)), std::norm(r.S(0, 0)),
              std::norm(r.S(2, 0)), r.balance[0], r.balance[1], r.balance[2], std::abs(r.S(2, 0) - r.S(0, 2)));
        for (int i = 0; i < 3; ++i) {
            QVERIFY(std::abs(r.balance[std::size_t(i)] - 1) < 1e-3);
            for (int j = 0; j < 3; ++j) QVERIFY(std::abs(r.S(i, j) - r.S(j, i)) < 1e-8);
        }
        QVERIFY(std::norm(r.S(1, 0)) > 0.65 && std::norm(r.S(1, 0)) < 0.8);
        QVERIFY(std::norm(r.S(2, 0)) > 0.03 && std::norm(r.S(2, 0)) < 0.15);
    }

    // The model's own way: an Electromagnetic Waves, Frequency Domain physics
    // with two Optical Ports (faces in the planes of its ends) and a
    // Wavelength Domain step of two wavelengths, solved as a 3D study is -
    // each wavelength a snapshot, its globals (S21 of a straight wire
    // e^-jβL, with its port's index there), its field |E| kept, its
    // Touchstone file; its memory estimated first and refused past a limit;
    // a mesh too coarse for the wavelength said.
    void aWavelengthStepSolvesTheFullWave()
    {
        const double L = 1;
        Model m = model3d(slowBuild() ? 0.4 : 0.25);
        addBox(m, QStringLiteral("ox"), 0, L, -0.8, 0.8, -0.6, 0.82);
        addBox(m, QStringLiteral("core"), 0, L, -0.25, 0.25, 0, 0.22);
        addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("ox")});
        addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), {QStringLiteral("core")});
        // (Its core a tenth of a micrometre, its prisms' edges to some 0.12:
        // still coarse for 1500 nm in silicon - λ0/n/4 is 108 nm -, said.)
        addSize(m, slowBuild() ? 0.15 : 0.1, {QStringLiteral("core")});
        Node ewfd = m.make(QStringLiteral("waves3"));
        QCOMPARE(ewfd.tag, QStringLiteral("ewfd"));
        ewfd.props.insert(QStringLiteral("portresolution"), QStringLiteral("15"));
        const QString tag = ewfd.tag;
        QVERIFY(m.add(m.component().tag, ewfd));
        for (int k = 0; k < 2; ++k) {
            Node port = m.make(QStringLiteral("waves3/port"));
            port.props.insert(QStringLiteral("name"), QString::number(k + 1));
            port.props.insert(QStringLiteral("absorbed"), 4);
            port.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("plane"), QJsonObject{{QStringLiteral("x"), k == 0 ? 0.0 : L}}}});
            QString why;
            QVERIFY2(m.add(tag, port, -1, &why), qPrintable(why));
        }
        for (Node& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                c.children.clear();
                Node step = m.make(QStringLiteral("wavelength"));
                step.props.insert(QStringLiteral("wavelengths"), QStringLiteral("1500[nm], 1600[nm]"));
                c.children.push_back(step);
            }
        // (Its kinds a 3D model's alone.)
        QVERIFY(nodeKind(QStringLiteral("waves3")) && (nodeKind(QStringLiteral("waves3"))->spaces & Spaces::ThreeD));
        QVERIFY(!(nodeKind(QStringLiteral("waves3"))->spaces & Spaces::TwoD));
        QVERIFY(childKinds(m.component(), Spaces::ThreeD).contains(QStringLiteral("waves3")));
        auto ps = evaluateParameters(m);
        const GeometryBuild3 g = buildGeometry3(m, *ps);
        QVERIFY(g.ok());
        // Past a limit of 1 MB: not solved, what it would take said.
        {
            SolveAsked3 asked;
            asked.limit = 1048576;
            const StudyRun3 run = runStudy3(m, ps, g.topology, nullptr, QString(), {}, asked);
            QVERIFY(!run.ok());
            QVERIFY2(run.error.contains(QLatin1String("past the limit")), qPrintable(run.error));
        }
        const StudyRun3 run = runStudy3(m, ps, g.topology, nullptr, QString(), {});
        QVERIFY2(run.ok(), qPrintable(run.error));
        QVERIFY(run.log.join(QLatin1Char('\n')).contains(QLatin1String("Estimated: ewfd")));
        QVERIFY2(run.warnings.join(QLatin1Char('\n')).contains(QLatin1String("coarse for the wavelength")), qPrintable(run.warnings.join(QLatin1Char('\n'))));
        const std::shared_ptr<Solution3> s = run.solutions.back();
        QVERIFY(s->spectral);
        QCOMPARE(s->snapshotCount(), 2);
        QCOMPARE(int(s->optics.size()), 1);
        QVERIFY(s->optics.front().fullWave);
        for (int k = 0; k < 2; ++k) {
            s->select(k);
            const double lambda = 299792458.0 / s->snapshotFrequency(k);
            const cd s21(*s->global(QStringLiteral("ewfd.S21")), *s->global(QStringLiteral("ewfd.S21_im")));
            const double n = *s->global(QStringLiteral("ewfd.neff_1_TE0"));
            const cd want = std::exp(cd(0, -2 * M_PI / lambda * 1e-6 * L * n));
            qInfo("at %.0f nm: neff %.5f, |S21| %.6f, phase error %.3f rad", lambda * 1e9, n, std::abs(s21), std::abs(std::arg(s21 / want)));
            QVERIFY(std::abs(std::abs(s21) - 1) < (slowBuild() ? 0.05 : 5e-3));
            QVERIFY(std::abs(std::arg(s21 / want)) < (slowBuild() ? 0.6 : 0.2));   // (0.11 rad at 1500 nm: a coarse mesh)
            QVERIFY(*s->global(QStringLiteral("ewfd.T")) > (slowBuild() ? 0.9 : 0.99));
            // Its field: |E| in the core, of 1 W fed - some 1e8 V/m in a
            // silicon wire.
            const Expression e = Expression::compile(QStringLiteral("ewfd.normE"), s->pointScope());
            QVERIFY(e.isValid());
            PointEvaluator3 ev(*s);
            QVERIFY(ev.at(Vec3{0.5e-6, 0, 0.11e-6}));
            const double E = ev.eval(e);
            qInfo("|E| at the core's centre %.3g V/m", E);
            QVERIFY(E > 1e7 && E < 1e9);
        }
        // Its Touchstone file: two ports, two wavelengths.
        Node ts = m.make(QStringLiteral("touchstone"));
        const Export3 out = export3(run.solutions, ts, QStringLiteral("wire"));
        QVERIFY2(out.main.error.isEmpty(), qPrintable(out.main.error));
        QCOMPARE(out.main.suffix, QStringLiteral("s2p"));
        QVERIFY(out.main.text.contains(QLatin1String("optical port 1's TE0 (neff")));
        QCOMPARE(int(out.main.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts).filter(QRegularExpression(QStringLiteral("^[0-9]"))).size()), 2);
    }
};

QTEST_GUILESS_MAIN(TestFem3Wave)
#include "test_fem3_wave.moc"
