/*
 * test_fem3_magnetic.cpp - magnetic fields solved in 3D (multiphysics/
 *                          fem3_magnetic.cpp): a model uniform in z the 2D
 *                          model of its plan (B and the energy, to rounding),
 *                          an infinite solenoid's µ0 J t, a cylinder of µr in
 *                          a uniform field and a magnetized one (exactly, for
 *                          their bounded domain), a coaxial line's
 *                          inductance, a wire's Lorentz force in a field
 *                          with its current from a currents physics, the
 *                          coils' inductance matrix, an iron ring's B-H
 *                          curve by Newton's method, and what is refused
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
#include "fem_sparse.h"

#include <QJsonDocument>
#include <QtTest>

#include <cmath>

using namespace qucs_s::fem;

namespace {

constexpr double Mu0 = 1.25663706127e-6;

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
}

QJsonObject rule(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

QJsonObject ofSide(const QString& object, const QString& side)
{
    return QJsonObject{{QStringLiteral("of"), QJsonArray{object}}, {QStringLiteral("side"), side}};
}

QJsonObject objects(const QString& object)
{
    return QJsonObject{{QStringLiteral("objects"), QJsonArray{object}}};
}

QJsonObject normal(double x, double y, double z)
{
    return QJsonObject{{QStringLiteral("normal"), QJsonArray{x, y, z}}};
}

/// Whether this build is a sanitized one (QUCS_TEST_SLOW: its path in any).
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

/// Whether the saddle point's LDLT is not Accelerate's: Eigen's LU of it
/// (CI's macOS 15, Linux below MUMPS's size, QUCS_FEM_SOLVER=eigen), or a
/// sanitized build. There the meshes are coarser: by Eigen's LU the loop
/// took 140 s and the solenoid 210 s on an M2 Ultra (Accelerate's: 3.6 s),
/// and CI's macOS runner, some 1.5 times slower, cut the solenoid at 300 s;
/// sanitized on CI's Linux job uniformInZ alone went past 900 s.
bool coarse()
{
    return slowBuild() || !acceleratedLu();
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

/// Divisions along z of the model's layers (\a object's).
void zDivisions(Model& m, const QString& object, int count)
{
    Node z = m.make(QStringLiteral("mesh/zdistribution"));
    z.props.insert(QStringLiteral("selection"), objects(object));
    z.props.insert(QStringLiteral("count"), count);
    m.add(m.mesh().tag, z);
}

/// A magnetic physics of \a order on every domain, its default Ampère's Law
/// the user's µr \a mur, its default insulation kept (\a insulated) or not.
QString addMagnetic(Model& m, int order, const QString& mur = QStringLiteral("1"), bool insulated = true)
{
    Node p = m.make(QStringLiteral("magnetic"));
    p.props.insert(QStringLiteral("order"), QString::number(order));
    const QString tag = p.tag;
    m.add(m.component().tag, p);
    Node* n = m.find(tag);
    for (Node& c : n->children) {
        if (c.type == QLatin1String("magnetic/ampere")) {
            c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            c.props.insert(QStringLiteral("mur"), mur);
        }
        if (c.type == QLatin1String("magnetic/insulation") && !insulated) c.enabled = false;
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

/// A domain feature on \a object of user's \a props (µr...).
void addAmpere(Model& m, const QString& physics, const QString& object, const QJsonObject& props)
{
    QJsonObject all = props;
    all.insert(QStringLiteral("source"), QStringLiteral("user"));
    addFeature(m, physics, QStringLiteral("magnetic/ampere"), objects(object), all);
}

Node& firstStep(Model& m)
{
    for (Node& c : m.root.children)
        if (c.type == QLatin1String("study"))
            for (Node& s : c.children) return s;
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

/// ∫ \a text dV over the prisms of domains whose expression \a where is
/// not nought (SI).
double integral(const Solution3& s, const QString& text, const QString& where = QStringLiteral("1"))
{
    const Expression e = Expression::compile(text, s.scope()), w = Expression::compile(where, s.scope());
    const Space3& sp = *s.fields().front().space;
    PointEvaluator3 ev(s);
    double sum = 0;
    for (int el = 0; el < sp.elements(); ++el)
        for (const auto& q : prismRule(4, 3).points) {
            const Space3::Map m = sp.map(el, q[0], q[1], q[2]);
            ev.at(el, q[0], q[1], q[2], m.x);
            if (ev.eval(w) == 0) continue;
            const double v = ev.eval(e);
            if (std::isfinite(v)) sum += q[3] * std::abs(m.det) * v;
        }
    return sum;
}

QString why(const Solved& sv)
{
    return sv.error + QLatin1Char('\n') + sv.r.log.join(QLatin1Char('\n'));
}

void info(const QString& what)
{
    qInfo("%s", qPrintable(what));
}

} // namespace

class TestFem3Magnetic : public QObject
{
    Q_OBJECT

private slots:
    // A sanitized build with MUMPS (CI's Linux job): its LDLTs MUMPS's
    // whatever their size, not only from 20,000 unknowns - the library is
    // the distribution's, optimized, where Eigen's LU is compiled into the
    // test at -O0 with the sanitizers, some hundreds of times slower; what
    // the sanitizers are to see is this engine's assembly, not the LU.
    void initTestCase()
    {
        if (slowBuild() && !qEnvironmentVariableIsSet("QUCS_FEM_MUMPS_FROM")) qputenv("QUCS_FEM_MUMPS_FROM", "0");
    }

    // A model uniform in z - an iron block of µr 100 beside a bar carrying
    // 1e8 A/m² along z, in air, its outer walls and its top and bottom
    // insulated -: the 2D model of its plan on the 3D model's own plan mesh,
    // B and Az at points and the energy to 1e-6, A along z alone - at order 1 and 2
    // (Az's nodal functions the 3D edge space's own along z).
    void uniformInZ()
    {
        for (int order : {1, 2}) {
            // (Coarser where the LU is Eigen's: the 2D model is solved on the
            // 3D model's own plan, so its answer is still the 3D one's.)
            Model m3 = model3d(coarse() ? 0.5 : 0.25);
            addBox(m3, QStringLiteral("air"), 0, 0, 0, 4, 3, 0.5);
            addBox(m3, QStringLiteral("iron"), 0.5, 0.5, 0, 1, 2, 0.5);
            addBox(m3, QStringLiteral("bar"), 2.25, 1, 0, 0.5, 1, 0.5);
            // (Two layers: a level between them whose multiplier is free.)
            for (const char* o : {"air", "iron", "bar"}) zDivisions(m3, QString::fromLatin1(o), 2);
            Model m2 = Model::blank();
            m2.component().props.insert(QStringLiteral("thickness"), QStringLiteral("0.5[mm]"));
            for (Model* m : {&m3, &m2}) {
                const bool three = m == &m3;
                if (!three) {
                    for (const auto& [label, x, y, w, d] : {std::tuple{QStringLiteral("air"), 0.0, 0.0, 4.0, 3.0}, std::tuple{QStringLiteral("iron"), 0.5, 0.5, 1.0, 2.0},
                                                            std::tuple{QStringLiteral("bar"), 2.25, 1.0, 0.5, 1.0}}) {
                        Node r = m->make(QStringLiteral("rectangle"));
                        r.label = label;
                        r.props.insert(QStringLiteral("position"), pairOf(x, y));
                        r.props.insert(QStringLiteral("size"), pairOf(w, d));
                        m->add(m->geometry().tag, r);
                    }
                }
                const QString mf = addMagnetic(*m, order);
                if (three) {
                    addAmpere(*m, mf, QStringLiteral("iron"), {{QStringLiteral("mur"), QStringLiteral("100")}});
                    addFeature(*m, mf, QStringLiteral("magnetic/current"), objects(QStringLiteral("bar")), {{QStringLiteral("Jez"), QStringLiteral("1e8[A/m^2]")}});
                } else {
                    const auto box2 = [](double x0, double y0, double x1, double y1) { return QJsonObject{{QStringLiteral("box"), QJsonArray{x0, y0, x1, y1}}}; };
                    QJsonObject a = {{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("mur"), QStringLiteral("100")}};
                    addFeature(*m, mf, QStringLiteral("magnetic/ampere"), box2(0.49, 0.49, 1.51, 2.51), a);
                    addFeature(*m, mf, QStringLiteral("magnetic/current"), box2(2.24, 0.99, 2.76, 2.01), {{QStringLiteral("Je"), QStringLiteral("1e8[A/m^2]")}});
                }
            }
            const Solved s3 = solve(m3);
            QVERIFY2(s3.error.isEmpty(), qPrintable(why(s3)));
            const StudyResult r2 = solveStudy(m2, evaluateParameters(m2), s3.mesh->plan, QString());
            QVERIFY2(r2.error.isEmpty() && r2.solution, qPrintable(r2.error));
            const Solution& two = *r2.solution;
            const double w3 = global(s3.s(), QStringLiteral("mf.W")), w2 = two.global(QStringLiteral("mf.W")).value_or(std::nan(""));
            info(QStringLiteral("order %1: W 3D %2 J, 2D %3 J").arg(order).arg(w3, 0, 'g', 12).arg(w2, 0, 'g', 12));
            QVERIFY2(std::abs(w3 - w2) < 1e-6 * std::abs(w2), qPrintable(QStringLiteral("order %1: W 3D %2, 2D %3").arg(order).arg(w3, 0, 'g', 12).arg(w2, 0, 'g', 12)));
            Locator loc(*s3.mesh->plan);
            for (const auto& [x, y] : {std::pair{1.0, 1.5}, std::pair{2.5, 1.5}, std::pair{3.3, 0.4}, std::pair{1.8, 2.7}}) {
                int t = -1;
                double xi = 0, eta = 0;
                QVERIFY(loc.locate(QPointF(x, y), &t, &xi, &eta));
                PointEvaluator e2(two);
                e2.moveTo(t, xi, eta);
                // (A too, not only B: the gauge holds it, ∇·A = 0, so it is 2D's
                // Az - a gauge of x and y alone left A's gradients along z free.)
                for (const char* c : {"Bx", "By", "Az"}) {
                    const QString v = QStringLiteral("mf.") + QString::fromLatin1(c);
                    const double b3 = valueAt(s3.s(), v, x, y, 0.25), b2 = e2.eval(Expression::compile(v, two.pointScope()));
                    QVERIFY2(std::abs(b3 - b2) < 1e-6 * std::max(1e-3, std::abs(b2)), qPrintable(QStringLiteral("order %1, (%2, %3): %4 3D %5, 2D %6").arg(order).arg(x).arg(y).arg(v).arg(b3).arg(b2)));
                }
                QVERIFY2(std::abs(valueAt(s3.s(), QStringLiteral("mf.Bz"), x, y, 0.25)) < 1e-9, "Bz");
                QVERIFY2(std::abs(valueAt(s3.s(), QStringLiteral("mf.Ax"), x, y, 0.25)) < 1e-12, "Ax");
            }
        }
    }

    // A current loop in the x-z plane - J = ∇ × (0, s, 0)'s, s a bump of x
    // and z, free of divergence -, in an insulated box of several layers:
    // A's divergence (by central differences) under a fifth of its terms,
    // the gauge's (A varies along z here, as no model uniform in z has it);
    // order 1's energy under order 2's (a conforming method's rises to the
    // field's), within 15 %.
    void aLoopAcrossTheLayers()
    {
        double energy[3] = {0, 0, 0};
        for (int order : {1, 2}) {
            // (0.23 where the LU is Eigen's: 62 s there, the gauge's worst
            // point 0.06 of its terms; at 0.25 one point read 0.185 of the
            // 0.2 allowed, at 0.3 past it.)
            Model m = model3d(coarse() ? 0.23 : 0.2);
            addBox(m, QStringLiteral("air"), 0, 0, 0, 2, 2, 2);
            addBox(m, QStringLiteral("coil"), 0.5, 0.75, 0.5, 1, 0.5, 1);
            zDivisions(m, QStringLiteral("air"), 4);
            zDivisions(m, QStringLiteral("coil"), 5);
            const QString mf = addMagnetic(m, order);
            const QString X = QStringLiteral("pi*(x-0.5[mm])/1[mm]"), Z = QStringLiteral("pi*(z-0.5[mm])/1[mm]");
            addFeature(m, mf, QStringLiteral("magnetic/current"), objects(QStringLiteral("coil")),
                       {{QStringLiteral("Jex"), QStringLiteral("-1e8[A/m^2]*sin(%1)^2*2*sin(%2)*cos(%2)").arg(X, Z)},
                        {QStringLiteral("Jez"), QStringLiteral("1e8[A/m^2]*2*sin(%1)*cos(%1)*sin(%2)^2").arg(X, Z)}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            energy[order] = global(sv.s(), QStringLiteral("mf.W"));
            if (order != 2) continue;
            const double d = 0.03;
            for (const auto& [x, y, z] : {std::tuple{0.8, 1.0, 0.7}, std::tuple{1.3, 0.9, 1.2}, std::tuple{1.2, 1.1, 0.6}, std::tuple{0.7, 0.8, 1.3}}) {
                const double dax = valueAt(sv.s(), QStringLiteral("mf.Ax"), x + d, y, z) - valueAt(sv.s(), QStringLiteral("mf.Ax"), x - d, y, z);
                const double day = valueAt(sv.s(), QStringLiteral("mf.Ay"), x, y + d, z) - valueAt(sv.s(), QStringLiteral("mf.Ay"), x, y - d, z);
                const double daz = valueAt(sv.s(), QStringLiteral("mf.Az"), x, y, z + d) - valueAt(sv.s(), QStringLiteral("mf.Az"), x, y, z - d);
                const double div = (dax + day + daz) / (2 * d * 1e-3);
                // (Beside its terms: their sum nought where they cancel.)
                const double terms = (std::abs(dax) + std::abs(day) + std::abs(daz)) / (2 * d * 1e-3);
                info(QStringLiteral("loop: at (%1, %2, %3) ∇·A %4, its terms' sizes %5: %6").arg(x).arg(y).arg(z).arg(div).arg(terms).arg(std::abs(div) / terms));
                QVERIFY2(std::abs(div) < 0.2 * terms, qPrintable(QStringLiteral("(%1, %2, %3): ∇·A %4, its terms %5").arg(x).arg(y).arg(z).arg(div).arg(terms)));
            }
        }
        info(QStringLiteral("loop: W order 1 %1 J, order 2 %2 J").arg(energy[1]).arg(energy[2]));
        // (A conforming method's energy rises to the field's as it refines.)
        QVERIFY2(energy[2] > 0 && energy[1] < energy[2] && energy[1] > 0.85 * energy[2], qPrintable(QStringLiteral("%1, %2 J").arg(energy[1]).arg(energy[2])));
    }

    // An infinite solenoid's section - a shell 1 to 1.1 mm of 1e8 A/m²
    // round z, every face natural (n × H = 0: its ends and its far wall, the
    // field outside nought) -: B in its bore µ0 J t along z within 1 %, and
    // nought outside it.
    void infiniteSolenoid()
    {
        // (Its shell several elements thick: at 0.1 mm, one, its two polygons'
        // chords - the edge elements' arcs - left its thickness 0.5 % off.)
        // (0.12 where the LU is Eigen's: 23 s there, 0.07 % off; at 0.1 its
        // one element's chords 0.5 % - the arcs fall as they fall.)
        Model m = model3d(coarse() ? 0.12 : 0.07);
        addCylinder(m, QStringLiteral("world"), 1.6, 0.4);
        addCylinder(m, QStringLiteral("outer"), 1.1, 0.4);
        addCylinder(m, QStringLiteral("bore"), 1.0, 0.4);
        // (Uniform in z: one layer.)
        for (const char* o : {"world", "outer", "bore"}) zDivisions(m, QString::fromLatin1(o), 1);
        const QString mf = addMagnetic(m, 2, QStringLiteral("1"), false);
        addFeature(m, mf, QStringLiteral("magnetic/current"), QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("outer")}}, {QStringLiteral("except"), objects(QStringLiteral("bore"))}},
                   {{QStringLiteral("Jex"), QStringLiteral("-1e8[A/m^2]*y/sqrt(x^2+y^2)")}, {QStringLiteral("Jey"), QStringLiteral("1e8[A/m^2]*x/sqrt(x^2+y^2)")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double want = Mu0 * 1e8 * 0.1e-3;
        const double centre = valueAt(sv.s(), QStringLiteral("mf.Bz"), 0.05, 0.02, 0.2), off = valueAt(sv.s(), QStringLiteral("mf.Bz"), 0.6, -0.3, 0.1);
        info(QStringLiteral("solenoid: Bz %1 and %2 T, µ0 J t %3 T; outside %4 T").arg(centre).arg(off).arg(want).arg(valueAt(sv.s(), QStringLiteral("mf.normB"), 1.4, 0.2, 0.2)));
        QVERIFY2(std::abs(centre - want) < 1e-2 * want && std::abs(off - want) < 1e-2 * want, qPrintable(QStringLiteral("%1, %2 T, µ0 J t %3 T").arg(centre).arg(off).arg(want)));
        QVERIFY2(valueAt(sv.s(), QStringLiteral("mf.normB"), 1.4, 0.2, 0.2) < 2e-2 * want, "outside");
    }

    // A cylinder of µr 50, 1 mm, in air out to 6 mm, B0 1 T across it held
    // at the outer wall (A = B0 y ẑ): B inside uniform, 2 B0 / ((1 + 1/µr)
    // + (a/R)² (1 - 1/µr)) - the bounded domain's exact field; a magnetized
    // cylinder of Br 1 T along x, µr 1, A nought at the wall: Br/2 (1 -
    // (a/R)²).
    void cylinderInAField()
    {
        const double a = 1, R = 6, mur = 50;
        for (int magnet : {0, 1}) {
            Model m = model3d(0.25);
            addCylinder(m, QStringLiteral("air"), R, 0.25);
            addCylinder(m, QStringLiteral("rod"), a, 0.25);
            zDivisions(m, QStringLiteral("air"), 1);
            zDivisions(m, QStringLiteral("rod"), 1);
            const QString mf = addMagnetic(m, 2);
            if (magnet) {
                addFeature(m, mf, QStringLiteral("magnetic/magnet"), objects(QStringLiteral("rod")),
                           {{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("Br"), QStringLiteral("1[T]")}, {QStringLiteral("dx"), QStringLiteral("1")},
                            {QStringLiteral("dy"), QStringLiteral("0")}, {QStringLiteral("dz"), QStringLiteral("0")}});
            } else {
                addAmpere(m, mf, QStringLiteral("rod"), {{QStringLiteral("mur"), QString::number(mur)}});
                addFeature(m, mf, QStringLiteral("magnetic/potential"), rule(R"({"exterior": true})"),
                           {{QStringLiteral("A0z"), QStringLiteral("1[T]*y")}});
                // (Its top and bottom insulated again: A has no part across z there.)
                addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, 1));
                addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, -1));
            }
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double want = magnet ? 0.5 * (1 - (a / R) * (a / R)) : 2 / ((1 + 1 / mur) + (a / R) * (a / R) * (1 - 1 / mur));
            for (const auto& [x, y] : {std::pair{0.0, 0.0}, std::pair{0.4, -0.3}, std::pair{-0.2, 0.6}}) {
                const double bx = valueAt(sv.s(), QStringLiteral("mf.Bx"), x + 0.013, y + 0.017, 0.12), by = valueAt(sv.s(), QStringLiteral("mf.By"), x + 0.013, y + 0.017, 0.12);
                info(QStringLiteral("%1: B (%2, %3) T, exact %4 T").arg(magnet ? QStringLiteral("magnet") : QStringLiteral("µr 50")).arg(bx).arg(by).arg(want));
                QVERIFY2(std::abs(bx - want) < 1e-2 * want && std::abs(by) < 1e-2 * want, qPrintable(QStringLiteral("(%1, %2): B (%3, %4), exact %5").arg(x).arg(y).arg(bx).arg(by).arg(want)));
            }
            if (magnet) {
                // H inside the magnet against B: -(Br - B)/µ0.
                const double hx = valueAt(sv.s(), QStringLiteral("mf.Hx"), 0.1, 0.1, 0.12);
                QVERIFY2(std::abs(hx + (1 - want) / Mu0) < 2e-2 * (1 - want) / Mu0, qPrintable(QString::number(hx)));
            }
        }
    }

    // A coaxial line, 1 mm long: 1 A along z in its core (0.5 mm) and back
    // in its shell (1.5 to 1.8 mm), its outer wall and ends insulated: its
    // energy L' l I²/2, L' µ0/2π (ln(b/a) + 1/4 + c⁴ ln(c/b)/(c² - b²)² -
    // (3c² - b²)/(4 (c² - b²))), each current density the polygon's own
    // area's.
    void coaxialInductance()
    {
        const double a = 0.5e-3, b = 1.5e-3, c = 1.8e-3, len = 1e-3;
        Model m = model3d(0.05);
        addCylinder(m, QStringLiteral("shell"), 1.8, 1);
        addCylinder(m, QStringLiteral("gap"), 1.5, 1);
        addCylinder(m, QStringLiteral("core"), 0.5, 1);
        for (const char* o : {"shell", "gap", "core"}) zDivisions(m, QString::fromLatin1(o), 1);
        const QString mf = addMagnetic(m, 2);
        // (The current over each conductor's polygon: computed below, its
        // densities 1 A over them.)
        addFeature(m, mf, QStringLiteral("magnetic/current"), objects(QStringLiteral("core")), {{QStringLiteral("Jez"), QStringLiteral("Jc")}});
        addFeature(m, mf, QStringLiteral("magnetic/current"), QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("shell")}}, {QStringLiteral("except"), objects(QStringLiteral("gap"))}},
                   {{QStringLiteral("Jez"), QStringLiteral("-Js")}});
        // First pass: the polygons' areas, by a solve of nought current.
        QList<Model::Parameter> ps;
        Model::Parameter jc, js;
        jc.name = QStringLiteral("Jc");
        js.name = QStringLiteral("Js");
        jc.expression = QStringLiteral("1[A/m^2]");
        js.expression = QStringLiteral("1[A/m^2]");
        m.setParameters({jc, js});
        Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double areaCore = integral(sv.s(), QStringLiteral("mf.Jz"), QStringLiteral("mf.Jz > 0")) / len;
        const double areaShell = -integral(sv.s(), QStringLiteral("mf.Jz"), QStringLiteral("mf.Jz < 0")) / len;
        jc.expression = QStringLiteral("%1[A/m^2]").arg(1 / areaCore, 0, 'g', 17);
        js.expression = QStringLiteral("%1[A/m^2]").arg(1 / areaShell, 0, 'g', 17);
        m.setParameters({jc, js});
        sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double Lp = Mu0 / (2 * M_PI)
                          * (std::log(b / a) + 0.25 + c * c * c * c * std::log(c / b) / ((c * c - b * b) * (c * c - b * b)) - (3 * c * c - b * b) / (4 * (c * c - b * b)));
        const double L = 2 * global(sv.s(), QStringLiteral("mf.W")) / 1.0;
        info(QStringLiteral("coax: L %1 H, L' l %2 H").arg(L).arg(Lp * len));
        QVERIFY2(std::abs(L - Lp * len) < 1e-2 * Lp * len, qPrintable(QStringLiteral("L %1 H, analytic %2 H").arg(L).arg(Lp * len)));
    }

    // A bar 1 mm long carrying 2 A along z - its current a currents
    // physics' between its ends, read by a coil -, in B0 0.5 T along x held
    // at the walls: its Lorentz force I l B0 along y (its own field's
    // nought); a second bar beside it: the coils' inductance matrix
    // symmetric and positive definite.
    void coilsFromCurrents()
    {
        Model m = model3d(0.2);
        addBox(m, QStringLiteral("air"), -2, -2, 0, 4, 4, 1);
        addBox(m, QStringLiteral("bar"), -0.2, -0.2, 0, 0.4, 0.4, 1);
        addBox(m, QStringLiteral("bar2"), 0.8, -0.2, 0, 0.4, 0.4, 1);
        for (const char* o : {"air", "bar", "bar2"}) zDivisions(m, QString::fromLatin1(o), 1);
        Node ec = m.make(QStringLiteral("currents"));
        ec.props.insert(QStringLiteral("order"), QStringLiteral("1"));
        ec.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("bar"), QStringLiteral("bar2")}}});
        const QString et = ec.tag;
        m.add(m.component().tag, ec);
        for (Node& c : m.find(et)->children)
            if (c.type == QLatin1String("currents/conservation")) {
                c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
                c.props.insert(QStringLiteral("sigma"), QStringLiteral("6e7[S/m]"));
            }
        addFeature(m, et, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("a")}});
        addFeature(m, et, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar2"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
        addFeature(m, et, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar"), QStringLiteral("top")));
        addFeature(m, et, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar2"), QStringLiteral("top")));
        const QString mf = addMagnetic(m, 2);
        addFeature(m, mf, QStringLiteral("magnetic/coil"), objects(QStringLiteral("bar")),
                   {{QStringLiteral("name"), QStringLiteral("1")}, {QStringLiteral("ec"), QJsonArray{et}}, {QStringLiteral("terminal"), QStringLiteral("a")}, {QStringLiteral("I0"), QStringLiteral("2[A]")}});
        addFeature(m, mf, QStringLiteral("magnetic/coil"), objects(QStringLiteral("bar2")),
                   {{QStringLiteral("name"), QStringLiteral("2")}, {QStringLiteral("ec"), QJsonArray{et}}, {QStringLiteral("terminal"), QStringLiteral("b")}, {QStringLiteral("I0"), QStringLiteral("0[A]")}});
        addFeature(m, mf, QStringLiteral("magnetic/potential"), rule(R"({"exterior": true})"),
                   {{QStringLiteral("A0z"), QStringLiteral("0.5[T]*y")}});
        addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, 1));
        addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, -1));
        firstStep(m).props.insert(QStringLiteral("sweep"), true);
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double fy = global(sv.s(), QStringLiteral("mf.Fy_1")), fx = global(sv.s(), QStringLiteral("mf.Fx_1"));
        const double want = 2 * 1e-3 * 0.5;
        info(QStringLiteral("coil force (%1, %2) N, I l B0 %3 N").arg(fx).arg(fy).arg(want));
        // Its current density N I0 over the bar's section: 2 A / 0.16 mm².
        const double jz = valueAt(sv.s(), QStringLiteral("mf.Jz"), 0, 0, 0.5);
        QVERIFY2(std::abs(jz - 2 / 0.16e-6) < 1e-6 * 2 / 0.16e-6, qPrintable(QStringLiteral("Jz %1 A/m², I0/A %2").arg(jz).arg(2 / 0.16e-6)));
        // (Its own field's force on it nought; the second bar, unfed, adds
        // none.)
        QVERIFY2(std::abs(fy - want) < 1e-3 * want && std::abs(fx) < 1e-3 * want, qPrintable(QStringLiteral("(%1, %2) N, I l B0 %3 N").arg(fx).arg(fy).arg(want)));
        const double l11 = global(sv.s(), QStringLiteral("mf.L11")), l12 = global(sv.s(), QStringLiteral("mf.L12")), l21 = global(sv.s(), QStringLiteral("mf.L21")),
                     l22 = global(sv.s(), QStringLiteral("mf.L22"));
        info(QStringLiteral("L: %1 %2; %3 %4 H").arg(l11).arg(l12).arg(l21).arg(l22));
        // (Symmetric, positive definite: the two bars at different places
        // in the box, each its own.)
        QVERIFY2(l11 > 0 && l22 > 0 && std::abs(l12 - l21) < 1e-6 * l11 && l12 > 0 && l12 * l12 < l11 * l22,
                 qPrintable(QStringLiteral("%1 %2; %3 %4").arg(l11).arg(l12).arg(l21).arg(l22)));
    }

    // An iron ring round a wire of 50 A, its B-H curve's: H round it I /
    // (2π r) whatever the iron, B the curve's of it - by Newton's method,
    // within 1.5 % at three radii; the wire's current its polygon's.
    void ironRing()
    {
        Model m = model3d(0.1);
        addCylinder(m, QStringLiteral("world"), 1.5, 0.3);
        addCylinder(m, QStringLiteral("ringout"), 1.2, 0.3);
        addCylinder(m, QStringLiteral("ringin"), 0.6, 0.3);
        addCylinder(m, QStringLiteral("wire"), 0.3, 0.3);
        for (const char* o : {"world", "ringout", "ringin", "wire"}) zDivisions(m, QString::fromLatin1(o), 1);
        const QString mf = addMagnetic(m, 2);
        addFeature(m, mf, QStringLiteral("magnetic/ampere"), QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("ringout")}}, {QStringLiteral("except"), objects(QStringLiteral("ringin"))}},
                   {{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("constitutive"), QStringLiteral("bh")},
                    {QStringLiteral("BH"), QJsonArray{QJsonArray{0.0, 0.0}, QJsonArray{0.5, 100.0}, QJsonArray{1.0, 300.0}, QJsonArray{1.4, 1500.0}, QJsonArray{1.7, 10000.0}, QJsonArray{2.0, 100000.0}}}});
        addFeature(m, mf, QStringLiteral("magnetic/current"), objects(QStringLiteral("wire")), {{QStringLiteral("Jez"), QStringLiteral("50[A]/(pi*(0.3[mm])^2)")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        QVERIFY2(sv.s().log.join(QLatin1Char('\n')).contains(QStringLiteral("Newton's step 2")), qPrintable(sv.s().log.join(QLatin1Char('\n'))));
        const double current = integral(sv.s(), QStringLiteral("mf.Jz")) / 0.3e-3;
        QString err;
        const std::shared_ptr<const BHCurve> curve =
            BHCurve::make({{0, 0}, {0.5, 100}, {1.0, 300}, {1.4, 1500}, {1.7, 10000}, {2.0, 100000}}, &err);
        QVERIFY(curve);
        for (double r : {0.7, 0.9, 1.1}) {
            const double H = current / (2 * M_PI * r * 1e-3);
            // (B of H: the curve inverted by bisection.)
            double lo = 0, hi = 3;
            for (int i = 0; i < 100; ++i) ((curve->H(0.5 * (lo + hi)) < H) ? lo : hi) = 0.5 * (lo + hi);
            const double want = 0.5 * (lo + hi);
            const double got = valueAt(sv.s(), QStringLiteral("mf.normB"), r * std::cos(0.4), r * std::sin(0.4), 0.15);
            info(QStringLiteral("ring: r %1 mm, H %2 A/m, |B| %3 T, curve %4 T").arg(r).arg(H).arg(got).arg(want));
            QVERIFY2(std::abs(got - want) < 1.5e-2 * want, qPrintable(QStringLiteral("r %1: |B| %2 T, the curve's %3 T").arg(r).arg(got).arg(want)));
        }
    }

    // What is refused: a coil whose currents physics is not there; Magnetic
    // Fields offered in 3D, its 3D components and not the plane's.
    void refusalsAndSchema()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("cube"), 0, 0, 0, 1, 1, 1);
        const QString mf = addMagnetic(m, 1);
        addFeature(m, mf, QStringLiteral("magnetic/coil"), objects(QStringLiteral("cube")), {{QStringLiteral("ec"), QJsonArray{QStringLiteral("ec")}}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.contains(QStringLiteral("which the model does not solve")), qPrintable(sv.error));
        QVERIFY(unsolvedPhysics3(m).isEmpty());
        const NodeKind* cur = nodeKind(QStringLiteral("magnetic/current"));
        QVERIFY(cur && cur->property(QStringLiteral("Jez")) && !(cur->property(QStringLiteral("Jez"))->spaces & Spaces::Plane));
        QVERIFY(cur->property(QStringLiteral("Je")) && !(cur->property(QStringLiteral("Je"))->spaces & Spaces::ThreeD));
        QVERIFY(nodeKind(QStringLiteral("magnetic"))->spaces & Spaces::ThreeD);
    }
};

QTEST_GUILESS_MAIN(TestFem3Magnetic)
#include "test_fem3_magnetic.moc"
