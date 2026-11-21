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
#include <QRegularExpression>
#include <QtTest>

#include <algorithm>
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

/// Whether the saddle point's LDLT may not be Accelerate's: off macOS (an
/// LU of Eigen's below MUMPS's size), QUCS_FEM_SOLVER=eigen, or a sanitized
/// build. There the meshes are coarser: by Eigen's LU of the complex
/// matrix the loop took 140 s and the solenoid 210 s on an M2 Ultra
/// (Accelerate's: 3.6 s), and CI's macOS runner, some 1.5 times slower, cut
/// the solenoid at 300 s; sanitized on CI's Linux job uniformInZ alone went
/// past 900 s. (Accelerate's real LDLT is macOS 10.13's: CI's macOS 15
/// too.)
bool coarse()
{
    return slowBuild() || choleskyBackend() != Backend::Accelerate;
}

/// A saddle point [K Bᵀ; B 0]: K a grid's Laplacian (\a side² unknowns, its
/// edge held), B a constraint on each of \a constraints pairs of them -
/// symmetric, indefinite, nonsingular, as the magnetostatics' gauge makes
/// its system.
Eigen::SparseMatrix<double> saddlePoint(int side, int constraints)
{
    const int n = side * side;
    std::vector<Eigen::Triplet<double>> t;
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j) {
            const int k = i * side + j;
            t.emplace_back(k, k, 4.0);
            if (i > 0) t.emplace_back(k, k - side, -1.0);
            if (i + 1 < side) t.emplace_back(k, k + side, -1.0);
            if (j > 0) t.emplace_back(k, k - 1, -1.0);
            if (j + 1 < side) t.emplace_back(k, k + 1, -1.0);
        }
    for (int c = 0; c < constraints; ++c) {
        const int p = (c * 7919) % n, q = (p + 1 + (c * 104729) % (n - 1)) % n;
        t.emplace_back(n + c, p, 1.0);
        t.emplace_back(p, n + c, 1.0);
        t.emplace_back(n + c, q, -0.5);
        t.emplace_back(q, n + c, -0.5);
    }
    Eigen::SparseMatrix<double> m(n + constraints, n + constraints);
    m.setFromTriplets(t.begin(), t.end());
    return m;
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

