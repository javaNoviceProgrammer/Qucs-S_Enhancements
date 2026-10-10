/*
 * test_fem_physics.cpp - the multiphysics solver's second phase against what
 *                        is known: heat in time (a slab's mode as it decays,
 *                        BDF2's and Euler's orders), Newton's method (k(T)
 *                        by Kirchhoff's transform, radiation), solid
 *                        mechanics (a cantilever, Lamé's thick cylinder
 *                        about its axis, thermal expansion free and held),
 *                        a spherical capacitor about its axis, sweeps that
 *                        mesh again or not, cut lines, DXF and SVG drawings
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_import.h"
#include "fem_results.h"

#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace qucs_s::fem;

namespace {

constexpr double Eps0 = 8.8541878188e-12;
constexpr double Sigma = 5.670374419e-8;

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

Model modelOf(const QByteArray& json, const QString& file = {})
{
    QString why;
    std::optional<Model> m = Model::fromJson(json, &why);
    if (!m) qFatal("not a model: %s", qPrintable(why));
    if (!why.isEmpty()) qWarning("%s", qPrintable(why));
    m->fileName = file;
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

/// A slab 0 < x < L (mm) of k 1, ρCp 1e6: T = 300 K at x = 0, insulated
/// elsewhere, starting at 300 + 10 sin(π x / 2L).
QByteArray slab(const QString& step)
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1", "parameters": [{"name": "L", "expression": "10[mm]"}],
      "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "slab", "size": ["L", "2"]}],
        "materials": [{"label": "m", "k": "1[W/(m*K)]", "rho": "1000[kg/m^3]", "Cp": "1000[J/(kg*K)]", "selection": {"all": true}}],
        "physics": [{"type": "heat", "tag": "ht", "features": [
          {"type": "init", "T0": "300[K] + 10[K]*sin(pi*x/(2*L))", "selection": {"all": true}},
          {"type": "temperature", "T0": "300[K]", "selection": {"of": ["slab"], "side": "left"}}]}],
        "mesh": {"size": "fine"}}],
      "studies": [{"type": "study", "steps": [%1]}]})json")
        .arg(step)
        .toUtf8();
}

} // namespace

class TestFemPhysics : public QObject
{
    Q_OBJECT

private slots:
    void valueLists();
    void slabInTime();
    void timeOrders();
    void newtonConductivity();
    void radiation();
    void cantilever();
    void thickCylinder();
    void thermalExpansion();
    void sphericalCapacitor();
    void sweeps();
    void cutLines();
    void dxfImport();
    void svgImport();
    void importedGeometry();
    void theExamples();
};

void TestFemPhysics::valueLists()
{
    const Scope& scope = Scope::builtins();
    Dim dim;
    QString why;
    std::optional<std::vector<double>> v = valueList(QStringLiteral("range(0, 0.25, 1)"), scope, &dim, &why);
    QVERIFY2(v, qPrintable(why));
    QCOMPARE(int(v->size()), 5);
    QCOMPARE(v->back(), 1.0);
    v = valueList(QStringLiteral("1[mm], 2[mm], linspace(3[mm], 5[mm], 3)"), scope, &dim, &why);
    QVERIFY2(v, qPrintable(why));
    QCOMPARE(int(v->size()), 5);
    QVERIFY(std::abs((*v)[4] - 5e-3) < 1e-15);
    QVERIFY(dim == Dim::length());
    QVERIFY(!valueList(QStringLiteral("1[mm], 2[s]"), scope, &dim, &why));
    QVERIFY(why.contains(QLatin1String("where the others are")));
    QVERIFY(!valueList(QStringLiteral("range(0, -1, 5)"), scope, &dim, &why));
}

void TestFemPhysics::slabInTime()
{
    // The mode decays as exp(-(π/2L)² α t), α = k/(ρCp) = 1e-6 m²/s: at
    // x = L its amplitude 10 K times that.
    const Run r = run(modelOf(slab(QString::fromUtf8(R"json({"type": "transient", "times": "range(0, 10, 100)", "rtol": "1e-4"})json"))));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    Solution& s = *r.solution;
    QCOMPARE(s.snapshotCount(), 11);
    QVERIFY(s.timeDependent);
    const double rate = std::pow(M_PI / (2 * 0.01), 2) * 1e-6;
    double worst = 0;
    for (int k = 0; k < s.snapshotCount(); ++k) {
        s.select(k);
        const double t = s.snapshotTime(k);
        const double exact = 300 + 10 * std::exp(-rate * t);
        const double got = at(r, QStringLiteral("T"), {10, 1});
        worst = std::max(worst, std::abs(got - exact));
        // Its globals at that time too.
        QVERIFY(std::abs(global(r, QStringLiteral("ht.Tmax")) - got) < 1e-6);
    }
    qInfo("slab in time: worst error %.2e K of 10 K (%s)", worst, qPrintable(s.log.join(QLatin1String("; "))));
    QVERIFY2(worst < 1e-2, qPrintable(QString::number(worst)));
    // A tighter tolerance: more steps, a smaller error (about as rtol^(2/3)
    // for a method of order 2 whose steps' errors are held).
    const Run tight = run(modelOf(slab(QStringLiteral("{\"type\": \"transient\", \"times\": \"range(0, 10, 100)\", \"rtol\": \"1e-6\"}"))));
    QVERIFY2(tight.error.isEmpty(), qPrintable(tight.error));
    double finer = 0;
    for (int k = 0; k < tight.solution->snapshotCount(); ++k) {
        tight.solution->select(k);
        finer = std::max(finer, std::abs(at(tight, QStringLiteral("T"), {10, 1}) - (300 + 10 * std::exp(-rate * tight.solution->snapshotTime(k)))));
    }
    qInfo("rtol 1e-6: worst error %.2e K", finer);
    QVERIFY2(finer < worst / 5, qPrintable(QStringLiteral("%1 against %2").arg(finer).arg(worst)));
    // t is a variable: an expression of it, at the selected snapshot.
    s.select(5);
    QVERIFY(std::abs(at(r, QStringLiteral("t"), {5, 1}) - 50) < 1e-12);
    QVERIFY(std::abs(at(r, QStringLiteral("time"), {5, 1}) - 50) < 1e-12);
}

void TestFemPhysics::timeOrders()
{
    // Fixed steps: halved, BDF2's error 4 times smaller, Euler's 2 times.
    const double rate = std::pow(M_PI / (2 * 0.01), 2) * 1e-6;
    const double exact = 300 + 10 * std::exp(-rate * 40);
    for (const QString& method : {QStringLiteral("bdf2"), QStringLiteral("euler")}) {
        double errors[2];
        for (int i = 0; i < 2; ++i) {
            const QString dt = i == 0 ? QStringLiteral("4") : QStringLiteral("2");
            const Run r = run(modelOf(slab(QString::fromUtf8(R"json({"type": "transient", "times": "0, 40", "stepping": "fixed", "dt": "%1[s]", "method": "%2"})json")
                                               .arg(dt, method))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            errors[i] = std::abs(at(r, QStringLiteral("T"), {10, 1}) - exact);
        }
        const double ratio = errors[0] / errors[1];
        qInfo("%s: errors %.3e, %.3e: ratio %.2f", qPrintable(method), errors[0], errors[1], ratio);
        if (method == QLatin1String("bdf2")) QVERIFY2(ratio > 3.2 && ratio < 5, qPrintable(QString::number(ratio)));
        else QVERIFY2(ratio > 1.7 && ratio < 2.3, qPrintable(QString::number(ratio)));
    }
}

void TestFemPhysics::newtonConductivity()
{
    // k = k0 (1 + b (T - 300)), 400 K at x = 0, 300 K at x = L: Kirchhoff's
    // θ = (T - 300) + b/2 (T - 300)² is linear in x.
    const Run r = run(modelOf(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "slab", "size": ["10", "2"]}],
        "materials": [{"label": "m", "k": "2[W/(m*K)]*(1 + 0.01[1/K]*(T - 300[K]))", "selection": {"all": true}}],
        "physics": [{"type": "heat", "tag": "ht", "features": [
          {"type": "temperature", "T0": "400[K]", "selection": {"of": ["slab"], "side": "left"}},
          {"type": "temperature", "T0": "300[K]", "selection": {"of": ["slab"], "side": "right"}}]}],
        "mesh": {"size": "normal"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary", "tolerance": "1e-10"}]}]})json"));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const double b = 0.01, thetaLeft = 100 + b / 2 * 100 * 100;
    for (double x : {2.5, 5.0, 7.5}) {
        const double theta = thetaLeft * (1 - x / 10);
        const double exact = 300 + (-1 + std::sqrt(1 + 2 * b * theta)) / b;
        const double got = at(r, QStringLiteral("T"), {x, 1});
        QVERIFY2(std::abs(got - exact) < 5e-4, qPrintable(QStringLiteral("x %1: %2, exactly %3").arg(x).arg(got).arg(exact)));
    }
    // The same discrete solution as Picard's iterations (to their tolerance).
    QByteArray picard = R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "slab", "size": ["10", "2"]}],
        "materials": [{"label": "m", "k": "2[W/(m*K)]*(1 + 0.01[1/K]*(T - 300[K]))", "selection": {"all": true}}],
        "physics": [{"type": "heat", "tag": "ht", "features": [
          {"type": "temperature", "T0": "400[K]", "selection": {"of": ["slab"], "side": "left"}},
          {"type": "temperature", "T0": "300[K]", "selection": {"of": ["slab"], "side": "right"}}]}],
        "mesh": {"size": "normal"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary", "nonlinear": "picard", "tolerance": "1e-12", "maxiter": 200}]}]})json";
    const Run p = run(modelOf(picard));
    QVERIFY2(p.error.isEmpty(), qPrintable(p.error));
    for (double x : {2.5, 5.0, 7.5}) {
        const double a = at(r, QStringLiteral("T"), {x, 1}), b2 = at(p, QStringLiteral("T"), {x, 1});
        QVERIFY2(std::abs(a - b2) < 1e-8, qPrintable(QStringLiteral("x %1: Newton %2, Picard %3").arg(x).arg(a, 0, 'g', 12).arg(b2, 0, 'g', 12)));
    }
    QVERIFY(p.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("iteration 10")));
    // Newton's: a few steps, not Picard's dozens.
    const QString log = r.solution->log.join(QLatin1Char('\n'));
    static const QRegularExpression steps(QStringLiteral("Newton converged in (\\d+) steps"));
    const QRegularExpressionMatch m = steps.match(log);
    QVERIFY2(m.hasMatch(), qPrintable(log));
    QVERIFY2(m.captured(1).toInt() <= 6, qPrintable(log));
}

void TestFemPhysics::radiation()
{
    // 1000 K at x = 0, radiating at x = L to 300 K: k (1000 - T) / L =
    // εσ (T⁴ - 300⁴).
    const Run r = run(modelOf(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "slab", "size": ["10", "2"]}],
        "materials": [{"label": "m", "k": "5[W/(m*K)]", "selection": {"all": true}}],
        "physics": [{"type": "heat", "tag": "ht", "order": "1", "features": [
          {"type": "temperature", "T0": "1000[K]", "selection": {"of": ["slab"], "side": "left"}},
          {"type": "radiation", "epsilon": "0.8", "Tamb": "300[K]", "selection": {"of": ["slab"], "side": "right"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary", "tolerance": "1e-11"}]}]})json"));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    double lo = 300, hi = 1000;
    for (int i = 0; i < 200; ++i) {
        const double t = (lo + hi) / 2;
        (5 * (1000 - t) / 0.01 > 0.8 * Sigma * (std::pow(t, 4) - std::pow(300.0, 4)) ? lo : hi) = t;
    }
    const double got = at(r, QStringLiteral("T"), {10, 1});
    QVERIFY2(std::abs(got - lo) < 1e-5, qPrintable(QStringLiteral("%1, exactly %2").arg(got).arg(lo)));
}

void TestFemPhysics::cantilever()
{
    // Plane stress, 100 x 10 mm, 1 mm thick, held at x = 0, 100 N down at its
    // end: Timoshenko's δ = P L³/(3EI) + P L/(κ G A).
    const Run r = run(modelOf(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "thickness": "1[mm]", "geometry": [
        {"type": "rectangle", "name": "beam", "size": ["100", "10"]}],
        "materials": [{"label": "steel", "E": "200[GPa]", "nu": "0.3", "selection": {"all": true}}],
        "physics": [{"type": "solid", "tag": "solid", "model": "planestress", "features": [
          {"type": "fixed", "selection": {"of": ["beam"], "side": "left"}},
          {"type": "load", "Fy": "-100[N]/(10[mm]*1[mm])", "selection": {"of": ["beam"], "side": "right"}}]}],
        "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"all": true}, "hmax": "2.5"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json"));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const double E = 200e9, nu = 0.3, L = 0.1, h = 0.01, t = 0.001, P = 100;
    const double I = t * h * h * h / 12, G = E / (2 * (1 + nu)), A = h * t;
    const double exact = P * L * L * L / (3 * E * I) + P * L / (5.0 / 6 * G * A);
    const double got = -at(r, QStringLiteral("solid.v"), {100, 5});
    qInfo("cantilever: tip %.6e m, Timoshenko %.6e m (%.2f %%)", got, exact, 100 * (got - exact) / exact);
    QVERIFY2(std::abs(got - exact) / exact < 0.015, qPrintable(QStringLiteral("%1, exactly %2").arg(got).arg(exact)));
    // What holds it carries the load.
    QVERIFY2(std::abs(global(r, QStringLiteral("solid.Ry")) - P) < 1e-6 * P, qPrintable(QString::number(global(r, QStringLiteral("solid.Ry")))));
    QVERIFY(std::abs(global(r, QStringLiteral("solid.Rx"))) < 1e-6 * P);
    // The bending stress at the root, top fibre, mid-way: M c / I.
    const double sx = at(r, QStringLiteral("solid.sx"), {50, 10});
    const double bending = P * 0.05 * (h / 2) / I;
    QVERIFY2(std::abs(sx - bending) / bending < 0.02, qPrintable(QStringLiteral("%1, beam theory %2").arg(sx).arg(bending)));
    // Its strain energy: half the load's work.
    QVERIFY2(std::abs(global(r, QStringLiteral("solid.W")) - 0.5 * P * got) / (0.5 * P * got) < 0.02,
             qPrintable(QString::number(global(r, QStringLiteral("solid.W")))));
}

void TestFemPhysics::thickCylinder()
{
    // About the axis: a long cylinder, 10 to 20 mm, 100 MPa inside, held
    // along z (plane strain): u(r) = (1+ν) p a²/(E (b²-a²)) ((1-2ν) r + b²/r),
    // σθ(a) = p (b² + a²)/(b² - a²).
    const Run r = run(modelOf(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "space": "axisymmetric", "geometry": [
        {"type": "rectangle", "name": "wall", "position": ["10", "0"], "size": ["10", "4"]}],
        "materials": [{"label": "steel", "E": "200[GPa]", "nu": "0.3", "selection": {"all": true}}],
        "physics": [{"type": "solid", "tag": "solid", "features": [
          {"type": "roller", "selection": {"of": ["wall"], "side": "bottom"}},
          {"type": "roller", "selection": {"of": ["wall"], "side": "top"}},
          {"type": "load", "kind": "pressure", "p": "100[MPa]", "selection": {"of": ["wall"], "side": "left"}}]}],
        "mesh": {"size": "fine"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json"));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QVERIFY(r.solution->axisymmetric());
    const double E = 200e9, nu = 0.3, p = 100e6, a = 0.01, b = 0.02;
    for (double radius : {0.01, 0.015, 0.02}) {
        const double exact = (1 + nu) * p * a * a / (E * (b * b - a * a)) * ((1 - 2 * nu) * radius + b * b / radius);
        const double got = at(r, QStringLiteral("solid.u"), {radius * 1000, 2});
        QVERIFY2(std::abs(got - exact) / exact < 1e-4, qPrintable(QStringLiteral("r %1: %2, exactly %3").arg(radius).arg(got).arg(exact)));
    }
    const double hoop = at(r, QStringLiteral("solid.sphi"), {10, 2});
    const double exactHoop = p * (b * b + a * a) / (b * b - a * a);
    QVERIFY2(std::abs(hoop - exactHoop) / exactHoop < 5e-3, qPrintable(QStringLiteral("%1, exactly %2").arg(hoop).arg(exactHoop)));
    // r is x about the axis.
    QVERIFY(std::abs(at(r, QStringLiteral("r"), {15, 2}) - 0.015) < 1e-12);
}

void TestFemPhysics::thermalExpansion()
{
    // 100 K warmer, α 1e-5: free, it grows α ΔT L and is not stressed; held
    // between two walls (plane stress), σx = -E α ΔT.
    for (const bool held : {false, true}) {
        const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "thickness": "1[mm]", "geometry": [
            {"type": "rectangle", "name": "bar", "size": ["20", "5"]}],
            "materials": [{"label": "m", "E": "100[GPa]", "nu": "0.25", "alpha": "1e-5[1/K]", "selection": {"all": true}}],
            "physics": [{"type": "solid", "tag": "solid", "model": "planestress", "features": [
              {"type": "elastic", "default": true, "thermal": true, "T": "393.15[K]", "selection": {"all": true}},
              {"type": "roller", "selection": {"of": ["bar"], "side": "left"}},
              {"type": "roller", "selection": {"of": ["bar"], "side": "bottom"}}%1]}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                                    .arg(held ? QString::fromUtf8(R"json(, {"type": "roller", "selection": {"of": ["bar"], "side": "right"}})json") : QString())
                                    .toUtf8();
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        if (!held) {
            QVERIFY(std::abs(at(r, QStringLiteral("solid.u"), {20, 2.5}) - 1e-5 * 100 * 0.02) < 1e-12);
            QVERIFY(std::abs(at(r, QStringLiteral("solid.v"), {20, 5}) - 1e-5 * 100 * 0.005) < 1e-12);
            QVERIFY(at(r, QStringLiteral("solid.mises"), {10, 2.5}) < 1e-3);
        } else {
            const double sx = at(r, QStringLiteral("solid.sx"), {10, 2.5});
            QVERIFY2(std::abs(sx + 100e9 * 1e-5 * 100) < 1e-3, qPrintable(QString::number(sx)));
            QVERIFY(std::abs(at(r, QStringLiteral("solid.sy"), {10, 2.5})) < 1e-3);
        }
    }
}

void TestFemPhysics::sphericalCapacitor()
{
    // About the axis, a sphere of 1 mm in one of 2 mm: C = 4πε0 ab/(b-a).
    const Run r = run(modelOf(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "space": "axisymmetric", "geometry": [
        {"type": "circle", "name": "outer", "radius": "2", "sector": "180", "rotation": "-90"},
        {"type": "circle", "name": "inner", "radius": "1", "sector": "180", "rotation": "-90"},
        {"type": "difference", "name": "gap", "input": ["outer"], "tools": ["inner"], "keep": true}],
        "materials": [{"label": "air", "material": "Air", "selection": {"all": true}}],
        "physics": [{"type": "electrostatics", "tag": "es", "selection": {"only": ["gap"]}, "features": [
          {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"between": [["gap"], ["inner"]]}},
          {"type": "ground", "selection": {"of": ["outer"], "except": {"box": [-0.001, -3, 0.001, 3]}}}]}],
        "mesh": {"size": "fine"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json"));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const double exact = 4 * M_PI * Eps0 * 1e-3 * 2e-3 / 1e-3;
    const double got = global(r, QStringLiteral("es.C11"));
    qInfo("sphere: C = %.8e F, exactly %.8e (%.2e)", got, exact, std::abs(got - exact) / exact);
    QVERIFY2(std::abs(got - exact) / exact < 1e-3, qPrintable(QStringLiteral("%1, exactly %2").arg(got).arg(exact)));
    // Its energy integrated round the axis: half C V².
    Node integral;
    integral.type = QStringLiteral("integral");
    integral.props.insert(QStringLiteral("expression"), QStringLiteral("es.We"));
    integral.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
    const DerivedResult d = evaluateDerived(*r.solution, integral);
    QVERIFY2(d.error.isEmpty() && d.values.size() == 1, qPrintable(d.error));
    QVERIFY2(std::abs(d.values[0].value - 0.5 * got) / (0.5 * got) < 2e-3, qPrintable(QString::number(d.values[0].value)));
}

void TestFemPhysics::sweeps()
{
    // A bar's length swept (the geometry made again), then its conductivity
    // (the mesh kept): R = L / (σ A).
    const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "L", "expression": "10[mm]"}, {"name": "s", "expression": "1e6[S/m]"}],
      "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "bar", "size": ["L", "1"]}],
        "materials": [{"label": "m", "sigma": "s", "selection": {"all": true}}],
        "physics": [{"type": "currents", "tag": "ec", "features": [
          {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["bar"], "side": "left"}},
          {"type": "ground", "selection": {"of": ["bar"], "side": "right"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "sweep", "parameters": [["L", "10, 20, 40", "mm"]]}, {"type": "stationary"}]}]})json";
    Run r = run(modelOf(json));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(int(r.study.solutions.size()), 3);
    QCOMPARE(r.study.swept, QStringList{QStringLiteral("L")});
    const double lengths[] = {0.01, 0.02, 0.04};
    for (int i = 0; i < 3; ++i) {
        const Solution& s = *r.study.solutions[std::size_t(i)];
        const double R = 1 / s.global(QStringLiteral("ec.G11")).value_or(NAN);
        const double exact = lengths[i] / (1e6 * 1e-3 * 1.0);   // per metre of thickness
        QVERIFY2(std::abs(R - exact) / exact < 1e-9, qPrintable(QStringLiteral("%1: %2, exactly %3").arg(s.label).arg(R).arg(exact)));
        QCOMPARE(s.sweptValues.value(QStringLiteral("L")), lengths[i]);
    }
    QCOMPARE(r.study.solutions[0]->label, QStringLiteral("L = 10 mm"));
    QVERIFY(r.study.solutions[0]->meshPtr() != r.study.solutions[1]->meshPtr());
    // The conductivity swept: one mesh for all.
    QByteArray other = json;
    other.replace(R"json([["L", "10, 20, 40", "mm"]])json", R"json([["s", "range(1e6, 1e6, 3e6)", "S/m"]])json");
    r = run(modelOf(other));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(int(r.study.solutions.size()), 3);
    QVERIFY(r.study.solutions[0]->meshPtr() == r.study.solutions[2]->meshPtr());
    // Two parameters, every combination: 3 x 2.
    other = json;
    other.replace(R"json([["L", "10, 20, 40", "mm"]])json", R"json([["L", "10, 20, 40", "mm"], ["s", "1e6, 2e6", "S/m"]])json");
    r = run(modelOf(other));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(int(r.study.solutions.size()), 6);
    QCOMPARE(r.study.solutions[1]->label, QStringLiteral("L = 10 mm, s = 2 MS/m"));
    // Its instances, for results.
    SolutionSet set{r.study.solutions, r.study.swept};
    QCOMPARE(int(instances(set, true).size()), 6);
    const std::vector<Instance> one = instances(set, false, 2);
    QCOMPARE(int(one.size()), 1);
    QCOMPARE(one[0].point, 1);
    // A unit missing for a parameter that has one: said.
    other = json;
    other.replace(R"json([["L", "10, 20, 40", "mm"]])json", R"json([["L", "10, 20", ""]])json");
    r = run(modelOf(other));
    QVERIFY(r.error.contains(QLatin1String("give its values a unit")));
}

void TestFemPhysics::cutLines()
{
    const Run r = run(modelOf(slab(QString::fromUtf8(R"json({"type": "transient", "times": "0, 20, 40"})json"))));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    SolutionSet set{{r.solution}, {}};
    const std::vector<Instance> all = instances(set, true);
    QCOMPARE(int(all.size()), 3);
    QCOMPARE(all[1].label, QStringLiteral("t = 20 s"));
    const Locator locator(*r.mesh);
    const double rate = std::pow(M_PI / (2 * 0.01), 2) * 1e-6;
    for (const Instance& in : all) {
        Solution* s = select(set, in);
        QVERIFY(s);
        const LineData line = lineData(*s, locator, QStringLiteral("T"), {0, 1}, {10, 1}, 11);
        QVERIFY2(line.error.isEmpty(), qPrintable(line.error));
        QCOMPARE(int(line.values.size()), 11);
        QVERIFY(std::abs(line.along.back() - 0.01) < 1e-15);
        for (int i = 0; i < 11; ++i) {
            const double x = 0.001 * i;
            const double exact = 300 + 10 * std::sin(M_PI * x / 0.02) * std::exp(-rate * in.time);
            QVERIFY2(std::abs(line.values[std::size_t(i)] - exact) < 2e-2, qPrintable(QStringLiteral("t %1, x %2").arg(in.time).arg(x)));
        }
    }
    // Outside the domains: no value.
    const LineData out = lineData(*r.solution, locator, QStringLiteral("T"), {-5, 1}, {5, 1}, 3);
    QVERIFY(std::isnan(out.values[0]) && std::isfinite(out.values[2]));
}

void TestFemPhysics::dxfImport()
{
    // A closed polyline (a 10 x 5 rectangle, its right side bulged out into a
    // half circle), a circle in it, three lines making a triangle, a spline;
    // in millimetres ($INSUNITS 4).
    const QByteArray dxf = "0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n4\n0\nENDSEC\n"
                           "0\nSECTION\n2\nENTITIES\n"
                           "0\nLWPOLYLINE\n8\nouter\n90\n4\n70\n1\n10\n0\n20\n0\n10\n10\n20\n0\n42\n1\n10\n10\n20\n5\n10\n0\n20\n5\n"
                           "0\nCIRCLE\n8\nouter\n10\n3\n20\n2.5\n40\n1\n"
                           "0\nLINE\n8\ntri\n10\n20\n20\n0\n11\n24\n21\n0\n"
                           "0\nLINE\n8\ntri\n10\n20\n20\n3\n11\n24\n21\n0\n"
                           "0\nLINE\n8\ntri\n10\n20\n20\n0\n11\n20\n21\n3\n"
                           "0\nTEXT\n8\ntri\n1\nhello\n"
                           "0\nENDSEC\n0\nEOF\n";
    ImportedDrawing d = readDxf(dxf);
    QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
    QCOMPARE(d.unit, 1e-3);
    QCOMPARE(int(d.paths.size()), 3);
    int closed = 0;
    for (const ImportedPath& p : d.paths) closed += p.closed ? 1 : 0;
    QCOMPARE(closed, 3);
    QVERIFY(d.warnings.join(QLatin1Char(' ')).contains(QLatin1String("TEXT")));
    // A layer alone.
    d = readDxf(dxf, {QStringLiteral("tri")});
    QCOMPARE(int(d.paths.size()), 1);
    QCOMPARE(int(d.paths[0].curves.size()), 3);
    // Not a DXF.
    QVERIFY(!readDxf("hello\nworld\n").error.isEmpty());
}

void TestFemPhysics::svgImport()
{
    // 20 x 10 mm (the viewBox in mm): a rectangle with rounded corners, a
    // circle, a path of a Bézier and an arc, a hidden one.
    const QByteArray svg = R"json(<svg xmlns="http://www.w3.org/2000/svg" width="20mm" height="10mm" viewBox="0 0 20 10">
      <rect x="1" y="1" width="8" height="6" rx="1"/>
      <g transform="translate(15, 5)"><circle cx="0" cy="0" r="2"/></g>
      <path d="M 10 9 C 11 8, 12 8, 13 9 A 1.5 1.5 0 0 1 10 9 Z"/>
      <rect x="0" y="0" width="1" height="1" style="display:none"/>
      <text x="0" y="0">label</text>
    </svg>)json";
    const ImportedDrawing d = readSvg(svg);
    QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
    QVERIFY(std::abs(d.unit - 1e-3) < 1e-15);
    QCOMPARE(int(d.paths.size()), 3);
    for (const ImportedPath& p : d.paths) QVERIFY(p.closed);
    // y turned up: the circle about (15, -5).
    bool found = false;
    for (const ImportedPath& p : d.paths)
        for (const Curve& c : p.curves)
            if (c.kind == Curve::Arc && std::abs(c.c.x() - 15) < 1e-12 && std::abs(c.c.y() + 5) < 1e-12) found = true;
    QVERIFY(found);
    QVERIFY(d.warnings.join(QLatin1Char(' ')).contains(QLatin1String("text")));
}

void TestFemPhysics::importedGeometry()
{
    // The drawings as an Import feature: domains, a hole, areas.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile f(dir.filePath(QStringLiteral("plate.svg")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"json(<svg xmlns="http://www.w3.org/2000/svg" width="10mm" height="5mm" viewBox="0 0 10 5">
                  <rect x="0" y="0" width="10" height="5"/><circle cx="5" cy="2.5" r="1"/></svg>)json");
    }
    const QString file = dir.filePath(QStringLiteral("plate.qfem"));
    Model m = modelOf(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "import", "name": "plate", "file": "plate.svg"}],
        "materials": [{"label": "m", "k": "1", "selection": {"all": true}}],
        "physics": [{"type": "heat", "tag": "ht", "features": [
          {"type": "temperature", "T0": "300[K]", "selection": {"exterior": true}},
          {"type": "source", "Q0": "1e6[W/m^3]", "selection": {"all": true}}]}],
        "mesh": {"size": "normal"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json",
                      file);
    std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    GeometryBuild g = buildGeometry(m, *ps);
    QVERIFY2(g.ok(), qPrintable(QStringList(g.errors.values()).join(QLatin1String("; "))));
    QCOMPARE(int(g.topology->domains.size()), 1);   // the circle a hole
    QVERIFY(std::abs(g.topology->domains[0].area - (50 - M_PI)) < 1e-3);
    QCOMPARE(g.files, QStringList{QFileInfo(dir.filePath(QStringLiteral("plate.svg"))).absoluteFilePath()});
    // Solids only where asked: curves alone make no domain.
    m.geometry().children[0].props.insert(QStringLiteral("curves"), QStringLiteral("curves"));
    g = buildGeometry(m, *ps);
    QVERIFY(!g.ok() || g.topology->domains.empty());
    // Scaled and moved; a DXF the same way.
    {
        QFile f(dir.filePath(QStringLiteral("shape.dxf")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n90\n4\n70\n1\n10\n0\n20\n0\n10\n4\n20\n0\n10\n4\n20\n2\n10\n0\n20\n2\n0\nENDSEC\n0\nEOF\n");
    }
    m.geometry().children[0].props.insert(QStringLiteral("file"), QStringLiteral("shape.dxf"));
    m.geometry().children[0].props.insert(QStringLiteral("curves"), QStringLiteral("solids"));
    m.geometry().children[0].props.insert(QStringLiteral("scale"), QStringLiteral("2"));
    m.geometry().children[0].props.insert(QStringLiteral("position"), QJsonArray{QStringLiteral("1"), QStringLiteral("1")});
    g = buildGeometry(m, *ps);
    QVERIFY2(g.ok(), qPrintable(QStringList(g.errors.values()).join(QLatin1String("; "))));
    QVERIFY(std::abs(g.topology->domains[0].area - 32) < 1e-9);
    QVERIFY(std::abs(g.topology->bounds.left() - 1) < 1e-9);
    // A file missing: said.
    m.geometry().children[0].props.insert(QStringLiteral("file"), QStringLiteral("none.dxf"));
    g = buildGeometry(m, *ps);
    QVERIFY(!g.ok());
    QVERIFY(g.errors.value(g.failedAt).contains(QLatin1String("cannot be read")));
    Q_UNUSED(slowBuild);
}

void TestFemPhysics::theExamples()
{
    // Each of phase 2's examples, solved as it comes, against what it says.
    const QString dir = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
    auto load = [&](const QString& name) {
        QFile f(dir + name);
        if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
        return modelOf(f.readAll(), dir + name);
    };
    {
        // The heater: hotter at each output time, a minute's 31 of them.
        const Run r = run(load(QStringLiteral("heater_transient.qfem")));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.solution->snapshotCount(), 31);
        double last = 0;
        for (int k = 0; k < r.solution->snapshotCount(); ++k) {
            r.solution->select(k);
            const double tmax = global(r, QStringLiteral("ht.Tmax"));
            QVERIFY2(tmax >= last - 1e-9, qPrintable(QStringLiteral("t %1: %2 after %3").arg(r.solution->snapshotTime(k)).arg(tmax).arg(last)));
            last = tmax;
        }
        qInfo("heater: %.2f °C after a minute (%s)", last - 273.15, qPrintable(r.solution->log.join(QLatin1String("; "))));
        QVERIFY(last - 273.15 > 25 && last - 273.15 < 200);
    }
    {
        // The bimetal: Timoshenko's tip, within a few per cent (plane stress
        // is no beam) - down: the aluminium on top grows more.
        const Run r = run(load(QStringLiteral("bimetal_strip.qfem")));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double n = 70.0 / 170, dAlpha = 23e-6 - 2.6e-6, h = 1e-3, L = 0.02;
        const double kappa = 24 * dAlpha * 100 / (h * (12 + (1 + n) * (1 + 1 / n)));
        const double exact = kappa * L * L / 2;
        const double tip = -at(r, QStringLiteral("solid.v"), {20, 0.5});
        qInfo("bimetal: tip %.4f mm, Timoshenko %.4f mm", tip * 1e3, exact * 1e3);
        QVERIFY2(std::abs(tip - exact) / exact < 0.05, qPrintable(QStringLiteral("%1, Timoshenko %2").arg(tip).arg(exact)));
    }
    {
        // The via: its liner's 2πε0εr H / ln(r2/r1), and somewhat more.
        const Run r = run(load(QStringLiteral("tsv_axisymmetric.qfem")));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double liner = 2 * M_PI * Eps0 * 3.9 * 50e-6 / std::log(5.5 / 5);
        const double c = global(r, QStringLiteral("es.C11"));
        qInfo("via: %.2f fF, the liner alone %.2f fF", c * 1e15, liner * 1e15);
        QVERIFY2(c > liner && c < 1.5 * liner, qPrintable(QString::number(c)));
    }
    {
        // The sweep: nine widths, Z0 falling as the strip widens; 3 mm, 50 Ω.
        const Run r = run(load(QStringLiteral("microstrip_sweep.qfem")));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 9);
        const Node* lp = r.solution->model().find(QStringLiteral("lp1"));
        QVERIFY(lp);
        double previous = INFINITY;
        for (const auto& s : r.study.solutions) {
            const DerivedResult d = evaluateDerived(*s, *lp);
            QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
            double z0 = NAN;
            for (const DerivedValue& v : d.values)
                if (v.name == QLatin1String("Z0")) z0 = v.value;
            QVERIFY2(z0 < previous, qPrintable(s->label));
            previous = z0;
            if (s->label == QLatin1String("w = 3 mm")) QVERIFY2(std::abs(z0 - 50.3) < 0.5, qPrintable(QString::number(z0)));
        }
    }
    {
        // The heat sink, drawn: radiation by Newton's method.
        const Run r = run(load(QStringLiteral("heatsink_import.qfem")));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.mesh->topology->domains.size()), 2);
        const double tmax = global(r, QStringLiteral("ht.Tmax")) - 273.15;
        qInfo("heat sink: %.1f °C at most, Rth %.3f K/W", tmax, global(r, QStringLiteral("ht.Rth")));
        QVERIFY(tmax > 30 && tmax < 150);
        QVERIFY(r.solution->log.join(QLatin1Char(' ')).contains(QLatin1String("Newton converged")));
    }
}

QTEST_GUILESS_MAIN(TestFemPhysics)
#include "test_fem_physics.moc"
