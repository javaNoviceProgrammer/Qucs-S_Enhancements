/*
 * test_fem_resonator.cpp - the multiphysics solver's resonator (a body of
 *                          revolution's modes, one azimuthal order at a
 *                          time) against what is known: a metal
 *                          cylinder's TE and TM modes, a dielectric
 *                          sphere's Mie resonances and their Q
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The figures checked are worked here: the cylinder's of Bessel's zeros,
 * the sphere's of Mie's coefficients' poles (Bohren and Huffman's, the
 * spherical Bessel functions in closed form for a complex argument, their
 * zeros by Newton's steps).
 */
#include "fem_mesh.h"
#include "fem_problem.h"
#include "fem_results.h"

#include <QFile>
#include <QRegularExpression>
#include <QtTest>

#include <cmath>
#include <complex>
#include <functional>

using namespace qucs_s::fem;

namespace {

constexpr double C = 299792458.0;
using cd = std::complex<double>;

bool slowBuild()
{
    // (QUCS_TEST_SLOW: a slow build's path in any, to check it quickly.)
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

Model modelOf(const QByteArray& json)
{
    QString why;
    std::optional<Model> m = Model::fromJson(json, &why);
    if (!m) qFatal("not a model: %s", qPrintable(why));
    if (!why.isEmpty()) qWarning("%s", qPrintable(why));
    return *m;
}

struct Run {
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<Solution> solution;
    StudyRun study;
    QString error;
};

Run run(const Model& model, const Progress& progress = {})
{
    Run r;
    r.parameters = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *r.parameters);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.study = runStudy(model, r.parameters, g.topology, nullptr, QString(), progress);
    if (!r.study.ok()) {
        r.error = r.study.error;
        return r;
    }
    r.solution = r.study.solutions.back();
    return r;
}

double global(const Solution& s, const QString& name)
{
    const std::optional<double> v = s.global(name);
    return v ? *v : NAN;
}

double at(const Solution& s, const QString& expression, QPointF p)
{
    const Locator locator(s.mesh());
    const std::optional<double> v = evaluateAt(s, locator, expression, p);
    return v ? *v : NAN;
}

/// Each mode a snapshot: its eigenfrequency (Hz) and Q.
struct Found {
    double f, Q;
};
std::vector<Found> modesOf(Solution& s)
{
    std::vector<Found> out;
    for (int k = 0; k < s.snapshotCount(); ++k) {
        s.select(k);
        out.push_back({global(s, QStringLiteral("emr.freq")), global(s, QStringLiteral("emr.Q"))});
    }
    return out;
}

// Spherical Bessel functions of the first and second kind, orders 0 to 4,
// of a complex argument; Riccati's ψ = z j, ξ = z h⁽²⁾ (outgoing, e^{jωt}).
cd sphj(int n, cd z)
{
    const cd s = std::sin(z), c = std::cos(z);
    switch (n) {
    case 0: return s / z;
    case 1: return s / (z * z) - c / z;
    case 2: return (3.0 / (z * z * z) - 1.0 / z) * s - 3.0 * c / (z * z);
    case 3: return (15.0 / std::pow(z, 4) - 6.0 / (z * z)) * s - (15.0 / (z * z * z) - 1.0 / z) * c;
    default: return (105.0 / std::pow(z, 5) - 45.0 / (z * z * z) + 1.0 / z) * s - (105.0 / std::pow(z, 4) - 10.0 / (z * z)) * c;
    }
}
cd sphy(int n, cd z)
{
    const cd s = std::sin(z), c = std::cos(z);
    switch (n) {
    case 0: return -c / z;
    case 1: return -c / (z * z) - s / z;
    case 2: return (-3.0 / (z * z * z) + 1.0 / z) * c - 3.0 * s / (z * z);
    case 3: return (-15.0 / std::pow(z, 4) + 6.0 / (z * z)) * c - (15.0 / (z * z * z) - 1.0 / z) * s;
    default: return (-105.0 / std::pow(z, 5) + 45.0 / (z * z * z) - 1.0 / z) * c - (105.0 / std::pow(z, 4) - 10.0 / (z * z)) * s;
    }
}
cd h2(int n, cd z) { return sphj(n, z) - cd(0, 1) * sphy(n, z); }
/// Bessel's J_n(x), its series (libc++ has no std::cyl_bessel_j).
double besselJ(int n, double x)
{
    double term = std::pow(x / 2, n) / std::tgamma(n + 1), sum = term;
    for (int k = 1; k < 60; ++k) {
        term *= -(x * x / 4) / (k * double(k + n));
        sum += term;
    }
    return sum;
}
cd psi(int n, cd z) { return z * sphj(n, z); }
cd dpsi(int n, cd z) { return sphj(n, z) + z * (sphj(n - 1, z) - double(n + 1) / z * sphj(n, z)); }
cd xi(int n, cd z) { return z * h2(n, z); }
cd dxi(int n, cd z) { return h2(n, z) + z * ((sphj(n - 1, z) - cd(0, 1) * sphy(n - 1, z)) - double(n + 1) / z * h2(n, z)); }
/// A sphere of index \a m's resonance of order n, TE (Mie's b_n) or TM
/// (a_n): x = k0 R, near \a guess.
cd mieRoot(bool te, int n, double m, cd guess)
{
    auto f = [&](cd x) {
        return te ? psi(n, m * x) * dxi(n, x) - m * xi(n, x) * dpsi(n, m * x) : m * psi(n, m * x) * dxi(n, x) - xi(n, x) * dpsi(n, m * x);
    };
    cd x = guess;
    for (int k = 0; k < 100; ++k) {
        const double h = 1e-7 * (1 + std::abs(x));
        const cd step = f(x) / ((f(x + h) - f(x - h)) / (2 * h));
        x -= step;
        if (std::abs(step) < 1e-14 * std::abs(x)) break;
    }
    return x;
}

/// A dielectric half-disk (a sphere's half section: radius 1 µm, index n)
/// in air, a perfectly matched frame round it; order m, about \a around.
QByteArray sphere(int m, const QString& n, const QString& hs, const QString& around, int count, double box = 4, double pml = 2,
                  const QString& ha = QStringLiteral("0.3"))
{
    return QStringLiteral(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um", "space": "axisymmetric",
        "geometry": [{"type": "rectangle", "name": "pml", "position": ["0", "%1"], "size": ["%2", "%3"]},
                     {"type": "rectangle", "name": "air", "position": ["0", "%4"], "size": ["%5", "%6"]},
                     {"type": "circle", "name": "ball", "center": ["0", "0"], "radius": "1", "sector": "180", "rotation": "-90"}],
        "materials": [{"label": "air", "n": "1", "selection": {"only": ["pml", "air"]}},
                      {"label": "glass", "n": "%7", "selection": {"only": ["ball"]}}],
        "physics": [{"type": "resonator", "tag": "emr", "m": %8, "features": [
            {"type": "wave", "selection": {"all": true}}, {"type": "pml", "selection": {"only": ["pml"]}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%12",
                 "features": [{"type": "size", "level": "domain", "selection": {"only": ["ball"]}, "hmax": "%9"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "eigenfrequency", "count": %10, "around": "%11"}]}]})json")
        .arg(-box - pml)
        .arg(box + pml)
        .arg(2 * (box + pml))
        .arg(-box)
        .arg(box)
        .arg(2 * box)
        .arg(n)
        .arg(m)
        .arg(hs)
        .arg(count)
        .arg(around)
        .arg(ha)
        .toUtf8();
}

/// The example vcsel_modes.qfem solved for order \a m (about \a around,
/// \a count modes, when given). In a slow build - the sanitized one, its
/// sparse LU's dense products some 150 times slower - the same cavity with
/// 8 and 6 pairs, 0.5 um along r.
Run vcselRun(int m, const QString& around = QString(), int count = 0)
{
    QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/vcsel_modes.qfem"));
    if (!f.open(QIODevice::ReadOnly)) qFatal("no example vcsel_modes.qfem");
    Model model = modelOf(f.readAll());
    Node* emr = model.find(QStringLiteral("emr"));
    if (!emr) qFatal("no physics emr");
    emr->props.insert(QStringLiteral("m"), m);
    if (!around.isEmpty()) {
        Node* step = nullptr;
        for (const Node* study : model.studies())
            for (const Node& st : study->children)
                if (st.type == QLatin1String("eigenfrequency")) step = model.find(st.tag);
        if (!step) qFatal("no eigenfrequency step");
        step->props.insert(QStringLiteral("around"), around);
        step->props.insert(QStringLiteral("count"), count);
    }
    if (slowBuild()) {
        for (Node& feature : model.find(model.mesh().tag)->children) {
            feature.props.insert(QStringLiteral("hlateral"), QStringLiteral("0.5"));
            feature.props.insert(QStringLiteral("count"),
                                 feature.label == QLatin1String("Mirrors") ? 2 : feature.label == QLatin1String("Cavity") ? 6 : 4);
        }
        QList<Model::Parameter> ps = model.parameters();
        for (Model::Parameter& p : ps) {
            if (p.name == QLatin1String("Nb")) p.expression = QStringLiteral("8");
            if (p.name == QLatin1String("Nt")) p.expression = QStringLiteral("6");
        }
        model.setParameters(ps);
    }
    return run(model);
}

} // namespace

class TestFemResonator : public QObject
{
    Q_OBJECT

private slots:
    // A metal (PEC) cylinder, radius 10 mm, 20 mm high: its TM_mnp modes at
    // k = √((j_mn/a)² + (pπ/h)²), TE_mnp at j'_mn - for m 0, 1 and 2 the
    // eight nearest 15 GHz each one of them, none missing, none more; Q
    // infinite. m 0's TE modes E round the axis alone, its TM ones none.
    void aMetalCylindersModes()
    {
        const double a = 0.010, h = 0.020;
        // (Bessel's zeros j_mn and j'_mn.)
        const std::vector<std::tuple<int, double, bool>> zeros = {
            {0, 2.404825557695773, false}, {0, 5.520078110286311, false}, {0, 3.831705970207512, true}, {0, 7.015586669815619, true},
            {1, 3.831705970207512, false}, {1, 7.015586669815619, false}, {1, 1.841183781340659, true}, {1, 5.331442773525033, true},
            {2, 5.135622301840683, false}, {2, 3.054236928227140, true}, {2, 6.706133194158459, true}};
        const bool slow = slowBuild();
        for (int m : {0, 1, 2}) {
            std::vector<double> exact;
            for (const auto& [mm, z, te] : zeros)
                if (mm == m)
                    for (int p = te ? 1 : 0; p <= 4; ++p) exact.push_back(std::hypot(z / a, p * M_PI / h) * C / (2 * M_PI));
            std::sort(exact.begin(), exact.end(), [](double x, double y) { return std::abs(x - 15e9) < std::abs(y - 15e9); });
            exact.resize(8);
            std::sort(exact.begin(), exact.end());
            const QByteArray json = QStringLiteral(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "mm", "space": "axisymmetric",
                "geometry": [{"type": "rectangle", "name": "cav", "position": ["0", "0"], "size": ["10", "20"]}],
                "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
                "physics": [{"type": "resonator", "tag": "emr", "m": %1, "features": [{"type": "wave", "selection": {"all": true}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
              "studies": [{"type": "study", "steps": [{"type": "eigenfrequency", "count": 8, "around": "15[GHz]"}]}]})json")
                                        .arg(m)
                                        .arg(slow ? QStringLiteral("2") : QStringLiteral("1"))
                                        .toUtf8();
            Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const std::vector<Found> got = modesOf(*r.solution);
            QCOMPARE(int(got.size()), 8);
            QStringList said;
            for (std::size_t k = 0; k < 8; ++k) {
                said << QStringLiteral("%1 (%2)").arg(got[k].f / 1e9, 0, 'f', 5).arg(exact[k] / 1e9, 0, 'f', 5);
                QVERIFY2(std::abs(got[k].f / exact[k] - 1) < (slow ? 2e-3 : 3e-4), qPrintable(said.join(QLatin1String(", "))));
                QVERIFY(std::isinf(got[k].Q));
            }
            qInfo("cylinder, m %d: %s GHz (exact)", m, qPrintable(said.join(QLatin1String(", "))));
            QCOMPARE(int(global(*r.solution, QStringLiteral("emr.m"))), m);
            if (m == 0) {
                // TE011 (19.759 GHz) E_φ alone; TM010 (11.474 GHz) none.
                Solution& s = *r.solution;
                for (int k = 0; k < s.snapshotCount(); ++k) {
                    s.select(k);
                    const double f = global(s, QStringLiteral("emr.freq")), phi = std::hypot(at(s, QStringLiteral("emr.Ephi"), QPointF(6, 7)),
                                                                                            at(s, QStringLiteral("emr.Ephi_im"), QPointF(6, 7)));
                    const double all = at(s, QStringLiteral("emr.normE"), QPointF(6, 7));
                    if (std::abs(f / 19.758999e9 - 1) < 2e-3) QVERIFY2(phi > 0.999 * all && all > 0, qPrintable(QStringLiteral("%1 %2").arg(phi).arg(all)));
                    if (std::abs(f / 11.474253e9 - 1) < 2e-3) {
                        QVERIFY2(phi < 1e-6 * all && all > 0, qPrintable(QStringLiteral("%1 %2").arg(phi).arg(all)));
                        // TM010's H: H_φ = j E_z(0) J1(kr) / η0 round the axis,
                        // E_z = E_z(0) J0(kr); H_r, H_z nought.
                        const QPointF p(5, 10);
                        const double x = 2.404825557695773 / 2, eta = 376.730313668;
                        const double ez = std::hypot(at(s, QStringLiteral("emr.Ez"), p), at(s, QStringLiteral("emr.Ez_im"), p));
                        const double hphi = std::hypot(at(s, QStringLiteral("emr.Hphi"), p), at(s, QStringLiteral("emr.Hphi_im"), p));
                        const double hr = std::hypot(at(s, QStringLiteral("emr.Hr"), p), at(s, QStringLiteral("emr.Hr_im"), p));
                        const double ratio = hphi / ez * eta, exact = besselJ(1, x) / besselJ(0, x);
                        qInfo("TM010: H_phi / E_z at r = a/2: %.6f / eta (exact %.6f), H_r %.2g", ratio, exact, hr / hphi);
                        QVERIFY2(std::abs(ratio / exact - 1) < (slow ? 2e-2 : 2e-3) && hr < 1e-3 * hphi, qPrintable(QStringLiteral("%1 %2").arg(ratio).arg(exact)));
                    }
                }
            }
        }
    }

    // A dielectric sphere (n 3.5, radius 1 µm) in air, a perfectly matched
    // frame round it: order 1's modes about 3.9 µm Mie's resonances - TE and
    // TM of orders 1 to 4 - their k0 R within 1e-3 (the sphere's polygon,
    // as h² - 2.5e-4 at half the size) and their Q (5 to 980) within 1 %.
    // Order 2 the same resonances of order 2 and more (a sphere's
    // degeneracy), none of order 1; a deeper frame the same Q.
    void aDielectricSphereAsMie()
    {
        const bool slow = slowBuild();
        const double n = 3.5;
        std::vector<std::tuple<QString, cd>> mie;
        for (bool te : {true, false})
            for (int l = 1; l <= 4; ++l)
                for (double x0 : {0.85, 1.21, 1.24, 1.55, 1.61, 1.76, 1.92, 1.97, 2.16, 2.2}) {
                    const cd x = mieRoot(te, l, n, cd(x0, 0.05));
                    if (!(x.real() > 0.2 && x.real() < 2.6 && x.imag() > 0 && x.imag() < 1)) continue;
                    bool known = false;
                    for (const auto& [name, y] : mie) known = known || std::abs(x - y) < 1e-6;
                    if (!known) mie.emplace_back(QStringLiteral("%1%2").arg(te ? QStringLiteral("TE") : QStringLiteral("TM")).arg(l), x);
                }
        auto check = [&](int m, const QString& hs, double box, double pml, int count, QStringList* names) {
            Run r = run(modelOf(sphere(m, QStringLiteral("3.5"), hs, QStringLiteral("3.9[um]"), count, box, pml,
                                       slow ? QStringLiteral("0.6") : QStringLiteral("0.3"))));
            if (!r.error.isEmpty()) return QStringLiteral("error: ") + r.error;
            QStringList said;
            for (const Found& f : modesOf(*r.solution)) {
                const double x = 2 * M_PI * f.f / C * 1e-6;
                const auto best = std::min_element(mie.begin(), mie.end(), [&](const auto& p, const auto& q) {
                    return std::abs(std::get<1>(p).real() - x) < std::abs(std::get<1>(q).real() - x);
                });
                const cd y = std::get<1>(*best);
                const double Q = y.real() / (2 * y.imag());
                said << QStringLiteral("%1 x %2 (%3) Q %4 (%5)").arg(std::get<0>(*best)).arg(x, 0, 'f', 5).arg(y.real(), 0, 'f', 5).arg(f.Q, 0, 'f', 2).arg(Q, 0, 'f', 2);
                if (std::abs(x / y.real() - 1) > (slow ? 4e-3 : 1.2e-3) || std::abs(f.Q / Q - 1) > (slow ? 0.03 : 0.01)) return said.join(QLatin1String("; "));
                if (names) *names << std::get<0>(*best);
            }
            qInfo("sphere, m %d: %s", m, qPrintable(said.join(QLatin1String("; "))));
            return QString();
        };
        // (A slow build - a sanitized Debug one, its LU some 60 times slower:
        // a coarser sphere in a smaller frame, its air coarser, 0.3 s here.)
        const QString hs = slow ? QStringLiteral("0.2") : QStringLiteral("0.1");
        QStringList one, two;
        QString bad = check(1, hs, slow ? 2.5 : 4, slow ? 1.5 : 2, slow ? 4 : 8, &one);
        QVERIFY2(bad.isEmpty(), qPrintable(bad));
        // (A slow build's four: the nearest 3.9 um - TM2, TE3, TE1, TM3.)
        if (slow)
            QVERIFY2(one.contains(QStringLiteral("TE1")) && one.contains(QStringLiteral("TM2")) && one.contains(QStringLiteral("TE3")), qPrintable(one.join(' ')));
        else
            QVERIFY2(one.contains(QStringLiteral("TE1")) && one.contains(QStringLiteral("TM1")) && one.contains(QStringLiteral("TE2")), qPrintable(one.join(' ')));
        if (!slow) {
            QVERIFY2(one.contains(QStringLiteral("TE3")) && one.contains(QStringLiteral("TE4")) && one.contains(QStringLiteral("TM3")), qPrintable(one.join(' ')));
            bad = check(2, hs, 4, 2, 6, &two);
            QVERIFY2(bad.isEmpty(), qPrintable(bad));
            for (const QString& name : two) QVERIFY2(!name.endsWith(QLatin1Char('1')), qPrintable(two.join(' ')));   // (order 1 has no m 2)
            QVERIFY2(two.contains(QStringLiteral("TE2")) && two.contains(QStringLiteral("TE3")), qPrintable(two.join(' ')));
            // A deeper frame, farther out: the same.
            bad = check(1, hs, 5, 3, 8, nullptr);
            QVERIFY2(bad.isEmpty(), qPrintable(bad));
        }
    }

    // The example vcsel_modes.qfem as its description says: order 1's HE11
    // at 978.52 nm, Q 16 000, the ring at 974.24 nm and HE12 at 973.43 nm
    // (its field highest on the axis, nought near r = 1 um). (A slow build
    // - the sanitized one -: the same cavity with 8 and 6 pairs, 0.5 um
    // along r, its order 1's modes found, near 980 nm, and by number; CI's
    // Release builds check the numbers.) Orders 0 and 2 below: three solves
    // in one function took CI's macOS runner past QtTest's 300 s, the
    // heavy tests run beside it.
    void theVcselExample()
    {
        const bool slow = slowBuild();
        const double tH = 0.98 / (4 * 3.52), tL = 0.98 / (4 * 2.98), tC = 0.98 / 3.4, z = 25 * (tH + tL) + tC / 2;
        Run one = vcselRun(1);
        QVERIFY2(one.error.isEmpty(), qPrintable(one.error));
        std::vector<double> lambda, Q;
        for (int k = 0; k < one.solution->snapshotCount(); ++k) {
            one.solution->select(k);
            lambda.push_back(global(*one.solution, QStringLiteral("emr.lambda0")) * 1e9);
            Q.push_back(global(*one.solution, QStringLiteral("emr.Q")));
        }
        qInfo("order 1: %.4f nm (Q %.0f), %.4f (%.0f), %.4f (%.0f)", lambda.at(0), Q.at(0), lambda.at(1), Q.at(1), lambda.at(2), Q.at(2));
        {
            // Each mode by its number (fem_evaluate's, a plot group's): that
            // mode, each at its own frequency; none asked, the first; a
            // frequency asked, the nearest's.
            const SolutionSet set{one.study.solutions, one.study.swept};
            for (int mode : {1, 2, 3}) {
                const std::vector<Instance> w = instances(set, false, 0, {}, {}, mode);
                QCOMPARE(int(w.size()), 1);
                QCOMPARE(w.front().snapshot, mode - 1);
                QCOMPARE(w.front().mode, mode);
            }
            QCOMPARE(instances(set, false).front().snapshot, 0);
            QCOMPARE(instances(set, false, 0, {}, QStringLiteral("%1[nm]").arg(lambda.at(1), 0, 'f', 3)).front().snapshot, 1);
            QCOMPARE(int(instances(set, true).size()), 3);
        }
        if (slow) {
            for (std::size_t k = 0; k < lambda.size(); ++k) QVERIFY(lambda[k] > 960 && lambda[k] < 1000 && Q[k] > 5);
            return;
        }
        QVERIFY(std::abs(lambda[0] - 978.5249) < 0.005 && std::abs(Q[0] / 15990.8 - 1) < 0.005);
        QVERIFY(std::abs(lambda[1] - 974.237) < 0.005 && std::abs(lambda[2] - 973.4269) < 0.005);
        // HE12: highest on the axis, nought near r = 1 um.
        one.solution->select(2);
        const double axis = at(*one.solution, QStringLiteral("emr.normE"), QPointF(0.2, z)), ring = at(*one.solution, QStringLiteral("emr.normE"), QPointF(1.0, z));
        QVERIFY2(axis > 10 * ring, qPrintable(QStringLiteral("%1 %2").arg(axis).arg(ring)));
    }

    // The same example's order 0: TM01 at 976.83 nm (E_r, no E_φ) and TE01
    // at 976.46 nm (E_φ alone); order 2's HE21 at 976.66 nm, four modes
    // about 980 nm, one of the absorbing layers' own left out, said. (Not
    // in a slow build: its order 1 alone, above.)
    void theVcselExamplesOtherOrders()
    {
        if (slowBuild()) QSKIP("a slow build checks the example's order 1 alone");
        const double tH = 0.98 / (4 * 3.52), tL = 0.98 / (4 * 2.98), tC = 0.98 / 3.4, z = 25 * (tH + tL) + tC / 2;
        auto norm = [z](Solution& s, double r, const char* what) {
            const QString e = QStringLiteral("emr.") + QLatin1String(what);
            return std::hypot(at(s, e, QPointF(r, z)), at(s, e + QStringLiteral("_im"), QPointF(r, z)));
        };
        // Order 0: TM01, then TE01, by their fields.
        Run zero = vcselRun(0);
        QVERIFY2(zero.error.isEmpty(), qPrintable(zero.error));
        double tm = 0, te = 0;
        for (int k = 0; k < zero.solution->snapshotCount(); ++k) {
            Solution& s = *zero.solution;
            s.select(k);
            if (global(s, QStringLiteral("emr.Q")) < 1000) continue;   // (the substrate's leaky one)
            const double phi = norm(s, 1.0, "Ephi"), all = at(s, QStringLiteral("emr.normE"), QPointF(1.0, z));
            const double l = global(s, QStringLiteral("emr.lambda0")) * 1e9;
            if (phi < 0.01 * all) tm = l;
            if (phi > 0.99 * all) te = l;
        }
        qInfo("order 0: TM01 %.4f nm, TE01 %.4f nm", tm, te);
        QVERIFY(tm > 0 && te > 0);
        QVERIFY(std::abs(tm - 976.834) < 0.005 && std::abs(te - 976.457) < 0.005);
        // Order 2: HE21 between them - four about 980 nm, one of the
        // absorbing layers' own among them left out, said.
        Run two = vcselRun(2, QStringLiteral("980[nm]"), 4);
        QVERIFY2(two.error.isEmpty(), qPrintable(two.error));
        QVERIFY2(two.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("modes of the absorbing layers left out")),
                 qPrintable(two.solution->log.join(QLatin1Char('\n'))));
        QCOMPARE(two.solution->snapshotCount(), 4);
        double he21 = 0, best = 0;   // (the highest Q: the next, 971.36 nm, Q 8 100)
        for (int k = 0; k < two.solution->snapshotCount(); ++k) {
            two.solution->select(k);
            const double q = global(*two.solution, QStringLiteral("emr.Q"));
            if (q > best) best = q, he21 = global(*two.solution, QStringLiteral("emr.lambda0")) * 1e9;
        }
        qInfo("order 2: HE21 %.4f nm", he21);
        QVERIFY(std::abs(he21 - 976.657) < 0.005);
    }

    // The azimuthal order m as an expression of the parameters, so that a
    // study's sweep runs over the orders - one solution per order, each
    // with its own emr.m. A whispering gallery's or a bend's loss per order
    // needed a script that edited the model and solved it again, order by
    // order (the user's bend study, 3). An order that is no whole number is
    // still refused, now by what the expression comes to.
    void theOrderIsSwept()
    {
        const QString json = QStringLiteral(R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "morder", "expression": "%1"}],
          "components": [{"unit": "mm", "space": "axisymmetric",
            "geometry": [{"type": "rectangle", "name": "cav", "position": ["0", "0"], "size": ["10", "20"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
            "physics": [{"type": "resonator", "tag": "emr", "m": "%2", "features": [{"type": "wave", "selection": {"all": true}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "%3"}}],
          "studies": [{"type": "study", "steps": [%4{"type": "eigenfrequency", "count": 2, "around": "15[GHz]"}]}]})json");
        const QString mesh = slowBuild() ? QStringLiteral("3") : QStringLiteral("2");
        const Run r = run(modelOf(json.arg(QStringLiteral("0"), QStringLiteral("morder"), mesh,
                                           QStringLiteral(R"({"type": "sweep", "parameters": [["morder", "0, 1, 2", ""]]}, )"))
                                      .toUtf8()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 3);
        QStringList said;
        double before = 0;
        for (int p = 0; p < 3; ++p) {
            Solution& s = *r.study.solutions[std::size_t(p)];
            QCOMPARE(s.sweptValues.value(QStringLiteral("morder")), double(p));
            QCOMPARE(int(global(s, QStringLiteral("emr.m"))), p);
            QVERIFY(s.snapshotCount() >= 1);
            s.select(0);
            const double f = global(s, QStringLiteral("emr.freq"));
            said << QStringLiteral("m %1: %2 GHz").arg(p).arg(f / 1e9, 0, 'f', 4);
            // Each order has its own modes: the cylinder's lowest near
            // 15 GHz moves with it.
            QVERIFY2(std::abs(f / before - 1) > 0.01, qPrintable(said.join(QLatin1String(", "))));
            before = f;
        }
        qInfo("swept over the orders: %s", qPrintable(said.join(QLatin1String(", "))));
        // What it comes to must still be a whole number.
        const Run half = run(modelOf(json.arg(QStringLiteral("3"), QStringLiteral("morder/2"), mesh, QString()).toUtf8()));
        QVERIFY2(half.error.contains(QLatin1String("whole number")), qPrintable(half.error));
    }

    // On the axis: order 1's field E_φ = -j E_r there (its two transverse
    // parts one circularly turning field; 3 % off by the mesh at 2 nm from
    // it), E_z nought; the curl's 1/r parts finite. Refused, and said: a 2D (not axisymmetric) component, a
    // geometry reaching x < 0, an Eigenfrequency step without a resonator,
    // what it searches about not a frequency or length; m not whole, a
    // count not from 1 to 200, an element order not 1 or 2, a Mode Analysis
    // step on it, a reflection not between 0 and 1. The layers' reflection
    // its strength.
    // A resonator's solve stops when asked: before and after its
    // factorization and before each product of its search. It looked
    // nowhere, so a model's close and fem_stop waited out the whole solve
    // (vcsel_modes 17 s, minutes under the sanitizers: the fuzzing of
    // 2026-10-16).
    void stopsWhenAsked()
    {
        const Model cavity = modelOf(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm", "space": "axisymmetric",
            "geometry": [{"type": "rectangle", "name": "cav", "position": ["0", "0"], "size": ["10", "20"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
            "physics": [{"type": "resonator", "tag": "emr", "m": 1, "features": [{"type": "wave", "selection": {"all": true}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "steps": [{"type": "eigenfrequency", "count": 4, "around": "15[GHz]"}]}]})json");
        const QLatin1String told("the eigenfrequencies");
        int checks = 0;
        const Run all = run(cavity, [&checks, told](double, const QString& what) {
            checks += what.contains(told) ? 1 : 0;
            return true;
        });
        QVERIFY2(all.error.isEmpty(), qPrintable(all.error));
        static const QRegularExpression productsRe(QStringLiteral("\\((\\d+) products"));
        const QRegularExpressionMatch m = productsRe.match(all.solution->log.join(QLatin1Char('\n')));
        QVERIFY2(m.hasMatch(), qPrintable(all.solution->log.join(QLatin1Char('\n'))));
        const int products = m.captured(1).toInt();
        QVERIFY(products > 10);
        // (Told once as it starts, asked before and after the factorization
        // and before each product.)
        QCOMPARE(checks, 3 + products);
        for (int at : {1, 2, 3, 4, products / 2, products + 3}) {
            int seen = 0;
            const Run part = run(cavity, [&seen, at, told](double, const QString& what) {
                if (!what.contains(told)) return true;
                return ++seen < at;
            });
            QVERIFY2(part.error.contains(QLatin1String("cancelled")), qPrintable(QStringLiteral("at %1: %2").arg(at).arg(part.error)));
            QCOMPARE(seen, at);
        }
    }

    // (Three functions, each under QtTest's limit: as one, 1,368 s on CI's
    // sanitized Linux, its limit 900.)
    void theAxis()
    {
        Run r = run(modelOf(sphere(1, QStringLiteral("3.5"), slowBuild() ? QStringLiteral("0.15") : QStringLiteral("0.1"), QStringLiteral("5.05[um]"), 1)));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        s.select(0);
        const QPointF near(0.002, 0.3);
        const cd er(at(s, QStringLiteral("emr.Er"), near), at(s, QStringLiteral("emr.Er_im"), near));
        const cd ephi(at(s, QStringLiteral("emr.Ephi"), near), at(s, QStringLiteral("emr.Ephi_im"), near));
        const cd ez(at(s, QStringLiteral("emr.Ez"), near), at(s, QStringLiteral("emr.Ez_im"), near));
        qInfo("by the axis: Er %g%+gj, Ephi %g%+gj, Ez %g", er.real(), er.imag(), ephi.real(), ephi.imag(), std::abs(ez));
        QVERIFY2(std::abs(er) > 0.05 && std::abs(ephi + cd(0, 1) * er) < 0.05 * std::abs(er), qPrintable(QStringLiteral("%1 %2").arg(std::abs(er)).arg(std::abs(ephi + cd(0, 1) * er))));
        QVERIFY(std::abs(ez) < 0.05 * std::abs(er));
        const double hr = at(s, QStringLiteral("emr.Hr"), QPointF(0, 0.3)), hz = at(s, QStringLiteral("emr.Hz"), QPointF(0, 0.3));
        QVERIFY2(std::isfinite(hr) && std::isfinite(hz) && std::abs(hr) < 1, qPrintable(QStringLiteral("%1 %2").arg(hr).arg(hz)));
    }

    void refusals()
    {
        Run r;
        QByteArray json = sphere(1, QStringLiteral("3.5"), QStringLiteral("0.3"), QStringLiteral("4[um]"), 2);
        json.replace(R"("space": "axisymmetric",)", "");
        r = run(modelOf(json));
        QVERIFY2(r.error.contains(QLatin1String("axisymmetric")), qPrintable(r.error));
        json = sphere(1, QStringLiteral("3.5"), QStringLiteral("0.3"), QStringLiteral("4[um]"), 2);
        json.replace(R"("rotation": "-90")", R"("rotation": "0")");
        r = run(modelOf(json));
        QVERIFY2(r.error.contains(QStringLiteral("r ≥ 0")), qPrintable(r.error));   // (the geometry's own refusal)
        json = sphere(1, QStringLiteral("3.5"), QStringLiteral("0.3"), QStringLiteral("4[V]"), 2);
        r = run(modelOf(json));
        QVERIFY2(r.error.contains(QLatin1String("searches about")), qPrintable(r.error));
        // (Each once taken as another, nothing said: m 1.5 as 0, a count of
        // 0 or 500 as 1 or 200, an element order 3 as 2, a Mode Analysis
        // step on a resonator as an Eigenfrequency step.)
        const QByteArray base = sphere(2, QStringLiteral("3.5"), QStringLiteral("0.3"), QStringLiteral("5.05[um]"), 2);
        for (const auto& [from, to, said] : {std::tuple{R"("m": 2)", R"("m": 1.5)", "whole number"},
                                            std::tuple{R"("count": 2)", R"("count": 0)", "from 1 to 200"},
                                            std::tuple{R"("count": 2)", R"("count": 500)", "from 1 to 200"},
                                            std::tuple{R"("m": 2)", R"("m": 2, "order": "3")", "one of 1, 2"},
                                            std::tuple{R"({"type": "eigenfrequency", "count": 2, "around": "5.05[um]"})",
                                                       R"({"type": "mode", "frequency": "59.4[THz]", "modes": 2})", "Eigenfrequency step"},
                                            std::tuple{R"({"type": "pml", "selection")", R"({"type": "pml", "reflection": "2", "selection")", "between 0 and 1"}}) {
            QByteArray j = base;
            QVERIFY2(j.contains(from), from);
            j.replace(from, to);
            r = run(modelOf(j));
            QVERIFY2(r.error.contains(QLatin1String(said)), qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(to), r.error)));
        }
    }

    // The absorbing layers' strength its reflection's: weaker (0.1 for
    // 1e-6), a mode's Q moved - the layers' own reflection in it.
    void theLayersReflection()
    {
        // (A sanitized build: coarser - 321 s there at 0.3 µm, some 1,000 on
        // CI's.)
        const bool slow = slowBuild();
        const QByteArray base = sphere(2, QStringLiteral("3.5"), slow ? QStringLiteral("0.45") : QStringLiteral("0.3"), QStringLiteral("5.05[um]"), 2, 4, 2,
                                       slow ? QStringLiteral("0.5") : QStringLiteral("0.3"));
        QByteArray weak = base;
        weak.replace(R"({"type": "pml", "selection")", R"({"type": "pml", "reflection": "0.1", "selection")");
        Run a = run(modelOf(base)), b = run(modelOf(weak));
        QVERIFY2(a.error.isEmpty() && b.error.isEmpty(), qPrintable(a.error + b.error));
        a.solution->select(0);
        b.solution->select(0);
        const double qa = global(*a.solution, QStringLiteral("emr.Q")), qb = global(*b.solution, QStringLiteral("emr.Q"));
        qInfo("Q with the layers reflecting 1e-6 %.6g, 0.1 %.6g", qa, qb);
        QVERIFY(std::isfinite(qa) && std::isfinite(qb) && std::abs(qb / qa - 1) > 1e-4);
    }
};

QTEST_GUILESS_MAIN(TestFemResonator)
#include "test_fem_resonator.moc"
