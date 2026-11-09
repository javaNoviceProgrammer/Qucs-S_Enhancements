/*
 * test_fem3_eme.cpp - light through a 3D model's waveguides by eigenmode
 *                     expansion, against what is known: a layered
 *                     geometry's cut, a silicon wire's modes, a straight
 *                     guide's transmission and phase, a junction's
 *                     reciprocity and power
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_eme.h"
#include "fem3_geometry.h"
#include "fem3_solver.h"
#include "fem_solver.h"

#include <Eigen/Dense>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include <atomic>
#include <cmath>
#include <complex>
#include <mutex>

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

/// A 3D model in µm.
Model model3d()
{
    Model m = Model::blank3d();
    m.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QStringLiteral("1"));
    return m;
}

void addBox(Model& m, const QString& label, double x, double y, double z, double w, double d, double h)
{
    Node n = m.make(QStringLiteral("box"));
    n.label = label;
    n.props.insert(QStringLiteral("position"), pairOf(x, y));
    n.props.insert(QStringLiteral("size"), pairOf(w, d));
    n.props.insert(QStringLiteral("z"), QString::number(z, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    m.add(m.geometry().tag, n);
}

void addMaterial(Model& m, const QString& label, const QString& n, const QStringList& on)
{
    Node mat = m.make(QStringLiteral("material"));
    mat.label = label;
    mat.props.insert(QStringLiteral("n"), n);
    mat.props.insert(QStringLiteral("selection"), objects(on));
    m.add(m.materials().tag, mat);
}

/// Oxide (1.444) \a clad µm below and above, y from -w/2 to w/2, along x
/// from 0 to \a length; silicon cores (3.476) 0.22 µm thick from z = 0, each
/// (x0, x1, y0, y1).
Model wire(double length, double window, const std::vector<std::array<double, 4>>& cores, double clad = 1.5)
{
    Model m = model3d();
    addBox(m, QStringLiteral("oxide"), 0, -window / 2, -clad, length, window, 2 * clad + 0.22);
    QStringList names;
    for (std::size_t i = 0; i < cores.size(); ++i) {
        const auto& c = cores[i];
        const QString name = QStringLiteral("core%1").arg(i + 1);
        addBox(m, name, c[0], c[2], 0, c[1] - c[0], c[3] - c[2], 0.22);
        names << name;
    }
    addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("oxide")});
    addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), names);
    return m;
}

struct Built {
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<Topology3> topology;
};

Built build(const Model& m)
{
    Built b;
    b.parameters = evaluateParameters(m);
    GeometryBuild3 g = buildGeometry3(m, *b.parameters);
    if (!g.ok()) qFatal("geometry: %s", qPrintable(QStringList(g.errors.values()).join(QLatin1String("; "))));
    b.topology = g.topology;
    return b;
}

/// A device along x over the whole window, its ports at its two ends.
EmeDevice deviceOf(const Topology3& t, int modes, double resolution)
{
    EmeDevice d;
    d.unitScale = t.unitScale;
    d.s0 = t.bounds.x0;
    d.s1 = t.bounds.x1;
    d.u0 = t.bounds.y0;
    d.u1 = t.bounds.y1;
    d.z0 = t.bounds.z0;
    d.z1 = t.bounds.z1;
    d.settings.modes = modes;
    d.settings.resolution = resolution;
    d.maxCell = 0.5;
    EmePort a, b;
    a.name = QStringLiteral("1");
    b.name = QStringLiteral("2");
    b.atEnd = true;
    d.ports = {a, b};
    return d;
}

/// A slab's TE0 (E along it) effective index: its transcendental equation.
double slabTE0(double n1, double n2, double d, double lambda)
{
    const double k0 = 2 * M_PI / lambda;
    double lo = n2 + 1e-12, hi = n1 - 1e-12;
    for (int i = 0; i < 200; ++i) {
        const double n = (lo + hi) / 2;
        const double kx = k0 * std::sqrt(n1 * n1 - n * n), g = k0 * std::sqrt(n * n - n2 * n2);
        // (tan(kx d/2) - g/kx falls as n rises; past π/2, n is below the first's.)
        if (kx * d / 2 >= M_PI / 2 || std::tan(kx * d / 2) - g / kx > 0) lo = n;
        else hi = n;
    }
    return (lo + hi) / 2;
}

/// The reflection and transmission of \a layers (index, thickness) between
/// media \a n0 and \a ns at \a lambda, at normal incidence: the transfer
/// (characteristic) matrices' product, each layer [cos φ, j sin φ / n; j n
/// sin φ, cos φ], φ = k0 n d.
void transferMatrix(const std::vector<std::pair<double, double>>& layers, double n0, double ns, double lambda, cd* r, cd* t)
{
    const double k0 = 2 * M_PI / lambda;
    cd m11 = 1, m12 = 0, m21 = 0, m22 = 1;
    for (const auto& [n, d] : layers) {
        const double ph = k0 * n * d;
        const cd a11 = std::cos(ph), a12 = cd(0, 1) * std::sin(ph) / n, a21 = cd(0, 1) * n * std::sin(ph), a22 = std::cos(ph);
        const cd b11 = m11 * a11 + m12 * a21, b12 = m11 * a12 + m12 * a22, b21 = m21 * a11 + m22 * a21, b22 = m21 * a12 + m22 * a22;
        m11 = b11;
        m12 = b12;
        m21 = b21;
        m22 = b22;
    }
    const cd den = n0 * m11 + n0 * ns * m12 + m21 + ns * m22;
    *r = (n0 * m11 + n0 * ns * m12 - m21 - ns * m22) / den;
    *t = 2.0 * n0 / den;
}

} // namespace

class TestFem3Eme : public QObject
{
    Q_OBJECT

private slots:
    // The cut of a wire in oxide at its middle: five rectangles - the
    // oxide below and above across the window, the core between two pieces
    // of oxide -, each of its 3D domain; the same cut further along alike,
    // one past the core's end not.
    void aLayeredGeometryIsCutIntoRectangles()
    {
        const Model m = wire(10, 4, {{{0, 6, -0.25, 0.25}}});
        const Built b = build(m);
        QString why;
        const CrossSection cs = cutAcross(*b.topology, QPointF(3, 0), QPointF(0, 1), -2, 2, -1.5, 1.72, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(int(cs.rects.size()), 5);
        int core = -1;
        for (const auto& r : cs.rects)
            if (std::abs(r.u0 + 0.25) < 1e-12 && std::abs(r.u1 - 0.25) < 1e-12) core = r.domain;
        QVERIFY(core >= 0);
        for (const auto& r : cs.rects) {
            if (r.domain == core) {
                QCOMPARE(r.z0, 0.0);
                QVERIFY(std::abs(r.z1 - 0.22) < 1e-12);
            } else {
                QVERIFY(r.domain != core);
            }
        }
        const CrossSection further = cutAcross(*b.topology, QPointF(5, 0), QPointF(0, 1), -2, 2, -1.5, 1.72, &why);
        QVERIFY(cs.sameAs(further, 1e-9));
        const CrossSection past = cutAcross(*b.topology, QPointF(8, 0), QPointF(0, 1), -2, 2, -1.5, 1.72, &why);
        QVERIFY(!cs.sameAs(past, 1e-9));
        QCOMPARE(int(past.rects.size()), 1);
        // Outside every solid: said.
        const CrossSection out = cutAcross(*b.topology, QPointF(3, 0), QPointF(0, 1), -3, 2, -1.5, 1.72, &why);
        QVERIFY(out.rects.empty());
        QVERIFY(why.contains(QLatin1String("outside")));
        // Through a core's very middle - on the diagonal its rectangle's two
        // triangles share, where rounding once put the point in neither (a
        // third of these cuts found "outside the solids") -: the core.
        for (double length : {1.7, 4.1, 9.9, 10.0, 12.7, 15.0})
            for (double width : {0.3, 0.45, 0.7})
                for (double x0 : {0.0, 0.1}) {
                    const Model mc = wire(x0 + length + 1, 4, {{{x0, x0 + length, -width / 2, width / 2}}});
                    const Built bc = build(mc);
                    QString whyNot;
                    const CrossSection c = cutAcross(*bc.topology, QPointF(x0 + length / 2, 0), QPointF(0, 1), -2, 2, -1.5, 1.72, &whyNot);
                    bool found = false;
                    for (const auto& r : c.rects) found = found || (std::abs(r.u1 - r.u0 - width) < 1e-9 && r.z0 == 0);
                    QVERIFY2(whyNot.isEmpty() && found,
                             qPrintable(QStringLiteral("a core %1 long, %2 wide, from %3: %4").arg(length).arg(width).arg(x0).arg(whyNot)));
                }
    }

    // A silicon wire's modes: TE0 and TM0 by their TE fraction, its index
    // the 2D mode solver's on the same cross-section; normalized so that the
    // overlaps of the modes with each other are the identity (the
    // bi-orthogonality mode matching stands on).
    void aWiresModesAreBiorthogonal()
    {
        const Model m = wire(10, 4, {{{0, 10, -0.25, 0.25}}});
        const Built b = build(m);
        QString why;
        // (A sanitized build: fewer modes, a coarser mesh, a smaller window -
        // its paths, not its accuracy.)
        const double half = slowBuild() ? 1.2 : 2, below = slowBuild() ? -0.8 : -1.5, above = slowBuild() ? 1.02 : 1.72;
        const CrossSection cs = cutAcross(*b.topology, QPointF(5, 0), QPointF(0, 1), -half, half, below, above, &why);
        EmeSettings st;
        st.modes = slowBuild() ? 6 : 12;
        st.resolution = slowBuild() ? 6 : 15;
        QElapsedTimer t;
        t.start();
        const EmeModes modes = solveSectionModes(m, b.parameters, *b.topology, cs, C0 / 1.55e-6, st, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(modes.count(), st.modes);
        qInfo("wire: TE0 %.6f, %s %.6f, %s %.6f (%lld ms)", modes.neff(0).real(), qPrintable(modes.label(1)), modes.neff(1).real(),
              qPrintable(modes.label(2)), modes.neff(2).real(), t.elapsed());
        QCOMPARE(modes.label(0), QStringLiteral("TE0"));
        QCOMPARE(modes.find(QStringLiteral("TE0")), 0);
        QVERIFY(modes.find(QStringLiteral("TM0")) > 0);
        // (The 2D solver's own on this window, h 0.02 µm in the core: 2.445409.)
        QVERIFY2(std::abs(modes.neff(0).real() - 2.44541) < (slowBuild() ? 2e-3 : 2e-4), qPrintable(QString::number(modes.neff(0).real(), 'g', 10)));
        const Eigen::MatrixXcd O = modeOverlap(modes, modes);
        const double off = (O - Eigen::MatrixXcd::Identity(st.modes, st.modes)).cwiseAbs().maxCoeff();
        qInfo("self-overlap: largest departure from the identity %.3g", off);
        QVERIFY2(off < 1e-8, qPrintable(QString::number(off)));
    }

    // A straight wire: one cell, no interface - S21 = e^-jβL, |S21| = 1,
    // S11 = 0 - and its ports' modes alike at both ends, so S21's phase is
    // the mode's own.
    void aStraightGuideTransmitsAll()
    {
        const double L = 10;
        const Model m = wire(L, 4, {{{0, L, -0.25, 0.25}}});
        const Built b = build(m);
        const EmeDevice d = deviceOf(*b.topology, 10, slowBuild() ? 8 : 12);
        const EmeResult r = solveEme(m, b.parameters, *b.topology, d, C0 / 1.55e-6);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(int(r.cells.size()), 1);
        QCOMPARE(int(r.portModes.size()), 2);
        const cd s21 = r.S(1, 0), s11 = r.S(0, 0), s12 = r.S(0, 1);
        const cd beta = 2 * M_PI / 1.55e-6 * r.portModes[0].neff;
        const cd expected = std::exp(-cd(0, 1) * beta * (L * 1e-6));
        qInfo("straight: S21 %.9f%+.9fi (e^-jβL %.9f%+.9fi), |S11| %.3g", s21.real(), s21.imag(), expected.real(), expected.imag(), std::abs(s11));
        QVERIFY(std::abs(s21 - expected) < 1e-9);
        QVERIFY(std::abs(s12 - s21) < 1e-12);
        QVERIFY(std::abs(s11) < 1e-12);
        QVERIFY(std::abs(r.balance[0] - 1) < 1e-9);
    }

    // A junction of a 0.5 µm wire into a 1 µm one, in a window 2.5 µm wide
    // with 0.8 µm of oxide below and above (some 30 propagating modes):
    // reciprocal (S21 = S12, to the overlaps' rounding), no more power out
    // than in, most of it through, a little back; the same with twice the
    // modes to a few parts in 10³.
    void aJunctionIsReciprocalAndKeepsPower()
    {
        const Model m = wire(4, 2.5, {{{0, 2, -0.25, 0.25}}, {{2, 4, -0.5, 0.5}}}, 0.8);
        const Built b = build(m);
        std::vector<cd> s21s;
        // (A sanitized build: one solve of 10 modes on a coarser mesh - its
        // reciprocity and power, not its convergence. The overlaps grow as
        // the modes squared: 20 took 234 s of a function's 300 there.)
        const std::vector<int> counts = slowBuild() ? std::vector<int>{10} : std::vector<int>{40, 80};
        for (int modes : counts) {
            EmeDevice d = deviceOf(*b.topology, modes, slowBuild() ? 6 : 12);
            if (slowBuild()) {
                // (And a smaller window: the 1 µm wire's mode is nought 0.5 µm off it.)
                d.u0 = -1.0;
                d.u1 = 1.0;
                d.z0 = -0.5;
                d.z1 = 0.72;
            }
            QElapsedTimer t;
            t.start();
            const EmeResult r = solveEme(m, b.parameters, *b.topology, d, C0 / 1.55e-6);
            QVERIFY2(r.ok(), qPrintable(r.error));
            QCOMPARE(int(r.cells.size()), 2);
            const cd s21 = r.S(1, 0), s12 = r.S(0, 1), s11 = r.S(0, 0), s22 = r.S(1, 1);
            qInfo("junction, %d modes: |S21|² %.6f, |S12|² %.6f, |S11|² %.3g, |S22|² %.3g, balance %.6f / %.6f, |S21 - S12| %.3g (%lld ms)",
                  modes, std::norm(s21), std::norm(s12), std::norm(s11), std::norm(s22), r.balance[0], r.balance[1], std::abs(s21 - s12),
                  t.elapsed());
            QVERIFY(std::abs(s21 - s12) < 1e-6);
            QVERIFY(r.balance[0] <= 1 + 1e-6 && r.balance[1] <= 1 + 1e-6);
            QVERIFY(std::norm(s21) > 0.85 && std::norm(s21) < 0.97);
            QVERIFY(std::norm(s11) < 0.01);
            s21s.push_back(s21);
        }
        if (s21s.size() == 2) QVERIFY2(std::abs(s21s[1] - s21s[0]) < 4e-3, qPrintable(QString::number(std::abs(s21s[1] - s21s[0]))));
    }

    // A wire's junction (0.5 into 1 µm) in a window 1.6 µm wide with 0.6 µm
    // of oxide below and above: E's continuity projected on the right's h
    // and H's on the left's e, or the other way, converge to the same TE0
    // |T|² slowly - its cut's corners (0.9119 and 0.8924 with 40 modes, 0.9062
    // and 0.8953 with 100, 0.9004 and 0.9007 with 200) -, their mean fast
    // (0.9021, 0.9008, 0.9006). With 40 modes the mean within 0.25 % of what
    // both reach with 200, either alone not.
    void theMeanOfTheTwoProjectionsConverges()
    {
        if (slowBuild()) QSKIP("a sanitized build: two cuts of 40 modes on a fine mesh take minutes");
        const double W = 1.6, clad = 0.6, limit = 0.9006;
        const Model m = wire(4, W, {{{0, 2, -0.25, 0.25}}, {{2, 4, -0.5, 0.5}}}, clad);
        const Built b = build(m);
        QString why;
        const CrossSection ca = cutAcross(*b.topology, QPointF(1, 0), QPointF(0, 1), -W / 2, W / 2, -clad, clad + 0.22, &why);
        const CrossSection cb = cutAcross(*b.topology, QPointF(3, 0), QPointF(0, 1), -W / 2, W / 2, -clad, clad + 0.22, &why);
        EmeSettings st;
        st.modes = 40;
        st.resolution = 10;
        const EmeModes ma = solveSectionModes(m, b.parameters, *b.topology, ca, C0 / 1.55e-6, st, &why);
        const EmeModes mb = solveSectionModes(m, b.parameters, *b.topology, cb, C0 / 1.55e-6, st, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        Eigen::MatrixXcd ab, ba;
        modeOverlaps(ma, mb, &ab, &ba);
        const int ta = ma.find(QStringLiteral("TE0")), tb = mb.find(QStringLiteral("TE0"));
        const double mean = std::norm(junctionS(ab, ba).s21(tb, ta)), one = std::norm(interfaceS(ab).s21(tb, ta)),
                     other = std::norm(interfaceS(ba).s12(tb, ta));
        qInfo("TE0 |T|² with 40 modes: %.5f and %.5f by each projection, %.5f their mean; both with 200: %.4f", one, other, mean, limit);
        QVERIFY2(std::abs(mean / limit - 1) < 2.5e-3, qPrintable(QString::number(mean)));
        QVERIFY(std::abs(one / limit - 1) > 5e-3 && std::abs(other / limit - 1) > 5e-3);
    }

    // The model's own way: an Electromagnetic Waves, Eigenmode Expansion
    // physics with two Optical Ports and a Wavelength Domain step of two
    // wavelengths, solved as a 3D study is - each wavelength a snapshot, its
    // S-parameters globals (S21 of a straight wire e^-jβL, the index the
    // material's dispersion gives at each), the results kept for export.
    void aWavelengthStepSolvesTheExpansion()
    {
        const double L = 6;
        Model m = wire(L, 3, {{{0, L, -0.25, 0.25}}}, 1.0);
        // Silica's own dispersion (Malitson's Sellmeier) for the oxide.
        for (Node& mat : m.materials().children)
            if (mat.label == QLatin1String("oxide"))
                mat.props.insert(QStringLiteral("n"), QStringLiteral("sqrt(1 + 0.6961663*(lambda0/1[um])^2/((lambda0/1[um])^2 - 0.0684043^2) + "
                                                                     "0.4079426*(lambda0/1[um])^2/((lambda0/1[um])^2 - 0.1162414^2) + "
                                                                     "0.8974794*(lambda0/1[um])^2/((lambda0/1[um])^2 - 9.896161^2))"));
        Node eme = m.make(QStringLiteral("eme"));
        eme.props.insert(QStringLiteral("modes"), slowBuild() ? 6 : 12);
        eme.props.insert(QStringLiteral("resolution"), slowBuild() ? QStringLiteral("6") : QStringLiteral("12"));
        const QString tag = eme.tag;
        QVERIFY(m.add(m.component().tag, eme));
        for (const char* end : {"start", "end"}) {
            Node port = m.make(QStringLiteral("eme/port"));
            port.props.insert(QStringLiteral("name"), QString::fromLatin1(end) == QLatin1String("start") ? QStringLiteral("1") : QStringLiteral("2"));
            port.props.insert(QStringLiteral("end"), QString::fromLatin1(end));
            port.props.insert(QStringLiteral("modes"), QStringLiteral("TE0, TM0"));
            QString why;
            QVERIFY2(m.add(tag, port, -1, &why), qPrintable(why));
        }
        // The study: a Wavelength Domain step in place of the Stationary one.
        for (Node& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                c.children.clear();
                Node step = m.make(QStringLiteral("wavelength"));
                step.props.insert(QStringLiteral("wavelengths"), QStringLiteral("1500[nm], 1600[nm]"));
                c.children.push_back(step);
            }
        auto ps = evaluateParameters(m);
        const GeometryBuild3 g = buildGeometry3(m, *ps);
        QVERIFY(g.ok());
        const StudyRun3 run = runStudy3(m, ps, g.topology, nullptr);
        QVERIFY2(run.ok(), qPrintable(run.error));
        const std::shared_ptr<Solution3> s = run.solutions.back();
        QVERIFY(s->spectral);
        QCOMPARE(s->snapshotCount(), 2);
        QCOMPARE(int(s->optics.size()), 1);
        QCOMPARE(int(s->optics.front().results.size()), 2);
        double neff[2];
        for (int k = 0; k < 2; ++k) {
            s->select(k);
            const double lambda = C0 / s->snapshotFrequency(k);
            QVERIFY(std::abs(lambda - (k == 0 ? 1.5e-6 : 1.6e-6)) < 1e-15);
            QVERIFY(std::abs(*s->global(tag + QStringLiteral(".lambda0")) - lambda) < 1e-15);
            neff[k] = *s->global(tag + QStringLiteral(".neff_1_TE0"));
            // Four S-parameter ports: 1 TE0, 2 TM0 at the start, 3 TE0, 4 TM0 at the end.
            const cd s31(*s->global(tag + QStringLiteral(".S31")), *s->global(tag + QStringLiteral(".S31_im")));
            const cd expected = std::exp(-cd(0, 1) * (2 * M_PI / lambda * neff[k]) * (L * 1e-6));
            qInfo("λ %.0f nm: TE0 %.6f, TM0 %.6f; S31 %.6f%+.6fi (e^-jβL %.6f%+.6fi); S41 %.2g, T %.9f", lambda * 1e9, neff[k],
                  *s->global(tag + QStringLiteral(".neff_1_TM0")), s31.real(), s31.imag(), expected.real(), expected.imag(),
                  std::hypot(*s->global(tag + QStringLiteral(".S41")), *s->global(tag + QStringLiteral(".S41_im"))),
                  *s->global(tag + QStringLiteral(".T")));
            QVERIFY(std::abs(s31 - expected) < 1e-6);
            QVERIFY(std::abs(*s->global(tag + QStringLiteral(".T")) - 1) < 1e-9);
            QVERIFY(std::abs(*s->global(tag + QStringLiteral(".S41_abs"))) < 1e-9);
        }
        // A silicon wire's index falls with the wavelength (its dispersion
        // and the oxide's).
        QVERIFY(neff[1] < neff[0]);
    }

    // A directional coupler of two silicon wires 0.2 µm apart: guide 1 from
    // the start, guide 2 to the end, side by side over L. Coupled-mode
    // theory's two supermodes give its coupling length, L_c = λ/2(n_e - n_o)
    // (37.75 µm here); the expansion carries its input into guide 2 all but
    // entirely at L_c (0.9994 with 40 modes), half of it at L_c/2 - the
    // abrupt start and end of a guide beside the other its departures from
    // sin²(πL/2L_c).
    void aCouplerCouplesOverItsCouplingLength()
    {
        if (slowBuild()) QSKIP("a sanitized build: three couplers of 40 modes take minutes");
        const double g = 0.2, w = 0.5, W = 3, clad = 1.0, y1 = -g / 2 - w, y2 = g / 2;
        // The coupled section's supermodes.
        double Lc = 0;
        {
            const Model m = wire(1, W, {{{0, 1, y1, y1 + w}}, {{0, 1, y2, y2 + w}}}, clad);
            const Built b = build(m);
            QString why;
            const CrossSection cs = cutAcross(*b.topology, QPointF(0.5, 0), QPointF(0, 1), -W / 2, W / 2, -clad, clad + 0.22, &why);
            EmeSettings st;
            st.modes = 6;
            st.resolution = slowBuild() ? 8 : 12;
            const EmeModes md = solveSectionModes(m, b.parameters, *b.topology, cs, C0 / 1.55e-6, st, &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
            const int e = md.find(QStringLiteral("TE0")), o = md.find(QStringLiteral("TE1"));
            QVERIFY(e >= 0 && o >= 0);
            Lc = 1.55 / (2 * (md.neff(e).real() - md.neff(o).real()));
            qInfo("coupled section: even %.6f, odd %.6f - L_c %.4f µm", md.neff(e).real(), md.neff(o).real(), Lc);
        }
        auto cross = [&](double L) {
            const double a = 1.5;
            const Model m = wire(2 * a + L, W, {{{0, a + L, y1, y1 + w}}, {{a, 2 * a + L, y2, y2 + w}}}, clad);
            const Built b = build(m);
            const EmeDevice d = deviceOf(*b.topology, slowBuild() ? 20 : 40, slowBuild() ? 8 : 12);
            const EmeResult r = solveEme(m, b.parameters, *b.topology, d, C0 / 1.55e-6);
            if (!r.ok()) qFatal("%s", qPrintable(r.error));
            return r;
        };
        const EmeResult full = cross(Lc), half = cross(Lc / 2), over = cross(1.2 * Lc);
        qInfo("into guide 2: %.5f at L_c, %.5f at L_c/2, %.5f at 1.2 L_c (sin² %.5f); back %.2g", std::norm(full.S(1, 0)), std::norm(half.S(1, 0)),
              std::norm(over.S(1, 0)), std::pow(std::sin(M_PI * 1.2 / 2), 2), std::norm(full.S(0, 0)));
        QVERIFY(std::norm(full.S(1, 0)) > 0.995);
        QVERIFY(std::norm(half.S(1, 0)) > 0.45 && std::norm(half.S(1, 0)) < 0.6);
        QVERIFY(std::norm(over.S(1, 0)) < std::norm(full.S(1, 0)));
        QVERIFY(std::abs(full.S(1, 0) - full.S(0, 1)) < 1e-9);
    }

    // A 1x2 MMI in silicon, 3 µm wide, fed in its middle, its outputs 0.5 µm
    // wires at ±W/4, each taken by a port in a box of its half: Soldano and
    // Pennings' self-imaging puts two images at 3 L_π/8 (L_π = π/(β0 - β1),
    // its two lowest modes' beat length). The split even (its symmetry), the
    // ports reciprocal, little back; at 0.9 x 3L_π/8 most of the light out
    // (0.811 with these 40 modes in a 4.5 µm window, 0.825 with 90, 0.835
    // with 130 in a 6 µm one), at 1.1 x much less (0.411, 0.416, 0.485) -
    // what holds whatever the window and the modes; where between the peak
    // lies moves with them (the write-up's table).
    void anMmiSplitsAtItsSelfImagingLength()
    {
        if (slowBuild()) QSKIP("a sanitized build: three MMIs of 40 modes take minutes");
        const double Wm = 3.0, wg = 0.5, Wwin = 4.5, clad = 1.0;
        double Lpi = 0;
        {
            const Model m = wire(1, Wwin, {{{0, 1, -Wm / 2, Wm / 2}}}, clad);
            const Built b = build(m);
            QString why;
            const CrossSection cs = cutAcross(*b.topology, QPointF(0.5, 0), QPointF(0, 1), -Wwin / 2, Wwin / 2, -clad, clad + 0.22, &why);
            EmeSettings st;
            st.modes = 8;
            st.resolution = 12;
            const EmeModes md = solveSectionModes(m, b.parameters, *b.topology, cs, C0 / 1.55e-6, st, &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
            Lpi = M_PI / (md.beta[std::size_t(md.find(QStringLiteral("TE0")))].real() - md.beta[std::size_t(md.find(QStringLiteral("TE1")))].real()) * 1e6;
        }
        auto run = [&](double L) {
            const double a = 1.5;
            const Model m = wire(2 * a + L, Wwin, {{{0, a, -wg / 2, wg / 2}}, {{a, a + L, -Wm / 2, Wm / 2}}, {{a + L, 2 * a + L, Wm / 4 - wg / 2, Wm / 4 + wg / 2}},
                                                    {{a + L, 2 * a + L, -Wm / 4 - wg / 2, -Wm / 4 + wg / 2}}},
                                 clad);
            const Built b = build(m);
            EmeDevice d = deviceOf(*b.topology, 40, 12);
            EmePort p3 = d.ports[1];
            d.ports[1].name = QStringLiteral("2");
            d.ports[1].whole = false;
            d.ports[1].u0 = 0;
            d.ports[1].u1 = Wwin / 2;
            d.ports[1].z0 = -clad;
            d.ports[1].z1 = clad + 0.22;
            p3 = d.ports[1];
            p3.name = QStringLiteral("3");
            p3.u0 = -Wwin / 2;
            p3.u1 = 0;
            d.ports.push_back(p3);
            const EmeResult r = solveEme(m, b.parameters, *b.topology, d, C0 / 1.55e-6);
            if (!r.ok()) qFatal("%s", qPrintable(r.error));
            return r;
        };
        double out[2];
        int k = 0;
        QVERIFY2(std::abs(Lpi - 22.183) < 0.01, qPrintable(QString::number(Lpi)));
        for (double f : {0.90, 1.10}) {
            const EmeResult r = run(f * 3 * Lpi / 8);
            out[k++] = std::norm(r.S(1, 0)) + std::norm(r.S(2, 0));
            qInfo("L %.3f µm (%.2f x 3L_π/8, L_π %.3f): |S21|² %.5f |S31|² %.5f, out %.4f, back %.2g", f * 3 * Lpi / 8, f, Lpi, std::norm(r.S(1, 0)),
                  std::norm(r.S(2, 0)), out[k - 1], std::norm(r.S(0, 0)));
            // (Even to the meshes' own asymmetry: 1e-5.)
            QVERIFY(std::abs(std::norm(r.S(1, 0)) - std::norm(r.S(2, 0))) < 1e-4);
            QVERIFY(std::abs(r.S(1, 0) - r.S(0, 1)) < 1e-9);
            QVERIFY(std::norm(r.S(0, 0)) < 0.05);
        }
        QVERIFY2(out[0] > 0.75 && out[0] < 0.9, qPrintable(QString::number(out[0])));
        QVERIFY2(out[1] < out[0] - 0.15, qPrintable(QString::number(out[1])));
    }

    // A bend's cut against a ring: a silicon wire's cross-section 2 µm from
    // an axis, its whispering-gallery mode of order 19 by the axisymmetric
    // eigenfrequency solver (its fields e^-jmφ: f_m, Q), and the 2D mode
    // solver's bend of radius 2 µm at f_m (its cross-section transformed):
    // β R = m - Re(neff) k0 R 18.99996 -, and its radiation Im(neff) the
    // ring's n_g/2Q within 2 % (5.016e-6 against 5.102e-6). Both with PMLs.
    void aBendsModeIsTheRingsWhisperingGalleryMode()
    {
        if (slowBuild()) QSKIP("a sanitized build: the ring's and the bend's fine meshes take minutes");
        const double R = 2;
        const int m = 19;
        auto run2d = [](const QString& json) -> std::shared_ptr<Solution> {
            QString why;
            std::optional<Model> model = Model::fromJson(json.toUtf8(), &why);
            if (!model) qFatal("%s", qPrintable(why));
            auto ps = evaluateParameters(*model);
            const GeometryBuild g = buildGeometry(*model, *ps);
            StudyRun r = runStudy(*model, ps, g.topology, nullptr, QString(), {});
            if (!r.ok()) qFatal("%s", qPrintable(r.error));
            auto s = r.solutions.back();
            s->select(0);
            return s;
        };
        const QString common = QStringLiteral(R"("geometry": [{"type": "rectangle", "name": "pml", "position": ["%1", "-2"], "size": ["4", "4.22"]},
                     {"type": "rectangle", "name": "oxide", "position": ["%1", "-1.5"], "size": ["3", "3.22"]},
                     {"type": "rectangle", "name": "core", "position": ["%2", "0"], "size": ["0.5", "0.22"]}],
        "materials": [{"label": "oxide", "n": "1.444", "selection": {"only": ["pml", "oxide"]}},
                      {"label": "silicon", "n": "3.476", "selection": {"only": ["core"]}}],
        "mesh": {"size": "normal", "custom": true, "hmax": "0.12",
                 "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "0.03"}]})");
        const auto ring = run2d(QStringLiteral(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "um", "space": "axisymmetric", %1,
            "physics": [{"type": "resonator", "tag": "emr", "m": %2, "features": [{"type": "wave", "selection": {"all": true}},
                                                                                  {"type": "pml", "selection": {"only": ["pml"]}}]}]}],
            "studies": [{"type": "study", "steps": [{"type": "eigenfrequency", "count": 1, "around": "1.55[um]"}]}]})json")
                                    .arg(common.arg(R - 1.5).arg(R - 0.25))
                                    .arg(m));
        const double f = *ring->global(QStringLiteral("emr.freq")), Q = *ring->global(QStringLiteral("emr.Q"));
        auto bend = [&](double freq, double* imag) {
            const auto s = run2d(QStringLiteral(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "um", %1,
                "physics": [{"type": "modes", "tag": "emw", "bendradius": "%2", "features": [{"type": "pml", "selection": {"only": ["pml"]}}]}]}],
                "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "%3[Hz]", "modes": 1, "neff": "2.6", "polarization": "te"}]}]})json")
                                     .arg(common.arg(-1.5).arg(-0.25))
                                     .arg(R)
                                     .arg(freq, 0, 'g', 17));
            *imag = std::abs(*s->global(QStringLiteral("emw.neff_im")));
            return *s->global(QStringLiteral("emw.neff"));
        };
        double im = 0, imA = 0, imB = 0;
        const double n = bend(f, &im), df = f * 1e-3, na = bend(f - df, &imA), nb = bend(f + df, &imB);
        const double ng = n + f * (nb - na) / (2 * df), mFound = n * 2 * M_PI * f / C0 * R * 1e-6;
        qInfo("ring R %.1f µm, m %d: f %.6f THz, Q %.4g; bend: neff %.7f, Re(neff) k0 R %.5f, Im(neff) %.4g against n_g/2Q %.4g (n_g %.4f)", R, m, f * 1e-12, Q,
              n, mFound, im, ng / (2 * Q), ng);
        QVERIFY2(std::abs(mFound - m) < 1e-3, qPrintable(QString::number(mFound, 'g', 10)));
        QVERIFY2(std::abs(im / (ng / (2 * Q)) - 1) < 0.05, qPrintable(QString::number(im / (ng / (2 * Q)))));
    }

    // A 90° bend of a silicon wire between two straight pieces, the light
    // along a path whose corner is rounded by the bend's radius - three
    // cells, the arc's one cut of the bend's modes -, absorbing walls (what
    // a junction radiates dies out instead of coming back at the other):
    // reciprocal, little back, its loss the two junctions' mismatch, larger
    // the tighter the bend (-0.011 dB at 5 µm, -0.137 dB at 1.5 µm with
    // these 20 modes; -0.164 dB with 40). A window reaching the bend's axis
    // refused. (A sanitized build: one bend of 6 modes on a coarse mesh, its
    // absorber thinner in its mesh - the path, its cells, the bend's modes
    // and their overlaps through the sanitizers - and the refusal.)
    void aBendAlongAPath()
    {
        const bool slow = slowBuild();
        int pieces = 0;
        auto bend = [&pieces, slow](double R, double inner) {
            const double w = 0.5, L = 2, clad = 1.0, half = 1.5;
            Model m = model3d();
            addBox(m, QStringLiteral("oxide"), -half, -half - 0.5, -clad, L + R + 2 * half + 0.5, R + L + 2 * half + 0.5, 2 * clad + 0.22);
            Node ext = m.make(QStringLiteral("extrude"));
            ext.label = QStringLiteral("core");
            ext.props.insert(QStringLiteral("z"), QStringLiteral("0"));
            ext.props.insert(QStringLiteral("height"), QStringLiteral("0.22"));
            const QString et = ext.tag;
            m.add(m.geometry().tag, ext);
            auto rect = [&](const QString& name, double x, double y, double wx, double wy) {
                Node r = m.make(QStringLiteral("rectangle"));
                r.label = name;
                r.props.insert(QStringLiteral("position"), pairOf(x, y));
                r.props.insert(QStringLiteral("size"), pairOf(wx, wy));
                m.add(et, r);
            };
            auto circle = [&](const QString& name, double radius) {
                Node c = m.make(QStringLiteral("circle"));
                c.label = name;
                c.props.insert(QStringLiteral("center"), pairOf(L, R));
                c.props.insert(QStringLiteral("radius"), QString::number(radius, 'g', 17));
                c.props.insert(QStringLiteral("sector"), QStringLiteral("90"));
                c.props.insert(QStringLiteral("rotation"), QStringLiteral("-90"));
                m.add(et, c);
            };
            rect(QStringLiteral("in"), -half, -w / 2, L + half, w);
            circle(QStringLiteral("outer"), R + w / 2);
            circle(QStringLiteral("inner"), R - w / 2);
            Node d = m.make(QStringLiteral("difference"));
            d.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("outer")});
            d.props.insert(QStringLiteral("tools"), QJsonArray{QStringLiteral("inner")});
            m.add(et, d);
            rect(QStringLiteral("out"), L + R - w / 2, R, w, L + half);
            addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("oxide")});
            addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), {QStringLiteral("core")});
            Node eme = m.make(QStringLiteral("eme"));
            eme.props.insert(QStringLiteral("axis"), QStringLiteral("path"));
            eme.props.insert(QStringLiteral("points"), QJsonArray{pairOf(-half, 0), pairOf(L + R, 0), pairOf(L + R, R + L + half)});
            eme.props.insert(QStringLiteral("radius"), QString::number(R, 'g', 17));
            eme.props.insert(QStringLiteral("window"), QStringLiteral("box"));
            eme.props.insert(QStringLiteral("across"), pairOf(-half, inner));
            eme.props.insert(QStringLiteral("height"), pairOf(-clad, clad + 0.22));
            eme.props.insert(QStringLiteral("sides"), QStringLiteral("absorbing"));
            // (A sanitized build: metal floors, the sides' absorber in one
            // step - four steps put four elements across each, and their
            // mesh is the cut's most.)
            eme.props.insert(QStringLiteral("floors"), slow ? QStringLiteral("pec") : QStringLiteral("absorbing"));
            eme.props.insert(QStringLiteral("absorberthickness"), QStringLiteral("0.5"));
            if (slow) eme.props.insert(QStringLiteral("absorbersteps"), 1);
            eme.props.insert(QStringLiteral("modes"), slow ? 6 : 20);
            eme.props.insert(QStringLiteral("resolution"), slow ? QStringLiteral("5") : QStringLiteral("10"));
            const QString tag = eme.tag;
            m.add(m.component().tag, eme);
            for (const char* end : {"start", "end"}) {
                Node port = m.make(QStringLiteral("eme/port"));
                port.props.insert(QStringLiteral("name"), QLatin1String(end) == QLatin1String("start") ? QStringLiteral("1") : QStringLiteral("2"));
                port.props.insert(QStringLiteral("end"), QString::fromLatin1(end));
                m.add(tag, port);
            }
            const Built b = build(m);
            EmeDevice device;
            QString why;
            if (!readEmeDevice(m, *m.find(tag), *b.topology, *b.parameters, C0 / 1.55e-6, &device, &why)) qFatal("%s", qPrintable(why));
            EmeResult r = solveEme(m, b.parameters, *b.topology, device, C0 / 1.55e-6);
            pieces = int(device.path.size());
            return r;
        };
        // Its cut reaching the axis of the tight bend: refused, said (before
        // any mode is solved).
        const EmeResult over = bend(1.5, 1.2);
        QVERIFY2(!over.ok() && over.error.contains(QLatin1String("within the bend's radius")), qPrintable(over.error));
        if (slow) {
            QElapsedTimer t;
            t.start();
            const EmeResult r = bend(5, 0.8);
            QVERIFY2(r.ok(), qPrintable(r.error));
            qInfo("90° bend, 6 modes: |S21|² %.6f, back %.2g (%lld ms)", std::norm(r.S(1, 0)), std::norm(r.S(0, 0)), t.elapsed());
            QCOMPARE(pieces, 3);
            QCOMPARE(int(r.cells.size()), 3);
            QVERIFY(std::abs(r.S(1, 0) - r.S(0, 1)) < 1e-9);
            QVERIFY(std::norm(r.S(1, 0)) > 0.9 && std::norm(r.S(1, 0)) + std::norm(r.S(0, 0)) <= 1 + 1e-9);
            return;
        }
        const EmeResult wide = bend(5, 0.8), tight = bend(1.5, 0.8);
        QVERIFY2(wide.ok() && tight.ok(), qPrintable(wide.error + tight.error));
        QCOMPARE(pieces, 3);
        qInfo("90° bend: |S21|² %.6f (%.4f dB) at 5 µm, %.6f (%.4f dB) at 1.5 µm; back %.2g, %.2g", std::norm(wide.S(1, 0)), 10 * std::log10(std::norm(wide.S(1, 0))),
              std::norm(tight.S(1, 0)), 10 * std::log10(std::norm(tight.S(1, 0))), std::norm(wide.S(0, 0)), std::norm(tight.S(0, 0)));
        for (const EmeResult* r : {&wide, &tight}) {
            QCOMPARE(int(r->cells.size()), 3);
            QVERIFY(std::abs(r->S(1, 0) - r->S(0, 1)) < 1e-9);
            QVERIFY(std::norm(r->S(0, 0)) < 1e-6);
        }
        QVERIFY(std::norm(wide.S(1, 0)) > 0.99);
        QVERIFY(std::norm(tight.S(1, 0)) < std::norm(wide.S(1, 0)) - 0.01);
    }

    // A bend uniform in z between metal floors - a 2D problem in the plan,
    // E along z - by the expansion along a path and by the 2D full-wave
    // solver in the plan: the same strips round the path (the core, 1.5 µm
    // of clear oxide outside it and 0.8 µm inside, 1 µm of absorber beyond
    // each in four steps to κ 1, metal beyond that), ports at both ends
    // taking the guide's one mode. A silicon core 0.2 µm wide at 2 µm, its
    // loss the straight's and the bend's modes' mismatch: |S21|² 0.994564
    // by the 2D solve, 0.994434 by the expansion (0.994460 with 60 modes);
    // a core of index 2, 0.5 µm wide, at 5 µm, where the bend radiates:
    // 0.911348 and 0.910109 (0.910184), the 2D solve's ports reflecting
    // some of what reaches them (its |S11|² 7.7e-4, the expansion's 8e-9).
    // Neither solve moves with a finer mesh (h 0.04 µm: 0.994564,
    // 0.911345). (A guide of two modes would not do: the 2D solve's ports
    // take the first alone and reflect the rest - a 0.5 µm silicon core
    // gave its |S11|² 0.26 at 2 µm.)
    void aBendAgreesWithTheFullWaveSolve()
    {
        if (slowBuild()) QSKIP("a sanitized build: the 2D solves' 100,000 triangles take minutes");
        auto both = [](double R, double w, const QString& ncore, cd* fem21, EmeResult* result) {
            const double Ls = 2, uo = 1.5, ui = 0.8, d = 1.0, t = 0.2;
            const int N = 4;
            const double f0 = C0 / 1.55e-6;
            auto kappaOf = [&](int i) { return std::pow((i + 0.5) / N, 2); };
            struct Strip {
                double u0, u1;
                int absorber;   // its step, -1 none
                bool core;
            };
            std::vector<Strip> strips;
            for (int i = N - 1; i >= 0; --i) strips.push_back({-uo - (i + 1) * d / N, -uo - i * d / N, i, false});
            strips.push_back({-uo, -w / 2, -1, false});
            strips.push_back({-w / 2, w / 2, -1, true});
            strips.push_back({w / 2, ui, -1, false});
            for (int i = 0; i < N; ++i) strips.push_back({ui + i * d / N, ui + (i + 1) * d / N, i, false});
            // The 2D model: each strip along the first piece (u = y), round
            // the arc about (Ls, R) (ρ = R - u, polygons of 0.01 µm chords)
            // and along the last (x = Ls + R - u).
            QJsonArray geometry, startNames, endNames, coreNames, oxideNames;
            std::vector<QJsonArray> absorberNames(N);
            const int arcPoints = int(std::ceil(R * M_PI / 2 / 0.01));
            for (std::size_t k = 0; k < strips.size(); ++k) {
                const Strip& s = strips[k];
                auto add = [&](const QString& name, const QJsonArray& points) {
                    geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("polygon")}, {QStringLiteral("name"), name}, {QStringLiteral("points"), points}});
                    (s.core ? coreNames : s.absorber < 0 ? oxideNames : absorberNames[s.absorber]).append(name);
                };
                const QString base = QStringLiteral("s%1").arg(k);
                add(base + QLatin1Char('a'), QJsonArray{pairOf(0, s.u0), pairOf(Ls, s.u0), pairOf(Ls, s.u1), pairOf(0, s.u1)});
                startNames.append(base + QLatin1Char('a'));
                QJsonArray arc;
                for (int i = 0; i <= arcPoints; ++i) {
                    const double a = -M_PI / 2 + M_PI / 2 * i / arcPoints;
                    arc.append(pairOf(Ls + (R - s.u0) * std::cos(a), R + (R - s.u0) * std::sin(a)));
                }
                for (int i = arcPoints; i >= 0; --i) {
                    const double a = -M_PI / 2 + M_PI / 2 * i / arcPoints;
                    arc.append(pairOf(Ls + (R - s.u1) * std::cos(a), R + (R - s.u1) * std::sin(a)));
                }
                add(base + QLatin1Char('b'), arc);
                add(base + QLatin1Char('c'),
                    QJsonArray{pairOf(Ls + R - s.u1, R), pairOf(Ls + R - s.u0, R), pairOf(Ls + R - s.u0, R + Ls), pairOf(Ls + R - s.u1, R + Ls)});
                endNames.append(base + QLatin1Char('c'));
            }
            auto material = [](const QString& label, const QString& n, const QJsonArray& on, double kappa) {
                QJsonObject m{{QStringLiteral("label"), label}, {QStringLiteral("n"), n}, {QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), on}}}};
                if (kappa > 0) m.insert(QStringLiteral("kappa"), QString::number(kappa, 'g', 17));
                return m;
            };
            QJsonArray materials{material(QStringLiteral("oxide"), QStringLiteral("1.444"), oxideNames, 0),
                                 material(QStringLiteral("core"), ncore, coreNames, 0)};
            for (int i = 0; i < N; ++i) materials.append(material(QStringLiteral("absorber%1").arg(i), QStringLiteral("1.444"), absorberNames[i], kappaOf(i)));
            auto port = [](const QString& name, const QJsonArray& of, const QString& side, bool excite) {
                return QJsonObject{{QStringLiteral("type"), QStringLiteral("port")},
                                   {QStringLiteral("name"), name},
                                   {QStringLiteral("excite"), excite},
                                   {QStringLiteral("selection"), QJsonObject{{QStringLiteral("of"), of}, {QStringLiteral("side"), side}}}};
            };
            const QJsonObject root{
                {QStringLiteral("format"), QStringLiteral("qucs-s multiphysics 1")},
                {QStringLiteral("components"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("unit"), QStringLiteral("um")},
                     {QStringLiteral("geometry"), geometry},
                     {QStringLiteral("materials"), materials},
                     {QStringLiteral("physics"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("waves")},
                                             {QStringLiteral("tag"), QStringLiteral("ewfd")},
                                             {QStringLiteral("polarization"), QStringLiteral("ez")},
                                             {QStringLiteral("order"), QStringLiteral("2")},
                                             {QStringLiteral("features"), QJsonArray{port(QStringLiteral("1"), startNames, QStringLiteral("left"), true),
                                                                                     port(QStringLiteral("2"), endNames, QStringLiteral("top"), false)}}}}},
                     {QStringLiteral("mesh"),
                      QJsonObject{{QStringLiteral("size"), QStringLiteral("normal")},
                                  {QStringLiteral("custom"), true},
                                  {QStringLiteral("hmax"), QStringLiteral("0.06")},
                                  {QStringLiteral("features"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("size")},
                                                                                      {QStringLiteral("level"), QStringLiteral("domain")},
                                                                                      {QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), coreNames}}},
                                                                                      {QStringLiteral("hmax"), QStringLiteral("0.02")}}}}}}}}},
                {QStringLiteral("studies"),
                 QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("study")},
                                        {QStringLiteral("steps"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("frequency")},
                                                                                         {QStringLiteral("frequencies"), QString::number(f0, 'g', 17) + QStringLiteral("[Hz]")}}}}}}}};
            QString why;
            std::optional<Model> m2 = Model::fromJson(QJsonDocument(root).toJson(), &why);
            if (!m2) qFatal("%s", qPrintable(why));
            auto ps2 = evaluateParameters(*m2);
            const GeometryBuild g2 = buildGeometry(*m2, *ps2);
            if (!g2.ok()) qFatal("the 2D bend's geometry");
            StudyRun run2 = runStudy(*m2, ps2, g2.topology, nullptr, QString(), {});
            if (!run2.ok()) qFatal("%s", qPrintable(run2.error));
            auto s2 = run2.solutions.back();
            s2->select(0);
            *fem21 = cd(*s2->global(QStringLiteral("ewfd.S21")), *s2->global(QStringLiteral("ewfd.S21_im")));
            // The expansion: the core extruded through the window's height,
            // the oxide round it; its cuts' sides the same absorber.
            Model m = model3d();
            addBox(m, QStringLiteral("oxide"), -0.5, -uo - d - 0.5, 0, Ls + R + uo + d + 1, R + Ls + uo + d + 1, t);
            Node ext = m.make(QStringLiteral("extrude"));
            ext.label = QStringLiteral("core");
            ext.props.insert(QStringLiteral("z"), QStringLiteral("0"));
            ext.props.insert(QStringLiteral("height"), QString::number(t, 'g', 17));
            const QString et = ext.tag;
            m.add(m.geometry().tag, ext);
            auto rect = [&](const QString& name, double x, double y, double wx, double wy) {
                Node r = m.make(QStringLiteral("rectangle"));
                r.label = name;
                r.props.insert(QStringLiteral("position"), pairOf(x, y));
                r.props.insert(QStringLiteral("size"), pairOf(wx, wy));
                m.add(et, r);
            };
            auto circle = [&](const QString& name, double radius) {
                Node c = m.make(QStringLiteral("circle"));
                c.label = name;
                c.props.insert(QStringLiteral("center"), pairOf(Ls, R));
                c.props.insert(QStringLiteral("radius"), QString::number(radius, 'g', 17));
                c.props.insert(QStringLiteral("sector"), QStringLiteral("90"));
                c.props.insert(QStringLiteral("rotation"), QStringLiteral("-90"));
                m.add(et, c);
            };
            rect(QStringLiteral("in"), -0.5, -w / 2, Ls + 0.5, w);
            circle(QStringLiteral("outer"), R + w / 2);
            circle(QStringLiteral("inner"), R - w / 2);
            Node dn = m.make(QStringLiteral("difference"));
            dn.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("outer")});
            dn.props.insert(QStringLiteral("tools"), QJsonArray{QStringLiteral("inner")});
            m.add(et, dn);
            rect(QStringLiteral("out"), Ls + R - w / 2, R, w, Ls + 0.5);
            addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("oxide")});
            addMaterial(m, QStringLiteral("core"), ncore, {QStringLiteral("core")});
            Node eme = m.make(QStringLiteral("eme"));
            eme.props.insert(QStringLiteral("axis"), QStringLiteral("path"));
            eme.props.insert(QStringLiteral("points"), QJsonArray{pairOf(0, 0), pairOf(Ls + R, 0), pairOf(Ls + R, R + Ls)});
            eme.props.insert(QStringLiteral("radius"), QString::number(R, 'g', 17));
            eme.props.insert(QStringLiteral("window"), QStringLiteral("box"));
            eme.props.insert(QStringLiteral("across"), pairOf(-uo, ui));
            eme.props.insert(QStringLiteral("height"), pairOf(0, t));
            eme.props.insert(QStringLiteral("sides"), QStringLiteral("absorbing"));
            eme.props.insert(QStringLiteral("floors"), QStringLiteral("pec"));
            eme.props.insert(QStringLiteral("absorberthickness"), QString::number(d, 'g', 17));
            eme.props.insert(QStringLiteral("absorberkappa"), QStringLiteral("1"));
            eme.props.insert(QStringLiteral("absorbersteps"), N);
            eme.props.insert(QStringLiteral("modes"), 30);
            const QString tag = eme.tag;
            m.add(m.component().tag, eme);
            for (const char* end : {"start", "end"}) {
                Node p = m.make(QStringLiteral("eme/port"));
                p.props.insert(QStringLiteral("name"), QLatin1String(end) == QLatin1String("start") ? QStringLiteral("1") : QStringLiteral("2"));
                p.props.insert(QStringLiteral("end"), QString::fromLatin1(end));
                p.props.insert(QStringLiteral("modes"), QStringLiteral("1"));
                m.add(tag, p);
            }
            const Built b = build(m);
            EmeDevice device;
            if (!readEmeDevice(m, *m.find(tag), *b.topology, *b.parameters, f0, &device, &why)) qFatal("%s", qPrintable(why));
            *result = solveEme(m, b.parameters, *b.topology, device, f0);
        };
        struct Case {
            double R, w;
            const char* n;
            double tolerance;
        };
        for (const Case& c : {Case{2, 0.2, "3.476", 5e-4}, Case{5, 0.5, "2", 3e-3}}) {
            cd fem21;
            EmeResult eme;
            both(c.R, c.w, QString::fromLatin1(c.n), &fem21, &eme);
            QVERIFY2(eme.ok(), qPrintable(eme.error));
            qInfo("bend of %.1f µm, core %s %.1f µm wide: 2D solve |S21|² %.6f, expansion %.6f (|S11|² %.2g)", c.R, c.n, c.w, std::norm(fem21),
                  std::norm(eme.S(1, 0)), std::norm(eme.S(0, 0)));
            QVERIFY2(std::abs(std::norm(eme.S(1, 0)) / std::norm(fem21) - 1) < c.tolerance, qPrintable(QString::number(std::norm(eme.S(1, 0)) / std::norm(fem21) - 1)));
            QVERIFY(std::norm(eme.S(0, 0)) < 1e-6);
        }
    }

    // A 1x2 MMI uniform in z between metal floors (a 2D problem in the plan,
    // E along z) against mode matching with every mode: a core of index 2
    // (access guides 0.5 µm wide, single-mode), the MMI 3 µm wide and 7.185
    // µm long (3L_π/8 of its two lowest modes), its outputs at ±W/4, 1.5 µm
    // of clear oxide beside it, 1 µm of absorber beyond in four steps to κ
    // 1, metal beyond that, 2 µm of guide either side. The reference: the
    // same across a box of finite differences (h 0.01 and 0.005 µm,
    // extrapolated), every one of its modes kept - so no truncation, exact
    // for that box - a scalar E_z, written apart from this code (the
    // write-up has it): the light out, the output section's supermodes'
    // power, 0.88043 (0.77602 at 6 µm), back 0.00303 (0.00291). The
    // expansion with 40 modes: 0.88091 and 0.77571, back 0.00306 and
    // 0.00294. (The 2D full-wave solver is no reference here: its ports
    // take their first mode alone and reflect what else reaches them - an
    // MMI's unimaged light -, which moved its light out by up to 0.04.)
    void anMmiAgreesWithModeMatchingOfEveryMode()
    {
        if (slowBuild()) QSKIP("a sanitized build: two expansions of 40 modes take minutes");
        const double W = 3, w = 0.5, a = 2, U = W / 2 + 1.5, t = 0.2;
        auto run = [&](double L) {
            Model m = model3d();
            const double xe = a + L + a;
            auto box = [&](const QString& label, double x, double y, double wx, double wy) { addBox(m, label, x, y, 0, wx, wy, t); };
            box(QStringLiteral("oxide"), 0, -U, xe, 2 * U);
            box(QStringLiteral("in"), 0, -w / 2, a, w);
            box(QStringLiteral("mmi"), a, -W / 2, L, W);
            box(QStringLiteral("out1"), a + L, W / 4 - w / 2, a, w);
            box(QStringLiteral("out2"), a + L, -W / 4 - w / 2, a, w);
            addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("oxide")});
            addMaterial(m, QStringLiteral("core"), QStringLiteral("2"), {QStringLiteral("in"), QStringLiteral("mmi"), QStringLiteral("out1"), QStringLiteral("out2")});
            Node eme = m.make(QStringLiteral("eme"));
            eme.props.insert(QStringLiteral("axis"), QStringLiteral("x"));
            eme.props.insert(QStringLiteral("window"), QStringLiteral("box"));
            eme.props.insert(QStringLiteral("across"), pairOf(-U, U));
            eme.props.insert(QStringLiteral("height"), pairOf(0, t));
            eme.props.insert(QStringLiteral("sides"), QStringLiteral("absorbing"));
            eme.props.insert(QStringLiteral("floors"), QStringLiteral("pec"));
            eme.props.insert(QStringLiteral("absorberthickness"), QStringLiteral("1"));
            eme.props.insert(QStringLiteral("absorberkappa"), QStringLiteral("1"));
            eme.props.insert(QStringLiteral("absorbersteps"), 4);
            eme.props.insert(QStringLiteral("modes"), 40);
            const QString tag = eme.tag;
            m.add(m.component().tag, eme);
            for (const char* end : {"start", "end"}) {
                Node p = m.make(QStringLiteral("eme/port"));
                const bool atEnd = QLatin1String(end) == QLatin1String("end");
                p.props.insert(QStringLiteral("name"), atEnd ? QStringLiteral("2") : QStringLiteral("1"));
                p.props.insert(QStringLiteral("end"), QString::fromLatin1(end));
                // (The end's two supermodes, even and odd: the light out.)
                p.props.insert(QStringLiteral("modes"), atEnd ? QStringLiteral("1, 2") : QStringLiteral("1"));
                m.add(tag, p);
            }
            const Built b = build(m);
            EmeDevice device;
            QString why;
            if (!readEmeDevice(m, *m.find(tag), *b.topology, *b.parameters, C0 / 1.55e-6, &device, &why)) qFatal("%s", qPrintable(why));
            return solveEme(m, b.parameters, *b.topology, device, C0 / 1.55e-6);
        };
        struct Case {
            double L, out, back;
        };
        for (const Case& c : {Case{7.185, 0.88043, 0.00303}, Case{6.0, 0.77602, 0.00291}}) {
            const EmeResult r = run(c.L);
            QVERIFY2(r.ok(), qPrintable(r.error));
            QCOMPARE(int(r.S.rows()), 3);
            const double out = std::norm(r.S(1, 0)) + std::norm(r.S(2, 0)), back = std::norm(r.S(0, 0));
            qInfo("MMI (2D) %.3f µm long: out %.5f (reference %.5f), back %.5f (%.5f); the odd supermode %.1g", c.L, out, c.out, back, c.back,
                  std::norm(r.S(2, 0)));
            QVERIFY2(std::abs(out / c.out - 1) < 2e-3, qPrintable(QString::number(out / c.out - 1)));
            QVERIFY2(std::abs(back / c.back - 1) < 0.03, qPrintable(QString::number(back / c.back - 1)));
            // (Symmetric: nothing in the odd one.)
            QVERIFY(std::norm(r.S(2, 0)) < 1e-4);
        }
    }

    // The overlaps on the machine's threads: the batched ones (each pair's
    // triangles in fixed chunks, their points' transverse fields gathered and
    // multiplied as matrices) as the point-by-point ones to rounding - two
    // meshes, one mesh, absorbing walls, a bend -, the same to the last bit on
    // 1 and 8 threads. (Measured, not asserted: a wire into an MMI's junction
    // of 40 modes 5.7 s point by point, 1.2 s batched, 0.18 s on 8 threads;
    // with absorbing walls 22.5, 4.9 and 0.70 s.) A whole solve's threads
    // are aWholeSolveRunsOnAnyThreadsAlike's: the two were one function, past
    // QtTest's 300 s on CI's macOS 15 runner (v27.0.9's tag run) - Eigen's LU
    // there for the cuts' modes, 98 and 34 s by it on an M2 Ultra.
    void theOverlapsRunOnAnyThreadsAlike()
    {
        const bool slow = slowBuild();
        Model m = wire(2, 4.5, {{{0, 1, -0.25, 0.25}}, {{1, 2, -1.5, 1.5}}}, 1.0);
        const Built b = build(m);
        QString why;
        // (A sanitized build: a smaller window, fewer modes, no absorbing walls -
        // the point-by-point reference slow there.)
        const double half = slow ? 1.75 : 2.25, below = slow ? -0.5 : -1, above = slow ? 0.72 : 1.22;
        const CrossSection ca = cutAcross(*b.topology, QPointF(0.5, 0), QPointF(0, 1), -half, half, below, above, &why);
        const CrossSection cb = cutAcross(*b.topology, QPointF(1.5, 0), QPointF(0, 1), -half, half, below, above, &why);
        auto solve = [&](CrossSection c, const QString& walls, double bend) {
            EmeSettings st;
            st.modes = slow ? 4 : 12;
            st.resolution = slow ? 5 : 10;
            st.sides = st.floors = walls;
            st.absorberThickness = 0.5;
            c.bendRadius = c.bendCenter = bend;
            EmeModes md = solveSectionModes(m, b.parameters, *b.topology, c, C0 / 1.55e-6, st, &why);
            return md;
        };
        const EmeModes a = solve(ca, QStringLiteral("pec"), 0), mmi = solve(cb, QStringLiteral("pec"), 0);
        EmeModes bend, aAbs, mmiAbs;
        std::vector<std::pair<const EmeModes*, const EmeModes*>> pairs{{&a, &mmi}, {&a, &a}};
        if (!slow) {
            bend = solve(ca, QStringLiteral("pec"), 3);
            pairs.push_back({&bend, &a});
            aAbs = solve(ca, QStringLiteral("absorbing"), 0);
            mmiAbs = solve(cb, QStringLiteral("absorbing"), 0);
            pairs.push_back({&aAbs, &mmiAbs});
        }
        QVERIFY2(why.isEmpty(), qPrintable(why));
        std::vector<Eigen::MatrixXcd> ab1, ba1, ab8, ba8;
        QVERIFY(modeOverlapsMany(pairs, 1, {}, {}, &ab1, &ba1));
        QVERIFY(modeOverlapsMany(pairs, 8, {}, {}, &ab8, &ba8));
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            QVERIFY(ab1[i] == ab8[i] && ba1[i] == ba8[i]);
            Eigen::MatrixXcd ab, ba;
            modeOverlapsByPoint(*pairs[i].first, *pairs[i].second, &ab, &ba);
            const double scale = std::max(ab.cwiseAbs().maxCoeff(), ba.cwiseAbs().maxCoeff());
            const double d = std::max((ab1[i] - ab).cwiseAbs().maxCoeff(), (ba1[i] - ba).cwiseAbs().maxCoeff()) / scale;
            qInfo("pair %d: batched against point by point %.2g of the largest entry", int(i), d);
            QVERIFY2(d < 1e-11, qPrintable(QString::number(d)));
        }
    }

    // A whole solve (a 1x2 MMI, its outputs in boxes) the same to the last
    // bit on 1 and 8 threads; progress said for the cuts and then the
    // overlaps; a Cancel in the overlaps heard within a second.
    void aWholeSolveRunsOnAnyThreadsAlike()
    {
        // (Its cuts alone take minutes there; the pool's threads are
        // theOverlapsRunOnAnyThreadsAlike's.)
        if (slowBuild()) QSKIP("a sanitized build: the cuts take minutes");
        Model mm = wire(6, 4.5, {{{0, 1.5, -0.25, 0.25}}, {{1.5, 4.5, -1.5, 1.5}}, {{4.5, 6, 0.5, 1.0}}, {{4.5, 6, -1.0, -0.5}}}, 1.0);
        const Built bm = build(mm);
        EmeDevice d = deviceOf(*bm.topology, 16, 8);
        d.ports.clear();
        EmePort in, up, down;
        in.name = QStringLiteral("1");
        up.name = QStringLiteral("2");
        up.atEnd = true;
        up.whole = false;
        up.u0 = 0;
        up.u1 = 2.25;
        up.z0 = d.z0;
        up.z1 = d.z1;
        down = up;
        down.name = QStringLiteral("3");
        down.u0 = -2.25;
        down.u1 = 0;
        d.ports = {in, up, down};
        clearEmeCache();
        d.settings.threads = 1;
        const EmeResult r1 = solveEme(mm, bm.parameters, *bm.topology, d, C0 / 1.55e-6);
        clearEmeCache();
        d.settings.threads = 8;
        QStringList said;
        std::mutex saying;
        d.settings.progress = [&](double, const QString& what) {
            std::lock_guard<std::mutex> lock(saying);
            said << what;
        };
        const EmeResult r8 = solveEme(mm, bm.parameters, *bm.topology, d, C0 / 1.55e-6);
        QVERIFY2(r1.ok() && r8.ok(), qPrintable(r1.error + r8.error));
        qInfo("MMI on 1 and 8 threads: |S21|² %.6f and %.6f; %s", std::norm(r1.S(1, 0)), std::norm(r8.S(1, 0)), qPrintable(r8.log.join(QLatin1String("; "))));
        QVERIFY(r1.S == r8.S);
        QVERIFY(!said.isEmpty() && said.first().startsWith(QLatin1String("cuts")) && said.last().startsWith(QLatin1String("junctions' overlaps")));
        QVERIFY(r8.log.first().contains(QLatin1String("the overlaps")));
        // Cancelled once the overlaps are under way: heard between their jobs.
        {
            clearEmeCache();
            std::atomic<bool> overlapping{false};
            QElapsedTimer since;
            d.settings.progress = [&](double, const QString& what) {
                if (what.startsWith(QLatin1String("junctions")) && !overlapping) {
                    overlapping = true;
                    since.start();
                }
            };
            d.settings.cancelled = [&] { return bool(overlapping); };
            d.settings.modes = 30;
            d.settings.resolution = 15;
            const EmeResult rc = solveEme(mm, bm.parameters, *bm.topology, d, C0 / 1.55e-6);
            QVERIFY2(rc.error == QLatin1String("cancelled"), qPrintable(rc.error));
            qInfo("cancelled in the overlaps: back %lld ms after", since.elapsed());
            QVERIFY(since.elapsed() < 1000);
        }
    }

    // A quarter-wave Bragg mirror filling the window - magnetic sides, metal
    // floors: its one propagating mode a plane wave, E along z, its index the
    // layer's - against the transfer matrix, exact for it: oxide, (H L)^N H of
    // index 2 and 1.444, each a quarter of 1550 nm in it, oxide. Drawn layer
    // by layer (2N + 3 cells of two cuts: each layer a domain of its own, of
    // one of two materials) and as a Periodic Section of the one period drawn:
    // S11 and S21, their phases with them, within 1e-11 at 10 periods (1e-14
    // found) and 1e-9 at 1,000 (7e-12 by the section's squaring, 2e-10 drawn:
    // the rounding of 2,000 products), in the stopband and out of it. A
    // period that begins as the guide before it does (L H after oxide) kept
    // apart from it; S to the n by squaring as by n products; a section past
    // the device's end, of no whole number of periods, or over another,
    // refused.
    void aBraggStackAgreesWithTheTransferMatrix()
    {
        const double nH = 2.0, nL = 1.444, dH = 1.55 / (4 * nH), dL = 1.55 / (4 * nL), a = 1.0, W = 0.3, H = 0.3;
        // drawn periods drawn (L H, or H L and a last H); a Periodic Section
        // of `periods` over the first one when `section`.
        auto stack = [&](int drawn, bool lowFirst, long long periods, bool section, Model* out) {
            Model m = model3d();
            QStringList hs, ls;
            double x = 0;
            auto layer = [&](const QString& name, double w, bool high) {
                addBox(m, name, x, -W / 2, 0, w, W, H);
                (high ? hs : ls) << name;
                x += w;
            };
            layer(QStringLiteral("in"), a, false);
            for (int i = 0; i < drawn; ++i) {
                layer(QStringLiteral("p%1a").arg(i), lowFirst ? dL : dH, !lowFirst);
                layer(QStringLiteral("p%1b").arg(i), lowFirst ? dH : dL, lowFirst);
            }
            if (!lowFirst) layer(QStringLiteral("last"), dH, true);
            layer(QStringLiteral("out"), a, false);
            addMaterial(m, QStringLiteral("low"), QString::number(nL), ls);
            addMaterial(m, QStringLiteral("high"), QString::number(nH), hs);
            Node eme = m.make(QStringLiteral("eme"));
            eme.props.insert(QStringLiteral("sides"), QStringLiteral("pmc"));
            eme.props.insert(QStringLiteral("floors"), QStringLiteral("pec"));
            eme.props.insert(QStringLiteral("modes"), 4);
            eme.props.insert(QStringLiteral("resolution"), QStringLiteral("10"));
            const QString tag = eme.tag;
            m.add(m.component().tag, eme);
            for (const char* end : {"start", "end"}) {
                Node p = m.make(QStringLiteral("eme/port"));
                p.props.insert(QStringLiteral("name"), QLatin1String(end) == QLatin1String("start") ? QStringLiteral("1") : QStringLiteral("2"));
                p.props.insert(QStringLiteral("end"), QString::fromLatin1(end));
                p.props.insert(QStringLiteral("modes"), QStringLiteral("1"));
                m.add(tag, p);
            }
            if (section) {
                Node per = m.make(QStringLiteral("eme/periodic"));
                per.props.insert(QStringLiteral("from"), QString::number(a, 'g', 17));
                per.props.insert(QStringLiteral("to"), QString::number(a + dH + dL, 'g', 17));
                per.props.insert(QStringLiteral("periods"), QString::number(periods));
                m.add(tag, per);
            }
            *out = m;
            return tag;
        };
        auto check = [&](int drawn, bool lowFirst, long long periods, bool section, double tolerance, int cells) {
            Model m;
            const QString tag = stack(drawn, lowFirst, periods, section, &m);
            const Built b = build(m);
            std::vector<std::pair<double, double>> layers;
            for (long long i = 0; i < periods; ++i) {
                layers.push_back(lowFirst ? std::make_pair(nL, dL) : std::make_pair(nH, dH));
                layers.push_back(lowFirst ? std::make_pair(nH, dH) : std::make_pair(nL, dL));
            }
            if (!lowFirst) layers.push_back({nH, dH});
            for (double lambda : {1.55, 1.40, 1.25}) {
                EmeDevice device;
                QString why;
                QVERIFY2(readEmeDevice(m, *m.find(tag), *b.topology, *b.parameters, C0 / (lambda * 1e-6), &device, &why), qPrintable(why));
                const EmeResult r = solveEme(m, b.parameters, *b.topology, device, C0 / (lambda * 1e-6));
                QVERIFY2(r.ok(), qPrintable(r.error));
                QCOMPARE(int(r.cells.size()), cells);
                QCOMPARE(r.sections, 2);
                cd rt, tt;
                transferMatrix(layers, nL, nL, lambda, &rt, &tt);
                // (The ports a from the stack's faces, in oxide.)
                const cd shift = std::exp(cd(0, -2 * 2 * M_PI / lambda * nL * a));
                const double e11 = std::abs(r.S(0, 0) - rt * shift), e21 = std::abs(r.S(1, 0) - tt * shift);
                qInfo("%s %lld periods (%s), %.2f µm: |S11|² %.10f (the matrix's %.10f), errors %.2g, %.2g", section ? "a section of" : "drawn,",
                      periods, lowFirst ? "L H" : "H L", lambda, std::norm(r.S(0, 0)), std::norm(rt), e11, e21);
                QVERIFY2(e11 < tolerance && e21 < tolerance, qPrintable(QStringLiteral("%1 %2").arg(e11).arg(e21)));
                QVERIFY(std::abs(r.S(1, 0) - r.S(0, 1)) < tolerance);
            }
        };
        check(10, false, 10, false, 1e-11, 23);
        check(1, false, 10, true, 1e-11, 5);
        check(1, false, 1000, true, 1e-9, 5);
        if (!slowBuild()) check(200, false, 200, false, 1e-9, 403);
        // A period that begins as the guide before it: oxide, then (L H)^N.
        check(1, true, 10, true, 1e-11, 4);
        // S to the n by squaring.
        {
            Eigen::MatrixXcd O(2, 2);
            O << 0.95, 0.12, -0.08, 0.9;
            const SMatrix P = cascade(interfaceS(O), propagationS({cd(8e6, -1e3), cd(5e6, -2e4)}, 1e-6));
            SMatrix product = P;
            for (int n = 1; n <= 9; ++n) {
                if (n > 1) product = cascade(product, P);
                const SMatrix power = powerS(P, n);
                const double e = std::max({(power.s11 - product.s11).cwiseAbs().maxCoeff(), (power.s12 - product.s12).cwiseAbs().maxCoeff(),
                                           (power.s21 - product.s21).cwiseAbs().maxCoeff(), (power.s22 - product.s22).cwiseAbs().maxCoeff()});
                QVERIFY2(e < 1e-12, qPrintable(QStringLiteral("n %1: %2").arg(n).arg(e)));
            }
        }
        // Refused: past the end, no whole number, over another.
        Model m;
        const QString tag = stack(1, false, 10, true, &m);
        const Built b = build(m);
        Node* per = nullptr;
        for (Node& c : m.find(tag)->children)
            if (c.type == QLatin1String("eme/periodic")) per = &c;
        QVERIFY(per);
        auto refused = [&](const char* expected) {
            EmeDevice device;
            QString why;
            const bool ok = readEmeDevice(m, *m.find(tag), *b.topology, *b.parameters, C0 / 1.55e-6, &device, &why);
            QVERIFY2(!ok && why.contains(QLatin1String(expected)), qPrintable(why));
        };
        per->props.insert(QStringLiteral("to"), QStringLiteral("9"));
        refused("reaches outside the device");
        per->props.insert(QStringLiteral("to"), QString::number(a + dH + dL, 'g', 17));
        per->props.insert(QStringLiteral("periods"), QStringLiteral("2.5"));
        refused("a whole number");
        per->props.insert(QStringLiteral("periods"), QStringLiteral("10"));
        Node again = m.make(QStringLiteral("eme/periodic"));
        again.props.insert(QStringLiteral("from"), QString::number(a + dH, 'g', 17));
        again.props.insert(QStringLiteral("to"), QString::number(a + 2 * dH + dL, 'g', 17));
        QVERIFY(m.add(tag, again));
        refused("overlap");
    }

    // A waveguide Bragg grating uniform in z (a 2D problem in the plan)
    // against the 2D full-wave solver: a core of index 2 (or 2.848, a
    // silicon slab's), its width alternating w2 and w1 at the period of the
    // slab's indices (L (n1 + n2) = 1550 nm), access guides of the mean width
    // 2 µm long, 1.5 µm of clear oxide beside it, 1 µm of absorber in four
    // steps beyond, metal beyond that; ports at both ends.
    // - E along the steps (E along z, metal floors), 30 periods of 0.42 and
    //   0.50 µm: |S11|² 0.348197 against 0.348139 at the stopband's centre,
    //   within 2.3e-4 across it at 40 elements per wavelength - at 15 its
    //   stopband sat 0.4 nm short (3.2e-3 off): its indices must be right to
    //   1e-4. The drawn grating and the Periodic Section alike.
    // - E across them (in the plane, magnetic floors - a silicon wire's TE
    //   sidewall grating's), 100 periods of 0.49 and 0.51 µm: the 2D solve's
    //   0.1497 (0.1492, 0.1499, 0.1497 at h 0.06, 0.04, 0.025 µm), the
    //   effective indices' coupled-mode estimate tanh²(2Δn N L/λ) 0.607 - the
    //   steps couple far more weakly than their indices say, E normal to them
    //   -, and the expansion 0.128 with 30 modes at 30 per wavelength (0.136
    //   with 60 at 40, 0.139 at 80): its κ 4 to 8 % low, slow in the modes
    //   and the mesh where E crosses a step's corner.
    void aGratingAgreesWithTheFullWaveSolve()
    {
        if (slowBuild()) QSKIP("a sanitized build: the 2D solves' 200,000 triangles take minutes");
        struct Grating {
            double w1, w2, nc;
            bool hz;
            int periods, modes;
            QString resolution;
        };
        // The 2D solve's |S11|², |S21|² at each wavelength; the expansion's
        // (a Periodic Section, or drawn tooth by tooth).
        auto solve = [](const Grating& gr, const std::vector<double>& lambdas, bool drawn, std::vector<double>* fem11, std::vector<double>* fem21,
                        std::vector<cd>* eme11, std::vector<cd>* eme21) {
            const double no = 1.444, k0 = 2 * M_PI / 1.55;
            auto slab = [&](double w) {
                double lo = no + 1e-12, hi = gr.nc - 1e-12;
                for (int i = 0; i < 200; ++i) {
                    const double n = (lo + hi) / 2, kx = k0 * std::sqrt(gr.nc * gr.nc - n * n), g = k0 * std::sqrt(n * n - no * no);
                    const double ratio = gr.hz ? gr.nc * gr.nc / (no * no) : 1.0;
                    if (kx * w / 2 >= M_PI / 2 || std::tan(kx * w / 2) - ratio * g / kx > 0) lo = n;
                    else hi = n;
                }
                return (lo + hi) / 2;
            };
            const double L = 1.55 / (slab(gr.w1) + slab(gr.w2)), wa = (gr.w1 + gr.w2) / 2, a = 2, U = gr.w2 / 2 + 1.5, d = 1.0, t = 0.2;
            const int steps = 4, N = gr.periods;
            auto kappaOf = [&](int i) { return std::pow((i + 0.5) / steps, 2); };
            auto num = [](double v) { return QString::number(v, 'g', 17); };
            if (fem11) {
                auto rect = [&](const QString& name, double x, double y, double w, double h) {
                    return QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")}, {QStringLiteral("name"), name},
                                       {QStringLiteral("position"), pairOf(x, y)}, {QStringLiteral("size"), pairOf(w, h)}};
                };
                const double xe = a + N * L + a;
                QJsonArray geometry{rect(QStringLiteral("clear"), 0, -U, xe, 2 * U)}, cores{QStringLiteral("coreIn"), QStringLiteral("coreOut")};
                QJsonArray ports1{QStringLiteral("clear"), QStringLiteral("coreIn")}, ports2{QStringLiteral("clear"), QStringLiteral("coreOut")};
                QJsonArray materials{QJsonObject{{QStringLiteral("label"), QStringLiteral("oxide")}, {QStringLiteral("n"), num(no)},
                                                 {QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("clear")}}}}}};
                for (int i = 0; i < steps; ++i) {
                    const QString lo = QStringLiteral("abL%1").arg(i), hi = QStringLiteral("abH%1").arg(i);
                    geometry.append(rect(lo, 0, -U - (i + 1) * d / steps, xe, d / steps));
                    geometry.append(rect(hi, 0, U + i * d / steps, xe, d / steps));
                    for (QJsonArray* p : {&ports1, &ports2}) {
                        p->append(lo);
                        p->append(hi);
                    }
                    materials.append(QJsonObject{{QStringLiteral("label"), QStringLiteral("absorber%1").arg(i)}, {QStringLiteral("n"), num(no)},
                                                 {QStringLiteral("kappa"), num(kappaOf(i))},
                                                 {QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), QJsonArray{lo, hi}}}}});
                }
                geometry.append(rect(QStringLiteral("coreIn"), 0, -wa / 2, a, wa));
                for (int p = 0; p < N; ++p) {
                    geometry.append(rect(QStringLiteral("w%1").arg(p), a + p * L, -gr.w2 / 2, L / 2, gr.w2));
                    geometry.append(rect(QStringLiteral("n%1").arg(p), a + p * L + L / 2, -gr.w1 / 2, L / 2, gr.w1));
                    cores.append(QStringLiteral("w%1").arg(p));
                    cores.append(QStringLiteral("n%1").arg(p));
                }
                geometry.append(rect(QStringLiteral("coreOut"), a + N * L, -wa / 2, a, wa));
                materials.append(QJsonObject{{QStringLiteral("label"), QStringLiteral("core")}, {QStringLiteral("n"), num(gr.nc)},
                                             {QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), cores}}}});
                QStringList fs;
                for (double l : lambdas) fs << num(C0 / (l * 1e-6));
                auto port = [](const QString& name, const QJsonArray& of, const QString& side, bool excite) {
                    return QJsonObject{{QStringLiteral("type"), QStringLiteral("port")}, {QStringLiteral("name"), name}, {QStringLiteral("excite"), excite},
                                       {QStringLiteral("selection"), QJsonObject{{QStringLiteral("of"), of}, {QStringLiteral("side"), side}}}};
                };
                const QJsonObject root{
                    {QStringLiteral("format"), QStringLiteral("qucs-s multiphysics 1")},
                    {QStringLiteral("components"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("unit"), QStringLiteral("um")},
                         {QStringLiteral("geometry"), geometry},
                         {QStringLiteral("materials"), materials},
                         {QStringLiteral("physics"),
                          QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("waves")},
                                                 {QStringLiteral("tag"), QStringLiteral("ewfd")},
                                                 {QStringLiteral("polarization"), gr.hz ? QStringLiteral("hz") : QStringLiteral("ez")},
                                                 {QStringLiteral("order"), QStringLiteral("2")},
                                                 {QStringLiteral("features"), QJsonArray{port(QStringLiteral("1"), ports1, QStringLiteral("left"), true),
                                                                                         port(QStringLiteral("2"), ports2, QStringLiteral("right"), false)}}}}},
                         {QStringLiteral("mesh"),
                          QJsonObject{{QStringLiteral("size"), QStringLiteral("normal")},
                                      {QStringLiteral("custom"), true},
                                      {QStringLiteral("hmax"), QStringLiteral("0.06")},
                                      {QStringLiteral("features"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("size")},
                                                                                          {QStringLiteral("level"), QStringLiteral("domain")},
                                                                                          {QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), cores}}},
                                                                                          {QStringLiteral("hmax"), QStringLiteral("0.02")}}}}}}}}},
                    {QStringLiteral("studies"),
                     QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("study")},
                                            {QStringLiteral("steps"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("frequency")},
                                                                                             {QStringLiteral("frequencies"), fs.join(QLatin1String(", "))}}}}}}}};
                QString why;
                std::optional<Model> m2 = Model::fromJson(QJsonDocument(root).toJson(), &why);
                if (!m2) qFatal("%s", qPrintable(why));
                auto ps2 = evaluateParameters(*m2);
                const GeometryBuild g2 = buildGeometry(*m2, *ps2);
                if (!g2.ok()) qFatal("the 2D grating's geometry");
                StudyRun run2 = runStudy(*m2, ps2, g2.topology, nullptr, QString(), {});
                if (!run2.ok()) qFatal("%s", qPrintable(run2.error));
                auto s2 = run2.solutions.back();
                for (std::size_t k = 0; k < lambdas.size(); ++k) {
                    s2->select(int(k));
                    fem11->push_back(std::norm(cd(*s2->global(QStringLiteral("ewfd.S11")), *s2->global(QStringLiteral("ewfd.S11_im")))));
                    fem21->push_back(std::norm(cd(*s2->global(QStringLiteral("ewfd.S21")), *s2->global(QStringLiteral("ewfd.S21_im")))));
                }
            }
            // The expansion: the cores extruded through the window's height.
            Model m = model3d();
            const int drawnPeriods = drawn ? N : 1;
            addBox(m, QStringLiteral("oxide"), 0, -U, 0, a + drawnPeriods * L + a, 2 * U, t);
            QStringList cores{QStringLiteral("coreIn"), QStringLiteral("coreOut")};
            addBox(m, QStringLiteral("coreIn"), 0, -wa / 2, 0, a, wa, t);
            for (int p = 0; p < drawnPeriods; ++p) {
                addBox(m, QStringLiteral("w%1").arg(p), a + p * L, -gr.w2 / 2, 0, L / 2, gr.w2, t);
                addBox(m, QStringLiteral("n%1").arg(p), a + p * L + L / 2, -gr.w1 / 2, 0, L / 2, gr.w1, t);
                cores << QStringLiteral("w%1").arg(p) << QStringLiteral("n%1").arg(p);
            }
            addBox(m, QStringLiteral("coreOut"), a + drawnPeriods * L, -wa / 2, 0, a, wa, t);
            addMaterial(m, QStringLiteral("oxide"), num(no), {QStringLiteral("oxide")});
            addMaterial(m, QStringLiteral("core"), num(gr.nc), cores);
            Node eme = m.make(QStringLiteral("eme"));
            eme.props.insert(QStringLiteral("window"), QStringLiteral("box"));
            eme.props.insert(QStringLiteral("across"), pairOf(-U, U));
            eme.props.insert(QStringLiteral("height"), pairOf(0, t));
            eme.props.insert(QStringLiteral("sides"), QStringLiteral("absorbing"));
            eme.props.insert(QStringLiteral("floors"), gr.hz ? QStringLiteral("pmc") : QStringLiteral("pec"));
            eme.props.insert(QStringLiteral("absorberthickness"), num(d));
            eme.props.insert(QStringLiteral("absorberkappa"), QStringLiteral("1"));
            eme.props.insert(QStringLiteral("absorbersteps"), steps);
            eme.props.insert(QStringLiteral("modes"), gr.modes);
            eme.props.insert(QStringLiteral("resolution"), gr.resolution);
            const QString tag = eme.tag;
            m.add(m.component().tag, eme);
            for (const char* end : {"start", "end"}) {
                Node p = m.make(QStringLiteral("eme/port"));
                p.props.insert(QStringLiteral("name"), QLatin1String(end) == QLatin1String("start") ? QStringLiteral("1") : QStringLiteral("2"));
                p.props.insert(QStringLiteral("end"), QString::fromLatin1(end));
                p.props.insert(QStringLiteral("modes"), QStringLiteral("1"));
                m.add(tag, p);
            }
            if (!drawn) {
                Node per = m.make(QStringLiteral("eme/periodic"));
                per.props.insert(QStringLiteral("from"), num(a));
                per.props.insert(QStringLiteral("to"), num(a + L));
                per.props.insert(QStringLiteral("periods"), QString::number(N));
                m.add(tag, per);
            }
            const Built b = build(m);
            for (double l : lambdas) {
                EmeDevice device;
                QString why;
                if (!readEmeDevice(m, *m.find(tag), *b.topology, *b.parameters, C0 / (l * 1e-6), &device, &why)) qFatal("%s", qPrintable(why));
                const EmeResult r = solveEme(m, b.parameters, *b.topology, device, C0 / (l * 1e-6));
                if (!r.ok()) qFatal("%s", qPrintable(r.error));
                eme11->push_back(r.S(0, 0));
                eme21->push_back(r.S(1, 0));
            }
        };
        // E along the steps.
        {
            const Grating gr{0.42, 0.50, 2.0, false, 30, 30, QStringLiteral("40")};
            const std::vector<double> lambdas{1.535, 1.550, 1.565};
            std::vector<double> f11, f21;
            std::vector<cd> e11, e21, d11, d21;
            solve(gr, lambdas, false, &f11, &f21, &e11, &e21);
            solve(gr, {1.550}, true, nullptr, nullptr, &d11, &d21);
            for (std::size_t k = 0; k < lambdas.size(); ++k) {
                qInfo("grating, E along its steps, %.3f µm: |S11|² %.6f |S21|² %.6f; the 2D solve's %.6f %.6f", lambdas[k], std::norm(e11[k]), std::norm(e21[k]),
                      f11[k], f21[k]);
                QVERIFY2(std::abs(std::norm(e11[k]) - f11[k]) < 5e-4 && std::abs(std::norm(e21[k]) - f21[k]) < 5e-4,
                         qPrintable(QStringLiteral("%1 %2").arg(std::norm(e11[k]) - f11[k]).arg(std::norm(e21[k]) - f21[k])));
            }
            qInfo("drawn tooth by tooth: |S11 - the section's| %.2g", std::abs(d11[0] - e11[1]));
            QVERIFY(std::abs(d11[0] - e11[1]) < 1e-9 && std::abs(d21[0] - e21[1]) < 1e-9);
        }
        // E across them.
        {
            const Grating gr{0.49, 0.51, 2.84778, true, 100, 30, QStringLiteral("30")};
            std::vector<double> f11, f21;
            std::vector<cd> e11, e21;
            solve(gr, {1.550}, false, &f11, &f21, &e11, &e21);
            const double R = std::norm(e11[0]);
            qInfo("grating, E across its steps: |S11|² %.4f; the 2D solve's %.4f; the effective indices' estimate 0.607", R, f11[0]);
            QVERIFY2(std::abs(R - f11[0]) < 0.03, qPrintable(QString::number(R - f11[0])));
            QVERIFY(R < 0.4 * 0.607);
        }
    }

    // A slab's step against the 2D full-wave solver: silicon 0.22 µm thick
    // into 0.5 µm, centred on z = 0, in oxide that absorbs beyond |z| = 0.5
    // µm (κ 0.3), metal at ±1.5 µm, 9 µm either side; TE (E along the slab) -
    // the EME on a 3D model a slice across, its sides metal; the 2D model
    // the same in the plane (x, z), Ez out of it, ports at both ends. The
    // absorber takes what the step radiates before it reaches a port, which
    // takes the first mode alone (nearer, the 2D solve's S21 swings with
    // the distance: 0.955, 0.943, 0.945, 0.948 at 2, 3, 4, 6 µm). Their |S21|²
    // within 0.1 % and |S11|² within 3 % of each other.
    void aSlabStepAgreesWithTheFullWaveSolve()
    {
        if (slowBuild()) QSKIP("a sanitized build: the 2D solve's 113,000 triangles take minutes");
        const double L = 9;
        // The 2D model.
        const QString json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "lossb", "position": ["%2", "-1.5"], "size": ["%3", "1.0"]},
                         {"type": "rectangle", "name": "losst", "position": ["%2", "0.5"], "size": ["%3", "1.0"]},
                         {"type": "rectangle", "name": "clad", "position": ["%2", "-0.5"], "size": ["%3", "1.0"]},
                         {"type": "rectangle", "name": "c1", "position": ["%2", "-0.11"], "size": ["%1", "0.22"]},
                         {"type": "rectangle", "name": "c2", "position": ["0", "-0.25"], "size": ["%1", "0.5"]}],
            "materials": [{"label": "oxide", "n": "1.444", "selection": {"objects": ["clad"]}},
                          {"label": "lossy", "n": "1.444", "kappa": "0.3", "selection": {"objects": ["lossb", "losst"]}},
                          {"label": "silicon", "n": "3.476", "selection": {"objects": ["c1", "c2"]}}],
            "physics": [{"type": "waves", "tag": "ewfd", "polarization": "ez", "order": "2", "features": [
                {"type": "port", "name": "1", "selection": {"of": ["clad", "c1", "lossb", "losst"], "side": "left"}},
                {"type": "port", "name": "2", "excite": false, "selection": {"of": ["clad", "c2", "lossb", "losst"], "side": "right"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.06",
                     "features": [{"type": "size", "level": "domain", "selection": {"objects": ["c1", "c2"]}, "hmax": "0.02"}]}}],
          "studies": [{"type": "study", "steps": [{"type": "frequency", "frequencies": "193.414489032[THz]"}]}]})json")
                                 .arg(L)
                                 .arg(-L)
                                 .arg(2 * L);
        QString why;
        std::optional<Model> m2 = Model::fromJson(json.toUtf8(), &why);
        QVERIFY2(m2, qPrintable(why));
        auto ps2 = evaluateParameters(*m2);
        const GeometryBuild g2 = buildGeometry(*m2, *ps2);
        QVERIFY(g2.ok());
        StudyRun run2 = runStudy(*m2, ps2, g2.topology, nullptr, QString(), {});
        QVERIFY2(run2.ok(), qPrintable(run2.error));
        auto s2 = run2.solutions.back();
        s2->select(0);
        const cd fem21(*s2->global(QStringLiteral("ewfd.S21")), *s2->global(QStringLiteral("ewfd.S21_im")));
        const cd fem11(*s2->global(QStringLiteral("ewfd.S11")), *s2->global(QStringLiteral("ewfd.S11_im")));
        // The EME's.
        Model m = model3d();
        const double W = 0.2;
        addBox(m, QStringLiteral("lossb"), 0, -W / 2, -1.5, 2 * L, W, 1.0);
        addBox(m, QStringLiteral("losst"), 0, -W / 2, 0.5, 2 * L, W, 1.0);
        addBox(m, QStringLiteral("oxide"), 0, -W / 2, -0.5, 2 * L, W, 1.0);
        addBox(m, QStringLiteral("core1"), 0, -W / 2, -0.11, L, W, 0.22);
        addBox(m, QStringLiteral("core2"), L, -W / 2, -0.25, L, W, 0.5);
        addMaterial(m, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("oxide")});
        {
            Node mat = m.make(QStringLiteral("material"));
            mat.label = QStringLiteral("lossy");
            mat.props.insert(QStringLiteral("n"), QStringLiteral("1.444"));
            mat.props.insert(QStringLiteral("kappa"), QStringLiteral("0.3"));
            mat.props.insert(QStringLiteral("selection"), objects({QStringLiteral("lossb"), QStringLiteral("losst")}));
            m.add(m.materials().tag, mat);
        }
        addMaterial(m, QStringLiteral("silicon"), QStringLiteral("3.476"), {QStringLiteral("core1"), QStringLiteral("core2")});
        const Built b = build(m);
        EmeDevice d = deviceOf(*b.topology, 40, 20);
        d.settings.sides = QStringLiteral("pec");
        const EmeResult r = solveEme(m, b.parameters, *b.topology, d, C0 / 1.55e-6);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const cd eme21 = r.S(1, 0), eme11 = r.S(0, 0);
        qInfo("slab step: 2D FEM |S21|² %.6f |S11|² %.5f; EME |S21|² %.6f |S11|² %.5f", std::norm(fem21), std::norm(fem11), std::norm(eme21),
              std::norm(eme11));
        QVERIFY2(std::abs(std::norm(eme21) / std::norm(fem21) - 1) < 1e-3, qPrintable(QString::number(std::norm(eme21) / std::norm(fem21) - 1)));
        QVERIFY2(std::abs(std::norm(eme11) / std::norm(fem11) - 1) < 0.03, qPrintable(QString::number(std::norm(eme11) / std::norm(fem11) - 1)));
        // The same absorber as the cut's absorbing walls (the window the
        // oxide |z| < 0.5 µm alone, the walls 1 µm of it at κ 0.3 in one
        // step, metal beyond): the same S-parameters.
        Model ma = model3d();
        addBox(ma, QStringLiteral("oxide"), 0, -W / 2, -0.5, 2 * L, W, 1.0);
        addBox(ma, QStringLiteral("core1"), 0, -W / 2, -0.11, L, W, 0.22);
        addBox(ma, QStringLiteral("core2"), L, -W / 2, -0.25, L, W, 0.5);
        addMaterial(ma, QStringLiteral("oxide"), QStringLiteral("1.444"), {QStringLiteral("oxide")});
        addMaterial(ma, QStringLiteral("silicon"), QStringLiteral("3.476"), {QStringLiteral("core1"), QStringLiteral("core2")});
        const Built ba = build(ma);
        EmeDevice da = deviceOf(*ba.topology, 40, 20);
        da.settings.sides = QStringLiteral("pec");
        da.settings.floors = QStringLiteral("absorbing");
        da.settings.absorberThickness = 1.0;
        da.settings.absorberKappa = 0.3;
        da.settings.absorberLayers = 1;
        const EmeResult ra = solveEme(ma, ba.parameters, *ba.topology, da, C0 / 1.55e-6);
        QVERIFY2(ra.ok(), qPrintable(ra.error));
        qInfo("slab step, absorbing walls: |S21|² %.6f |S11|² %.5f", std::norm(ra.S(1, 0)), std::norm(ra.S(0, 0)));
        QVERIFY2(std::abs(std::norm(ra.S(1, 0)) / std::norm(eme21) - 1) < 1e-4, qPrintable(QString::number(std::norm(ra.S(1, 0)) / std::norm(eme21) - 1)));
        QVERIFY2(std::abs(std::norm(ra.S(0, 0)) / std::norm(eme11) - 1) < 0.01, qPrintable(QString::number(std::norm(ra.S(0, 0)) / std::norm(eme11) - 1)));
        // Port 1's mode, of both: the slab's TE0, by its transcendental
        // equation, 2.8477822 (2.8477783: what the absorber 0.39 µm from
        // the core takes of it and the mesh's error within 2e-5).
        const double nSlab = slabTE0(3.476, 1.444, 0.22, 1.55);
        for (const EmeResult* e : {&r, &ra}) {
            const auto pm = std::find_if(e->portModes.begin(), e->portModes.end(),
                                         [](const EmeResult::PortMode& p) { return p.port == 0 && p.label == QLatin1String("TE0"); });
            QVERIFY(pm != e->portModes.end());
            qInfo("slab TE0: neff %.7f%+.2gj against %.7f", pm->neff.real(), pm->neff.imag(), nSlab);
            QVERIFY2(std::abs(pm->neff.real() - nSlab) < 2e-5, qPrintable(QString::number(pm->neff.real() - nSlab)));
        }
    }
};

QTEST_GUILESS_MAIN(TestFem3Eme)
#include "test_fem3_eme.moc"