/// A cylinder of radius \a r from \a z0, \a h high.
void addCylinderAt(Model& m, const QString& label, double r, double z0, double h)
{
    Node n = m.make(QStringLiteral("cylinder"));
    n.label = label;
    n.props.insert(QStringLiteral("center"), pairOf(0, 0));
    n.props.insert(QStringLiteral("radius"), QString::number(r, 'g', 17));
    n.props.insert(QStringLiteral("z"), QString::number(z0, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    m.add(m.geometry().tag, n);
}

/// A coax \a len mm long shorted by an end cap 0.3 mm thick - its core 0.5
/// mm, its shell 1.5 to 1.8 mm, copper - fed at z = 0 between its core and
/// its shell by a currents physics, whose current a coil of a magnetic
/// physics reads: mf.L_1 its DC inductance. (A layer a millimetre.)
Model shortedCoax(double hmax, int len, QString* ecTag, QString* mfTag)
{
    Model m = model3d(hmax);
    addCylinderAt(m, QStringLiteral("shell"), 1.8, 0, len);
    addCylinderAt(m, QStringLiteral("gap"), 1.5, 0, len);
    addCylinderAt(m, QStringLiteral("core"), 0.5, 0, len);
    addCylinderAt(m, QStringLiteral("cap"), 1.8, len, 0.3);
    for (const char* o : {"shell", "gap", "core"}) zDivisions(m, QString::fromLatin1(o), len);
    zDivisions(m, QStringLiteral("cap"), 1);
    const QJsonObject metal{{QStringLiteral("only"), QJsonArray{QStringLiteral("shell"), QStringLiteral("core"), QStringLiteral("cap")}}};
    Node ec = m.make(QStringLiteral("currents"));
    ec.props.insert(QStringLiteral("order"), QStringLiteral("2"));
    ec.props.insert(QStringLiteral("selection"), metal);
    const QString et = ec.tag;
    m.add(m.component().tag, ec);
    for (Node& c : m.find(et)->children)
        if (c.type == QLatin1String("currents/conservation")) {
            c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            c.props.insert(QStringLiteral("sigma"), QStringLiteral("5.8e7[S/m]"));
        }
    // (The core's floor its terminal; the shell's own floor, an annulus -
    // its side rule's floors less the gap's and the core's -, its ground.)
    addFeature(m, et, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("core"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("1")}});
    QJsonObject ground = ofSide(QStringLiteral("shell"), QStringLiteral("bottom"));
    ground.insert(QStringLiteral("except"), ofSide(QStringLiteral("gap"), QStringLiteral("bottom")));
    addFeature(m, et, QStringLiteral("currents/ground"), ground);
    const QString mf = addMagnetic(m, 2);
    addFeature(m, mf, QStringLiteral("magnetic/coil"), metal,
               {{QStringLiteral("name"), QStringLiteral("1")}, {QStringLiteral("ec"), QJsonArray{et}}, {QStringLiteral("terminal"), QStringLiteral("1")}});
    *ecTag = et;
    *mfTag = mf;
    return m;
}

/// The lines of \a log that begin with \a name and hold \a text.
int linesOf(const QStringList& log, const QString& name, const QString& text)
{
    int n = 0;
    for (const QString& line : log) n += line.startsWith(name + QStringLiteral(": ")) && line.contains(text) ? 1 : 0;
    return n;
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

/// \a m solved (its progress told to \a progress).
Solved solve(const Model& m, const Progress& progress = {})
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
    out.r = solveStudy3(m, ps, mb.mesh, QString(), progress);
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
    // QUCS_TEST_PART 1 those before magneticsInTimeSine, 2 it and those
    // after (magnetic fields in time, the refusals) - by their place here,
    // so that one added lands in a part -, unset all. Together past 900 s
    // under ASan once fields in time came.
    void init()
    {
        const QByteArray part = qgetenv("QUCS_TEST_PART");
        if (part.isEmpty()) return;
        const QMetaObject* mo = metaObject();
        int second = -1, here = -1;
        for (int k = mo->methodOffset(); k < mo->methodCount(); ++k) {
            const QByteArray name = mo->method(k).name();
            if (name == "magneticsInTimeSine") second = k;
            if (name == QTest::currentTestFunction()) here = k;
        }
        if (second < 0 || here < 0) return;
        if (part != QByteArray::number(here >= second ? 2 : 1)) QSKIP("in the test's other part (QUCS_TEST_PART)");
    }

    // A sanitized build with MUMPS (CI's Linux job): its LDLTs MUMPS's
    // whatever their size, not only from 20,000 unknowns - the library is
    // the distribution's, optimized, where Eigen's LU is compiled into the
    // test at -O0 with the sanitizers, some hundreds of times slower; what
    // the sanitizers are to see is this engine's assembly, not the LU.
    void initTestCase()
    {
        if (slowBuild() && !qEnvironmentVariableIsSet("QUCS_FEM_MUMPS_FROM")) qputenv("QUCS_FEM_MUMPS_FROM", "0");
        // (Accelerate's likewise on a sanitized macOS build: a cycle's coax
        // of some 15,000 unknowns, below its size, took 217 s by Eigen's LU
        // at -O0, and two steps of it past QtTest's 300.)
        if (slowBuild() && !qEnvironmentVariableIsSet("QUCS_FEM_ACCELERATE_FROM")) qputenv("QUCS_FEM_ACCELERATE_FROM", "0");
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

    // Force Calculation, Maxwell's stress tensor over a body's faces with the
    // air round it, weighted (Arkkio's). A bar fed 2 A along z (an External
    // Current Density) in a uniform 0.5 T along y: its force I l B0 along x,
    // as the Lorentz force ∫ J × B of the same field - two ways, to 0.2 %
    // (0.0001 % found; its faces' integral alone, the first version, 2 %
    // off). In a Frequency Domain step at 50 Hz, nothing conducting, the time
    // average half of it. A rod magnetized
    // along x (Br 1 T, radius 1 mm, uniform in z) in a uniform B0 0.5 T
    // along y: its torque about z m × B0 = (Br/µ0) π a² h B0 - its own
    // field's image parallel to it, no torque -, along its axis the same,
    // to 2 %; its force nought (under 1 % of the torque over the radius).
    void forceByMaxwellStress()
    {
        for (bool harmonic : {false, true}) {
            Model m = model3d(0.2);
            addBox(m, QStringLiteral("air"), -2, -2, 0, 4, 4, 1);
            addBox(m, QStringLiteral("bar"), -0.2, -0.2, 0, 0.4, 0.4, 1);
            for (const char* o : {"air", "bar"}) zDivisions(m, QString::fromLatin1(o), 1);
            const QString mf = addMagnetic(m, 2);
            addFeature(m, mf, QStringLiteral("magnetic/current"), objects(QStringLiteral("bar")),
                       {{QStringLiteral("Jez"), QStringLiteral("2[A]/0.16[mm^2]")}});
            addFeature(m, mf, QStringLiteral("magnetic/potential"), rule(R"({"exterior": true})"), {{QStringLiteral("A0z"), QStringLiteral("-0.5[T]*x")}});
            addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, 1));
            addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, -1));
            addFeature(m, mf, QStringLiteral("magnetic/force"), objects(QStringLiteral("bar")), {{QStringLiteral("name"), QStringLiteral("bar")}});
            if (harmonic)
                for (Node& st : m.root.children)
                    if (st.type == QLatin1String("study")) {
                        for (Node& step : st.children) step.enabled = false;
                        Node fr = m.make(QStringLiteral("frequency"));
                        fr.props.insert(QStringLiteral("frequencies"), QStringLiteral("50[Hz]"));
                        st.children.push_back(fr);
                    }
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            // (J along z, B along y: J × B along -x, I l B0 = 1e-3 N; its time
            // average half of it.)
            const double share = harmonic ? 0.5 : 1.0;
            const double fx = global(sv.s(), QStringLiteral("mf.Fx_bar")), fy = global(sv.s(), QStringLiteral("mf.Fy_bar"));
            const double lorentz = harmonic ? -share * 1e-3 : -integral(sv.s(), QStringLiteral("mf.Jz*mf.By"));
            const QString said = QStringLiteral("a fed bar%1: Maxwell's (%2, %3) N, ∫ J × B %4 N, I l B0 %5 N")
                                     .arg(harmonic ? QStringLiteral(" at 50 Hz, its time average") : QString()).arg(fx).arg(fy).arg(lorentz).arg(-share * 1e-3);
            QVERIFY2(std::abs(fx - lorentz) < 2e-3 * std::abs(lorentz) && std::abs(fx + share * 1e-3) < 2e-3 * 1e-3 && std::abs(fy) < 2e-3 * 1e-3, qPrintable(said));
            info(said);
        }
        {
            const double a = 1, R = 6, h = 0.25, B0 = 0.5;
            Model m = model3d(0.25);
            addCylinder(m, QStringLiteral("air"), R, h);
            addCylinder(m, QStringLiteral("rod"), a, h);
            zDivisions(m, QStringLiteral("air"), 1);
            zDivisions(m, QStringLiteral("rod"), 1);
            const QString mf = addMagnetic(m, 2);
            addFeature(m, mf, QStringLiteral("magnetic/magnet"), objects(QStringLiteral("rod")),
                       {{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("Br"), QStringLiteral("1[T]")}, {QStringLiteral("dx"), QStringLiteral("1")},
                        {QStringLiteral("dy"), QStringLiteral("0")}, {QStringLiteral("dz"), QStringLiteral("0")}});
            addFeature(m, mf, QStringLiteral("magnetic/potential"), rule(R"({"exterior": true})"), {{QStringLiteral("A0z"), QStringLiteral("-0.5[T]*x")}});
            addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, 1));
            addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, -1));
            addFeature(m, mf, QStringLiteral("magnetic/force"), objects(QStringLiteral("rod")), {{QStringLiteral("name"), QStringLiteral("rod")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double want = 1 / Mu0 * M_PI * (a * 1e-3) * (a * 1e-3) * (h * 1e-3) * B0;
            const double tz = global(sv.s(), QStringLiteral("mf.Tz_rod")), t = global(sv.s(), QStringLiteral("mf.T_rod"));
            const double fx = global(sv.s(), QStringLiteral("mf.Fx_rod")), fy = global(sv.s(), QStringLiteral("mf.Fy_rod"));
            const QString said = QStringLiteral("a magnet in a field: torque %1 (along its axis %2) N m against m × B0 %3; force (%4, %5) N")
                                     .arg(tz).arg(t).arg(want).arg(fx).arg(fy);
            QVERIFY2(std::abs(tz - want) < 2e-2 * want && std::abs(t - tz) < 1e-12 * want && std::hypot(fx, fy) < 1e-2 * want / (a * 1e-3), qPrintable(said));
            info(said);
        }
        // A second fed bar beside it (the review of 8d5e9e9e): 0.4 mm apart,
        // air between, its force its own ∫ J × B; touching, its layer in the
        // other bar - not air -, said (its force taking part of the other's).
        for (bool touching : {false, true}) {
            Model m = model3d(0.2);
            addBox(m, QStringLiteral("air"), -2, -2, 0, 4, 4, 1);
            addBox(m, QStringLiteral("bar"), -0.2, -0.2, 0, 0.4, 0.4, 1);
            addBox(m, QStringLiteral("bar2"), touching ? 0.2 : 0.6, -0.2, 0, 0.4, 0.4, 1);
            for (const char* o : {"air", "bar", "bar2"}) zDivisions(m, QString::fromLatin1(o), 1);
            const QString mf = addMagnetic(m, 2);
            for (const char* o : {"bar", "bar2"})
                addFeature(m, mf, QStringLiteral("magnetic/current"), objects(QString::fromLatin1(o)), {{QStringLiteral("Jez"), QStringLiteral("2[A]/0.16[mm^2]")}});
            addFeature(m, mf, QStringLiteral("magnetic/potential"), rule(R"({"exterior": true})"), {{QStringLiteral("A0z"), QStringLiteral("-0.5[T]*x")}});
            addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, 1));
            addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, -1));
            addFeature(m, mf, QStringLiteral("magnetic/force"), objects(QStringLiteral("bar")), {{QStringLiteral("name"), QStringLiteral("bar")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double fx = global(sv.s(), QStringLiteral("mf.Fx_bar"));
            const double lorentz = -integral(sv.s(), QStringLiteral("mf.Jz*mf.By*(x < 0.2[mm])"));
            const bool warned = sv.r.warnings.join(QLatin1Char(' ')).contains(QStringLiteral("not air"));
            const QString said = QStringLiteral("two fed bars %1: Maxwell's %2 N, ∫ J × B over the first %3 N, warned %4")
                                     .arg(touching ? QStringLiteral("touching") : QStringLiteral("0.4 mm apart")).arg(fx).arg(lorentz).arg(warned);
            if (touching) QVERIFY2(warned, qPrintable(said));
            else QVERIFY2(!warned && std::abs(fx - lorentz) < 2e-3 * std::abs(lorentz), qPrintable(said));
            info(said);
        }
    }


    // A coil of 10 turns on a bar (0.4 mm square, 1 mm long, its currents
    // physics' J along it), 0.2 A, in a uniform 0.5 T across it: its wire's
    // resistance ∫ |j1|²/(σw fill) = N² l/(σw fill A) (σw 6e7 S/m, fill 0.5:
    // 0.0208333 Ω) at rest - its voltage R I0 -, and in the frequency domain
    // (50 Hz, no eddy currents in turns) V/I's real part the same; its
    // Lorentz force N I0 l B0 = 1 mN at rest, its time average half of it.
    void coilWireAndAveragedForce()
    {
        const double R = 100 * 1e-3 / (6e7 * 0.5 * 0.16e-6);
        for (bool harmonic : {false, true}) {
            Model m = model3d(0.2);
            addBox(m, QStringLiteral("air"), -2, -2, 0, 4, 4, 1);
            addBox(m, QStringLiteral("bar"), -0.2, -0.2, 0, 0.4, 0.4, 1);
            for (const char* o : {"air", "bar"}) zDivisions(m, QString::fromLatin1(o), 1);
            Node ec = m.make(QStringLiteral("currents"));
            ec.props.insert(QStringLiteral("order"), QStringLiteral("1"));
            ec.props.insert(QStringLiteral("selection"), objects(QStringLiteral("bar")));
            const QString et = ec.tag;
            m.add(m.component().tag, ec);
            for (Node& c : m.find(et)->children)
                if (c.type == QLatin1String("currents/conservation")) {
                    c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
                    c.props.insert(QStringLiteral("sigma"), QStringLiteral("6e7[S/m]"));
                }
            addFeature(m, et, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("a")}});
            addFeature(m, et, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar"), QStringLiteral("top")));
            const QString mf = addMagnetic(m, 2);
            addFeature(m, mf, QStringLiteral("magnetic/coil"), objects(QStringLiteral("bar")),
                       {{QStringLiteral("name"), QStringLiteral("1")}, {QStringLiteral("ec"), QJsonArray{et}}, {QStringLiteral("terminal"), QStringLiteral("a")},
                        {QStringLiteral("conductor"), QStringLiteral("multi")}, {QStringLiteral("N"), QStringLiteral("10")}, {QStringLiteral("I0"), QStringLiteral("0.2[A]")},
                        {QStringLiteral("sigmaw"), QStringLiteral("6e7[S/m]")}, {QStringLiteral("fill"), QStringLiteral("0.5")}});
            addFeature(m, mf, QStringLiteral("magnetic/potential"), rule(R"({"exterior": true})"), {{QStringLiteral("A0z"), QStringLiteral("0.5[T]*y")}});
            addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, 1));
            addFeature(m, mf, QStringLiteral("magnetic/insulation"), normal(0, 0, -1));
            if (harmonic)
                for (Node& st : m.root.children)
                    if (st.type == QLatin1String("study")) {
                        for (Node& step : st.children) step.enabled = false;
                        Node fr = m.make(QStringLiteral("frequency"));
                        fr.props.insert(QStringLiteral("frequencies"), QStringLiteral("50[Hz]"));
                        st.children.push_back(fr);
                    }
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double r = global(sv.s(), QStringLiteral("mf.R_1")), v = global(sv.s(), QStringLiteral("mf.V_1")), fy = global(sv.s(), QStringLiteral("mf.Fy_1"));
            const double share = harmonic ? 0.5 : 1.0;
            const QString said = QStringLiteral("a coil of turns%1: R %2 Ω against %3, V %4 V, Fy %5 N against %6")
                                     .arg(harmonic ? QStringLiteral(" at 50 Hz") : QString()).arg(r, 0, 'g', 8).arg(R, 0, 'g', 8).arg(v, 0, 'g', 8).arg(fy).arg(share * 1e-3);
            QVERIFY2(std::abs(r - R) < 1e-6 * R && std::abs(fy - share * 1e-3) < 2e-3 * 1e-3, qPrintable(said));
            if (!harmonic) QVERIFY2(std::abs(v - R * 0.2) < 1e-9 * R, qPrintable(said));
            // (Its impedance matrix's diagonal, the coil swept: R too.)
            else QVERIFY2(std::abs(global(sv.s(), QStringLiteral("mf.R11")) - R) < 1e-6 * R, qPrintable(said + QStringLiteral(", R11 %1").arg(global(sv.s(), QStringLiteral("mf.R11")))));
            info(said);
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
        // (0.08 where the LU is Eigen's: two solves 16.6 s there, 0.59 % off;
        // 0.05 took 72 s on an M2 Ultra and past QtTest's 300 on CI's macOS
        // runner, and 0.1 was 0.92 % off of the 1 % allowed.)
        Model m = model3d(coarse() ? 0.08 : 0.05);
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

    // The DC inductance of a coax from a currents physics through a coil
    // (nothing tested that path against an exact
    // answer): its length's dL/dlen, from two lengths (their end caps' and
    // feeds' the same), µ0/2π (ln(b/a) + 1/4 + c⁴ ln(c/b)/(c² - b²)² - (3c²
    // - b²)/(4 (c² - b²))). Each physics solved once - the coil's magnetic
    // field reads the currents, which read nothing of it: no iterations -,
    // and the magnetic field's saddle point factorized once.
    void dcInductanceFromCurrents()
    {
        const double a = 0.5e-3, b = 1.5e-3, c = 1.8e-3;
        const double Lp = Mu0 / (2 * M_PI)
                          * (std::log(b / a) + 0.25 + c * c * c * c * std::log(c / b) / ((c * c - b * b) * (c * c - b * b)) - (3 * c * c - b * b) / (4 * (c * c - b * b)));
        // (0.15 mm: +0.18 %; Eigen's LU and a sanitized build coarser, 0.25
        // mm: +0.40 % - 1.26 and 2.75 % low while the coil carried its
        // currents physics' J scaled to that physics' own current, its curved
        // section's, where this field's triangles are straight.)
        const double hmax = coarse() ? 0.25 : 0.15;
        double L[2] = {0, 0};
        const int lengths[2] = {2, 4};
        QStringList told;
        const Progress progress = [&told](double, const QString& what) {
            if (told.isEmpty() || told.back() != what) told << what;
            return true;
        };
        for (int k = 0; k < 2; ++k) {
            QString et, mf;
            const Model m = shortedCoax(hmax, lengths[k], &et, &mf);
            told.clear();
            const Solved sv = solve(m, progress);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            L[k] = global(sv.s(), QStringLiteral("mf.L_1"));
            const QStringList& log = sv.s().log;
            const QString mfName = m.find(mf)->name();
            QVERIFY2(!log.join(QLatin1Char('\n')).contains(QStringLiteral(", iteration 1:")), qPrintable(log.join(QLatin1Char('\n'))));
            QVERIFY2(linesOf(log, mfName, QStringLiteral("factorized in")) == 1, qPrintable(log.join(QLatin1Char('\n'))));
            QVERIFY2(linesOf(log, mfName, QStringLiteral("assembled in")) == 1, qPrintable(log.join(QLatin1Char('\n'))));
            // Its progress told each physics and its phase: the currents
            // solved, then the field factorized (no pass: solved once).
            const QString ecName = m.find(et)->name();
            const auto at = [&told](const QString& text) {
                for (int i = 0; i < told.size(); ++i)
                    if (told[i].contains(text)) return i;
                return -1;
            };
            const int currents = at(ecName + QStringLiteral(": solving")), factorizing = at(mfName + QStringLiteral(": factorizing, "));
            QVERIFY2(currents >= 0 && factorizing > currents && at(QStringLiteral("(pass")) < 0, qPrintable(told.join(QLatin1Char('\n'))));
            QVERIFY2(told[factorizing].contains(QStringLiteral(" unknowns")), qPrintable(told[factorizing]));
            // Its estimate (the review's finding 5: fitted to a real
            // Cholesky, 7 to 36 times low): its unknowns every edge's and
            // node's, the solve's fewer (those held on the insulated faces:
            // a third here, a tenth at a million); its factors those
            // Accelerate's analysis says, within a factor of two (the fit's
            // solves were larger, within 3 %).
            const Estimate3* est = nullptr;
            for (const Estimate3& e : sv.r.estimates)
                if (e.physics == mf) est = &e;
            QVERIFY(est);
            QRegularExpressionMatch unknowns, factors;
            for (const QString& line : log) {
                if (!unknowns.hasMatch()) unknowns = QRegularExpression(QStringLiteral("^%1: (\\d+) unknowns").arg(mfName)).match(line);
                if (!factors.hasMatch()) factors = QRegularExpression(QStringLiteral("its factors ([0-9.]+) MB")).match(line);
            }
            QVERIFY2(unknowns.hasMatch(), qPrintable(log.join(QLatin1Char('\n'))));
            const double solved = unknowns.captured(1).toDouble();
            QVERIFY2(est->unknowns >= solved && est->unknowns < 1.5 * solved, qPrintable(QStringLiteral("%1 estimated, %2 solved").arg(est->unknowns).arg(solved)));
            if (choleskyBackend() == Backend::Accelerate && factors.hasMatch() && !coarse()) {
                const double measured = factors.captured(1).toDouble() * 1048576.0, estimated = est->directBytes / 2;
                info(QStringLiteral("magnetic factors: %1 MB estimated, %2 MB by Accelerate's analysis").arg(estimated / 1048576.0).arg(measured / 1048576.0));
                QVERIFY2(estimated > 0.5 * measured && estimated < 2 * measured, qPrintable(QStringLiteral("%1 against %2 bytes").arg(estimated).arg(measured)));
            }
        }
        const double perLength = (L[1] - L[0]) / 2e-3;
        info(QStringLiteral("coax from currents: L %1, %2 H; dL/dlen %3 H/m, exact %4 H/m").arg(L[0]).arg(L[1]).arg(perLength).arg(Lp));
        QVERIFY2(std::abs(perLength - Lp) < (coarse() ? 1e-2 : 5e-3) * Lp, qPrintable(QStringLiteral("dL/dlen %1 H/m, exact %2 H/m").arg(perLength).arg(Lp)));
    }

    // A cycle - the currents' conductivity reads the magnetic field (nought
    // times |B|: the answer the same) -: the two in turn till they agree,
    // the magnetic matrix the same at each pass and factorized once, its
    // factors kept for the next.
    void aCycleFactorizesOnce()
    {
        QString et, mf;
        Model m = shortedCoax(0.3, 1, &et, &mf);
        for (Node& c : m.find(et)->children)
            if (c.type == QLatin1String("currents/conservation")) c.props.insert(QStringLiteral("sigma"), QStringLiteral("5.8e7[S/m]*(1 + 0*mf.normB/1[T])"));
        QStringList told;
        const Solved sv = solve(m, [&told](double, const QString& what) {
            told << what;
            return true;
        });
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const QStringList& log = sv.s().log;
        const QString mfName = m.find(mf)->name();
        QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("iteration 2")), qPrintable(log.join(QLatin1Char('\n'))));
        QVERIFY2(linesOf(log, mfName, QStringLiteral("factorized in")) == 1, qPrintable(log.join(QLatin1Char('\n'))));
        QVERIFY2(linesOf(log, mfName, QStringLiteral("factors kept")) >= 1, qPrintable(log.join(QLatin1Char('\n'))));
        // (Its progress: each pass said.)
        const QString all = told.join(QLatin1Char('\n'));
        QVERIFY2(all.contains(mfName + QStringLiteral(": factorizing, ")) && all.contains(QStringLiteral("(pass 1)")) && all.contains(QStringLiteral("(pass 2)")),
                 qPrintable(all));
        QVERIFY(global(sv.s(), QStringLiteral("mf.L_1")) > 0);
    }

    // Two Stationary steps, each solving the field of the same matrix: the
    // second factorizes its own - the first step's factors let go at its
    // end, not held through the rest of the study (two large ones at once).
    void factorsLetGoAtAStepsEnd()
    {
        QString et, mf;
        Model m = shortedCoax(0.3, 1, &et, &mf);
        for (Node& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                Node second = m.make(QStringLiteral("stationary"));
                c.children.push_back(second);
            }
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const QStringList& log = sv.s().log;
        const QString mfName = m.find(mf)->name();
        QVERIFY2(linesOf(log, mfName, QStringLiteral("factorized in")) == 2 && linesOf(log, mfName, QStringLiteral("factors kept")) == 0,
                 qPrintable(log.join(QLatin1Char('\n'))));
    }

    // The real LDLT of a saddle point the complex path's (the complex
    // symmetric LDLT of the same matrix, its imaginary parts nought, which
    // the magnetostatics took before) to rounding, by each backend this
    // build has - Accelerate's from its size (here from 0), MUMPS's, Eigen's
    // LU; its memory limit kept.
    void realLdltAgreesWithComplex()
    {
        // (Sanitized, Eigen's LU at -O0: a smaller one - 7,300 unknowns went
        // past QtTest's 300 s there.)
        const Eigen::SparseMatrix<double> a = slowBuild() ? saddlePoint(30, 120) : saddlePoint(80, 900);
        Eigen::VectorXd b(a.rows());
        for (int k = 0; k < int(b.size()); ++k) b[k] = std::sin(0.37 * k) + 0.25;
        // (QUCS_FEM_SOLVER, which overrides the choice, put aside for the
        // test's own.)
        // (Put back however the test ends: a failure returns at once.)
        struct Restore {
            QByteArray fromA = qgetenv("QUCS_FEM_ACCELERATE_FROM"), fromM = qgetenv("QUCS_FEM_MUMPS_FROM"), solver = qgetenv("QUCS_FEM_SOLVER");
            static void put(const char* name, const QByteArray& v)
            {
                if (v.isNull()) qunsetenv(name);
                else qputenv(name, v);
            }
            ~Restore()
            {
                setDirectSolver(DirectSolver::Automatic);
                put("QUCS_FEM_ACCELERATE_FROM", fromA);
                put("QUCS_FEM_MUMPS_FROM", fromM);
                put("QUCS_FEM_SOLVER", solver);
            }
        } restore;
        qputenv("QUCS_FEM_ACCELERATE_FROM", "0");
        qputenv("QUCS_FEM_MUMPS_FROM", "0");
        qunsetenv("QUCS_FEM_SOLVER");
        std::vector<std::pair<DirectSolver, QString>> backends{{DirectSolver::Eigen, QStringLiteral("Eigen's LU")}};
        if (accelerateInBuild()) backends.emplace_back(DirectSolver::Accelerate, QStringLiteral("Accelerate's LDLT"));
        if (mumpsInBuild()) backends.emplace_back(DirectSolver::Mumps, QStringLiteral("MUMPS's LDLT"));
        for (const auto& [solver, name] : backends) {
            setDirectSolver(solver);
            SparseRealLdlt real;
            real.compute(a);
            QVERIFY2(real.info() == Eigen::Success, qPrintable(name + QStringLiteral(": ") + QString::fromStdString(real.lastErrorMessage())));
            QCOMPARE(real.name(), name);
            SparseComplexLdlt complex;
            complex.compute(a.cast<std::complex<double>>());
            QVERIFY2(complex.info() == Eigen::Success, qPrintable(QString::fromStdString(complex.lastErrorMessage())));
            const Eigen::VectorXd x = real.solve(b), y = complex.solve(b.cast<std::complex<double>>()).real();
            const double gap = (x - y).norm() / y.norm(), residual = (b - a * x).norm() / b.norm();
            info(QStringLiteral("%1: |x - x_complex| / |x| %2, residual %3").arg(name).arg(gap).arg(residual));
            QVERIFY2(gap < 1e-10 && residual < 1e-10, qPrintable(QStringLiteral("%1: %2, %3").arg(name).arg(gap).arg(residual)));
            // (Many right-hand sides at once: each the one's.)
            Eigen::MatrixXd B(a.rows(), 2);
            B.col(0) = b;
            B.col(1) = 2 * b;
            const Eigen::MatrixXd X = real.solveMany(B);
            QVERIFY((X.col(0) - x).norm() < 1e-12 * x.norm() && (X.col(1) - 2 * x).norm() < 1e-12 * x.norm());
            // The memory limit (where the backend says its factors' size).
            if (solver != DirectSolver::Eigen) {
                QVERIFY(real.factorBytes() > 0);
                SparseRealLdlt small;
                small.setMemoryLimit(1024);
                small.compute(a);
                QVERIFY(small.info() != Eigen::Success);
                QVERIFY2(QString::fromStdString(small.lastErrorMessage()).contains(QStringLiteral("past the limit")), small.lastErrorMessage().c_str());
            }
        }
        setDirectSolver(DirectSolver::Automatic);
        // (Accelerate's analysis kept for the same pattern.)
        if (accelerateInBuild()) {
            SparseRealLdlt again;
            again.compute(a);
            QVERIFY(again.reusedAnalysis());
        }
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
        // (Each Newton's step's tangent another matrix: each factorized, none
        // solved with the factors kept from the step before.)
        const QString mfName = m.find(mf)->name();
        const int steps = int(std::count_if(sv.s().log.begin(), sv.s().log.end(),
                                            [&mfName](const QString& l) { return l.startsWith(mfName + QStringLiteral(", Newton's step")); }));
        QVERIFY2(steps >= 2 && linesOf(sv.s().log, mfName, QStringLiteral("factorized in")) == steps
                     && linesOf(sv.s().log, mfName, QStringLiteral("factors kept")) == 0,
                 qPrintable(sv.s().log.join(QLatin1Char('\n'))));
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


    // Magnetic fields in time (a Time Dependent step: eddy currents σ (∂A/∂t
    // + ∇V), the frequency domain's matrices stepped by BDF). The shorted
    // coax (copper, its core 0.5 mm: µ0 σ a² 18 µs) fed 1 A at 20 kHz,
    // sin(ωt) from rest: once its start has died away (its slowest mode some
    // 3 µs, magneticsInTimeSteps), its voltage the frequency domain's Re(V̂
    // e^{jωt}) at 20 kHz - its skin effect's resistance and inductance - at
    // four times of its second period, to 2 % of |V̂| (0.5 % found); its
    // losses some.
    void magneticsInTimeSine()
    {
        using cd = std::complex<double>;
        const double f = 20e3, w = 2 * M_PI * f;
        QString et, mf;
        auto coax = [&] { return shortedCoax(slowBuild() ? 0.4 : 0.3, 1, &et, &mf); };
        auto coilOf = [&](Model& m) -> Node* {
            for (Node& c : m.find(mf)->children)
                if (c.type == QLatin1String("magnetic/coil")) return &c;
            return nullptr;
        };
        auto stepOf = [&](Model& m, const QString& type, const QString& key, const QString& value) {
            for (Node& c : m.root.children)
                if (c.type == QLatin1String("study")) {
                    c.children.clear();
                    Node st = m.make(type);
                    st.props.insert(key, value);
                    // (Fixed steps of 1 µs, a period's 50th - a sanitized
                    // build's 2 µs: BDF2's matrix the same at each step
                    // after its first two - factorized three times.)
                    if (type == QLatin1String("transient")) {
                        st.props.insert(QStringLiteral("stepping"), QStringLiteral("fixed"));
                        st.props.insert(QStringLiteral("dt"), slowBuild() ? QStringLiteral("2[us]") : QStringLiteral("1[us]"));
                    }
                    c.children.push_back(st);
                }
        };
        // The frequency domain at 20 kHz.
        Model mh = coax();
        coilOf(mh)->props.insert(QStringLiteral("I0"), QStringLiteral("1[A]"));
        stepOf(mh, QStringLiteral("frequency"), QStringLiteral("frequencies"), QStringLiteral("20[kHz]"));
        const Solved sh = solve(mh);
        QVERIFY2(sh.error.isEmpty(), qPrintable(why(sh)));
        const cd V(global(sh.s(), QStringLiteral("mf.V_1")), global(sh.s(), QStringLiteral("mf.V_1_im")));
        // In time, sin(ωt) = Re(-j e^{jωt}): its voltage Re(-j V̂ e^{jωt}).
        Model mt = coax();
        coilOf(mt)->props.insert(QStringLiteral("I0"), QStringLiteral("1[A]*sin(2*pi*20[kHz]*t)"));
        stepOf(mt, QStringLiteral("transient"), QStringLiteral("times"), QStringLiteral("0, 40[us], 45[us], 50[us], 55[us]"));
        const Solved st = solve(mt);
        QVERIFY2(st.error.isEmpty(), qPrintable(why(st)));
        Solution3& s = st.s();
        QCOMPARE(s.snapshotCount(), 5);
        QString said = QStringLiteral("V̂ at 20 kHz %1%2j V;").arg(V.real()).arg(V.imag(), 0, 'g', 6, QLatin1Char('+'));
        for (int k = 1; k < 5; ++k) {
            s.select(k);
            const double t = s.time(), want = (cd(0, -1) * V * std::exp(cd(0, w * t))).real(), got = global(s, QStringLiteral("mf.V_1"));
            const double I = global(s, QStringLiteral("mf.I_1"));
            said += QStringLiteral(" t %1 µs: %2 V against %3 (I %4 A);").arg(t * 1e6).arg(got, 0, 'g', 6).arg(want, 0, 'g', 6).arg(I, 0, 'g', 6);
            // (A sanitized build's coarser steps: 3 %.)
            QVERIFY2(std::abs(got - want) < (slowBuild() ? 3e-2 : 2e-2) * std::abs(V) && std::abs(I - std::sin(w * t)) < 1e-9, qPrintable(said));
            QVERIFY(global(s, QStringLiteral("mf.P")) > 0);
        }
        info(said);
    }

    // A step of 1 A into the shorted coax: its voltage at first its core's
    // skin's, higher, falling to its DC resistance's (the frequency domain's
    // at 10 Hz) - its slowest mode µ0 σ a²/α1², α1 J0's first zero, some 3
    // µs -: to 1e-3 at 50 µs. Fed a voltage instead, its current found.
    void magneticsInTimeSteps()
    {
        QString et, mf;
        auto coax = [&] { return shortedCoax(slowBuild() ? 0.4 : 0.3, 1, &et, &mf); };
        auto coilOf = [&](Model& m) -> Node* {
            for (Node& c : m.find(mf)->children)
                if (c.type == QLatin1String("magnetic/coil")) return &c;
            return nullptr;
        };
        auto stepOf = [&](Model& m, const QString& type, const QString& key, const QString& value) {
            for (Node& c : m.root.children)
                if (c.type == QLatin1String("study")) {
                    c.children.clear();
                    Node st = m.make(type);
                    st.props.insert(key, value);
                    // (Fixed steps of 1 µs, a period's 50th - a sanitized
                    // build's 2 µs: BDF2's matrix the same at each step
                    // after its first two - factorized three times.)
                    if (type == QLatin1String("transient")) {
                        st.props.insert(QStringLiteral("stepping"), QStringLiteral("fixed"));
                        st.props.insert(QStringLiteral("dt"), slowBuild() ? QStringLiteral("2[us]") : QStringLiteral("1[us]"));
                    }
                    c.children.push_back(st);
                }
        };
        Model mr = coax();
        coilOf(mr)->props.insert(QStringLiteral("I0"), QStringLiteral("1[A]"));
        stepOf(mr, QStringLiteral("frequency"), QStringLiteral("frequencies"), QStringLiteral("10[Hz]"));
        const Solved rest = solve(mr);
        QVERIFY2(rest.error.isEmpty(), qPrintable(why(rest)));
        const double R = global(rest.s(), QStringLiteral("mf.R_1"));
        Model ms = coax();
        coilOf(ms)->props.insert(QStringLiteral("I0"), QStringLiteral("1[A]"));
        stepOf(ms, QStringLiteral("transient"), QStringLiteral("times"), QStringLiteral("0, 2[us], 50[us]"));
        const Solved sst = solve(ms);
        QVERIFY2(sst.error.isEmpty(), qPrintable(why(sst)));
        // (Its values at the start in its steps' layout - a step's error
        // reads them against its own.)
        const int fi = sst.s().fieldIndex(mf);
        sst.s().select(0);
        const std::size_t first = sst.s().fields()[std::size_t(fi)].values.size();
        sst.s().select(1);
        QVERIFY2(first > 0 && first == sst.s().fields()[std::size_t(fi)].values.size(),
                 qPrintable(QStringLiteral("%1 values at the start, %2 after").arg(first).arg(sst.s().fields()[std::size_t(fi)].values.size())));
        const double early = global(sst.s(), QStringLiteral("mf.V_1"));
        sst.s().select(2);
        const double late = global(sst.s(), QStringLiteral("mf.V_1"));
        const QString said2 = QStringLiteral("a step of 1 A: %1 V at 2 µs, %2 V at 50 µs against its DC resistance's %3 V").arg(early).arg(late).arg(R);
        QVERIFY2(std::abs(late - R) < 1e-3 * R && early > 1.2 * late, qPrintable(said2));
        info(said2);
        // Fed a voltage instead (its role), R volts reached in a ramp over 10
        // µs (a voltage of t: its part of each step's right-hand side
        // assembled again at each): its current found, rising to 1 A (its
        // L/R some 6 µs) - to 1e-3 at 100 µs.
        Model mv = coax();
        coilOf(mv)->props.insert(QStringLiteral("role"), QStringLiteral("voltage"));
        coilOf(mv)->props.insert(QStringLiteral("V0"), QStringLiteral("%1[V]*((t < 10[us])*t/10[us] + (t >= 10[us]))").arg(R, 0, 'g', 17));
        stepOf(mv, QStringLiteral("transient"), QStringLiteral("times"), QStringLiteral("0, 2[us], 100[us]"));
        const Solved sv = solve(mv);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        sv.s().select(1);
        const double i0 = global(sv.s(), QStringLiteral("mf.I_1"));
        sv.s().select(2);
        const double i1 = global(sv.s(), QStringLiteral("mf.I_1")), v1 = global(sv.s(), QStringLiteral("mf.V_1"));
        const QString said3 = QStringLiteral("fed R volts by a ramp: its current %1 A at 2 µs, %2 A at 100 µs (its voltage %3 V)").arg(i0).arg(i1).arg(v1);
        QVERIFY2(std::abs(i1 - 1) < 1e-3 && i0 < 0.2 && std::abs(v1 - R) < 1e-12 * R, qPrintable(said3));
        info(said3);
    }

    // The iron ring in the frequency domain (50 Hz, nothing conducting): its
    // B-H curve by its effective permeability - each element's B̂/(µ0 H1),
    // H1 the fundamental of H for a sinusoidal B of amplitude B̂ -, solved in
    // turn. Round the wire H's amplitude is N Î/(2π r) whatever the iron, so
    // B̂ there is the root of H1(B̂) = Ĥ (H1 worked out here from the curve):
    // at three radii to 2 % (0.04 % found, µr at each quadrature point; its
    // element's middle's alone, 2.9 % off in saturation), and H's amplitude
    // there Ĥ (to 5 %: the point's B over its nearest point's µr).
    void ironRingInFrequency()
    {
        // (A sanitized build: a coarser mesh, µr to a hundredth.)
        Model m = model3d(slowBuild() ? 0.15 : 0.1);
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
        for (Node& st : m.root.children)
            if (st.type == QLatin1String("study")) {
                for (Node& step : st.children) step.enabled = false;
                Node fr = m.make(QStringLiteral("frequency"));
                fr.props.insert(QStringLiteral("frequencies"), QStringLiteral("50[Hz]"));
                if (slowBuild()) fr.props.insert(QStringLiteral("tolerance"), QStringLiteral("1e-2"));
                st.children.push_back(fr);
            }
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        QString err;
        const std::shared_ptr<const BHCurve> curve = BHCurve::make({{0, 0}, {0.5, 100}, {1.0, 300}, {1.4, 1500}, {1.7, 10000}, {2.0, 100000}}, &err);
        QVERIFY(curve);
        // H1(B̂), the fundamental by the trapezoid rule on a fine grid.
        auto h1 = [&](double bhat) {
            const int n = 2000;
            double sum = 0;
            for (int k = 1; k < n; ++k) {
                const double th = M_PI * k / n;
                sum += curve->H(bhat * std::sin(th)) * std::sin(th);
            }
            return 2 / M_PI * sum * (M_PI / n);
        };
        const double current = integral(sv.s(), QStringLiteral("mf.Jz")) / 0.3e-3;
        QString said = sv.s().log.filter(QStringLiteral("effective permeability")).join(QLatin1Char(' '));
        for (double r : {0.7, 0.9, 1.1}) {
            const double H = current / (2 * M_PI * r * 1e-3);
            double lo = 0, hi = 3;
            for (int i = 0; i < 100; ++i) ((h1(0.5 * (lo + hi)) < H) ? lo : hi) = 0.5 * (lo + hi);
            const double want = 0.5 * (lo + hi);
            const double got = valueAt(sv.s(), QStringLiteral("mf.normB"), r * std::cos(0.4), r * std::sin(0.4), 0.15);
            const double hgot = valueAt(sv.s(), QStringLiteral("mf.normH"), r * std::cos(0.4), r * std::sin(0.4), 0.15);
            said += QStringLiteral("; r %1 mm: Ĥ %2 A/m (%3), B̂ %4 T against %5").arg(r).arg(H).arg(hgot).arg(got).arg(want);
            // (H the point's own B over its nearest quadrature point's µr: 5 %.)
            QVERIFY2(std::abs(got - want) < 2e-2 * want && std::abs(hgot - H) < 5e-2 * H, qPrintable(said));
        }
        info(said);
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
