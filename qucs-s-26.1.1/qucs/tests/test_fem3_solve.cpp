/*
 * test_fem3_solve.cpp - heat, electrostatics and electric currents solved in
 *                       3D (multiphysics/fem3_solver.h): patch tests (a
 *                       linear field order 1's, a quadratic order 2's,
 *                       exactly), the error's rate as the mesh is refined, a
 *                       model uniform in z the 2D model of its plan times its
 *                       thickness to rounding (fields, capacitance and
 *                       conductance matrices, power), a TSV's oxide liner the
 *                       coaxial capacitance, terminals, Joule heating, a
 *                       film and radiation, a crystal's tensor in a 3D
 *                       model's axes, multigrid against a direct solve, and
 *                       what is refused
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_estimate.h"
#include "fem3_export.h"
#include "fem3_results.h"
#include "fem3_solver.h"

#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtTest>

#include <cmath>
#include <set>

using namespace qucs_s::fem;

namespace {

constexpr double Eps0 = 8.8541878188e-12;

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
}

QJsonObject rule(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

/// A box of six numbers (the geometry's unit) as a selection.
QJsonObject boxRule(double x0, double y0, double z0, double x1, double y1, double z1)
{
    QJsonArray b;
    for (double v : {x0, y0, z0, x1, y1, z1}) b.append(v);
    return QJsonObject{{QStringLiteral("box"), b}};
}

QJsonObject boxRule2(double x0, double y0, double x1, double y1)
{
    QJsonArray b;
    for (double v : {x0, y0, x1, y1}) b.append(v);
    return QJsonObject{{QStringLiteral("box"), b}};
}

QJsonObject ofSide(const QString& object, const QString& side)
{
    QJsonArray of;
    of.append(object);
    return QJsonObject{{QStringLiteral("of"), of}, {QStringLiteral("side"), side}};
}

QJsonObject objects(const QString& object)
{
    QJsonArray of;
    of.append(object);
    return QJsonObject{{QStringLiteral("objects"), of}};
}

Model model3d(double hmax)
{
    Model m = Model::blank3d();
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QString::number(hmax, 'g', 17));
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

void addCylinder(Model& m, const QString& label, double r, double h)
{
    Node n = m.make(QStringLiteral("cylinder"));
    n.label = label;
    n.props.insert(QStringLiteral("center"), pairOf(0, 0));
    n.props.insert(QStringLiteral("radius"), QString::number(r, 'g', 17));
    n.props.insert(QStringLiteral("z"), QStringLiteral("0"));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    m.add(m.geometry().tag, n);
}

/// A physics of \a order; its domain feature user's \a value (k, εr, σ) where
/// given.
QString addPhysics(Model& m, const QString& type, int order, const QString& value = {})
{
    Node p = m.make(type);
    p.props.insert(QStringLiteral("order"), QString::number(order));
    const QString tag = p.tag;
    m.add(m.component().tag, p);
    if (!value.isEmpty()) {
        Node* n = m.find(tag);
        const QString feature = type == QLatin1String("heat") ? QStringLiteral("heat/solid")
                                : type == QLatin1String("electrostatics") ? QStringLiteral("electrostatics/charge")
                                                                          : QStringLiteral("currents/conservation");
        const QString key = type == QLatin1String("heat") ? QStringLiteral("k") : type == QLatin1String("electrostatics") ? QStringLiteral("epsilonr") : QStringLiteral("sigma");
        for (Node& c : n->children)
            if (c.type == feature) {
                c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
                c.props.insert(key, value);
            }
    }
    return tag;
}

void addFeature(Model& m, const QString& physics, const QString& type, const QJsonObject& selection, const QJsonObject& props = {})
{
    Node f = m.make(type);
    f.props.insert(QStringLiteral("selection"), selection);
    for (auto it = props.begin(); it != props.end(); ++it) f.props.insert(it.key(), it.value());
    QString why;
    QVERIFY2(m.add(physics, f, -1, &why) != nullptr, qPrintable(why));
}

void addMaterial(Model& m, const QString& label, const QJsonObject& props, const QJsonObject& selection)
{
    Node mat = m.make(QStringLiteral("material"));
    mat.label = label;
    for (auto it = props.begin(); it != props.end(); ++it) mat.props.insert(it.key(), it.value());
    mat.props.insert(QStringLiteral("selection"), selection);
    m.add(m.materials().tag, mat);
}

/// The first study's Stationary step.
Node& stationary(Model& m)
{
    for (Node& c : m.root.children)
        if (c.type == QLatin1String("study"))
            for (Node& s : c.children)
                if (s.type == QLatin1String("stationary")) return s;
    static Node none;
    return none;
}

/// The first study's steps one Time Dependent step of \a props.
Node& inTime(Model& m, const QJsonObject& props)
{
    for (Node& c : m.root.children)
        if (c.type == QLatin1String("study")) {
            c.children.clear();
            Node t = m.make(QStringLiteral("transient"));
            for (auto it = props.begin(); it != props.end(); ++it) t.props.insert(it.key(), it.value());
            c.children.push_back(t);
            return c.children.back();
        }
    static Node none;
    return none;
}

struct Solved {
    std::shared_ptr<const Mesh3> mesh;
    StudyResult3 r;
    QString error;
    Solution3& s() const { return *r.solution; }
};

Solved solve(const Model& m)
{
    Solved out;
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    const GeometryBuild3 g = buildGeometry3(m, *ps);
    if (!g.ok()) {
        out.error = QStringLiteral("geometry: ") + g.errors.value(g.failedAt);
        return out;
    }
    const Mesh3Build mb = buildMesh3(m, *ps, g.topology);
    if (!mb.mesh) {
        out.error = QStringLiteral("mesh: ") + mb.error;
        return out;
    }
    out.mesh = mb.mesh;
    out.r = solveStudy3(m, ps, mb.mesh);
    out.error = out.r.error;
    return out;
}

/// \a text's value at point \a x (mm) of a solution.
double valueAt(const Solution3& s, const QString& text, double x, double y, double z)
{
    PointEvaluator3 ev(s);
    const double k = s.mesh().unitScale;
    if (!ev.at(Vec3{x * k, y * k, z * k})) return std::nan("");
    return ev.eval(Expression::compile(text, s.pointScope()));
}

double global(const Solution3& s, const QString& name)
{
    return s.global(name).value_or(std::nan(""));
}

/// ∫ \a text dV over the solution's prisms (its first field's), SI.
double integral(const Solution3& s, const QString& text)
{
    const Expression e = Expression::compile(text, s.scope());
    const Space3& sp = *s.fields().front().space;
    PointEvaluator3 ev(s);
    double sum = 0;
    for (int el = 0; el < sp.elements(); ++el)
        for (const auto& q : prismRule(4, 3).points) {
            const Space3::Map m = sp.map(el, q[0], q[1], q[2]);
            ev.at(el, q[0], q[1], q[2], m.x);
            sum += q[3] * std::abs(m.det) * ev.eval(e);
        }
    return sum;
}

/// The L2 norm of \a text less \a exact (of x, y, z in m) over the solution.
double l2Error(const Solution3& s, const QString& text, const std::function<double(double, double, double)>& exact)
{
    const Expression e = Expression::compile(text, s.pointScope());
    const Space3& sp = *s.fields().front().space;
    PointEvaluator3 ev(s);
    double sum = 0;
    for (int el = 0; el < sp.elements(); ++el)
        for (const auto& q : prismRule(5, 3).points) {
            const Space3::Map m = sp.map(el, q[0], q[1], q[2]);
            ev.at(el, q[0], q[1], q[2], m.x);
            const double d = ev.eval(e) - exact(m.x.x, m.x.y, m.x.z);
            sum += q[3] * std::abs(m.det) * d * d;
        }
    return std::sqrt(sum);
}

} // namespace

class TestFem3Solve : public QObject
{
    Q_OBJECT

private slots:
    // A linear temperature held on the faces is order 1's exactly, through
    // two boxes and the face between them; a quadratic one with its source
    // order 2's - at every degree of freedom, edges' and layers' middles too.
    void patchTests()
    {
        for (int order : {1, 2}) {
            Model m = model3d(0.4);
            addBox(m, QStringLiteral("left"), 0, 0, 0, 1, 1, 1);
            addBox(m, QStringLiteral("right"), 1, 0, 0, 1, 1, 1);
            const QString ht = addPhysics(m, QStringLiteral("heat"), order, QStringLiteral("2[W/(m*K)]"));
            const QString exact = order == 1 ? QStringLiteral("300[K] + 1e4[K/m]*(x + 2*y + 3*z)") : QStringLiteral("300[K] + 1e7[K/m^2]*x^2 + 2e6[K/m^2]*y*z");
            addFeature(m, ht, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"), QJsonObject{{QStringLiteral("T0"), exact}});
            // (-k ΔT = -2·1e7·2 for the quadratic: its source.)
            if (order == 2) addFeature(m, ht, QStringLiteral("heat/source"), rule(R"({"all": true})"), QJsonObject{{QStringLiteral("Q0"), QStringLiteral("-4e7[W/m^3]")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const Field3& f = sv.s().fields().front();
            double worst = 0;
            for (int dof = 0; dof < f.space->size(); ++dof) {
                const Vec3 x = f.space->point(dof);
                const double want = 300 + (order == 1 ? 1e4 * (x.x + 2 * x.y + 3 * x.z) : 1e7 * x.x * x.x + 2e6 * x.y * x.z);
                worst = std::max(worst, std::abs(f.values[std::size_t(dof)] - want));
            }
            QVERIFY2(worst < 1e-7, qPrintable(QStringLiteral("order %1: %2 K off").arg(order).arg(worst)));
        }
    }

    // sin·sin·sin in a cube, held at its faces: the L2 error falls as h² with
    // order 1 and as h³ with order 2.
    void convergence()
    {
        const auto exact = [](double x, double y, double z) {
            const double k = M_PI / 1e-3;
            return 300 + 100 * std::sin(k * x) * std::sin(k * y) * std::sin(k * z);
        };
        for (int order : {1, 2}) {
            double errors[2];
            // (Order 1 past its coarsest meshes, where its rate is still on
            // its way to 2: 1.78 from 0.2 to 0.1 mm.)
            const double sizes[2] = {order == 1 ? 0.1 : 0.4, order == 1 ? 0.05 : 0.2};
            for (int i = 0; i < 2; ++i) {
                Model m = model3d(sizes[i]);
                addBox(m, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
                const QString ht = addPhysics(m, QStringLiteral("heat"), order, QStringLiteral("2[W/(m*K)]"));
                addFeature(m, ht, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"), QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
                addFeature(m, ht, QStringLiteral("heat/source"), rule(R"({"all": true})"),
                           QJsonObject{{QStringLiteral("Q0"), QStringLiteral("3*(pi/1[mm])^2*2[W/(m*K)]*100[K]*sin(pi*x/1[mm])*sin(pi*y/1[mm])*sin(pi*z/1[mm])")}});
                const Solved sv = solve(m);
                QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
                errors[i] = l2Error(sv.s(), QStringLiteral("ht.T"), exact);
            }
            const double rate = std::log2(errors[0] / errors[1]);
            QVERIFY2(rate > (order == 1 ? 1.8 : 2.7), qPrintable(QStringLiteral("order %1: errors %2, %3 - rate %4").arg(order).arg(errors[0]).arg(errors[1]).arg(rate)));
        }
    }

    // A model uniform in z - two materials side by side, a source, a held
    // wall and a film on the other, top and bottom insulated - is its plan's
    // 2D model times its thickness: the same temperature at each degree of
    // freedom, the same capacitance and conductance matrices, resistance,
    // energy and power, to rounding - order 1 and 2, on the 3D model's own
    // plan mesh.
    void invariantInZ()
    {
        for (int order : {1, 2}) {
            Model m3 = model3d(0.3);
            addBox(m3, QStringLiteral("blk1"), 0, 0, 0, 1, 1, 0.5);
            addBox(m3, QStringLiteral("blk2"), 1, 0, 0, 1, 1, 0.5);
            stationary(m3).props.insert(QStringLiteral("solver"), QStringLiteral("direct"));
            Model m2 = Model::blank();
            m2.component().props.insert(QStringLiteral("thickness"), QStringLiteral("0.5[mm]"));
            for (Node& c : m2.root.children)
                if (c.type == QLatin1String("study"))
                    for (Node& st : c.children) st.props.insert(QStringLiteral("solver"), QStringLiteral("direct"));
            const QJsonObject propsA{{QStringLiteral("k"), QStringLiteral("1[W/(m*K)]")},
                                     {QStringLiteral("epsilonr"), QStringLiteral("2")},
                                     {QStringLiteral("sigma"), QStringLiteral("1e3[S/m]")}};
            const QJsonObject propsB{{QStringLiteral("k"), QStringLiteral("3[W/(m*K)]")},
                                     {QStringLiteral("epsilonr"), QStringLiteral("5")},
                                     {QStringLiteral("sigma"), QStringLiteral("4e3[S/m]")}};
            addMaterial(m3, QStringLiteral("A"), propsA, objects(QStringLiteral("blk1")));
            addMaterial(m3, QStringLiteral("B"), propsB, objects(QStringLiteral("blk2")));
            addMaterial(m2, QStringLiteral("A"), propsA, boxRule2(-0.01, -0.01, 1.01, 1.01));
            addMaterial(m2, QStringLiteral("B"), propsB, boxRule2(0.99, -0.01, 2.01, 1.01));
            const QJsonObject left3 = ofSide(QStringLiteral("blk1"), QStringLiteral("left")), right3 = ofSide(QStringLiteral("blk2"), QStringLiteral("right"));
            const QJsonObject back3 = boxRule(-0.01, 0.99, -0.01, 2.01, 1.01, 0.51);
            const QJsonObject left2 = boxRule2(-0.01, -0.01, 0.01, 1.01), right2 = boxRule2(1.99, -0.01, 2.01, 1.01);
            const QJsonObject back2 = boxRule2(-0.01, 0.99, 2.01, 1.01);
            for (Model* m : {&m3, &m2}) {
                const bool three = m == &m3;
                const QString ht = addPhysics(*m, QStringLiteral("heat"), order);
                addFeature(*m, ht, QStringLiteral("heat/temperature"), three ? left3 : left2, QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
                addFeature(*m, ht, QStringLiteral("heat/flux"), three ? right3 : right2,
                           QJsonObject{{QStringLiteral("kind"), QStringLiteral("convective")}, {QStringLiteral("h"), QStringLiteral("5e4[W/(m^2*K)]")}, {QStringLiteral("Text"), QStringLiteral("310[K]")}});
                addFeature(*m, ht, QStringLiteral("heat/source"), three ? objects(QStringLiteral("blk1")) : boxRule2(-0.01, -0.01, 1.01, 1.01),
                           QJsonObject{{QStringLiteral("Q0"), QStringLiteral("1e9[W/m^3]")}});
                for (const QString type : {QStringLiteral("electrostatics"), QStringLiteral("currents")}) {
                    const QString p = addPhysics(*m, type, order);
                    addFeature(*m, p, type + QStringLiteral("/terminal"), three ? left3 : left2, QJsonObject{{QStringLiteral("name"), QStringLiteral("1")}});
                    addFeature(*m, p, type + QStringLiteral("/terminal"), three ? right3 : right2,
                               QJsonObject{{QStringLiteral("name"), QStringLiteral("2")}, {QStringLiteral("V0"), QStringLiteral("0.5[V]")}});
                    addFeature(*m, p, type + QStringLiteral("/ground"), three ? back3 : back2);
                }
            }
            const Solved s3 = solve(m3);
            QVERIFY2(s3.error.isEmpty(), qPrintable(s3.error));
            const StudyResult r2 = solveStudy(m2, evaluateParameters(m2), s3.mesh->plan, QString());
            QVERIFY2(r2.error.isEmpty() && r2.solution, qPrintable(r2.error));
            const Solution& two = *r2.solution;
            for (const Field3& f3 : s3.s().fields()) {
                const int fi = two.fieldIndex(f3.tag);
                QVERIFY(fi >= 0);
                const Field& f2 = two.fields()[std::size_t(fi)];
                QCOMPARE(f2.space->size(), f3.space->plan().size());
                double worst = 0, scale = 0;
                for (int dof = 0; dof < f3.space->size(); ++dof) {
                    const double v2 = f2.values[std::size_t(f3.space->origin(dof).first)];
                    worst = std::max(worst, std::abs(f3.values[std::size_t(dof)] - v2));
                    scale = std::max(scale, std::abs(v2));
                }
                QVERIFY2(worst <= 1e-9 * scale, qPrintable(QStringLiteral("%1, order %2: %3 off").arg(f3.tag).arg(order).arg(worst)));
            }
            for (const QString name : {QStringLiteral("ht.Tmax"), QStringLiteral("ht.P"), QStringLiteral("es.C11"), QStringLiteral("es.C12"),
                                       QStringLiteral("es.C22"), QStringLiteral("es.Q_1"), QStringLiteral("es.W"), QStringLiteral("ec.G11"),
                                       QStringLiteral("ec.G12"), QStringLiteral("ec.R"), QStringLiteral("ec.I_2"), QStringLiteral("ec.P")}) {
                const double a = global(s3.s(), name), b = two.global(name).value_or(std::nan(""));
                QVERIFY2(std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-9 * std::abs(b),
                         qPrintable(QStringLiteral("%1, order %2: 3D %3, 2D %4").arg(name).arg(order).arg(a, 0, 'g', 12).arg(b, 0, 'g', 12)));
            }
        }
    }

    // A TSV's oxide liner - a coaxial shell between the copper and the
    // silicon - holds 2π ε0 εr h / ln(b/a) (G. Katti, M. Stucchi, K. De Meyer
    // and W. Dehaene, IEEE Trans. Electron Devices 57, 256-262, 2010, its
    // oxide capacitance): order 2 on its arcs within 0.1 %, order 1's polygon
    // short of them.
    void coaxialLiner()
    {
        const double a = 0.1, b = 0.3, h = 0.5, er = 3.9;
        const double exact = 2 * M_PI * Eps0 * er * h * 1e-3 / std::log(b / a);
        double c[3] = {0, 0, 0};
        for (int order : {1, 2}) {
            Model m = model3d(0.05);
            addCylinder(m, QStringLiteral("shell"), b, h);
            addCylinder(m, QStringLiteral("via"), a, h);
            Node d = m.make(QStringLiteral("difference"));
            d.label = QStringLiteral("liner");
            d.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("shell")});
            d.props.insert(QStringLiteral("tools"), QJsonArray{QStringLiteral("via")});
            m.add(m.geometry().tag, d);
            const QString es = addPhysics(m, QStringLiteral("electrostatics"), order, QString::number(er));
            // Every face grounded, then the via's wall - wholly inside its
            // radius - its terminal, and the top and bottom zero charge: each
            // later feature wins where they meet.
            QJsonArray nz, nz2;
            for (double v : {0.0, 0.0, 1.0}) nz.append(v);
            for (double v : {0.0, 0.0, -1.0}) nz2.append(v);
            addFeature(m, es, QStringLiteral("electrostatics/ground"), boxRule(-b - 0.01, -b - 0.01, -0.01, b + 0.01, b + 0.01, h + 0.01));
            addFeature(m, es, QStringLiteral("electrostatics/terminal"), boxRule(-a - 0.01, -a - 0.01, -0.01, a + 0.01, a + 0.01, h + 0.01));
            addFeature(m, es, QStringLiteral("electrostatics/zerocharge"), QJsonObject{{QStringLiteral("normal"), nz}});
            addFeature(m, es, QStringLiteral("electrostatics/zerocharge"), QJsonObject{{QStringLiteral("normal"), nz2}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            c[order] = global(sv.s(), QStringLiteral("es.C11"));
        }
        QVERIFY2(std::abs(c[2] - exact) < 1e-3 * exact, qPrintable(QStringLiteral("order 2: %1 F, the formula %2 F").arg(c[2]).arg(exact)));
        QVERIFY2(std::abs(c[1] - exact) < 5e-2 * exact && std::abs(c[1] - exact) > std::abs(c[2] - exact),
                 qPrintable(QStringLiteral("order 1: %1 F").arg(c[1])));
    }

    // A bar's resistance L/(σA); three terminals' capacitance matrix
    // symmetric, its diagonal positive and the rest negative; a floating
    // conductor between two at a potential between theirs.
    void terminals()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("bar"), 0, 0, 0, 4, 1, 1);
        const QString ec = addPhysics(m, QStringLiteral("currents"), 1, QStringLiteral("1e6[S/m]"));
        addFeature(m, ec, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar"), QStringLiteral("left")), QJsonObject{{QStringLiteral("name"), QStringLiteral("a")}});
        addFeature(m, ec, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar"), QStringLiteral("right")));
        Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        const double R = 4e-3 / (1e6 * 1e-6);
        QVERIFY2(std::abs(global(sv.s(), QStringLiteral("ec.R")) - R) < 1e-7 * R, qPrintable(sv.s().globalText(QStringLiteral("ec.R"))));
        QVERIFY(std::abs(global(sv.s(), QStringLiteral("ec.I_a")) - 1 / R) < 1e-7 / R);
        // E = -∇V, from the terminal (1 V) to the ground: along +x; J = σ E.
        const double Ex = valueAt(sv.s(), QStringLiteral("ec.Ex"), 2, 0.5, 0.5);
        QVERIFY2(std::abs(Ex - 1 / 4e-3) < 1e-6 / 4e-3, qPrintable(QString::number(Ex)));
        QVERIFY(std::abs(valueAt(sv.s(), QStringLiteral("ec.Jx"), 2, 0.5, 0.5) - 1e6 * Ex) < 1e-6 * 1e6 * Ex);
        QVERIFY(std::abs(valueAt(sv.s(), QStringLiteral("ec.Ey"), 2, 0.5, 0.5)) < 1e-6 * Ex);

        Model e = model3d(0.4);
        addBox(e, QStringLiteral("box"), 0, 0, 0, 3, 1, 1);
        const QString es = addPhysics(e, QStringLiteral("electrostatics"), 1, QStringLiteral("1"));
        addFeature(e, es, QStringLiteral("electrostatics/terminal"), ofSide(QStringLiteral("box"), QStringLiteral("left")), QJsonObject{{QStringLiteral("name"), QStringLiteral("1")}});
        addFeature(e, es, QStringLiteral("electrostatics/terminal"), ofSide(QStringLiteral("box"), QStringLiteral("right")), QJsonObject{{QStringLiteral("name"), QStringLiteral("2")}});
        addFeature(e, es, QStringLiteral("electrostatics/terminal"), ofSide(QStringLiteral("box"), QStringLiteral("top")), QJsonObject{{QStringLiteral("name"), QStringLiteral("3")}});
        addFeature(e, es, QStringLiteral("electrostatics/ground"), ofSide(QStringLiteral("box"), QStringLiteral("bottom")));
        sv = solve(e);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        const auto C = [&sv](int i, int j) { return global(sv.s(), QStringLiteral("es.C%1%2").arg(i).arg(j)); };
        for (int i = 1; i <= 3; ++i) {
            QVERIFY(C(i, i) > 0);
            for (int j = 1; j <= 3; ++j)
                if (i != j) {
                    QVERIFY(C(i, j) < 0);
                    QVERIFY2(std::abs(C(i, j) - C(j, i)) < 1e-6 * std::abs(C(i, j)), qPrintable(QStringLiteral("C%1%2 %3, C%2%1 %4").arg(i).arg(j).arg(C(i, j)).arg(C(j, i))));
                }
        }
        // A space charge in a box whose every face is one terminal at 0 V:
        // the terminal holds its opposite, -ρV, Gauss's law in discrete form.
        Model q = model3d(0.4);
        addBox(q, QStringLiteral("box"), 0, 0, 0, 3, 1, 1);
        const QString qs = addPhysics(q, QStringLiteral("electrostatics"), 1, QStringLiteral("2"));
        addFeature(q, qs, QStringLiteral("electrostatics/terminal"), rule(R"({"exterior": true})"),
                   QJsonObject{{QStringLiteral("name"), QStringLiteral("1")}, {QStringLiteral("V0"), QStringLiteral("0[V]")}});
        addFeature(q, qs, QStringLiteral("electrostatics/spacecharge"), rule(R"({"all": true})"), QJsonObject{{QStringLiteral("rho"), QStringLiteral("1e-3[C/m^3]")}});
        stationary(q).props.insert(QStringLiteral("sweep"), false);
        sv = solve(q);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        const double Q = global(sv.s(), QStringLiteral("es.Q_1")), want = -1e-3 * 3e-9;
        QVERIFY2(std::abs(Q - want) < 1e-9 * std::abs(want), qPrintable(QStringLiteral("%1 C, -ρV %2 C").arg(Q).arg(want)));
        // A floating conductor (no charge) in the middle: between 0 and 1 V.
        Model f = model3d(0.4);
        addBox(f, QStringLiteral("a"), 0, 0, 0, 1, 1, 1);
        addBox(f, QStringLiteral("b"), 1, 0, 0, 1, 1, 1);
        const QString fs = addPhysics(f, QStringLiteral("electrostatics"), 1, QStringLiteral("1"));
        addFeature(f, fs, QStringLiteral("electrostatics/terminal"), ofSide(QStringLiteral("a"), QStringLiteral("left")), QJsonObject{{QStringLiteral("name"), QStringLiteral("1")}});
        addFeature(f, fs, QStringLiteral("electrostatics/ground"), ofSide(QStringLiteral("b"), QStringLiteral("right")));
        addFeature(f, fs, QStringLiteral("electrostatics/floating"), boxRule(0.99, -0.01, -0.01, 1.01, 1.01, 1.01));
        stationary(f).props.insert(QStringLiteral("sweep"), false);
        sv = solve(f);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        const Field3& field = sv.s().fields().front();
        QCOMPARE(int(field.terminals.size()), 2);
        const double vf = field.terminals[1].potential;
        // (Series capacitors of one width: the middle at half the volt.)
        QVERIFY2(std::abs(vf - 0.5) < 1e-6, qPrintable(QString::number(vf)));
    }

    // A bar's Joule heat, its ends held cold and its sides insulated: T rises
    // by σ(V/L)² L²/(8k) in the middle - order 2's exactly, ec and ht solved
    // in turn till they agree.
    void jouleHeating()
    {
        Model m = model3d(0.4);
        addBox(m, QStringLiteral("bar"), 0, 0, 0, 2, 0.5, 0.5);
        const QString ec = addPhysics(m, QStringLiteral("currents"), 2, QStringLiteral("5e7[S/m]"));
        addFeature(m, ec, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar"), QStringLiteral("left")),
                   QJsonObject{{QStringLiteral("name"), QStringLiteral("1")}, {QStringLiteral("V0"), QStringLiteral("0.1[V]")}});
        addFeature(m, ec, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar"), QStringLiteral("right")));
        const QString ht = addPhysics(m, QStringLiteral("heat"), 2, QStringLiteral("400[W/(m*K)]"));
        addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("bar"), QStringLiteral("left")), QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
        addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("bar"), QStringLiteral("right")), QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
        addFeature(m, ht, QStringLiteral("heat/source"), rule(R"({"all": true})"), QJsonObject{{QStringLiteral("Q0"), QStringLiteral("ec.Qrh")}});
        stationary(m).props.insert(QStringLiteral("tolerance"), QStringLiteral("1e-10"));
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        const double E = 0.1 / 2e-3, Q = 5e7 * E * E, rise = Q * 4e-6 / (8 * 400);
        QVERIFY2(std::abs(global(sv.s(), QStringLiteral("ht.Tmax")) - (300 + rise)) < 1e-6 * rise, qPrintable(sv.s().globalText(QStringLiteral("ht.Tmax"))));
        QVERIFY(std::abs(valueAt(sv.s(), QStringLiteral("ht.T"), 0.5, 0.25, 0.25) - (300 + Q * 0.5e-3 * 1.5e-3 / 800)) < 1e-6 * rise);
        QVERIFY(std::abs(global(sv.s(), QStringLiteral("ht.P")) - Q * 2e-3 * 0.5e-3 * 0.5e-3) < 1e-6 * Q * 5e-10);
        QVERIFY2(sv.s().log.join(QLatin1Char('\n')).contains(QStringLiteral("iteration 2")), qPrintable(sv.s().log.join(QLatin1Char('\n'))));
    }

    // A slab held at its bottom, its top a film to 290 K: the 1D solution, a
    // parabola, order 2's exactly; the same top radiating, Picard's
    // iterations to the 1D solution of its quartic.
    void filmAndRadiation()
    {
        const double L = 0.2e-3, k = 1, Q = 1e9, h = 1e4, Text = 290;
        // T = 300 + a z - Q z²/(2k); -k T'(L) = h (T(L) - Text).
        const double a = (Q * L - h * (300 - Q * L * L / (2 * k) - Text)) / (k + h * L);
        const double top = 300 + a * L - Q * L * L / (2 * k);
        for (bool radiation : {false, true}) {
            Model m = model3d(0.25);
            addBox(m, QStringLiteral("slab"), 0, 0, 0, 1, 1, 0.2);
            const QString ht = addPhysics(m, QStringLiteral("heat"), 2, QStringLiteral("1[W/(m*K)]"));
            addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("slab"), QStringLiteral("bottom")), QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
            addFeature(m, ht, QStringLiteral("heat/source"), rule(R"({"all": true})"), QJsonObject{{QStringLiteral("Q0"), QStringLiteral("1e9[W/m^3]")}});
            if (!radiation)
                addFeature(m, ht, QStringLiteral("heat/flux"), ofSide(QStringLiteral("slab"), QStringLiteral("top")),
                           QJsonObject{{QStringLiteral("kind"), QStringLiteral("convective")}, {QStringLiteral("h"), QStringLiteral("1e4[W/(m^2*K)]")}, {QStringLiteral("Text"), QStringLiteral("290[K]")}});
            else
                addFeature(m, ht, QStringLiteral("heat/radiation"), ofSide(QStringLiteral("slab"), QStringLiteral("top")),
                           QJsonObject{{QStringLiteral("epsilon"), QStringLiteral("0.8")}, {QStringLiteral("Tamb"), QStringLiteral("290[K]")}});
            stationary(m).props.insert(QStringLiteral("tolerance"), QStringLiteral("1e-12"));
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const double got = valueAt(sv.s(), QStringLiteral("ht.T"), 0.5, 0.5, 0.2);
            if (!radiation) {
                QVERIFY2(std::abs(got - top) < 1e-8 * top, qPrintable(QStringLiteral("%1 K, the parabola's %2 K").arg(got, 0, 'g', 12).arg(top, 0, 'g', 12)));
                continue;
            }
            // The quartic's: -k T'(L) = εσ(T(L)⁴ - Tamb⁴), by bisection on a.
            const double sigma = 5.670374419e-8;
            double lo = -1e8, hi = 1e8;
            for (int i = 0; i < 200; ++i) {
                const double mid = 0.5 * (lo + hi), t = 300 + mid * L - Q * L * L / (2 * k);
                const double f = -k * (mid - Q * L / k) - 0.8 * sigma * (std::pow(t, 4) - std::pow(Text, 4));
                (f > 0 ? lo : hi) = mid;
            }
            const double want = 300 + lo * L - Q * L * L / (2 * k);
            QVERIFY2(std::abs(got - want) < 1e-7 * want, qPrintable(QStringLiteral("%1 K, the quartic's %2 K").arg(got, 0, 'g', 12).arg(want, 0, 'g', 12)));
            QVERIFY(sv.s().log.join(QLatin1Char('\n')).contains(QStringLiteral("Picard")));
        }
    }

    // A crystal's k along its axes (1; 2; 3) with no orientation: in a 3D
    // model its x2 - the plane's up, a wafer's normal - is z, its x3 - the
    // plane's along - y: the flux through a cube each way says so.
    void crystalTensor()
    {
        const char* axes[3] = {"x", "y", "z"};
        const char* low[3] = {"left", "front", "bottom"};
        const char* high[3] = {"right", "back", "top"};
        const double want[3] = {1, 3, 2};
        for (int a = 0; a < 3; ++a) {
            Model m = model3d(0.5);
            addBox(m, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
            addMaterial(m, QStringLiteral("Xtal"), QJsonObject{{QStringLiteral("k"), QStringLiteral("1; 2; 3")}}, rule(R"({"all": true})"));
            const QString ht = addPhysics(m, QStringLiteral("heat"), 1);
            addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("cube"), QString::fromLatin1(low[a])), QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
            addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("cube"), QString::fromLatin1(high[a])), QJsonObject{{QStringLiteral("T0"), QStringLiteral("310[K]")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const double q = valueAt(sv.s(), QStringLiteral("ht.q") + QString::fromLatin1(axes[a]), 0.5, 0.5, 0.5);
            QVERIFY2(std::abs(q + want[a] * 10 / 1e-3) < 1e-6 * want[a] * 1e4, qPrintable(QStringLiteral("q%1 = %2").arg(QString::fromLatin1(axes[a])).arg(q)));
        }
    }

    // Conjugate gradients with multigrid and a direct factorization agree;
    // each says what it was.
    void iterativeAndDirect()
    {
        std::vector<double> values[2];
        QString logs[2];
        int i = 0;
        for (const QString solver : {QStringLiteral("iterative"), QStringLiteral("direct")}) {
            Model m = model3d(0.25);
            addBox(m, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
            const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("2[W/(m*K)]"));
            addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("cube"), QStringLiteral("bottom")), QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
            addFeature(m, ht, QStringLiteral("heat/source"), rule(R"({"all": true})"), QJsonObject{{QStringLiteral("Q0"), QStringLiteral("1e9[W/m^3]*x/1[mm]")}});
            stationary(m).props.insert(QStringLiteral("solver"), solver);
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            values[i] = sv.s().fields().front().values;
            logs[i] = sv.s().log.join(QLatin1Char('\n'));
            ++i;
        }
        QVERIFY2(logs[0].contains(QStringLiteral("algebraic multigrid")), qPrintable(logs[0]));
        QVERIFY2(logs[1].contains(QStringLiteral("Cholesky")), qPrintable(logs[1]));
        QCOMPARE(values[0].size(), values[1].size());
        double worst = 0;
        for (std::size_t k = 0; k < values[0].size(); ++k) worst = std::max(worst, std::abs(values[0][k] - values[1][k]));
        QVERIFY2(worst < 1e-6, qPrintable(QString::number(worst)));
    }

    // A thin film heated from above in its middle, its back held: no degree
    // of freedom colder than its back - its prisms far wider than tall (a
    // linear prism's terms lumped; consistent, they undershoot), order 1.
    void thinLayersStayWithinBounds()
    {
        Model m = model3d(0.2);
        addBox(m, QStringLiteral("film"), 0, 0, 0, 2, 2, 0.02);
        addBox(m, QStringLiteral("pad"), 0.8, 0.8, 0.02, 0.4, 0.4, 0.002);
        const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1.4[W/(m*K)]"));
        addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("film"), QStringLiteral("bottom")), QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
        addFeature(m, ht, QStringLiteral("heat/source"), objects(QStringLiteral("pad")),
                   QJsonObject{{QStringLiteral("kind"), QStringLiteral("power")}, {QStringLiteral("P0"), QStringLiteral("100[mW]")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        QVERIFY(sv.mesh->maxStretch > 5);
        const std::vector<double>& v = sv.s().fields().front().values;
        const double low = *std::min_element(v.begin(), v.end());
        QVERIFY2(low >= 300 - 1e-9, qPrintable(QStringLiteral("%1 K, under the 300 K held").arg(low, 0, 'g', 12)));
        QVERIFY(global(sv.s(), QStringLiteral("ht.Tmax")) > 301);
        // (Its power spread through the pad: all of it made.)
        QVERIFY(std::abs(global(sv.s(), QStringLiteral("ht.P")) - 0.1) < 1e-12);
    }

    // A tall slab of one layer - its prisms far taller than wide -, its
    // bottom held at 300 K but for a patch at 400 K, its top insulated: every
    // temperature between the two (a linear prism's in-plane term lumped along
    // z; consistent, a plan node's two levels couple the wrong way, and its
    // top - free, over a bottom that varies - leaves the bounds), order 1.
    void tallPrismsStayWithinBounds()
    {
        Model m = model3d(0.08);
        addBox(m, QStringLiteral("slab"), 0, 0, 0, 1, 1, 3);
        addBox(m, QStringLiteral("pad"), 0.35, 0.35, -0.01, 0.3, 0.3, 0.01);
        Node z = m.make(QStringLiteral("mesh/zdistribution"));
        z.props.insert(QStringLiteral("selection"), objects(QStringLiteral("slab")));
        z.props.insert(QStringLiteral("count"), 1);
        m.add(m.mesh().tag, z);
        const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1[W/(m*K)]"));
        QJsonObject cold = ofSide(QStringLiteral("slab"), QStringLiteral("bottom"));
        QJsonArray pad;
        pad.append(QStringLiteral("pad"));
        cold.insert(QStringLiteral("except"), QJsonObject{{QStringLiteral("of"), pad}});
        addFeature(m, ht, QStringLiteral("heat/temperature"), cold, QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
        addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("pad"), QStringLiteral("bottom")), QJsonObject{{QStringLiteral("T0"), QStringLiteral("400[K]")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        QVERIFY2(sv.mesh->maxStretch > 10, qPrintable(QString::number(sv.mesh->maxStretch)));
        const std::vector<double>& v = sv.s().fields().front().values;
        const auto [low, high] = std::minmax_element(v.begin(), v.end());
        QVERIFY2(*low >= 300 - 1e-9 && *high <= 400 + 1e-9, qPrintable(QStringLiteral("%1 to %2 K").arg(*low, 0, 'g', 12).arg(*high, 0, 'g', 12)));
    }

    // A slice across x, y or z of a linear field: each cut's corners where
    // the plane is, each value the field's there; across z the plane's
    // layer's triangles, across x and y quadrilaterals as two.
    void slices()
    {
        Model m = model3d(0.4);
        addBox(m, QStringLiteral("left"), 0, 0, 0, 1, 1, 1);
        addBox(m, QStringLiteral("right"), 1, 0, 0, 1, 1, 1);
        const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("2[W/(m*K)]"));
        addFeature(m, ht, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"),
                   QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K] + 1e4[K/m]*(x + 2*y + 3*z)")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        const Expression e = Expression::compile(QStringLiteral("ht.T"), sv.s().pointScope());
        const double at[3] = {1.37, 0.41, 0.73};
        for (int axis = 0; axis < 3; ++axis) {
            const std::vector<SliceTriangle> cut = slice(sv.s(), e, axis, at[axis]);
            QVERIFY(cut.size() > 4);
            double area = 0;
            for (const SliceTriangle& t : cut) {
                for (int k = 0; k < 3; ++k) {
                    const Vec3& p = t.p[std::size_t(k)];
                    QVERIFY(std::abs((axis == 0 ? p.x : axis == 1 ? p.y : p.z) - at[axis]) < 1e-12);
                    const double want = 300 + 1e4 * 1e-3 * (p.x + 2 * p.y + 3 * p.z);
                    QVERIFY2(std::abs(t.value[std::size_t(k)] - want) < 1e-7, qPrintable(QStringLiteral("axis %1: %2, the field %3").arg(axis).arg(t.value[std::size_t(k)]).arg(want)));
                }
                area += 0.5 * (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).norm();
            }
            // (The plane's section: 2 x 1 across z, 1 x 1 across x, 2 x 1
            // across y.)
            const double section = axis == 0 ? 1 : 2;
            QVERIFY2(std::abs(area - section) < 1e-9, qPrintable(QStringLiteral("axis %1: %2").arg(axis).arg(area)));
        }
        // The nodes' values: a field's own, its values.
        const std::vector<double> nodal = nodalValues(sv.s(), e);
        QCOMPARE(nodal.size(), sv.mesh->nodes.size());
        for (std::size_t n = 0; n < nodal.size(); ++n) QCOMPARE(nodal[n], sv.s().fields().front().values[n]);
        const std::vector<double> twice = nodalValues(sv.s(), Expression::compile(QStringLiteral("2*ht.T"), sv.s().pointScope()));
        for (std::size_t n = 0; n < nodal.size(); ++n) QVERIFY(std::abs(twice[n] - 2 * nodal[n]) < 1e-9);
    }

    // What cannot be solved is said: a field held nowhere, a domain without
    // its material, a user's tensor of the plane, a step in time, a physics
    // of the plane's alone.
    // A model uniform in z in time, as at rest: at every output time every
    // degree of freedom is the 2D model's of its plan - its ρ Cp consistent
    // in the plane, as the triangles' are, its factor along z lumped - and
    // so is ht.Tmax: orders 1 and 2, BDF2's fixed steps, a source of t, a
    // held face, a film.
    void heatInTimeInvariantInZ()
    {
        for (int order : {1, 2}) {
            Model m3 = model3d(0.3);
            addBox(m3, QStringLiteral("blk1"), 0, 0, 0, 1, 1, 0.5);
            addBox(m3, QStringLiteral("blk2"), 1, 0, 0, 1, 1, 0.5);
            Model m2 = Model::blank();
            m2.component().props.insert(QStringLiteral("thickness"), QStringLiteral("0.5[mm]"));
            const QJsonObject propsA{{QStringLiteral("k"), QStringLiteral("1[W/(m*K)]")},
                                     {QStringLiteral("rho"), QStringLiteral("1000[kg/m^3]")},
                                     {QStringLiteral("Cp"), QStringLiteral("1000[J/(kg*K)]")}};
            const QJsonObject propsB{{QStringLiteral("k"), QStringLiteral("3[W/(m*K)]")},
                                     {QStringLiteral("rho"), QStringLiteral("2000[kg/m^3]")},
                                     {QStringLiteral("Cp"), QStringLiteral("700[J/(kg*K)]")}};
            addMaterial(m3, QStringLiteral("A"), propsA, objects(QStringLiteral("blk1")));
            addMaterial(m3, QStringLiteral("B"), propsB, objects(QStringLiteral("blk2")));
            addMaterial(m2, QStringLiteral("A"), propsA, boxRule2(-0.01, -0.01, 1.01, 1.01));
            addMaterial(m2, QStringLiteral("B"), propsB, boxRule2(0.99, -0.01, 2.01, 1.01));
            const QJsonObject left3 = ofSide(QStringLiteral("blk1"), QStringLiteral("left")), right3 = ofSide(QStringLiteral("blk2"), QStringLiteral("right"));
            const QJsonObject left2 = boxRule2(-0.01, -0.01, 0.01, 1.01), right2 = boxRule2(1.99, -0.01, 2.01, 1.01);
            const QJsonObject step{{QStringLiteral("times"), QStringLiteral("range(0, 0.1[s], 0.6[s])")},
                                   {QStringLiteral("stepping"), QStringLiteral("fixed")},
                                   {QStringLiteral("dt"), QStringLiteral("0.025[s]")},
                                   {QStringLiteral("solver"), QStringLiteral("direct")}};
            for (Model* m : {&m3, &m2}) {
                const bool three = m == &m3;
                const QString ht = addPhysics(*m, QStringLiteral("heat"), order);
                addFeature(*m, ht, QStringLiteral("heat/temperature"), three ? left3 : left2, QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
                addFeature(*m, ht, QStringLiteral("heat/flux"), three ? right3 : right2,
                           QJsonObject{{QStringLiteral("kind"), QStringLiteral("convective")}, {QStringLiteral("h"), QStringLiteral("5e3[W/(m^2*K)]")}, {QStringLiteral("Text"), QStringLiteral("320[K]")}});
                addFeature(*m, ht, QStringLiteral("heat/source"), three ? objects(QStringLiteral("blk1")) : boxRule2(-0.01, -0.01, 1.01, 1.01),
                           QJsonObject{{QStringLiteral("Q0"), QStringLiteral("1e8[W/m^3]*t/1[s]")}});
                inTime(*m, step);
            }
            const Solved s3 = solve(m3);
            QVERIFY2(s3.error.isEmpty(), qPrintable(s3.error));
            const StudyResult r2 = solveStudy(m2, evaluateParameters(m2), s3.mesh->plan, QString());
            QVERIFY2(r2.error.isEmpty() && r2.solution, qPrintable(r2.error));
            Solution& two = *r2.solution;
            Solution3& three = s3.s();
            QVERIFY(three.timeDependent);
            QCOMPARE(three.snapshotCount(), 7);
            QCOMPARE(two.snapshotCount(), 7);
            for (int i = 0; i < 7; ++i) {
                two.select(i);
                three.select(i);
                QCOMPARE(three.time(), two.time());
                const Field3& f3 = three.fields().front();
                const Field& f2 = two.fields().front();
                double worst = 0, scale = 0;
                for (int dof = 0; dof < f3.space->size(); ++dof) {
                    const double v2 = f2.values[std::size_t(f3.space->origin(dof).first)];
                    worst = std::max(worst, std::abs(f3.values[std::size_t(dof)] - v2));
                    scale = std::max(scale, std::abs(v2));
                }
                QVERIFY2(worst <= 1e-9 * scale, qPrintable(QStringLiteral("order %1, t = %2 s: %3 K off").arg(order).arg(three.time()).arg(worst)));
                const double a = global(three, QStringLiteral("ht.Tmax")), b = two.global(QStringLiteral("ht.Tmax")).value_or(std::nan(""));
                QVERIFY2(std::abs(a - b) <= 1e-9 * b, qPrintable(QStringLiteral("order %1, t = %2 s: ht.Tmax 3D %3, 2D %4").arg(order).arg(three.time()).arg(a, 0, 'g', 12).arg(b, 0, 'g', 12)));
            }
            // (The series of a global, as a graph over time reads it.)
            Dim dim;
            QString why;
            const std::vector<double> tmax = three.globalSeries(QStringLiteral("ht.Tmax"), &dim, &why);
            QVERIFY2(tmax.size() == 7 && dim == Dim::temperature(), qPrintable(why));
            QVERIFY(tmax.back() > tmax.front());
        }
    }

    // An insulated block heated by 2 mW from t = 0: its heat ∫ ρ Cp (T - T0)
    // dV is P t at every output time to rounding - backward Euler and BDF2
    // on adaptive steps and the implicit midpoint rule on fixed ones each
    // hold a straight line exactly -, at order 1 and 2; ht.P the 2 mW.
    void energyInTime()
    {
        struct Method {
            const char* method;
            const char* stepping;
        };
        for (const Method& how : {Method{"euler", "adaptive"}, Method{"bdf2", "adaptive"}, Method{"midpoint", "fixed"}})
            for (int order : {1, 2}) {
                Model m = model3d(0.4);
                addBox(m, QStringLiteral("heater"), 0, 0, 0, 1, 1, 1);
                addBox(m, QStringLiteral("body"), 1, 0, 0, 1, 1, 1);
                addMaterial(m, QStringLiteral("A"),
                            QJsonObject{{QStringLiteral("k"), QStringLiteral("2[W/(m*K)]")},
                                        {QStringLiteral("rho"), QStringLiteral("1000[kg/m^3]")},
                                        {QStringLiteral("Cp"), QStringLiteral("1000[J/(kg*K)]")}},
                            rule(R"({"all": true})"));
                const QString ht = addPhysics(m, QStringLiteral("heat"), order);
                addFeature(m, ht, QStringLiteral("heat/source"), objects(QStringLiteral("heater")),
                           QJsonObject{{QStringLiteral("kind"), QStringLiteral("power")}, {QStringLiteral("P0"), QStringLiteral("2[mW]")}});
                QJsonObject step{{QStringLiteral("times"), QStringLiteral("range(0, 0.25[s], 1[s])")},
                                 {QStringLiteral("method"), QString::fromLatin1(how.method)},
                                 {QStringLiteral("stepping"), QString::fromLatin1(how.stepping)},
                                 {QStringLiteral("solver"), QStringLiteral("direct")}};
                if (QLatin1String(how.stepping) == QLatin1String("fixed")) step.insert(QStringLiteral("dt"), QStringLiteral("0.05[s]"));
                inTime(m, step);
                const Solved sv = solve(m);
                QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
                Solution3& s = sv.s();
                QCOMPARE(s.snapshotCount(), 5);
                // (At the start too, before a step: what its source makes.)
                s.select(0);
                QVERIFY2(std::abs(global(s, QStringLiteral("ht.P")) - 2e-3) < 1e-12, qPrintable(s.globalText(QStringLiteral("ht.P"))));
                for (int i = 1; i < s.snapshotCount(); ++i) {
                    s.select(i);
                    const double heat = integral(s, QStringLiteral("1e6[J/(m^3*K)]*(ht.T - 293.15[K])")), made = 2e-3 * s.time();
                    QVERIFY2(std::abs(heat - made) <= 1e-9 * made,
                             qPrintable(QStringLiteral("%1, order %2, t = %3 s: %4 J, P t %5 J").arg(how.method).arg(order).arg(s.time()).arg(heat, 0, 'g', 12).arg(made, 0, 'g', 12)));
                    QVERIFY2(std::abs(global(s, QStringLiteral("ht.P")) - 2e-3) < 1e-12, qPrintable(s.globalText(QStringLiteral("ht.P"))));
                }
            }
    }

    // A slab 1 mm thick, its top held at 310 K from t = 0, insulated
    // elsewhere, from 300 K: its bottom's temperature is the series T1 + (T0 -
    // T1) Σ 4(-1)^n/((2n+1)π) exp(-(2n+1)² π² α t / 4L²) (Carslaw and
    // Jaeger's slab), adaptive BDF2 at order 2 within 0.005 K of its 10 K, order
    // 1 within 0.05 K.
    void slabInTime()
    {
        const double L = 1e-3, alpha = 1e-6, T0 = 300, T1 = 310;
        auto exact = [&](double t) {
            double sum = 0;
            for (int n = 0; n < 60; ++n) sum += 4 * (n % 2 ? -1 : 1) / ((2 * n + 1) * M_PI) * std::exp(-std::pow((2 * n + 1) * M_PI, 2) * alpha * t / (4 * L * L));
            return T1 + (T0 - T1) * sum;
        };
        for (int order : {1, 2}) {
            Model m = model3d(0.1);
            addBox(m, QStringLiteral("slab"), 0, 0, 0, 0.3, 0.3, 1);
            const QString ht = addPhysics(m, QStringLiteral("heat"), order, QStringLiteral("1[W/(m*K)]"));
            for (Node& c : m.find(ht)->children)
                if (c.type == QLatin1String("heat/solid")) {
                    c.props.insert(QStringLiteral("rho"), QStringLiteral("1000[kg/m^3]"));
                    c.props.insert(QStringLiteral("Cp"), QStringLiteral("1000[J/(kg*K)]"));
                } else if (c.type == QLatin1String("heat/init")) {
                    c.props.insert(QStringLiteral("T0"), QStringLiteral("300[K]"));
                }
            addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("slab"), QStringLiteral("top")), QJsonObject{{QStringLiteral("T0"), QStringLiteral("310[K]")}});
            inTime(m, QJsonObject{{QStringLiteral("times"), QStringLiteral("0, 0.05[s], 0.1[s], 0.2[s], 0.4[s]")},
                                  {QStringLiteral("rtol"), QStringLiteral("1e-4")},
                                  {QStringLiteral("solver"), QStringLiteral("direct")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            Solution3& s = sv.s();
            QCOMPARE(s.snapshotCount(), 5);
            double worst = 0;
            QString said;
            for (int i = 1; i < s.snapshotCount(); ++i) {
                s.select(i);
                const double t = s.time(), got = valueAt(s, QStringLiteral("ht.T"), 0.15, 0.15, 0), want = exact(t);
                said += QStringLiteral(" t = %1 s: %2 K, the series %3 K;").arg(t).arg(got, 0, 'f', 5).arg(want, 0, 'f', 5);
                worst = std::max(worst, std::abs(got - want));
            }
            QVERIFY2(worst < (order == 2 ? 0.005 : 0.05), qPrintable(QStringLiteral("order %1:%2").arg(order).arg(said)));
        }
    }

    // A slab of one layer of tall prisms, its top held 10 K above where it
    // starts, its steps short (1 ms against its 4 s): no temperature under its
    // start's or over the top's, at order 1 - the capacity's factor along z
    // lumped. (Consistent, the bottom's row couples the two levels the wrong
    // way, m/h - k/hz > 0: the bottom falls some 5 K at the first step.)
    void tallPrismsInTime()
    {
        Model m = model3d(0.25);
        addBox(m, QStringLiteral("slab"), 0, 0, 0, 0.5, 0.5, 2);
        Node zd = m.make(QStringLiteral("mesh/zdistribution"));
        zd.props.insert(QStringLiteral("selection"), rule(R"({"all": true})"));
        zd.props.insert(QStringLiteral("count"), 1);
        m.add(m.mesh().tag, zd);
        const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1[W/(m*K)]"));
        for (Node& c : m.find(ht)->children)
            if (c.type == QLatin1String("heat/solid")) {
                c.props.insert(QStringLiteral("rho"), QStringLiteral("1000[kg/m^3]"));
                c.props.insert(QStringLiteral("Cp"), QStringLiteral("1000[J/(kg*K)]"));
            } else if (c.type == QLatin1String("heat/init")) {
                c.props.insert(QStringLiteral("T0"), QStringLiteral("300[K]"));
            }
        addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("slab"), QStringLiteral("top")), QJsonObject{{QStringLiteral("T0"), QStringLiteral("310[K]")}});
        inTime(m, QJsonObject{{QStringLiteral("times"), QStringLiteral("0, 1[ms], 2[ms]")},
                              {QStringLiteral("stepping"), QStringLiteral("fixed")},
                              {QStringLiteral("dt"), QStringLiteral("1[ms]")},
                              {QStringLiteral("solver"), QStringLiteral("direct")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        QCOMPARE(int(sv.mesh->heights.size()), 2);
        Solution3& s = sv.s();
        for (int i = 1; i < s.snapshotCount(); ++i) {
            s.select(i);
            const Field3& f = s.fields().front();
            const auto [lo, hi] = std::minmax_element(f.values.begin(), f.values.end());
            QVERIFY2(*lo >= 300 - 1e-9 && *hi <= 310 + 1e-9, qPrintable(QStringLiteral("t = %1 s: %2 to %3 K").arg(s.time()).arg(*lo, 0, 'f', 6).arg(*hi, 0, 'f', 6)));
        }
    }

    // A bar's terminal driven in time, V0 = 1 mV t/s, its current a step's at
    // rest at each output time: ec.I_a V/R and ec.P V²/R exactly; its Joule
    // heat warming it in turn, ∫ ρ Cp (T - T0) dV is ∫ V²/R dt = V'² t³ / 3R
    // within 0.1 % on BDF2's fixed steps of 2.5 ms (0.35 % at 10 ms: its error, as h²).
    void terminalInTime()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("bar"), 0, 0, 0, 4, 1, 1);
        addMaterial(m, QStringLiteral("metal"),
                    QJsonObject{{QStringLiteral("sigma"), QStringLiteral("1e6[S/m]")},
                                {QStringLiteral("k"), QStringLiteral("100[W/(m*K)]")},
                                {QStringLiteral("rho"), QStringLiteral("1000[kg/m^3]")},
                                {QStringLiteral("Cp"), QStringLiteral("1000[J/(kg*K)]")}},
                    rule(R"({"all": true})"));
        const QString ec = addPhysics(m, QStringLiteral("currents"), 1);
        addFeature(m, ec, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar"), QStringLiteral("left")),
                   QJsonObject{{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), QStringLiteral("1[mV]*t/1[s]")}});
        addFeature(m, ec, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar"), QStringLiteral("right")));
        const QString ht = addPhysics(m, QStringLiteral("heat"), 1);
        addFeature(m, ht, QStringLiteral("heat/source"), rule(R"({"all": true})"), QJsonObject{{QStringLiteral("Q0"), QStringLiteral("ec.Qrh")}});
        inTime(m, QJsonObject{{QStringLiteral("times"), QStringLiteral("range(0, 0.25[s], 1[s])")},
                              {QStringLiteral("stepping"), QStringLiteral("fixed")},
                              {QStringLiteral("dt"), QStringLiteral("2.5[ms]")},
                              {QStringLiteral("solver"), QStringLiteral("direct")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        Solution3& s = sv.s();
        QCOMPARE(s.snapshotCount(), 5);
        const double R = 4e-3 / (1e6 * 1e-6);
        for (int i = 1; i < s.snapshotCount(); ++i) {
            s.select(i);
            const double t = s.time(), V = 1e-3 * t;
            QVERIFY2(std::abs(global(s, QStringLiteral("ec.V_a")) - V) <= 1e-12, qPrintable(s.globalText(QStringLiteral("ec.V_a"))));
            QVERIFY2(std::abs(std::abs(global(s, QStringLiteral("ec.I_a"))) - V / R) <= 1e-6 * V / R, qPrintable(s.globalText(QStringLiteral("ec.I_a"))));
            QVERIFY2(std::abs(global(s, QStringLiteral("ec.P")) - V * V / R) <= 1e-6 * V * V / R, qPrintable(s.globalText(QStringLiteral("ec.P"))));
            const double heat = integral(s, QStringLiteral("1e6[J/(m^3*K)]*(ht.T - 293.15[K])")), made = 1e-6 * t * t * t / (3 * R);
            QVERIFY2(std::abs(heat - made) <= 1e-3 * made, qPrintable(QStringLiteral("t = %1 s: %2 J, ∫ V²/R dt %3 J").arg(t).arg(heat, 0, 'g', 8).arg(made, 0, 'g', 8)));
        }
    }

    // A Parametric Sweep: a bar's length L swept, each point's geometry and
    // mesh its own, ec.R L/(σA) at each (order 1's linear field, exactly);
    // its conductivity swept, one mesh for all; a point refused after the
    // first stops the sweep there, the point before kept and where said.
    void aSweep()
    {
        auto bar = [](const QString& sweepOf, const QString& values, const QString& unit) {
            Model m = model3d(0.5);
            QList<Model::Parameter> ps = m.parameters();
            ps << Model::Parameter{QStringLiteral("L"), QStringLiteral("4[mm]"), QString()}
               << Model::Parameter{QStringLiteral("s"), QStringLiteral("1e6[S/m]"), QString()};
            m.setParameters(ps);
            Node n = m.make(QStringLiteral("box"));
            n.label = QStringLiteral("bar");
            n.props.insert(QStringLiteral("position"), pairOf(0, 0));
            n.props.insert(QStringLiteral("size"), QJsonArray{QStringLiteral("L"), QStringLiteral("1")});
            n.props.insert(QStringLiteral("z"), QStringLiteral("0"));
            n.props.insert(QStringLiteral("height"), QStringLiteral("1"));
            m.add(m.geometry().tag, n);
            const QString ec = addPhysics(m, QStringLiteral("currents"), 1, QStringLiteral("s"));
            addFeature(m, ec, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar"), QStringLiteral("left")), QJsonObject{{QStringLiteral("name"), QStringLiteral("a")}});
            addFeature(m, ec, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar"), QStringLiteral("right")));
            for (Node& c : m.root.children)
                if (c.type == QLatin1String("study")) {
                    Node sw = m.make(QStringLiteral("sweep"));
                    QJsonArray rows;
                    rows.append(QJsonArray{sweepOf, values, unit});
                    sw.props.insert(QStringLiteral("parameters"), rows);
                    c.children.push_back(sw);
                }
            return m;
        };
        // The length: a mesh each.
        Model m = bar(QStringLiteral("L"), QStringLiteral("2, 4, 6"), QStringLiteral("mm"));
        StudyRun3 run = runStudy3(m, evaluateParameters(m), nullptr, nullptr);
        QVERIFY2(run.ok(), qPrintable(run.error));
        QCOMPARE(run.swept, QStringList{QStringLiteral("L")});
        QCOMPARE(int(run.solutions.size()), 3);
        for (int k = 0; k < 3; ++k) {
            const Solution3& s = *run.solutions[std::size_t(k)];
            const double L = 2e-3 * (k + 1), want = L / (1e6 * 1e-6);
            QVERIFY2(std::abs(global(s, QStringLiteral("ec.R")) - want) <= 1e-9 * want, qPrintable(s.label + QStringLiteral(": ") + s.globalText(QStringLiteral("ec.R"))));
            QCOMPARE(s.label, QStringLiteral("L = %1 mm").arg(2 * (k + 1)));
            QCOMPARE(s.sweptValues.value(QStringLiteral("L")), L);
        }
        QVERIFY(run.solutions[0]->meshPtr() != run.solutions[1]->meshPtr());
        // The conductivity: one mesh.
        m = bar(QStringLiteral("s"), QStringLiteral("1e6, 2e6"), QStringLiteral("S/m"));
        run = runStudy3(m, evaluateParameters(m), nullptr, nullptr);
        QVERIFY2(run.ok(), qPrintable(run.error));
        QCOMPARE(int(run.solutions.size()), 2);
        QVERIFY(run.solutions[0]->meshPtr() == run.solutions[1]->meshPtr());
        QVERIFY(std::abs(global(*run.solutions[1], QStringLiteral("ec.R")) - 2e-3) <= 1e-12);
        // A point refused: the sweep stopped there.
        m = bar(QStringLiteral("s"), QStringLiteral("1e6, 0"), QStringLiteral("S/m"));
        run = runStudy3(m, evaluateParameters(m), nullptr, nullptr);
        QVERIFY2(run.ok() && run.solutions.size() == 1, qPrintable(run.error));
        QVERIFY2(run.stopped.contains(QStringLiteral("point 2 of 2")) && run.stopped.contains(QStringLiteral("its point before kept")), qPrintable(run.stopped));
    }

    // Derived values of a 3D solution, on a linear temperature held on two
    // boxes' faces (order 1's exactly): over a domain ∫ 1 its volume, ∫ x its
    // centroid's x times it, the average of T T at its centroid; T's maximum
    // and minimum over both its corners' (ht.Tmax, ht.Tmin); T at a point; over
    // a face ∫ 1 its area, T's average its middle's; over an edge ∫ 1 its
    // length, T's average its middle's; a global of two named. Then a
    // cylinder's surface 2πr(r + h) on order 2's arcs within 1e-4, a bar's
    // ∫ ec.Qrh dV its ec.P, and an insulated block's average T over its
    // output times T0 + P t/(ρ Cp V) - each instance, its label.
    void derivedValues()
    {
        auto value = [](const DerivedResult& r) {
            return r.values.isEmpty() || !r.error.isEmpty() ? std::nan("") : r.values.front().value;
        };
        auto node = [](Model& m, const QString& type, const QJsonObject& props) {
            Node n = m.make(type);
            for (auto it = props.begin(); it != props.end(); ++it) n.props.insert(it.key(), it.value());
            return n;
        };
        Model m = model3d(0.4);
        addBox(m, QStringLiteral("left"), 0, 0, 0, 1, 1, 1);
        addBox(m, QStringLiteral("right"), 1, 0, 0, 1, 1, 1);
        const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("2[W/(m*K)]"));
        addFeature(m, ht, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"),
                   QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K] + 1e4[K/m]*(x + 2*y + 3*z)")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        const Solution3& s = sv.s();
        const QJsonObject left = objects(QStringLiteral("left")), all = rule(R"({"all": true})");
        struct Case {
            const char* what;
            QString type;
            QJsonObject props;
            double want;
            double tolerance;
        };
        const QJsonObject edgeX = boxRule(-0.01, -0.01, -0.01, 2.01, 0.01, 0.01);
        const QList<Case> cases = {
            {"the left box's volume", QStringLiteral("integral"), {{QStringLiteral("selection"), left}, {QStringLiteral("expression"), QStringLiteral("1")}}, 1e-9, 1e-12},
            {"∫ x dV over both", QStringLiteral("integral"), {{QStringLiteral("selection"), all}, {QStringLiteral("expression"), QStringLiteral("x")}}, 2e-12, 1e-10},
            {"T's average over the left box", QStringLiteral("average"), {{QStringLiteral("selection"), left}, {QStringLiteral("expression"), QStringLiteral("ht.T")}}, 330, 1e-10},
            {"T's maximum", QStringLiteral("maximum"), {{QStringLiteral("selection"), all}, {QStringLiteral("expression"), QStringLiteral("ht.T")}}, 370, 1e-10},
            {"T's minimum", QStringLiteral("minimum"), {{QStringLiteral("selection"), all}, {QStringLiteral("expression"), QStringLiteral("ht.T")}}, 300, 1e-10},
            {"the left box's top's area", QStringLiteral("integral"),
             {{QStringLiteral("level"), QStringLiteral("boundary")}, {QStringLiteral("selection"), ofSide(QStringLiteral("left"), QStringLiteral("top"))}, {QStringLiteral("expression"), QStringLiteral("1")}}, 1e-6, 1e-12},
            {"T's average over the right face", QStringLiteral("average"),
             {{QStringLiteral("level"), QStringLiteral("boundary")}, {QStringLiteral("selection"), ofSide(QStringLiteral("right"), QStringLiteral("right"))}, {QStringLiteral("expression"), QStringLiteral("ht.T")}}, 345, 1e-10},
            {"the edge along x's length", QStringLiteral("integral"),
             {{QStringLiteral("level"), QStringLiteral("edge")}, {QStringLiteral("selection"), edgeX}, {QStringLiteral("expression"), QStringLiteral("1")}}, 2e-3, 1e-12},
            {"T's average along it", QStringLiteral("average"),
             {{QStringLiteral("level"), QStringLiteral("edge")}, {QStringLiteral("selection"), edgeX}, {QStringLiteral("expression"), QStringLiteral("ht.T")}}, 310, 1e-10},
        };
        for (const Case& c : cases) {
            Model mm = m;
            const DerivedResult r = evaluateDerived3(s, node(mm, c.type, c.props));
            const double got = value(r);
            QVERIFY2(std::abs(got - c.want) <= c.tolerance * std::abs(c.want),
                     qPrintable(QStringLiteral("%1: %2, not %3 (%4)").arg(QString::fromUtf8(c.what)).arg(got, 0, 'g', 15).arg(c.want).arg(r.error)));
        }
        QCOMPARE(value(evaluateDerived3(s, node(m, QStringLiteral("maximum"), {{QStringLiteral("selection"), all}, {QStringLiteral("expression"), QStringLiteral("ht.T")}}))),
                 global(s, QStringLiteral("ht.Tmax")));
        // At a point; at points of the geometry (its corners, its T's).
        QJsonArray at;
        // (A coordinate with a unit: 250 µm is 0.25 of the geometry's mm.)
        at.append(QJsonArray{QStringLiteral("250[um]"), QStringLiteral("0.5"), QStringLiteral("0.75")});
        at.append(QJsonArray{QStringLiteral("5"), QStringLiteral("5"), QStringLiteral("5")});
        DerivedResult r = evaluateDerived3(s, node(m, QStringLiteral("pointeval"), {{QStringLiteral("where"), QStringLiteral("coordinates")}, {QStringLiteral("coordinates3"), at}, {QStringLiteral("expression"), QStringLiteral("ht.T")}, {QStringLiteral("unit"), QStringLiteral("degC")}}));
        QVERIFY2(r.values.size() == 1 && std::abs(r.values.front().value - 335) < 1e-9, qPrintable(r.error));
        QVERIFY2(r.values.front().name == QStringLiteral("ht.T at (0.25, 0.5, 0.75)") && r.values.front().text.endsWith(QStringLiteral("degC")),
                 qPrintable(r.values.front().name + QStringLiteral(" = ") + r.values.front().text));
        QVERIFY2(r.error.contains(QStringLiteral("(5, 5, 5)")), qPrintable(r.error));
        r = evaluateDerived3(s, node(m, QStringLiteral("global"), {{QStringLiteral("expression"), QStringLiteral("dT = ht.Tmax - ht.Tmin; ht.Tmax")}}));
        QVERIFY2(r.values.size() == 2 && r.values[0].name == QStringLiteral("dT") && std::abs(r.values[0].value - 70) < 1e-9, qPrintable(r.error));
        r = evaluateDerived3(s, node(m, QStringLiteral("global"), {{QStringLiteral("expression"), QStringLiteral("ht.T")}}));
        QVERIFY2(r.error.contains(QStringLiteral("varies over the model")), qPrintable(r.error));
        // A cylinder's surface on order 2's arcs.
        {
            Model c = model3d(0.1);
            addCylinder(c, QStringLiteral("cyl"), 0.3, 0.5);
            const QString h2 = addPhysics(c, QStringLiteral("heat"), 2, QStringLiteral("1[W/(m*K)]"));
            addFeature(c, h2, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"));
            const Solved cv = solve(c);
            QVERIFY2(cv.error.isEmpty(), qPrintable(cv.error));
            const double area = value(evaluateDerived3(cv.s(), node(c, QStringLiteral("integral"),
                {{QStringLiteral("level"), QStringLiteral("boundary")}, {QStringLiteral("selection"), rule(R"({"exterior": true})")}, {QStringLiteral("expression"), QStringLiteral("1")}})));
            const double want = 2 * M_PI * 0.3e-3 * (0.3e-3 + 0.5e-3);
            QVERIFY2(std::abs(area - want) < 1e-4 * want, qPrintable(QStringLiteral("%1 m², 2πr(r + h) %2 m²").arg(area).arg(want)));
        }
        // A bar's Joule heat.
        {
            Model b = model3d(0.5);
            addBox(b, QStringLiteral("bar"), 0, 0, 0, 4, 1, 1);
            const QString ec = addPhysics(b, QStringLiteral("currents"), 1, QStringLiteral("1e6[S/m]"));
            addFeature(b, ec, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar"), QStringLiteral("left")),
                       QJsonObject{{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), QStringLiteral("1[mV]")}});
            addFeature(b, ec, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar"), QStringLiteral("right")));
            const Solved bv = solve(b);
            QVERIFY2(bv.error.isEmpty(), qPrintable(bv.error));
            const double joule = value(evaluateDerived3(bv.s(), node(b, QStringLiteral("integral"), {{QStringLiteral("selection"), rule(R"({"all": true})")}, {QStringLiteral("expression"), QStringLiteral("ec.Qrh")}})));
            QVERIFY2(std::abs(joule - global(bv.s(), QStringLiteral("ec.P"))) <= 1e-9 * joule, qPrintable(QStringLiteral("%1 W, ec.P %2 W").arg(joule).arg(global(bv.s(), QStringLiteral("ec.P")))));
        }
        // An insulated block's average temperature at each output time.
        {
            Model t = model3d(0.5);
            addBox(t, QStringLiteral("block"), 0, 0, 0, 2, 1, 1);
            addMaterial(t, QStringLiteral("A"),
                        QJsonObject{{QStringLiteral("k"), QStringLiteral("2[W/(m*K)]")}, {QStringLiteral("rho"), QStringLiteral("1000[kg/m^3]")}, {QStringLiteral("Cp"), QStringLiteral("1000[J/(kg*K)]")}},
                        rule(R"({"all": true})"));
            const QString h = addPhysics(t, QStringLiteral("heat"), 1);
            addFeature(t, h, QStringLiteral("heat/source"), rule(R"({"all": true})"),
                       QJsonObject{{QStringLiteral("kind"), QStringLiteral("power")}, {QStringLiteral("P0"), QStringLiteral("2[mW]")}});
            inTime(t, QJsonObject{{QStringLiteral("times"), QStringLiteral("range(0, 0.5[s], 1[s])")}, {QStringLiteral("solver"), QStringLiteral("direct")}});
            const Solved tv = solve(t);
            QVERIFY2(tv.error.isEmpty(), qPrintable(tv.error));
            const Solutions3 set{tv.r.solution};
            const std::vector<Instance3> each = instances3(set, true);
            QCOMPARE(int(each.size()), 3);
            const Node avg = node(t, QStringLiteral("average"), {{QStringLiteral("selection"), rule(R"({"all": true})")}, {QStringLiteral("expression"), QStringLiteral("ht.T")}});
            for (const Instance3& in : each) {
                Solution3* at = select3(set, in);
                QVERIFY(at);
                const double want = 293.15 + 2e-3 * in.time / (1e6 * 2e-9);
                const double got = value(evaluateDerived3(*at, avg));
                QVERIFY2(std::abs(got - want) < 1e-9, qPrintable(QStringLiteral("%1: %2 K, not %3 K").arg(in.label).arg(got, 0, 'g', 12).arg(want, 0, 'g', 12)));
            }
            QCOMPARE(each[1].label, QStringLiteral("t = 500 ms"));
            const std::vector<Instance3> asked = instances3(set, false, 0, QStringLiteral("0.4[s]"));
            QVERIFY(asked.size() == 1 && asked.front().snapshot == 1 && asked.front().label.isEmpty());
        }
    }

    // Isosurfaces and slices of linear temperatures held on a box's faces
    // (order 1's exactly): T of z alone at 304 K is the plane z = 0.4 mm - in a
    // layer, its tetrahedra cut across, not on their floors -, its area the
    // box's 2 mm²; an oblique field's isosurface has the level at
    // each corner; a sphere's (the distance from the box's middle at 0.3 mm)
    // is closed - every edge two triangles' -, as the prisms beside each
    // other split their walls alike; a slice through the middle of normal
    // (1, 2, 3) has each corner on its plane, the field's value there; a
    // uniform field's arrows each its vector, one in each cell.
    void isosurfacesAndSlices()
    {
        auto heated = [](const QString& T0) {
            Model m = model3d(0.25);
            addBox(m, QStringLiteral("box"), 0, 0, 0, 2, 1, 1);
            const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1[W/(m*K)]"));
            addFeature(m, ht, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"), QJsonObject{{QStringLiteral("T0"), T0}});
            return m;
        };
        auto area = [](const std::array<Vec3, 3>& p) {
            const Vec3 a{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z}, b{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
            const Vec3 c{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
            return 0.5 * std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
        };
        {
            const Solved sv = solve(heated(QStringLiteral("300[K] + 1e4[K/m]*z")));
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const std::vector<double> nodal = nodalValues(sv.s(), Expression::compile(QStringLiteral("ht.T"), sv.s().pointScope()));
            const std::vector<IsoTriangle> iso = isosurfaces(sv.s(), nodal, {304});
            QVERIFY(!iso.empty());
            double sum = 0, worst = 0;
            for (const IsoTriangle& t : iso) {
                sum += area(t.p);
                for (const Vec3& v : t.p) worst = std::max(worst, std::abs(v.z - 0.4));
            }
            QVERIFY2(worst < 1e-9 && std::abs(sum - 2) < 1e-9, qPrintable(QStringLiteral("%1 mm off the plane, %2 mm²").arg(worst).arg(sum, 0, 'g', 12)));
            // (A level past the field's: none.)
            QVERIFY(isosurfaces(sv.s(), nodal, {400}).empty());
        }
        {
            auto T = [](const Vec3& v) { return 300 + 10 * (v.x + 2 * v.y + 3 * v.z); };   // v in mm
            const Solved sv = solve(heated(QStringLiteral("300[K] + 1e4[K/m]*(x + 2*y + 3*z)")));
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const Expression e = Expression::compile(QStringLiteral("ht.T"), sv.s().pointScope());
            const std::vector<double> nodal = nodalValues(sv.s(), e);
            const std::vector<IsoTriangle> iso = isosurfaces(sv.s(), nodal, {320, 340});
            QVERIFY(!iso.empty());
            double worst = 0;
            for (const IsoTriangle& t : iso)
                for (const Vec3& v : t.p) worst = std::max(worst, std::abs(T(v) - (t.level == 0 ? 320 : 340)));
            QVERIFY2(worst < 1e-9, qPrintable(QStringLiteral("%1 K off its level").arg(worst)));
            // A slice through the middle of normal (1, 2, 3).
            const Vec3 o{1, 0.5, 0.5}, n{1, 2, 3};
            const std::vector<SliceTriangle> cut = slicePlane(sv.s(), e, o, n);
            QVERIFY(!cut.empty());
            double off = 0, wrong = 0;
            for (const SliceTriangle& st : cut)
                for (int k = 0; k < 3; ++k) {
                    const Vec3& v = st.p[std::size_t(k)];
                    off = std::max(off, std::abs((v.x - o.x) * n.x + (v.y - o.y) * n.y + (v.z - o.z) * n.z) / std::sqrt(14.0));
                    wrong = std::max(wrong, std::abs(st.value[std::size_t(k)] - T(v)));
                }
            QVERIFY2(off < 1e-9 && wrong < 1e-9, qPrintable(QStringLiteral("%1 mm off the plane, %2 K off").arg(off).arg(wrong)));
        }
        {
            const Solved sv = solve(heated(QStringLiteral("300[K] + 1e6[K/m]*sqrt((x - 1[mm])^2 + (y - 0.5[mm])^2 + (z - 0.5[mm])^2)")));
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            // (The distance itself at the nodes: what is held on the faces,
            // the field inside not.)
            const Expression r = Expression::compile(QStringLiteral("sqrt((x - 1[mm])^2 + (y - 0.5[mm])^2 + (z - 0.5[mm])^2)"), sv.s().pointScope());
            const std::vector<IsoTriangle> sphere = isosurfaces(sv.s(), nodalValues(sv.s(), r), {0.3e-3});
            QVERIFY(sphere.size() > 50);
            // Each edge, by its two corners rounded, counted.
            std::map<std::pair<std::array<long long, 3>, std::array<long long, 3>>, int> edges;
            auto key = [](const Vec3& v) { return std::array<long long, 3>{std::llround(v.x * 1e9), std::llround(v.y * 1e9), std::llround(v.z * 1e9)}; };
            for (const IsoTriangle& t : sphere)
                for (int k = 0; k < 3; ++k) {
                    auto a = key(t.p[std::size_t(k)]), b = key(t.p[std::size_t((k + 1) % 3)]);
                    if (b < a) std::swap(a, b);
                    if (a != b) ++edges[{a, b}];
                }
            int open = 0;
            for (const auto& [e, n] : edges) open += n != 2 ? 1 : 0;
            QVERIFY2(open == 0, qPrintable(QStringLiteral("%1 of %2 edges not two triangles'").arg(open).arg(edges.size())));
        }
        {
            Model m = model3d(0.5);
            addBox(m, QStringLiteral("box"), 0, 0, 0, 2, 1, 1);
            const QString es = addPhysics(m, QStringLiteral("electrostatics"), 1, QStringLiteral("1"));
            addFeature(m, es, QStringLiteral("electrostatics/potential"), rule(R"({"exterior": true})"), QJsonObject{{QStringLiteral("V0"), QStringLiteral("1e3[V/m]*x")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const Scope& sc = sv.s().scope();
            const std::vector<Arrow3> arrows = arrows3(sv.s(), {Expression::compile(QStringLiteral("es.Ex"), sc), Expression::compile(QStringLiteral("es.Ey"), sc),
                                                                Expression::compile(QStringLiteral("es.Ez"), sc)},
                                                       sv.s().topology().bounds, 4, 2, 2);
            QCOMPARE(int(arrows.size()), 16);
            for (const Arrow3& a : arrows)
                QVERIFY2(std::abs(a.v.x + 1e3) < 1e-6 && std::abs(a.v.y) < 1e-6 && std::abs(a.v.z) < 1e-6, qPrintable(QStringLiteral("%1, %2, %3").arg(a.v.x).arg(a.v.y).arg(a.v.z)));
            QCOMPARE(arrows.front().at.x, 0.25);
        }
    }

    // A 3D model's exports, read back. A VTU of a linear field (order 1): its
    // points the mesh's nodes, ht.T at each the field's there, each cell a
    // wedge whose floor's normal points away from its top (VTK's), its
    // domain from 1. Of an order 2 field quadratic wedges: each edge's middle
    // where its corners' middle is, the quadratic field's value there. A
    // series in time a PVD of its output times, a VTU each. Field on a Grid in
    // 3D: a CSV of every grid point, the linear field at each and nan beyond
    // the model; a NumPy grid of shape (1, nz, ny, nx), its points' x, y, z
    // beside it; columns with a blank line after each row of x.
    void exports()
    {
        // A VTU's arrays by name (its points' "", none named), as numbers.
        auto arrays = [](const QByteArray& xml) {
            QHash<QString, std::vector<double>> out;
            QXmlStreamReader r(xml);
            while (!r.atEnd()) {
                r.readNext();
                if (!r.isStartElement() || r.name() != QLatin1String("DataArray")) continue;
                const QString name = r.attributes().value(QStringLiteral("Name")).toString();
                const QString text = r.readElementText();
                std::vector<double>& v = out[name];
                for (const QString& t : text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
                    v.push_back(t == QLatin1String("nan") ? std::nan("") : t.toDouble());
            }
            return out;
        };
        auto node = [](Model& m, const QString& type, const QJsonObject& props) {
            Node n = m.make(type);
            for (auto it = props.begin(); it != props.end(); ++it) n.props.insert(it.key(), it.value());
            return n;
        };
        auto T = [](double x, double y, double z) { return 300 + 10 * (x + 2 * y + 3 * z); };   // mm
        {
            Model m = model3d(0.4);
            addBox(m, QStringLiteral("left"), 0, 0, 0, 1, 1, 1);
            addBox(m, QStringLiteral("right"), 1, 0, 0, 1, 1, 1);
            const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("2[W/(m*K)]"));
            addFeature(m, ht, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"),
                       QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K] + 1e4[K/m]*(x + 2*y + 3*z)")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const Solutions3 set{sv.r.solution};
            const Export3 vtu = export3(set, node(m, QStringLiteral("vtu"), {{QStringLiteral("expressions"), QStringLiteral("ht.T")}}), QStringLiteral("box"));
            QVERIFY2(vtu.main.error.isEmpty() && vtu.main.suffix == QLatin1String("vtu") && vtu.parts.empty(), qPrintable(vtu.main.error));
            const auto a = arrays(vtu.main.data);
            const std::vector<double>& points = a.value(QString()), &t = a.value(QStringLiteral("ht.T"));
            const std::vector<double>& cells = a.value(QStringLiteral("connectivity")), &types = a.value(QStringLiteral("types"));
            const Mesh3& mesh = sv.s().mesh();
            QCOMPARE(points.size(), 3 * mesh.nodes.size());
            QCOMPARE(t.size(), mesh.nodes.size());
            QCOMPARE(cells.size(), 6 * mesh.prisms.size());
            QVERIFY(std::all_of(types.begin(), types.end(), [](double v) { return v == 13; }));
            QCOMPARE(a.value(QStringLiteral("dom")).size(), mesh.prisms.size());
            double worst = 0;
            for (std::size_t i = 0; i < t.size(); ++i) worst = std::max(worst, std::abs(t[i] - T(points[3 * i], points[3 * i + 1], points[3 * i + 2])));
            QVERIFY2(worst < 1e-8, qPrintable(QStringLiteral("%1 K off").arg(worst)));
            int inward = 0;
            for (std::size_t c = 0; c < mesh.prisms.size(); ++c) {
                auto at = [&](int k) {
                    const std::size_t p = std::size_t(cells[6 * c + std::size_t(k)]);
                    return Vec3{points[3 * p], points[3 * p + 1], points[3 * p + 2]};
                };
                const Vec3 p0 = at(0), p1 = at(1), p2 = at(2), p3 = at(3), p4 = at(4), p5 = at(5);
                const Vec3 u{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z}, v{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
                const Vec3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
                const Vec3 up{(p3.x + p4.x + p5.x - p0.x - p1.x - p2.x) / 3, (p3.y + p4.y + p5.y - p0.y - p1.y - p2.y) / 3, (p3.z + p4.z + p5.z - p0.z - p1.z - p2.z) / 3};
                inward += n.x * up.x + n.y * up.y + n.z * up.z < 0 ? 0 : 1;
            }
            QVERIFY2(inward == 0, qPrintable(QStringLiteral("%1 wedges with their floor's normal toward their top").arg(inward)));
            // Field on a Grid: a CSV over and beyond the model.
            const QJsonObject grid{{QStringLiteral("expressions"), QStringLiteral("ht.T")},
                                   {QStringLiteral("from"), QJsonArray{QStringLiteral("-0.5"), QStringLiteral("-0.5")}},
                                   {QStringLiteral("to"), QJsonArray{QStringLiteral("2.5"), QStringLiteral("1.5")}},
                                   {QStringLiteral("zrange"), QJsonArray{QStringLiteral("0.25"), QStringLiteral("750[um]")}},
                                   {QStringLiteral("nx"), 7}, {QStringLiteral("ny"), 5}, {QStringLiteral("nz"), 3}};
            QJsonObject csvProps = grid;
            csvProps.insert(QStringLiteral("format"), QStringLiteral("csv"));
            const Export3 csv = export3(set, node(m, QStringLiteral("fieldexport"), csvProps), QStringLiteral("box"));
            QVERIFY2(csv.main.error.isEmpty() && csv.main.suffix == QLatin1String("csv"), qPrintable(csv.main.error));
            const QStringList lines = csv.main.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            QCOMPARE(lines.size(), 1 + 7 * 5 * 3);
            QCOMPARE(lines.front(), QStringLiteral("x (mm),y (mm),z (mm),ht.T (K)"));
            int inside = 0, beyond = 0;
            std::set<double> zs;
            for (int i = 1; i < lines.size(); ++i) {
                const QStringList c = lines[i].split(QLatin1Char(','));
                const double x = c[0].toDouble(), y = c[1].toDouble(), z = c[2].toDouble();
                zs.insert(z);
                const bool in = x >= 0 && x <= 2 && y >= 0 && y <= 1;
                if (in) {
                    ++inside;
                    QVERIFY2(std::abs(c[3].toDouble() - T(x, y, z)) < 1e-8, qPrintable(lines[i]));
                } else {
                    ++beyond;
                    QCOMPARE(c[3], QStringLiteral("nan"));
                }
            }
            QVERIFY(inside > 0 && beyond > 0);
            // (Its z from 0.25 to 750 µm, as given: the model's own is 0 to 1.)
            QVERIFY2(zs == (std::set<double>{0.25, 0.5, 0.75}), qPrintable(lines[1]));
            QJsonObject npyProps = grid;
            npyProps.insert(QStringLiteral("format"), QStringLiteral("npy"));
            const Export3 np = export3(set, node(m, QStringLiteral("fieldexport"), npyProps), QStringLiteral("box"));
            QVERIFY2(np.main.error.isEmpty() && np.main.suffix == QLatin1String("npy"), qPrintable(np.main.error));
            QVERIFY2(np.main.data.contains("'shape': (1, 3, 5, 7)") && np.main.coordinates.contains("'shape': (3, 3, 5, 7)"), np.main.data.left(120).constData());
            QCOMPARE(np.main.data.size() - np.main.data.indexOf('\n') - 1, qsizetype(8 * 105));
            QJsonObject datProps = grid;
            datProps.insert(QStringLiteral("format"), QStringLiteral("dat"));
            const Export3 dat = export3(set, node(m, QStringLiteral("fieldexport"), datProps), QStringLiteral("box"));
            QVERIFY2(dat.main.error.isEmpty() && dat.main.text.count(QStringLiteral("\n\n\n")) == 3 && dat.main.text.startsWith(QStringLiteral("# x (mm) y (mm) z (mm)")),
                     qPrintable(dat.main.text.left(200)));
        }
        {
            // Order 2: quadratic wedges.
            Model m = model3d(0.5);
            addBox(m, QStringLiteral("box"), 0, 0, 0, 1, 1, 1);
            const QString ht = addPhysics(m, QStringLiteral("heat"), 2, QStringLiteral("2[W/(m*K)]"));
            addFeature(m, ht, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"),
                       QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K] + 1e7[K/m^2]*x^2 + 2e6[K/m^2]*y*z")}});
            addFeature(m, ht, QStringLiteral("heat/source"), rule(R"({"all": true})"), QJsonObject{{QStringLiteral("Q0"), QStringLiteral("-4e7[W/m^3]")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const Export3 vtu = export3(Solutions3{sv.r.solution}, node(m, QStringLiteral("vtu"), {{QStringLiteral("expressions"), QStringLiteral("T = ht.T")}}), QStringLiteral("q"));
            QVERIFY2(vtu.main.error.isEmpty(), qPrintable(vtu.main.error));
            const auto a = arrays(vtu.main.data);
            const std::vector<double>& points = a.value(QString()), &t = a.value(QStringLiteral("T")), &cells = a.value(QStringLiteral("connectivity"));
            QCOMPARE(cells.size(), 15 * sv.s().mesh().prisms.size());
            QVERIFY(a.value(QStringLiteral("types")).front() == 26);
            double worst = 0, off = 0;
            static const int edge[9][2] = {{0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5}, {5, 3}, {0, 3}, {1, 4}, {2, 5}};
            for (std::size_t c = 0; c < sv.s().mesh().prisms.size(); ++c)
                for (int k = 0; k < 9; ++k) {
                    const std::size_t m0 = std::size_t(cells[15 * c + std::size_t(6 + k)]), a0 = std::size_t(cells[15 * c + std::size_t(edge[k][0])]),
                                      b0 = std::size_t(cells[15 * c + std::size_t(edge[k][1])]);
                    for (int d = 0; d < 3; ++d) off = std::max(off, std::abs(points[3 * m0 + std::size_t(d)] - 0.5 * (points[3 * a0 + std::size_t(d)] + points[3 * b0 + std::size_t(d)])));
                    const double x = points[3 * m0], y = points[3 * m0 + 1], z = points[3 * m0 + 2];
                    worst = std::max(worst, std::abs(t[m0] - (300 + 10 * x * x + 2 * y * z)));
                }
            QVERIFY2(off < 1e-12 && worst < 1e-7, qPrintable(QStringLiteral("middles %1 mm off, %2 K off").arg(off).arg(worst)));
        }
        {
            // A series in time: a PVD of its three output times.
            Model m = model3d(0.5);
            addBox(m, QStringLiteral("block"), 0, 0, 0, 2, 1, 1);
            addMaterial(m, QStringLiteral("A"),
                        QJsonObject{{QStringLiteral("k"), QStringLiteral("2[W/(m*K)]")}, {QStringLiteral("rho"), QStringLiteral("1000[kg/m^3]")}, {QStringLiteral("Cp"), QStringLiteral("1000[J/(kg*K)]")}},
                        rule(R"({"all": true})"));
            const QString ht = addPhysics(m, QStringLiteral("heat"), 1);
            addFeature(m, ht, QStringLiteral("heat/source"), rule(R"({"all": true})"),
                       QJsonObject{{QStringLiteral("kind"), QStringLiteral("power")}, {QStringLiteral("P0"), QStringLiteral("2[mW]")}});
            inTime(m, QJsonObject{{QStringLiteral("times"), QStringLiteral("range(0, 0.5[s], 1[s])")}, {QStringLiteral("solver"), QStringLiteral("direct")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const Export3 pvd = export3(Solutions3{sv.r.solution},
                                        node(m, QStringLiteral("vtu"), {{QStringLiteral("expressions"), QStringLiteral("ht.T")}, {QStringLiteral("solutions"), QStringLiteral("all")}}),
                                        QStringLiteral("block_vtu1"));
            QVERIFY2(pvd.main.error.isEmpty() && pvd.main.suffix == QLatin1String("pvd") && pvd.parts.size() == 3, qPrintable(pvd.main.error));
            QVERIFY2(pvd.main.data.contains("timestep=\"0.5\"") && pvd.main.data.contains("file=\"block_vtu1_0002.vtu\""), pvd.main.data.constData());
            QCOMPARE(pvd.parts[2].first, QStringLiteral("_0002.vtu"));
            const std::vector<double> last = arrays(pvd.parts[2].second).value(QStringLiteral("ht.T"));
            QVERIFY(!last.empty() && std::abs(last.front() - (293.15 + 1.0)) < 1e-6);
        }
    }

    // A function of three arguments by its table - f(x, y, z) = x + 2y + 3z on
    // a 2 x 3 x 2 grid - trilinear, so exact for it: at a point inside, and
    // constant beyond the grid; a heat source of it, ht.P its integral; a
    // table that is not a whole grid refused, said.
    void interpolationOfThree()
    {
        auto withTable = [](bool whole) {
            Model m = model3d(0.5);
            Node f = m.make(QStringLiteral("interpolation"));
            f.props.insert(QStringLiteral("name"), QStringLiteral("f3"));
            f.props.insert(QStringLiteral("dimension"), QStringLiteral("3"));
            f.props.insert(QStringLiteral("argunit"), QStringLiteral("mm, mm, mm"));
            f.props.insert(QStringLiteral("unit"), QStringLiteral("W/m^3"));
            QJsonArray rows;
            for (double x : {0.0, 2.0})
                for (double y : {0.0, 0.5, 1.0})
                    for (double z : {0.0, 1.0}) {
                        if (!whole && x == 2.0 && y == 1.0 && z == 1.0) continue;
                        rows.append(QJsonArray{x, y, z, x + 2 * y + 3 * z});
                    }
            f.props.insert(QStringLiteral("grid3"), rows);
            m.add(m.root.child(QStringLiteral("definitions"))->tag, f);
            return m;
        };
        Model m = withTable(true);
        bool added = false;
        for (const Node& d : m.root.child(QStringLiteral("definitions"))->children) added = added || d.type == QLatin1String("interpolation");
        QVERIFY(added);
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        QVERIFY2(ps->errors.isEmpty(), qPrintable(ps->errors.values().join(QStringLiteral("; "))));
        const Expression inside = Expression::compile(QStringLiteral("f3(0.5[mm], 0.8[mm], 0.25[mm])"), ps->scope);
        QVERIFY2(inside.isValid() && inside.isConstant(), qPrintable(inside.error()));
        QVERIFY2(std::abs(inside.constant() - (0.5 + 1.6 + 0.75)) < 1e-12, qPrintable(QString::number(inside.constant(), 'g', 17)));
        const Expression beyond = Expression::compile(QStringLiteral("f3(10[mm], 0.8[mm], -3[mm])"), ps->scope);
        QVERIFY(beyond.isConstant() && std::abs(beyond.constant() - (2 + 1.6 + 0)) < 1e-12);
        QVERIFY(!Expression::compile(QStringLiteral("f3(1[mm], 2[mm])"), ps->scope).isValid());
        // A heat source of it, read at each point: ht.P its integral, V (x̄ + 2ȳ + 3z̄).
        addBox(m, QStringLiteral("box"), 0, 0, 0, 2, 1, 1);
        const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1[W/(m*K)]"));
        addFeature(m, ht, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"));
        addFeature(m, ht, QStringLiteral("heat/source"), rule(R"({"all": true})"), QJsonObject{{QStringLiteral("Q0"), QStringLiteral("1e9*f3(x, y, z)")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
        const double want = 1e9 * 2e-9 * (1 + 2 * 0.5 + 3 * 0.5);
        QVERIFY2(std::abs(global(sv.s(), QStringLiteral("ht.P")) - want) < 1e-9 * want, qPrintable(sv.s().globalText(QStringLiteral("ht.P"))));
        // Not a whole grid: refused.
        const std::shared_ptr<ParameterScope> broken = evaluateParameters(withTable(false));
        QVERIFY2(broken->errors.value(QStringLiteral("f3")).contains(QStringLiteral("not a grid of every x with every y and every z")),
                 qPrintable(broken->errors.values().join(QStringLiteral("; "))));
    }

    /// What a solve will hold and take, said before it begins: each
    /// physics' unknowns and its matrix's nonzeros - counted, as a brute
    /// count of the pairs that share a prism has them, at orders 1 and 2 -,
    /// the fits within their stated share of each solve measured in phase C,
    /// a mesh's estimate the solve's, said first in its log; a study past the
    /// limit not solved and what would bring it under said, forced solved; a
    /// solver for this solve alone.
    void estimates()
    {
        Model m = model3d(0.25);
        addBox(m, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
        const QString ht = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1[W/(m*K)]"));
        addFeature(m, ht, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("cube"), QStringLiteral("bottom")),
                   QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        const GeometryBuild3 g = buildGeometry3(m, *ps);
        QVERIFY(g.ok());
        const Mesh3Build mb = buildMesh3(m, *ps, g.topology);
        QVERIFY(mb.mesh);
        for (int order : {1, 2}) {
            const Space3 sp(mb.mesh, order);
            std::set<std::pair<int, int>> pairs;
            for (int e = 0; e < sp.elements(); ++e)
                for (int i = 0; i < sp.perElement(); ++i)
                    for (int j = 0; j < sp.perElement(); ++j)
                        if (sp.dofs(e)[i] >= 0 && sp.dofs(e)[j] >= 0) pairs.insert({sp.dofs(e)[i], sp.dofs(e)[j]});
            QCOMPARE(patternNonzeros(sp), qint64(pairs.size()));
            // (Its rows in chunks on threads, as a large space's are: the same.)
            QCOMPARE(patternNonzeros(sp, 7), qint64(pairs.size()));
        }
        // The fits, against what was measured (M2 Ultra): the app's own solves
        // of boxes of order 1 and 2 (estimate_probe) and phase C's order-1
        // Laplacians (amg_bench.tsv) - the solver's own part of each peak,
        // its matrix and vectors aside; multigrid's within 7 %, the
        // Cholesky's within 17 % and its time within a factor of two.
        const double MB = 1048576;
        const struct {
            double n, nonzeros, cholMB, cholS;
        } direct[] = {{15162, 296460, 49, 0.57},        {44070, 881496, 162, 0.39},        {119187, 2415765, 562, 1.24},
                      {309852, 6342570, 1853, 4.03},    {14637, 619245, 75, 0.69},         {46531, 2029291, 309, 0.71},
                      {114431, 5052341, 884, 2.00},     {28830, 410248, 94, 0.176},        {67240, 969398, 281, 0.473},
                      {130050, 1889748, 634, 1.013},    {270400, 3956414, 1779, 2.984},    {524880, 7717998, 4168, 9.224},
                      {1020100, 15059498, 7920, 31.678}};
        for (const auto& r : direct) {
            const double bytes = directSolverBytes(r.n, r.nonzeros) / (r.cholMB * MB), seconds = directSolverSeconds(r.n, r.nonzeros) / r.cholS;
            QVERIFY2(std::abs(bytes - 1) < 0.18 && seconds > 0.45 && seconds < 2.1,
                     qPrintable(QStringLiteral("%1 unknowns: memory %2, time %3 of the measured").arg(r.n).arg(bytes).arg(seconds)));
        }
        const struct {
            double nonzeros, amgMB, amgS;
        } iterative[] = {{881496, 72.5, 0.176}, {2415765, 201.6, 0.53}, {6342570, 528.2, 1.593}, {2029291, 155.4, 0.333}, {5052341, 400.6, 0.91}};
        for (const auto& r : iterative)
            QVERIFY2(std::abs(iterativeSolverBytes(r.nonzeros) / (r.amgMB * MB) - 1) < 0.07
                         && std::abs((iterativeSolverSeconds(r.nonzeros) + assemblySeconds(0)) / r.amgS - 1) < 0.5,
                     qPrintable(QString::number(r.nonzeros)));
        // A mesh's estimate, and the solve's, said first in its log.
        const std::vector<Estimate3> before = estimate3(m, mb.mesh);
        QCOMPARE(int(before.size()), 1);
        SolveAsked3 asked;
        StudyResult3 r = solveStudy3(m, ps, mb.mesh, {}, {}, asked);
        QVERIFY2(r.solution, qPrintable(r.error));
        QCOMPARE(int(r.estimates.size()), 1);
        // (A copy: r is solved again below.)
        const Estimate3 e = r.estimates.front();
        QCOMPARE(e.physics, ht);
        QCOMPARE(e.unknowns, qint64(r.solution->fields().front().space->size()));
        QCOMPARE(e.nonzeros, before.front().nonzeros);
        QCOMPARE(e.bytes(), before.front().bytes());
        QVERIFY(e.nonzeros > 10 * e.unknowns && e.bytes() > 0 && e.seconds() > 0);
        QVERIFY2(r.log.first().startsWith(QStringLiteral("Estimated: %1 (order 1): %2 unknowns").arg(ht, QLocale().toString(e.unknowns))),
                 qPrintable(r.log.first()));
        // Past a limit: not solved, what would bring it under said; forced,
        // solved and said.
        asked.limit = e.bytes() / 2;
        r = solveStudy3(m, ps, mb.mesh, {}, {}, asked);
        QVERIFY(!r.solution);
        QVERIFY2(r.error.contains(QStringLiteral("past the limit of %1").arg(bytesText(asked.limit))) && r.error.contains(QStringLiteral("a coarser mesh"))
                     && r.error.contains(QStringLiteral("force")),
                 qPrintable(r.error));
        asked.force = true;
        r = solveStudy3(m, ps, mb.mesh, {}, {}, asked);
        QVERIFY2(r.solution && r.log.join(QLatin1Char('\n')).contains(QStringLiteral("Solved past the limit, as asked (force)")), qPrintable(r.error));
        // A direct solve asked, a Cholesky; an iterative one, multigrid.
        asked = SolveAsked3();
        asked.solver = QStringLiteral("direct");
        r = solveStudy3(m, ps, mb.mesh, {}, {}, asked);
        QVERIFY2(r.solution && r.log.join(QLatin1Char('\n')).contains(QStringLiteral("Cholesky (METIS order)")), qPrintable(r.log.join(QLatin1Char('\n'))));
        QCOMPARE(r.estimates.front().method, QStringLiteral("direct"));
        asked.solver = QStringLiteral("iterative");
        r = solveStudy3(m, ps, mb.mesh, {}, {}, asked);
        QVERIFY2(r.solution && r.log.join(QLatin1Char('\n')).contains(QStringLiteral("conjugate gradients with algebraic multigrid")),
                 qPrintable(r.log.join(QLatin1Char('\n'))));
        // A direct solve past the limit where multigrid fits: the iterative
        // solver offered, with its own estimate.
        Estimate3 big = e;
        big.method = QStringLiteral("direct");
        big.directBytes = 10.0 * 1073741824.0;
        big.iterativeBytes = 1.0 * 1073741824.0;
        const QString past = pastTheLimit({big}, 2.0 * 1073741824.0);
        QVERIFY2(past.contains(QStringLiteral("some 10 GB, past the limit of 2.0 GB")) && past.contains(QStringLiteral("the iterative solver (some 1.0 GB")),
                 qPrintable(past));
        QVERIFY(pastTheLimit({big}, 0).isEmpty() && pastTheLimit({big}, 20.0 * 1073741824.0).isEmpty());
        // A study's peak its largest physics' (each lets go of its system):
        // an order-2 physics beside it on its own space.
        const QString ec = addPhysics(m, QStringLiteral("currents"), 2, QStringLiteral("1[S/m]"));
        const std::vector<Estimate3> two = estimate3(m, mb.mesh);
        QCOMPARE(int(two.size()), 2);
        QCOMPARE(two[1].physics, ec);
        QCOMPARE(two[1].order, 2);
        QCOMPARE(two[1].unknowns, qint64(Space3(mb.mesh, 2).size()));
        QCOMPARE(two[0].nonzeros, e.nonzeros);
        QVERIFY(two[1].nonzeros > two[0].nonzeros);
        QCOMPARE(peakBytes(two), std::max(two[0].bytes(), two[1].bytes()));
        QVERIFY(physicalMemory() > 1e9);
    }

    void refusals()
    {
        auto base = [] {
            Model m = model3d(0.5);
            addBox(m, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
            return m;
        };
        const auto refused = [](const Model& m, const QString& said) {
            const QString error = solve(m).error;
            return error.contains(said) ? QString() : error.isEmpty() ? QStringLiteral("(solved)") : error;
        };
        Model m = base();
        addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1[W/(m*K)]"));
        QString why = refused(m, QStringLiteral("holds the temperature nowhere"));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        m = base();
        addPhysics(m, QStringLiteral("electrostatics"), 1, QStringLiteral("1"));
        why = refused(m, QStringLiteral("holds the potential nowhere"));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        m = base();
        const QString ht = addPhysics(m, QStringLiteral("heat"), 1);
        addFeature(m, ht, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"));
        why = refused(m, QStringLiteral("has no material"));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        m = base();
        const QString ht2 = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1[W/(m*K)]"));
        for (Node& c : m.find(ht2)->children)
            if (c.type == QLatin1String("heat/solid")) c.props.insert(QStringLiteral("anisotropy"), QStringLiteral("diagonal"));
        addFeature(m, ht2, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"));
        why = refused(m, QStringLiteral("isotropic in 3D"));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        m = base();
        const QString ht3 = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1[W/(m*K)]"));
        addFeature(m, ht3, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"));
        for (Node& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                Node t = m.make(QStringLiteral("frequency"));
                c.children.push_back(t);
            }
        why = refused(m, QStringLiteral("not solved in 3D yet"));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        // A step in time where a domain has no ρ and Cp.
        m = base();
        const QString ht4 = addPhysics(m, QStringLiteral("heat"), 1, QStringLiteral("1[W/(m*K)]"));
        addFeature(m, ht4, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"));
        for (Node& c : m.find(ht4)->children)
            if (c.type == QLatin1String("heat/solid")) {
                c.props.insert(QStringLiteral("rho"), QString());
                c.props.insert(QStringLiteral("Cp"), QString());
            }
        inTime(m, QJsonObject{{QStringLiteral("times"), QStringLiteral("0, 1[s]")}});
        why = refused(m, QStringLiteral("has no density and heat capacity"));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        // A terminal of x.
        m = base();
        const QString ec = addPhysics(m, QStringLiteral("currents"), 1, QStringLiteral("1[S/m]"));
        addFeature(m, ec, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("cube"), QStringLiteral("left")),
                   QJsonObject{{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), QStringLiteral("1[V/m]*x")}});
        addFeature(m, ec, QStringLiteral("currents/ground"), ofSide(QStringLiteral("cube"), QStringLiteral("right")));
        why = refused(m, QStringLiteral("must be a constant, or of t alone"));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        m = base();
        Node solid = m.make(QStringLiteral("solid"));
        m.component().children.push_back(solid);   // (as a file could have it: add() refuses it)
        why = refused(m, QStringLiteral("not solved in 3D yet"));
        QVERIFY2(why.isEmpty(), qPrintable(why));
    }
};

QTEST_GUILESS_MAIN(TestFem3Solve)
#include "test_fem3_solve.moc"
