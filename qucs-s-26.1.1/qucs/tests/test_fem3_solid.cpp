/*
 * test_fem3_solid.cpp - solid mechanics solved in 3D (multiphysics/
 *                       fem3_solid.cpp): patch tests (a uniform strain order
 *                       1's, a quadratic displacement with its body load
 *                       order 2's, exactly), a cantilever's deflection
 *                       against Timoshenko's beam, Lamé's thick-walled
 *                       cylinder, a free bar's thermal expansion and its
 *                       weight's reaction, a bimetal strip heated (heat and
 *                       solid coupled) against Timoshenko's curvature,
 *                       silicon's Young's modulus along [100] and [110],
 *                       multigrid with rigid-body modes against a direct
 *                       solve, and what is refused
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_estimate.h"
#include "fem3_results.h"
#include "fem3_solver.h"
#include "fem_results.h"
#include "fem_solver.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QtTest>

#include <cmath>

using namespace qucs_s::fem;

namespace {

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
}

QJsonObject rule(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

QJsonObject boxRule(double x0, double y0, double z0, double x1, double y1, double z1)
{
    QJsonArray b;
    for (double v : {x0, y0, z0, x1, y1, z1}) b.append(v);
    return QJsonObject{{QStringLiteral("box"), b}};
}

QJsonObject ofSide(const QString& object, const QString& side)
{
    return QJsonObject{{QStringLiteral("of"), QJsonArray{object}}, {QStringLiteral("side"), side}};
}

QJsonObject objects(const QString& object)
{
    return QJsonObject{{QStringLiteral("objects"), QJsonArray{object}}};
}

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

/// Divisions along z of an object's layers.
void zDivisions(Model& m, const QString& object, int count)
{
    Node z = m.make(QStringLiteral("mesh/zdistribution"));
    z.props.insert(QStringLiteral("selection"), objects(object));
    z.props.insert(QStringLiteral("count"), count);
    m.add(m.mesh().tag, z);
}

/// A solid of \a order, its default Linear Elastic Material the user's
/// \a props (E, nu, alpha...) where given.
QString addSolid(Model& m, int order, const QJsonObject& props = {})
{
    Node p = m.make(QStringLiteral("solid"));
    p.props.insert(QStringLiteral("order"), QString::number(order));
    const QString tag = p.tag;
    m.add(m.component().tag, p);
    Node* n = m.find(tag);
    for (Node& c : n->children)
        if (c.type == QLatin1String("solid/elastic") && !props.isEmpty()) {
            c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            for (auto it = props.begin(); it != props.end(); ++it) c.props.insert(it.key(), it.value());
        }
    return tag;
}

/// The physics' default Linear Elastic Material node.
Node* elastic(Model& m, const QString& physics)
{
    for (Node& c : m.find(physics)->children)
        if (c.type == QLatin1String("solid/elastic")) return &c;
    return nullptr;
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

QString why(const Solved& sv)
{
    return sv.error + QLatin1Char('\n') + sv.r.log.join(QLatin1Char('\n'));
}

/// A measurement for the write-up, in the test's log.
void info(const QString& what)
{
    qInfo("%s", qPrintable(what));
}

} // namespace

class TestFem3Solid : public QObject
{
    Q_OBJECT

private slots:
    // A uniform strain held on every face - u, v, w linear in x, y, z - is
    // order 1's at every degree of freedom, its stress C ε everywhere; a
    // quadratic displacement u = a x² held, with the body load that balances
    // it, -(λ + 2µ) 2a, order 2's.
    void patchTests()
    {
        const double E = 200e9, nu = 0.3;
        const double lambda = E * nu / ((1 + nu) * (1 - 2 * nu)), mu = E / (2 * (1 + nu));
        for (int order : {1, 2}) {
            Model m = model3d(0.5);
            addBox(m, QStringLiteral("left"), 0, 0, 0, 1, 1, 1);
            addBox(m, QStringLiteral("right"), 1, 0, 0, 1, 1, 1);
            const QString solid = addSolid(m, order, {{QStringLiteral("E"), QStringLiteral("200[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.3")}});
            const double a = 1e3;   // 1/m: u = a x² (order 2)
            QJsonObject held;
            if (order == 1) {
                held = {{QStringLiteral("u0"), QStringLiteral("1e-3*x + 2e-4*y")},
                        {QStringLiteral("v0"), QStringLiteral("-3e-4*y + 1e-4*z")},
                        {QStringLiteral("w0"), QStringLiteral("5e-4*z + 3e-4*x")}};
            } else {
                held = {{QStringLiteral("u0"), QStringLiteral("1e3[1/m]*x^2")}, {QStringLiteral("v0"), QStringLiteral("0[m]")}, {QStringLiteral("w0"), QStringLiteral("0[m]")}};
                addFeature(m, solid, QStringLiteral("solid/bodyload"), rule(R"({"all": true})"),
                           {{QStringLiteral("fx"), QString::number(-(lambda + 2 * mu) * 2 * a, 'g', 17) + QStringLiteral("[N/m^3]")}});
            }
            addFeature(m, solid, QStringLiteral("solid/displacement"), rule(R"({"exterior": true})"), held);
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const Field3& f = sv.s().fields().front();
            const int n = f.space->size();
            double worst = 0, scale = 0;
            for (int dof = 0; dof < n; ++dof) {
                const Vec3 x = f.space->point(dof);
                double want[3];
                if (order == 1) {
                    want[0] = 1e-3 * x.x + 2e-4 * x.y;
                    want[1] = -3e-4 * x.y + 1e-4 * x.z;
                    want[2] = 5e-4 * x.z + 3e-4 * x.x;
                } else {
                    want[0] = a * x.x * x.x;
                    want[1] = want[2] = 0;
                }
                for (int c = 0; c < 3; ++c) {
                    worst = std::max(worst, std::abs(f.values[std::size_t(c * n + dof)] - want[c]));
                    scale = std::max(scale, std::abs(want[c]));
                }
            }
            QVERIFY2(worst < 1e-9 * scale, qPrintable(QStringLiteral("order %1: %2 m off, of %3").arg(order).arg(worst).arg(scale)));
            if (order == 1) {
                // σ = C ε: εxx 1e-3, εyy -3e-4, εzz 5e-4, γxy 2e-4, γyz 1e-4, γxz 3e-4.
                const double tr = 1e-3 - 3e-4 + 5e-4;
                const double sx = valueAt(sv.s(), QStringLiteral("solid.sx"), 0.7, 0.3, 0.6);
                const double sxy = valueAt(sv.s(), QStringLiteral("solid.sxy"), 1.4, 0.8, 0.2);
                const double syz = valueAt(sv.s(), QStringLiteral("solid.syz"), 0.2, 0.5, 0.9);
                QVERIFY2(std::abs(sx - (lambda * tr + 2 * mu * 1e-3)) < 1e-6 * std::abs(sx), qPrintable(QString::number(sx)));
                QVERIFY2(std::abs(sxy - mu * 2e-4) < 1e-6 * std::abs(sxy), qPrintable(QString::number(sxy)));
                QVERIFY2(std::abs(syz - mu * 1e-4) < 1e-6 * std::abs(syz), qPrintable(QString::number(syz)));
                const double exy = valueAt(sv.s(), QStringLiteral("solid.exy"), 1.4, 0.8, 0.2);
                QVERIFY2(std::abs(exy - 1e-4) < 1e-12, qPrintable(QString::number(exy)));
            }
        }
    }

    // A cantilever 10 mm long, 1 mm square, its root fixed and its tip's face
    // pushed down by P: its tip's deflection Timoshenko's, P L³/(3 E I) +
    // P L/(κ G A), κ 5/6, within 1 % at order 2 (ν 0: no anticlastic bending
    // at the root); its reaction P. A linear prism locks: far too stiff.
    void cantilever()
    {
        const double L = 10e-3, b = 1e-3, E = 200e9, P = 10;
        const double I = b * b * b * b / 12, A = b * b, G = E / 2;
        const double exact = P * L * L * L / (3 * E * I) + P * L / (5.0 / 6.0 * G * A);
        double tip[3] = {0, 0, 0};
        for (int order : {1, 2}) {
            Model m = model3d(slowBuild() ? 0.5 : 0.25);
            addBox(m, QStringLiteral("beam"), 0, 0, 0, 10, 1, 1);
            zDivisions(m, QStringLiteral("beam"), slowBuild() ? 2 : 4);
            const QString solid = addSolid(m, order, {{QStringLiteral("E"), QStringLiteral("200[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0")}});
            addFeature(m, solid, QStringLiteral("solid/fixed"), ofSide(QStringLiteral("beam"), QStringLiteral("left")));
            addFeature(m, solid, QStringLiteral("solid/load"), ofSide(QStringLiteral("beam"), QStringLiteral("right")),
                       {{QStringLiteral("Fz"), QString::number(-P / A, 'g', 17) + QStringLiteral("[N/m^2]")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            tip[order] = -valueAt(sv.s(), QStringLiteral("solid.w"), 10, 0.5, 0.5);
            const double rz = global(sv.s(), QStringLiteral("solid.Rz"));
            QVERIFY2(std::abs(rz - P) < 1e-6 * P, qPrintable(QStringLiteral("order %1: reaction %2 N").arg(order).arg(rz)));
            QVERIFY2(std::abs(global(sv.s(), QStringLiteral("solid.Rx"))) < 1e-6 * P, qPrintable(QString::number(global(sv.s(), QStringLiteral("solid.Rx")))));
            if (order == 2) {
                // Its strain energy the work of the load, P δ / 2.
                const double W = global(sv.s(), QStringLiteral("solid.W"));
                QVERIFY2(std::abs(W - 0.5 * P * tip[2]) < 2e-2 * W, qPrintable(QStringLiteral("W %1 J, P δ/2 %2 J").arg(W).arg(0.5 * P * tip[2])));
                // The root's bending stress M c / I at the top, away from the
                // corner: σx = P L (b/2) / I.
                const double sx = valueAt(sv.s(), QStringLiteral("solid.sx"), 2, 0.5, 1);
                const double want = P * (L - 2e-3) * (b / 2) / I;
                QVERIFY2(std::abs(sx - want) < 3e-2 * want, qPrintable(QStringLiteral("σx %1 Pa, M c/I %2 Pa").arg(sx).arg(want)));
            }
        }
        info(QStringLiteral("cantilever: order 1 %1 m, order 2 %2 m, Timoshenko %3 m").arg(tip[1]).arg(tip[2]).arg(exact));
        QVERIFY2(std::abs(tip[2] - exact) < 1e-2 * exact, qPrintable(QStringLiteral("order 2: %1 m, Timoshenko %2 m").arg(tip[2]).arg(exact)));
        // (Order 1 stiffer, as a linear prism is in bending.)
        QVERIFY2(tip[1] < tip[2], qPrintable(QStringLiteral("order 1: %1 m, order 2 %2 m").arg(tip[1]).arg(tip[2])));
    }

    // Lamé's thick-walled cylinder, a 1 mm to 2 mm, pressed inside by p, in
    // plane strain (its top and bottom on rollers): u_r = p a² / (E (b² -
    // a²)) ((1 + ν)(1 - 2ν) r + (1 + ν) b²/r), σθ = p a²/(b² - a²) (1 +
    // b²/r²), σr = p a²/(b² - a²) (1 - b²/r²) - order 2 on its arcs within
    // 0.5 %.
    void lameCylinder()
    {
        const double a = 1e-3, b = 2e-3, E = 100e9, nu = 0.3, p = 1e8;
        Model m = model3d(slowBuild() ? 0.25 : 0.15);
        addCylinder(m, QStringLiteral("shell"), 2, 0.3);
        addCylinder(m, QStringLiteral("hole"), 1, 0.3);
        Node d = m.make(QStringLiteral("difference"));
        d.label = QStringLiteral("tube");
        d.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("shell")});
        d.props.insert(QStringLiteral("tools"), QJsonArray{QStringLiteral("hole")});
        m.add(m.geometry().tag, d);
        // A quarter of it, on rollers across its planes of symmetry.
        addBox(m, QStringLiteral("quadrant"), 0, 0, 0, 3, 3, 0.3);
        Node q = m.make(QStringLiteral("intersection"));
        q.label = QStringLiteral("quarter");
        q.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("tube"), QStringLiteral("quadrant")});
        m.add(m.geometry().tag, q);
        const QString solid = addSolid(m, 2, {{QStringLiteral("E"), QStringLiteral("100[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.3")}});
        for (const auto& n : {QJsonArray{0.0, 0.0, 1.0}, QJsonArray{0.0, 0.0, -1.0}, QJsonArray{-1.0, 0.0, 0.0}, QJsonArray{0.0, -1.0, 0.0}})
            addFeature(m, solid, QStringLiteral("solid/roller"), QJsonObject{{QStringLiteral("normal"), n}});
        addFeature(m, solid, QStringLiteral("solid/load"), boxRule(-0.01, -0.01, -0.01, 1.01, 1.01, 0.31),
                   {{QStringLiteral("kind"), QStringLiteral("pressure")}, {QStringLiteral("p"), QStringLiteral("100[MPa]")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const auto ur = [&](double r) { return p * a * a / (E * (b * b - a * a)) * ((1 + nu) * (1 - 2 * nu) * r + (1 + nu) * b * b / r); };
        const double k = p * a * a / (b * b - a * a);
        const double c = std::cos(0.3), s = std::sin(0.3);
        // (Just inside its arcs: a point's prism found by its plan's
        // straight triangles.)
        for (double rmm : {1.02, 1.5, 1.98}) {
            const double r = rmm * 1e-3;
            const double got = valueAt(sv.s(), QStringLiteral("solid.u*x/sqrt(x^2+y^2) + solid.v*y/sqrt(x^2+y^2)"), rmm * c, rmm * s, 0.15);
            info(QStringLiteral("Lamé: u_r(%1 mm) %2 m, exact %3 m").arg(rmm).arg(got).arg(ur(r)));
            QVERIFY2(std::abs(got - ur(r)) < 5e-3 * ur(r), qPrintable(QStringLiteral("r %1 mm: u_r %2 m, Lamé %3 m").arg(rmm).arg(got).arg(ur(r))));
        }
        // The hoop and radial stresses at r = 1.5 mm, 0.3 rad round: the
        // stress turned into r and θ there.
        const QString sr = QStringLiteral("solid.sx*%1 + solid.sy*%2 + 2*solid.sxy*%3").arg(c * c).arg(s * s).arg(s * c);
        const QString st = QStringLiteral("solid.sx*%1 + solid.sy*%2 - 2*solid.sxy*%3").arg(s * s).arg(c * c).arg(s * c);
        const double hoop = valueAt(sv.s(), st, 1.5 * c, 1.5 * s, 0.15);
        info(QStringLiteral("Lamé: σθ %1 Pa, σr %2 Pa").arg(hoop).arg(valueAt(sv.s(), sr, 1.5 * c, 1.5 * s, 0.15)));
        const double want = k * (1 + b * b / (1.5e-3 * 1.5e-3));
        QVERIFY2(std::abs(hoop - want) < 5e-3 * want, qPrintable(QStringLiteral("σθ %1 Pa, Lamé %2 Pa").arg(hoop).arg(want)));
        const double radial = valueAt(sv.s(), sr, 1.5 * c, 1.5 * s, 0.15);
        const double wantR = k * (1 - b * b / (1.5e-3 * 1.5e-3));
        QVERIFY2(std::abs(radial - wantR) < 1e-2 * std::abs(wantR), qPrintable(QStringLiteral("σr %1 Pa, Lamé %2 Pa").arg(radial).arg(wantR)));
        // Plane strain: σz = ν (σr + σθ).
        const double sz = valueAt(sv.s(), QStringLiteral("solid.sz"), 1.5 * c, 1.5 * s, 0.15);
        QVERIFY2(std::abs(sz - nu * (want + wantR)) < 1e-2 * std::abs(nu * (want + wantR)), qPrintable(QString::number(sz)));
    }

    // A solid cylinder pressed all round (a quarter, on rollers), and a cube
    // pressed on three faces: plane strain's u_r = -p (1 + ν)(1 - 2ν) r / E
    // on the arc and -p (1 - 2ν)/E each way in the cube - each face's
    // outward normal, on whichever of its triangle's edges a wall lies.
    void pressureOnEveryWall()
    {
        const double E = 100e9, nu = 0.3, p = 1e8;
        Model m = model3d(0.12);
        addCylinder(m, QStringLiteral("rod"), 1, 0.3);
        addBox(m, QStringLiteral("quadrant"), 0, 0, 0, 2, 2, 0.3);
        Node q = m.make(QStringLiteral("intersection"));
        q.label = QStringLiteral("quarter");
        q.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("rod"), QStringLiteral("quadrant")});
        m.add(m.geometry().tag, q);
        const QString solid = addSolid(m, 2, {{QStringLiteral("E"), QStringLiteral("100[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.3")}});
        // (Pressed first everywhere, the planes then rollers over it.)
        addFeature(m, solid, QStringLiteral("solid/load"), rule(R"({"exterior": true})"),
                   {{QStringLiteral("kind"), QStringLiteral("pressure")}, {QStringLiteral("p"), QStringLiteral("100[MPa]")}});
        for (const auto& n : {QJsonArray{0.0, 0.0, 1.0}, QJsonArray{0.0, 0.0, -1.0}, QJsonArray{-1.0, 0.0, 0.0}, QJsonArray{0.0, -1.0, 0.0}})
            addFeature(m, solid, QStringLiteral("solid/roller"), QJsonObject{{QStringLiteral("normal"), n}});
        Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        for (double a : {0.1, 0.5, 0.8, 1.2, 1.5}) {
            const double r = 0.97, x = r * std::cos(a), y = r * std::sin(a);
            const double got = valueAt(sv.s(), QStringLiteral("solid.u*x/sqrt(x^2+y^2) + solid.v*y/sqrt(x^2+y^2)"), x, y, 0.15);
            const double want = -p * (1 + nu) * (1 - 2 * nu) * r * 1e-3 / E;
            QVERIFY2(std::abs(got - want) < 2e-3 * std::abs(want), qPrintable(QStringLiteral("θ %1: u_r %2 m, %3 m").arg(a).arg(got).arg(want)));
        }
        Model c = model3d(0.3);
        addBox(c, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
        const QString s2 = addSolid(c, 1, {{QStringLiteral("E"), QStringLiteral("100[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.3")}});
        for (const char* side : {"right", "back", "top"})
            addFeature(c, s2, QStringLiteral("solid/load"), ofSide(QStringLiteral("cube"), QString::fromLatin1(side)),
                       {{QStringLiteral("kind"), QStringLiteral("pressure")}, {QStringLiteral("p"), QStringLiteral("100[MPa]")}});
        for (const char* side : {"left", "front", "bottom"})
            addFeature(c, s2, QStringLiteral("solid/roller"), ofSide(QStringLiteral("cube"), QString::fromLatin1(side)));
        sv = solve(c);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double strain = -p * (1 - 2 * nu) / E;
        for (const char* v : {"solid.ex", "solid.ey", "solid.ez"}) {
            const double got = valueAt(sv.s(), QString::fromLatin1(v), 0.3, 0.6, 0.4);
            QVERIFY2(std::abs(got - strain) < 1e-9 * std::abs(strain), qPrintable(QStringLiteral("%1 %2, %3").arg(QString::fromLatin1(v)).arg(got).arg(strain)));
        }
    }

    // A free bar warmed 100 K, held only on three planes of symmetry by
    // rollers: its far corner moves α ΔT (L, W, H), exactly at order 1, with
    // no stress. Its weight on a fixed base: the base's reaction ρ g V.
    void thermalExpansionAndWeight()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("bar"), 0, 0, 0, 4, 1, 1);
        const QString solid = addSolid(m, 1, {{QStringLiteral("E"), QStringLiteral("70[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.33")},
                                              {QStringLiteral("thermal"), true}, {QStringLiteral("alpha"), QStringLiteral("2.3e-5[1/K]")},
                                              {QStringLiteral("T"), QStringLiteral("400[K]")}, {QStringLiteral("Tref"), QStringLiteral("300[K]")}});
        for (const char* side : {"left", "front", "bottom"})
            addFeature(m, solid, QStringLiteral("solid/roller"), ofSide(QStringLiteral("bar"), QString::fromLatin1(side)));
        Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double strain = 2.3e-5 * 100;
        const double u = valueAt(sv.s(), QStringLiteral("solid.u"), 4, 1, 1), v = valueAt(sv.s(), QStringLiteral("solid.v"), 4, 1, 1),
                     w = valueAt(sv.s(), QStringLiteral("solid.w"), 4, 1, 1);
        QVERIFY2(std::abs(u - strain * 4e-3) < 1e-12 && std::abs(v - strain * 1e-3) < 1e-12 && std::abs(w - strain * 1e-3) < 1e-12,
                 qPrintable(QStringLiteral("%1, %2, %3 m").arg(u).arg(v).arg(w)));
        // (Its nodes' values of each component, as the view colours them.)
        const std::vector<double> nu = nodalValues(sv.s(), Expression::compile(QStringLiteral("solid.u"), sv.s().scope()));
        const std::vector<double> nv = nodalValues(sv.s(), Expression::compile(QStringLiteral("solid.v"), sv.s().scope()));
        QVERIFY2(std::abs(*std::max_element(nu.begin(), nu.end()) - strain * 4e-3) < 1e-12, "nodal u");
        QVERIFY2(std::abs(*std::max_element(nv.begin(), nv.end()) - strain * 1e-3) < 1e-12, "nodal v");
        const double mises = valueAt(sv.s(), QStringLiteral("solid.mises"), 2.2, 0.4, 0.7);
        QVERIFY2(std::abs(mises) < 1e-6 * 70e9 * strain, qPrintable(QString::number(mises)));
        QVERIFY2(std::abs(global(sv.s(), QStringLiteral("solid.dmax")) - strain * std::sqrt(18.0) * 1e-3) < 1e-12, qPrintable(QString::number(global(sv.s(), QStringLiteral("solid.dmax")))));

        // Its weight: ρ g along -z, the base fixed - the reaction ρ g V up.
        Model w8 = model3d(0.5);
        addBox(w8, QStringLiteral("block"), 0, 0, 0, 2, 1, 1);
        const QString s2 = addSolid(w8, 2, {{QStringLiteral("E"), QStringLiteral("70[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.33")}});
        addFeature(w8, s2, QStringLiteral("solid/fixed"), ofSide(QStringLiteral("block"), QStringLiteral("bottom")));
        addFeature(w8, s2, QStringLiteral("solid/bodyload"), rule(R"({"all": true})"), {{QStringLiteral("fz"), QStringLiteral("-2700[kg/m^3]*g_const")}});
        sv = solve(w8);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double weight = 2700 * 9.80665 * 2e-9;
        QVERIFY2(std::abs(global(sv.s(), QStringLiteral("solid.Rz")) - weight) < 1e-9 * weight,
                 qPrintable(QStringLiteral("%1 N, ρ g V %2 N").arg(global(sv.s(), QStringLiteral("solid.Rz"))).arg(weight)));
    }

    // A bimetal strip - two layers 0.5 mm thick alike in E, α 1e-5 and
    // 2e-5 per K - warmed 50 K by a heat physics, clamped at one end: its
    // curvature Timoshenko's (1925), 1.5 Δα ΔT / h for equal layers, its tip
    // κ L²/2 - heat and solid solved in turn, the solid reading ht.T.
    void bimetalStrip()
    {
        const double L = 20e-3, h = 1e-3, dAlpha = 1e-5, dT = 50;
        const double kappa = 1.5 * dAlpha * dT / h;
        Model m = model3d(slowBuild() ? 1.0 : 0.5);
        addBox(m, QStringLiteral("low"), 0, 0, 0, 20, 1, 0.5);
        addBox(m, QStringLiteral("high"), 0, 0, 0.5, 20, 1, 0.5);
        zDivisions(m, QStringLiteral("low"), 2);
        zDivisions(m, QStringLiteral("high"), 2);
        Node ht = m.make(QStringLiteral("heat"));
        ht.props.insert(QStringLiteral("order"), QStringLiteral("1"));
        const QString heat = ht.tag;
        m.add(m.component().tag, ht);
        for (Node& c : m.find(heat)->children)
            if (c.type == QLatin1String("heat/solid")) {
                c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
                c.props.insert(QStringLiteral("k"), QStringLiteral("100[W/(m*K)]"));
            }
        addFeature(m, heat, QStringLiteral("heat/temperature"), rule(R"({"exterior": true})"), {{QStringLiteral("T0"), QStringLiteral("343.15[K]")}});
        const QString solid = addSolid(m, 2, {{QStringLiteral("E"), QStringLiteral("100[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0")},
                                              {QStringLiteral("thermal"), true}, {QStringLiteral("alpha"), QStringLiteral("1e-5[1/K]")},
                                              {QStringLiteral("T"), QStringLiteral("ht.T")}, {QStringLiteral("Tref"), QStringLiteral("293.15[K]")}});
        elastic(m, solid)->props.insert(QStringLiteral("selection"), objects(QStringLiteral("low")));
        Node top = m.make(QStringLiteral("solid/elastic"));
        top.props = elastic(m, solid)->props;
        top.props.insert(QStringLiteral("alpha"), QStringLiteral("2e-5[1/K]"));
        top.props.insert(QStringLiteral("selection"), objects(QStringLiteral("high")));
        m.add(solid, top);
        addFeature(m, solid, QStringLiteral("solid/fixed"), boxRule(-0.01, -0.01, -0.01, 0.01, 1.01, 1.01));
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        // (Warming it bends it down: the upper layer grows more.)
        const double tip = valueAt(sv.s(), QStringLiteral("solid.w"), 20, 0.5, 0.5);
        const double want = -kappa * L * L / 2;
        info(QStringLiteral("bimetal: tip %1 m, Timoshenko %2 m").arg(tip).arg(want));
        QVERIFY2(std::abs(tip - want) < 2e-2 * std::abs(want), qPrintable(QStringLiteral("tip %1 m, Timoshenko %2 m").arg(tip).arg(want)));
    }

    // Silicon's stiffness - C11 165.7, C12 63.9, C44 79.6 GPa, from its
    // material, cube along the axes - pulled along x: its strain σ / E with
    // E 1/S11, 130.1 GPa along [100]; the crystal turned 45° about the
    // wafer's normal (z), E 169.1 GPa along [110].
    void siliconAnisotropy()
    {
        const double C11 = 165.7e9, C12 = 63.9e9, C44 = 79.6e9;
        const double det = (C11 - C12) * (C11 + 2 * C12);
        const double S11 = (C11 + C12) / det, S12 = -C12 / det, S44 = 1 / C44;
        const double E100 = 1 / S11, E110 = 1 / (S11 - 0.5 * (S11 - S12 - S44 / 2));
        for (int turned : {0, 1}) {
            Model m = model3d(0.5);
            addBox(m, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
            QJsonObject mat = {{QStringLiteral("C"), QStringLiteral("C11=165.7[GPa]; C12=63.9[GPa]; C44=79.6[GPa]; C22=165.7[GPa]; C33=165.7[GPa]; "
                                                                  "C13=63.9[GPa]; C23=63.9[GPa]; C55=79.6[GPa]; C66=79.6[GPa]")}};
            if (turned) {
                // (The wafer's normal is the plane's y: a turn about it,
                // Euler's z-x-z as 90°, 45°, -90°.)
                mat.insert(QStringLiteral("orientation"), QStringLiteral("euler"));
                mat.insert(QStringLiteral("euler_phi"), QStringLiteral("90"));
                mat.insert(QStringLiteral("euler_theta"), QStringLiteral("45"));
                mat.insert(QStringLiteral("euler_psi"), QStringLiteral("-90"));
            }
            addMaterial(m, QStringLiteral("Si"), mat, rule(R"({"all": true})"));
            const QString solid = addSolid(m, 1);
            for (const char* side : {"left", "front", "bottom"})
                addFeature(m, solid, QStringLiteral("solid/roller"), ofSide(QStringLiteral("cube"), QString::fromLatin1(side)));
            addFeature(m, solid, QStringLiteral("solid/load"), ofSide(QStringLiteral("cube"), QStringLiteral("right")), {{QStringLiteral("Fx"), QStringLiteral("100[MPa]")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double ex = valueAt(sv.s(), QStringLiteral("solid.ex"), 0.5, 0.5, 0.5);
            const double want = 1e8 / (turned ? E110 : E100);
            QVERIFY2(std::abs(ex - want) < 1e-6 * want, qPrintable(QStringLiteral("turned %1: εx %2, σ/E %3 (E %4 GPa)").arg(turned).arg(ex).arg(want).arg(1e-9 * 1e8 / ex)));
            // (Pulled along a direction of two-fold symmetry: no shear.)
            QVERIFY2(std::abs(valueAt(sv.s(), QStringLiteral("solid.exy"), 0.5, 0.5, 0.5)) < 1e-9 * want, "shear strain");
            // Its sides: along [001] (the wafer's normal, z) S12 σ, and along
            // [1-10] (y, turned) S12 + (S11 - S12 - S44/2)/2 - a crystal
            // turned about another axis than z has them otherwise.
            const double ey = valueAt(sv.s(), QStringLiteral("solid.ey"), 0.5, 0.5, 0.5), ez = valueAt(sv.s(), QStringLiteral("solid.ez"), 0.5, 0.5, 0.5);
            const double wantY = 1e8 * (turned ? S12 + 0.5 * (S11 - S12 - S44 / 2) : S12), wantZ = 1e8 * S12;
            QVERIFY2(std::abs(ey - wantY) < 1e-6 * std::abs(wantY) && std::abs(ez - wantZ) < 1e-6 * std::abs(wantZ),
                     qPrintable(QStringLiteral("turned %1: εy %2 (%3), εz %4 (%5)").arg(turned).arg(ey).arg(wantY).arg(ez).arg(wantZ)));
        }
    }

    // Multigrid's conjugate gradients, the six rigid-body modes its near
    // null space, give a direct solve's field to its tolerance on a
    // cantilever, in under 150 iterations (94 here: a thin beam is hard on
    // aggregation).
    void iterativeAndDirect()
    {
        std::vector<double> fields[2];
        for (int direct : {0, 1}) {
            Model m = model3d(0.25);
            addBox(m, QStringLiteral("beam"), 0, 0, 0, 6, 1, 1);
            zDivisions(m, QStringLiteral("beam"), 4);
            const QString solid = addSolid(m, 2, {{QStringLiteral("E"), QStringLiteral("200[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.3")}});
            addFeature(m, solid, QStringLiteral("solid/fixed"), ofSide(QStringLiteral("beam"), QStringLiteral("left")));
            addFeature(m, solid, QStringLiteral("solid/load"), ofSide(QStringLiteral("beam"), QStringLiteral("top")), {{QStringLiteral("Fz"), QStringLiteral("-1[MPa]")}});
            stationary(m).props.insert(QStringLiteral("solver"), direct ? QStringLiteral("direct") : QStringLiteral("iterative"));
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            fields[direct] = sv.s().fields().front().values;
            if (!direct) {
                const QString log = sv.s().log.join(QLatin1Char('\n'));
                info(log);
                const QRegularExpressionMatch it = QRegularExpression(QStringLiteral("multigrid: (\\d+) iterations")).match(log);
                QVERIFY2(it.hasMatch() && it.captured(1).toInt() < 150, qPrintable(log));
                QVERIFY2(!log.contains(QStringLiteral("incomplete Cholesky")), qPrintable(log));
            }
        }
        double num = 0, den = 0;
        for (std::size_t i = 0; i < fields[0].size(); ++i) {
            num += (fields[0][i] - fields[1][i]) * (fields[0][i] - fields[1][i]);
            den += fields[1][i] * fields[1][i];
        }
        QVERIFY2(std::sqrt(num / den) < 1e-6, qPrintable(QString::number(std::sqrt(num / den))));
    }

    // Uniform in z - two materials side by side, its left face fixed, its
    // right pulled and sheared, warmed along x, its top and bottom on
    // rollers -: the 2D model in plane strain on the 3D model's own plan
    // mesh, at every degree of freedom (w nought), and its strain energy and
    // reactions, to 1e-9 - at order 1 and 2.
    void planeStrainInZ()
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
            const QJsonObject propsA{{QStringLiteral("E"), QStringLiteral("200[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.3")}, {QStringLiteral("alpha"), QStringLiteral("1e-5[1/K]")}};
            const QJsonObject propsB{{QStringLiteral("E"), QStringLiteral("70[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.33")}, {QStringLiteral("alpha"), QStringLiteral("2.3e-5[1/K]")}};
            addMaterial(m3, QStringLiteral("A"), propsA, objects(QStringLiteral("blk1")));
            addMaterial(m3, QStringLiteral("B"), propsB, objects(QStringLiteral("blk2")));
            const auto box2 = [](double x0, double y0, double x1, double y1) {
                return QJsonObject{{QStringLiteral("box"), QJsonArray{x0, y0, x1, y1}}};
            };
            addMaterial(m2, QStringLiteral("A"), propsA, box2(-0.01, -0.01, 1.01, 1.01));
            addMaterial(m2, QStringLiteral("B"), propsB, box2(0.99, -0.01, 2.01, 1.01));
            for (Model* m : {&m3, &m2}) {
                const bool three = m == &m3;
                Node p = m->make(QStringLiteral("solid"));
                p.props.insert(QStringLiteral("order"), QString::number(order));
                const QString solid = p.tag;
                m->add(m->component().tag, p);
                for (Node& c : m->find(solid)->children)
                    if (c.type == QLatin1String("solid/elastic")) {
                        c.props.insert(QStringLiteral("thermal"), true);
                        c.props.insert(QStringLiteral("T"), QStringLiteral("293.15[K] + 2e4[K/m]*x"));
                    }
                addFeature(*m, solid, QStringLiteral("solid/fixed"), three ? ofSide(QStringLiteral("blk1"), QStringLiteral("left")) : box2(-0.01, -0.01, 0.01, 1.01));
                addFeature(*m, solid, QStringLiteral("solid/load"), three ? ofSide(QStringLiteral("blk2"), QStringLiteral("right")) : box2(1.99, -0.01, 2.01, 1.01),
                           {{QStringLiteral("Fx"), QStringLiteral("50[MPa]")}, {QStringLiteral("Fy"), QStringLiteral("10[MPa]*y/1[mm]")}});
                if (three) {
                    addFeature(*m, solid, QStringLiteral("solid/roller"), QJsonObject{{QStringLiteral("normal"), QJsonArray{0.0, 0.0, 1.0}}});
                    addFeature(*m, solid, QStringLiteral("solid/roller"), QJsonObject{{QStringLiteral("normal"), QJsonArray{0.0, 0.0, -1.0}}});
                }
            }
            const Solved s3 = solve(m3);
            QVERIFY2(s3.error.isEmpty(), qPrintable(why(s3)));
            const StudyResult r2 = solveStudy(m2, evaluateParameters(m2), s3.mesh->plan, QString());
            QVERIFY2(r2.error.isEmpty() && r2.solution, qPrintable(r2.error));
            const Solution& two = *r2.solution;
            const Field3& f3 = s3.s().fields().front();
            const Field& f2 = two.fields().front();
            const int n3 = f3.space->size(), n2 = f2.space->size();
            QCOMPARE(n2, f3.space->plan().size());
            double worst = 0, scale = 0, wmax = 0;
            for (int dof = 0; dof < n3; ++dof) {
                const int pd = f3.space->origin(dof).first;
                for (int c = 0; c < 2; ++c) {
                    const double v2 = f2.values[std::size_t(c * n2 + pd)];
                    worst = std::max(worst, std::abs(f3.values[std::size_t(c * n3 + dof)] - v2));
                    scale = std::max(scale, std::abs(v2));
                }
                wmax = std::max(wmax, std::abs(f3.values[std::size_t(2 * n3 + dof)]));
            }
            QVERIFY2(worst <= 1e-9 * scale && wmax <= 1e-12 * scale, qPrintable(QStringLiteral("order %1: %2 off of %3, w %4").arg(order).arg(worst).arg(scale).arg(wmax)));
            for (const QString name : {QStringLiteral("solid.W"), QStringLiteral("solid.Rx"), QStringLiteral("solid.Ry"), QStringLiteral("solid.dmax")}) {
                const double a = global(s3.s(), name), b = two.global(name).value_or(std::nan(""));
                QVERIFY2(std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-9 * std::abs(b),
                         qPrintable(QStringLiteral("%1, order %2: 3D %3, 2D %4").arg(name).arg(order).arg(a, 0, 'g', 12).arg(b, 0, 'g', 12)));
            }
            // σz = ν (σx + σy) - E α ΔT of plane strain at a point.
            const double a = valueAt(s3.s(), QStringLiteral("solid.sz"), 0.4, 0.6, 0.25);
            PointEvaluator e2(two);
            int t = -1;
            double xi = 0, eta = 0;
            QVERIFY(Locator(*s3.mesh->plan).locate(QPointF(0.4, 0.6), &t, &xi, &eta));
            e2.moveTo(t, xi, eta);
            const double b = e2.eval(Expression::compile(QStringLiteral("solid.sz"), two.pointScope()));
            QVERIFY2(std::abs(a - b) <= 1e-6 * std::abs(b), qPrintable(QStringLiteral("σz 3D %1, 2D %2").arg(a).arg(b)));
        }
    }

    // A cantilever's natural frequencies, 10 mm by 1 mm square, steel (ν 0):
    // its first two - bending each way - Euler and Bernoulli's 1.8751² /(2π
    // L²) √(E I / (ρ A)) within 1.5 %; its next two 4.6941²'s within 5 %
    // (Timoshenko's shear and rotary inertia lower them more); then its
    // torsion, √(G J / (ρ Ip)) / (4 L), J 0.1406 a⁴ (Saint-Venant's square)
    // within 3 %, and its axial c/(4 L) within 1.5 % - each a snapshot of its
    // frequency, solid.freq.
    void cantileverModes()
    {
        const double L = 10e-3, a = 1e-3, E = 200e9, rho = 7850;
        const double I = a * a * a * a / 12, A = a * a, G = E / 2;
        const double f1 = 1.8751 * 1.8751 / (2 * M_PI * L * L) * std::sqrt(E * I / (rho * A));
        const double f2 = 4.6941 * 4.6941 / (2 * M_PI * L * L) * std::sqrt(E * I / (rho * A));
        const double ft = std::sqrt(G * 0.1406 * a * a * a * a / (rho * a * a * a * a / 6)) / (4 * L);
        const double fa = std::sqrt(E / rho) / (4 * L);
        Model m = model3d(slowBuild() ? 0.5 : 0.25);
        addBox(m, QStringLiteral("beam"), 0, 0, 0, 10, 1, 1);
        zDivisions(m, QStringLiteral("beam"), slowBuild() ? 2 : 4);
        const QString solid = addSolid(m, 2, {{QStringLiteral("E"), QStringLiteral("200[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0")},
                                              {QStringLiteral("rho"), QStringLiteral("7850[kg/m^3]")}});
        addFeature(m, solid, QStringLiteral("solid/fixed"), ofSide(QStringLiteral("beam"), QStringLiteral("left")));
        for (Node& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                c.children.clear();
                Node e = m.make(QStringLiteral("eigenfrequency"));
                e.props.insert(QStringLiteral("count"), 6);
                c.children.push_back(e);
            }
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        Solution3& s = sv.s();
        QVERIFY(s.eigen);
        QCOMPARE(s.snapshotCount(), 6);
        std::vector<double> f;
        for (int k = 0; k < 6; ++k) f.push_back(s.snapshotFrequency(k));
        const QString all = QStringLiteral("%1, %2, %3, %4, %5, %6 Hz; f1 %7, f2 %8, torsion %9, axial %10")
                                .arg(f[0]).arg(f[1]).arg(f[2]).arg(f[3]).arg(f[4]).arg(f[5]).arg(f1).arg(f2).arg(ft).arg(fa);
        QVERIFY2(std::abs(f[0] - f1) < 1.5e-2 * f1 && std::abs(f[1] - f1) < 1.5e-2 * f1, qPrintable(all));
        QVERIFY2(std::abs(f[2] - f2) < 5e-2 * f2 && std::abs(f[3] - f2) < 5e-2 * f2, qPrintable(all));
        QVERIFY2(std::abs(f[4] - ft) < 3e-2 * ft, qPrintable(all));
        QVERIFY2(std::abs(f[5] - fa) < 1.5e-2 * fa, qPrintable(all));
        // Its snapshots: the lowest selected, its globals of each.
        QCOMPARE(s.selected(), 0);
        QVERIFY(std::abs(global(s, QStringLiteral("solid.freq")) - f[0]) < 1e-9 * f[0]);
        s.select(5);
        QVERIFY(std::abs(global(s, QStringLiteral("solid.freq")) - f[5]) < 1e-9 * f[5]);
        // (The axial mode: its tip moves along x, scaled to 1.)
        QVERIFY2(std::abs(std::abs(valueAt(s, QStringLiteral("solid.u"), 10, 0.5, 0.5)) - 1) < 2e-2, qPrintable(QString::number(valueAt(s, QStringLiteral("solid.u"), 10, 0.5, 0.5))));
        info(all);
    }

    // A piezoelectric block - e33 15 C/m², a stiffness of E 100 GPa and ν 0,
    // εr 1000 - between a grounded bottom and a top at 10 V, free to grow:
    // its strain e33 E3 / C33 (its top moves -e33 V / C33), its charge the
    // free capacitance's (ε + e33²/C33) A/h, the terminal sweep's es.C_top_top the
    // clamped one, ε A/h - solid and electrostatics solved in turn.
    void piezoelectricBlock()
    {
        const double e33 = 15, C33 = 100e9, eps = 1000 * 8.8541878188e-12, V = 10, h = 1e-3, A = 1e-6;
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("pzt"), 0, 0, 0, 1, 1, 1);
        Node es = m.make(QStringLiteral("electrostatics"));
        es.props.insert(QStringLiteral("order"), QStringLiteral("1"));
        const QString et = es.tag;
        m.add(m.component().tag, es);
        for (Node& c : m.find(et)->children)
            if (c.type == QLatin1String("electrostatics/charge")) {
                c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
                c.props.insert(QStringLiteral("epsilonr"), QStringLiteral("1000"));
            }
        addFeature(m, et, QStringLiteral("electrostatics/ground"), ofSide(QStringLiteral("pzt"), QStringLiteral("bottom")));
        addFeature(m, et, QStringLiteral("electrostatics/terminal"), ofSide(QStringLiteral("pzt"), QStringLiteral("top")),
                   {{QStringLiteral("name"), QStringLiteral("top")}, {QStringLiteral("V0"), QStringLiteral("10[V]")}});
        stationary(m).props.insert(QStringLiteral("sweep"), true);
        const QString solid = addSolid(m, 1, {{QStringLiteral("E"), QStringLiteral("100[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0")}});
        for (const char* side : {"left", "front", "bottom"})
            addFeature(m, solid, QStringLiteral("solid/roller"), ofSide(QStringLiteral("pzt"), QString::fromLatin1(side)));
        addFeature(m, solid, QStringLiteral("solid/piezo"), rule(R"({"all": true})"),
                   {{QStringLiteral("es"), QJsonArray{et}}, {QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("e"), QStringLiteral("e33=15[C/m^2]")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double w = valueAt(sv.s(), QStringLiteral("solid.w"), 0.5, 0.5, 1);
        const double wantW = -e33 * V / C33;
        QVERIFY2(std::abs(w - wantW) < 1e-6 * std::abs(wantW), qPrintable(QStringLiteral("top w %1 m, -e33 V / C33 %2 m").arg(w).arg(wantW)));
        const double q = global(sv.s(), et + QStringLiteral(".Q_top"));
        const double free = (eps + e33 * e33 / C33) * A / h;
        QVERIFY2(std::abs(q / V - free) < 1e-6 * free, qPrintable(QStringLiteral("Q/V %1 F, free %2 F").arg(q / V).arg(free)));
        const double clamped = global(sv.s(), et + QStringLiteral(".C_top_top"));
        QVERIFY2(std::abs(clamped - eps * A / h) < 1e-6 * eps * A / h, qPrintable(QStringLiteral("C11 %1 F, clamped %2 F").arg(clamped).arg(eps * A / h)));
        // D = ε E + e S: its z component the charge's density.
        const double dz = valueAt(sv.s(), et + QStringLiteral(".Dz"), 0.5, 0.5, 0.5);
        QVERIFY2(std::abs(std::abs(dz) - free * V / A) < 1e-6 * free * V / A, qPrintable(QString::number(dz)));
    }

    // What is refused: a solid held nowhere; a domain without its elastic
    // properties. A 3D model's solid offers z's components, not the plane's
    // approximation, and is made of order 2; its estimate counts three
    // unknowns at each degree of freedom.
    void refusalsAndSchema()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
        QString solid = addSolid(m, 1, {{QStringLiteral("E"), QStringLiteral("1[GPa]")}, {QStringLiteral("nu"), QStringLiteral("0.3")}});
        Solved sv = solve(m);
        QVERIFY2(sv.error.contains(QStringLiteral("held nowhere")), qPrintable(sv.error));

        Model m2 = model3d(0.5);
        addBox(m2, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
        solid = addSolid(m2, 1);
        addFeature(m2, solid, QStringLiteral("solid/fixed"), ofSide(QStringLiteral("cube"), QStringLiteral("left")));
        sv = solve(m2);
        QVERIFY2(sv.error.contains(QStringLiteral("no material")), qPrintable(sv.error));

        Model m3 = Model::blank3d();
        Node p = m3.make(QStringLiteral("solid"));
        QCOMPARE(p.text(QStringLiteral("order")), QStringLiteral("2"));
        const NodeKind* disp = nodeKind(QStringLiteral("solid/displacement"));
        QVERIFY(disp && disp->property(QStringLiteral("w0")) && (disp->property(QStringLiteral("w0"))->spaces & Spaces::ThreeD));
        QVERIFY(!(disp->property(QStringLiteral("w0"))->spaces & Spaces::Plane));
        const NodeKind* s = nodeKind(QStringLiteral("solid"));
        QVERIFY(s && s->property(QStringLiteral("model")) && !(s->property(QStringLiteral("model"))->spaces & Spaces::ThreeD));
        QVERIFY(s->spaces & Spaces::ThreeD);
        QVERIFY(unsolvedPhysics3(m).isEmpty());

        // Estimates: three unknowns at each dof.
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        const GeometryBuild3 g = buildGeometry3(m, *ps);
        const Mesh3Build mb = buildMesh3(m, *ps, g.topology);
        QVERIFY(mb.mesh);
        const std::vector<Estimate3> e = estimate3(m, mb.mesh);
        QCOMPARE(int(e.size()), 1);
        QCOMPARE(e.front().unknowns, qint64(Space3(mb.mesh, 1).size()) * 3);
    }
};

QTEST_GUILESS_MAIN(TestFem3Solid)
#include "test_fem3_solid.moc"
