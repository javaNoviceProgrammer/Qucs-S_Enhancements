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

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include <cmath>

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

QTEST_GUILESS_MAIN(TestFemEngine)
#include "test_fem_engine.moc"
