/*
 * test_fem_pde.cpp - the multiphysics solver's PDEs of the user's own
 *                    against what is known: Poisson's equation's
 *                    manufactured solution and its convergence, the Bratu
 *                    problem (nonlinear, 1D), convection and diffusion, a
 *                    Robin condition, a layered 1D domain, heat and waves in
 *                    time, eigenvalues of a string and a membrane, a
 *                    coupled system, the general form against the
 *                    coefficient form, anisotropic diffusion, about an axis
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_results.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QtTest>

#include <cmath>
#include <functional>
#include <vector>

using namespace qucs_s::fem;

namespace {

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

Run run(const Model& model, const QString& study = {})
{
    Run r;
    r.parameters = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *r.parameters);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.study = runStudy(model, r.parameters, g.topology, nullptr, study);
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

/// A rectangle (m) of a PDE: its physics' properties and features, its steps.
QByteArray model(const QString& size, const QString& physics, const QString& features, const QString& steps, const QString& hmax,
                 const QString& extra = {})
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "m" %6,
        "geometry": [{"type": "rectangle", "name": "box", "size": [%1]}],
        "physics": [{"type": "pde", "tag": "c" %2, "features": [%3]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%5"}}],
      "studies": [{"type": "study", "steps": [%4]}]})json")
        .arg(size, physics, features, steps, hmax, extra)
        .toUtf8();
}

const QString Stationary = QStringLiteral(R"x({"type": "stationary"})x");
const QString Ends = QStringLiteral(R"x({"type": "dirichlet", "r": "0", "selection": {"of": ["box"], "side": "left"}},
                                       {"type": "dirichlet", "r": "0", "selection": {"of": ["box"], "side": "right"}})x");

} // namespace

class TestFemPde : public QObject
{
    Q_OBJECT

private slots:
    void poisson()
    {
        // -Δu = 2π² sin(πx) sin(πy), 0 on the square's edges: u = sin(πx)
        // sin(πy); quadratic elements, the error falling as h³.
        auto error = [](const QString& hmax) {
            const Run r = run(modelOf(model(QStringLiteral(R"x("1", "1")x"), QString(),
                                            QStringLiteral(R"x({"type": "coefficient", "f": "2*pi^2*sin(pi*x/1[m])*sin(pi*y/1[m])", "selection": {"all": true}},
                                                              {"type": "dirichlet", "r": "0", "selection": {"exterior": true}})x"),
                                            Stationary, hmax)));
            if (!r.error.isEmpty()) qFatal("%s", qPrintable(r.error));
            double worst = 0;
            for (double x : {0.13, 0.31, 0.5, 0.77})
                for (double y : {0.21, 0.5, 0.64, 0.9})
                    worst = std::max(worst, std::abs(at(*r.solution, QStringLiteral("u"), {x, y}) - std::sin(M_PI * x) * std::sin(M_PI * y)));
            return worst;
        };
        const double coarse = error(QStringLiteral("0.2")), fine = error(QStringLiteral("0.1"));
        qInfo("Poisson: largest error %.3g (h 0.2), %.3g (h 0.1): %.1f times smaller", coarse, fine, coarse / fine);
        QVERIFY(fine < 5e-4);
        QVERIFY(coarse / fine > 5);
    }

    void bratu()
    {
        // -u'' = e^u on (0, 1), u = 0 at both ends - nonlinear: u =
        // -2 ln(cosh((x - ½) θ/2) / cosh(θ/4)), θ = √2 cosh(θ/4).
        const Run r = run(modelOf(model(QStringLiteral(R"x("1", "0.1")x"), QStringLiteral(R"x(, "space": "1d")x"),
                                        QStringLiteral(R"x({"type": "coefficient", "f": "exp(u)", "selection": {"all": true}}, )x") + Ends,
                                        Stationary, QStringLiteral("0.05"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        double theta = 1.5;
        for (int i = 0; i < 100; ++i) theta = std::sqrt(2.0) * std::cosh(theta / 4);
        auto exact = [&](double x) { return -2 * std::log(std::cosh((x - 0.5) * theta / 2) / std::cosh(theta / 4)); };
        const Solution& s = *r.solution;
        double worst = 0;
        for (double x : {0.1, 0.25, 0.5, 0.8}) worst = std::max(worst, std::abs(at(s, QStringLiteral("u"), {x, 0.05}) - exact(x)));
        const double slope = at(s, QStringLiteral("ux"), {0.25, 0.05}), slopeExact = -theta * std::tanh((0.25 - 0.5) * theta / 2);
        qInfo("Bratu: u(½) %.10f (%.10f), worst error %.2g, u'(¼) %.8f (%.8f); %g Newton iterations", at(s, QStringLiteral("u"), {0.5, 0.05}),
              exact(0.5), worst, slope, slopeExact, global(s, QStringLiteral("c.iterations")));
        QVERIFY(worst < 1e-9);
        QVERIFY(std::abs(slope - slopeExact) < 2e-6);
        QVERIFY(global(s, QStringLiteral("c.iterations")) >= 3);
    }

    void convectionAndDiffusion()
    {
        // -u'' + 20 u' = 0, u(0) = 0, u(1) = 1: (e^(20x) - 1)/(e^20 - 1).
        const QString ends = QStringLiteral(R"x({"type": "dirichlet", "r": "0", "selection": {"of": ["box"], "side": "left"}},
                                               {"type": "dirichlet", "r": "1", "selection": {"of": ["box"], "side": "right"}})x");
        const Run r = run(modelOf(model(QStringLiteral(R"x("1", "0.1")x"), QStringLiteral(R"x(, "space": "1d")x"),
                                        QStringLiteral(R"x({"type": "coefficient", "f": "0", "beta": ["20", "0"], "selection": {"all": true}}, )x") + ends,
                                        Stationary, QStringLiteral("0.05"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        for (double x : {0.5, 0.9, 0.97}) {
            const double u = at(*r.solution, QStringLiteral("u"), {x, 0.05}), exact = std::expm1(20 * x) / std::expm1(20.0);
            qInfo("convection and diffusion: u(%.2f) %.9f (%.9f)", x, u, exact);
            QVERIFY(std::abs(u - exact) < 1e-7);
        }
    }

    void robinAndLayers()
    {
        // -u'' = 0, u(0) = 1, u'(1) = -2 u(1) (-n·Γ = g - q u, q 2): u = 1 - 2x/3.
        const Run r = run(modelOf(model(QStringLiteral(R"x("1", "0.1")x"), QStringLiteral(R"x(, "space": "1d")x"),
                                        QStringLiteral(R"x({"type": "coefficient", "f": "0", "selection": {"all": true}},
                                                          {"type": "dirichlet", "r": "1", "selection": {"of": ["box"], "side": "left"}},
                                                          {"type": "flux", "q": "2", "selection": {"of": ["box"], "side": "right"}})x"),
                                        Stationary, QStringLiteral("0.05"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY(std::abs(at(*r.solution, QStringLiteral("u"), {1, 0.05}) - 1.0 / 3) < 1e-10);
        QVERIFY(std::abs(at(*r.solution, QStringLiteral("u"), {0.3, 0.05}) - 0.8) < 1e-10);
        // Two layers, c 1 then 4: -(c u')' = 0 from u(0) = 0 to u(2) = 1, the
        // flux c u' the same in both: u(1) = 0.8.
        const Run two = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "m",
            "geometry": [{"type": "rectangle", "name": "a", "size": ["1", "0.1"]}, {"type": "rectangle", "name": "b", "position": ["1", "0"], "size": ["1", "0.1"]}],
            "physics": [{"type": "pde", "tag": "c", "space": "1d", "features": [
              {"type": "coefficient", "f": "0", "selection": {"only": ["a"]}},
              {"type": "coefficient", "c": "4", "f": "0", "selection": {"only": ["b"]}},
              {"type": "dirichlet", "r": "0", "selection": {"of": ["a"], "side": "left"}},
              {"type": "dirichlet", "r": "1", "selection": {"of": ["b"], "side": "right"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.05"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json").toUtf8()));
        QVERIFY2(two.error.isEmpty(), qPrintable(two.error));
        const double mid = at(*two.solution, QStringLiteral("u"), {1, 0.05});
        qInfo("layers: u(1) %.12f (0.8); the flux %.9f and %.9f", mid, at(*two.solution, QStringLiteral("ux"), {0.5, 0.05}),
              4 * at(*two.solution, QStringLiteral("ux"), {1.5, 0.05}));
        QVERIFY(std::abs(mid - 0.8) < 1e-10);
    }

    void heatAndWavesInTime()
    {
        // ∂u/∂t = u'' from sin(πx): e^(-π² t) sin(πx).
        const Run heat = run(modelOf(model(QStringLiteral(R"x("1", "0.1")x"), QStringLiteral(R"x(, "space": "1d", "elements": 100)x"),
                                           QStringLiteral(R"x({"type": "coefficient", "f": "0", "selection": {"all": true}},
                                                             {"type": "init", "u0": "sin(pi*x/1[m])", "selection": {"all": true}}, )x") + Ends,
                                           QStringLiteral(R"x({"type": "transient", "times": "range(0, 0.05, 0.2)", "rtol": "1e-7"})x"),
                                           QStringLiteral("0.05"))));
        QVERIFY2(heat.error.isEmpty(), qPrintable(heat.error));
        Solution& h = *heat.solution;
        h.select(h.snapshotCount() - 1);
        const double u = at(h, QStringLiteral("u"), {0.5, 0.05}), exact = std::exp(-M_PI * M_PI * 0.2);
        qInfo("heat: u(½, 0.2) %.7f (%.7f); %s", u, exact, qPrintable(h.log.join(QLatin1String("; "))));
        QVERIFY(std::abs(u / exact - 1) < 1e-3);
        // ∂²u/∂t² = u'' from sin(πx) at rest: cos(πt) sin(πx), a period later as it was.
        const Run wave = run(modelOf(model(QStringLiteral(R"x("1", "0.1")x"), QStringLiteral(R"x(, "space": "1d", "elements": 100)x"),
                                           QStringLiteral(R"x({"type": "coefficient", "f": "0", "ea": "1", "da": "0", "selection": {"all": true}},
                                                             {"type": "init", "u0": "sin(pi*x/1[m])", "selection": {"all": true}}, )x") + Ends,
                                           QStringLiteral(R"x({"type": "transient", "times": "range(0, 0.5, 2)", "rtol": "1e-6"})x"),
                                           QStringLiteral("0.05"))));
        QVERIFY2(wave.error.isEmpty(), qPrintable(wave.error));
        Solution& w = *wave.solution;
        double values[5];
        for (int k = 0; k < 5; ++k) {
            w.select(k);
            values[k] = at(w, QStringLiteral("u"), {0.5, 0.05});
        }
        w.select(2);
        const double rate = at(w, QStringLiteral("ut"), {0.5, 0.05});
        qInfo("wave: u(½) at t 0, 0.5, 1, 1.5, 2: %.5f %.5f %.5f %.5f %.5f; at t 1 its rate %.5f", values[0], values[1], values[2], values[3],
              values[4], rate);
        QVERIFY(std::abs(values[2] + 1) < 0.01 && std::abs(values[4] - 1) < 0.02 && std::abs(values[1]) < 0.02);
        QVERIFY(std::abs(rate) < 0.05);
    }

    void eigenvalues()
    {
        // A string of length π: -u'' = λ u, held at its ends λ = n²; free (zero
        // flux) 0, 1, 4.
        const QString eig = QStringLiteral(R"x({"type": "eigenvalue", "count": 4})x");
        const Run held = run(modelOf(model(QStringLiteral(R"x("pi", "0.1")x"), QStringLiteral(R"x(, "space": "1d")x"),
                                           QStringLiteral(R"x({"type": "coefficient", "f": "0", "selection": {"all": true}}, )x") + Ends, eig,
                                           QStringLiteral("0.05"))));
        QVERIFY2(held.error.isEmpty(), qPrintable(held.error));
        Solution& s = *held.solution;
        QCOMPARE(s.snapshotCount(), 4);
        for (int k = 0; k < 4; ++k) {
            s.select(k);
            const double l = global(s, QStringLiteral("c.lambda"));
            qInfo("string: λ%d %.10f (%d)", k + 1, l, (k + 1) * (k + 1));
            QVERIFY(std::abs(l - (k + 1) * (k + 1)) < 1e-7 * (k + 1) * (k + 1));
        }
        // Its first eigenfunction a sine.
        s.select(0);
        QVERIFY(std::abs(std::abs(at(s, QStringLiteral("u"), {M_PI / 6, 0.05})) - 0.5) < 1e-6);
        const Run free = run(modelOf(model(QStringLiteral(R"x("pi", "0.1")x"), QStringLiteral(R"x(, "space": "1d")x"),
                                           QStringLiteral(R"x({"type": "coefficient", "f": "0", "selection": {"all": true}})x"),
                                           QStringLiteral(R"x({"type": "eigenvalue", "count": 3, "shift": "-0.5"})x"), QStringLiteral("0.05"))));
        QVERIFY2(free.error.isEmpty(), qPrintable(free.error));
        for (int k = 0; k < 3; ++k) {
            free.solution->select(k);
            QVERIFY(std::abs(global(*free.solution, QStringLiteral("c.lambda")) - k * k) < 1e-7);
        }
        // A square membrane: π² (m² + n²) - 2π², 5π² twice, 8π².
        const QString hmax = slowBuild() ? QStringLiteral("0.1") : QStringLiteral("0.05");
        const Run drum = run(modelOf(model(QStringLiteral(R"x("1", "1")x"), QString(),
                                           QStringLiteral(R"x({"type": "coefficient", "f": "0", "selection": {"all": true}},
                                                             {"type": "dirichlet", "r": "0", "selection": {"exterior": true}})x"),
                                           eig, hmax)));
        QVERIFY2(drum.error.isEmpty(), qPrintable(drum.error));
        const double expected[4] = {2, 5, 5, 8};
        for (int k = 0; k < 4; ++k) {
            drum.solution->select(k);
            const double l = global(*drum.solution, QStringLiteral("c.lambda")) / (M_PI * M_PI);
            qInfo("membrane: λ%d/π² %.7f (%g)", k + 1, l, expected[k]);
            QVERIFY(std::abs(l / expected[k] - 1) < (slowBuild() ? 1e-3 : 1e-4));
        }
    }

    void aSystem()
    {
        // -Δu + v = f1, -Δv = f2 with u = sin(πx) sin(πy), v = sin(2πx) sin(πy):
        // the coupling through F, its Jacobian's off-diagonal part.
        const QString u = QStringLiteral("sin(pi*x/1[m])*sin(pi*y/1[m])"), v = QStringLiteral("sin(2*pi*x/1[m])*sin(pi*y/1[m])");
        const Run r = run(modelOf(model(QStringLiteral(R"x("1", "1")x"), QStringLiteral(R"x(, "variables": "u, v")x"),
                                        QStringLiteral(R"x({"type": "coefficient", "f": "2*pi^2*%1 + %2 - v; 5*pi^2*%2", "selection": {"all": true}},
                                                          {"type": "dirichlet", "r": "0; 0", "selection": {"exterior": true}})x")
                                            .arg(u, v),
                                        Stationary, QStringLiteral("0.1"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const QPointF p(0.3, 0.6);
        const double eu = std::sin(M_PI * 0.3) * std::sin(M_PI * 0.6), evv = std::sin(2 * M_PI * 0.3) * std::sin(M_PI * 0.6);
        qInfo("system: u %.6f (%.6f), v %.6f (%.6f); %g Newton iterations", at(s, QStringLiteral("u"), p), eu, at(s, QStringLiteral("v"), p), evv,
              global(s, QStringLiteral("c.iterations")));
        QVERIFY(std::abs(at(s, QStringLiteral("u"), p) - eu) < 1e-3 && std::abs(at(s, QStringLiteral("v"), p) - evv) < 1e-3);
        QVERIFY(global(s, QStringLiteral("c.iterations")) <= 3);
    }

    void generalForm()
    {
        // The general form's Γ = -∇u, F = f: the coefficient form's answer.
        const QString f = QStringLiteral("2*pi^2*sin(pi*x/1[m])*sin(pi*y/1[m])");
        const Run a = run(modelOf(model(QStringLiteral(R"x("1", "1")x"), QString(),
                                        QStringLiteral(R"x({"type": "coefficient", "f": "%1", "selection": {"all": true}},
                                                          {"type": "dirichlet", "r": "0", "selection": {"exterior": true}})x").arg(f),
                                        Stationary, QStringLiteral("0.15"))));
        const Run b = run(modelOf(model(QStringLiteral(R"x("1", "1")x"), QString(),
                                        QStringLiteral(R"x({"type": "general", "Gx": "-ux", "Gy": "-uy", "F": "%1", "selection": {"all": true}},
                                                          {"type": "dirichlet", "r": "0", "selection": {"exterior": true}})x").arg(f),
                                        Stationary, QStringLiteral("0.15"))));
        QVERIFY2(a.error.isEmpty() && b.error.isEmpty(), qPrintable(a.error + b.error));
        for (double x : {0.2, 0.5, 0.7}) QVERIFY(std::abs(at(*a.solution, QStringLiteral("u"), {x, 0.4}) - at(*b.solution, QStringLiteral("u"), {x, 0.4})) < 1e-10);
        // Anisotropic: -(2 u_xx + u_yy) = 3π² sin(πx) sin(πy).
        const Run c = run(modelOf(model(QStringLiteral(R"x("1", "1")x"), QString(),
                                        QStringLiteral(R"x({"type": "coefficient", "diffusion": "anisotropic", "cxx": "2", "cyy": "1",
                                                           "f": "3*pi^2*sin(pi*x/1[m])*sin(pi*y/1[m])", "selection": {"all": true}},
                                                          {"type": "dirichlet", "r": "0", "selection": {"exterior": true}})x"),
                                        Stationary, QStringLiteral("0.1"))));
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        QVERIFY(std::abs(at(*c.solution, QStringLiteral("u"), {0.5, 0.5}) - 1) < 1e-3);
    }

    void aboutAnAxis()
    {
        // -(1/r)(r u_r)_r = 4 in a disc of radius 1, 0 at its rim: 1 - r²
        // (quadratic elements: exact).
        const Run r = run(modelOf(model(QStringLiteral(R"x("1", "0.2")x"), QString(),
                                        QStringLiteral(R"x({"type": "coefficient", "f": "4", "selection": {"all": true}},
                                                          {"type": "dirichlet", "r": "0", "selection": {"of": ["box"], "side": "right"}})x"),
                                        Stationary, QStringLiteral("0.1"), QStringLiteral(R"x(, "space": "axisymmetric")x"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        for (double rr : {0.0, 0.3, 0.75}) {
            const double u = at(*r.solution, QStringLiteral("u"), {rr, 0.1});
            QVERIFY2(std::abs(u - (1 - rr * rr)) < 1e-9, qPrintable(QString::number(u - (1 - rr * rr))));
        }
    }

    void theExamples()
    {
        const QString dir = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
        auto load = [&](const QString& name) {
            QFile f(dir + name);
            if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
            Model m = modelOf(f.readAll());
            m.fileName = dir + name;
            return m;
        };
        auto solve = [&](const Model& m) {
            const Run r = run(m);
            if (!r.error.isEmpty()) qFatal("%s: %s", qPrintable(m.fileName), qPrintable(r.error));
            return r;
        };
        QElapsedTimer timer;
        {
            // Bratu, swept: u(½) at each λ against its closed form.
            timer.start();
            const Run r = solve(load(QStringLiteral("bratu_ode.qfem")));
            QCOMPARE(int(r.study.solutions.size()), 4);
            const double lams[4] = {0.5, 1, 2, 3};
            QStringList said;
            for (int k = 0; k < 4; ++k) {
                double theta = 1;
                for (int i = 0; i < 200; ++i) theta = std::sqrt(2 * lams[k]) * std::cosh(theta / 4);
                const double exact = 2 * std::log(std::cosh(theta / 4)), u = at(*r.study.solutions[std::size_t(k)], QStringLiteral("u"), {0.5, 0.025});
                said << QStringLiteral("λ %1: %2 (%3)").arg(lams[k]).arg(u, 0, 'f', 7).arg(exact, 0, 'f', 7);
                QVERIFY2(std::abs(u - exact) < 1e-7, qPrintable(said.last()));
            }
            qInfo("Bratu: u(½) %s; %lld ms", qPrintable(said.join(QLatin1String(", "))), timer.elapsed());
        }
        {
            // The drum: λ = j² of Bessel's zeros.
            timer.start();
            Model m = load(QStringLiteral("drum_modes.qfem"));
            if (slowBuild()) m.find(m.mesh().tag)->props.insert(QStringLiteral("hmax"), QStringLiteral("0.12"));
            const Run r = solve(m);
            Solution& s = *r.solution;
            QCOMPARE(s.snapshotCount(), 6);
            const double j[6] = {2.404825558, 3.831705970, 3.831705970, 5.135622302, 5.135622302, 5.520078110};
            QStringList said;
            for (int k = 0; k < 6; ++k) {
                s.select(k);
                const double l = global(s, QStringLiteral("c.lambda"));
                said << QString::number(l, 'f', 5);
                QVERIFY2(std::abs(l / (j[k] * j[k]) - 1) < (slowBuild() ? 2e-3 : 3e-4), qPrintable(QString::number(l / (j[k] * j[k]))));
            }
            qInfo("drum: λ %s (5.78319, 14.68197 twice, 26.37462 twice, 30.47126); %lld ms", qPrintable(said.join(QLatin1String(", "))), timer.elapsed());
        }
        {
            // Fisher's front: between t 20 and 40 it travels near 2√(rD) = 2.
            // (A sanitized build: to t 20, on fewer elements.)
            timer.start();
            Model m = load(QStringLiteral("fisher_front.qfem"));
            if (slowBuild()) {
                for (const Node* p : m.physics()) m.find(p->tag)->props.insert(QStringLiteral("elements"), 150);
                for (Node& step : m.find(m.studies().front()->tag)->children)
                    if (step.type == QLatin1String("transient")) step.props.insert(QStringLiteral("times"), QStringLiteral("range(0, 5, 20)"));
            }
            const Run r = solve(m);
            Solution& s = *r.solution;
            auto front = [&](double t) -> double {
                s.select(s.snapshotAt(t));
                double last = 1;
                for (double x = 0; x <= 100; x += 0.05) {
                    const double u = at(s, QStringLiteral("u"), {x, 0.5});
                    if (u < 0.5) return x - 0.05 * (0.5 - u) / (last - u);
                    last = u;
                }
                return NAN;
            };
            const double t1 = slowBuild() ? 10 : 20, t2 = slowBuild() ? 20 : 40;
            const double a = front(t1), b = front(t2), speed = (b - a) / (t2 - t1);
            qInfo("Fisher: the front at %.2f (t %g), %.2f (t %g): %.3f per unit time (2√(rD) 2, less Bramson's 3/2t); %lld ms", a, t1, b, t2, speed,
                  timer.elapsed());
            QVERIFY(slowBuild() ? speed > 1.8 && speed < 2.2 : speed > 1.88 && speed < 2.0);
        }
        {
            // The ring: ln r / ln 2, and its flux 2π/ln 2 through the outer circle.
            timer.start();
            const Model m = load(QStringLiteral("annulus_laplace.qfem"));
            const Run r = solve(m);
            const double u = at(*r.solution, QStringLiteral("u"), {1.5, 0}), u2 = at(*r.solution, QStringLiteral("u"), {0, -1.3});
            double flux = NAN;
            for (const Node& d : m.results().child(QStringLiteral("derived"))->children)
                if (d.type == QLatin1String("integral")) {
                    const DerivedResult dr = evaluateDerived(*r.solution, d);
                    QVERIFY2(dr.error.isEmpty() && !dr.values.isEmpty(), qPrintable(dr.error));
                    flux = dr.values.front().value;
                }
            qInfo("ring: u(1.5) %.7f, u(1.3) %.7f (ln r/ln 2 %.7f, %.7f); the flux %.5f (2π/ln 2 %.5f); %lld ms", u, u2, std::log(1.5) / std::log(2.0),
                  std::log(1.3) / std::log(2.0), flux, 2 * M_PI / std::log(2.0), timer.elapsed());
            QVERIFY(std::abs(u - std::log(1.5) / std::log(2.0)) < 5e-5 && std::abs(u2 - std::log(1.3) / std::log(2.0)) < 5e-5);
            QVERIFY(std::abs(flux / (2 * M_PI / std::log(2.0)) - 1) < 2e-3);
        }
    }
};

QTEST_MAIN(TestFemPde)
#include "test_fem_pde.moc"
