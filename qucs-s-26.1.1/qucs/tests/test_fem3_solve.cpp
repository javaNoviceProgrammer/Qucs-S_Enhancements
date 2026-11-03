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
#include "fem3_results.h"
#include "fem3_solver.h"

#include <QJsonDocument>
#include <QtTest>

#include <cmath>

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

struct Solved {
    std::shared_ptr<const Mesh3> mesh;
    StudyResult3 r;
    QString error;
    const Solution3& s() const { return *r.solution; }
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
                Node t = m.make(QStringLiteral("transient"));
                c.children.push_back(t);
            }
        why = refused(m, QStringLiteral("not solved in 3D yet"));
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
