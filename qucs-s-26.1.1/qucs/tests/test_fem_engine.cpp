/*
 * test_fem_engine.cpp - the multiphysics solver's engine against what is
 *                       known: units and expressions; geometry made one
 *                       (domains, boundaries, points); meshes of good
 *                       angles and asked sizes; electrostatics, currents
 *                       and heat against closed-form answers (a coax, a
 *                       capacitor, a bar, a slab, a pipe), a microstrip
 *                       against qucs-transcalc's Hammerstad-Jensen, the
 *                       errors falling as the elements' order promises
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_materials.h"
#include "fem_results.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

using namespace qucs_s::fem;

namespace {

constexpr double Eps0 = 8.8541878188e-12;
constexpr double LightSpeed = 299792458.0;

/// The Debug build under the sanitizers is slower: CI's Linux.
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
    const std::optional<Model> m = Model::fromJson(json, &why);
    if (!m) qFatal("not a model: %s", qPrintable(why));
    if (!why.isEmpty()) qWarning("%s", qPrintable(why));
    return *m;
}

struct Run {
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<const Topology> topology;
    std::shared_ptr<const Mesh> mesh;
    std::shared_ptr<Solution> solution;
    QString error;
};

Run run(const Model& model, bool solve = true)
{
    Run r;
    r.parameters = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *r.parameters);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.topology = g.topology;
    const MeshBuild m = buildMesh(model, *r.parameters, g.topology);
    if (!m.mesh) {
        r.error = QStringLiteral("mesh: ") + m.error;
        return r;
    }
    r.mesh = m.mesh;
    if (!solve) return r;
    const StudyResult s = solveStudy(model, r.parameters, m.mesh, QString());
    if (!s.solution) {
        r.error = QStringLiteral("solve: ") + s.error;
        return r;
    }
    r.solution = s.solution;
    return r;
}

double global(const Run& r, const QString& name)
{
    const std::optional<double> v = r.solution ? r.solution->global(name) : std::nullopt;
    return v ? *v : NAN;
}

/// qucs-transcalc's microstrip (microstrip.cpp, microstrip_Z0()):
/// Hammerstad and Jensen's, with Wheeler's correction for the strip's
/// thickness, no cover. Z0 and εeff.
std::pair<double, double> transcalcMicrostrip(double w, double h, double t, double er)
{
    const double pi = M_PI, zf0 = 376.73031346177;
    auto z0Homogeneous = [&](double u) {
        const double f = 6.0 + (2.0 * pi - 6.0) * std::exp(-std::pow(30.666 / u, 0.7528));
        return (zf0 / (2.0 * pi)) * std::log(f / u + std::sqrt(1.0 + 4.0 / (u * u)));
    };
    auto fillingFactor = [&](double u, double e_r) {
        const double u2 = u * u, u3 = u2 * u, u4 = u3 * u;
        const double a = 1.0 + std::log((u4 + u2 / 2704) / (u4 + 0.432)) / 49.0 + std::log(1.0 + u3 / 5929.741) / 18.7;
        const double b = 0.564 * std::pow((e_r - 0.9) / (e_r + 3.0), 0.053);
        return std::pow(1.0 + 10.0 / u, -a * b);
    };
    auto deltaU = [&](double u, double t_h, double e_r) {
        if (t_h <= 0) return 0.0;
        double du = (t_h / pi) * std::log(1.0 + (4.0 * std::exp(1.0)) * std::pow(std::tanh(std::sqrt(6.517 * u)), 2.0) / t_h);
        return 0.5 * du * (1.0 + 1.0 / std::cosh(std::sqrt(e_r - 1.0)));
    };
    double u = w / h;
    const double t_h = t / h;
    const double z0h1 = z0Homogeneous(u + deltaU(u, t_h, 1.0));
    u += deltaU(u, t_h, er);
    const double z0hr = z0Homogeneous(u);
    const double qInf = fillingFactor(u, er);
    const double qT = (2.0 * std::log(2.0) / pi) * (t_h / std::sqrt(u));
    const double q = qInf - qT;   // no cover
    const double erEffT = 0.5 * (er + 1.0) + 0.5 * q * (er - 1.0);
    const double erEff = erEffT * std::pow(z0h1 / z0hr, 2.0);
    return {z0hr / std::sqrt(erEffT), erEff};
}

QByteArray coaxModel(int order, const QString& hmax)
{
    return QStringLiteral(R"({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "a", "expression": "1[mm]"}, {"name": "b", "expression": "3[mm]"}],
      "components": [{"unit": "mm",
        "geometry": [{"type": "circle", "name": "outer", "radius": "b"}, {"type": "circle", "name": "inner", "radius": "a"}],
        "materials": [{"material": "Air", "selection": {"all": true}}],
        "physics": [{"type": "electrostatics", "tag": "es", "order": "%1", "selection": {"only": ["outer"]}, "features": [
          {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["inner"]}},
          {"type": "ground", "selection": {"of": ["outer"]}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})")
        .arg(order)
        .arg(hmax)
        .toUtf8();
}

} // namespace

class TestFemEngine : public QObject
{
    Q_OBJECT

private slots:
    void units();
    void expressions();
    void hostileExpressions();
    void hostilePositions();
    void constantsWithTheirUnits();
    void modelRoundTrip();
    void proposalFormat();
    void formUnionCounts();
    void selectionRules();
    void booleans();
    void meshQuality();
    void parallelPlates();
    void floatingPotential();
    void coaxConvergence();
    void convergenceRates();
    void barResistance();
    void slabWithConvection();
    void pipeConduction();
    void heatSource();
    void jouleHeating();
    void microstripAgainstTranscalc();
    void helpfulErrors();
    void largeModelTiming();
    void everyExamplesSelectionsResolve();
    void aMeshStopsWhileItSmooths();
    void fuzz();
    void dumpKinds();
};

void TestFemEngine::units()
{
    std::optional<Unit> u = parseUnit(QStringLiteral("mm"));
    QVERIFY(u);
    QCOMPARE(u->scale, 1e-3);
    QVERIFY(u->dim == Dim::length());
    u = parseUnit(QStringLiteral("W/(m*K)"));
    QVERIFY(u && u->dim == Dim::of(1, 1, -3, 0, -1));
    u = parseUnit(QStringLiteral("um^2"));
    QVERIFY(u && std::abs(u->scale - 1e-12) < 1e-24 && u->dim == Dim::area());
    u = parseUnit(QStringLiteral("degC"));
    QVERIFY(u && u->offset == 273.15);
    u = parseUnit(QStringLiteral("kΩ"));
    QVERIFY(u && u->scale == 1e3 && u->dim == Dim::resistance());
    u = parseUnit(QStringLiteral("GPa"));
    QVERIFY(u && u->scale == 1e9);
    QString why;
    QVERIFY(!parseUnit(QStringLiteral("furlong"), &why));
    QVERIFY(why.contains(QStringLiteral("furlong")));
    QCOMPARE(formatQuantity(1.286e-10, Dim::capacitance()), QStringLiteral("128.6 pF"));
    QCOMPARE(formatQuantity(50.31, Dim::resistance()), QStringLiteral("50.31 Ω"));
    QCOMPARE(formatQuantity(0.0033, Dim::length()), QStringLiteral("3.3 mm"));
    QCOMPARE(dimName(Dim::of(1, 1, -3, 0, -1)), QStringLiteral("W/(m·K)"));
    // Beyond the prefixes (a to E): its SI unit's power of ten - "4.591e-11
    // aJ" was 4.591e-29 J (the update check of 110e8b9).
    QCOMPARE(formatQuantity(4.591e-29, Dim::of(2, 1, -2)), QStringLiteral("4.591e-29 J"));
    QCOMPARE(formatQuantity(2.132e-29, Dim::current()), QStringLiteral("2.132e-29 A"));
    QCOMPARE(formatQuantity(3e21, Dim::length()), QStringLiteral("3e+21 m"));
    QCOMPARE(formatQuantity(4.5e-18, Dim::of(2, 1, -2)), QStringLiteral("4.5 aJ"));
}

// The constants, each with its unit - the text that describes
// expressions saying which; a constant's value written as a bare number
// pointed out (an integral of 1.602e-19*semi.R was in µHz, not µA: the
// update check of 48d5fa8, 6).
void TestFemEngine::constantsWithTheirUnits()
{
    const Scope& b = Scope::builtins();
    auto dim = [&](const QString& text) { return Expression::compile(text, b).dim(); };
    for (const auto& [name, unit] : std::vector<std::pair<const char*, const char*>>{
             {"q0", "C"}, {"e_const", "C"}, {"eps0", "F/m"}, {"mu0", "H/m"}, {"c0", "m/s"}, {"kB", "J/K"}, {"h_const", "J*s"},
             {"sigma_const", "W/(m^2*K^4)"}, {"N_A_const", "1/mol"}, {"R_const", "J/(mol*K)"}, {"g_const", "m/s^2"}})
        QVERIFY2(dim(QLatin1String(name)) == dim(QStringLiteral("1[%1]").arg(QLatin1String(unit))), name);
    const QString text = constantsText();
    for (const char* said : {"q0 or e_const (the elementary charge, C)", "eps0 or epsilon0_const (the vacuum permittivity, F/m)", "kB or k_B_const"})
        QVERIFY2(text.contains(QLatin1String(said)), qPrintable(text));
    QStringList bare = bareConstants(QStringLiteral("1.602e-19*semi.R"));
    QCOMPARE(bare.size(), 1);
    QVERIFY2(bare.front().contains(QLatin1String("1.602e-19 is q0's value without its unit (the elementary charge, C)")), qPrintable(bare.front()));
    bare = bareConstants(QStringLiteral("es.C11/(8.854e-12*5) + 1.38e-23*T"));
    QCOMPARE(bare.size(), 2);
    QVERIFY(bare.front().contains(QLatin1String("eps0")) && bare.back().contains(QLatin1String("kB")));
    for (const char* none : {"q0*semi.R", "1.602e-19[C]*semi.R", "2.5e3*x", "x1.602e-19", "1e-19*y"})
        QVERIFY2(bareConstants(QLatin1String(none)).isEmpty(), none);
    QCOMPARE(bareConstants(QStringLiteral("2.998e8/f")).size(), 1);   // (c0, 299792458; 3e8 too, 0.07 % off)
}

void TestFemEngine::expressions()
{
    const Scope& b = Scope::builtins();
    auto value = [&](const QString& text) {
        const Expression e = Expression::compile(text, b);
        if (!e.isValid()) qWarning("%s: %s", qPrintable(text), qPrintable(e.error()));
        return e.isValid() && e.isConstant() ? e.constant() : NAN;
    };
    QCOMPARE(value(QStringLiteral("3[mm]")), 0.003);
    QCOMPARE(value(QStringLiteral("10u")), 1e-5);
    QCOMPARE(value(QStringLiteral("2.2k")), 2200.0);
    QCOMPARE(value(QStringLiteral("1GHz")), 1e9);
    QCOMPARE(value(QStringLiteral("3mm")), 0.003);
    QCOMPARE(value(QStringLiteral("1m")), 1e-3);   // Qucs's milli
    QCOMPARE(value(QStringLiteral("1[m]")), 1.0);
    QCOMPARE(value(QStringLiteral("20[degC]")), 293.15);
    QVERIFY(std::abs(value(QStringLiteral("-20[degC]")) - 253.15) < 1e-12);
    QCOMPARE(value(QStringLiteral("2^3^2")), 512.0);   // right to left
    QCOMPARE(value(QStringLiteral("-2^2")), -4.0);
    QCOMPARE(value(QStringLiteral("if(1>0, 2, 3) + max(1, 4)")), 6.0);
    QVERIFY(std::abs(value(QStringLiteral("sin(pi/2)")) - 1) < 1e-15);
    QVERIFY(std::abs(value(QStringLiteral("eps0*4.4")) - Eps0 * 4.4) < 1e-24);
    QCOMPARE(Expression::compile(QStringLiteral("3[mm]*2[mm]"), b).dim(), Dim::area());
    // Units that do not agree: said, not refused.
    const Expression mixed = Expression::compile(QStringLiteral("1[V] + 2[m]"), b);
    QVERIFY(mixed.isValid());
    QVERIFY(!mixed.warnings().isEmpty());
    QVERIFY(!Expression::compile(QStringLiteral("sin(1[m])"), b).warnings().isEmpty());
    // Unknown names: a suggestion.
    const Expression unknown = Expression::compile(QStringLiteral("epsO*2"), b);
    QVERIFY(!unknown.isValid());
    QVERIFY2(unknown.error().contains(QStringLiteral("eps0")), qPrintable(unknown.error()));
    QVERIFY(!Expression::compile(QStringLiteral("2pi"), b).isValid());
    QVERIFY(!Expression::compile(QStringLiteral("(1+2"), b).isValid());
    // Variables: compiled once, evaluated anywhere; constants folded.
    Scope s(&b);
    const int x = s.addVariable(QStringLiteral("x"), Dim::length());
    const int t = s.addVariable(QStringLiteral("T"), Dim::temperature());
    const Expression e = Expression::compile(QStringLiteral("5.8e7[S/m]/(1+0.0039[1/K]*(T-293.15[K])) * (1 + 0*x[1/m])"), s);
    QVERIFY(e.isValid());
    QVERIFY(!e.isConstant());
    QCOMPARE(e.dim(), Dim::of(-3, -1, 3, 2));
    double vals[2];
    vals[x] = 0.1;
    vals[t] = 393.15;
    QVERIFY(std::abs(e.eval(vals) - 5.8e7 / 1.39) < 1);
    QVERIFY(e.uses(t) && e.uses(x));
    // Functions of the model: analytic, inlined; a table.
    Scope f(&b);
    Scope::Function sq;
    sq.arguments << QStringLiteral("u");
    sq.body = QStringLiteral("u*u + 1");
    f.setFunction(QStringLiteral("sq"), sq);
    QCOMPARE(Expression::compile(QStringLiteral("sq(3)"), f).constant(), 10.0);
    Scope::Function tab;
    tab.arguments << QStringLiteral("x");
    auto table = std::make_shared<Expression::Table>();
    table->x = {0, 1, 2};
    table->y = {0, 10, 40};
    tab.table = table;
    f.setFunction(QStringLiteral("tab"), tab);
    QCOMPARE(Expression::compile(QStringLiteral("tab(1.5)"), f).constant(), 25.0);
    QCOMPARE(Expression::compile(QStringLiteral("tab(9)"), f).constant(), 40.0);
    // Of two, on a grid: bilinear between its points - f(u, v) = 10 u + 2 u
    // v + 2 v exactly, bilinear itself -, constant beyond.
    Scope::Function tab2;
    tab2.arguments << QStringLiteral("u") << QStringLiteral("v");
    auto grid = std::make_shared<Expression::Table>();
    grid->x = {0, 1, 3};
    grid->grid = {0, 2};
    grid->z = {0, 4, 10, 18, 30, 46};
    tab2.table = grid;
    f.setFunction(QStringLiteral("tab2"), tab2);
    QCOMPARE(Expression::compile(QStringLiteral("tab2(2, 1)"), f).constant(), 26.0);
    QCOMPARE(Expression::compile(QStringLiteral("tab2(0.5, 0.5)"), f).constant(), 6.5);
    QCOMPARE(Expression::compile(QStringLiteral("tab2(5, 3)"), f).constant(), 46.0);
    QCOMPARE(Expression::compile(QStringLiteral("tab2(-1, 1)"), f).constant(), 2.0);
    QVERIFY(!Expression::compile(QStringLiteral("tab2(1)"), f).isValid());
    // A model's: its rows (x, y, f) in any order, in their units; a table
    // not every x with every y refused.
    const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
        "functions": [{"type": "interpolation", "name": "g", "dimension": "2", "argunit": "um, um", "unit": "1/m^3",
                       "grid": [[3, 2, 46], [0, 0, 0], [1, 2, 18], [0, 2, 4], [3, 0, 30], [1, 0, 10]]},
                      {"type": "interpolation", "name": "bad", "dimension": "2", "grid": [[0, 0, 1], [1, 0, 2], [0, 1, 3]]}],
        "parameters": [{"name": "a", "expression": "g(2[um], 1[um])"}, {"name": "b", "expression": "g(0.5e-6, 1e-6)"}]})json";
    const Model m = modelOf(json);
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    QVERIFY2(!ps->errors.contains(QStringLiteral("a")), qPrintable(ps->errors.value(QStringLiteral("a"))));
    QCOMPARE(ps->values.value(QStringLiteral("a")), 26.0);
    QCOMPARE(ps->values.value(QStringLiteral("b")), 8.0);
    QCOMPARE(ps->dims.value(QStringLiteral("a")), Dim::of(-3));
    QVERIFY2(ps->errors.value(QStringLiteral("bad")).contains(QLatin1String("not a grid of every x with every y (3 points for 2 x 2)")),
             qPrintable(ps->errors.value(QStringLiteral("bad"))));
}

// What the expressions' fuzzer found (the fuzz of 21 October), each
// refused or answered at once: brackets and signs nested thousands deep
// (E1: the parser's recursion overflowed a stack), a sum thousands long (as
// deep a tree, compiled and freed by recursion), a function reading its
// argument twice nested eighteen deep (E2: written out, 2^18 copies, 33 s),
// a table at no number and a table short of values (E4: a read past its
// end), powers past any unit's (E5: wrapped, or undefined).
void TestFemEngine::hostileExpressions()
{
    const Scope& b = Scope::builtins();
    auto error = [&b](const QString& text) {
        const Expression e = Expression::compile(text, b);
        return e.isValid() ? QString() : e.error();
    };
    auto value = [&b](const QString& text) {
        const Expression e = Expression::compile(text, b);
        return e.isValid() && e.isConstant() ? e.constant() : NAN;
    };
    // E1.
    const int deep = 2247;
    QVERIFY(error(QString(deep, QLatin1Char('(')) + QLatin1Char('1') + QString(deep, QLatin1Char(')'))).contains(QLatin1String("nests too deeply")));
    QVERIFY(error(QString(deep, QLatin1Char('-')) + QLatin1Char('1')).contains(QLatin1String("nests too deeply")));
    QVERIFY(error(QStringLiteral("2^").repeated(deep) + QLatin1Char('1')).contains(QLatin1String("nests too deeply")));
    QVERIFY(error(QStringLiteral("1+").repeated(5000) + QLatin1Char('1')).contains(QLatin1String("the expression is too deep (more than 200 levels)")));
    // (A sum of 65 terms was refused as functions calling each other.)
    QCOMPARE(value(QString(50, QLatin1Char('(')) + QLatin1Char('7') + QString(50, QLatin1Char(')'))), 7.0);
    QCOMPARE(value(QStringLiteral("1+").repeated(198) + QLatin1Char('1')), 199.0);
    QCOMPARE(value(QString(50, QLatin1Char('-')) + QLatin1Char('3')), 3.0);
    // E2.
    Scope f(&b);
    const int x = f.addVariable(QStringLiteral("x"), Dim());
    Scope::Function sq;
    sq.arguments << QStringLiteral("u");
    sq.body = QStringLiteral("u*u + 1");
    f.setFunction(QStringLiteral("sq"), sq);
    auto nested = [](int n) {
        QString text = QStringLiteral("x");
        for (int k = 0; k < n; ++k) text = QStringLiteral("sq(") + text + QLatin1Char(')');
        return text;
    };
    QElapsedTimer timer;
    timer.start();
    const Expression big = Expression::compile(nested(18), f);
    QVERIFY(!big.isValid());
    QVERIFY2(big.error().contains(QLatin1String("too large")), qPrintable(big.error()));
    QVERIFY(timer.elapsed() < 10000);
    const Expression four = Expression::compile(nested(4), f);
    QVERIFY2(four.isValid(), qPrintable(four.error()));
    double v[1];
    v[x] = 1;
    QCOMPARE(four.eval(v), 677.0);   // 1, 2, 5, 26, 677
    Scope::Function loop;
    loop.arguments << QStringLiteral("u");
    loop.body = QStringLiteral("again(u) + 1");
    f.setFunction(QStringLiteral("again"), loop);
    const Expression endless = Expression::compile(QStringLiteral("again(x)"), f);
    QVERIFY(!endless.isValid());
    QVERIFY2(endless.error().contains(QLatin1String("call each other too deeply")), qPrintable(endless.error()));
    // E4.
    Scope::Function tab;
    tab.arguments << QStringLiteral("t");
    auto table = std::make_shared<Expression::Table>();
    table->x = {0, 1, 2};
    table->y = {0, 10, 40};
    tab.table = table;
    f.setFunction(QStringLiteral("tab"), tab);
    const Expression read = Expression::compile(QStringLiteral("tab(x)"), f);
    QVERIFY(read.isValid());
    v[x] = 1.5;
    QCOMPARE(read.eval(v), 25.0);
    v[x] = NAN;
    QVERIFY(std::isnan(read.eval(v)));
    Expression::Table shortOfValues;
    shortOfValues.x = {0, 1, 2};
    shortOfValues.y = {0, 10};
    QVERIFY(std::isnan(shortOfValues.at(1.5)));
    Expression::Table grid;
    grid.x = {0, 1, 3};
    grid.grid = {0, 2};
    grid.z = {0, 4, 10, 18, 30, 46};
    QCOMPARE(grid.at(2, 1), 26.0);
    QVERIFY(std::isnan(grid.at(NAN, 1)));
    QVERIFY(std::isnan(grid.at(1, NAN)));
    // E5.
    QVERIFY(!parseUnit(QStringLiteral("m^2147483647")));
    QVERIFY(!parseUnit(QStringLiteral("m^99999999999")));
    QVERIFY(!parseUnit(QStringLiteral("m^64")));
    QVERIFY(!parseUnit(QStringLiteral("m^-64")));
    const std::optional<Unit> u63 = parseUnit(QStringLiteral("m^63"));
    QVERIFY(u63 && u63->dim == Dim::of(63));
    QCOMPARE(int((Dim::of(100) * Dim::of(100)).p[0]), 127);
    QCOMPARE(int((Dim::of(-100) / Dim::of(100)).p[0]), -127);
    QCOMPARE(int(Dim::of(3).pow(63).p[0]), 127);
    QCOMPARE(int(Dim::of(-2).pow(std::numeric_limits<int>::max()).p[0]), -127);
    QVERIFY(error(QStringLiteral("2[m^999]")).contains(QLatin1String("less than 64")));
    // The deepest: 99 brackets, signs and powers; functions written out
    // to the compiler's 256 levels, and past them.
    QCOMPARE(value(QString(99, QLatin1Char('(')) + QLatin1Char('1') + QString(99, QLatin1Char(')'))), 1.0);
    QCOMPARE(value(QString(99, QLatin1Char('-')) + QLatin1Char('1')), -1.0);
    QCOMPARE(value(QStringLiteral("1^").repeated(99) + QLatin1Char('1')), 1.0);
    Scope::Function plus;
    plus.arguments << QStringLiteral("u");
    plus.body = QStringLiteral("u + 1 + 0");
    f.setFunction(QStringLiteral("plus"), plus);
    // (Read 191 high; written out, three levels a call: 311.)
    QString wrapped = QStringLiteral("1+").repeated(130) + QLatin1Char('x');
    for (int k = 0; k < 60; ++k) wrapped = QStringLiteral("plus(") + wrapped + QLatin1Char(')');
    const Expression past = Expression::compile(wrapped, f);
    QVERIFY(!past.isValid());
    QVERIFY2(past.error().contains(QLatin1String("too deep with its functions written out")), qPrintable(past.error()));
}

// The shipped implant's silicon a kilometre's million off in its model of
// micrometres, and further: built and meshed, or refused - said. (Its
// vertices' cells, rounded to a long long's largest, overflowed with a
// neighbour's index: UBSan; the fuzz of 21 October, G1.) At 1e300 its
// width is lost in its coordinates' digits: said so (G2). A mesh's hmax of
// 1e308 (M1).
void TestFemEngine::hostilePositions()
{
    QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/boron_implant_anneal.qfem"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray json = f.readAll();
    const QByteArray at = R"("position": ["0", "-0.8"])";
    QVERIFY(json.contains(at));
    // (Under the sanitizers, two: a build of the implant is 18 s there.)
    const std::vector<const char*> positions =
        slowBuild() ? std::vector<const char*>{"1e9[m]", "-1e300"} : std::vector<const char*>{"1e9[m]", "-1e9[m]", "1e300", "-1e300", "1e15", "0/0"};
    for (const char* x : positions) {
        QByteArray moved = json;
        moved.replace(at, QByteArray(R"("position": [")") + x + R"(", "-0.8"])");
        const Run r = run(modelOf(moved), false);
        QVERIFY2(r.mesh || !r.error.isEmpty(), x);
        // (Its width lost in its coordinates' digits at 1e300: said so,
        // not in the triangulation's words - "#1 is a duplicate of
        // #4294967295".)
        if (QByteArray(x).endsWith("1e300")) QVERIFY2(r.error.contains(QLatin1String("si: thinner than the union's tolerance")), qPrintable(r.error));
    }
    // The implant meshed with an hmax of 1e308: its rays across the narrow
    // regions reached as far, their grid's cells cast past an int's range
    // (UBSan; the fuzz of 21 October, M1, the photodiode's) - meshed.
    QJsonObject o = QJsonDocument::fromJson(json).object();
    QJsonArray components = o.value(QStringLiteral("components")).toArray();
    QJsonObject c = components.first().toObject();
    QJsonObject mesh = c.value(QStringLiteral("mesh")).toObject();
    mesh.insert(QStringLiteral("custom"), true);
    mesh.insert(QStringLiteral("hmax"), 1e308);
    c.insert(QStringLiteral("mesh"), mesh);
    components[0] = c;
    o.insert(QStringLiteral("components"), components);
    const Run far = run(modelOf(QJsonDocument(o).toJson()), false);
    QVERIFY2(far.mesh, qPrintable(far.error));
}

void TestFemEngine::modelRoundTrip()
{
    Model m = Model::blank();
    m.setParameters({{QStringLiteral("w"), QStringLiteral("3[mm]"), QStringLiteral("trace width")}});
    Node r = m.make(QStringLiteral("rectangle"));
    r.label = QStringLiteral("substrate");
    r.props.insert(QStringLiteral("size"), QJsonArray{QStringLiteral("20"), QStringLiteral("1.6")});
    QVERIFY(m.add(m.geometry().tag, r));
    Node es = m.make(QStringLiteral("electrostatics"));
    QCOMPARE(es.tag, QStringLiteral("es"));
    QCOMPARE(int(es.children.size()), 2);   // Charge Conservation, Zero Charge
    Node* added = m.add(m.component().tag, es);
    QVERIFY(added);
    Node term = m.make(QStringLiteral("electrostatics/terminal"));
    Node* t1 = m.add(added->tag, term);
    QVERIFY(t1);
    QCOMPARE(t1->text(QStringLiteral("name")), QStringLiteral("1"));
    Node* t2 = m.add(QStringLiteral("es"), m.make(QStringLiteral("electrostatics/terminal")));
    QCOMPARE(t2->text(QStringLiteral("name")), QStringLiteral("2"));
    // Form Union stays last.
    QCOMPARE(m.geometry().children.back().type, QStringLiteral("form union"));
    QString why;
    QVERIFY(!m.remove(m.geometry().children.back().tag, &why));
    QVERIFY(!why.isEmpty());
    // A Rectangle may not go under a physics.
    QVERIFY(!m.add(QStringLiteral("es"), m.make(QStringLiteral("rectangle")), -1, &why));

    const QByteArray json = m.toJson();
    const std::optional<Model> back = Model::fromJson(json, &why);
    QVERIFY2(back, qPrintable(why));
    QCOMPARE(back->toJson(), json);
    QCOMPARE(back->parameters().first().description, QStringLiteral("trace width"));
    // Renaming an object renames it in the rules.
    Model renamed = *back;
    t1 = renamed.find(t1->tag);
    t1->props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("of"), QJsonArray{QStringLiteral("substrate")}}});
    renamed.renameObject(QStringLiteral("substrate"), QStringLiteral("board"));
    QCOMPARE(renamed.find(t1->tag)->selection().value(QStringLiteral("of")).toArray().first().toString(), QStringLiteral("board"));
}

void TestFemEngine::proposalFormat()
{
    // As the proposal writes a model: names, "bottom of", a library material.
    const Model m = modelOf(R"({
      "format": "qucs-s multiphysics 1",
      "parameters": [{"name": "w", "expression": "3[mm]", "description": "trace width"}, {"name": "h", "expression": "1.6[mm]"}],
      "components": [{
        "space": "2D", "unit": "mm",
        "geometry": [
          {"type": "rectangle", "name": "substrate", "position": ["-10", "0"], "size": ["20", "h"]},
          {"type": "rectangle", "name": "trace", "position": ["-w/2", "h"], "size": ["w", "0.035"]},
          {"type": "rectangle", "name": "air", "position": ["-10", "0"], "size": ["20", "10"]},
          {"type": "form union"}
        ],
        "materials": [{"material": "FR-4", "domains": {"objects": ["substrate"]}}],
        "physics": [{"type": "electrostatics", "tag": "es", "features": [
          {"type": "terminal", "boundaries": {"of": ["trace"]}, "V0": "1[V]"},
          {"type": "ground", "boundaries": {"bottom of": ["substrate"]}}]}],
        "mesh": {"size": "fine"}
      }],
      "studies": [{"type": "study", "steps": [{"type": "stationary", "physics": ["es"]}]}]
    })");
    QCOMPARE(m.objectNames(), (QStringList{QStringLiteral("substrate"), QStringLiteral("trace"), QStringLiteral("air")}));
    const Node* ground = nullptr;
    for (const Node& f : m.find(QStringLiteral("es"))->children)
        if (f.type == QLatin1String("electrostatics/ground")) ground = &f;
    QVERIFY(ground);
    QCOMPARE(ground->selection().value(QStringLiteral("side")).toString(), QStringLiteral("bottom"));
    QCOMPARE(m.materials().children.front().text(QStringLiteral("library")), QStringLiteral("FR-4"));
    const Run r = run(m, false);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    // Three domains: the air alone, the substrate, the trace.
    QCOMPARE(int(r.topology->domains.size()), 3);
    QString why;
    const QVector<int> bottom = resolveSelection(*r.topology, m, Level::Boundary, ground->selection(), &why);
    QVERIFY2(why.isEmpty(), qPrintable(why));
    QCOMPARE(bottom.size(), 1);
    QCOMPARE(r.topology->vertices[std::size_t(r.topology->boundaries[std::size_t(bottom.first())].start)].y(), 0.0);
}

void TestFemEngine::formUnionCounts()
{
    // A circle in a square: 2 domains, 4 + 4 boundaries, 4 + 4 points.
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "sq", "base": "center", "size": ["4", "4"]},
        {"type": "circle", "name": "disk", "radius": "1"}]}]})");
    const Run r = run(m, false);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(int(r.topology->domains.size()), 2);
    QCOMPARE(int(r.topology->boundaries.size()), 8);
    QCOMPARE(int(r.topology->points.size()), 8);
    // Domain 1 is the square's (leftmost), domain 2 the disk.
    QCOMPARE(r.topology->domains[1].owner, 1);
    QVERIFY(std::abs(r.topology->domains[1].area - M_PI) < 0.01);
    QVERIFY(std::abs(r.topology->domains[0].area - (16 - M_PI)) < 0.01);
    // A boundary on the circle: one curve, an arc.
    int arcs = 0;
    for (const Topology::Boundary& b : r.topology->boundaries)
        if (b.curve >= 0 && r.topology->curves[std::size_t(b.curve)].kind == Curve::Arc) {
            ++arcs;
            QVERIFY(b.up >= 0 && b.down >= 0);   // between two domains
        }
    QCOMPARE(arcs, 4);

    // Two rectangles side by side: their shared edge one boundary.
    const Model two = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "a", "size": ["1", "1"]},
        {"type": "rectangle", "name": "b", "position": ["1", "0"], "size": ["1", "1"]}]}]})");
    const Run r2 = run(two, false);
    QCOMPARE(int(r2.topology->domains.size()), 2);
    QCOMPARE(int(r2.topology->boundaries.size()), 7);
    QCOMPARE(int(r2.topology->points.size()), 6);
    // A T: a rectangle on another's middle.
    const Model tee = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "base", "size": ["3", "1"]},
        {"type": "rectangle", "name": "stem", "position": ["1", "1"], "size": ["1", "2"]}]}]})");
    const Run r3 = run(tee, false);
    QCOMPARE(int(r3.topology->domains.size()), 2);
    QCOMPARE(int(r3.topology->boundaries.size()), 9);
}

void TestFemEngine::selectionRules()
{
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "air", "position": ["-10", "0"], "size": ["20", "10"]},
        {"type": "rectangle", "name": "substrate", "position": ["-10", "0"], "size": ["20", "1.6"]},
        {"type": "rectangle", "name": "trace", "position": ["-1.5", "1.6"], "size": ["3", "0.035"]}],
       "selections": [{"label": "metal", "level": "boundary", "selection": {"of": ["trace"]}}]}]})");
    const Run r = run(m, false);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const Topology& t = *r.topology;
    QCOMPARE(int(t.domains.size()), 3);
    QString why;
    auto pick = [&](Level level, const char* json) {
        const QJsonObject rule = QJsonDocument::fromJson(QByteArray(json)).object();
        const QVector<int> got = resolveSelection(t, m, level, rule, &why);
        return got;
    };
    QCOMPARE(pick(Level::Domain, R"({"all": true})").size(), 3);
    QCOMPARE(pick(Level::Domain, R"({"objects": ["air"]})").size(), 3);   // the air box holds all
    QCOMPARE(pick(Level::Domain, R"({"only": ["air"]})").size(), 1);      // its own, the smallest
    QCOMPARE(pick(Level::Domain, R"({"only": ["substrate"]})").size(), 1);
    QCOMPARE(pick(Level::Boundary, R"({"of": ["trace"]})").size(), 4);
    QCOMPARE(pick(Level::Boundary, R"({"named": "metal"})").size(), 4);
    QCOMPARE(pick(Level::Boundary, R"({"of": ["trace"], "side": "top"})").size(), 1);
    QCOMPARE(pick(Level::Boundary, R"({"of": ["substrate"], "side": "bottom"})").size(), 1);
    // Between the substrate and the air alone: its top, either side of the trace.
    QCOMPARE(pick(Level::Boundary, R"({"between": ["substrate", "air"]})").size(), 2);
    QCOMPARE(pick(Level::Boundary, R"({"exterior": true})").size(), 6);
    QCOMPARE(pick(Level::Boundary, R"({"of": ["trace"], "except": {"of": ["trace"], "side": "bottom"}})").size(), 3);
    QCOMPARE(pick(Level::Domain, R"({"numbers": [1, 3]})"), (QVector<int>{0, 2}));
    QCOMPARE(pick(Level::Domain, R"({"box": [-2, 1.5, 2, 1.7]})").size(), 1);
    QVERIFY(why.isEmpty());
    pick(Level::Boundary, R"({"of": ["nothing"]})");
    QVERIFY(why.contains(QStringLiteral("nothing")));
    pick(Level::Domain, R"({"numbers": [7]})");
    QVERIFY(why.contains(QStringLiteral("7")));
    QCOMPARE(describeSelection(QJsonObject{{QStringLiteral("of"), QJsonArray{QStringLiteral("trace")}}, {QStringLiteral("side"), QStringLiteral("top")}},
                               Level::Boundary),
             QStringLiteral("the boundaries of trace (top)"));
}

void TestFemEngine::booleans()
{
    // A square with a hole: one domain, its boundaries round both.
    const Model hole = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "sq", "base": "center", "size": ["4", "4"]},
        {"type": "circle", "name": "c", "radius": "1"},
        {"type": "difference", "name": "plate", "input": ["sq"], "tools": ["c"]}]}]})");
    Run r = run(hole, false);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(int(r.topology->domains.size()), 1);
    QVERIFY(std::abs(r.topology->domains[0].area - (16 - M_PI)) < 0.01);
    QCOMPARE(int(r.topology->boundaries.size()), 8);
    // The hole's edges keep their arcs.
    int arcs = 0;
    for (const Topology::Boundary& b : r.topology->boundaries)
        if (b.curve >= 0 && r.topology->curves[std::size_t(b.curve)].kind == Curve::Arc) ++arcs;
    QCOMPARE(arcs, 4);
    // Two squares united: one outline of eight edges.
    const Model united = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "a", "size": ["2", "2"]},
        {"type": "rectangle", "name": "b", "position": ["1", "1"], "size": ["2", "2"]},
        {"type": "union", "name": "u", "input": ["a", "b"]}]}]})");
    r = run(united, false);
    QCOMPARE(int(r.topology->domains.size()), 1);
    QCOMPARE(int(r.topology->boundaries.size()), 8);
    QVERIFY(std::abs(r.topology->domains[0].area - 7) < 1e-9);
    // Kept apart inside: three domains.
    const Model kept = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "a", "size": ["2", "2"]},
        {"type": "rectangle", "name": "b", "position": ["1", "1"], "size": ["2", "2"]},
        {"type": "union", "name": "u", "input": ["a", "b"], "interior": true}]}]})");
    r = run(kept, false);
    QCOMPARE(int(r.topology->domains.size()), 3);
    // An intersection; a move, a rotation, an array.
    const Model moved = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "a", "size": ["2", "2"]},
        {"type": "rectangle", "name": "b", "position": ["1", "1"], "size": ["2", "2"]},
        {"type": "intersection", "name": "i", "input": ["a", "b"]},
        {"type": "rotate", "name": "r", "input": ["i"], "angle": "90", "center": ["1", "1"]},
        {"type": "array", "name": "row", "input": ["r"], "count": ["3", "1"], "displacement": ["5", "0"]}]}]})");
    r = run(moved, false);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(int(r.topology->domains.size()), 3);
    for (const Topology::Domain& d : r.topology->domains) QVERIFY(std::abs(d.area - 1) < 1e-9);
    QCOMPARE(r.topology->objectNames, (QStringList{QStringLiteral("row_1"), QStringLiteral("row_2"), QStringLiteral("row_3")}));
    {
        // The array's name selects its copies; a copy's its own; no other
        // stem does.
        QString why;
        auto pick = [&](const char* rule) { return resolveSelection(*r.topology, moved, Level::Domain, QJsonDocument::fromJson(rule).object(), &why); };
        QCOMPARE(pick(R"({"only": ["row"]})").size(), 3);
        QCOMPARE(pick(R"({"objects": ["row"]})").size(), 3);
        QVERIFY(why.isEmpty());
        QCOMPARE(pick(R"({"only": ["row_2"]})").size(), 1);
        QCOMPARE(pick(R"({"only": ["ro"]})").size(), 0);
        QVERIFY2(why.contains(QLatin1String("ro")), qPrintable(why));
        // The array's input, used up: said - the array names its copies.
        why.clear();
        QCOMPARE(pick(R"({"only": ["r"]})").size(), 0);
        QVERIFY2(why.contains(QLatin1String("array row's input")), qPrintable(why));
        // (A name ending in s: rows' input, not rows's - the update check of
        // 8bd5709.)
        const Model rows = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
            {"type": "rectangle", "name": "r", "size": ["2", "2"]},
            {"type": "array", "name": "rows", "input": ["r"], "count": ["3", "1"], "displacement": ["5", "0"]}]}]})");
        const Run rr = run(rows, false);
        why.clear();
        QCOMPARE(resolveSelection(*rr.topology, rows, Level::Domain, QJsonDocument::fromJson(R"({"only": ["r"]})").object(), &why).size(), 0);
        QVERIFY2(why.contains(QLatin1String("array rows' input")) && !why.contains(QLatin1String("rows's")), qPrintable(why));
    }
    // An unknown input: said, with the objects there are.
    const Model bad = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "a", "size": ["2", "2"]},
        {"type": "move", "name": "m", "input": ["z"], "displacement": ["1", "0"]}]}]})");
    const ParameterScope ps;
    const GeometryBuild g = buildGeometry(bad, *evaluateParameters(bad));
    QVERIFY(!g.ok());
    QVERIFY(g.errors.value(g.failedAt).contains(QStringLiteral("a")));
}

void TestFemEngine::meshQuality()
{
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "sq", "base": "center", "size": ["10", "10"]},
        {"type": "circle", "name": "c", "radius": "2"},
        {"type": "polygon", "name": "tri", "points": [["-4", "-4"], ["-1", "-4"], ["-4", "-2"]]}],
        "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["c"]}, "hmax": "0.2"}]}}]})");
    const Run r = run(m, false);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const Mesh& mesh = *r.mesh;
    qInfo("mesh: %d nodes, %d triangles; min angle %.1f°, quality min %.3f mean %.3f", int(mesh.nodes.size()),
          int(mesh.triangles.size()), mesh.minAngle, mesh.minQuality, mesh.meanQuality);
    QVERIFY(mesh.minAngle > 20);
    QVERIFY(mesh.meanQuality > 0.85);
    // Every triangle in a domain, counter-clockwise; the areas add up.
    double area = 0;
    std::vector<double> domainArea(r.topology->domains.size(), 0);
    double longestInDisk = 0;
    for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
        const auto& v = mesh.triangles[t];
        const QPointF a = mesh.nodes[std::size_t(v[0])], b = mesh.nodes[std::size_t(v[1])], c = mesh.nodes[std::size_t(v[2])];
        const double ar = ((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2;
        QVERIFY(ar > 0);
        area += ar;
        QVERIFY(mesh.domain[t] >= 0);
        domainArea[std::size_t(mesh.domain[t])] += ar;
        if (r.topology->domains[std::size_t(mesh.domain[t])].owner == 1)
            for (int k = 0; k < 3; ++k) {
                const QPointF p = mesh.nodes[std::size_t(v[std::size_t(k)])], q = mesh.nodes[std::size_t(v[std::size_t((k + 1) % 3)])];
                longestInDisk = std::max(longestInDisk, std::hypot(p.x() - q.x(), p.y() - q.y()));
            }
    }
    QVERIFY(std::abs(area - 100) < 1e-6);
    for (std::size_t d = 0; d < domainArea.size(); ++d)
        QVERIFY2(std::abs(domainArea[d] - r.topology->domains[d].area) < 0.02 * r.topology->domains[d].area,
                 qPrintable(QStringLiteral("domain %1: %2 / %3").arg(d + 1).arg(domainArea[d]).arg(r.topology->domains[d].area)));
    // The disk's elements as asked.
    QVERIFY2(longestInDisk < 0.2 * 1.6, qPrintable(QString::number(longestInDisk)));
    // Its boundary nodes on the circle.
    for (const Mesh::Edge& e : mesh.edges) {
        if (e.curve < 0 || r.topology->curves[std::size_t(e.curve)].kind != Curve::Arc) continue;
        for (int n : {e.a, e.b}) QVERIFY(std::abs(std::hypot(mesh.nodes[std::size_t(n)].x(), mesh.nodes[std::size_t(n)].y()) - 2) < 1e-9);
    }
    // The same mesh again: the same.
    const Run again = run(m, false);
    QCOMPARE(again.mesh->triangles.size(), mesh.triangles.size());
}

void TestFemEngine::parallelPlates()
{
    // A capacitor whose sides carry no charge: its field uniform, C = ε w / d.
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "gap", "size": ["10", "1"]}],
        "materials": [{"label": "dielectric", "epsilonr": "4", "selection": {"all": true}}],
        "physics": [{"type": "electrostatics", "tag": "es", "order": "1", "features": [
          {"type": "terminal", "name": "1", "V0": "5[V]", "selection": {"of": ["gap"], "side": "top"}},
          {"type": "ground", "selection": {"of": ["gap"], "side": "bottom"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    const Run r = run(m);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const double exact = Eps0 * 4 * 10e-3 / 1e-3;   // per metre of thickness
    QVERIFY2(std::abs(global(r, QStringLiteral("es.C11")) - exact) < 1e-9 * exact, qPrintable(QString::number(global(r, QStringLiteral("es.C11")))));
    QVERIFY(std::abs(global(r, QStringLiteral("es.Q_1")) - 5 * exact) < 1e-9 * 5 * exact);
    QVERIFY(std::abs(global(r, QStringLiteral("es.W")) - 0.5 * exact * 25) < 1e-9 * exact * 25);
    // E = 5 kV/m everywhere; V linear.
    const Locator locator(*r.mesh);
    QString why;
    QVERIFY(std::abs(*evaluateAt(*r.solution, locator, QStringLiteral("es.Ey"), {3.3, 0.4}, &why) + 5000) < 1e-6);
    QVERIFY(std::abs(*evaluateAt(*r.solution, locator, QStringLiteral("V"), {7, 0.25}, &why) - 1.25) < 1e-9);
    QVERIFY(std::abs(*evaluateAt(*r.solution, locator, QStringLiteral("es.normD"), {7, 0.25}, &why) - 4 * Eps0 * 5000) < 1e-15);
    // Derived values: the energy density's integral, its average.
    Node integral = m.make(QStringLiteral("integral"));
    integral.props.insert(QStringLiteral("expression"), QStringLiteral("es.We"));
    DerivedResult d = evaluateDerived(*r.solution, integral);
    QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
    QVERIFY(std::abs(d.values.first().value - 0.5 * exact * 25) < 1e-9 * exact * 25);
    Node global = m.make(QStringLiteral("global"));
    global.props.insert(QStringLiteral("expression"), QStringLiteral("es.C11; es.Q_1/es.V_1"));
    d = evaluateDerived(*r.solution, global);
    QCOMPARE(d.values.size(), 2);
    QVERIFY(d.values[0].text.endsWith(QStringLiteral("pF")));
}

void TestFemEngine::floatingPotential()
{
    // A floating plate halfway: at half the voltage, no charge.
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "lower", "size": ["10", "1"]},
        {"type": "rectangle", "name": "upper", "position": ["0", "1"], "size": ["10", "1"]}],
        "materials": [{"label": "m", "epsilonr": "2", "selection": {"all": true}}],
        "physics": [{"type": "electrostatics", "tag": "es", "features": [
          {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["upper"], "side": "top"}},
          {"type": "ground", "selection": {"of": ["lower"], "side": "bottom"}},
          {"type": "floating", "label": "middle", "selection": {"between": ["lower", "upper"]}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    const Run r = run(m);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QVERIFY2(std::abs(global(r, QStringLiteral("es.V_middle")) - 0.5) < 1e-9, qPrintable(QString::number(global(r, QStringLiteral("es.V_middle")))));
    // In series: half the capacitance of one.
    const double one = Eps0 * 2 * 10e-3 / 1e-3;
    QVERIFY(std::abs(global(r, QStringLiteral("es.C11")) - one / 2) < 1e-9 * one);
}

void TestFemEngine::coaxConvergence()
{
    // C = 2πε0 / ln(b/a) per metre: the error falls as h², linear
    // elements; as h⁴, quadratic ones on the arcs.
    const double exact = 2 * M_PI * Eps0 / std::log(3.0);
    double previous[3] = {NAN, NAN, NAN};
    const QStringList sizes = slowBuild() ? QStringList{QStringLiteral("0.5"), QStringLiteral("0.25")}
                                          : QStringList{QStringLiteral("0.5"), QStringLiteral("0.25"), QStringLiteral("0.125")};
    for (int order = 1; order <= 2; ++order) {
        double last = NAN;
        for (const QString& h : sizes) {
            const Run r = run(modelOf(coaxModel(order, h)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double c = global(r, QStringLiteral("es.C11"));
            const double error = std::abs(c - exact) / exact;
            qInfo("coax, order %d, hmax %s mm: %d triangles, C = %.6g F/m, error %.2e", order, qPrintable(h),
                  int(r.mesh->triangles.size()), c, error);
            // Quadratic elements, curved on the arcs: h halved, the error 16
            // times smaller (with room). Linear ones: their chords' error
            // and the field's cancel in part - only bounded (the rate on a
            // straight geometry: convergenceRates).
            if (!std::isnan(last) && order == 2) {
                const double drop = last / error;
                QVERIFY2(drop > 6 || error < 1e-9, qPrintable(QStringLiteral("drop %1").arg(drop)));
            }
            QVERIFY2(error < (order == 1 ? 6e-3 : 5e-5), qPrintable(QString::number(error)));
            last = error;
        }
        previous[order] = last;
        QVERIFY2(last < (order == 1 ? 1e-3 : 5e-6), qPrintable(QString::number(last)));
    }
    Q_UNUSED(previous);
}

void TestFemEngine::convergenceRates()
{
    // -∇²T = 2π² sin(πx) sin(πy) on the unit square, T = 0 round it:
    // T = sin(πx) sin(πy), its integral 4/π². h halved: the integral's
    // error 4 times smaller (linear elements), 16 times (quadratic).
    const double exact = 4 / (M_PI * M_PI);
    const QStringList sizes = slowBuild() ? QStringList{QStringLiteral("0.1"), QStringLiteral("0.05")}
                                          : QStringList{QStringLiteral("0.1"), QStringLiteral("0.05"), QStringLiteral("0.025")};
    for (int order = 1; order <= 2; ++order) {
        double last = NAN;
        for (const QString& h : sizes) {
            const QByteArray json = QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "m",
              "geometry": [{"type": "rectangle", "name": "sq", "size": ["1", "1"]}],
              "materials": [{"label": "m", "k": "1", "selection": {"all": true}}],
              "physics": [{"type": "heat", "tag": "ht", "order": "%1", "features": [
                {"type": "source", "Q0": "2*pi^2*sin(pi*x[1/m])*sin(pi*y[1/m])[W/m^3]", "selection": {"all": true}},
                {"type": "temperature", "T0": "0[K]", "selection": {"all": true}}]}],
              "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})")
                                         .arg(order)
                                         .arg(h)
                                         .toUtf8();
            const Model m = modelOf(json);
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Node integral = m.make(QStringLiteral("integral"));
            integral.props.insert(QStringLiteral("expression"), QStringLiteral("T"));
            const DerivedResult d = evaluateDerived(*r.solution, integral);
            QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
            const double error = std::abs(d.values.first().value - exact) / exact;
            qInfo("sin sin, order %d, hmax %s: %d triangles, error %.2e", order, qPrintable(h), int(r.mesh->triangles.size()), error);
            if (!std::isnan(last)) {
                const double drop = last / error;
                QVERIFY2(drop > (order == 1 ? 3.0 : 9.0), qPrintable(QStringLiteral("drop %1").arg(drop)));
            }
            last = error;
        }
    }
}

void TestFemEngine::barResistance()
{
    // R = L / (σ w d): a bar of copper between two contacts.
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "bar", "size": ["10", "1"]}],
        "materials": [{"material": "Copper", "selection": {"all": true}}],
        "physics": [{"type": "currents", "tag": "ec", "order": "1", "features": [
          {"type": "terminal", "name": "1", "V0": "1[mV]", "selection": {"of": ["bar"], "side": "left"}},
          {"type": "ground", "selection": {"of": ["bar"], "side": "right"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    const Run r = run(m);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const double exact = 10e-3 / (5.998e7 * 1e-3 * 1.0);
    QVERIFY2(std::abs(global(r, QStringLiteral("ec.R")) - exact) < 1e-9 * exact, qPrintable(QString::number(global(r, QStringLiteral("ec.R")))));
    QVERIFY(std::abs(global(r, QStringLiteral("ec.I_1")) - 1e-3 / exact) < 1e-6 * 1e-3 / exact);
    QVERIFY(std::abs(global(r, QStringLiteral("ec.P")) - 1e-6 / exact) < 1e-6 * 1e-6 / exact);
    // Driven by a current instead: the voltage it takes.
    Model byCurrent = m;
    for (Node& f : byCurrent.find(QStringLiteral("ec"))->children)
        if (f.type == QLatin1String("currents/terminal")) {
            f.props.insert(QStringLiteral("drive"), QStringLiteral("current"));
            f.props.insert(QStringLiteral("I0"), QStringLiteral("2[A]"));
        }
    const Run c = run(byCurrent);
    QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
    QVERIFY(std::abs(global(c, QStringLiteral("ec.V_1")) - 2 * exact) < 1e-9 * 2 * exact);
}

void TestFemEngine::slabWithConvection()
{
    // 80 K across a slab of k = 1 and a film of h = 10: q = ΔT / (L/k + 1/h).
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "slab", "size": ["10", "4"]}],
        "materials": [{"label": "m", "k": "1[W/(m*K)]", "selection": {"all": true}}],
        "physics": [{"type": "heat", "tag": "ht", "order": "1", "features": [
          {"type": "temperature", "T0": "100[degC]", "selection": {"of": ["slab"], "side": "left"}},
          {"type": "flux", "kind": "convective", "h": "10[W/(m^2*K)]", "Text": "20[degC]", "selection": {"of": ["slab"], "side": "right"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    const Run r = run(m);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const double q = 80 / (0.01 / 1 + 1 / 10.0);
    const double tRight = 293.15 + q / 10;
    const Locator locator(*r.mesh);
    QString why;
    QVERIFY(std::abs(*evaluateAt(*r.solution, locator, QStringLiteral("T"), {10, 2}, &why) - tRight) < 1e-6);
    QVERIFY(std::abs(*evaluateAt(*r.solution, locator, QStringLiteral("ht.qx"), {5, 1}, &why) - q) < 1e-6);
    QVERIFY(std::abs(global(r, QStringLiteral("ht.Tmax")) - 373.15) < 1e-9);
    // In degrees Celsius, as a plot shows it.
    const SurfaceData s = surfaceData(*r.solution, QStringLiteral("T"), QStringLiteral("degC"));
    QVERIFY2(s.error.isEmpty() && s.warning.isEmpty(), qPrintable(s.error + s.warning));
    QVERIFY(std::abs(s.max - 100) < 1e-9);
    QVERIFY(std::abs(s.min - (tRight - 273.15)) < 1e-6);
}

void TestFemEngine::pipeConduction()
{
    // Between two radii: T(r) = T1 + (T2 - T1) ln(r/r1) / ln(r2/r1).
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "circle", "name": "outer", "radius": "2"}, {"type": "circle", "name": "bore", "radius": "1"}],
        "materials": [{"label": "m", "k": "10", "selection": {"all": true}}],
        "physics": [{"type": "heat", "tag": "ht", "selection": {"only": ["outer"]}, "features": [
          {"type": "temperature", "T0": "400[K]", "selection": {"of": ["bore"]}},
          {"type": "temperature", "T0": "300[K]", "selection": {"of": ["outer"]}}]}],
        "mesh": {"size": "normal"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    const Run r = run(m);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const Locator locator(*r.mesh);
    QString why;
    for (double radius : {1.2, 1.5, 1.8}) {
        const double exact = 400 - 100 * std::log(radius) / std::log(2.0);
        const double got = *evaluateAt(*r.solution, locator, QStringLiteral("T"), {radius * std::cos(0.7), radius * std::sin(0.7)}, &why);
        QVERIFY2(std::abs(got - exact) < 5e-2, qPrintable(QStringLiteral("r %1: %2, exactly %3").arg(radius).arg(got).arg(exact)));
    }
    // The heat through the outer boundary: 2πk ΔT / ln 2 per metre.
    Node out = m.make(QStringLiteral("integral"));
    out.props.insert(QStringLiteral("level"), QStringLiteral("boundary"));
    out.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("of"), QJsonArray{QStringLiteral("outer")}}});
    out.props.insert(QStringLiteral("expression"), QStringLiteral("(ht.qx*x + ht.qy*y)/sqrt(x^2+y^2)"));
    const DerivedResult d = evaluateDerived(*r.solution, out);
    QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
    const double exact = 2 * M_PI * 10 * 100 / std::log(2.0);
    QVERIFY2(std::abs(d.values.first().value - exact) < 0.01 * exact, qPrintable(QString::number(d.values.first().value)));
}

void TestFemEngine::heatSource()
{
    // Heat made evenly, both ends at 0 °C: a parabola, its top Q L² / 8k.
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "slab", "size": ["10", "2"]}],
        "materials": [{"label": "m", "k": "2", "selection": {"all": true}}],
        "physics": [{"type": "heat", "tag": "ht", "features": [
          {"type": "source", "kind": "power", "P0": "4[W]", "selection": {"all": true}},
          {"type": "temperature", "T0": "0[degC]", "selection": {"of": ["slab"], "side": "left"}},
          {"type": "temperature", "T0": "0[degC]", "selection": {"of": ["slab"], "side": "right"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    const Run r = run(m);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const double q = 4 / (10e-3 * 2e-3 * 1.0);   // W/m³
    const double top = q * 0.01 * 0.01 / (8 * 2);
    QVERIFY2(std::abs(global(r, QStringLiteral("ht.Tmax")) - 273.15 - top) < 1e-6 * top,
             qPrintable(QString::number(global(r, QStringLiteral("ht.Tmax")) - 273.15)));
    QVERIFY(std::abs(global(r, QStringLiteral("ht.P")) - 4) < 1e-9);
    QVERIFY(std::abs(global(r, QStringLiteral("ht.Rth")) - top / 4) < 1e-6 * top);
}

void TestFemEngine::jouleHeating()
{
    // A bar's currents heat it: the heat source ec.Qrh; solved in turn.
    const Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "bar", "size": ["10", "1"]}],
        "materials": [{"label": "resistive", "sigma": "1e5[S/m]", "k": "20", "selection": {"all": true}}],
        "physics": [
          {"type": "currents", "tag": "ec", "features": [
            {"type": "terminal", "name": "1", "V0": "0.1[V]", "selection": {"of": ["bar"], "side": "left"}},
            {"type": "ground", "selection": {"of": ["bar"], "side": "right"}}]},
          {"type": "heat", "tag": "ht", "features": [
            {"type": "source", "Q0": "ec.Qrh", "selection": {"all": true}},
            {"type": "temperature", "T0": "300[K]", "selection": {"of": ["bar"], "side": "left"}},
            {"type": "temperature", "T0": "300[K]", "selection": {"of": ["bar"], "side": "right"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    const Run r = run(m);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QVERIFY(std::abs(global(r, QStringLiteral("ht.P")) - global(r, QStringLiteral("ec.P"))) < 1e-6 * global(r, QStringLiteral("ec.P")));
    // The power made evenly: a parabola, its top Q L² / 8k.
    const double q = 1e5 * (0.1 / 0.01) * (0.1 / 0.01);
    QVERIFY2(std::abs(global(r, QStringLiteral("ht.Tmax")) - 300 - q * 1e-4 / 160) < 1e-4 * q * 1e-4 / 160,
             qPrintable(QString::number(global(r, QStringLiteral("ht.Tmax")))));
}

void TestFemEngine::microstripAgainstTranscalc()
{
    // A 3 mm strip, 35 µm thick, on 1.6 mm of εr 4.4: Z0 and εeff as
    // qucs-transcalc has them (no cover), within 1.5 %.
    const QByteArray json = R"({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "w", "expression": "3[mm]"}, {"name": "h", "expression": "1.6[mm]"}, {"name": "t", "expression": "35[um]"}],
      "components": [{"unit": "mm",
        "geometry": [
          {"type": "rectangle", "name": "air", "position": ["-30*h", "0"], "size": ["60*h", "30*h"]},
          {"type": "rectangle", "name": "substrate", "position": ["-30*h", "0"], "size": ["60*h", "h"]},
          {"type": "rectangle", "name": "trace", "position": ["-w/2", "h"], "size": ["w", "t"]}],
        "materials": [{"material": "Air", "selection": {"all": true}},
                      {"label": "substrate", "epsilonr": "4.4", "selection": {"only": ["substrate"]}}],
        "physics": [{"type": "electrostatics", "tag": "es", "selection": {"except": {"only": ["trace"]}, "all": true}, "features": [
          {"type": "terminal", "name": "1", "selection": {"of": ["trace"]}},
          {"type": "ground", "selection": {"of": ["substrate"], "side": "bottom"}}]}],
        "mesh": {"size": "normal"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}],
      "results": {"derived": [{"type": "lineparams", "tag": "lp1", "terminal": "1"}]}})";
    const Model m = modelOf(json);
    const Run r = run(m);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const DerivedResult d = evaluateDerived(*r.solution, *m.find(QStringLiteral("lp1")));
    QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
    double z0 = NAN, eeff = NAN;
    for (const DerivedValue& v : d.values) {
        if (v.name == QLatin1String("Z0")) z0 = v.value;
        if (v.name == QLatin1String("epsilon_eff")) eeff = v.value;
    }
    const auto [tz0, teeff] = transcalcMicrostrip(3e-3, 1.6e-3, 35e-6, 4.4);
    qInfo("microstrip: %d triangles; Z0 %.3f Ω (transcalc %.3f), εeff %.4f (transcalc %.4f)", int(r.mesh->triangles.size()), z0, tz0,
          eeff, teeff);
    QVERIFY2(std::abs(z0 - tz0) < 0.015 * tz0, qPrintable(QString::number(z0)));
    QVERIFY2(std::abs(eeff - teeff) < 0.015 * teeff, qPrintable(QString::number(eeff)));
    // L C = εeff / c²
    double l = NAN, c = NAN;
    for (const DerivedValue& v : d.values) {
        if (v.name == QLatin1String("L")) l = v.value;
        if (v.name == QLatin1String("C")) c = v.value;
    }
    QVERIFY(std::abs(l * c * LightSpeed * LightSpeed - eeff) < 1e-9 * eeff);
}

void TestFemEngine::helpfulErrors()
{
    // No ground: said, not a singular matrix.
    Model m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "r", "size": ["1", "1"]}],
        "materials": [{"material": "Air", "selection": {"all": true}}],
        "physics": [{"type": "electrostatics", "tag": "es"}], "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    Run r = run(m);
    QVERIFY(r.error.contains(QStringLiteral("not determined")));
    // A material that lacks what a physics needs: said, with the domain.
    m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "r", "size": ["1", "1"]}],
        "materials": [{"material": "Air", "selection": {"all": true}}],
        "physics": [{"type": "currents", "tag": "ec", "features": [{"type": "ground", "selection": {"all": true}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    r = run(m);
    QVERIFY2(r.error.contains(QStringLiteral("domain 1")) && r.error.contains(QStringLiteral("Air")), qPrintable(r.error));
    // No material at all.
    m = modelOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "geometry": [
        {"type": "rectangle", "name": "r", "size": ["1", "1"]}],
        "physics": [{"type": "heat", "tag": "ht", "features": [{"type": "temperature", "selection": {"all": true}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})");
    r = run(m);
    QVERIFY2(r.error.contains(QStringLiteral("no material")), qPrintable(r.error));
    // A parameter that does not evaluate.
    m = modelOf(R"({"format": "qucs-s multiphysics 1", "parameters": [{"name": "w", "expression": "3[mm"}],
        "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "r", "size": ["w", "1"]}]}]})");
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    QVERIFY(ps->errors.contains(QStringLiteral("w")));
    const GeometryBuild g = buildGeometry(m, *ps);
    QVERIFY(!g.ok());
}

void TestFemEngine::largeModelTiming()
{
    // Fast enough: a model of some hundred thousand unknowns in seconds
    // (Release; the sanitizers' build a smaller one).
    const QString h = slowBuild() ? QStringLiteral("0.2") : QStringLiteral("0.03");
    QElapsedTimer timer;
    timer.start();
    const Model m = modelOf(coaxModel(2, h));
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    const GeometryBuild g = buildGeometry(m, *ps);
    QVERIFY(g.ok());
    const qint64 geometryMs = timer.restart();
    const MeshBuild mesh = buildMesh(m, *ps, g.topology);
    QVERIFY2(mesh.mesh, qPrintable(mesh.error));
    const qint64 meshMs = timer.restart();
    const StudyResult s = solveStudy(m, ps, mesh.mesh, QString());
    QVERIFY2(s.solution, qPrintable(s.error));
    const qint64 solveMs = timer.restart();
    const SurfaceData plot = surfaceData(*s.solution, QStringLiteral("es.normE"));
    const qint64 plotMs = timer.elapsed();
    const int dofs = s.solution->fields().front().space->size();
    qInfo("coax, hmax %s mm: %d triangles, %d unknowns; geometry %lld ms, mesh %lld ms, solve %lld ms, plot %lld ms", qPrintable(h),
          int(mesh.mesh->triangles.size()), dofs, geometryMs, meshMs, solveMs, plotMs);
    if (!slowBuild()) {
        QVERIFY(dofs > 100000);
        QVERIFY2(meshMs + solveMs < 20000, "a model of 100,000 unknowns took more than 20 s");
    }
}

// Each shipped multiphysics example's selections name what its geometry
// makes, each at its own level: the gapped core inductor's refinement of its
// gap named "slot", a difference's tool - no object once the core was cut,
// its gap meshed coarse and only a warning said (the hunt of 19 October, B6).
void TestFemEngine::everyExamplesSelectionsResolve()
{
    const QDir dir(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics"));
    const QStringList files = dir.entryList({QStringLiteral("*.qfem")}, QDir::Files);
    QVERIFY(files.size() > 40);
    QStringList bad;
    int checked = 0;
    for (const QString& name : files) {
        QFile f(dir.filePath(name));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QString why;
        std::optional<Model> m = Model::fromJson(f.readAll(), &why);
        if (!m) {
            bad << QStringLiteral("%1: %2").arg(name, why);
            continue;
        }
        m->fileName = dir.filePath(name);
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(*m);
        const GeometryBuild g = buildGeometry(*m, *ps);
        if (!g.ok()) {
            bad << QStringLiteral("%1: its geometry: %2").arg(name, QStringList(g.errors.values()).join(QLatin1String("; ")));
            continue;
        }
        std::function<void(const Node&)> walk = [&walk, &m, &g, &bad, &name, &checked](const Node& n) {
            if (n.enabled && n.props.contains(QStringLiteral("selection"))) {
                Level level = n.kind() ? n.kind()->selection : Level::None;
                if (n.props.contains(QStringLiteral("level"))) level = levelOf(n.text(QStringLiteral("level")));
                if (level != Level::None) {
                    QString error;
                    (void)resolveSelection(*g.topology, *m, level, n.value(QStringLiteral("selection")).toObject(), &error);
                    if (!error.isEmpty()) bad << QStringLiteral("%1: %2: %3").arg(name, n.name(), error);
                    ++checked;
                }
            }
            for (const Node& c : n.children) walk(c);
        };
        walk(m->component());
    }
    qInfo("%d examples, %d selections", int(files.size()), checked);
    QVERIFY2(bad.isEmpty(), qPrintable(bad.join(QLatin1Char('\n'))));
}

// A mesh asked to stop while it smooths, and while it makes its edges
// Delaunay, stops: each pass asks - 349,000 nodes took 1.3 and 3 s, a model
// closed then waiting for them (the hunt of 19 October, B2).
void TestFemEngine::aMeshStopsWhileItSmooths()
{
    const Model m = modelOf(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "a", "size": ["10", "10"]}, {"type": "circle", "name": "b", "position": ["5", "5"], "radius": "2"}, {"type": "form union"}],
        "physics": [{"type": "electrostatics", "tag": "es", "features": []}],
        "mesh": {"size": "normal", "custom": true, "hmax": "0.3"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json");
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    const GeometryBuild g = buildGeometry(m, *ps);
    QVERIFY(g.ok());
    int smoothing = 0;
    const MeshBuild whole = buildMesh(m, *ps, g.topology, [&smoothing](double, const QString& w) {
        if (w == QLatin1String("Smoothing")) ++smoothing;
        return true;
    });
    QVERIFY2(whole.mesh, qPrintable(whole.error));
    qInfo("asked %d times as it smoothed", smoothing);
    QVERIFY(smoothing >= 3);
    int asked = 0;
    const MeshBuild stopped = buildMesh(m, *ps, g.topology, [&asked](double, const QString& w) { return !(w == QLatin1String("Smoothing") && ++asked == 2); });
    QVERIFY(!stopped.mesh);
    QCOMPARE(stopped.error, QStringLiteral("cancelled"));
}

// The node kinds as JSON (QUCS_KINDS_DUMP: the file): each kind's type,
// group, children and properties - key, kind, unit, default, choices,
// showIf - for the model fuzzers (docs/bug_hunts/2026-10-21-fuzz).
void TestFemEngine::dumpKinds()
{
    const QString file = qEnvironmentVariable("QUCS_KINDS_DUMP");
    if (file.isEmpty()) QSKIP("set QUCS_KINDS_DUMP to a file");
    QJsonArray kinds;
    for (const NodeKind* k : nodeKinds()) {
        QJsonArray props;
        for (const PropertyDef& p : k->properties)
            props.append(QJsonObject{{"key", p.key}, {"kind", int(p.kind)}, {"unit", p.unit}, {"default", p.defaultValue},
                                     {"choices", QJsonArray::fromStringList(p.choices)}, {"showIf", p.showIf}, {"spatial", p.spatial}});
        kinds.append(QJsonObject{{"type", k->type}, {"group", k->group}, {"children", QJsonArray::fromStringList(k->children)},
                                 {"defaults", QJsonArray::fromStringList(k->defaults)}, {"selection", int(k->selection)}, {"properties", props}});
    }
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(kinds).toJson());
}

// The expressions' fuzzer (QUCS_EXPR_FUZZ seconds, QUCS_EXPR_FUZZ_SEED):
// text made of the language's pieces and junk - numbers hostile, units
// real and not, every function with any number of arguments, the
// scope's variables, constants and model functions (one calling itself,
// two calling each other, a table) -, nested deep and chained long; each
// compiled, its unit and linearity asked, evaluated at hostile values; the
// units, constants and quantities' formatting too. The current text in
// QUCS_FUZZ_OUT/expr-current.txt, a slow one (over 2 s) beside it.
void TestFemEngine::fuzz()
{
    const int seconds = qEnvironmentVariableIntValue("QUCS_EXPR_FUZZ");
    if (seconds <= 0) QSKIP("set QUCS_EXPR_FUZZ to a number of seconds");
    QRandomGenerator rng(quint32(qEnvironmentVariableIntValue("QUCS_EXPR_FUZZ_SEED") + 1));
    const QString out = qEnvironmentVariable("QUCS_FUZZ_OUT");
    auto pick = [&rng](const QStringList& from) { return from.at(int(rng.bounded(quint32(from.size())))); };
    Scope base(&Scope::builtins());
    base.setConstant(QStringLiteral("a"), 2.5, Dim::length());
    base.setConstant(QStringLiteral("Vg"), 1, Dim::voltage());
    const int sx = base.addVariable(QStringLiteral("x"), Dim::length()), sy = base.addVariable(QStringLiteral("y"), Dim::length());
    const int sT = base.addVariable(QStringLiteral("T"), Dim::temperature()), st = base.addVariable(QStringLiteral("t"), Dim::of(0, 0, 1));
    const int sV = base.addVariable(QStringLiteral("es.V"), Dim::voltage());
    Scope::Function sq;
    sq.arguments << QStringLiteral("u");
    sq.body = QStringLiteral("u*u + 1");
    base.setFunction(QStringLiteral("sq"), sq);
    Scope::Function self;
    self.arguments << QStringLiteral("u");
    self.body = QStringLiteral("rec(u) + 1");
    base.setFunction(QStringLiteral("rec"), self);
    Scope::Function ping, pong;
    ping.arguments << QStringLiteral("u");
    ping.body = QStringLiteral("pong(u*2)");
    pong.arguments << QStringLiteral("v") << QStringLiteral("w");
    pong.body = QStringLiteral("ping(v) - w");
    base.setFunction(QStringLiteral("ping"), ping);
    base.setFunction(QStringLiteral("pong"), pong);
    Scope::Function tab;
    tab.arguments << QStringLiteral("x");
    auto table = std::make_shared<Expression::Table>();
    table->x = {0, 1, 2, 3};
    table->y = {0, 1e300, -1e300, std::nan("")};
    tab.table = table;
    base.setFunction(QStringLiteral("tab"), tab);
    Scope::Function empty;
    empty.arguments << QStringLiteral("x");
    empty.table = std::make_shared<Expression::Table>();
    base.setFunction(QStringLiteral("none"), empty);
    const QStringList numbers{"0", "1", "-1", "2.5", "1e308", "1e309", "1e-320", "4.9e-324", "1e-400", "0x10", "1e", "1e+", ".", "..", "1.2.3",
                              "10u", "3k", "2.2M", "1G", "5f", "9999999999999999999999999999", "0.0000000000000000000000001", "1_0", "NaN",
                              "inf", "1e2147483648", "1e-2147483649"};
    const QStringList units{"mm", "m", "V/m", "W/(m*K)", "1/K", "degC", "degF", "Ω", "kΩ", "µm", "um^2", "GHz", "S/m", "nosuch", "", " ",
                            "m^1e9", "m^-2147483648", "m^2147483647", "(", ")", "m/", "/m", "m^", "m^^2", "1/0", "kg*m^2/s^3/A", "K^-1",
                            "m^0.5", "cm^2/(V*s)", "1/cm^3", "eV", "T", "A/m^2"};
    const QStringList functions{"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh", "exp", "log", "log10",
                                "sqrt", "abs", "sign", "min", "max", "pow", "floor", "ceil", "round", "mod", "if", "interp", "neps",
                                "keps", "sq", "rec", "ping", "pong", "tab", "none", "nosuch", "x", "pi"};
    const QStringList names{"x", "y", "T", "t", "es.V", "a", "Vg", "pi", "eps0", "mu0", "c0", "kB", "q0", "e_const", "h_const", "N_A_const",
                            "nosuch", "es", "es.", ".V", "x.y", "T.T", "rec", "sq"};
    const QStringList operators{"+", "-", "*", "/", "^", "<", "<=", ">", ">=", "==", "!=", "&&", "||", "!", ",", "(", ")", "[", "]",
                                " ", "\t", "\n", "**", "^^", "--", "e", "E", ";", "=", "?", ":", "@", "#", "\\", "'", "\"", "µ", "Ω"};
    // A grammar's expression to depth d.
    std::function<QString(int)> grammar = [&](int d) -> QString {
        const quint32 c = rng.bounded(d <= 0 ? 3u : 9u);
        switch (c) {
        case 0: return pick(numbers) + (rng.bounded(3u) == 0 ? "[" + pick(units) + "]" : QString());
        case 1: return pick(names);
        case 2: return pick(numbers);
        case 3: return "(" + grammar(d - 1) + ")";
        case 4: return grammar(d - 1) + pick({"+", "-", "*", "/", "^", "<", "==", "&&", "||"}) + grammar(d - 1);
        case 5: return pick({"-", "!", "+"}) + grammar(d - 1);
        case 6: {
            QStringList args;
            const int n = int(rng.bounded(rng.bounded(8u) == 0 ? 40u : 5u));
            for (int k = 0; k < n; ++k) args << grammar(d - 1);
            return pick(functions) + "(" + args.join(QLatin1Char(',')) + ")";
        }
        case 7: return grammar(d - 1) + "[" + pick(units) + "]";
        default: return pick(names) + pick(operators) + grammar(d - 1);
        }
    };
    auto hostile = [&rng]() {
        static const double values[] = {0, -0.0, 1, -1, 1e308, -1e308, 1e-320, std::numeric_limits<double>::infinity(),
                                        -std::numeric_limits<double>::infinity(), std::nan(""), 293.15, 0.5};
        return rng.bounded(3u) == 0 ? rng.generateDouble() * 2e3 - 1e3 : values[rng.bounded(quint32(std::size(values)))];
    };
    auto save = [&out](const QString& name, const QString& text) {
        if (out.isEmpty()) return;
        QFile f(QDir(out).filePath(name));
        if (f.open(QIODevice::WriteOnly)) f.write(text.toUtf8());
    };
    QElapsedTimer timer;
    timer.start();
    long compiled = 0, valid = 0, evaluated = 0, slow = 0;
    while (timer.elapsed() < seconds * 1000LL) {
        QString text;
        const quint32 kind = rng.bounded(10u);
        if (kind < 4) text = grammar(int(rng.bounded(7u)));
        else if (kind < 7) {
            const int n = int(rng.bounded(60u)) + 1;
            for (int k = 0; k < n; ++k) {
                const quint32 c = rng.bounded(5u);
                text += c == 0 ? pick(numbers) : c == 1 ? pick(names) : c == 2 ? pick(functions) : c == 3 ? "[" + pick(units) + "]" : pick(operators);
            }
        } else if (kind < 9) {
            // Deep and long: nesting, unary chains, powers, sums.
            const int n = int(rng.bounded(4u)) == 0 ? int(rng.bounded(200000u)) : int(rng.bounded(3000u));
            switch (rng.bounded(7u)) {
            case 0: text = QString(n, QLatin1Char('(')) + "1" + QString(n, QLatin1Char(')')); break;
            case 1: text = QString(n, QLatin1Char('-')) + "x"; break;
            case 2: text = QString(n, QLatin1Char('!')) + "1"; break;
            case 3: {
                QStringList parts;
                for (int k = 0; k < n; ++k) parts << "x";
                text = parts.join(rng.bounded(2u) ? "^" : "+");
                break;
            }
            case 4: text = QString(QStringLiteral("sin(")).repeated(n) + "x" + QString(n, QLatin1Char(')')); break;
            case 5: text = QString(QStringLiteral("sq(")).repeated(n) + "x" + QString(n, QLatin1Char(')')); break;
            default: text = QString(n, QLatin1Char('(')); break;
            }
        } else {
            // A good one damaged.
            text = grammar(4);
            for (int k = int(rng.bounded(4u)); k >= 0 && !text.isEmpty(); --k) {
                const int at = int(rng.bounded(quint32(text.size())));
                const quint32 c = rng.bounded(3u);
                if (c == 0) text.remove(at, int(rng.bounded(5u)) + 1);
                else if (c == 1) text.insert(at, pick(operators));
                else text[at] = QChar(char16_t(rng.bounded(0x3000u)));
            }
        }
        save(QStringLiteral("expr-current.txt"), text);
        QElapsedTimer one;
        one.start();
        Expression e = Expression::compile(text, base);
        ++compiled;
        (void)e.error();
        (void)e.warnings();
        (void)e.dim();
        if (e.isValid()) {
            ++valid;
            (void)e.linearity([&rng](int) { return Expression::SlotKind(rng.bounded(4u)); });
            for (int k = 0; k < 3; ++k) {
                double values[16];
                for (double& v : values) v = hostile();
                (void)e.eval(values);
                ++evaluated;
            }
        }
        // Its pieces: a unit, a constant of a unit, the bare constants, a
        // quantity of any unit formatted.
        QString why, warning;
        (void)parseUnit(pick(units) + (rng.bounded(4u) == 0 ? pick(operators) : QString()), &why);
        Dim d = Dim::of(int(rng.bounded(9u)) - 4, int(rng.bounded(5u)) - 2, int(rng.bounded(9u)) - 4, int(rng.bounded(5u)) - 2, int(rng.bounded(3u)) - 1);
        if (rng.bounded(20u) == 0)
            for (auto& power : d.p) power = std::int8_t(int(rng.bounded(256u)) - 128);
        (void)evaluateConstant(text.left(2000), base, d, &why, &warning);
        (void)formatQuantity(hostile(), d, int(rng.bounded(20u)) - 2);
        (void)formatNumber(hostile(), int(rng.bounded(20u)) - 2);
        (void)dimName(d);
        (void)unitName(d);
        (void)bareConstants(text.left(2000));
        if (one.elapsed() > 2000) {
            ++slow;
            save(QStringLiteral("expr-slow-%1.txt").arg(slow), text);
            qWarning("slow: %lld ms for %lld characters", qint64(one.elapsed()), qint64(text.size()));
        }
    }
    (void)sx;
    (void)sy;
    (void)sT;
    (void)st;
    (void)sV;
    qInfo("expressions: %ld compiled, %ld valid, %ld evaluations, %ld slow, in %d s", compiled, valid, evaluated, slow, seconds);
}

QTEST_GUILESS_MAIN(TestFemEngine)
#include "test_fem_engine.moc"
