/*
 * test_fem_adapt.cpp - a 2D model's mesh convergence check against what is
 *                      known: a coax's capacitance in closed form, a
 *                      re-entrant corner where cutting every element converges
 *                      slowly and refining where the error is does not (the rates
 *                      asserted), values that agree on meshes refined elsewhere
 *                      (caught), goal-oriented refinement of a value far from the
 *                      largest error, the caps and Cancel, a solve that fails
 *                      partway, bad input, old files; the mesh cut by its longest
 *                      edges (conforming, nested, its arcs on their curves); the
 *                      estimate on series of known rates. Every run is bounded by
 *                      its meshes and unknowns, none by a time.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_adapt.h"
#include "fem_optimize.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include <cmath>
#include <limits>
#include <map>
#include <set>

using namespace qucs_s::fem;

namespace {

constexpr double Eps0 = 8.8541878188e-12;

/// A sanitized or Debug build: forty times slower here, more on CI's runners.
/// Its heavy cases ask for less - the same assertions on smaller references and
/// tolerances (QtTest stops a function at 300 s).
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
    return *m;
}

/// \a json with a check of \a values and settings \a props added; its tag.
QString addCheck(Model& m, const QStringList& values, const QJsonObject& props = {})
{
    Node n = m.make(QStringLiteral("convergence"));
    QJsonArray rows;
    for (const QString& v : values) rows.append(QJsonArray{v, QString(), QString()});
    n.props.insert(QStringLiteral("values"), rows);
    for (auto it = props.begin(); it != props.end(); ++it) n.props.insert(it.key(), it.value());
    QString why;
    Node* added = m.add(m.root.tag, n, -1, &why);
    if (!added) qFatal("not added: %s", qPrintable(why));
    return added->tag;
}

ConvergenceResult check(const Model& m, const QString& tag, const Progress& progress = {})
{
    return runConvergence(m, tag, evaluateParameters(m), nullptr, nullptr, progress);
}

/// A coax, a = 1 mm, b = 3 mm, of elements of order \a order: C = 2 pi eps0 / ln 3.
QByteArray coax(int order, const QString& hmax = QStringLiteral("0.5"))
{
    return QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm",
      "geometry": [{"type": "circle", "name": "outer", "radius": "3"}, {"type": "circle", "name": "inner", "radius": "1"}],
      "materials": [{"label": "air", "epsilonr": "1", "selection": {"all": true}}],
      "physics": [{"type": "electrostatics", "tag": "es", "order": "%1", "selection": {"only": ["outer"]}, "features": [
        {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["inner"]}},
        {"type": "ground", "selection": {"of": ["outer"]}}]}],
      "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}]})")
        .arg(order)
        .arg(hmax)
        .toUtf8();
}

/// An L-shaped gap between electrodes: the field infinite at the re-entrant
/// corner, its energy's error ~ h^(4/3) on a uniform mesh (unknowns^-2/3).
QByteArray corner(const QString& hmax = QStringLiteral("0.25"))
{
    return QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm",
      "geometry": [{"type": "rectangle", "name": "square", "size": ["2", "2"]},
                   {"type": "rectangle", "name": "cut", "position": ["1", "1"], "size": ["1", "1"]},
                   {"type": "difference", "name": "plate", "input": ["square"], "tools": ["cut"]}],
      "materials": [{"label": "m", "epsilonr": "1", "selection": {"all": true}}],
      "physics": [{"type": "electrostatics", "tag": "es", "order": "1", "features": [
        {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"box": [0.99, 0.99, 2.01, 2.01]}},
        {"type": "ground", "selection": {"box": [-0.01, -0.01, 0.01, 2.01]}},
        {"type": "ground", "selection": {"box": [-0.01, -0.01, 2.01, 0.01]}}]}],
      "mesh": {"size": "normal", "custom": true, "hmax": "%1"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}]})")
        .arg(hmax)
        .toUtf8();
}

/// Two parts side by side: an L-shaped electrode's gap on the left (electrostatics
/// there alone, its corner singular) and a heated bar on the right (heat there
/// alone, a source in a small square of it). The bar's coarse mesh holds its
/// Tmax off by a few per cent.
QByteArray twoParts(const QString& extra = {})
{
    return QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm",
      "geometry": [{"type": "rectangle", "name": "left", "size": ["2", "2"]},
                   {"type": "rectangle", "name": "cut", "position": ["1", "1"], "size": ["1", "1"]},
                   {"type": "difference", "name": "gap", "input": ["left"], "tools": ["cut"]},
                   {"type": "rectangle", "name": "bar", "position": ["3", "0"], "size": ["4", "2"]},
                   {"type": "rectangle", "name": "spot", "position": ["4.6", "0.8"], "size": ["0.4", "0.4"]},
                   {"type": "form union"}],
      "materials": [{"label": "m", "epsilonr": "1", "k": "1[W/(m*K)]", "selection": {"all": true}}],
      "physics": [{"type": "electrostatics", "tag": "es", "order": "1", "selection": {"objects": ["gap"]}, "features": [
                     {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"box": [0.99, 0.99, 2.01, 2.01]}},
                     {"type": "ground", "selection": {"box": [-0.01, -0.01, 0.01, 2.01]}},
                     {"type": "ground", "selection": {"box": [-0.01, -0.01, 2.01, 0.01]}}]},
                  {"type": "heat", "tag": "ht", "order": "1", "selection": {"objects": ["bar"]}, "features": [
                     {"type": "source", "Q0": "1e9[W/m^3]", "selection": {"box": [4.59, 0.79, 5.01, 1.21]}},
                     {"type": "temperature", "T0": "300[K]", "selection": {"box": [6.99, -0.01, 7.01, 2.01]}}]}],
      "mesh": {"size": "normal", "custom": true, "hmax": "0.5"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}]%1})")
        .arg(extra)
        .toUtf8();
}

/// Heat in two plates apart: an L-shaped one, hot along its re-entrant
/// corner's sides (its gradient infinite there) and cold at its left; a bar heated
/// in a small square, cold at its right end, a sensor's average temperature in it
/// watched (a Derived Values node). Nothing of the L reaches the sensor.
QByteArray twoPlates()
{
    return QByteArray(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm",
      "geometry": [{"type": "rectangle", "name": "square", "size": ["2", "2"]},
                   {"type": "rectangle", "name": "cut", "position": ["1", "1"], "size": ["1", "1"]},
                   {"type": "difference", "name": "plate", "input": ["square"], "tools": ["cut"]},
                   {"type": "rectangle", "name": "bar", "position": ["3", "0"], "size": ["4", "2"]},
                   {"type": "rectangle", "name": "spot", "position": ["3.6", "0.8"], "size": ["0.4", "0.4"]},
                   {"type": "rectangle", "name": "sensor", "position": ["5.6", "0.8"], "size": ["0.4", "0.4"]},
                   {"type": "form union"}],
      "materials": [{"label": "m", "k": "1[W/(m*K)]", "selection": {"all": true}}],
      "physics": [{"type": "heat", "tag": "ht", "order": "1", "features": [
        {"type": "temperature", "T0": "400[K]", "selection": {"box": [0.99, 0.99, 2.01, 1.01]}},
        {"type": "temperature", "T0": "400[K]", "selection": {"box": [0.99, 0.99, 1.01, 2.01]}},
        {"type": "temperature", "T0": "300[K]", "selection": {"box": [-0.01, -0.01, 0.01, 2.01]}},
        {"type": "source", "Q0": "1e8[W/m^3]", "selection": {"box": [3.59, 0.79, 4.01, 1.21]}},
        {"type": "temperature", "T0": "300[K]", "selection": {"box": [6.99, -0.01, 7.01, 2.01]}}]}],
      "mesh": {"size": "normal", "custom": true, "hmax": "0.5"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}],
      "results": {"derived": [{"type": "average", "tag": "sensor", "expression": "T", "level": "domain", "selection": {"box": [5.59, 0.79, 6.01, 1.21]}}]}})");
}

/// The review's L: [-1, 1]^2 without its fourth quadrant, heat of k = 1 on
/// quadratic elements, T = 300 K + r^(2/3) sin(2 theta / 3) on its whole boundary
/// (theta from the re-entrant corner's first side); the integral of |q|^2 watched,
/// 1.8362267 exactly (by quadrature in polar form).
QByteArray reviewsCorner()
{
    return QByteArray(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "m",
      "geometry": [{"type": "polygon", "name": "L", "points": [["0", "0"], ["1", "0"], ["1", "1"], ["-1", "1"], ["-1", "-1"], ["0", "-1"]]}],
      "materials": [{"label": "m", "k": "1[W/(m*K)]", "selection": {"all": true}}],
      "physics": [{"type": "heat", "tag": "ht", "order": "2", "features": [
        {"type": "temperature", "T0": "300[K] + 1[K]*(sqrt(x^2+y^2)/1[m])^(2/3)*sin(2/3*(atan2((-y-x)/1[m], (y-x)/1[m]) + 3*pi/4))", "selection": {"all": true}}]}],
      "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}],
      "results": {"derived": [{"type": "integral", "tag": "en", "expression": "(ht.normq/1[W/m^2])^2", "level": "domain", "selection": {"all": true}}]}})json");
}
constexpr double kReviewsCorner = 1.8362267;

double total(const ConvergenceResult& r, int point = 0)
{
    double n = 0;
    for (const ConvergenceRow& row : r.rows)
        if (row.point == point) n += row.unknowns;
    return n;
}

const ConvergenceResult::Final* finalOf(const ConvergenceResult& r, int column = 0, int point = 0)
{
    for (const ConvergenceResult::Final& f : r.values)
        if (f.column == column && f.point == point) return &f;
    return nullptr;
}

/// The value's rate of error against \a exact over the rows of \a kind: the
/// slope of log error on log unknowns, by least squares.
double rateAgainst(const ConvergenceResult& r, double exact, const QString& kind)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (const ConvergenceRow& row : r.rows) {
        if (row.kind != kind && !(kind == QLatin1String("adaptive") && row.kind == QLatin1String("start"))
            && !(kind == QLatin1String("uniform") && row.kind == QLatin1String("start")))
            continue;
        const double x = std::log(row.unknowns), y = std::log(std::abs(row.values[0] - exact));
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++n;
    }
    return n >= 2 ? -(n * sxy - sx * sy) / (n * sxx - sx * sx) : NAN;
}

} // namespace

class TestFemAdapt : public QObject
{
    Q_OBJECT
private slots:
    void bisectionKeepsTheMeshWhole();
    void bisectionPutsArcNodesOnTheirArcs();
    void anUnmarkedMeshIsTheSame();
    void estimateOfKnownRates();
    void estimateOfErraticAndUnmovedValues();
    void estimateOfAlternatingChanges();
    void coaxAgainstItsClosedForm();
    void reentrantCornerRates();
    void aCornerIsGradedAsItShouldBe();
    void aRateFasterThanTheOrderIsNotClaimed();
    void theErrorSaidIsNotBelowTheErrorMade();
    void falseAgreementIsCaught();
    void falseAgreementWithoutTheCheckIsSaid();
    void goalOrientedRefinesForTheValue();
    void goalWithoutAnAdjointFallsBack();
    void edgeElementsHaveAnIndicator();
    void capsStopIt();
    void cancelKeepsWhatItHas();
    void aSolveFailingPartway();
    void aValueTheResultsLack();
    void badSettings();
    void aSweepsEndsEachChecked();
    void theModelIsNotTouched();
    void theRefinedMeshKept();
    void resultInTheFile();
    void aDamagedResultIsReadAsFarAsItGoes();
    void oldFilesLoadUnchanged();
    void adaptiveStepsStillWork();
};

void TestFemAdapt::bisectionKeepsTheMeshWhole()
{
    // Cut three times where an indicator says: every edge inside two triangles
    // and on the boundary one, no node in the middle of another's edge, the
    // area kept, each coarse node where it was.
    Model m = modelOf(corner());
    const auto ps = evaluateParameters(m);
    const GeometryBuild g = buildGeometry(m, *ps);
    QVERIFY(g.ok());
    MeshBuild b = buildMesh(m, *ps, g.topology);
    QVERIFY(b.mesh);
    std::shared_ptr<const Mesh> mesh = b.mesh;
    double area0 = 0;
    for (std::size_t t = 0; t < mesh->triangles.size(); ++t) area0 += mesh->bounds().width() * 0 + 0;
    const auto areaOf = [](const Mesh& mm) {
        double a = 0;
        for (const auto& t : mm.triangles) {
            const QPointF p = mm.nodes[std::size_t(t[0])], q = mm.nodes[std::size_t(t[1])], r = mm.nodes[std::size_t(t[2])];
            const double s = ((q.x() - p.x()) * (r.y() - p.y()) - (r.x() - p.x()) * (q.y() - p.y())) / 2;
            if (!(s > 0)) return -1.0;
            a += s;
        }
        return a;
    };
    area0 = areaOf(*mesh);
    QVERIFY(std::abs(area0 - 3e0) < 1e-9);
    for (int round = 0; round < 3; ++round) {
        std::vector<char> marked(mesh->triangles.size(), 0);
        // The triangles nearest the corner (1, 1).
        for (std::size_t t = 0; t < marked.size(); ++t) {
            const auto& v = mesh->triangles[t];
            const QPointF c = (mesh->nodes[std::size_t(v[0])] + mesh->nodes[std::size_t(v[1])] + mesh->nodes[std::size_t(v[2])]) / 3.0;
            marked[t] = std::hypot(c.x() - 1, c.y() - 1) < 0.3 ? 1 : 0;
        }
        const std::shared_ptr<Mesh> finer = bisectMesh(*mesh, marked);
        QVERIFY(finer);
        QVERIFY(finer->triangles.size() > mesh->triangles.size());
        for (std::size_t n = 0; n < mesh->nodes.size(); ++n) QCOMPARE(finer->nodes[n], mesh->nodes[n]);
        QVERIFY(std::abs(areaOf(*finer) - area0) < 1e-9);
        std::map<std::pair<int, int>, int> uses;
        for (const auto& t : finer->triangles)
            for (int k = 0; k < 3; ++k) {
                const int a = t[std::size_t(k)], c = t[std::size_t((k + 1) % 3)];
                ++uses[{std::min(a, c), std::max(a, c)}];
            }
        std::set<std::pair<int, int>> boundary;
        for (const Mesh::Edge& e : finer->edges) {
            boundary.insert({std::min(e.a, e.b), std::max(e.a, e.b)});
            QVERIFY(e.left >= 0 || e.right >= 0);
        }
        int outside = 0;
        for (const auto& [edge, count] : uses) {
            QVERIFY(count == 1 || count == 2);
            if (count == 1) {
                ++outside;
                QVERIFY2(boundary.count(edge), "an edge of one triangle is not a boundary edge: a hanging node");
            }
        }
        QVERIFY(outside > 0);
        QVERIFY2(finer->minAngle > 0.5 * mesh->minAngle - 1e-9, qPrintable(QStringLiteral("%1 %2").arg(finer->minAngle).arg(mesh->minAngle)));
        QCOMPARE(finer->domain.size(), finer->triangles.size());
        mesh = finer;
    }
    // (A wrong size: refused.)
    QVERIFY(!bisectMesh(*mesh, std::vector<char>(3, 1)));
}

void TestFemAdapt::bisectionPutsArcNodesOnTheirArcs()
{
    Model m = modelOf(coax(1));
    const auto ps = evaluateParameters(m);
    const GeometryBuild g = buildGeometry(m, *ps);
    MeshBuild b = buildMesh(m, *ps, g.topology);
    QVERIFY(b.mesh);
    const std::shared_ptr<Mesh> finer = bisectMesh(*b.mesh, std::vector<char>(b.mesh->triangles.size(), 1));
    QVERIFY(finer);
    // Every node on a boundary edge of the inner or outer circle at its radius.
    int checked = 0;
    for (const Mesh::Edge& e : finer->edges)
        for (int n : {e.a, e.b}) {
            const double r = std::hypot(finer->nodes[std::size_t(n)].x(), finer->nodes[std::size_t(n)].y());
            if (std::abs(r - 1) < 0.05 || std::abs(r - 3) < 0.05) {
                QVERIFY2(std::abs(r - 1) < 1e-9 || std::abs(r - 3) < 1e-9, qPrintable(QString::number(r, 'g', 15)));
                ++checked;
            }
        }
    QVERIFY(checked > 100);
    QVERIFY(finer->triangles.size() >= 2 * b.mesh->triangles.size());
}

void TestFemAdapt::anUnmarkedMeshIsTheSame()
{
    Model m = modelOf(coax(2));
    const auto ps = evaluateParameters(m);
    const GeometryBuild g = buildGeometry(m, *ps);
    MeshBuild b = buildMesh(m, *ps, g.topology);
    const std::shared_ptr<Mesh> same = bisectMesh(*b.mesh, std::vector<char>(b.mesh->triangles.size(), 0));
    QVERIFY(same);
    QCOMPARE(same->triangles, b.mesh->triangles);
    QCOMPARE(same->edges.size(), b.mesh->edges.size());
    for (std::size_t e = 0; e < same->edges.size(); ++e) {
        QCOMPARE(same->edges[e].left, b.mesh->edges[e].left);
        QCOMPARE(same->edges[e].right, b.mesh->edges[e].right);
        QCOMPARE(same->edges[e].boundary, b.mesh->edges[e].boundary);
    }
}

void TestFemAdapt::estimateOfKnownRates()
{
    // Q = 2 + 3 N^-q on meshes growing by 1.6: the limit and the rate found.
    for (double q : {0.5, 1.0, 2.0}) {
        std::vector<double> ns, qs;
        for (double n = 100; n < 1e5; n *= 1.6) {
            ns.push_back(n);
            qs.push_back(2 + 3 * std::pow(n, -q));
        }
        const ValueEstimate e = estimateValue(ns, qs, 2);
        QCOMPARE(e.trust, ValueEstimate::Extrapolated);
        QVERIFY(e.steady);
        QVERIFY2(std::abs(e.rate - q) < 1e-6, qPrintable(QString::number(e.rate)));
        QVERIFY(std::abs(e.limit - 2) < 1e-9);
        // The error said is the finest value's, with room, and not below it.
        const double actual = std::abs(qs.back() - 2);
        QVERIFY(e.error >= actual && e.error < 1.3 * actual);
    }
    // Meshes hardly apart: the finer kept, the one before skipped.
    const ValueEstimate close = estimateValue({100, 105, 110}, {1.0, 1.01, 1.02}, 1);
    QCOMPARE(close.trust, ValueEstimate::Few);
    // Two meshes: a bound with room, never extrapolated.
    const ValueEstimate two = estimateValue({100, 200}, {1.1, 1.05}, 1);
    QCOMPARE(two.trust, ValueEstimate::Bounded);
    QVERIFY(two.error > 0.05);
    // Faster than the elements can converge: two agreed by chance.
    std::vector<double> ns{100, 200, 400}, qs{1.0 + 1e-2, 1.0 + 1e-6, 1.0 + 1e-10};
    const ValueEstimate fast = estimateValue(ns, qs, 1);
    QCOMPARE(fast.trust, ValueEstimate::Bounded);
    QVERIFY(fast.why.contains(QStringLiteral("chance")));
    // A change that does not slow down: no rate.
    const ValueEstimate steady = estimateValue({100, 200, 400}, {1.0, 1.1, 1.2}, 1);
    QCOMPARE(steady.trust, ValueEstimate::Erratic);
}

void TestFemAdapt::estimateOfErraticAndUnmovedValues()
{
    const ValueEstimate jumps = estimateValue({100, 200, 400}, {1.0, 1.1, 1.0}, 1);
    QCOMPARE(jumps.trust, ValueEstimate::Erratic);
    QVERIFY(jumps.error >= 0.3 - 1e-12);
    const ValueEstimate settling = estimateValue({100, 200, 400}, {1.0, 1.1, 1.08}, 1);
    QCOMPARE(settling.trust, ValueEstimate::Bounded);
    const ValueEstimate still = estimateValue({100, 200, 400}, {5.0, 5.0, 5.0}, 1);
    QCOMPARE(still.trust, ValueEstimate::Unmoved);
    QCOMPARE(still.error, 0.0);
    // NaN left out; none at all: few.
    QCOMPARE(estimateValue({100, 200}, {NAN, NAN}, 1).trust, ValueEstimate::Few);
    for (auto t : {ValueEstimate::Few, ValueEstimate::Unmoved, ValueEstimate::Extrapolated, ValueEstimate::Bounded, ValueEstimate::Erratic})
        QCOMPARE(ValueEstimate::trustOf(ValueEstimate::trustWord(t)), std::optional<ValueEstimate::Trust>(t));
}

void TestFemAdapt::estimateOfAlternatingChanges()
{
    // Every element cut by longest-edge bisection, one way and then the other: the
    // changes alternate with the cut, and one pair of them reads a rate the next
    // does not. The error said is not below the error left.
    // The L corner's uniform meshes on macOS (8 November), against its limit
    // 1.1326822727e-11 from adaptive runs at 1e-5 to 1e-6: on every three or more.
    const std::vector<double> ns{106, 232, 489, 1041, 2171, 4513, 9282, 19026, 38743, 78787, 159645, 323519, 655235, 1327233};
    const std::vector<double> qs{1.1466588801e-11, 1.14276768465e-11, 1.13871229834e-11, 1.1366494225e-11, 1.13502921358e-11, 1.13424843571e-11,
                                 1.13361310253e-11, 1.13330378002e-11, 1.13305275587e-11, 1.13292928149e-11, 1.13282973282e-11, 1.13278040854e-11,
                                 1.13274090295e-11, 1.13272122667e-11};
    const double limit = 1.1326822727e-11;
    for (std::size_t k = 3; k <= ns.size(); ++k) {
        const ValueEstimate e = estimateValue({ns.begin(), ns.begin() + long(k)}, {qs.begin(), qs.begin() + long(k)}, 1);
        QVERIFY2(e.error >= std::abs(qs[k - 1] - limit), qPrintable(QStringLiteral("%1 meshes: said %2, left %3 (%4)").arg(k).arg(e.error).arg(std::abs(qs[k - 1] - limit)).arg(e.why)));
    }
    // CI's Linux meshes (docs/bug_hunts/2026-11-08-linux-debug-ci.md, N3): the changes
    // fell by 0.39, 0.98 and 0.35 a step, the last pair read as unknowns^-1.4, faster
    // than the order, where the error fell as ^-0.62 and 2.4 of the last change were
    // left. Bounded at the order from the last change alone, it said 1.84.
    const std::vector<double> linux{232, 493, 1041, 2200, 4620};
    const double d3 = 0.39 * 0.98 * 0.35;
    const std::vector<double> moving{1 + 0.39 + 0.39 * 0.98 + d3 + 2.4 * d3, 0.39 + 0.39 * 0.98 + d3 + 2.4 * d3, 0.39 * 0.98 + d3 + 2.4 * d3, d3 + 2.4 * d3, 2.4 * d3};
    const ValueEstimate jittered = estimateValue(linux, moving, 1);
    QVERIFY2(jittered.error >= 2.4 * d3, qPrintable(QStringLiteral("said %1 changes, left 2.4: %2").arg(jittered.error / d3).arg(jittered.why)));
    // A smooth sequence is not said worse for it: unknowns^-1, steady, as before.
    std::vector<double> smooth;
    for (double n : {100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0}) smooth.push_back(1 + 1 / n);
    const ValueEstimate s = estimateValue({100, 200, 400, 800, 1600, 3200}, smooth, 1);
    QCOMPARE(s.trust, ValueEstimate::Extrapolated);
    QVERIFY(s.steady);
    QVERIFY2(std::abs(s.error - 1.25 / 3200) < 1e-9, qPrintable(QString::number(s.error * 3200)));
}

void TestFemAdapt::coaxAgainstItsClosedForm()
{
    const double exact = 2 * M_PI * Eps0 / std::log(3.0);
    // (A first-order coax's arcs are chords: its domain moves as nodes go onto
    // them, a few parts in 1e5 each time - asked for 1e-3 alone.)
    for (int order : {1, 2}) {
        for (double tolerance : order == 1 || slowBuild() ? std::vector<double>{1e-3} : std::vector<double>{1e-3, 1e-5}) {
            Model m = modelOf(coax(order));
            const QString tag = addCheck(m, {QStringLiteral("es.C11")}, {{QStringLiteral("tolerance"), QString::number(tolerance)}});
            const ConvergenceResult r = check(m, tag);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QVERIFY2(r.converged, qPrintable(r.summary + QStringLiteral(" | ") + r.notes.join(QStringLiteral(" | "))));
            const ConvergenceResult::Final* f = finalOf(r);
            QVERIFY(f && f->settled);
            const double actual = std::abs(f->value - exact) / exact;
            qInfo("coax, order %d, tolerance %g: %d solves, %.0f unknowns at most, error %.2e (said %.2e), %s", order, tolerance, r.solves, r.largest,
                  actual, f->relative, qPrintable(f->trust));
            // Within the tolerance, and the error said not below the error made
            // (with a little room: the arcs' chords are the model's, not the circle's).
            QVERIFY2(actual <= tolerance, qPrintable(QString::number(actual)));
            QVERIFY2(f->relative >= 0.5 * actual, qPrintable(QStringLiteral("said %1, made %2").arg(f->relative).arg(actual)));
            // (The check last, or one more cut of every element after a check that
            // left an error just above the tolerance.)
            bool checked = false;
            for (const ConvergenceRow& row : r.rows) checked = checked || row.kind == QLatin1String("check");
            QVERIFY(checked);
            QVERIFY(r.rows.back().kind == QLatin1String("check") || r.rows.back().kind == QLatin1String("uniform"));
            QVERIFY(r.evidence.join(QString()).contains(QStringLiteral("check")));
            QVERIFY(r.method.contains(QStringLiteral("Zienkiewicz")));
        }
    }
}

void TestFemAdapt::reentrantCornerRates()
{
    // The energy of the L's gap: the reference from a fine adaptive run, then the
    // rate of each way against it. Cut everywhere, unknowns^-2/3 (h^4/3 of the
    // corner's r^2/3); where the error is, unknowns^-1, as a smooth field's.
    // (A slow build: 1e-3 against a reference to 3e-5, the uniform meshes to 300,000
    // unknowns - the rates the same, the uniform route's cost asserted more loosely.)
    const QString tolerance = slowBuild() ? QStringLiteral("1e-3") : QStringLiteral("1e-4");
    Model ref = modelOf(corner());
    const QString rt = addCheck(ref, {QStringLiteral("es.W")}, {{QStringLiteral("tolerance"), slowBuild() ? QStringLiteral("3e-5") : QStringLiteral("1e-5")}, {QStringLiteral("maxmeshes"), 40}});
    const ConvergenceResult reference = check(ref, rt);
    QVERIFY2(reference.converged, qPrintable(reference.summary));
    const double exact = finalOf(reference)->limit;
    Model a = modelOf(corner());
    const QString at = addCheck(a, {QStringLiteral("es.W")}, {{QStringLiteral("tolerance"), tolerance}, {QStringLiteral("maxmeshes"), 30}});
    const ConvergenceResult adaptive = check(a, at);
    QVERIFY2(adaptive.converged, qPrintable(adaptive.summary));
    Model u = modelOf(corner());
    const QString ut = addCheck(u, {QStringLiteral("es.W")}, {{QStringLiteral("tolerance"), tolerance}, {QStringLiteral("refine"), QStringLiteral("uniform")},
                                                         {QStringLiteral("maxunknowns"), slowBuild() ? 3e5 : 2e6}, {QStringLiteral("maxmeshes"), 30}});
    const ConvergenceResult uniform = check(u, ut);
    const double qa = rateAgainst(adaptive, exact, QStringLiteral("adaptive"));
    const double qu = rateAgainst(uniform, exact, QStringLiteral("uniform"));
    const double ea = std::abs(finalOf(adaptive)->value - exact) / exact, eu = std::abs(finalOf(uniform)->value - exact) / exact;
    qInfo("corner: adaptive %d solves, %.0f unknowns in all, rate %.3f, error %.2e; uniform %d solves, %.0f in all, rate %.3f, error %.2e (%s)", adaptive.solves,
          adaptive.unknownsSolved, qa, ea, uniform.solves, uniform.unknownsSolved, qu, eu, qPrintable(uniform.status));
    QVERIFY2(qa > 0.85, qPrintable(QString::number(qa)));
    QVERIFY2(qu > 0.55 && qu < 0.8, qPrintable(QString::number(qu)));
    QVERIFY(ea <= tolerance.toDouble());
    // The same accuracy, by cutting every element: many more unknowns - the
    // first mesh of each within the tolerance of the reference. (Not the
    // unknowns solved in all: those count the meshes each took to be sure, and
    // the adaptive one's pre-asymptotic jitter cost it as many as uniform's on
    // the meshes of 8 November - 36,758 against 37,785 - though uniform's mesh
    // within 1e-3 was nine times its.)
    const auto firstWithin = [&](const ConvergenceResult& r) {
        for (const ConvergenceRow& row : r.rows)
            if (!row.values.empty() && std::abs(row.values[0] - exact) <= tolerance.toDouble() * std::abs(exact)) return row.unknowns;
        return std::numeric_limits<double>::infinity();
    };
    const double na = firstWithin(adaptive), nu = firstWithin(uniform);
    qInfo("corner: the first mesh within %s: adaptive %.0f unknowns, uniform %.0f", qPrintable(tolerance), na, nu);
    QVERIFY(std::isfinite(na));
    QVERIFY2(nu > (slowBuild() ? 1.5 : 10.0) * na || !uniform.converged, qPrintable(QStringLiteral("%1 against %2: %3").arg(nu).arg(na).arg(uniform.summary)));
    // The adaptive one's estimate held: the said error not below the error made.
    QVERIFY(finalOf(adaptive)->error >= std::abs(finalOf(adaptive)->value - exact));
    // And the uniform one's verdict, where it says it converged: within the
    // tolerance. (With the first mesh within the tolerance compared below, a
    // uniform run that claimed the tolerance without reaching it would otherwise
    // pass unseen: after f3c591c a slow build's said 9.3e-4 and made 1.23e-3 at
    // 1e-3, its changes' jitter read as a rate faster than the order and
    // bounded at the order. docs/bug_hunts/2026-11-08-linux-debug-ci.md, N3.)
    if (uniform.converged)
        QVERIFY2(eu <= tolerance.toDouble(), qPrintable(QStringLiteral("said %1, made %2: %3").arg(finalOf(uniform)->error / std::abs(exact)).arg(eu).arg(uniform.summary)));
    // And its smallest elements at the corner.
    QVERIFY(adaptive.mesh);
    double smallest = 1e300;
    QPointF where;
    for (std::size_t t = 0; t < adaptive.mesh->triangles.size(); ++t) {
        const auto& v = adaptive.mesh->triangles[t];
        const QPointF p = adaptive.mesh->nodes[std::size_t(v[0])], q = adaptive.mesh->nodes[std::size_t(v[1])], s = adaptive.mesh->nodes[std::size_t(v[2])];
        const double area = std::abs((q.x() - p.x()) * (s.y() - p.y()) - (s.x() - p.x()) * (q.y() - p.y()));
        if (area < smallest) {
            smallest = area;
            where = (p + q + s) / 3.0;
        }
    }
    QVERIFY2(std::hypot(where.x() - 1, where.y() - 1) < 0.05, qPrintable(QStringLiteral("%1 %2").arg(where.x()).arg(where.y())));
}

void TestFemAdapt::aCornerIsGradedAsItShouldBe()
{
    // The review of 063142f, N1: on its L the check's errors fell as unknowns^-1.2
    // where quadratic elements graded into the corner fall as ^-2 - each round
    // marked at least 35 % of the triangles, most of them far from the corner.
    // Now at least 10 %: the rate against the exact value, and the unknowns for
    // 1e-5.
    Model m = modelOf(reviewsCorner());
    const QString tag = addCheck(m, {QStringLiteral("en")}, {{QStringLiteral("tolerance"), QStringLiteral("1e-5")}, {QStringLiteral("maxmeshes"), 40}});
    const ConvergenceResult r = check(m, tag);
    QVERIFY2(r.converged, qPrintable(r.summary));
    const double q = rateAgainst(r, kReviewsCorner, QStringLiteral("adaptive"));
    const double made = std::abs(finalOf(r)->value - kReviewsCorner) / kReviewsCorner;
    qInfo("the review's corner: %d solves, %.0f unknowns in all, largest %.0f; rate %.2f; error made %.2e, said %.2e (%s)", r.solves, r.unknownsSolved, r.largest, q, made,
          finalOf(r)->relative, qPrintable(finalOf(r)->trust));
    QVERIFY2(q > 1.6, qPrintable(QString::number(q)));
    QVERIFY(made <= 1e-5);
    QVERIFY(finalOf(r)->relative >= made);
    // (At 35 % it took some 33,000 unknowns at its last mesh for 1e-5.)
    QVERIFY2(r.largest < 15000, qPrintable(QString::number(r.largest)));
}

void TestFemAdapt::aRateFasterThanTheOrderIsNotClaimed()
{
    // Changes falling faster than elements of the order can converge are not a
    // rate: the corner's read unknowns^-2.2 on quadratic elements where its error
    // fell as ^-1.2, and said "extrapolated" (the review of 063142f, N1).
    std::vector<double> ns, qs;
    for (double n = 100; n < 1e4; n *= 1.6) {
        ns.push_back(n);
        qs.push_back(2 + 300 * std::pow(n, -2.5));
    }
    const ValueEstimate fast = estimateValue(ns, qs, 2);
    QCOMPARE(fast.trust, ValueEstimate::Bounded);
    QVERIFY(std::isnan(fast.rate));
    QVERIFY2(fast.why.contains(QStringLiteral("2.5")), qPrintable(fast.why));
    QVERIFY(fast.error >= std::abs(qs.back() - 2));
    // At the order, extrapolated as before.
    QCOMPARE(estimateValue(ns, std::vector<double>(qs.size()), 2).trust, ValueEstimate::Unmoved);
}

void TestFemAdapt::theErrorSaidIsNotBelowTheErrorMade()
{
    // Shipped models whose corners make the rounds' rate faster than the check's:
    // each value's error said against the error made (a reference thirty times
    // tighter), and within the tolerance. Read at the rounds' rate alone, the TSV's
    // was said 4.4 times too small.
    const struct {
        const char* file;
        const char* value;
        double tolerance;
    } cases[] = {{"tsv_axisymmetric.qfem", "es.C11", 1e-3}, {"resistor_heating.qfem", "ec.R", 1e-3}, {"microstrip.qfem", "es.C11", 1e-3}};
    // (A slow build: the resistor alone, against a reference to a tenth.)
    const double tighter = slowBuild() ? 10 : 30;
    for (const auto& c : cases) {
        if (slowBuild() && QLatin1String(c.file) != QLatin1String("resistor_heating.qfem")) continue;
        QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/") + QLatin1String(c.file));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray json = f.readAll();
        Model ref = modelOf(json);
        const QString rt = addCheck(ref, {QLatin1String(c.value)}, {{QStringLiteral("tolerance"), QString::number(c.tolerance / tighter)}, {QStringLiteral("maxmeshes"), 40},
                                                                {QStringLiteral("maxunknowns"), QStringLiteral("4e6")}});
        const ConvergenceResult reference = check(ref, rt);
        QVERIFY2(reference.converged, qPrintable(QLatin1String(c.file) + QStringLiteral(": ") + reference.summary));
        Model m = modelOf(json);
        const QString tag = addCheck(m, {QLatin1String(c.value)}, {{QStringLiteral("tolerance"), QString::number(c.tolerance)}});
        const ConvergenceResult r = check(m, tag);
        QVERIFY2(r.converged, qPrintable(r.summary));
        const double exact = finalOf(reference)->limit;
        const double made = std::abs(finalOf(r)->value - exact) / std::abs(exact);
        qInfo("%s %s: error made %.2e, said %.2e (%s), tolerance %g", c.file, c.value, made, finalOf(r)->relative, qPrintable(finalOf(r)->trust), c.tolerance);
        QVERIFY(made <= c.tolerance);
        // (The reference has an error of its own, a thirtieth of the tolerance at
        // most: the error made is known to within it.)
        const double known = finalOf(reference)->relative;
        QVERIFY2(finalOf(r)->relative >= made - known,
                 qPrintable(QStringLiteral("%1: said %2, made %3 (the reference within %4)").arg(QLatin1String(c.file)).arg(finalOf(r)->relative).arg(made).arg(known)));
    }
}

void TestFemAdapt::falseAgreementIsCaught()
{
    // Refined by the electrostatics alone, the bar on the right is hardly
    // touched: its Tmax agrees from mesh to mesh. The check, every element cut,
    // moves it.
    Model m = modelOf(twoParts());
    const QString tag = addCheck(m, {QStringLiteral("ht.Tmax")}, {{QStringLiteral("tolerance"), QStringLiteral("1e-3")}, {QStringLiteral("physics"), QJsonArray{QStringLiteral("es")}}});
    const ConvergenceResult r = check(m, tag);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    qInfo("%s", qPrintable(r.summary));
    for (const QString& e : r.evidence) qInfo("  %s", qPrintable(e));
    QVERIFY(r.falseConvergence);
    QVERIFY(r.evidence.join(QString()).contains(QStringLiteral("agreed on meshes refined elsewhere")));
    bool sawCheck = false, sawUniform = false;
    for (const ConvergenceRow& row : r.rows) {
        sawCheck = sawCheck || row.kind == QLatin1String("check");
        sawUniform = sawUniform || row.kind == QLatin1String("uniform");
    }
    QVERIFY(sawCheck && sawUniform);
    // On from there every element cut: converged to the truth, a fine uniform mesh's.
    Model fine = modelOf(twoParts());
    const QString ft = addCheck(fine, {QStringLiteral("ht.Tmax")}, {{QStringLiteral("tolerance"), slowBuild() ? QStringLiteral("1e-4") : QStringLiteral("1e-5")}, {QStringLiteral("maxmeshes"), 40}});
    const ConvergenceResult truth = check(fine, ft);
    QVERIFY2(truth.converged, qPrintable(truth.summary));
    const double exact = finalOf(truth)->limit;
    QVERIFY2(r.converged, qPrintable(r.summary));
    QVERIFY2(std::abs(finalOf(r)->value - exact) / std::abs(exact - 300) < 1e-2, qPrintable(QStringLiteral("%1 %2").arg(finalOf(r)->value).arg(exact)));
}

void TestFemAdapt::falseAgreementWithoutTheCheckIsSaid()
{
    // Without the check, the same agreement passes, and the result says what it
    // stands on - and the value is off, which is why the check is on by default.
    Model m = modelOf(twoParts());
    const QString tag = addCheck(m, {QStringLiteral("ht.Tmax")},
                                 {{QStringLiteral("tolerance"), QStringLiteral("1e-3")}, {QStringLiteral("physics"), QJsonArray{QStringLiteral("es")}}, {QStringLiteral("check"), false}});
    const ConvergenceResult r = check(m, tag);
    QVERIFY(r.converged);
    QVERIFY(!r.falseConvergence);
    QVERIFY(r.evidence.join(QString()).contains(QStringLiteral("no independent check")));
    const Node* node = convergenceNode(m, tag);
    QVERIFY(node);
    QVERIFY(!node->flag(QStringLiteral("check")));
}

void TestFemAdapt::goalOrientedRefinesForTheValue()
{
    // The sensor's average, in the bar of two plates apart: refined by its
    // adjoint, it settles within the tolerance of a reference, an adjoint solved
    // on each mesh it refined. (It is not cheaper here: the fields' error settles
    // it too, on fewer unknowns - the sensor's own region is smooth, and the
    // goal-oriented value converges one way and then the other, which settles
    // later. Both are logged; the write-up has the numbers.)
    const QString asked = slowBuild() ? QStringLiteral("1e-5") : QStringLiteral("1e-6");
    QByteArray json = twoPlates();
    json.replace("400[K]", "101[K]").replace("300[K]", "1[K]");
    Model ref = modelOf(json);
    // (The reference refined by the adjoint: by the fields' error, at 1e-8, the
    // check catches the sensor's agreement as false and cuts every element.)
    const QString rt = addCheck(ref, {QStringLiteral("sensor")}, {{QStringLiteral("tolerance"), slowBuild() ? QStringLiteral("1e-6") : QStringLiteral("1e-7")},
                                                             {QStringLiteral("refine"), QStringLiteral("goal")}, {QStringLiteral("maxmeshes"), 40}});
    const ConvergenceResult reference = check(ref, rt);
    QVERIFY2(reference.converged, qPrintable(reference.summary + reference.notes.join(QStringLiteral(" | "))));
    const double exact = finalOf(reference)->limit;
    QVERIFY(exact > 1 && exact < 101);
    Model g = modelOf(json);
    const QString gt = addCheck(g, {QStringLiteral("sensor")}, {{QStringLiteral("tolerance"), asked}, {QStringLiteral("refine"), QStringLiteral("goal")}, {QStringLiteral("maxmeshes"), 40}});
    const ConvergenceResult goal = check(g, gt);
    Model a = modelOf(json);
    const QString at = addCheck(a, {QStringLiteral("sensor")}, {{QStringLiteral("tolerance"), asked}, {QStringLiteral("maxmeshes"), 40}});
    const ConvergenceResult fields = check(a, at);
    for (const ConvergenceResult* r : {&goal, &fields})
        qInfo("%s: %s; %d solves (+%d adjoints), %.0f unknowns in all, largest %.0f; error %.2e", r == &goal ? "goal" : "fields", qPrintable(r->status), r->solves,
              r->adjoints, r->unknownsSolved, r->largest, std::abs(finalOf(*r)->value - exact) / exact);
    for (const ConvergenceResult* r : {&goal, &fields}) {
        QVERIFY2(r->converged, qPrintable(r->summary));
        QVERIFY(std::abs(finalOf(*r)->value - exact) / exact <= asked.toDouble());
    }
    int refinedForIt = 0;
    for (const ConvergenceRow& row : goal.rows) refinedForIt += row.kind == QLatin1String("goal");
    QVERIFY(refinedForIt > 0 && goal.adjoints >= refinedForIt);
    QVERIFY(goal.method.contains(QStringLiteral("adjoint")));
    QCOMPARE(fields.adjoints, 0);
}

void TestFemAdapt::goalWithoutAnAdjointFallsBack()
{
    // A capacitance is an energy: its field's error is goal-oriented already.
    // A value of no linear functional: the fields' error, said.
    Model m = modelOf(coax(1));
    const QString tag = addCheck(m, {QStringLiteral("es.C11"), QStringLiteral("es.C11*2")}, {{QStringLiteral("refine"), QStringLiteral("goal")}});
    const ConvergenceResult r = check(m, tag);
    QVERIFY2(r.converged, qPrintable(r.summary));
    QVERIFY(r.method.contains(QStringLiteral("energy")));
    QVERIFY2(r.notes.join(QString()).contains(QStringLiteral("no adjoint")), qPrintable(r.notes.join(QStringLiteral(" | "))));
    QCOMPARE(r.adjoints, 0);
}

void TestFemAdapt::edgeElementsHaveAnIndicator()
{
    // A mode analysis on edge elements: refined by the electric field's recovery,
    // where the old adaptive step refused.
    QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/wr90_modes.qfem"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    Model m = modelOf(f.readAll());
    const QString tag = addCheck(m, {QStringLiteral("emw.neff")}, {{QStringLiteral("tolerance"), QStringLiteral("1e-4")}});
    const ConvergenceResult r = check(m, tag);
    QVERIFY2(r.converged, qPrintable(r.summary));
    QVERIFY2(r.method.contains(QStringLiteral("electric field recovered")), qPrintable(r.method));
    QVERIFY(r.rows.size() >= 3);
    QCOMPARE(r.rows.at(1).kind, QStringLiteral("adaptive"));
}

void TestFemAdapt::capsStopIt()
{
    // At most 3 meshes, a tolerance it cannot reach in them.
    Model m = modelOf(corner());
    QString tag = addCheck(m, {QStringLiteral("es.W")}, {{QStringLiteral("tolerance"), QStringLiteral("1e-6")}, {QStringLiteral("maxmeshes"), 3}});
    ConvergenceResult r = check(m, tag);
    QCOMPARE(r.status, QStringLiteral("meshes"));
    QVERIFY(!r.converged);
    QCOMPARE(r.solves, 3);
    QVERIFY(r.summary.contains(QStringLiteral("Not converged")));
    QVERIFY(finalOf(r) && !finalOf(r)->settled && std::isfinite(finalOf(r)->value));
    // At most 300 unknowns: never one past it.
    Model n = modelOf(corner());
    tag = addCheck(n, {QStringLiteral("es.W")}, {{QStringLiteral("tolerance"), QStringLiteral("1e-6")}, {QStringLiteral("maxunknowns"), 300}});
    r = check(n, tag);
    QCOMPARE(r.status, QStringLiteral("unknowns"));
    for (const ConvergenceRow& row : r.rows) QVERIFY(row.unknowns <= 300);
    QVERIFY(r.notes.join(QString()).contains(QStringLiteral("past the limit")));
}

void TestFemAdapt::cancelKeepsWhatItHas()
{
    Model m = modelOf(corner());
    const QString tag = addCheck(m, {QStringLiteral("es.W")}, {{QStringLiteral("tolerance"), QStringLiteral("1e-7")}, {QStringLiteral("maxmeshes"), 40}});
    // Stopped once the third mesh's solve begins.
    int meshes = 0;
    QString lastWhat;
    const ConvergenceResult r = check(m, tag, [&](double, const QString& what) {
        if (what.startsWith(QStringLiteral("Mesh 3 "))) ++meshes;
        lastWhat = what;
        return meshes == 0;
    });
    QCOMPARE(r.status, QStringLiteral("cancelled"));
    QVERIFY(!r.converged);
    QCOMPARE(int(r.rows.size()), 2);
    QVERIFY(r.summary.startsWith(QStringLiteral("Cancelled")));
    // Stopped before anything: nothing solved, no crash.
    const ConvergenceResult none = check(m, tag, [](double, const QString&) { return false; });
    QCOMPARE(none.status, QStringLiteral("cancelled"));
    QVERIFY(none.rows.empty());
}

void TestFemAdapt::aSolveFailingPartway()
{
    // Its conductivity not a number where the elements are finer than 0.1 mm: the
    // first meshes solve, a later one fails, what came before kept and said.
    QByteArray json = corner("0.25");
    json.replace("\"epsilonr\": \"1\"", "\"epsilonr\": \"sqrt(h - 0.06[mm])/sqrt(1[mm])\"");
    Model m = modelOf(json);
    const QString tag = addCheck(m, {QStringLiteral("es.W")}, {{QStringLiteral("tolerance"), QStringLiteral("1e-6")}, {QStringLiteral("maxmeshes"), 20}});
    const ConvergenceResult r = check(m, tag);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    qInfo("%s | %s", qPrintable(r.summary), qPrintable(r.notes.join(QStringLiteral(" | "))));
    QVERIFY(!r.converged);
    QVERIFY(r.rows.size() >= 2);
    const bool failed = r.status == QLatin1String("failed");
    const bool lost = std::isnan(r.rows.back().values[0]);
    QVERIFY2(failed || lost, qPrintable(r.status));
    if (failed) QVERIFY(r.notes.join(QString()).contains(QStringLiteral("did not solve")));
    // The first mesh does not solve: an error, nothing kept.
    QByteArray never = corner("0.25");
    never.replace("\"epsilonr\": \"1\"", "\"epsilonr\": \"-1/0\"");
    Model n = modelOf(never);
    const QString nt = addCheck(n, {QStringLiteral("es.W")});
    const ConvergenceResult nr = check(n, nt);
    QVERIFY(!nr.converged);
    QVERIFY(nr.status == QLatin1String("failed") || !nr.error.isEmpty());
}

void TestFemAdapt::aValueTheResultsLack()
{
    Model m = modelOf(coax(1));
    const QString tag = addCheck(m, {QStringLiteral("es.nosuch")});
    const ConvergenceResult r = check(m, tag);
    QVERIFY(!r.error.isEmpty());
    QVERIFY2(r.error.contains(QStringLiteral("es.C11")), qPrintable(r.error));
    QVERIFY(r.rows.empty());
    // A field, not one number.
    Model n = modelOf(coax(1));
    const QString nt = addCheck(n, {QStringLiteral("es.normE")});
    QVERIFY(check(n, nt).error.contains(QStringLiteral("varies")));
}

void TestFemAdapt::badSettings()
{
    const QList<QPair<QJsonObject, QString>> bad{
        {{{QStringLiteral("tolerance"), QStringLiteral("0")}}, QStringLiteral("tolerance")},
        {{{QStringLiteral("tolerance"), QStringLiteral("2")}}, QStringLiteral("tolerance")},
        {{{QStringLiteral("tolerance"), QStringLiteral("abc")}}, QStringLiteral("not a number")},
        {{{QStringLiteral("maxmeshes"), 1}}, QStringLiteral("meshes")},
        {{{QStringLiteral("maxmeshes"), 2.5}}, QStringLiteral("meshes")},
        {{{QStringLiteral("maxmeshes"), 1000}}, QStringLiteral("meshes")},
        {{{QStringLiteral("maxunknowns"), QStringLiteral("10")}}, QStringLiteral("unknowns")},
        {{{QStringLiteral("refine"), QStringLiteral("bogus")}}, QStringLiteral("bogus")},
        {{{QStringLiteral("sweep"), QStringLiteral("middle")}}, QStringLiteral("middle")},
        {{{QStringLiteral("physics"), QJsonArray{QStringLiteral("nope")}}}, QStringLiteral("nope")},
        {{{QStringLiteral("study"), QStringLiteral("std9")}}, QStringLiteral("std9")},
    };
    for (const auto& [props, word] : bad) {
        Model m = modelOf(coax(1));
        const QString tag = addCheck(m, {QStringLiteral("es.C11")}, props);
        QString why;
        QVERIFY(!convergencePlan(m, *convergenceNode(m, tag), *evaluateParameters(m), &why));
        QVERIFY2(why.contains(word), qPrintable(why));
        // And the run says the same, before any solve.
        const ConvergenceResult r = check(m, tag);
        QCOMPARE(r.error, why);
        QCOMPARE(r.solves, 0);
    }
    // A row's own tolerance and floor.
    Model m = modelOf(coax(1));
    Node n = m.make(QStringLiteral("convergence"));
    const auto oneRow = [](const QJsonArray& row) {
        QJsonArray rows;
        rows.append(row);
        return rows;
    };
    n.props.insert(QStringLiteral("values"), oneRow(QJsonArray{QStringLiteral("es.C11"), QStringLiteral("5"), QString()}));
    QString why;
    Node* added = m.add(m.root.tag, n, -1, &why);
    QVERIFY(!convergencePlan(m, *added, *evaluateParameters(m), &why));
    QVERIFY(why.contains(QStringLiteral("row 1")));
    added->props.insert(QStringLiteral("values"), oneRow(QJsonArray{QStringLiteral("es.C11"), QString(), QStringLiteral("-1[pF]")}));
    QVERIFY(!convergencePlan(m, *added, *evaluateParameters(m), &why));
    added->props.insert(QStringLiteral("values"), QJsonArray{});
    QVERIFY(!convergencePlan(m, *added, *evaluateParameters(m), &why));
    QVERIFY(why.contains(QStringLiteral("at least one")));
    QJsonArray many;
    for (int i = 0; i < 21; ++i) many.append(QJsonArray{QStringLiteral("es.C11")});
    added->props.insert(QStringLiteral("values"), many);
    QVERIFY(!convergencePlan(m, *added, *evaluateParameters(m), &why));
    // A 3D model's: refused.
    Model three = Model::blank3d();
    Node c = three.make(QStringLiteral("convergence"));
    c.props.insert(QStringLiteral("values"), oneRow(QJsonArray{QStringLiteral("x")}));
    QVERIFY(!convergencePlan(three, c, *evaluateParameters(three), &why));
}

void TestFemAdapt::aSweepsEndsEachChecked()
{
    QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/microstrip_sweep.qfem"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray json = f.readAll();
    Model m = modelOf(json);
    const QString tag = addCheck(m, {QStringLiteral("es.C11")});
    const ConvergenceResult r = check(m, tag);
    QVERIFY2(r.converged, qPrintable(r.summary));
    QCOMPARE(r.points.size(), 2);
    QVERIFY(finalOf(r, 0, 0) && finalOf(r, 0, 1));
    QVERIFY(finalOf(r, 0, 0)->value != finalOf(r, 0, 1)->value);
    // Its own values alone: one series, and said.
    Model own = modelOf(json);
    const QString ot = addCheck(own, {QStringLiteral("es.C11")}, {{QStringLiteral("sweep"), QStringLiteral("own")}});
    const ConvergenceResult o = check(own, ot);
    QVERIFY(o.points.isEmpty());
    QVERIFY(o.notes.join(QString()).contains(QStringLiteral("own values")));
}

void TestFemAdapt::theModelIsNotTouched()
{
    Model m = modelOf(corner());
    const QString tag = addCheck(m, {QStringLiteral("es.W")});
    const QByteArray before = QJsonDocument(m.toObject()).toJson();
    const ConvergenceResult r = check(m, tag);
    QVERIFY(r.converged);
    QCOMPARE(QJsonDocument(m.toObject()).toJson(), before);
}

void TestFemAdapt::theRefinedMeshKept()
{
    // Kept, built again from the sizes: about as fine, and of the same value
    // within the tolerance.
    Model m = modelOf(corner());
    const QString tag = addCheck(m, {QStringLiteral("es.W")}, {{QStringLiteral("tolerance"), QStringLiteral("1e-3")}});
    const ConvergenceResult r = check(m, tag);
    QVERIFY(r.converged && !r.sizes.isEmpty());
    // The coarsest mesh within the tolerance is kept, not the check's when a coarser
    // one is enough (the review of 063142f, K1): here every mesh before the check
    // is off its limit by more than the room the finest's error leaves.
    QVERIFY(r.keptRow > 0 && r.keptRow <= r.rows.back().index);
    int keptTriangles = 0;
    for (const ConvergenceRow& row : r.rows)
        if (row.index == r.keptRow) keptTriangles = row.triangles;
    QVERIFY(keptTriangles > 0 && keptTriangles <= r.rows.back().triangles);
    QVERIFY(r.notes.join(QString()).contains(QStringLiteral("Keep Refined Mesh keeps mesh %1").arg(r.keptRow)));
    {
        // A coax of quadratic elements at 1e-3: its own mesh is enough, and Keep
        // says so rather than keep the check's mesh.
        Model c = modelOf(coax(2));
        const QString ct = addCheck(c, {QStringLiteral("es.C11")}, {{QStringLiteral("tolerance"), QStringLiteral("1e-3")}});
        const ConvergenceResult cr = check(c, ct);
        QVERIFY(cr.converged);
        QCOMPARE(cr.keptRow, 1);
        QVERIFY(cr.sizes.isEmpty());
        c.find(ct)->props.insert(QStringLiteral("result"), cr.toJson());
        QString said;
        QVERIFY(!qucs_s::fem::keepRefinedMesh(c, *c.find(ct), &said));
        QVERIFY2(said.contains(QStringLiteral("within the tolerance already")), qPrintable(said));
    }
    Node* node = m.find(tag);
    node->props.insert(QStringLiteral("result"), r.toJson());
    QString why;
    QVERIFY2(qucs_s::fem::keepRefinedMesh(m, *node, &why), qPrintable(why));
    // Once: the second replaces the first.
    QVERIFY(qucs_s::fem::keepRefinedMesh(m, *m.find(tag), &why));
    // Kept, the result is not "changed since" (its own sizes kept) - the review's nit.
    QCOMPARE(convergenceSetup(m, *m.find(tag)), r.setup);
    int adapted = 0;
    for (const Node& c : m.mesh().children) adapted += c.type == QLatin1String("mesh/adapted");
    QCOMPARE(adapted, 1);
    // Through the file and back.
    QString problems;
    const std::optional<Model> again = Model::fromJson(QJsonDocument(m.toObject()).toJson(), &problems);
    QVERIFY2(again && problems.isEmpty(), qPrintable(problems));
    const auto ps = evaluateParameters(*again);
    const GeometryBuild g = buildGeometry(*again, *ps);
    const MeshBuild b = buildMesh(*again, *ps, g.topology);
    QVERIFY(b.mesh);
    StudyRun run = runStudy(*again, ps, g.topology, b.mesh, QString());
    QVERIFY(run.ok());
    const double w = *run.solutions.front()->global(QStringLiteral("es.W"));
    const ConvergenceResult::Final* f = finalOf(r);
    qInfo("kept: %zu triangles (mesh %d had %d, the finest %d), W %.8g against %.8g", b.mesh->triangles.size(), r.keptRow, keptTriangles, r.rows.back().triangles, w, f->limit);
    QVERIFY2(std::abs(w - f->limit) / std::abs(f->limit) <= 1e-3, qPrintable(QString::number(std::abs(w - f->limit) / std::abs(f->limit))));
    // Nothing to keep: said.
    Model n = modelOf(corner());
    const QString nt = addCheck(n, {QStringLiteral("es.W")});
    QVERIFY(!qucs_s::fem::keepRefinedMesh(n, *n.find(nt), &why));
    QVERIFY(why.contains(QStringLiteral("no refined mesh")));
}

void TestFemAdapt::resultInTheFile()
{
    Model m = modelOf(corner());
    const QString tag = addCheck(m, {QStringLiteral("es.W"), QStringLiteral("es.C11")});
    ConvergenceResult r = check(m, tag);
    QVERIFY(r.converged);
    m.find(tag)->props.insert(QStringLiteral("result"), r.toJson());
    const QJsonObject o = m.toObject();
    QVERIFY(o.contains(QStringLiteral("convergences")));
    QCOMPARE(o.value(QStringLiteral("convergences")).toArray().size(), 1);
    QString problems;
    const std::optional<Model> back = Model::fromJson(QJsonDocument(o).toJson(), &problems);
    QVERIFY2(back && problems.isEmpty(), qPrintable(problems));
    const Node* node = convergenceNode(*back, tag);
    QVERIFY(node);
    // Under the tree's group node, after the studies.
    const Node* group = back->root.child(QStringLiteral("convergences"));
    QVERIFY(group && group->children.size() == 1);
    const ConvergenceResult again = ConvergenceResult::fromJson(node->value(QStringLiteral("result")).toObject());
    QCOMPARE(again.status, r.status);
    QCOMPARE(again.converged, r.converged);
    QCOMPARE(again.rows.size(), r.rows.size());
    QCOMPARE(again.values.size(), r.values.size());
    QCOMPARE(again.columns, r.columns);
    QCOMPARE(again.units, r.units);
    QCOMPARE(again.evidence, r.evidence);
    QCOMPARE(again.sizes, r.sizes);
    for (std::size_t i = 0; i < r.rows.size(); ++i) {
        QCOMPARE(again.rows[i].unknowns, r.rows[i].unknowns);
        QCOMPARE(again.rows[i].values, r.rows[i].values);
        QCOMPARE(again.rows[i].kind, r.rows[i].kind);
    }
    QCOMPARE(again.values[0].limit, r.values[0].limit);
    QCOMPARE(again.setup, convergenceSetup(*back, *node));
    // The model changed: the result is of the model before.
    Model changed = *back;
    changed.find(changed.mesh().tag)->props.insert(QStringLiteral("hmax"), QStringLiteral("0.2"));
    QVERIFY(again.setup != convergenceSetup(changed, *convergenceNode(changed, tag)));
    // An error kept as an error.
    ConvergenceResult failed;
    failed.error = QStringLiteral("x");
    QCOMPARE(ConvergenceResult::fromJson(failed.toJson()).error, QStringLiteral("x"));
}

void TestFemAdapt::aDamagedResultIsReadAsFarAsItGoes()
{
    Model m = modelOf(corner());
    const QString tag = addCheck(m, {QStringLiteral("es.W")});
    const QJsonObject good = check(m, tag).toJson();
    // Rows of another length, of junk, values of a column it has not.
    QJsonObject bad = good;
    QJsonArray rows = bad.value(QStringLiteral("rows")).toArray();
    rows.append(QStringLiteral("1 2 3"));
    rows.append(QStringLiteral("a b c d e f g h i"));
    rows.append(42);
    rows.append(QStringLiteral("1 -4 start 3 4 5 6 7 8"));
    bad.insert(QStringLiteral("rows"), rows);
    QJsonArray values = bad.value(QStringLiteral("values")).toArray();
    values.append(QJsonObject{{QStringLiteral("column"), 7}});
    values.append(QJsonObject{{QStringLiteral("column"), -1}});
    values.append(QStringLiteral("x"));
    bad.insert(QStringLiteral("values"), values);
    bad.insert(QStringLiteral("units"), QJsonArray{});
    bad.insert(QStringLiteral("solves"), QStringLiteral("many"));
    const ConvergenceResult r = ConvergenceResult::fromJson(bad);
    QCOMPARE(r.rows.size(), ConvergenceResult::fromJson(good).rows.size());
    QCOMPARE(r.values.size(), std::size_t(1));
    QCOMPARE(r.units.size(), r.columns.size());
    // Anything at all: nothing much, no crash.
    for (const QJsonValue& v : {QJsonValue(), QJsonValue(1), QJsonValue(QStringLiteral("x")), QJsonValue(QJsonArray{1, 2})})
        ConvergenceResult::fromJson(QJsonObject{{QStringLiteral("rows"), v}, {QStringLiteral("values"), v}, {QStringLiteral("columns"), v}});
    // inf and nan kept as words.
    ConvergenceResult n;
    n.status = QStringLiteral("meshes");
    n.columns = {QStringLiteral("a")};
    ConvergenceResult::Final f;
    n.values.push_back(f);
    const ConvergenceResult back = ConvergenceResult::fromJson(n.toJson());
    QVERIFY(std::isinf(back.values.at(0).error) && std::isnan(back.values.at(0).value));
}

void TestFemAdapt::oldFilesLoadUnchanged()
{
    // Every shipped model: read and written with no "convergences" added, the
    // tree's node there (an empty group, of a 2D model).
    const QDir dir(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics"));
    int read = 0;
    for (const QString& name : dir.entryList({QStringLiteral("*.qfem")})) {
        QFile f(dir.filePath(name));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QString why;
        const std::optional<Model> m = Model::fromJson(f.readAll(), &why);
        QVERIFY2(m, qPrintable(name + QStringLiteral(": ") + why));
        QVERIFY2(!m->toObject().contains(QStringLiteral("convergences")), qPrintable(name));
        if (!m->is3D()) QVERIFY2(m->root.child(QStringLiteral("convergences")), qPrintable(name));
        QVERIFY(convergenceNodes(*m).empty());
        ++read;
    }
    QVERIFY(read > 40);
    // A 3D model has no such node.
    QVERIFY(!Model::blank3d().root.child(QStringLiteral("convergences")));
    // And a blank 2D one's, with nothing in the file.
    const Model blank = Model::blank();
    QVERIFY(blank.root.child(QStringLiteral("convergences")));
    QVERIFY(!blank.toObject().contains(QStringLiteral("convergences")));
}

void TestFemAdapt::adaptiveStepsStillWork()
{
    // A study's own adaptive step solves as it did: the check leaves it alone.
    QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/adaptive_corner.qfem"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const Model m = modelOf(f.readAll());
    const auto ps = evaluateParameters(m);
    const GeometryBuild g = buildGeometry(m, *ps);
    StudyRun run = runStudy(m, ps, g.topology, nullptr, QString());
    QVERIFY(run.ok());
    QVERIFY(run.log.join(QString()).contains(QStringLiteral("Adaptive mesh refinement")));
    QCOMPARE(int(run.solutions.front()->mesh().triangles.size()), 1168);
}

QTEST_GUILESS_MAIN(TestFemAdapt)
#include "test_fem_adapt.moc"
