/*
 * test_fem3_eddy.cpp - magnetic fields in 3D in the frequency domain
 *                      (multiphysics/fem3_magnetic.cpp's harmonic solve):
 *                      eddy currents jωσA, an electric potential on the
 *                      conductors' nodes, a single conductor fed a current
 *                      or held at a voltage at its currents physics'
 *                      terminal - a round wire's internal impedance against
 *                      Bessel's, a coax's against its exact one, the
 *                      impedance matrix symmetric and its roles, a disc's
 *                      eddy currents against the 2D axisymmetric solver's,
 *                      the losses the coils' I²R, a core's µr'' losses, a
 *                      heat physics of the eddy currents' losses, the
 *                      skin depth's warning, the coils' Touchstone file,
 *                      the iterative solve against the direct one
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
#include "fem_results.h"
#include "fem_solver.h"
#include "fem_sparse.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <complex>

using namespace qucs_s::fem;

namespace {

constexpr double Mu0 = 1.25663706127e-6;
using cd = std::complex<double>;

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
}

QJsonObject rule(const char* json)
{
    return QJsonDocument::fromJson(json).object();
}

QJsonObject ofSide(const QString& object, const QString& side)
{
    return QJsonObject{{QStringLiteral("of"), object}, {QStringLiteral("side"), side}};
}

QJsonObject objects(const QString& object)
{
    return QJsonObject{{QStringLiteral("objects"), QJsonArray{object}}};
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

/// Whether the complex LDLT may not be Accelerate's (off macOS: an LU of
/// Eigen's below MUMPS's size), or the build is sanitized: coarser meshes.
bool coarse()
{
    return slowBuild() || choleskyBackend() != Backend::Accelerate;
}

Model model3d(double hmax)
{
    Model m = Model::blank3d();
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QString::number(hmax, 'g', 17));
    return m;
}

/// A cylinder about (\a x, \a y) of radius \a r from \a z0, \a h high (mm).
void addCylinderAt(Model& m, const QString& label, double x, double y, double r, double z0, double h)
{
    Node n = m.make(QStringLiteral("cylinder"));
    n.label = label;
    n.props.insert(QStringLiteral("center"), pairOf(x, y));
    n.props.insert(QStringLiteral("radius"), QString::number(r, 'g', 17));
    n.props.insert(QStringLiteral("z"), QString::number(z0, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    m.add(m.geometry().tag, n);
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

void zDivisions(Model& m, const QString& object, int count)
{
    Node z = m.make(QStringLiteral("mesh/zdistribution"));
    z.props.insert(QStringLiteral("selection"), objects(object));
    z.props.insert(QStringLiteral("count"), count);
    m.add(m.mesh().tag, z);
}

void addFeature(Model& m, const QString& physics, const QString& type, const QJsonObject& selection, const QJsonObject& props = {})
{
    Node f = m.make(type);
    f.props.insert(QStringLiteral("selection"), selection);
    for (auto it = props.begin(); it != props.end(); ++it) f.props.insert(it.key(), it.value());
    QString why;
    QVERIFY2(m.add(physics, f, -1, &why) != nullptr, qPrintable(why));
}

/// A currents physics on \a selection of conductivity \a sigma.
QString addCurrents(Model& m, const QJsonObject& selection, const QString& sigma, int order = 2)
{
    Node ec = m.make(QStringLiteral("currents"));
    ec.props.insert(QStringLiteral("order"), QString::number(order));
    ec.props.insert(QStringLiteral("selection"), selection);
    const QString tag = ec.tag;
    m.add(m.component().tag, ec);
    for (Node& c : m.find(tag)->children)
        if (c.type == QLatin1String("currents/conservation")) {
            c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            c.props.insert(QStringLiteral("sigma"), sigma);
        }
    return tag;
}

/// A magnetic physics of order 2 on every domain, its default Ampère's Law
/// the user's µr 1 (no conductivity of its own: a coil's conductor takes its
/// currents physics').
QString addMagnetic(Model& m)
{
    Node p = m.make(QStringLiteral("magnetic"));
    p.props.insert(QStringLiteral("order"), QStringLiteral("2"));
    const QString tag = p.tag;
    m.add(m.component().tag, p);
    for (Node& c : m.find(tag)->children)
        if (c.type == QLatin1String("magnetic/ampere")) {
            c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            c.props.insert(QStringLiteral("mur"), QStringLiteral("1"));
        }
    return tag;
}

/// The study's steps: a Frequency Domain step of \a frequencies alone (its
/// coil sweep \a sweep; its frequency sweep \a fastSweep, exact unless
/// asked - each test's own solves).
void frequencyStep(Model& m, const QString& frequencies, bool sweep = true, const QString& fastSweep = QStringLiteral("exact"))
{
    for (Node& c : m.root.children)
        if (c.type == QLatin1String("study")) {
            c.children.erase(std::remove_if(c.children.begin(), c.children.end(), [](const Node& s) { return s.type == QLatin1String("frequency"); }),
                             c.children.end());
            for (Node& s : c.children) s.enabled = false;
            Node f = m.make(QStringLiteral("frequency"));
            f.props.insert(QStringLiteral("frequencies"), frequencies);
            f.props.insert(QStringLiteral("sweep"), sweep);
            f.props.insert(QStringLiteral("fastsweep"), fastSweep);
            c.children.push_back(f);
        }
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
    out.r = solveStudy3(m, ps, mb.mesh, QString(), {});
    out.error = out.r.error;
    return out;
}

double global(const Solution3& s, const QString& name)
{
    return s.global(name).value_or(std::nan(""));
}

double valueAt(const Solution3& s, const QString& text, double x, double y, double z)
{
    PointEvaluator3 ev(s);
    const double k = s.mesh().unitScale;
    if (!ev.at(Vec3{x * k, y * k, z * k})) return std::nan("");
    return ev.eval(Expression::compile(text, s.pointScope()));
}

/// ∫ \a text dV over the prisms where \a where is not nought (SI).
double integral(const Solution3& s, const QString& text, const QString& where = QStringLiteral("1"))
{
    const Expression e = Expression::compile(text, s.scope()), w = Expression::compile(where, s.scope());
    const Space3& sp = *s.fields().back().space;
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

/// Bessel's J0 and J1 of a complex argument, by their series (|z| to 20).
void bessel01(cd z, cd* j0, cd* j1)
{
    const cd h = z / 2.0, h2 = h * h;
    cd t0 = 1, t1 = h, s0 = 0, s1 = 0;
    for (int k = 0; k < 200; ++k) {
        s0 += t0;
        s1 += t1;
        t0 *= -h2 / double((k + 1) * (k + 1));
        t1 *= -h2 / double((k + 1) * (k + 2));
        if (std::abs(t0) < 1e-18 * std::abs(s0) && std::abs(t1) < 1e-18 * std::abs(s1)) break;
    }
    *j0 = s0;
    *j1 = s1;
}

/// Bessel's Y0 and Y1 of a complex argument, by their series (|z| to 20):
/// Y0 = (2/π)(ln(z/2) + γ) J0 + (2/π) Σ (-1)^(k+1) H_k (z²/4)^k/(k!)², Y1 =
/// -2/(πz) + (2/π) ln(z/2) J1 - (1/π) Σ (-1)^k (ψ(k+1) + ψ(k+2)) (z/2)^(2k+1)
/// / (k! (k+1)!).
void bessel01y(cd z, cd* y0, cd* y1)
{
    constexpr double gamma = 0.57721566490153286;
    cd j0, j1;
    const auto series = [](cd zz, cd* a, cd* b) {
        const cd h = zz / 2.0, h2 = h * h;
        cd t0 = 1, t1 = h, s0 = 0, s1 = 0;
        for (int k = 0; k < 200; ++k) {
            s0 += t0;
            s1 += t1;
            t0 *= -h2 / double((k + 1) * (k + 1));
            t1 *= -h2 / double((k + 1) * (k + 2));
            if (std::abs(t0) < 1e-18 * std::abs(s0) && std::abs(t1) < 1e-18 * std::abs(s1)) break;
        }
        *a = s0;
        *b = s1;
    };
    series(z, &j0, &j1);
    const cd h = z / 2.0, h2 = h * h, lg = std::log(h);
    cd s0 = 0, s1 = 0, t0 = h2, t1 = h;
    double H = 0;   // H_k
    double psi1 = -gamma, psi2 = 1 - gamma;   // ψ(k + 1), ψ(k + 2)
    for (int k = 0; k < 200; ++k) {
        // Y1's term k: (-1)^k (ψ(k+1) + ψ(k+2)) h^(2k+1) / (k! (k+1)!).
        s1 += (psi1 + psi2) * t1;
        if (k >= 1) {
            H += 1.0 / k;
            s0 += (k % 2 == 1 ? 1.0 : -1.0) * H * t0;
            t0 *= h2 / double((k + 1) * (k + 1));
        }
        t1 *= -h2 / double((k + 1) * (k + 2));
        psi1 += 1.0 / (k + 1);
        psi2 += 1.0 / (k + 2);
        if (k > 2 && std::abs(t1) < 1e-18 && std::abs(t0) < 1e-18) break;
    }
    *y0 = (2 / M_PI) * (lg + gamma) * j0 + (2 / M_PI) * s0;
    *y1 = -2.0 / (M_PI * z) + (2 / M_PI) * lg * j1 - s1 / M_PI;
}

/// A round wire's internal impedance per length (Ω/m): k J0(ka) / (2π a σ
/// J1(ka)), k² = -jωµ0σ.
cd internalImpedance(double a, double sigma, double f)
{
    const double omega = 2 * M_PI * f;
    const cd k = std::sqrt(cd(0, -omega * Mu0 * sigma));
    cd j0, j1;
    bessel01(k * a, &j0, &j1);
    return k * j0 / (2 * M_PI * a * sigma * j1);
}

/// A coax's outer conductor's internal impedance per length (Ω/m), from
/// its inner radius \a b to its outer \a c, the current returning in it (H
/// nought outside): -k N / (2π b σ D), N = J0(kb) - J1(kc) Y0(kb)/Y1(kc), D
/// = J1(kb) - J1(kc) Y1(kb)/Y1(kc).
cd outerImpedance(double b, double c, double sigma, double f)
{
    const double omega = 2 * M_PI * f;
    const cd k = std::sqrt(cd(0, -omega * Mu0 * sigma));
    cd j0b, j1b, j0c, j1c, y0b, y1b, y0c, y1c;
    bessel01(k * b, &j0b, &j1b);
    bessel01(k * c, &j0c, &j1c);
    bessel01y(k * b, &y0b, &y1b);
    bessel01y(k * c, &y0c, &y1c);
    const cd N = j0b - j1c * y0b / y1c, D = j1b - j1c * y1b / y1c;
    return -k * N / (2 * M_PI * b * sigma * D);
}

/// A coax \a len mm long shorted by an end cap 0.3 mm thick - its core 0.5
/// mm, its shell 1.5 to 1.8 mm, copper - fed at z = 0 between its core
/// (terminal 1) and its shell (ground) by a currents physics; a magnetic
/// physics' coil on its metal.
Model shortedCoax(double hmax, int len, QString* ecTag, QString* mfTag)
{
    Model m = model3d(hmax);
    addCylinderAt(m, QStringLiteral("shell"), 0, 0, 1.8, 0, len);
    addCylinderAt(m, QStringLiteral("gap"), 0, 0, 1.5, 0, len);
    addCylinderAt(m, QStringLiteral("core"), 0, 0, 0.5, 0, len);
    addCylinderAt(m, QStringLiteral("cap"), 0, 0, 1.8, len, 0.3);
    for (const char* o : {"shell", "gap", "core"}) zDivisions(m, QString::fromLatin1(o), len);
    zDivisions(m, QStringLiteral("cap"), 1);
    const QJsonObject metal{{QStringLiteral("only"), QJsonArray{QStringLiteral("shell"), QStringLiteral("core"), QStringLiteral("cap")}}};
    const QString et = addCurrents(m, metal, QStringLiteral("5.8e7[S/m]"));
    addFeature(m, et, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("core"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("1")}});
    QJsonObject ground = ofSide(QStringLiteral("shell"), QStringLiteral("bottom"));
    ground.insert(QStringLiteral("except"), ofSide(QStringLiteral("gap"), QStringLiteral("bottom")));
    addFeature(m, et, QStringLiteral("currents/ground"), ground);
    const QString mf = addMagnetic(m);
    addFeature(m, mf, QStringLiteral("magnetic/coil"), metal,
               {{QStringLiteral("name"), QStringLiteral("1")}, {QStringLiteral("ec"), QJsonArray{et}}, {QStringLiteral("terminal"), QStringLiteral("1")}});
    *ecTag = et;
    *mfTag = mf;
    return m;
}

/// Two copper bars 0.4 mm square and 1 mm long along z, 1 mm apart, in an
/// insulated box of air (a perfect conductor: their return), each fed at its
/// floor by a currents physics (terminals a and b) and grounded at its top;
/// coils 1 and 2 of a magnetic physics on them.
Model twoBars(QString* ecTag, QString* mfTag)
{
    Model m = model3d(0.2);
    addBox(m, QStringLiteral("air"), -2, -2, 0, 4, 4, 1);
    addBox(m, QStringLiteral("bar"), -0.2, -0.2, 0, 0.4, 0.4, 1);
    addBox(m, QStringLiteral("bar2"), 0.8, -0.2, 0, 0.4, 0.4, 1);
    for (const char* o : {"air", "bar", "bar2"}) zDivisions(m, QString::fromLatin1(o), 2);
    const QString et = addCurrents(m, QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("bar"), QStringLiteral("bar2")}}}, QStringLiteral("5.8e7[S/m]"), 1);
    addFeature(m, et, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("a")}});
    addFeature(m, et, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("bar2"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
    addFeature(m, et, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar"), QStringLiteral("top")));
    addFeature(m, et, QStringLiteral("currents/ground"), ofSide(QStringLiteral("bar2"), QStringLiteral("top")));
    const QString mf = addMagnetic(m);
    addFeature(m, mf, QStringLiteral("magnetic/coil"), objects(QStringLiteral("bar")),
               {{QStringLiteral("name"), QStringLiteral("1")}, {QStringLiteral("ec"), QJsonArray{et}}, {QStringLiteral("terminal"), QStringLiteral("a")}});
    addFeature(m, mf, QStringLiteral("magnetic/coil"), objects(QStringLiteral("bar2")),
               {{QStringLiteral("name"), QStringLiteral("2")}, {QStringLiteral("ec"), QJsonArray{et}}, {QStringLiteral("terminal"), QStringLiteral("b")}, {QStringLiteral("I0"), QStringLiteral("0[A]")}});
    *ecTag = et;
    *mfTag = mf;
    return m;
}

/// Coil \a name's feature of \a physics in \a m.
Node* coilOf(Model& m, const QString& physics, const QString& name)
{
    for (Node& c : m.find(physics)->children)
        if (c.type == QLatin1String("magnetic/coil") && c.text(QStringLiteral("name")) == name) return &c;
    return nullptr;
}

/// A 2D model's solution (its JSON), solved: its last, or why not.
std::shared_ptr<Solution> solve2d(const QByteArray& json, QString* why)
{
    std::optional<Model> m = Model::fromJson(json, why);
    if (!m) return nullptr;
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(*m);
    const GeometryBuild g = buildGeometry(*m, *ps);
    if (!g.ok()) {
        *why = QStringList(g.errors.values()).join(QLatin1String("; "));
        return nullptr;
    }
    const StudyRun run = runStudy(*m, ps, g.topology, nullptr, QString());
    if (!run.ok()) {
        *why = run.error;
        return nullptr;
    }
    return run.solutions.back();
}

cd complexGlobal(const Solution3& s, const QString& re, const QString& im)
{
    return {global(s, re), s.global(im).value_or(0.0)};
}

/// A wire of radius \a a (mm) along z, \a len mm (\a layers of it), in air
/// to a radius \a R whose wall and ends are insulated (a perfect conductor:
/// its return) - a coax uniform in z. Its currents physics feeds it at its floor (terminal
/// 1), its top its ground; a magnetic physics' coil on it.
Model roundWire(double hmax, double a, double R, double len, QString* ecTag, QString* mfTag, int layers = 1)
{
    Model m = model3d(hmax);
    addCylinderAt(m, QStringLiteral("air"), 0, 0, R, 0, len);
    addCylinderAt(m, QStringLiteral("wire"), 0, 0, a, 0, len);
    zDivisions(m, QStringLiteral("air"), layers);
    zDivisions(m, QStringLiteral("wire"), layers);
    const QString et = addCurrents(m, objects(QStringLiteral("wire")), QStringLiteral("5.8e7[S/m]"));
    addFeature(m, et, QStringLiteral("currents/terminal"), ofSide(QStringLiteral("wire"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("1")}});
    addFeature(m, et, QStringLiteral("currents/ground"), ofSide(QStringLiteral("wire"), QStringLiteral("top")));
    const QString mf = addMagnetic(m);
    addFeature(m, mf, QStringLiteral("magnetic/coil"), objects(QStringLiteral("wire")),
               {{QStringLiteral("name"), QStringLiteral("1")}, {QStringLiteral("ec"), QJsonArray{et}}, {QStringLiteral("terminal"), QStringLiteral("1")}});
    *ecTag = et;
    *mfTag = mf;
    return m;
}

/// The wire of 10 A (0.3 mm and 4 layers sanitized, 0.2 and 6 else) and a
/// heat physics on it of conductivity 0.01 W/(m K) - its ends at 293.15 K,
/// its source the eddy currents' losses mf.Qrh.
Model heatedWire(QString* mf, QString* heat)
{
    // (Sanitized: coarser, fewer layers - its cycle's solves past
    // QtTest's 300 s at 0.2 mm and 6.)
    QString et;
    Model m = roundWire(slowBuild() ? 0.3 : 0.2, 0.5, 1.5, 1, &et, mf, slowBuild() ? 4 : 6);
    coilOf(m, *mf, QStringLiteral("1"))->props.insert(QStringLiteral("I0"), QStringLiteral("10[A]"));
    Node ht = m.make(QStringLiteral("heat"));
    ht.props.insert(QStringLiteral("order"), QStringLiteral("1"));
    ht.props.insert(QStringLiteral("selection"), objects(QStringLiteral("wire")));
    *heat = ht.tag;
    const QString h = *heat;
    m.add(m.component().tag, ht);
    for (Node& c : m.find(h)->children)
        if (c.type == QLatin1String("heat/solid")) {
            c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            c.props.insert(QStringLiteral("k"), QStringLiteral("0.01[W/(m*K)]"));
        }
    addFeature(m, h, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("wire"), QStringLiteral("bottom")), {{QStringLiteral("T0"), QStringLiteral("293.15[K]")}});
    addFeature(m, h, QStringLiteral("heat/temperature"), ofSide(QStringLiteral("wire"), QStringLiteral("top")), {{QStringLiteral("T0"), QStringLiteral("293.15[K]")}});
    addFeature(m, h, QStringLiteral("heat/source"), objects(QStringLiteral("wire")), {{QStringLiteral("Q0"), QStringLiteral("mf.Qrh")}});
    return m;
}

} // namespace

class TestFem3Eddy : public QObject
{
    Q_OBJECT

private slots:
    // Its functions in two ctest entries, each its own time limit:
    // QUCS_TEST_PART 1 those before aDiscUnderARing, 2 it and those after (the
    // disc, the fast sweep's and the heat's) - by their place here, so that
    // one added lands in a part -, unset all. Under ASan beside other builds
    // some 650 and 610 s; split at aFastSweep the first was 854 of its 900.
    void init()
    {
        const QByteArray part = qgetenv("QUCS_TEST_PART");
        if (part.isEmpty()) return;
        const QMetaObject* mo = metaObject();
        int second = -1, here = -1;
        for (int k = mo->methodOffset(); k < mo->methodCount(); ++k) {
            const QByteArray name = mo->method(k).name();
            if (name == "aDiscUnderARing") second = k;
            if (name == QTest::currentTestFunction()) here = k;
        }
        if (second < 0 || here < 0) return;
        if (part != QByteArray::number(here >= second ? 2 : 1)) QSKIP("in the test's other part (QUCS_TEST_PART)");
    }

    void initTestCase()
    {
        // (A sanitized build's LDLTs the libraries' at every size: Eigen's
        // LU compiled into the test at -O0 is some hundreds of times slower.)
        if (slowBuild() && !qEnvironmentVariableIsSet("QUCS_FEM_MUMPS_FROM")) qputenv("QUCS_FEM_MUMPS_FROM", "0");
        if (slowBuild() && !qEnvironmentVariableIsSet("QUCS_FEM_ACCELERATE_FROM")) qputenv("QUCS_FEM_ACCELERATE_FROM", "0");
    }

    // A round copper wire, 0.5 mm in radius, in a perfectly conducting
    // return 1.5 mm out: a coax uniform in z, its exact impedance per length
    // its wire's internal one (Bessel's) and jωµ0 ln(R/a)/2π. Its resistance
    // over its own at 10 Hz (the polygon's: the edge elements' triangles are
    // straight), and its internal inductance's fall from 10 Hz, within 1 %
    // of Bessel's from r/δ 0.02 to 3.4; at 10 Hz its L the exact one's within
    // 0.5 %, its losses ½ R I², the current at its terminal the one held.
    void aRoundWire()
    {
        const double a = 0.5e-3, R = 1.5e-3, len = 1e-3, sigma = 5.8e7;
        const double hmax = coarse() ? 0.2 : 0.1;
        const QList<double> fs{10, 2e3, 5e4, 2e5};
        QString et, mf;
        Model m = roundWire(hmax, 0.5, 1.5, 1, &et, &mf);
        // At rest: its DC inductance.
        Solved dc = solve(m);
        QVERIFY2(dc.error.isEmpty(), qPrintable(why(dc)));
        const double Ldc = global(dc.s(), QStringLiteral("mf.L_1"));
        QStringList texts;
        for (double f : fs) texts << QString::number(f, 'g', 17);
        frequencyStep(m, texts.join(QLatin1String(", ")));
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        QCOMPARE(sv.s().snapshotCount(), int(fs.size()));
        const QString log = sv.s().log.join(QLatin1Char('\n'));
        QVERIFY2(!log.contains(QStringLiteral("is not its")), qPrintable(log));
        const double rdcExact = len / (sigma * M_PI * a * a);
        const double lext = Mu0 / (2 * M_PI) * std::log(R / a) * len;
        double l0 = 0, rdc = 0;
        for (int k = 0; k < int(fs.size()); ++k) {
            sv.s().select(k);
            const double f = fs[k], w = 2 * M_PI * f;
            const double r = global(sv.s(), QStringLiteral("mf.R_1")), l = global(sv.s(), QStringLiteral("mf.L_1"));
            const cd zi = internalImpedance(a, sigma, f) * len;
            const double rWant = zi.real() / rdcExact, lWant = zi.imag() / w - Mu0 / (8 * M_PI) * len;
            if (k == 0) {
                l0 = l;
                rdc = r;
            }
            // (The losses' time average ½ R |I|², I 1 A.)
            const double p = global(sv.s(), QStringLiteral("mf.P"));
            QVERIFY2(std::abs(p - 0.5 * r) < 1e-6 * r, qPrintable(QStringLiteral("f %1: P %2 W, ½ R I² %3 W").arg(f).arg(p).arg(0.5 * r)));
            const double delta = std::sqrt(2 / (w * Mu0 * sigma));
            info(QStringLiteral("f %1 Hz (r/δ %2): R/Rdc %3 (Bessel %4), L %5 nH (L - L(10 Hz) %6, Bessel %7 nH; external %8 nH)")
                     .arg(f)
                     .arg(a / delta, 0, 'g', 3)
                     .arg(r / rdc, 0, 'g', 6)
                     .arg(rWant, 0, 'g', 6)
                     .arg(l * 1e9, 0, 'g', 6)
                     .arg((l - l0) * 1e9, 0, 'g', 4)
                     .arg(lWant * 1e9, 0, 'g', 4)
                     .arg(lext * 1e9, 0, 'g', 6));
            QVERIFY2(std::abs(r / rdc - rWant) < 1e-2 * rWant, qPrintable(QStringLiteral("f %1: R/Rdc %2, Bessel %3").arg(f).arg(r / rdc).arg(rWant)));
            QVERIFY2(std::abs((l - l0) - lWant) < 1e-2 * Mu0 / (8 * M_PI) * len, qPrintable(QStringLiteral("f %1: ΔL %2, Bessel %3").arg(f).arg(l - l0).arg(lWant)));
        }
        const double lExact = lext + Mu0 / (8 * M_PI) * len;
        info(QStringLiteral("L at 10 Hz %1 nH, at rest %2 nH, exact %3 nH").arg(l0 * 1e9).arg(Ldc * 1e9).arg(lExact * 1e9));
        QVERIFY2(std::abs(l0 - lExact) < 5e-3 * lExact, qPrintable(QStringLiteral("L at 10 Hz %1, exact %2").arg(l0).arg(lExact)));
        // (At rest the coil carries its currents physics' J scaled to N I0
        // through this field's own elements - their triangles straight, that
        // physics' curved: scaled to its own current, L was 0.9 % low here and
        // 3 % at 0.2 mm. Now the same as at 10 Hz.)
        QVERIFY2(std::abs(l0 - Ldc) < 1e-3 * Ldc, qPrintable(QStringLiteral("L at 10 Hz %1, at rest %2").arg(l0).arg(Ldc)));
    }

    // The coax of the DC inductance's test (core 0.5 mm, shell 1.5 to 1.8
    // mm, copper), shorted by an end cap, at two lengths: its impedance per
    // length, the difference over the difference of lengths (the cap's and
    // the feed's the same), the exact coax's - its core's and its shell's
    // internal impedances (Bessel's) and jωµ0 ln(b/a)/2π - from 10 Hz, where
    // its L is the DC one, to 100 kHz (its core 2.4 skin depths, its shell
    // 1.4).
    void aCoax()
    {
        const double a = 0.5e-3, b = 1.5e-3, c = 1.8e-3, sigma = 5.8e7;
        // (A sanitized build's 0.25 mm, 1 and 2 mm long, two frequencies: at
        // 0.2 mm, 2 and 4 mm long, three, 210 s under ASan, QtTest's limit
        // 300.)
        const double hmax = slowBuild() ? 0.25 : coarse() ? 0.2 : 0.12;
        const QList<double> fs = slowBuild() ? QList<double>{10, 1e5} : QList<double>{10, 2e4, 1e5};
        QStringList texts;
        for (double f : fs) texts << QString::number(f, 'g', 17);
        // (The shell's formula at DC: its resistance.)
        QVERIFY(std::abs(outerImpedance(b, c, sigma, 1e-3).real() * sigma * M_PI * (c * c - b * b) - 1) < 1e-6);
        QVERIFY(std::abs(internalImpedance(a, sigma, 1e-3).real() * sigma * M_PI * a * a - 1) < 1e-6);
        const int lengths[2] = {slowBuild() ? 1 : 2, slowBuild() ? 2 : 4};
        std::vector<std::vector<cd>> Z(2);
        for (int k = 0; k < 2; ++k) {
            QString et, mf;
            Model m = shortedCoax(hmax, lengths[k], &et, &mf);
            frequencyStep(m, texts.join(QLatin1String(", ")), false);
            QElapsedTimer t;
            t.start();
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            info(QStringLiteral("coax of %1 mm: solved in %2 s").arg(lengths[k]).arg(t.elapsed() / 1000.0));
            // Its estimate a complex factorization's at each frequency: its
            // factors within a factor of two of those Accelerate's analysis
            // says.
            const Estimate3* est = nullptr;
            for (const Estimate3& e : sv.r.estimates)
                if (e.physics == mf) est = &e;
            QVERIFY(est);
            double measured = 0;
            for (const QString& line : sv.s().log) {
                const QRegularExpressionMatch mt = QRegularExpression(QStringLiteral("its factors ([0-9.]+) MB")).match(line);
                if (mt.hasMatch()) measured = std::max(measured, mt.captured(1).toDouble() * 1048576.0);
            }
            if (choleskyBackend() == Backend::Accelerate && measured > 0 && !coarse()) {
                info(QStringLiteral("complex factors: %1 MB estimated, %2 MB by Accelerate's analysis").arg(est->directBytes / 2 / 1048576.0).arg(measured / 1048576.0));
                QVERIFY2(est->directBytes / 2 > 0.5 * measured && est->directBytes / 2 < 2 * measured, qPrintable(QStringLiteral("%1 against %2").arg(est->directBytes / 2).arg(measured)));
            }
            for (int i = 0; i < int(fs.size()); ++i) {
                sv.s().select(i);
                Z[std::size_t(k)].push_back(complexGlobal(sv.s(), QStringLiteral("mf.V_1"), QStringLiteral("mf.V_1_im")));
            }
        }
        const double Lp = Mu0 / (2 * M_PI)
                          * (std::log(b / a) + 0.25 + c * c * c * c * std::log(c / b) / ((c * c - b * b) * (c * c - b * b)) - (3 * c * c - b * b) / (4 * (c * c - b * b)));
        for (int i = 0; i < int(fs.size()); ++i) {
            const double f = fs[i], w = 2 * M_PI * f;
            const cd per = (Z[1][std::size_t(i)] - Z[0][std::size_t(i)]) / ((lengths[1] - lengths[0]) * 1e-3);
            const cd want = internalImpedance(a, sigma, f) + outerImpedance(b, c, sigma, f) + cd(0, w * Mu0 / (2 * M_PI) * std::log(b / a));
            info(QStringLiteral("coax at %1 Hz: dR/dlen %2 Ω/m (exact %3), dL/dlen %4 nH/mm (exact %5; DC %6)")
                     .arg(f)
                     .arg(per.real(), 0, 'g', 6)
                     .arg(want.real(), 0, 'g', 6)
                     .arg(per.imag() / w * 1e6, 0, 'g', 6)
                     .arg(want.imag() / w * 1e6, 0, 'g', 6)
                     .arg(Lp * 1e6, 0, 'g', 6));
            QVERIFY2(std::abs(per.real() - want.real()) < 2e-2 * want.real(), qPrintable(QStringLiteral("f %1: dR/dlen %2, exact %3").arg(f).arg(per.real()).arg(want.real())));
            QVERIFY2(std::abs(per.imag() - want.imag()) < 1e-2 * want.imag(), qPrintable(QStringLiteral("f %1: dX/dlen %2, exact %3").arg(f).arg(per.imag()).arg(want.imag())));
            if (i == 0) QVERIFY2(std::abs(per.imag() / w - Lp) < 1e-2 * Lp, qPrintable(QStringLiteral("dL/dlen at 10 Hz %1, DC %2").arg(per.imag() / w).arg(Lp)));
        }
    }

    // Two bars in an insulated box: their impedance matrix symmetric and
    // passive; the solution of coil 1 fed 1 A, coil 2 none, its first column;
    // coil 1 held at 1 V with coil 2 grounded, or floating, the currents the
    // matrix gives.
    void twoCoilsAndTheirRoles()
    {
        QString et, mf;
        Model m = twoBars(&et, &mf);
        frequencyStep(m, QStringLiteral("100[kHz]"));
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double w = 2 * M_PI * 1e5;
        const auto z = [&](const char* ij) {
            return cd(global(sv.s(), QStringLiteral("mf.R%1").arg(QLatin1String(ij))), w * global(sv.s(), QStringLiteral("mf.L%1").arg(QLatin1String(ij))));
        };
        const cd z11 = z("11"), z12 = z("12"), z21 = z("21"), z22 = z("22");
        const auto text = [](cd v) { return QStringLiteral("(%1, %2)").arg(v.real(), 0, 'g', 6).arg(v.imag(), 0, 'g', 6); };
        info(QStringLiteral("Z: %1 %2; %3 %4 Ω").arg(text(z11), text(z12), text(z21), text(z22)));
        QVERIFY2(std::abs(z12 - z21) < 1e-8 * std::abs(z11), qPrintable(QStringLiteral("Z12 - Z21 %1").arg(std::abs(z12 - z21))));
        QVERIFY(z11.real() > 0 && z22.real() > 0 && z11.real() * z22.real() > z12.real() * z12.real());
        QVERIFY(z11.imag() > 0 && z22.imag() > 0 && z11.imag() * z22.imag() > z12.imag() * z12.imag());
        const cd v1 = complexGlobal(sv.s(), QStringLiteral("mf.V_1"), QStringLiteral("mf.V_1_im")), v2 = complexGlobal(sv.s(), QStringLiteral("mf.V_2"), QStringLiteral("mf.V_2_im"));
        QVERIFY2(std::abs(v1 - z11) < 1e-8 * std::abs(z11) && std::abs(v2 - z21) < 1e-8 * std::abs(z11),
                 qPrintable(QStringLiteral("V %1, %2").arg(std::abs(v1 - z11)).arg(std::abs(v2 - z21))));
        // Coil 1 at 1 V, coil 2 grounded: I = Z⁻¹ (1, 0).
        coilOf(m, mf, QStringLiteral("1"))->props.insert(QStringLiteral("role"), QStringLiteral("voltage"));
        coilOf(m, mf, QStringLiteral("1"))->props.insert(QStringLiteral("V0"), QStringLiteral("1[V]"));
        coilOf(m, mf, QStringLiteral("2"))->props.insert(QStringLiteral("role"), QStringLiteral("grounded"));
        const Solved held = solve(m);
        QVERIFY2(held.error.isEmpty(), qPrintable(why(held)));
        const cd det = z11 * z22 - z12 * z21;
        const cd i1 = complexGlobal(held.s(), QStringLiteral("mf.I_1"), QStringLiteral("mf.I_1_im")), i2 = complexGlobal(held.s(), QStringLiteral("mf.I_2"), QStringLiteral("mf.I_2_im"));
        info(QStringLiteral("held at 1 V, the other grounded: I %1 %2 A, Z⁻¹ %3 %4 A").arg(std::abs(i1)).arg(std::abs(i2)).arg(std::abs(z22 / det)).arg(std::abs(z21 / det)));
        QVERIFY2(std::abs(i1 - z22 / det) < 1e-6 * std::abs(z22 / det) && std::abs(i2 + z21 / det) < 1e-6 * std::abs(z22 / det),
                 qPrintable(QStringLiteral("I %1 %2, Z⁻¹ %3 %4").arg(std::abs(i1)).arg(std::abs(i2)).arg(std::abs(z22 / det)).arg(std::abs(z21 / det))));
        // Coil 2 floating (no current): I1 1 / Z11.
        coilOf(m, mf, QStringLiteral("2"))->props.insert(QStringLiteral("role"), QStringLiteral("floating"));
        const Solved floating = solve(m);
        QVERIFY2(floating.error.isEmpty(), qPrintable(why(floating)));
        const cd f1 = complexGlobal(floating.s(), QStringLiteral("mf.I_1"), QStringLiteral("mf.I_1_im"));
        QVERIFY2(std::abs(f1 - 1.0 / z11) < 1e-6 * std::abs(1.0 / z11), qPrintable(QStringLiteral("I %1, 1/Z11 %2").arg(std::abs(f1)).arg(std::abs(1.0 / z11))));
        QVERIFY2(std::abs(global(floating.s(), QStringLiteral("mf.V_2")) - (z21 / z11).real()) < 1e-6 * std::abs(z21 / z11), "V2 of the floating coil");
    }

    // A core's losses: the wire's air of µr 1 - 0.1j, its external
    // impedance jωµ0 (1 - 0.1j) ln(R/a)/2π - its resistance up by 0.1 ω
    // L_ext, its inductance the same -, those losses mf.Pm; the power in at
    // its terminal, ½ Re(V I*), the losses' sum mf.P + mf.Pm.
    void coreLosses()
    {
        const double a = 0.5e-3, R = 1.5e-3, len = 1e-3, w = 2 * M_PI * 1e5;
        QString et, mf;
        Model m = roundWire(0.2, 0.5, 1.5, 1, &et, &mf);
        frequencyStep(m, QStringLiteral("100[kHz]"));
        const Solved plain = solve(m);
        QVERIFY2(plain.error.isEmpty(), qPrintable(why(plain)));
        QJsonObject air = objects(QStringLiteral("air"));
        air.insert(QStringLiteral("except"), objects(QStringLiteral("wire")));
        addFeature(m, mf, QStringLiteral("magnetic/ampere"), air,
                   {{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("mur"), QStringLiteral("1")}, {QStringLiteral("murIm"), QStringLiteral("0.1")}});
        const Solved lossy = solve(m);
        QVERIFY2(lossy.error.isEmpty(), qPrintable(why(lossy)));
        const double r0 = global(plain.s(), QStringLiteral("mf.R_1")), l0 = global(plain.s(), QStringLiteral("mf.L_1"));
        const double r1 = global(lossy.s(), QStringLiteral("mf.R_1")), l1 = global(lossy.s(), QStringLiteral("mf.L_1"));
        const double p = global(lossy.s(), QStringLiteral("mf.P")), pm = global(lossy.s(), QStringLiteral("mf.Pm"));
        const double want = 0.1 * w * Mu0 / (2 * M_PI) * std::log(R / a) * len;
        info(QStringLiteral("µr'' 0.1: R up %1 Ω (exact %2), L %3 against %4 nH; P %5 W, Pm %6 W, ½ R I² %7 W").arg(r1 - r0).arg(want).arg(l1 * 1e9).arg(l0 * 1e9).arg(p).arg(pm).arg(0.5 * r1));
        QVERIFY2(std::abs((r1 - r0) - want) < 1e-2 * want, qPrintable(QStringLiteral("R up %1, exact %2").arg(r1 - r0).arg(want)));
        QVERIFY2(std::abs(l1 - l0) < 1e-3 * l0, qPrintable(QStringLiteral("L %1 against %2").arg(l1).arg(l0)));
        QVERIFY2(std::abs(pm - 0.5 * (r1 - r0)) < 1e-3 * pm, qPrintable(QStringLiteral("Pm %1, ½ ΔR I² %2").arg(pm).arg(0.5 * (r1 - r0))));
        QVERIFY2(std::abs(p + pm - 0.5 * r1) < 1e-6 * r1, qPrintable(QStringLiteral("P + Pm %1, ½ R I² %2").arg(p + pm).arg(0.5 * r1)));
        QVERIFY(std::abs(global(plain.s(), QStringLiteral("mf.Pm"))) < 1e-9 * global(plain.s(), QStringLiteral("mf.P")));
    }

    // The wire's mesh, 0.2 mm, against its skin depth: at 10 Hz (21 mm)
    // nothing said, at 2 MHz (47 µm) a warning that its eddy currents are not
    // resolved.
    void aMeshCoarserThanItsSkinDepth()
    {
        QString et, mf;
        Model m = roundWire(0.2, 0.5, 1.5, 1, &et, &mf);
        frequencyStep(m, QStringLiteral("10[Hz], 2[MHz]"), false);
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        QStringList said;
        for (const QString& line : sv.s().log)
            if (line.contains(QStringLiteral("skin depth"))) said << line;
        QVERIFY2(said.size() == 1 && said.front().startsWith(QStringLiteral("Warning: ")) && said.front().contains(QStringLiteral("2 MHz")),
                 qPrintable(sv.s().log.join(QLatin1Char('\n'))));
    }

    // The two bars' impedance matrix at two frequencies, a Touchstone file
    // (# Hz Z RI R 1): a two-port's columns, each the globals' R + jωL.
    void aTouchstoneOfTheCoils()
    {
        QString et, mf;
        Model m = twoBars(&et, &mf);
        frequencyStep(m, QStringLiteral("10[kHz], 100[kHz]"));
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        Node node = m.make(QStringLiteral("touchstone"));
        node.props.insert(QStringLiteral("optics"), QJsonArray{mf});
        const Export3 out = export3(Solutions3{sv.r.solution}, node, QStringLiteral("coils"));
        QVERIFY2(out.main.error.isEmpty(), qPrintable(out.main.error));
        QCOMPARE(out.main.suffix, QStringLiteral("s2p"));
        QStringList data;
        bool option = false;
        for (const QString& line : out.main.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            if (line.startsWith(QLatin1Char('#'))) option = line.trimmed() == QLatin1String("# Hz Z RI R 1");
            else if (!line.startsWith(QLatin1Char('!'))) data << line;
        }
        QVERIFY2(option && data.size() == 2, qPrintable(out.main.text));
        const QStringList v = data[1].split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QCOMPARE(v.size(), 9);
        sv.s().select(1);
        const double w = 2 * M_PI * 1e5;
        const char* order[4] = {"11", "21", "12", "22"};
        QVERIFY(std::abs(v[0].toDouble() - 1e5) < 1e-3);
        for (int k = 0; k < 4; ++k) {
            const double r = global(sv.s(), QStringLiteral("mf.R%1").arg(QLatin1String(order[k]))), x = w * global(sv.s(), QStringLiteral("mf.L%1").arg(QLatin1String(order[k])));
            QVERIFY2(std::abs(v[1 + 2 * k].toDouble() - r) < 1e-8 * std::abs(r) + 1e-15 && std::abs(v[2 + 2 * k].toDouble() - x) < 1e-8 * std::abs(x),
                     qPrintable(QStringLiteral("Z%1: %2 %3 against %4 %5").arg(QLatin1String(order[k]), v[1 + 2 * k], v[2 + 2 * k]).arg(r).arg(x)));
        }
        // A Stationary and a Time Dependent step after it: its rows the
        // frequency domain's alone, the same, its ports its last
        // frequency's (before: refused, the coils' terminals the time
        // domain's; or a row a snapshot, of 0 Hz).
        for (Node& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                c.children.push_back(m.make(QStringLiteral("stationary")));
                Node tr = m.make(QStringLiteral("transient"));
                tr.props.insert(QStringLiteral("times"), QStringLiteral("0, 1[us]"));
                c.children.push_back(tr);
            }
        const Solved after = solve(m);
        QVERIFY2(after.error.isEmpty(), qPrintable(why(after)));
        QVERIFY(after.s().snapshotCount() > 2);
        const Export3 again = export3(Solutions3{after.r.solution}, node, QStringLiteral("coils"));
        QVERIFY2(again.main.error.isEmpty(), qPrintable(again.main.error));
        QStringList rows;
        for (const QString& line : again.main.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            if (!line.startsWith(QLatin1Char('#')) && !line.startsWith(QLatin1Char('!'))) rows << line;
        QVERIFY2(rows == data && again.main.suffix == QLatin1String("s2p"), qPrintable(again.main.text));
        QVERIFY2(again.main.summary.contains(QStringLiteral("at 2 frequencies")), qPrintable(again.main.summary));
    }

    // A copper disc 5 mm in radius, 1 mm thick, under a ring of 1e6 A/m²
    // round the axis (r 3 to 4 mm, 1 mm high, 0.5 mm above it) at 1 kHz (δ
    // 2.1 mm), in an insulated cylinder: the disc a conductor fed nothing,
    // its eddy currents' losses and the energy the 2D axisymmetric solver's
    // of its section within 3 %.
    void aDiscUnderARing()
    {
        const double hmax = coarse() ? 1.2 : 0.9;
        Model m = model3d(hmax);
        addCylinderAt(m, QStringLiteral("air"), 0, 0, 8, 0, 5);
        addCylinderAt(m, QStringLiteral("disc"), 0, 0, 5, 1, 1);
        addCylinderAt(m, QStringLiteral("ring"), 0, 0, 4, 2.5, 1);
        addCylinderAt(m, QStringLiteral("hole"), 0, 0, 3, 2.5, 1);
        zDivisions(m, QStringLiteral("disc"), 3);
        zDivisions(m, QStringLiteral("ring"), 2);
        const QString mf = addMagnetic(m);
        addFeature(m, mf, QStringLiteral("magnetic/ampere"), objects(QStringLiteral("disc")),
                   {{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("mur"), QStringLiteral("1")}, {QStringLiteral("sigma"), QStringLiteral("5.8e7[S/m]")}});
        QJsonObject ring = objects(QStringLiteral("ring"));
        ring.insert(QStringLiteral("except"), objects(QStringLiteral("hole")));
        addFeature(m, mf, QStringLiteral("magnetic/current"), ring,
                   {{QStringLiteral("Jex"), QStringLiteral("-1e6[A/m^2]*y/sqrt(x^2 + y^2)")}, {QStringLiteral("Jey"), QStringLiteral("1e6[A/m^2]*x/sqrt(x^2 + y^2)")}});
        frequencyStep(m, QStringLiteral("1[kHz]"));
        QElapsedTimer t;
        t.start();
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double p3 = global(sv.s(), QStringLiteral("mf.P")), w3 = global(sv.s(), QStringLiteral("mf.W"));
        info(QStringLiteral("disc in 3D: %1 s").arg(t.elapsed() / 1000.0));
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm", "space": "axisymmetric",
            "geometry": [{"type": "rectangle", "name": "air", "position": ["0", "0"], "size": ["8", "5"]},
                         {"type": "rectangle", "name": "disc", "position": ["0", "1"], "size": ["5", "1"]},
                         {"type": "rectangle", "name": "ring", "position": ["3", "2.5"], "size": ["1", "1"]}],
            "physics": [{"type": "magnetic", "tag": "mf", "features": [
              {"type": "ampere", "source": "user", "mur": "1", "sigma": "0[S/m]", "selection": {"all": true}},
              {"type": "ampere", "source": "user", "mur": "1", "sigma": "5.8e7[S/m]", "selection": {"only": ["disc"]}},
              {"type": "current", "Je": "1e6[A/m^2]", "selection": {"only": ["ring"]}}]}],
            "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["disc", "ring"]}, "hmax": "0.1"}]}}],
          "studies": [{"type": "study", "steps": [{"type": "frequency", "frequencies": "1[kHz]"}]}]})json";
        QString why2;
        const std::shared_ptr<Solution> s2 = solve2d(json, &why2);
        QVERIFY2(s2, qPrintable(why2));
        const double p2 = s2->global(QStringLiteral("mf.P")).value_or(NAN), w2 = s2->global(QStringLiteral("mf.W")).value_or(NAN);
        info(QStringLiteral("disc under a ring: P %1 W (2D %2), W %3 J (2D %4)").arg(p3).arg(p2).arg(w3).arg(w2));
        QVERIFY2(std::abs(p3 / p2 - 1) < 3e-2, qPrintable(QStringLiteral("P %1, 2D %2").arg(p3).arg(p2)));
        QVERIFY2(std::abs(w3 / w2 - 1) < 3e-2, qPrintable(QStringLiteral("W %1, 2D %2").arg(w3).arg(w2)));
        // (No current through the disc's surface: what flows in it, round.)
        const double jr = valueAt(sv.s(), QStringLiteral("(x*mf.Jx + y*mf.Jy)/sqrt(x^2 + y^2)"), 4.9, 0.3, 1.5);
        const double jphi = valueAt(sv.s(), QStringLiteral("(x*mf.Jy - y*mf.Jx)/sqrt(x^2 + y^2)"), 4.9, 0.3, 1.5);
        info(QStringLiteral("at the disc's rim: Jr %1, Jφ %2 A/m²").arg(jr).arg(jphi));
        QVERIFY2(std::abs(jr) < 0.05 * std::abs(jphi), qPrintable(QStringLiteral("Jr %1, Jφ %2").arg(jr).arg(jphi)));
        // (Fed nothing, its potential's constant held: nought, its field
        // round the axis, at every point of it.)
        const double scale = std::hypot(valueAt(sv.s(), QStringLiteral("mf.Ex"), 4.9, 0.3, 1.5), valueAt(sv.s(), QStringLiteral("mf.Ey"), 4.9, 0.3, 1.5)) * 5e-3;
        for (const auto& [x, y] : {std::pair{0.5, 0.2}, std::pair{-3.0, 2.0}, std::pair{4.0, -1.0}}) {
            const double v = std::hypot(valueAt(sv.s(), QStringLiteral("mf.V"), x, y, 1.5), valueAt(sv.s(), QStringLiteral("mf.V_im"), x, y, 1.5));
            QVERIFY2(v < 2e-2 * scale, qPrintable(QStringLiteral("|V| %1 V at (%2, %3), |E| r %4 V").arg(v).arg(x).arg(y).arg(scale)));
        }
    }

    // The wire's ends on faces where A is not held (a Perfect Magnetic
    // Conductor, not insulated): refused, its terminal named - the voltage
    // between terminal and ground is not one value there.
    void aTerminalWhereAIsFree()
    {
        QString et, mf;
        Model m = roundWire(0.2, 0.5, 1.5, 1, &et, &mf);
        for (const char* side : {"bottom", "top"})
            addFeature(m, mf, QStringLiteral("magnetic/pmc"), ofSide(QStringLiteral("air"), QString::fromLatin1(side)));
        frequencyStep(m, QStringLiteral("10[Hz]"), false);
        const Solved sv = solve(m);
        QVERIFY2(sv.error.contains(QStringLiteral("terminal or ground")) && sv.error.contains(QStringLiteral("Magnetic Insulation")), qPrintable(why(sv)));
    }

    // The fast sweep (a reduced basis, each exact solve's derivatives in it)
    // against each frequency solved: the coax's voltage at 16 frequencies
    // from 10 Hz to 300 kHz, and the two bars' impedance matrix at 13 from 1
    // kHz to 1 MHz, each within 1e-6 of the exact solve's (1.5e-9 and 3e-10
    // measured), from four factorizations at most (three measured).
    void aFastSweep()
    {
        double largest = 0, ends = 0;
        const auto compare = [&largest, &ends](Model m, const QString& frequencies, const QStringList& globals, int* exactSolves, double* worst,
                                        const QString& tolerance = QString()) {
            frequencyStep(m, frequencies, true, QStringLiteral("exact"));
            QElapsedTimer t;
            t.start();
            const Solved exact = solve(m);
            const double exactSeconds = t.elapsed() / 1000.0;
            QVERIFY2(exact.error.isEmpty(), qPrintable(why(exact)));
            frequencyStep(m, frequencies, true, QStringLiteral("fast"));
            if (!tolerance.isEmpty())
                for (Node& c : m.root.children)
                    for (Node& st : c.children)
                        if (st.type == QLatin1String("frequency")) st.props.insert(QStringLiteral("sweeptolerance"), tolerance);
            t.restart();
            const Solved fast = solve(m);
            info(QStringLiteral("fast %1 s, exact %2 s").arg(t.elapsed() / 1000.0).arg(exactSeconds));
            QVERIFY2(fast.error.isEmpty(), qPrintable(why(fast)));
            QCOMPARE(fast.s().snapshotCount(), exact.s().snapshotCount());
            *exactSolves = 0;
            for (const QString& line : fast.s().log)
                if (line.contains(QStringLiteral("factorized in"))) ++*exactSolves;
            *worst = 0;
            ends = 0;
            for (int k = 0; k < exact.s().snapshotCount(); ++k) {
                exact.s().select(k);
                fast.s().select(k);
                QCOMPARE(fast.s().frequency(), exact.s().frequency());
                for (const QString& g : globals) {
                    const double a = global(exact.s(), g), b = global(fast.s(), g);
                    double scale = 0;
                    // (Of the largest of its kind: R, L, V.)
                    for (const QString& h : globals)
                        if (h.left(4) == g.left(4)) scale = std::max(scale, std::abs(global(exact.s(), h)));
                    *worst = std::max(*worst, std::abs(a - b) / scale);
                    // (Its band's ends solved exactly by both: kept as solved,
                    // the same to rounding.)
                    if (k == 0 || k == exact.s().snapshotCount() - 1) ends = std::max(ends, std::abs(a - b) / scale);
                }
            }
            const QString log = fast.s().log.join(QLatin1Char('\n'));
            const QRegularExpressionMatch mt = QRegularExpression(QStringLiteral("its largest residual ([0-9.e+-]+)")).match(log);
            QVERIFY2(mt.hasMatch(), qPrintable(log));
            largest = mt.captured(1).toDouble();
        };
        QString et, mf;
        int solves = 0;
        double worst = 0;
        // Sanitized: the bars swept fast alone, at four frequencies - three
        // factorizations, their Z symmetric at each (the projection keeps
        // it). Each frequency solved exactly too passed QtTest's 300 s under
        // ASan even at four: Eigen's dense algebra is compiled into the test
        // at -O0 there.
        if (slowBuild()) {
            Model m = twoBars(&et, &mf);
            frequencyStep(m, QStringLiteral("logspace(1[kHz], 1[MHz], 4)"), true, QStringLiteral("fast"));
            const Solved fast = solve(m);
            QVERIFY2(fast.error.isEmpty(), qPrintable(why(fast)));
            QCOMPARE(fast.s().snapshotCount(), 4);
            for (const QString& line : fast.s().log)
                if (line.contains(QStringLiteral("factorized in"))) ++solves;
            QVERIFY2(solves == 3, qPrintable(fast.s().log.join(QLatin1Char('\n'))));
            for (int k = 0; k < 4; ++k) {
                fast.s().select(k);
                const double r12 = global(fast.s(), QStringLiteral("mf.R12")), r21 = global(fast.s(), QStringLiteral("mf.R21"));
                const double l12 = global(fast.s(), QStringLiteral("mf.L12")), l21 = global(fast.s(), QStringLiteral("mf.L21"));
                const double l11 = global(fast.s(), QStringLiteral("mf.L11")), r11 = global(fast.s(), QStringLiteral("mf.R11"));
                QVERIFY2(std::abs(r12 - r21) < 1e-8 * r11 && std::abs(l12 - l21) < 1e-8 * l11, qPrintable(QStringLiteral("at %1: R12 %2, R21 %3").arg(k).arg(r12).arg(r21)));
            }
            return;
        }
        const Model coax = shortedCoax(0.15, 2, &et, &mf);
        compare(coax, QStringLiteral("logspace(10[Hz], 300[kHz], 16)"), {QStringLiteral("mf.V_1"), QStringLiteral("mf.V_1_im")}, &solves, &worst);
        info(QStringLiteral("coax: %1 solved exactly; V within %2, at its band's ends %3").arg(solves).arg(worst).arg(ends));
        // (5e-14 measured; rebuilt from the basis 9e-10.)
        QVERIFY2(ends < 1e-12, qPrintable(QStringLiteral("V at its band's ends off by %1").arg(ends)));
        QVERIFY2(worst < 1e-6, qPrintable(QStringLiteral("V off by %1").arg(worst)));
        QVERIFY2(solves <= 4, qPrintable(QStringLiteral("%1 solved exactly").arg(solves)));
        const Model bars = twoBars(&et, &mf);
        const QStringList z{QStringLiteral("mf.R11"), QStringLiteral("mf.R12"), QStringLiteral("mf.R22"), QStringLiteral("mf.L11"), QStringLiteral("mf.L12"), QStringLiteral("mf.L22")};
        const QString band = QStringLiteral("logspace(1[kHz], 1[MHz], 13)");
        compare(bars, band, z, &solves, &worst);
        info(QStringLiteral("bars: %1 solved exactly; Z within %2 (R and L each of their largest)").arg(solves).arg(worst));
        QVERIFY2(worst < 1e-6, qPrintable(QStringLiteral("Z off by %1").arg(worst)));
        QVERIFY2(solves <= 4, qPrintable(QStringLiteral("%1 solved exactly").arg(solves)));
        // A tolerance its band's ends and middle do not meet: more solved
        // where the residual is largest, till every residual is under it.
        compare(bars, band, z, &solves, &worst, QStringLiteral("1e-9"));
        info(QStringLiteral("bars at a tolerance of 1e-9: %1 solved exactly, its largest residual %2; Z within %3").arg(solves).arg(largest).arg(worst));
        QVERIFY2(solves > 3 && largest <= 1e-9, qPrintable(QStringLiteral("%1 solved exactly, its largest residual %2").arg(solves).arg(largest)));
    }

    // A heat physics of the eddy currents' losses (Heat Source mf.Qrh): its
    // heat the losses' time average, mf.P, its temperature solved at each
    // frequency after the field (by the fast sweep).
    void heatOfTheLosses()
    {
        QString mf, h;
        Model m = heatedWire(&mf, &h);
        // (Four frequencies by the fast sweep: the heat, reading the field,
        // solved at each after it is set.)
        frequencyStep(m, QStringLiteral("10[Hz], 1[kHz], 10[kHz], 100[kHz]"), true, QStringLiteral("fast"));
        QElapsedTimer t;
        t.start();
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        info(QStringLiteral("heat: four frequencies in %1 s").arg(t.elapsed() / 1000.0));
        QVERIFY2(sv.s().log.join(QLatin1Char('\n')).contains(QStringLiteral("fast sweep, 4 frequencies")), qPrintable(sv.s().log.join(QLatin1Char('\n'))));
        QCOMPARE(sv.s().snapshotCount(), 4);
        double tmax[4];
        for (int k = 0; k < 4; ++k) {
            sv.s().select(k);
            const double p = global(sv.s(), QStringLiteral("mf.P")), q = global(sv.s(), QStringLiteral("ht.P"));
            info(QStringLiteral("snapshot %1: mf.P %2 W, ht.P %3 W, Tmax %4 K").arg(k).arg(p).arg(q).arg(global(sv.s(), QStringLiteral("ht.Tmax"))));
            QVERIFY2(std::abs(q - p) < 2e-3 * p, qPrintable(QStringLiteral("ht.P %1, mf.P %2").arg(q).arg(p)));
            tmax[k] = global(sv.s(), QStringLiteral("ht.Tmax"));
        }
        // (The heat solved at each: its rise there, more at 100 kHz, whose
        // losses are 44 % more - ht.P alone reads the field's losses, solved
        // or not.)
        QVERIFY2(tmax[0] > 300 && tmax[3] > tmax[0] + 5, qPrintable(QStringLiteral("Tmax %1 K at 10 Hz, %2 K at 100 kHz").arg(tmax[0]).arg(tmax[3])));
    }

    // With σ of T (the wire's Ampère's Law: copper's 0.39 %/K) the field and
    // the heat in turn till they agree, the resistance up by α times the
    // wire's mean rise; a fast sweep asked of it refused (its matrix changes
    // with T), each frequency solved exactly. (Apart from heatOfTheLosses:
    // together past 250 s of QtTest's 300 under ASan.)
    void conductivityOfTemperature()
    {
        QString mf, h;
        Model m = heatedWire(&mf, &h);
        frequencyStep(m, QStringLiteral("10[Hz]"));
        const Solved cold = solve(m);
        QVERIFY2(cold.error.isEmpty(), qPrintable(why(cold)));
        const double rCold = global(cold.s(), QStringLiteral("mf.R_1"));
        QElapsedTimer t;
        // σ of T: the magnetic field reads the heat, which reads it.
        addFeature(m, mf, QStringLiteral("magnetic/ampere"), objects(QStringLiteral("wire")),
                   {{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("mur"), QStringLiteral("1")},
                    {QStringLiteral("sigma"), QStringLiteral("5.8e7[S/m]/(1 + 0.0039[1/K]*(%1.T - 293.15[K]))").arg(h)}});
        frequencyStep(m, QStringLiteral("10[Hz]"));
        // (Sanitized: its passes to 1e-4, not 1e-6 - 239 s of QtTest's 300
        // beside other builds.)
        if (slowBuild())
            for (Node& c : m.root.children)
                for (Node& st : c.children)
                    if (st.type == QLatin1String("frequency")) st.props.insert(QStringLiteral("tolerance"), QStringLiteral("1e-4"));
        t.start();
        const Solved hot = solve(m);
        info(QStringLiteral("heat: σ of T in %1 s").arg(t.elapsed() / 1000.0));
        QVERIFY2(hot.error.isEmpty(), qPrintable(why(hot)));
        const double r = global(hot.s(), QStringLiteral("mf.R_1"));
        const double volume = integral(hot.s(), QStringLiteral("1"), QStringLiteral("mf.sigma > 0"));
        const double rise = integral(hot.s(), QStringLiteral("%1.T - 293.15[K]").arg(h), QStringLiteral("mf.sigma > 0")) / volume;
        info(QStringLiteral("σ of T: R %1 Ω, cold %2 Ω (up %3, α ΔT %4)").arg(r).arg(rCold).arg(r / rCold - 1).arg(0.0039 * rise));
        QVERIFY2(hot.s().log.join(QLatin1Char('\n')).contains(QStringLiteral("iteration 2")), qPrintable(hot.s().log.join(QLatin1Char('\n'))));
        QVERIFY2(std::abs((r / rCold - 1) - 0.0039 * rise) < 2e-2 * 0.0039 * rise, qPrintable(QStringLiteral("R up %1, α ΔT %2").arg(r / rCold - 1).arg(0.0039 * rise)));
        // Its matrix changing with T, not with ω alone: four frequencies
        // asked fast each solved exactly, and said. (Not where sanitized:
        // four cycles' solves past QtTest's 300 s.)
        if (slowBuild()) return;
        frequencyStep(m, QStringLiteral("10[Hz], 20[Hz], 50[Hz], 100[Hz]"), true, QStringLiteral("fast"));
        const Solved cycle = solve(m);
        QVERIFY2(cycle.error.isEmpty(), qPrintable(why(cycle)));
        const QString log = cycle.s().log.join(QLatin1Char('\n'));
        QVERIFY2(log.contains(QStringLiteral("not by the fast sweep")) && !log.contains(QStringLiteral("fast sweep, ")), qPrintable(log));
        QCOMPARE(cycle.s().snapshotCount(), 4);
    }

    // The iterative solve (the step's Solver iterative): GMRES with the
    // auxiliary space preconditioner, against the direct solve - the shorted
    // coax's impedance from 10 Hz to 1 MHz (its core 6.6 skin depths at 1
    // MHz; at 1 MHz the preconditioner's blocks apart did not converge), the
    // two bars' impedance matrix (a coil held at a voltage too, a column of
    // its own), each to 1e-6 of its largest entry. (A sanitized build: the
    // coax at 0.4 mm and 10 kHz alone.)
    void anIterativeSolve()
    {
        const auto setSolver = [](Model& m, const QString& solver) {
            for (Node& c : m.root.children)
                for (Node& st : c.children)
                    if (st.type == QLatin1String("frequency")) st.props.insert(QStringLiteral("solver"), solver);
        };
        // Each solver's globals at each frequency: the direct's and the
        // iterative's.
        const auto both = [&](Model& m, const QStringList& globals, QList<double>* direct, QList<double>* iterative, QString* log) {
            for (const QString& solver : {QStringLiteral("direct"), QStringLiteral("iterative")}) {
                setSolver(m, solver);
                QElapsedTimer t;
                t.start();
                const Solved sv = solve(m);
                QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
                info(QStringLiteral("%1: %2 s").arg(solver).arg(t.elapsed() / 1000.0));
                if (solver == QLatin1String("iterative")) {
                    *log = sv.s().log.join(QLatin1Char('\n'));
                    // (Its estimate the iterative solve's.)
                    bool estimated = false;
                    for (const Estimate3& e : sv.r.estimates)
                        if (e.physics == QLatin1String("mf")) estimated = e.method == QLatin1String("iterative") && e.iterativeBytes > 0;
                    QVERIFY2(estimated, "the magnetic field's estimate the iterative solve's");
                }
                for (int k = 0; k < sv.s().snapshotCount(); ++k) {
                    sv.s().select(k);
                    for (const QString& g : globals) (solver == QLatin1String("direct") ? direct : iterative)->append(global(sv.s(), g));
                }
            }
        };
        const auto agree = [](const QString& what, const QList<double>& d, const QList<double>& it, int perFrequency) {
            QVERIFY(d.size() == it.size() && !d.isEmpty());
            for (int k0 = 0; k0 < d.size(); k0 += perFrequency) {
                double largest = 0;
                for (int k = k0; k < k0 + perFrequency; ++k) largest = std::max(largest, std::abs(d[k]));
                for (int k = k0; k < k0 + perFrequency; ++k)
                    QVERIFY2(std::abs(it[k] - d[k]) < 1e-6 * largest,
                             qPrintable(QStringLiteral("%1, value %2: %3 iterative, %4 direct").arg(what).arg(k).arg(it[k], 0, 'g', 12).arg(d[k], 0, 'g', 12)));
            }
        };
        {
            QString et, mf;
            Model m = shortedCoax(slowBuild() ? 0.4 : 0.25, slowBuild() ? 1 : 2, &et, &mf);
            frequencyStep(m, slowBuild() ? QStringLiteral("1e4") : QStringLiteral("10, 1e4, 1e6"), false);
            QList<double> d, it;
            QString log;
            both(m, {QStringLiteral("mf.V_1"), QStringLiteral("mf.V_1_im")}, &d, &it, &log);
            if (QTest::currentTestFailed()) return;
            for (const QString& line : log.split(QLatin1Char('\n')))
                if (line.contains(QStringLiteral("GMRES"))) info(line);
            QVERIFY2(log.contains(QStringLiteral("GMRES with the auxiliary space preconditioner")), qPrintable(log));
            agree(QStringLiteral("coax"), d, it, 2);
        }
        if (slowBuild()) return;
        {
            QString et, mf;
            Model m = twoBars(&et, &mf);
            coilOf(m, mf, QStringLiteral("2"))->props.insert(QStringLiteral("role"), QStringLiteral("voltage"));
            coilOf(m, mf, QStringLiteral("2"))->props.insert(QStringLiteral("V0"), QStringLiteral("1[mV]"));
            // (Its solves, the estimate's: the field's and coil 1's - the one
            // fed a current - at each frequency; a fast sweep's three
            // frequencies solved exactly, three times each.)
            const auto solvesOf = [&](const QString& frequencies, const QString& fast) {
                frequencyStep(m, frequencies, true, fast);
                for (const Node& c : m.root.children)
                    if (c.type == QLatin1String("study")) return magneticSolves(c, m.find(mf), evaluateParameters(m)->scope);
                return -1;
            };
            QCOMPARE(solvesOf(QStringLiteral("1[kHz], 10[kHz]"), QStringLiteral("exact")), 4);
            QCOMPARE(solvesOf(QStringLiteral("1[kHz], 2[kHz], 3[kHz], 4[kHz], 5[kHz], 6[kHz]"), QStringLiteral("fast")), 18);
            frequencyStep(m, QStringLiteral("100[kHz]"));
            // (A tolerance double precision does not reach, 1e-30: taken as
            // 1e-12 - before, 2000 iterations and an error.)
            for (Node& c : m.root.children)
                for (Node& st : c.children)
                    if (st.type == QLatin1String("frequency")) st.props.insert(QStringLiteral("tolerance"), QStringLiteral("1e-30"));
            QList<double> d, it;
            QString log;
            both(m, {QStringLiteral("mf.R11"), QStringLiteral("mf.L11"), QStringLiteral("mf.I_2"), QStringLiteral("mf.I_2_im")}, &d, &it, &log);
            if (QTest::currentTestFailed()) return;
            for (const QString& line : log.split(QLatin1Char('\n')))
                if (line.contains(QStringLiteral("GMRES"))) info(line);
            agree(QStringLiteral("bars"), {d[0], d[1]}, {it[0], it[1]}, 1);
            agree(QStringLiteral("bars"), {d[2], d[3]}, {it[2], it[3]}, 2);
        }
        // The air about the bars of µr 1000 (γ each element's own ν's:
        // before, of ν0, GMRES stopped at a residual of 0.84); and the bars in
        // a held field of 0.5 T along x (its part solved apart: before, the
        // tolerance spent on it, the coil's V 1.7e-5 off).
        for (const bool core : {true, false}) {
            QString et, mf;
            Model m = twoBars(&et, &mf);
            if (core)
                addFeature(m, mf, QStringLiteral("magnetic/ampere"),
                           QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("air")}},
                                       {QStringLiteral("except"), QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("bar"), QStringLiteral("bar2")}}}}},
                           {{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("mur"), QStringLiteral("1000")}});
            else
                addFeature(m, mf, QStringLiteral("magnetic/potential"), rule(R"({"exterior": true})"), {{QStringLiteral("A0z"), QStringLiteral("0.5[T]*y")}});
            frequencyStep(m, QStringLiteral("100[kHz]"), false);
            QList<double> d, it;
            QString log;
            both(m, {QStringLiteral("mf.V_1"), QStringLiteral("mf.V_1_im")}, &d, &it, &log);
            if (QTest::currentTestFailed()) return;
            for (const QString& line : log.split(QLatin1Char('\n')))
                if (line.contains(QStringLiteral("GMRES"))) info((core ? QStringLiteral("µr 1000: ") : QStringLiteral("held field: ")) + line);
            agree(core ? QStringLiteral("bars in µr 1000") : QStringLiteral("bars in a held field"), d, it, 2);
        }
    }
};

QTEST_GUILESS_MAIN(TestFem3Eddy)

#include "test_fem3_eddy.moc"
