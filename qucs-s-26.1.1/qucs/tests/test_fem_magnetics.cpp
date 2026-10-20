/*
 * test_fem_magnetics.cpp - the multiphysics solver's magnetic fields
 *                          against what is known: a round wire's field and
 *                          inductance, a B-H curve by Newton's method, a
 *                          magnet's field inside it, a solenoid about its
 *                          axis, a wire's skin effect (Bessel functions), a
 *                          two-wire line's inductance matrix; coupled lines'
 *                          matrices, modes and exports; coils' roles
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_export.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QtTest>

#include <cmath>
#include <complex>

using namespace qucs_s::fem;

namespace {

constexpr double Mu0 = 1.25663706127e-6;
constexpr double SigmaCu = 5.998e7;
using cd = std::complex<double>;

/// A Debug or sanitized build (CI's Linux): ten to thirty times slower -
/// the examples solved at fewer frequencies and sweep points there.
bool slowBuild()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
#ifndef NDEBUG
    return true;
#else
    return false;
#endif
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
    std::shared_ptr<const Mesh> mesh;
    std::shared_ptr<Solution> solution;
    StudyRun study;
    QString error;
};

Run run(const Model& model)
{
    Run r;
    r.parameters = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *r.parameters);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.study = runStudy(model, r.parameters, g.topology, nullptr, QString());
    if (!r.study.ok()) {
        r.error = r.study.error;
        return r;
    }
    r.solution = r.study.solutions.back();
    r.mesh = r.solution->meshPtr();
    return r;
}

double at(const Run& r, const QString& expression, QPointF p)
{
    const Locator locator(*r.mesh);
    QString why;
    const std::optional<double> v = evaluateAt(*r.solution, locator, expression, p, &why);
    if (!v) qWarning("%s at (%g, %g): %s", qPrintable(expression), p.x(), p.y(), qPrintable(why));
    return v ? *v : NAN;
}

double global(const Run& r, const QString& name)
{
    const std::optional<double> v = r.solution ? r.solution->global(name) : std::nullopt;
    return v ? *v : NAN;
}

/// J_n(z) by its series (|z| of a few: a wire a few skin depths thick).
cd besselJ(int n, cd z)
{
    cd term = std::pow(z / 2.0, n);
    for (int k = 1; k <= n; ++k) term /= double(k);
    cd sum = term;
    const cd z2 = -(z * z) / 4.0;
    for (int k = 1; k < 200; ++k) {
        term *= z2 / (double(k) * double(k + n));
        sum += term;
        if (std::abs(term) < 1e-18 * std::abs(sum)) break;
    }
    return sum;
}

/// A round copper wire of radius 1 mm in a perfect conductor's circle of
/// radius 40 mm (A = 0 there): \a step its study's step.
QByteArray wire(const QString& step, const QString& radius = QStringLiteral("40"), const QString& wireMesh = QStringLiteral("0.2"))
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "mm",
        "geometry": [{"type": "circle", "name": "air", "radius": "%2"}, {"type": "circle", "name": "wire", "radius": "1"}],
        "materials": [{"material": "Air", "selection": {"only": ["air"]}}, {"material": "Copper", "selection": {"only": ["wire"]}}],
        "physics": [{"type": "magnetic", "tag": "mf", "features": [
          {"type": "coil", "name": "1", "I0": "10[A]", "selection": {"only": ["wire"]}}]}],
        "mesh": {"size": "fine", "features": [
          {"type": "size", "level": "domain", "selection": {"only": ["air"]}, "hmax": "1.5"},
          {"type": "size", "level": "domain", "selection": {"only": ["wire"]}, "hmax": "%3"}]}}],
      "studies": [{"type": "study", "steps": [%1]}]})json")
        .arg(step, radius, wireMesh)
        .toUtf8();
}

/// A ground-signal-signal-ground line: gold 5 µm thick on 300 µm of
/// silica, air above (2 m of it: the matrices per metre) - signals 20 µm
/// wide (the second \a wide2), 30 µm apart, 10 µm from grounds 100 µm
/// wide; the grounds one coil (G), each
/// signal its own; its elements \a hmax (µm) in the metals.  As its odd
/// mode's half-model (\a oddHalf): x ≥ 0, its signal coil 1, the ground
/// grounded, a ground (es) and A = 0 (mf) on the wall.
QByteArray gssg(const QString& hmax, const QString& frequencies, bool oddHalf = false, const QString& wide2 = QStringLiteral("20"))
{
    const QString metals = oddHalf ? QStringLiteral("\"s2\", \"gR\"") : QStringLiteral("\"s1\", \"s2\", \"gL\", \"gR\"");
    const QString geometry = oddHalf
                                 ? QStringLiteral(R"({"type": "rectangle", "name": "air", "position": ["0", "0"], "size": ["W/2", "H"]},
                      {"type": "rectangle", "name": "sub", "position": ["0", "-H"], "size": ["W/2", "H"]},
                      {"type": "rectangle", "name": "s2", "position": ["s/2", "0"], "size": ["Ws", "te"]},
                      {"type": "rectangle", "name": "gR", "position": ["s/2+Ws+g", "0"], "size": ["Wg", "te"]})")
                                 : QStringLiteral(R"({"type": "rectangle", "name": "air", "position": ["-W/2", "0"], "size": ["W", "H"]},
                      {"type": "rectangle", "name": "sub", "position": ["-W/2", "-H"], "size": ["W", "H"]},
                      {"type": "rectangle", "name": "s1", "position": ["-s/2-Ws", "0"], "size": ["Ws", "te"]},
                      {"type": "rectangle", "name": "s2", "position": ["s/2", "0"], "size": ["Ws2", "te"]},
                      {"type": "rectangle", "name": "gL", "position": ["-s/2-Ws-g-Wg", "0"], "size": ["Wg", "te"]},
                      {"type": "rectangle", "name": "gR", "position": ["s/2+Ws2+g", "0"], "size": ["Wg", "te"]})");
    const QString es = oddHalf ? QStringLiteral(R"({"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["s2"]}},
                      {"type": "ground", "selection": {"of": ["gR"]}}, {"type": "ground", "selection": {"of": ["air", "sub"], "side": "left"}})")
                               : QStringLiteral(R"({"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["s1"]}},
                      {"type": "terminal", "name": "2", "V0": "1[V]", "selection": {"of": ["s2"]}}, {"type": "ground", "selection": {"of": ["gL", "gR"]}})");
    const QString mf = oddHalf ? QStringLiteral(R"({"type": "coil", "name": "1", "I0": "1[A]", "selection": {"only": ["s2"]}},
                      {"type": "coil", "name": "G", "role": "grounded", "selection": {"only": ["gR"]}})")
                               : QStringLiteral(R"({"type": "coil", "name": "1", "I0": "1[A]", "selection": {"only": ["s1"]}},
                      {"type": "coil", "name": "2", "I0": "1[A]", "selection": {"only": ["s2"]}},
                      {"type": "coil", "name": "G", "I0": "1[A]", "selection": {"only": ["gL", "gR"]}})");
    const QString line = oddHalf ? QStringLiteral(R"("terminal": "1", "conductor": "1")") : QStringLiteral(R"("terminal": "1, 2", "conductor": "1, 2", "return": "G")");
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Ws", "expression": "20[um]"}, {"name": "Ws2", "expression": "%7[um]"}, {"name": "g", "expression": "10[um]"},
                     {"name": "s", "expression": "30[um]"}, {"name": "Wg", "expression": "100[um]"}, {"name": "te", "expression": "5[um]"},
                     {"name": "H", "expression": "300[um]"}, {"name": "W", "expression": "600[um]"}],
      "components": [{"unit": "um", "thickness": "2[m]",
        "geometry": [%1],
        "materials": [{"material": "Air", "selection": {"only": ["air"]}}, {"material": "Silicon dioxide", "selection": {"only": ["sub"]}},
                      {"material": "Gold", "selection": {"only": [%2]}}],
        "physics": [{"type": "electrostatics", "tag": "es", "selection": {"all": true, "except": {"objects": [%2]}}, "features": [%3]},
                    {"type": "magnetic", "tag": "mf", "reference": "%8", "features": [%4]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "40", "features": [
          {"type": "size", "level": "domain", "selection": {"only": [%2]}, "hmax": "%5"},
          {"type": "size", "level": "boundary", "selection": {"of": [%2]}, "hmax": "%11"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary", "physics": ["es"]}, {"type": "frequency", "physics": ["mf"], "frequencies": "%6"}]}],
      "results": {"derived": [{"type": "lineparams", "tag": "lp", "method": "rlgc", %9},
                              {"type": "lineparams", "tag": "lq", "method": "quasitem", "terminal": "%10"}]}})json")
        .arg(geometry, metals, es, mf, hmax, frequencies, wide2, oddHalf ? QString() : QStringLiteral("G"), line)
        .arg(oddHalf ? QStringLiteral("1") : QStringLiteral("1, 2"), QString::number(hmax.toDouble() / 2))
        .toUtf8();
}

} // namespace

class TestFemMagnetics : public QObject
{
    Q_OBJECT

private slots:
    void bhCurve()
    {
        QString why;
        auto c = BHCurve::make({{0.5, 100}, {1.0, 250}, {1.5, 1000}, {1.8, 8000}}, &why);
        QVERIFY2(c, qPrintable(why));
        // Through its points, rising everywhere, (0, 0) added.
        QCOMPARE(c->B().size(), std::size_t(5));
        QVERIFY(std::abs(c->H(1.0) - 250) < 1e-9);
        double last = -1;
        for (double b = 0; b < 2.5; b += 0.001) {
            const double h = c->H(b);
            QVERIFY2(h > last, qPrintable(QStringLiteral("H falls at B = %1").arg(b)));
            QVERIFY(c->slope(b) > 0);
            last = h;
        }
        // Beyond its last point as in vacuum.
        QVERIFY(std::abs(c->slope(2.0) * Mu0 - 1) < 1e-12);
        QVERIFY(std::abs(c->H(2.0) - (8000 + 0.2 / Mu0)) < 1e-6);
        // Its energy's derivative is H; its slope H's.
        for (double b : {0.2, 0.7, 1.2, 1.7, 2.1}) {
            const double d = 1e-6;
            QVERIFY(std::abs((c->energy(b + d) - c->energy(b - d)) / (2 * d) - c->H(b)) < 1e-4 * c->H(b));
            QVERIFY(std::abs((c->H(b + d) - c->H(b - d)) / (2 * d) - c->slope(b)) < 1e-4 * c->slope(b));
        }
        // ν at 0 is the first slope.
        QVERIFY(std::abs(c->nu(0) - 200) < 1e-9);
        QVERIFY(!BHCurve::make({{0.5, 100}, {0.4, 200}}, &why));
        QVERIFY(!BHCurve::make({{0, 10}, {0.4, 200}}, &why));
    }

    void wireAtRest()
    {
        // B = µ0 I / 2πr outside, µ0 I r / 2πa² inside; L = µ0/2π (ln(R/a) + 1/4)
        // per metre; R = 1 / (σ π a²).
        const Run r = run(modelOf(wire(QStringLiteral(R"json({"type": "stationary"})json"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double I = 10;
        for (double x : {3.0, 5.0, 10.0, 25.0}) {
            const double expected = Mu0 * I / (2 * M_PI * x * 1e-3);
            // (A gradient, what converges slowest: its mean round the circle -
            // Ampère's law - not one point's, which depends on the triangle it
            // falls in, and so on a platform's mesh.)
            double got = 0;
            double worst = 0;
            for (int k = 0; k < 24; ++k) {
                const double b = at(r, QStringLiteral("mf.normB"), QPointF(x * std::cos(k * M_PI / 12), x * std::sin(k * M_PI / 12)));
                got += b / 24;
                worst = std::max(worst, std::abs(b / expected - 1));
            }
            qInfo("wire: r = %g mm: the mean %.5f %% off, the worst point %.3f %%", x, (got / expected - 1) * 100, worst * 100);
            QVERIFY2(std::abs(got / expected - 1) < 5e-3, qPrintable(QStringLiteral("r = %1 mm: %2 T against %3 T").arg(x).arg(got).arg(expected)));
        }
        const double inside = at(r, QStringLiteral("mf.normB"), QPointF(0.5, 0));
        QVERIFY2(std::abs(inside / (Mu0 * I * 0.5e-3 / (2 * M_PI * 1e-6)) - 1) < 5e-3, qPrintable(QString::number(inside)));
        // Its current, all of it, out of the plane: the current density's integral.
        QVERIFY(std::abs(at(r, QStringLiteral("mf.Jz"), QPointF(0.3, 0.2)) - I / (M_PI * 1e-6)) < 1e-3 * I / (M_PI * 1e-6));
        const double L = Mu0 / (2 * M_PI) * (std::log(40.0) + 0.25);
        QVERIFY2(std::abs(global(r, QStringLiteral("mf.L_1")) / L - 1) < 1e-3, qPrintable(QString::number(global(r, QStringLiteral("mf.L_1")))));
        QVERIFY2(std::abs(global(r, QStringLiteral("mf.L11")) / L - 1) < 1e-3, qPrintable(QString::number(global(r, QStringLiteral("mf.L11")))));
        // The energy is ½ L I².
        QVERIFY(std::abs(global(r, QStringLiteral("mf.W")) / (0.5 * L * I * I) - 1) < 1e-3);
        const double R = 1 / (SigmaCu * M_PI * 1e-6);
        QVERIFY2(std::abs(global(r, QStringLiteral("mf.R_1")) / R - 1) < 1e-3, qPrintable(QString::number(global(r, QStringLiteral("mf.R_1")))));
        QVERIFY(std::abs(global(r, QStringLiteral("mf.P")) / (R * I * I) - 1) < 1e-3);
        QVERIFY(std::abs(global(r, QStringLiteral("mf.V_1")) / (R * I) - 1) < 1e-3);
    }

    void bhCurveByNewton()
    {
        // An iron ring round a wire: H = I / 2πr in it, whatever its µ; B from
        // the curve at that H.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "circle", "name": "air", "radius": "40"}, {"type": "circle", "name": "iron", "radius": "20"},
                         {"type": "circle", "name": "gap", "radius": "10"}, {"type": "circle", "name": "wire", "radius": "1"}],
            "materials": [{"material": "Air", "selection": {"only": ["air", "gap"]}},
                          {"material": "Copper", "selection": {"only": ["wire"]}},
                          {"label": "Steel", "mur": "1000", "BH": [[0.4, 50], [0.8, 100], [1.0, 150], [1.2, 300], [1.4, 600],
                                                                   [1.6, 1500], [1.8, 5000], [2.0, 20000]],
                           "selection": {"only": ["iron"]}}],
            "physics": [{"type": "magnetic", "tag": "mf", "features": [
              {"type": "ampere", "constitutive": "bh", "selection": {"only": ["iron"]}},
              {"type": "coil", "name": "1", "I0": "500[A]", "selection": {"only": ["wire"]}}]}],
            "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["iron"]}, "hmax": "0.6"}]}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QString why;
        auto curve = BHCurve::make({{0.4, 50}, {0.8, 100}, {1.0, 150}, {1.2, 300}, {1.4, 600}, {1.6, 1500}, {1.8, 5000}, {2.0, 20000}}, &why);
        QVERIFY(curve);
        const double I = 500;
        double worst = 0;
        for (double radius : {11.0, 15.0, 19.0}) {
            const double H = I / (2 * M_PI * radius * 1e-3);
            double lo = 0, hi = 3;
            for (int i = 0; i < 100; ++i) {
                const double mid = (lo + hi) / 2;
                (curve->H(mid) < H ? lo : hi) = mid;
            }
            const double got = at(r, QStringLiteral("mf.normB"), QPointF(radius * 0.8, radius * 0.6));
            worst = std::max(worst, std::abs(got / lo - 1));
            QVERIFY2(std::abs(got / lo - 1) < 3e-3, qPrintable(QStringLiteral("r = %1 mm: %2 T against %3 T").arg(radius).arg(got).arg(lo)));
            const double h = at(r, QStringLiteral("mf.normH"), QPointF(radius * 0.8, radius * 0.6));
            QVERIFY2(std::abs(h / H - 1) < 5e-3, qPrintable(QStringLiteral("H %1 against %2").arg(h).arg(H)));
        }
        // Newton's quadratic convergence: a few steps (its first solve's; the
        // coils' sweep solves again from there).
        int steps = -1;
        static const QRegularExpression said(QStringLiteral("Newton converged in (\\d+) steps"));
        for (const QString& line : r.solution->log)
            if (const QRegularExpressionMatch m = said.match(line); m.hasMatch() && steps < 0) steps = m.captured(1).toInt();
        qInfo("B-H curve: B within %.2f %% of the curve's at H = I/2πr; Newton in %d steps", 100 * worst, steps);
        QVERIFY2(steps > 0 && steps <= 15, qPrintable(r.solution->log.join(QLatin1Char('\n'))));
        // The energy: in the iron by the curve, ∫ H dB.
        QVERIFY(global(r, QStringLiteral("mf.W")) > 0);
    }

    void magnet()
    {
        // A round magnet magnetized evenly along x: inside, B = Br / 2 - and a
        // little less for the circle round it at A = 0, which keeps its flux
        // in: (1 - (a/R)²).
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "circle", "name": "air", "radius": "30"}, {"type": "circle", "name": "magnet", "radius": "1"}],
            "materials": [{"material": "Air", "selection": {"all": true}}],
            "physics": [{"type": "magnetic", "tag": "mf", "features": [
              {"type": "magnet", "source": "user", "Br": "1.2[T]", "direction": ["1", "0"], "selection": {"only": ["magnet"]}}]}],
            "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["magnet"]}, "hmax": "0.15"}]}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double expected = 0.6 * (1 - 1.0 / 900);
        for (QPointF p : {QPointF(0, 0), QPointF(0.4, 0.3), QPointF(-0.5, -0.2)}) {
            const double bx = at(r, QStringLiteral("mf.Bx"), p), by = at(r, QStringLiteral("mf.By"), p);
            QVERIFY2(std::abs(bx / expected - 1) < 2e-3, qPrintable(QStringLiteral("Bx %1 against %2").arg(bx).arg(expected)));
            QVERIFY(std::abs(by) < 2e-3 * expected);
            // H inside is -Br/2 (µ0 H = B - Br).
            QVERIFY(std::abs(Mu0 * at(r, QStringLiteral("mf.Hx"), p) / (expected - 1.2) - 1) < 2e-3);
        }
    }

    void solenoidAboutItsAxis()
    {
        // A thick solenoid's field at its middle: µ0 J (ℓ/2) ln((b + √(b² +
        // (ℓ/2)²)) / (a + √(a² + (ℓ/2)²))).
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm", "space": "axisymmetric",
            "geometry": [{"type": "rectangle", "name": "air", "position": ["0", "-400"], "size": ["400", "800"]},
                         {"type": "rectangle", "name": "winding", "position": ["5", "-50"], "size": ["1", "100"]}],
            "materials": [{"material": "Air", "selection": {"all": true}}],
            "physics": [{"type": "magnetic", "tag": "mf", "features": [
              {"type": "coil", "name": "s", "conductor": "multi", "N": "1000", "I0": "1[A]", "selection": {"only": ["winding"]}}]}],
            "mesh": {"size": "normal", "features": [{"type": "size", "level": "domain", "selection": {"only": ["winding"]}, "hmax": "0.5"}]}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double a = 5e-3, b = 6e-3, half = 50e-3, J = 1000 / (1e-3 * 100e-3);
        const double expected = Mu0 * J * half * std::log((b + std::hypot(b, half)) / (a + std::hypot(a, half)));
        const double got = at(r, QStringLiteral("mf.Bz"), QPointF(0.5, 0));
        qInfo("solenoid: Bz %.5f mT in its middle, %.5f mT exactly (%+.2f %%)", got * 1e3, expected * 1e3, 100 * (got / expected - 1));
        QVERIFY2(std::abs(got / expected - 1) < 5e-3, qPrintable(QStringLiteral("Bz %1 T against %2 T").arg(got).arg(expected)));
        // On the axis too, B along it.
        QVERIFY(std::abs(at(r, QStringLiteral("mf.Br"), QPointF(1, 20))) < 1e-2 * expected);
        // The flux function's contours are the field lines: ψ = πr²B near the axis.
        const double psi = at(r, QStringLiteral("mf.psi"), QPointF(2, 0));
        QVERIFY(std::abs(psi / (M_PI * 4e-6 * got) - 1) < 1e-2);
        // Its inductance from its energy and from its flux linkage agree.
        const double L = global(r, QStringLiteral("mf.L_s"));
        QVERIFY2(std::abs(2 * global(r, QStringLiteral("mf.W")) / L - 1) < 2e-3, qPrintable(QString::number(L)));
    }

    void skinEffect()
    {
        // A round wire three skin depths thick: Z = k J0(ka) / (2πaσ J1(ka))
        // inside, k = (1 - j)/δ, and jω µ0/2π ln(R/a) outside.
        const double a = 1e-3, delta = a / 3;
        const double omega = 2 / (Mu0 * SigmaCu * delta * delta);
        const double f = omega / (2 * M_PI);
        const QString step = QStringLiteral(R"json({"type": "frequency", "frequencies": "%1[Hz]"})json").arg(f, 0, 'g', 17);
        QByteArray json = wire(step, QStringLiteral("10"), QStringLiteral("0.05"));
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY(r.solution->frequencyDomain);
        QCOMPARE(r.solution->snapshotCount(), 1);
        QVERIFY(std::abs(r.solution->frequency() / f - 1) < 1e-12);
        const cd k = cd(1, -1) / delta;
        const cd zin = k * besselJ(0, k * a) / (2 * M_PI * a * SigmaCu * besselJ(1, k * a));
        const cd z = zin + cd(0, omega * Mu0 / (2 * M_PI) * std::log(10.0));
        const double R = global(r, QStringLiteral("mf.R_1")), L = global(r, QStringLiteral("mf.L_1"));
        qInfo("skin effect: R %.3e Ω/m (Bessel %.3e, %+.2f %%), L %.4e H/m (%+.3f %%)", R, z.real(), 100 * (R / z.real() - 1), L,
              100 * (L / (z.imag() / omega) - 1));
        QVERIFY2(std::abs(R / z.real() - 1) < 5e-3, qPrintable(QStringLiteral("R %1 against %2").arg(R).arg(z.real())));
        QVERIFY2(std::abs(L / (z.imag() / omega) - 1) < 2e-3, qPrintable(QStringLiteral("L %1 against %2").arg(L).arg(z.imag() / omega)));
        // More than at rest: the current crowds to the surface.
        QVERIFY(R > 1.5 / (SigmaCu * M_PI * a * a));
        QVERIFY(at(r, QStringLiteral("mf.normJ"), QPointF(0.99, 0)) > 3 * at(r, QStringLiteral("mf.normJ"), QPointF(0, 0)));
        // The losses: ½ R |I|².
        QVERIFY(std::abs(global(r, QStringLiteral("mf.P")) / (0.5 * R * 100) - 1) < 5e-3);
        // The impedance matrix's one entry is the coil's own.
        QVERIFY(std::abs(global(r, QStringLiteral("mf.R11")) / R - 1) < 1e-9);
    }

    void twoWireLine()
    {
        // Two wires, a current each way: L12 = L21, and the loop's L11 + L22 -
        // 2 L12 is µ0/π (ln((d/a) (R² - s²)/(R² + s²)) + 1/4) (the circle at A = 0
        // as images).
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "circle", "name": "air", "radius": "30"},
                         {"type": "circle", "name": "w1", "center": ["-3", "0"], "radius": "0.5"},
                         {"type": "circle", "name": "w2", "center": ["3", "0"], "radius": "0.5"}],
            "materials": [{"material": "Air", "selection": {"only": ["air"]}}, {"material": "Copper", "selection": {"only": ["w1", "w2"]}}],
            "physics": [{"type": "magnetic", "tag": "mf", "features": [
              {"type": "coil", "name": "1", "I0": "1[A]", "selection": {"only": ["w1"]}},
              {"type": "coil", "name": "2", "I0": "-1[A]", "selection": {"only": ["w2"]}}]}],
            "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["w1", "w2"]}, "hmax": "0.1"}]}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double l11 = global(r, QStringLiteral("mf.L11")), l12 = global(r, QStringLiteral("mf.L12"));
        const double l21 = global(r, QStringLiteral("mf.L21")), l22 = global(r, QStringLiteral("mf.L22"));
        QVERIFY2(std::abs(l12 / l21 - 1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(l12).arg(l21)));
        QVERIFY(std::abs(l11 / l22 - 1) < 1e-3);
        const double s = 3, R = 30;
        const double expected = Mu0 / M_PI * (std::log((2 * s / 0.5) * (R * R - s * s) / (R * R + s * s)) + 0.25);
        const double loop = l11 + l22 - 2 * l12;
        qInfo("two-wire line: loop %.4e H/m, images %.4e (%+.3f %%)", loop, expected, 100 * (loop / expected - 1));
        QVERIFY2(std::abs(loop / expected - 1) < 2e-3, qPrintable(QStringLiteral("%1 against %2").arg(loop).arg(expected)));
        // And as the coils' own: Φ1 / I1 with both at their currents.
        QVERIFY(std::abs(global(r, QStringLiteral("mf.L_1")) / (l11 - l12) - 1) < 1e-6);
    }

    void coaxLineParameters()
    {
        // A coax, its outer conductor perfect: R + jωL is the inner wire's
        // (Bessel) and jω µ0/2π ln(b/a); C = 2πε/ln(b/a); G = ω C tan δ.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "a", "expression": "0.5[mm]"}, {"name": "b", "expression": "1.75[mm]"}],
          "components": [{"unit": "mm",
            "geometry": [{"type": "circle", "name": "dielectric", "radius": "b"}, {"type": "circle", "name": "inner", "radius": "a"}],
            "materials": [{"label": "PTFE", "epsilonr": "2.1", "tand": "0.002", "mur": "1", "selection": {"only": ["dielectric"]}},
                          {"material": "Copper", "selection": {"only": ["inner"]}}],
            "physics": [{"type": "electrostatics", "tag": "es", "selection": {"only": ["dielectric"]}, "features": [
                           {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["inner"]}},
                           {"type": "ground", "selection": {"of": ["dielectric"]}}]},
                        {"type": "magnetic", "tag": "mf", "features": [
                           {"type": "coil", "name": "1", "I0": "1[A]", "selection": {"only": ["inner"]}}]}],
            "mesh": {"size": "fine", "features": [
              {"type": "size", "level": "domain", "selection": {"only": ["inner"]}, "hmax": "0.05"},
              {"type": "size", "level": "boundary", "selection": {"of": ["inner"]}, "hmax": "0.02"}]}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary", "physics": ["es"]},
                                                  {"type": "frequency", "physics": ["mf"], "frequencies": "100[kHz], 1[MHz]"}]}],
          "results": {"derived": [{"type": "lineparams", "tag": "lp1", "method": "rlgc", "physics": ["es"], "terminal": "1",
                                   "magnetic": ["mf"], "conductor": "1"}]}})json";
        const Model model = modelOf(json);
        Run r = run(model);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.solution->snapshotCount(), 2);
        const Node* lp = model.find(QStringLiteral("lp1"));
        QVERIFY(lp);
        const double a = 0.5e-3, b = 1.75e-3, eps = 2.1 * 8.8541878188e-12;
        for (int k = 0; k < 2; ++k) {
            r.solution->select(k);
            const double f = r.solution->frequency();
            const double omega = 2 * M_PI * f;
            const LineRLGC l = lineRLGC(*r.solution, *lp);
            QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
            const double delta = std::sqrt(2 / (omega * Mu0 * SigmaCu));
            const cd kk = cd(1, -1) / delta;
            const cd z = kk * besselJ(0, kk * a) / (2 * M_PI * a * SigmaCu * besselJ(1, kk * a)) + cd(0, omega * Mu0 / (2 * M_PI) * std::log(b / a));
            qInfo("coax at %g Hz: R %+.2f %%, L %+.3f %%, C %+.3f %%, G %+.3f %% against the closed forms", f, 100 * (l.R / z.real() - 1),
                  100 * (l.L / (z.imag() / omega) - 1), 100 * (l.C / (2 * M_PI * eps / std::log(b / a)) - 1),
                  100 * (l.G / (omega * 2 * M_PI * eps / std::log(b / a) * 0.002) - 1));
            QVERIFY2(std::abs(l.R / z.real() - 1) < 5e-3, qPrintable(QStringLiteral("f %1: R %2 against %3").arg(f).arg(l.R).arg(z.real())));
            QVERIFY2(std::abs(l.L / (z.imag() / omega) - 1) < 2e-3, qPrintable(QStringLiteral("L %1 against %2").arg(l.L).arg(z.imag() / omega)));
            const double C = 2 * M_PI * eps / std::log(b / a);
            QVERIFY2(std::abs(l.C / C - 1) < 1e-3, qPrintable(QStringLiteral("C %1 against %2").arg(l.C).arg(C)));
            QVERIFY2(std::abs(l.G / (omega * C * 0.002) - 1) < 2e-3, qPrintable(QStringLiteral("G %1 against %2").arg(l.G).arg(omega * C * 0.002)));
            const cd zc = std::sqrt(cd(l.R, omega * l.L) / cd(l.G, omega * l.C));
            QVERIFY(std::abs(l.Zc - zc) < 1e-9 * std::abs(zc));
            // α ≈ R/2Z0 + G Z0/2 (dB/m: × 8.686).
            const double alpha = (l.R / (2 * zc.real()) + l.G * zc.real() / 2) * 20 / std::log(10.0);
            QVERIFY2(std::abs(l.alphaDb / alpha - 1) < 2e-2, qPrintable(QStringLiteral("α %1 against %2").arg(l.alphaDb).arg(alpha)));
            // The Derived Values node says the same.
            const DerivedResult d = evaluateDerived(*r.solution, *lp);
            QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
            QCOMPARE(d.values.at(0).name, QStringLiteral("R"));
            QVERIFY(std::abs(d.values.at(0).value - l.R) < 1e-12 * l.R);
            QVERIFY2(d.values.at(0).text.endsWith(QStringLiteral("Ω/m")), qPrintable(d.values.at(0).text));
            // n_eff = β/k0, εeff its square; the quasi-static εeff C/C0 the
            // PTFE's 2.1 (its field all in it), less than (β/k0)² (the
            // conductor's loss slows the wave).
            QMap<QString, double> v;
            for (const DerivedValue& x : d.values) v.insert(x.name, x.value);
            QVERIFY(std::abs(v.value("n_eff") - l.gamma.imag() * 299792458.0 / omega) < 1e-12);
            QVERIFY(std::abs(v.value("epsilon_eff") / (v.value("n_eff") * v.value("n_eff")) - 1) < 1e-12);
            QVERIFY2(std::abs(v.value("epsilon_eff_qs") / 2.1 - 1) < 2e-3, qPrintable(QString::number(v.value("epsilon_eff_qs"))));
            QVERIFY(v.value("epsilon_eff") > v.value("epsilon_eff_qs"));
        }
        // Lower at 100 kHz: less skin effect.
        r.solution->select(0);
        const double r0 = lineRLGC(*r.solution, *lp).R;
        r.solution->select(1);
        QVERIFY(lineRLGC(*r.solution, *lp).R > 1.2 * r0);

        // As a Touchstone file: a line of 1 m between ports of its own Z0 - S11
        // none, S21 e^-γℓ.
        SolutionSet set;
        set.solutions = r.study.solutions;
        const LineSweep sweep = lineSweep(set, *lp);
        QVERIFY2(sweep.error.isEmpty(), qPrintable(sweep.error));
        QCOMPARE(int(sweep.points.size()), 2);
        QVERIFY(sweep.points[0].frequency < sweep.points[1].frequency);
        const LineRLGC& top = sweep.points[1];
        const auto sm = lineSParameters(top, 1.0, top.Zc.real());
        QVERIFY(std::abs(sm[0]) < 0.05);
        QVERIFY(std::abs(sm[1] - std::exp(-top.gamma)) < 0.05);
        QVERIFY(std::abs(sm[1] - sm[2]) < 1e-12);
        const QString ts = touchstone(sweep, 1.0, 50, {QStringLiteral("a test")});
        QVERIFY(ts.startsWith(QStringLiteral("! a test\n")));
        QVERIFY(ts.contains(QStringLiteral("# Hz S RI R 50\n")));
        const QStringList rows = ts.split(QLatin1Char('\n'), Qt::SkipEmptyParts).filter(QRegularExpression(QStringLiteral("^[0-9]")));
        QCOMPARE(rows.size(), 2);
        const QStringList cells = rows[1].split(QLatin1Char(' '));
        QCOMPARE(cells.size(), 9);
        QVERIFY(std::abs(cells[0].toDouble() / 1e6 - 1) < 1e-9);
        // |S|² of a line that loses little: |S11|² + |S21|² just under 1.
        const double s11 = std::hypot(cells[1].toDouble(), cells[2].toDouble()), s21 = std::hypot(cells[3].toDouble(), cells[4].toDouble());
        QVERIFY(s11 * s11 + s21 * s21 < 1 && s11 * s11 + s21 * s21 > 0.95);

        // As SPICE: sections, and an LTRA line.
        const QString ladder = spiceLine(top, 0.1, QStringLiteral("coax"), 4, false);
        QVERIFY(ladder.contains(QStringLiteral(".subckt coax in out ref\n")));
        QCOMPARE(ladder.count(QRegularExpression(QStringLiteral("^L\\d "), QRegularExpression::MultilineOption)), 4);
        QCOMPARE(ladder.count(QRegularExpression(QStringLiteral("^C\\d "), QRegularExpression::MultilineOption)), 4);
        QVERIFY(ladder.contains(QStringLiteral(".ends coax")));
        const QString ltra = spiceLine(top, 0.1, QStringLiteral("coax"), 4, true);
        QVERIFY(ltra.contains(QStringLiteral("O1 in ref out ref coax_ltra")));
        QVERIFY(ltra.contains(QStringLiteral("LEN=0.1")));

        // As a Qucs subcircuit: an RLCG part between two ports.
        const QString sch = qucsLineSchematic(top, 0.1, QStringLiteral("26.1.8"), QStringLiteral("coax"));
        QVERIFY(sch.startsWith(QStringLiteral("<Qucs Schematic 26.1.8>\n")));
        QVERIFY(sch.contains(QStringLiteral("<RLCG Line1 1 200 100")));
        QCOMPARE(sch.count(QStringLiteral("<Port P")), 2);
        QCOMPARE(sch.count(QStringLiteral(".PortSym")), 2);
        QVERIFY(sch.contains(QStringLiteral("\"0.1\" 1 \"26.85\" 0>")));
        const QString clip = qucsLineSchematic(top, 0.1, QStringLiteral("26.1.8"), QStringLiteral("coax"), true);
        QVERIFY(clip.contains(QStringLiteral("<Components>\n  <RLCG Line1 1 0 100")));
        QVERIFY(!clip.contains(QStringLiteral("<Port")));

        // An export node runs them.
        Model withExports = model;
        Node ts1 = withExports.make(QStringLiteral("touchstone"));
        ts1.props.insert(QStringLiteral("line"), QStringLiteral("lp1"));
        ts1.props.insert(QStringLiteral("length"), QStringLiteral("25[mm]"));
        QVERIFY(withExports.add(QStringLiteral("exports"), ts1));
        const ExportText e = exportText(withExports, set, *withExports.find(ts1.tag), QStringLiteral("26.1.8"));
        QVERIFY2(e.error.isEmpty(), qPrintable(e.error));
        QCOMPARE(e.suffix, QStringLiteral("s2p"));
        QVERIFY(e.summary.contains(QStringLiteral("2 frequencies")));
        // A quasi-TEM line has no R, L, G, C.
        Model quasi = withExports;
        quasi.find(QStringLiteral("lp1"))->props.insert(QStringLiteral("method"), QStringLiteral("quasitem"));
        QVERIFY(exportText(quasi, set, *quasi.find(ts1.tag), QStringLiteral("26.1.8")).error.contains(QStringLiteral("quasi-TEM")));
        // The model written and read keeps its exports.
        std::optional<Model> again = Model::fromJson(withExports.toJson());
        QVERIFY(again);
        QVERIFY(again->find(ts1.tag));
        QCOMPARE(again->find(ts1.tag)->text(QStringLiteral("length")), QStringLiteral("25[mm]"));
    }

    void aCoupledPair()
    {
        // The reviewer's ground-signal-signal-ground pair, smaller: a Line
        // Parameters node of both signals - its matrices those of the
        // terminals and of the coils reduced against the grounds' coil by
        // hand, its even and odd modes the pair's, the odd one its half-model's
        // (the ground grounded, not fed -1 A); its exports.
        const QString hmax = slowBuild() ? QStringLiteral("4") : QStringLiteral("2");
        const QString frequencies = slowBuild() ? QStringLiteral("20[GHz]") : QStringLiteral("1[GHz], 20[GHz]");
        QElapsedTimer clock;
        clock.start();
        const Model model = modelOf(gssg(hmax, frequencies));
        Run r = run(model);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Node* lp = model.find(QStringLiteral("lp"));
        const Node* lq = model.find(QStringLiteral("lq"));
        QVERIFY(lp && lq);
        QCOMPARE(lineTerminals(*lp), (QStringList{QStringLiteral("1"), QStringLiteral("2")}));
        const double d = r.solution->thickness();
        const int snaps = r.solution->snapshotCount();
        std::vector<CoupledLines> each;
        for (int k = 0; k < snaps; ++k) {
            r.solution->select(k);
            const double omega = 2 * M_PI * r.solution->frequency();
            const CoupledLines cl = coupledLines(*r.solution, *lp);
            QVERIFY2(cl.error.isEmpty(), qPrintable(cl.error));
            QVERIFY(cl.rlgc && cl.symmetricPair);
            QCOMPARE(int(cl.modes.size()), 2);
            QCOMPARE(cl.modes[0].name, QStringLiteral("even"));
            QCOMPARE(cl.modes[1].name, QStringLiteral("odd"));
            // C: the terminal sweep's, per metre.
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) {
                    const double c = global(r, QStringLiteral("es.C_%1_%2").arg(i + 1).arg(j + 1)) / d;
                    QVERIFY2(std::abs(cl.C[std::size_t(i)][std::size_t(j)] / c - 1) < 1e-6,
                             qPrintable(QStringLiteral("C %1 %2: %3 against %4").arg(i).arg(j).arg(cl.C[std::size_t(i)][std::size_t(j)]).arg(c)));
                }
            QVERIFY(cl.C0[0][0] > 0 && cl.C0[0][1] < 0 && cl.C[0][0] > cl.C0[0][0]);
            // R and L: the coils' matrix reduced by hand - and the physics'
            // own against its reference coil G.
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) {
                    const QString a = QString::number(i + 1), b = QString::number(j + 1), g = QStringLiteral("G");
                    auto z = [&](const QString& x, const QString& y) {
                        return cd(global(r, QStringLiteral("mf.R_%1_%2").arg(x, y)), omega * global(r, QStringLiteral("mf.L_%1_%2").arg(x, y)));
                    };
                    const cd reduced = z(a, b) - z(a, g) - z(g, b) + z(g, g);
                    const cd own(global(r, QStringLiteral("mf.Rr_%1_%2").arg(a, b)), omega * global(r, QStringLiteral("mf.Lr_%1_%2").arg(a, b)));
                    QVERIFY(std::abs(own - reduced) < 1e-12 * std::abs(reduced));
                    const cd line(cl.R[std::size_t(i)][std::size_t(j)], omega * cl.L[std::size_t(i)][std::size_t(j)]);
                    QVERIFY2(std::abs(line - reduced / d) < 1e-6 * std::abs(reduced / d), qPrintable(QStringLiteral("Z %1 %2").arg(a, b)));
                }
            // The modes: the pair's, of Z ± Z12 and Y ± Y12 (the mesh's
            // asymmetry, 1e-3 or less, averaged out).
            auto mean = [](const CoupledLines::Matrix& x, double sign) { return 0.5 * (x[0][0] + x[1][1]) + sign * x[0][1]; };
            QVERIFY(std::abs(cl.C[0][0] / cl.C[1][1] - 1) < 1e-3);
            for (int m = 0; m < 2; ++m) {
                const double sign = m == 0 ? 1 : -1;
                const cd z = cd(mean(cl.R, sign), omega * mean(cl.L, sign));
                const cd y = cd(mean(cl.G, sign), omega * mean(cl.C, sign));
                const LineMode& lm = cl.modes[std::size_t(m)];
                QVERIFY2(std::abs(lm.gamma - std::sqrt(z * y)) < 1e-9 * std::abs(lm.gamma), qPrintable(lm.name));
                QVERIFY(std::abs(lm.Z - std::sqrt(z / y)) < 1e-9 * std::abs(lm.Z));
                QVERIFY(std::abs(lm.nEff - lm.gamma.imag() * 299792458.0 / omega) < 1e-12);
                QVERIFY(std::abs(lm.epsEffQs - mean(cl.C, sign) / mean(cl.C0, sign)) < 1e-9 * lm.epsEffQs);
                // (β/k0)² is more than C/C0: the conductors' loss slows it.
                QVERIFY(lm.nEff * lm.nEff > lm.epsEffQs);
            }
            // The Derived Values node says so: the pair's, R in Ω/m.
            const DerivedResult dr = evaluateDerived(*r.solution, *lp);
            QVERIFY2(dr.error.isEmpty(), qPrintable(dr.error));
            QMap<QString, DerivedValue> v;
            for (const DerivedValue& x : dr.values) v.insert(x.name, x);
            for (const char* name : {"C_1_2", "C0_1_1", "L_1_2", "R_1_1", "G_1_2", "Z0_even", "Z0_odd_im", "alpha_odd", "beta_even", "n_eff_odd",
                                     "epsilon_eff_odd", "epsilon_eff_qs_even", "Zdiff", "Zcm_im", "v_odd"})
                QVERIFY2(v.contains(QString::fromLatin1(name)), name);
            QVERIFY2(v.value("R_1_1").text.endsWith(QStringLiteral("Ω/m")), qPrintable(v.value("R_1_1").text));
            QVERIFY(std::abs(v.value("Zdiff").value - 2 * cl.modes[1].Z.real()) < 1e-9 * v.value("Zdiff").value);
            QVERIFY(std::abs(v.value("Zcm").value - cl.modes[0].Z.real() / 2) < 1e-9 * v.value("Zcm").value);
            QVERIFY2(v.value("alpha_odd").text.endsWith(QStringLiteral("dB/m")), qPrintable(v.value("alpha_odd").text));
            QVERIFY(!v.contains(QStringLiteral("V_1_1")));   // (a pair's vectors are its even and odd)
            each.push_back(cl);
        }
        // Quasi-TEM: L = C0⁻¹/c², the modes' εeff the quasi-static ones.
        {
            const CoupledLines q = coupledLines(*r.solution, *lq);
            QVERIFY2(q.error.isEmpty(), qPrintable(q.error));
            const double c2 = 299792458.0 * 299792458.0;
            const double det = q.C0[0][0] * q.C0[1][1] - q.C0[0][1] * q.C0[1][0];
            QVERIFY(std::abs(q.L[0][0] - q.C0[1][1] / det / c2) < 1e-9 * q.L[0][0]);
            QVERIFY(std::abs(q.L[0][1] + q.C0[0][1] / det / c2) < 1e-9 * q.L[0][0]);
            // (To the square of the mesh's asymmetry.)
            for (const LineMode& m : q.modes) QVERIFY2(std::abs(m.nEff * m.nEff / m.epsEffQs - 1) < 1e-6, qPrintable(m.name));
            QVERIFY(!evaluateDerived(*r.solution, *lq).values.isEmpty());
        }
        // The odd mode's half-model: its ground grounded - not fed -1 A - and
        // no return; the same line.
        const Model half = modelOf(gssg(hmax, frequencies, true));
        Run h = run(half);
        QVERIFY2(h.error.isEmpty(), qPrintable(h.error));
        for (int k = 0; k < snaps; ++k) {
            h.solution->select(k);
            const LineRLGC l = lineRLGC(*h.solution, *half.find(QStringLiteral("lp")));
            QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
            const LineMode& odd = each[std::size_t(k)].modes[1];
            qInfo("odd mode at %g Hz: Z %.4f%+.4fj, half-model's %.4f%+.4fj; n_eff %.5f, %.5f; α %.3f, %.3f dB/m", l.frequency, odd.Z.real(), odd.Z.imag(),
                  l.Zc.real(), l.Zc.imag(), odd.nEff, l.gamma.imag() * 299792458.0 / (2 * M_PI * l.frequency), odd.alphaDb, l.alphaDb);
            QVERIFY2(std::abs(l.Zc / odd.Z - 1.0) < 2e-3, qPrintable(QStringLiteral("Z %1 against %2").arg(std::abs(l.Zc)).arg(std::abs(odd.Z))));
            QVERIFY(std::abs(l.gamma / odd.gamma - 1.0) < 2e-3);
            // Its ground's current, found: part of the return - the rest in
            // the mirror signal.
            const double ig = global(h, QStringLiteral("mf.I_G"));
            QVERIFY2(ig < -0.05 && ig > -0.95, qPrintable(QString::number(ig)));
        }
        // As a 4-port: reciprocal, lossy; its mixed-mode Sdd and Scc the odd
        // and even modes' lines between ports of 50 Ω, Sdc none.
        const CoupledLines& top = each.back();
        const auto se = coupledSParameters(top, 0.01, 50);
        QCOMPARE(int(se.size()), 4);
        for (int i = 0; i < 4; ++i) {
            double power = 0;
            for (int j = 0; j < 4; ++j) {
                QVERIFY(std::abs(se[std::size_t(i)][std::size_t(j)] - se[std::size_t(j)][std::size_t(i)]) < 1e-9);
                power += std::norm(se[std::size_t(j)][std::size_t(i)]);
            }
            QVERIFY2(power < 1 && power > 0.5, qPrintable(QString::number(power)));
        }
        const auto mm = mixedModeSParameters(se);
        for (int m = 0; m < 2; ++m) {
            LineRLGC mode;
            mode.gamma = top.modes[std::size_t(m)].gamma;
            mode.Zc = top.modes[std::size_t(m)].Z;
            const auto two = lineSParameters(mode, 0.01, 50);
            const int o = m == 0 ? 2 : 0;   // (Scc at ports c1, c2; Sdd at d1, d2)
            QVERIFY2(std::abs(mm[std::size_t(o)][std::size_t(o)] - two[0]) < 1e-3, qPrintable(top.modes[std::size_t(m)].name));
            QVERIFY(std::abs(mm[std::size_t(o + 1)][std::size_t(o)] - two[1]) < 1e-3);
        }
        QVERIFY(std::abs(mm[0][2]) < 1e-3 && std::abs(mm[2][1]) < 1e-3);
        // One line through the coupled path (the half-model's): as lineRLGC
        // and lineSParameters have it - G by the terminals' fields as by the
        // solution's.
        {
            const CoupledLines c1 = coupledLines(*h.solution, *half.find(QStringLiteral("lp")));
            QVERIFY2(c1.error.isEmpty(), qPrintable(c1.error));
            QCOMPARE(int(c1.modes.size()), 1);
            const LineRLGC l1 = lineRLGC(*h.solution, *half.find(QStringLiteral("lp")));
            QVERIFY2(l1.error.isEmpty(), qPrintable(l1.error));
            QVERIFY2(std::abs(c1.G[0][0] / l1.G - 1) < 1e-6, qPrintable(QStringLiteral("G %1 against %2").arg(c1.G[0][0]).arg(l1.G)));
            QVERIFY(std::abs(c1.C[0][0] / l1.C - 1) < 1e-9);
            // Its quasi-static εeff as the coupled path has it: C/C0.
            const DerivedResult hd = evaluateDerived(*h.solution, *half.find(QStringLiteral("lp")));
            QVERIFY2(hd.error.isEmpty(), qPrintable(hd.error));
            double qs = NAN;
            for (const DerivedValue& x : hd.values)
                if (x.name == QLatin1String("epsilon_eff_qs")) qs = x.value;
            QVERIFY2(std::abs(qs / c1.modes[0].epsEffQs - 1) < 1e-9, qPrintable(QStringLiteral("%1 against %2").arg(qs).arg(c1.modes[0].epsEffQs)));
            QVERIFY(qs > 1.5 && qs < 4);
            QVERIFY(std::abs(c1.modes[0].gamma / l1.gamma - 1.0) < 1e-6);
            const auto s1 = coupledSParameters(c1, 0.01, 50);
            const auto s2 = lineSParameters(l1, 0.01, 50);
            QVERIFY(std::abs(s1[0][0] - s2[0]) < 1e-6 && std::abs(s1[1][0] - s2[1]) < 1e-6);
        }
        // Its files: Touchstone (a 4-port's rows), mixed-mode, SPICE sections,
        // a Qucs coupled line part.
        CoupledSweep sweep = coupledSweep(SolutionSet{r.study.solutions, {}}, *lp);
        QVERIFY2(sweep.error.isEmpty(), qPrintable(sweep.error));
        QCOMPARE(int(sweep.points.size()), snaps);
        const QString ts = touchstoneCoupled(sweep, 0.01, 50, false, {QStringLiteral("a pair")});
        QVERIFY(ts.contains(QStringLiteral("# Hz S RI R 50\n")));
        QVERIFY(ts.contains(QStringLiteral("Ports: 1 line 1's near end, 2 line 2's near end, 3 line 1's far end, 4 line 2's far end")));
        const QStringList rows = ts.split(QLatin1Char('\n'), Qt::SkipEmptyParts).filter(QRegularExpression(QStringLiteral("^[ 0-9-]")));
        QCOMPARE(rows.size(), 4 * snaps);
        QCOMPARE(rows[0].split(QLatin1Char(' ')).size(), 9);
        QCOMPARE(rows[1].trimmed().split(QLatin1Char(' ')).size(), 8);
        QVERIFY(touchstoneCoupled(sweep, 0.01, 50, true).contains(QStringLiteral("ports d1 d2 c1 c2")));
        // (Its option line's R 50 the single-ended reference of the modes'
        // 100 and 25 Ω, said: the check of 31ba73b, 1.)
        QVERIFY(touchstoneCoupled(sweep, 0.01, 50, true).contains(QStringLiteral("The option line's R 50 is the single-ended reference")));
        const QString cir = spiceCoupled(top, 0.01, QStringLiteral("pair"), 3);
        QVERIFY(cir.contains(QStringLiteral(".subckt pair in_1 in_2 out_1 out_2 ref\n")));
        QCOMPARE(cir.count(QRegularExpression(QStringLiteral("^K\\d_1_2 L\\d_1 L\\d_2 0\\.\\d+"), QRegularExpression::MultilineOption)), 3);
        QCOMPARE(cir.count(QRegularExpression(QStringLiteral("^H\\d_1_2 \\S+ \\S+ V\\d_2 "), QRegularExpression::MultilineOption)), 3);
        QCOMPARE(cir.count(QRegularExpression(QStringLiteral("^C\\d_1_2 n\\d_1 n\\d_2 |^C3_1_2 out_1 out_2 "), QRegularExpression::MultilineOption)), 3);
        QString why;
        const QString sch = qucsCoupledSchematic(top, 0.01, QStringLiteral("27.0.1"), QStringLiteral("pair"), &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QVERIFY(sch.contains(QStringLiteral("<CTLIN Line1 1 200 100")) && sch.contains(QStringLiteral(" dB\" 1")));
        QCOMPARE(sch.count(QStringLiteral("<Port P")), 4);
        QCOMPARE(sch.count(QStringLiteral(".PortSym")), 4);
        // Export nodes: .s4p; an LTRA line is one line.
        Model withExports = model;
        Node t = withExports.make(QStringLiteral("touchstone"));
        t.props.insert(QStringLiteral("line"), QStringLiteral("lp"));
        t.props.insert(QStringLiteral("mixed"), true);
        QVERIFY(withExports.add(QStringLiteral("exports"), t));
        Node sp = withExports.make(QStringLiteral("spice"));
        sp.props.insert(QStringLiteral("line"), QStringLiteral("lp"));
        sp.props.insert(QStringLiteral("model"), QStringLiteral("ltra"));
        QVERIFY(withExports.add(QStringLiteral("exports"), sp));
        const SolutionSet set{r.study.solutions, {}};
        const ExportText e = exportText(withExports, set, *withExports.find(t.tag), QStringLiteral("27.0.1"));
        QVERIFY2(e.error.isEmpty(), qPrintable(e.error));
        QCOMPARE(e.suffix, QStringLiteral("s4p"));
        QVERIFY(e.summary.contains(QStringLiteral("mixed-mode")));
        QVERIFY(exportText(withExports, set, *withExports.find(sp.tag), QStringLiteral("27.0.1")).error.contains(QStringLiteral("LTRA")));
        qInfo("a coupled pair: %lld ms", clock.elapsed());
    }

    void anAsymmetricPair()
    {
        // Signals of 20 and 40 µm: no even and odd modes - its modes of
        // (R + jωL)(G + jωC), Z i = γ v and Y v = γ i each, Zc Y Zc = Z.
        const Model model = modelOf(gssg(slowBuild() ? QStringLiteral("5") : QStringLiteral("2.5"), QStringLiteral("10[GHz]"), false, QStringLiteral("40")));
        Run r = run(model);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const CoupledLines cl = coupledLines(*r.solution, *model.find(QStringLiteral("lp")));
        QVERIFY2(cl.error.isEmpty(), qPrintable(cl.error));
        QVERIFY(!cl.symmetricPair);
        QCOMPARE(int(cl.modes.size()), 2);
        QCOMPARE(cl.modes[0].name, QStringLiteral("1"));
        QVERIFY(cl.modes[0].nEff >= cl.modes[1].nEff);
        const double omega = 2 * M_PI * cl.frequency;
        cd Z[2][2], Y[2][2];
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) {
                Z[i][j] = cd(cl.R[std::size_t(i)][std::size_t(j)], omega * cl.L[std::size_t(i)][std::size_t(j)]);
                Y[i][j] = cd(cl.G[std::size_t(i)][std::size_t(j)], omega * cl.C[std::size_t(i)][std::size_t(j)]);
            }
        for (const LineMode& m : cl.modes) {
            double big = 0;
            for (const cd& x : m.v) big = std::max(big, std::abs(x));
            QVERIFY(std::abs(big - 1) < 1e-12);
            for (int i = 0; i < 2; ++i) {
                const cd zi = Z[i][0] * m.i[0] + Z[i][1] * m.i[1], yv = Y[i][0] * m.v[0] + Y[i][1] * m.v[1];
                QVERIFY2(std::abs(zi - m.gamma * m.v[std::size_t(i)]) < 1e-6 * std::abs(m.gamma), qPrintable(m.name));
                QVERIFY(std::abs(yv - m.gamma * m.i[std::size_t(i)]) < 1e-9 * std::abs(m.gamma * m.i[std::size_t(i)]) + 1e-12);
            }
            QVERIFY(m.alphaDb > 0 && m.epsEffQs > 1);
        }
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) {
                cd zyz = 0;
                for (int a = 0; a < 2; ++a)
                    for (int b = 0; b < 2; ++b) zyz += cl.Zc[std::size_t(i)][std::size_t(a)] * Y[a][b] * cl.Zc[std::size_t(b)][std::size_t(j)];
                QVERIFY2(std::abs(zyz - Z[i][j]) < 1e-6 * std::abs(Z[0][0]), qPrintable(QStringLiteral("%1 %2").arg(i).arg(j)));
            }
        const DerivedResult dr = evaluateDerived(*r.solution, *model.find(QStringLiteral("lp")));
        QVERIFY2(dr.error.isEmpty(), qPrintable(dr.error));
        QStringList names;
        for (const DerivedValue& v : dr.values) names << v.name;
        for (const char* name : {"Z0_1", "n_eff_2", "V_1_1", "V_2_2_im", "I_1_2", "Zc_1_2", "Zc_2_1_im"}) QVERIFY2(names.contains(QString::fromLatin1(name)), name);
        QVERIFY(!names.contains(QStringLiteral("Zdiff")));
        // No Qucs part: a pair's alone.
        QString why;
        QVERIFY(qucsCoupledSchematic(cl, 0.01, QStringLiteral("27.0.1"), QStringLiteral("x"), &why).isEmpty());
        QVERIFY(why.contains(QStringLiteral("symmetric pair")));
        // Coils fewer than terminals: said.
        Model fewer = model;
        fewer.find(QStringLiteral("lp"))->props.insert(QStringLiteral("conductor"), QStringLiteral("1"));
        QVERIFY(coupledLines(*r.solution, *fewer.find(QStringLiteral("lp"))).error.contains(QStringLiteral("want as many coils")));
    }

    void coilRoles()
    {
        // Two wires in a perfect conductor's circle at 10 kHz: the second
        // grounded (as a conductor with no coil), floating (no net current),
        // fed a voltage; the first's impedance the same when it is fed a
        // voltage of its current's; the matrix of the coils fed a current;
        // reduced against a reference coil.
        auto twoWires = [](const QString& second, const QString& extra = QString(), const QString& step = QStringLiteral(
                               "{\"type\": \"frequency\", \"frequencies\": \"10[kHz]\"}")) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "mm", "thickness": "0.5[m]",
                "geometry": [{"type": "circle", "name": "air", "radius": "20"},
                             {"type": "circle", "name": "w1", "center": ["-2", "0"], "radius": "0.5"},
                             {"type": "circle", "name": "w2", "center": ["2", "0"], "radius": "0.5"}],
                "materials": [{"material": "Air", "selection": {"only": ["air"]}}, {"material": "Copper", "selection": {"only": ["w1", "w2"]}}],
                "physics": [{"type": "magnetic", "tag": "mf"%2, "features": [
                  {"type": "coil", "name": "1", "I0": "1[A]", "selection": {"only": ["w1"]}}%1]}],
                "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["w1", "w2"]}, "hmax": "0.1"}]}}],
              "studies": [{"type": "study", "steps": [%3]}]})json")
                .arg(second, extra, step)
                .toUtf8();
        };
        auto complexOf = [](const Run& r, const QString& name) { return cd(global(r, name), global(r, name + QStringLiteral("_im"))); };
        // Grounded: as no coil at all; its current found.
        const Run grounded = run(modelOf(twoWires(QStringLiteral(R"(, {"type": "coil", "name": "2", "role": "grounded", "selection": {"only": ["w2"]}})"))));
        QVERIFY2(grounded.error.isEmpty(), qPrintable(grounded.error));
        const Run none = run(modelOf(twoWires(QString())));
        QVERIFY2(none.error.isEmpty(), qPrintable(none.error));
        const cd v1 = complexOf(grounded, QStringLiteral("mf.V_1"));
        QVERIFY2(std::abs(v1 - complexOf(none, QStringLiteral("mf.V_1"))) < 1e-9 * std::abs(v1), qPrintable(QString::number(std::abs(v1))));
        const cd i2 = complexOf(grounded, QStringLiteral("mf.I_2"));
        QVERIFY2(std::abs(i2) > 1e-4 && std::abs(i2) < 1, qPrintable(QString::number(std::abs(i2))));
        // Its matrix: coil 1's alone, its V/I.
        QVERIFY(grounded.solution->global(QStringLiteral("mf.L_1_1")).has_value());
        QVERIFY(!grounded.solution->global(QStringLiteral("mf.L_1_2")).has_value());
        QVERIFY(std::abs(global(grounded, QStringLiteral("mf.L_1_1")) / global(grounded, QStringLiteral("mf.L_1")) - 1) < 1e-9);
        // Fed a voltage of coil 1's: its current 1 A again, the same Z.
        const QString v = QString::number(std::abs(v1), 'g', 17);
        const Run byVoltage = run(modelOf(twoWires(QStringLiteral(R"(, {"type": "coil", "name": "2", "role": "grounded", "selection": {"only": ["w2"]}})"))
                                              .replace("\"I0\": \"1[A]\"", QStringLiteral("\"role\": \"voltage\", \"V0\": \"%1[V]\"").arg(v).toUtf8())));
        QVERIFY2(byVoltage.error.isEmpty(), qPrintable(byVoltage.error));
        const cd i1 = complexOf(byVoltage, QStringLiteral("mf.I_1"));
        QVERIFY2(std::abs(std::abs(i1) - 1) < 1e-9, qPrintable(QString::number(std::abs(i1))));
        QVERIFY(std::abs(global(byVoltage, QStringLiteral("mf.L_1")) / global(grounded, QStringLiteral("mf.L_1")) - 1) < 1e-9);
        QVERIFY(std::abs(global(byVoltage, QStringLiteral("mf.R_1")) / global(grounded, QStringLiteral("mf.R_1")) - 1) < 1e-9);
        QVERIFY(!byVoltage.solution->global(QStringLiteral("mf.L_1_1")).has_value());   // (no coil fed a current)
        // Floating: no net current, its voltage found.
        const Run floating = run(modelOf(twoWires(QStringLiteral(R"(, {"type": "coil", "name": "2", "role": "floating", "selection": {"only": ["w2"]}})"))));
        QVERIFY2(floating.error.isEmpty(), qPrintable(floating.error));
        QVERIFY(std::abs(complexOf(floating, QStringLiteral("mf.I_2"))) < 1e-9);
        QVERIFY(std::abs(complexOf(floating, QStringLiteral("mf.V_2"))) > 0);
        // Both fed, against coil 2: Lr = L11 - L12 - L21 + L22, the loop's.
        const Run both = run(modelOf(twoWires(QStringLiteral(R"(, {"type": "coil", "name": "2", "I0": "-1[A]", "selection": {"only": ["w2"]}})"),
                                              QStringLiteral(R"(, "reference": "2")"))));
        QVERIFY2(both.error.isEmpty(), qPrintable(both.error));
        for (const char* letter : {"L", "R"}) {
            const QString l = QString::fromLatin1(letter);
            const double loop = global(both, QStringLiteral("mf.%1_1_1").arg(l)) - global(both, QStringLiteral("mf.%1_1_2").arg(l))
                                - global(both, QStringLiteral("mf.%1_2_1").arg(l)) + global(both, QStringLiteral("mf.%1_2_2").arg(l));
            QVERIFY2(std::abs(global(both, QStringLiteral("mf.%1r_1_1").arg(l)) / loop - 1) < 1e-12, letter);
            // (Fed +1 and -1 A: V1 - V2 is the loop's.)
            if (l == QLatin1String("L"))
                QVERIFY(std::abs((complexOf(both, QStringLiteral("mf.V_1")) - complexOf(both, QStringLiteral("mf.V_2"))).imag() / (2 * M_PI * 1e4) / loop - 1) < 1e-9);
        }
        QVERIFY(!both.solution->global(QStringLiteral("mf.Lr_2_2")).has_value());
        // A reference that is no coil fed a current: refused.
        const Run bad = run(modelOf(twoWires(QStringLiteral(R"(, {"type": "coil", "name": "2", "role": "grounded", "selection": {"only": ["w2"]}})"),
                                             QStringLiteral(R"(, "reference": "2")"))));
        QVERIFY2(bad.error.contains(QStringLiteral("reference coil 2")), qPrintable(bad.error));
        // At rest: a voltage drives V/R, grounded or floating none.
        const Run rest = run(modelOf(twoWires(QStringLiteral(R"(, {"type": "coil", "name": "2", "role": "voltage", "V0": "1[mV]", "selection": {"only": ["w2"]}})"),
                                              QString(), QStringLiteral("{\"type\": \"stationary\"}"))));
        QVERIFY2(rest.error.isEmpty(), qPrintable(rest.error));
        QVERIFY2(std::abs(global(rest, QStringLiteral("mf.I_2")) * global(rest, QStringLiteral("mf.R_2")) / 1e-3 - 1) < 1e-9,
                 qPrintable(QString::number(global(rest, QStringLiteral("mf.I_2")))));
        QVERIFY(std::abs(global(rest, QStringLiteral("mf.V_2")) / 1e-3 - 1) < 1e-9);
        const Run restGrounded = run(modelOf(twoWires(QStringLiteral(R"(, {"type": "coil", "name": "2", "role": "grounded", "selection": {"only": ["w2"]}})"),
                                                      QString(), QStringLiteral("{\"type\": \"stationary\"}"))));
        QVERIFY2(restGrounded.error.isEmpty(), qPrintable(restGrounded.error));
        QCOMPARE(global(restGrounded, QStringLiteral("mf.I_2")), 0.0);
    }

    void theExamples()
    {
        // Phase 3's examples, solved as they come, against what they say.
        const QString dir = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
        auto load = [&](const QString& name) {
            QFile f(dir + name);
            if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
            Model m = modelOf(f.readAll());
            m.fileName = dir + name;
            return m;
        };
        QElapsedTimer timer;
        {
            // The coax: Z0 about 52 Ω; R from its DC value to the skin effect's
            // Rs/2π (1/a + 1/b); L falling to µ0/2π ln(b/a).
            timer.start();
            Model m = load(QStringLiteral("coax_rlgc.qfem"));
            const int frequencies = slowBuild() ? 2 : 9;
            if (slowBuild())
                for (Node& step : m.find(m.studies().front()->tag)->children)
                    if (step.type == QLatin1String("frequency")) step.props.insert(QStringLiteral("frequencies"), QStringLiteral("1[kHz], 10[MHz]"));
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QCOMPARE(r.solution->snapshotCount(), frequencies);
            SolutionSet set;
            set.solutions = r.study.solutions;
            const LineSweep sweep = lineSweep(set, *m.find(QStringLiteral("lp1")));
            QVERIFY2(sweep.error.isEmpty(), qPrintable(sweep.error));
            const LineRLGC& low = sweep.points.front();
            const LineRLGC& high = sweep.points.back();
            const double rdc = 1 / (SigmaCu * M_PI * 0.25e-6) + 1 / (SigmaCu * M_PI * (4e-6 - 1.75e-3 * 1.75e-3));
            const double delta = std::sqrt(2 / (2 * M_PI * high.frequency * Mu0 * SigmaCu));
            const double rs = 1 / (SigmaCu * delta) / (2 * M_PI) * (1 / 0.5e-3 + 1 / 1.75e-3);
            const double lext = Mu0 / (2 * M_PI) * std::log(3.5);
            const double z0 = 60 / std::sqrt(2.1) * std::log(3.5);
            qInfo("coax: R %.4f (DC %.4f) to %.4f Ω/m (skin %.4f), L %.1f to %.1f nH/m (external %.1f), Z0 %.2f Ω (%.2f), α %.4f dB/m; %s; %lld ms",
                  low.R, rdc, high.R, rs, low.L * 1e9, high.L * 1e9, lext * 1e9, high.Zc.real(), z0, high.alphaDb,
                  qPrintable(r.solution->log.filter(QStringLiteral("degrees of freedom")).join(QString())), timer.elapsed());
            QVERIFY(std::abs(low.R / rdc - 1) < 0.03);
            QVERIFY(std::abs(high.R / rs - 1) < 0.08);
            QVERIFY(high.L < low.L && high.L > lext && high.L < 1.05 * lext);
            QVERIFY(std::abs(high.Zc.real() / z0 - 1) < 0.02);
            const ExportText e = exportText(m, set, *m.find(QStringLiteral("ts1")), QStringLiteral("26.1.8"));
            QVERIFY2(e.error.isEmpty(), qPrintable(e.error));
            QCOMPARE(e.text.count(QRegularExpression(QStringLiteral("^[0-9]"), QRegularExpression::MultilineOption)), frequencies);
        }
        {
            // The gapped core: the gap's L at small currents, falling as it
            // saturates.
            timer.start();
            Model m = load(QStringLiteral("gapped_core_inductor.qfem"));
            const QStringList currents = slowBuild() ? QStringList{QStringLiteral("1"), QStringLiteral("12"), QStringLiteral("25")}
                                                     : QStringList{QStringLiteral("1"), QStringLiteral("4"), QStringLiteral("8"), QStringLiteral("12"),
                                                                   QStringLiteral("16"), QStringLiteral("20"), QStringLiteral("25")};
            // (A list of one list built by append: braced, some compilers copy it.)
            QJsonArray swept;
            swept.append(QJsonArray{QStringLiteral("I"), currents.join(QLatin1String(", ")), QStringLiteral("A")});
            for (Node& step : m.find(m.studies().front()->tag)->children)
                if (step.type == QLatin1String("sweep")) step.props.insert(QStringLiteral("parameters"), swept);
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QCOMPARE(int(r.study.solutions.size()), int(currents.size()));
            const Node* ind = m.find(QStringLiteral("ind"));
            std::vector<double> L;
            for (const auto& s : r.study.solutions) {
                const DerivedResult d = evaluateDerived(*s, *ind);
                QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
                L.push_back(d.values.at(0).value);
            }
            const double gap = Mu0 * 100 * 100 * 15e-3 * 20e-3 / 1e-3;
            QStringList each;
            for (double l : L) each << QString::number(l * 1e3, 'f', 3);
            qInfo("gapped core: L %s mH at %s A (the gap's %.3f mH); %lld ms", qPrintable(each.join(QLatin1String(", "))),
                  qPrintable(currents.join(QLatin1String(", "))), gap * 1e3, timer.elapsed());
            QVERIFY(L.front() > gap && L.front() < 1.6 * gap);
            // (The steel's permeability rises a little before its knee; past
            // it, the inductance falls.)
            for (std::size_t i = std::min<std::size_t>(3, L.size() - 1); i < L.size(); ++i) QVERIFY2(L[i] < L[i - 1], qPrintable(QString::number(i)));
            QVERIFY(L.back() < 0.7 * L.front());
        }
        {
            // Induction heating: tens of watts in a skin; the billet warmer.
            timer.start();
            const Run r = run(load(QStringLiteral("induction_heating.qfem")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double p = global(r, QStringLiteral("mf.P")), heat = global(r, QStringLiteral("ht.P")), tmax = global(r, QStringLiteral("ht.Tmax"));
            qInfo("induction heating: %.1f W induced, %.1f W heating, %.1f °C at most; %lld ms", p, heat, tmax - 273.15, timer.elapsed());
            QVERIFY(p > 5 && p < 500);
            QVERIFY2(std::abs(heat / p - 1) < 0.02, qPrintable(QStringLiteral("%1 %2").arg(heat).arg(p)));
            QVERIFY(tmax > 320 && tmax < 1200);
            // The skin: more at the surface than at the axis.
            QVERIFY(at(r, QStringLiteral("mf.Qrh"), {19.5, 0}) > 5 * at(r, QStringLiteral("mf.Qrh"), {2, 0}));
        }
        {
            // The heater's cross-section: the waveguide warmed, a phase shift.
            timer.start();
            const Model m = load(QStringLiteral("heater_crosssection.qfem"));
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double dT = at(r, QStringLiteral("T"), {0, 0.11}) - 293.15;
            const double phase = 2 * M_PI / 1.55e-6 * 1.86e-4 * dT * 1e-3;
            qInfo("heater: the waveguide %.1f K warmer, %.2f rad (π at %.1f mW); %lld ms", dT, phase, 20 * M_PI / phase, timer.elapsed());
            QVERIFY(dT > 1 && dT < 20);
            QVERIFY(std::abs(20 * M_PI / phase - 26) < 5);
        }
    }

    void spiceMatrices()
    {
        // Two wires: their inductance matrix as coupled inductors.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "circle", "name": "air", "radius": "30"},
                         {"type": "circle", "name": "w1", "center": ["-3", "0"], "radius": "0.5"},
                         {"type": "circle", "name": "w2", "center": ["3", "0"], "radius": "0.5"}],
            "materials": [{"material": "Air", "selection": {"only": ["air"]}}, {"material": "Copper", "selection": {"only": ["w1", "w2"]}}],
            "physics": [{"type": "magnetic", "tag": "mf", "features": [
              {"type": "coil", "name": "a", "I0": "1[A]", "selection": {"only": ["w1"]}},
              {"type": "coil", "name": "b", "I0": "-1[A]", "selection": {"only": ["w2"]}}]}],
            "mesh": {"size": "normal"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QString why;
        const QString cir = spiceMatrix(*r.solution, QStringLiteral("mf"), QStringLiteral("pair"), {}, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QVERIFY(cir.contains(QStringLiteral(".subckt pair a_p a_n b_p b_n")));
        QVERIFY(cir.contains(QRegularExpression(QStringLiteral("^Ka_b La Lb 0\\.\\d+"), QRegularExpression::MultilineOption)));
        QVERIFY(cir.contains(QRegularExpression(QStringLiteral("^Ra a_p a_m "), QRegularExpression::MultilineOption)));
        // No electrostatics: said.
        QVERIFY(spiceMatrix(*r.solution, QStringLiteral("es"), QStringLiteral("x"), {}, &why).isEmpty());
        QVERIFY(!why.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestFemMagnetics)
#include "test_fem_magnetics.moc"
