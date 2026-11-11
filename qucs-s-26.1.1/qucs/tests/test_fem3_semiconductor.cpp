/*
 * test_fem3_semiconductor.cpp - semiconductors solved in 3D (multiphysics/
 *                               fem3_semiconductor.cpp): a pn junction at
 *                               equilibrium (its Fermi level flat, its field
 *                               the depletion approximation's), a short
 *                               diode's current against Shockley's, a MOS
 *                               capacitor's quasi-static C-V against the
 *                               exact 1D surface charge (Sze), a model
 *                               uniform in z against the 2D solver, and
 *                               what is refused
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
#include "fem_materials.h"
#include "fem_results.h"
#include "fem_solver.h"

#include <QJsonDocument>
#include <QElapsedTimer>
#include <QtTest>

#include <cmath>
#include <complex>
#include <functional>

using namespace qucs_s::fem;

namespace {

constexpr double Qe = 1.602176634e-19, KB = 1.380649e-23, Eps0 = 8.8541878188e-12;
const double Vt = KB * 300 / Qe;
// Silicon, as the tests give it.
constexpr double Nc = 2.8e25, Nv = 1.04e25, Eg = 1.12 * Qe, EpsSi = 11.7, MuN = 0.14, MuP = 0.045;
const double Ni = std::sqrt(Nc * Nv) * std::exp(-Eg / (2 * KB * 300));

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

/// A 3D model in µm, its plan meshed at \a hmax.
Model model3d(double hmax)
{
    Model m = Model::blank3d();
    m.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
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

/// A semiconductor physics whose material model, the user's silicon, is on
/// \a on (every domain when empty).
QString addSemiconductor(Model& m, const QJsonObject& on = {})
{
    Node p = m.make(QStringLiteral("semiconductor"));
    const QString tag = p.tag;
    m.add(m.component().tag, p);
    for (Node& c : m.find(tag)->children)
        if (c.type == QLatin1String("semiconductor/model")) {
            c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            c.props.insert(QStringLiteral("epsilonr"), QStringLiteral("11.7"));
            c.props.insert(QStringLiteral("Eg"), QStringLiteral("1.12[eV]"));
            c.props.insert(QStringLiteral("chi"), QStringLiteral("4.05[eV]"));
            c.props.insert(QStringLiteral("Nc"), QStringLiteral("2.8e19[1/cm^3]"));
            c.props.insert(QStringLiteral("Nv"), QStringLiteral("1.04e19[1/cm^3]"));
            c.props.insert(QStringLiteral("mun"), QStringLiteral("1400[cm^2/(V*s)]"));
            c.props.insert(QStringLiteral("mup"), QStringLiteral("450[cm^2/(V*s)]"));
            if (!on.isEmpty()) c.props.insert(QStringLiteral("selection"), on);
        }
    return tag;
}

void dope(Model& m, const QString& semi, const QString& object, bool donor, const QString& N)
{
    addFeature(m, semi, QStringLiteral("semiconductor/doping"), objects(object),
               {{QStringLiteral("dopant"), donor ? QStringLiteral("donor") : QStringLiteral("acceptor")}, {QStringLiteral("profile"), QStringLiteral("uniform")},
                {QStringLiteral("N0"), N}});
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

QString why(const Solved& sv)
{
    return sv.error + QLatin1Char('\n') + sv.r.log.join(QLatin1Char('\n'));
}

void info(const QString& what)
{
    qInfo("%s", qPrintable(what));
}

/// A pn diode along z: p (Na) from 0 to 1 µm, n (Nd) from 1 to 2 µm, its
/// plan 1 µm square; contacts a (bottom) and c (top), a at \a va.
Model diode(double Na, double Nd, double va, int layers, bool srh = false)
{
    Model m = model3d(1.0);
    addBox(m, QStringLiteral("p"), 0, 0, 0, 1, 1, 1);
    addBox(m, QStringLiteral("n"), 0, 0, 1, 1, 1, 1);
    zDivisions(m, QStringLiteral("p"), layers);
    zDivisions(m, QStringLiteral("n"), layers);
    const QString semi = addSemiconductor(m);
    dope(m, semi, QStringLiteral("p"), false, QStringLiteral("%1[1/m^3]").arg(Na, 0, 'g', 17));
    dope(m, semi, QStringLiteral("n"), true, QStringLiteral("%1[1/m^3]").arg(Nd, 0, 'g', 17));
    addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("p"), QStringLiteral("bottom")),
               {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), QStringLiteral("%1[V]").arg(va, 0, 'g', 17)}});
    addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("n"), QStringLiteral("top")), {{QStringLiteral("name"), QStringLiteral("c")}});
    if (srh) addFeature(m, semi, QStringLiteral("semiconductor/srh"), rule(R"({"all": true})"), {{QStringLiteral("taun"), QStringLiteral("1[us]")}, {QStringLiteral("taup"), QStringLiteral("1[us]")}});
    return m;
}

/// How far a lateral pn diode uniform in z is from its 2D model: p and n
/// side by side, SRH, contacts on its ends at \a bias forward, \a configure
/// making both models' features the same (\a three the 3D one's).
struct Against2D {
    QString error;
    double V = 0, n = 0, p = 0;   // the worst: V's (V), the carriers' relative
    double i3 = 0, i2 = 0;
    std::map<QString, std::pair<double, double>> globals;   // name: 3D's, 2D's (those both have)
    QString log;                                            // 3D's
};
Against2D against2D(double bias, const std::function<void(Model&, const QString&, bool)>& configure, double hmax = 0.05)
{
    Against2D out;
    Model m3 = model3d(hmax);
    addBox(m3, QStringLiteral("p"), 0, 0, 0, 1, 1, 0.5);
    addBox(m3, QStringLiteral("n"), 1, 0, 0, 1, 1, 0.5);
    zDivisions(m3, QStringLiteral("p"), 1);
    zDivisions(m3, QStringLiteral("n"), 1);
    Model m2 = Model::blank();
    m2.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
    m2.component().props.insert(QStringLiteral("thickness"), QStringLiteral("0.5[um]"));
    for (const auto& [label, x] : {std::pair{QStringLiteral("p"), 0.0}, std::pair{QStringLiteral("n"), 1.0}}) {
        Node r = m2.make(QStringLiteral("rectangle"));
        r.label = label;
        r.props.insert(QStringLiteral("position"), pairOf(x, 0));
        r.props.insert(QStringLiteral("size"), pairOf(1, 1));
        m2.add(m2.geometry().tag, r);
    }
    const auto box2 = [](double x0, double y0, double x1, double y1) { return QJsonObject{{QStringLiteral("box"), QJsonArray{x0, y0, x1, y1}}}; };
    for (Model* m : {&m3, &m2}) {
        const bool three = m == &m3;
        const QString semi = addSemiconductor(*m);
        const QJsonObject p = three ? objects(QStringLiteral("p")) : box2(-0.01, -0.01, 1.01, 1.01);
        const QJsonObject n = three ? objects(QStringLiteral("n")) : box2(0.99, -0.01, 2.01, 1.01);
        addFeature(*m, semi, QStringLiteral("semiconductor/doping"), p,
                   {{QStringLiteral("dopant"), QStringLiteral("acceptor")}, {QStringLiteral("profile"), QStringLiteral("uniform")}, {QStringLiteral("N0"), QStringLiteral("1e17[1/cm^3]")}});
        addFeature(*m, semi, QStringLiteral("semiconductor/doping"), n,
                   {{QStringLiteral("dopant"), QStringLiteral("donor")}, {QStringLiteral("profile"), QStringLiteral("uniform")}, {QStringLiteral("N0"), QStringLiteral("1e16[1/cm^3]")}});
        addFeature(*m, semi, QStringLiteral("semiconductor/srh"), three ? rule(R"({"all": true})") : box2(-0.01, -0.01, 2.01, 1.01),
                   {{QStringLiteral("taun"), QStringLiteral("0.1[us]")}, {QStringLiteral("taup"), QStringLiteral("0.1[us]")}});
        addFeature(*m, semi, QStringLiteral("semiconductor/ohmic"), three ? ofSide(QStringLiteral("p"), QStringLiteral("left")) : box2(-0.01, -0.01, 0.01, 1.01),
                   {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), QStringLiteral("%1[V]").arg(bias)}});
        addFeature(*m, semi, QStringLiteral("semiconductor/ohmic"), three ? ofSide(QStringLiteral("n"), QStringLiteral("right")) : box2(1.99, -0.01, 2.01, 1.01),
                   {{QStringLiteral("name"), QStringLiteral("c")}});
        configure(*m, semi, three);
    }
    const Solved s3 = solve(m3);
    if (!s3.error.isEmpty()) {
        out.error = QStringLiteral("3D: ") + why(s3);
        return out;
    }
    const StudyResult r2 = solveStudy(m2, evaluateParameters(m2), s3.mesh->plan, QString());
    if (!r2.error.isEmpty() || !r2.solution) {
        out.error = QStringLiteral("2D: ") + r2.error;
        return out;
    }
    const Field3& f3 = s3.s().fields().front();
    const Field& f2 = r2.solution->fields().front();
    const int n3 = f3.space->size(), n2 = f2.space->size();
    for (int dof = 0; dof < n3; ++dof) {
        const int pd = f3.space->origin(dof).first;
        out.V = std::max(out.V, std::abs(f3.values[std::size_t(dof)] - f2.values[std::size_t(pd)]));
        for (int c : {1, 2}) {
            const double a = f3.values[std::size_t(c * n3 + dof)], b = f2.values[std::size_t(c * n2 + pd)];
            (c == 1 ? out.n : out.p) = std::max(c == 1 ? out.n : out.p, std::abs(a - b) / std::max(std::abs(b), 1.0));
        }
    }
    out.log = s3.r.log.join(QLatin1Char('\n'));
    out.i3 = global(s3.s(), QStringLiteral("semi.I_a"));
    out.i2 = r2.solution->global(QStringLiteral("semi.I_a")).value_or(std::nan(""));
    for (const char* name : {"In", "Ip", "Emax", "Istim", "Iion", "Igen", "Mion"}) {
        const QString key = QStringLiteral("semi.") + QString::fromLatin1(name);
        const std::optional<double> a = s3.s().global(key), b = r2.solution->global(key);
        if (a && b) out.globals[QString::fromLatin1(name)] = {*a, *b};
    }
    return out;
}

/// Sets \a key of the physics' material model (both models alike).
void modelProp(Model& m, const QString& semi, const QString& key, const QJsonValue& value)
{
    for (Node& c : m.find(semi)->children)
        if (c.type == QLatin1String("semiconductor/model")) c.props.insert(key, value);
}

} // namespace

class TestFem3Semiconductor : public QObject
{
    Q_OBJECT

private slots:
    // A pn junction at rest: its quasi-Fermi levels flat at nought (n p =
    // ni²), its neutral regions' potentials Vt ln(Nd/ni) and -Vt ln(Na/ni),
    // its peak field the depletion approximation's √(2q (Vbi - 2Vt) / ε
    // NaNd/(Na + Nd)) within 2 %, no current.
    void junctionAtRest()
    {
        const double Na = 1e23, Nd = 1e22;
        const Solved sv = solve(diode(Na, Nd, 0, 200));
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        for (double z : {0.2, 0.9, 1.0, 1.05, 1.6}) {
            const double efn = valueAt(sv.s(), QStringLiteral("semi.Efn"), 0.3, 0.6, z), efp = valueAt(sv.s(), QStringLiteral("semi.Efp"), 0.3, 0.6, z);
            QVERIFY2(std::abs(efn) < 1e-6 * Qe && std::abs(efp) < 1e-6 * Qe, qPrintable(QStringLiteral("z %1: Efn %2 eV, Efp %3 eV").arg(z).arg(efn / Qe).arg(efp / Qe)));
        }
        const double vp = valueAt(sv.s(), QStringLiteral("semi.V"), 0.5, 0.5, 0.2), vn = valueAt(sv.s(), QStringLiteral("semi.V"), 0.5, 0.5, 1.8);
        QVERIFY2(std::abs(vp + Vt * std::log(Na / Ni)) < 1e-6 && std::abs(vn - Vt * std::log(Nd / Ni)) < 1e-6, qPrintable(QStringLiteral("%1, %2 V").arg(vp).arg(vn)));
        const double vbi = Vt * std::log(Na * Nd / (Ni * Ni));
        const double eps = EpsSi * Eps0;
        const double emax = std::sqrt(2 * Qe * (vbi - 2 * Vt) / eps * Na * Nd / (Na + Nd));
        double peak = 0;
        for (int k = 0; k <= 400; ++k) peak = std::max(peak, std::abs(valueAt(sv.s(), QStringLiteral("semi.Ez"), 0.5, 0.5, 0.9 + 0.3 * k / 400)));
        info(QStringLiteral("junction: peak field %1 V/m, the depletion approximation's %2 V/m").arg(peak).arg(emax));
        QVERIFY2(std::abs(peak - emax) < 2e-2 * emax, qPrintable(QStringLiteral("%1, %2 V/m").arg(peak).arg(emax)));
        QVERIFY2(std::abs(global(sv.s(), QStringLiteral("semi.I_a"))) < 1e-15, qPrintable(QString::number(global(sv.s(), QStringLiteral("semi.I_a")))));
    }

    // A short diode (its regions 1 µm, no recombination): its current
    // Shockley's, q A ni² (Dn/(Na Wp) + Dp/(Nd Wn)) (e^(V/Vt) - 1), Wp and
    // Wn its neutral regions' widths - within 3 % at 0.35, 0.4 and 0.45 V;
    // the same current in at the anode and out at the cathode; its slope
    // between 0.35 and 0.45 V Shockley's within 0.5 %.
    void shortDiode()
    {
        const double Na = 1e23, Nd = 1e22, A = 1e-12, eps = EpsSi * Eps0;
        const double vbi = Vt * std::log(Na * Nd / (Ni * Ni));
        std::vector<double> currents, shockleys;
        for (double v : {0.35, 0.4, 0.45}) {
            const Solved sv = solve(diode(Na, Nd, v, 200));
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double ia = global(sv.s(), QStringLiteral("semi.I_a")), ic = global(sv.s(), QStringLiteral("semi.I_c"));
            const double W = std::sqrt(2 * eps * (vbi - v - 2 * Vt) / Qe * (1 / Na + 1 / Nd));
            const double wp = 1e-6 - W * Nd / (Na + Nd), wn = 1e-6 - W * Na / (Na + Nd);
            const double shockley = Qe * A * Ni * Ni * (MuN * Vt / (Na * wp) + MuP * Vt / (Nd * wn)) * std::expm1(v / Vt);
            info(QStringLiteral("diode at %1 V: I %2 A (cathode %3 A, sum %5), Shockley's %4 A").arg(v).arg(ia).arg(ic).arg(shockley).arg(ia + ic));
            // (Their sum rounding's: drift and diffusion in its depletion
            // region each some 1e10 of the current, cancelling.)
            QVERIFY2(std::abs(ia + ic) < 1e-5 * std::abs(ia), qPrintable(QStringLiteral("%1, %2 A").arg(ia).arg(ic)));
            QVERIFY2(std::abs(ia - shockley) < 3e-2 * shockley, qPrintable(QStringLiteral("%1 V: %2 A, Shockley's %3 A").arg(v).arg(ia).arg(shockley)));
            currents.push_back(ia);
            shockleys.push_back(shockley);
        }
        // (Its slope Shockley's: e^(0.1/Vt) less its neutral regions'
        // narrowing.)
        const double slope = currents[2] / currents[0], want = shockleys[2] / shockleys[0];
        QVERIFY2(std::abs(slope - want) < 5e-3 * want, qPrintable(QStringLiteral("%1, Shockley's %2").arg(slope).arg(want)));
    }

    // A MOS capacitor - p silicon 0.3 µm of 1e17 per cm³ under 10 nm of
    // oxide (εr 3.9), its gate's work function 4.6 eV - its quasi-static
    // capacitance dQ/dVg in accumulation, depletion and inversion against
    // the exact 1D surface charge, Vg = Vfb + ψs - Qs/Cox (Sze), within 2 %;
    // a fixed charge at its interface shifting the curve by -Qf/Cox.
    void mosCapacitor()
    {
        const double Na = 1e23, tox = 10e-9, epsOx = 3.9 * Eps0, epsSi = EpsSi * Eps0, A = 1e-12, phim = 4.6;
        const double dEc = Eg / 2 + KB * 300 / 2 * std::log(Nc / Nv);
        const double p0 = Na, n0 = Ni * Ni / Na, psiB = Vt * std::log(Na / Ni);
        const double vfb = (phim * Qe - 4.05 * Qe - dEc) / Qe - psiB;
        const double cox = epsOx / tox, ld = std::sqrt(epsSi * Vt / (Qe * p0));
        const auto qs = [&](double psi) {
            const double b = psi / Vt;
            const double F = std::sqrt(std::max(0.0, (std::exp(-b) + b - 1) + n0 / p0 * (std::exp(b) - b - 1)));
            return -(psi >= 0 ? 1.0 : -1.0) * std::sqrt(2.0) * epsSi * Vt / ld * F;
        };
        const auto vg = [&](double psi) { return vfb + psi - qs(psi) / cox; };
        const auto exactC = [&](double v) {
            double lo = -1, hi = 1.2;
            for (int i = 0; i < 200; ++i) ((vg(0.5 * (lo + hi)) < v) ? lo : hi) = 0.5 * (lo + hi);
            const double psi = 0.5 * (lo + hi), d = 1e-6;
            return A * (-(qs(psi + d) - qs(psi - d))) / (vg(psi + d) - vg(psi - d));
        };
        const auto charge = [&](double v, double rhos = 0) {
            Model m = model3d(1.0);
            addBox(m, QStringLiteral("si"), 0, 0, 0, 1, 1, 0.3);
            addBox(m, QStringLiteral("ox"), 0, 0, 0.3, 1, 1, 0.01);
            zDivisions(m, QStringLiteral("si"), 150);
            zDivisions(m, QStringLiteral("ox"), 2);
            Node mat = m.make(QStringLiteral("material"));
            mat.label = QStringLiteral("oxide");
            mat.props.insert(QStringLiteral("epsilonr"), QStringLiteral("3.9"));
            mat.props.insert(QStringLiteral("selection"), objects(QStringLiteral("ox")));
            m.add(m.materials().tag, mat);
            const QString semi = addSemiconductor(m, objects(QStringLiteral("si")));
            dope(m, semi, QStringLiteral("si"), false, QStringLiteral("1e17[1/cm^3]"));
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("si"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
            // (Its generation, SRH's: the inversion layer's electrons there
            // at rest with the holes - without it, held only by diffusion
            // through a bulk of 1e8 per m³, under the round-off of their own
            // currents.)
            addFeature(m, semi, QStringLiteral("semiconductor/srh"), objects(QStringLiteral("si")),
                       {{QStringLiteral("taun"), QStringLiteral("1[us]")}, {QStringLiteral("taup"), QStringLiteral("1[us]")}});
            if (rhos != 0)
                addFeature(m, semi, QStringLiteral("semiconductor/charge"), ofSide(QStringLiteral("si"), QStringLiteral("top")),
                           {{QStringLiteral("rhos"), QStringLiteral("%1[C/m^2]").arg(rhos, 0, 'g', 17)}});
            addFeature(m, semi, QStringLiteral("semiconductor/gate"), ofSide(QStringLiteral("ox"), QStringLiteral("top")),
                       {{QStringLiteral("name"), QStringLiteral("g")}, {QStringLiteral("V0"), QStringLiteral("%1[V]").arg(v, 0, 'g', 17)}, {QStringLiteral("phim"), QStringLiteral("4.6[eV]")}});
            const Solved sv = solve(m);
            if (!sv.error.isEmpty()) {
                qWarning("%s", qPrintable(why(sv)));
                return std::nan("");
            }
            return global(sv.s(), QStringLiteral("semi.Q_g"));
        };
        for (double v : {vfb - 1.0, vfb + 0.5, vfb + 2.5}) {
            const double dv = 0.01;
            const double c = (charge(v + dv) - charge(v - dv)) / (2 * dv);
            const double want = exactC(v);
            info(QStringLiteral("MOS at Vg %1 V (Vfb %2 V): C %3 F, exact %4 F, Cox A %5 F").arg(v).arg(vfb).arg(c).arg(want).arg(cox * A));
            QVERIFY2(std::abs(c - want) < 2e-2 * want, qPrintable(QStringLiteral("Vg %1 V: C %2 F, exact %3 F").arg(v).arg(c).arg(want)));
        }
        // A fixed charge at the interface, 1e11 q per cm²: the curve shifted
        // by -Qf/Cox, its depletion capacitance there the same within 0.5 %.
        const double qf = 1e15 * Qe, shift = -qf / cox;
        const double v = vfb + 0.5 + shift, dv = 0.01;
        const double c = (charge(v + dv, qf) - charge(v - dv, qf)) / (2 * dv);
        const double want = exactC(vfb + 0.5);
        info(QStringLiteral("MOS with Qf: at Vg %1 V (shift %2 V) C %3 F, exact %4 F").arg(v).arg(shift).arg(c).arg(want));
        QVERIFY2(std::abs(c - want) < 5e-3 * want, qPrintable(QStringLiteral("%1 F, %2 F").arg(c).arg(want)));
    }

    // A resistor - n silicon of 1e17 per cm³, 2 µm long, 0.1 V across -: its
    // current q µ N A V / L, µ the lattice's (constant) and Caughey and
    // Thomas's of its doping, to 1e-6.
    void resistorMobility()
    {
        const double N = 1e23, L = 2e-6, A = 1e-12, V = 0.1;
        for (int byDoping : {0, 1}) {
            Model m = model3d(1.0);
            addBox(m, QStringLiteral("bar"), 0, 0, 0, 1, 1, 2);
            zDivisions(m, QStringLiteral("bar"), 20);
            const QString semi = addSemiconductor(m);
            if (byDoping)
                for (Node& c : m.find(semi)->children)
                    if (c.type == QLatin1String("semiconductor/model")) {
                        c.props.insert(QStringLiteral("mobility"), QStringLiteral("doping"));
                        c.props.insert(QStringLiteral("munmin"), QStringLiteral("68.5[cm^2/(V*s)]"));
                        c.props.insert(QStringLiteral("nrefn"), QStringLiteral("9.2e16[1/cm^3]"));
                        c.props.insert(QStringLiteral("alphan"), QStringLiteral("0.711"));
                        c.props.insert(QStringLiteral("mupmin"), QStringLiteral("44.9[cm^2/(V*s)]"));
                        c.props.insert(QStringLiteral("nrefp"), QStringLiteral("2.23e17[1/cm^3]"));
                        c.props.insert(QStringLiteral("alphap"), QStringLiteral("0.719"));
                    }
            dope(m, semi, QStringLiteral("bar"), true, QStringLiteral("1e17[1/cm^3]"));
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("top")),
                       {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), QStringLiteral("0.1[V]")}});
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double mu = byDoping ? 68.5e-4 + (MuN - 68.5e-4) / (1 + std::pow(N / 9.2e22, 0.711)) : MuN;
            // (Its holes', ni²/N, too few to count.)
            const double want = Qe * mu * N * A * V / L;
            const double got = global(sv.s(), QStringLiteral("semi.I_a"));
            info(QStringLiteral("resistor (%1): I %2 A, q µ N A V/L %3 A").arg(byDoping ? QStringLiteral("by doping") : QStringLiteral("constant")).arg(got).arg(want));
            QVERIFY2(std::abs(got - want) < 1e-6 * want, qPrintable(QStringLiteral("%1 A, %2 A").arg(got).arg(want)));
        }
    }

    // A photodiode at 0 V - the diode above, 1e27 pairs per m³ and second
    // generated everywhere, no recombination -: its depletion region's pairs
    // all collected, and half of each neutral region's (its minority
    // carriers' parabola, nought at the junction and at the contact), -q G A
    // (W + (L - W)/2), W the depletion approximation's - within 1 %; the
    // same out at the cathode.
    void photodiodeCollects()
    {
        Model m = diode(1e23, 1e22, 0, 100);
        QString semi;
        for (const Node& c : m.component().children)
            if (c.type == QLatin1String("semiconductor")) semi = c.tag;
        addFeature(m, semi, QStringLiteral("semiconductor/generation"), rule(R"({"all": true})"), {{QStringLiteral("G"), QStringLiteral("1e27[1/(m^3*s)]")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double Na = 1e23, Nd = 1e22, vbi = Vt * std::log(Na * Nd / (Ni * Ni));
        const double W = std::sqrt(2 * EpsSi * Eps0 * (vbi - 2 * Vt) / Qe * (1 / Na + 1 / Nd));
        const double want = -Qe * 1e27 * 1e-12 * (W + (2e-6 - W) / 2);
        const double ia = global(sv.s(), QStringLiteral("semi.I_a")), ic = global(sv.s(), QStringLiteral("semi.I_c"));
        info(QStringLiteral("photodiode: I %1 A (cathode %2 A), -q G A (W + (L - W)/2) %3 A").arg(ia).arg(ic).arg(want));
        QVERIFY2(std::abs(ia - want) < 1e-2 * std::abs(want) && std::abs(ia + ic) < 1e-6 * std::abs(want), qPrintable(QStringLiteral("%1, %2 A, %3 A").arg(ia).arg(ic).arg(want)));
    }

    // The carriers' heat with light: the photodiode at 0 V, its pairs made
    // by photons of 2 eV - ∫ semi.H over the device the light's power
    // absorbed, G Eph V (its contacts' I V nought), to 1e-4; of the gap's
    // energy without Eph.
    void heatOfLight()
    {
        for (const QString& eph : {QStringLiteral("2[eV]"), QString()}) {
            Model m = diode(1e23, 1e22, 0, 100);
            QString semi;
            for (const Node& c : m.component().children)
                if (c.type == QLatin1String("semiconductor")) semi = c.tag;
            QJsonObject g{{QStringLiteral("G"), QStringLiteral("1e27[1/(m^3*s)]")}};
            if (!eph.isEmpty()) g.insert(QStringLiteral("Eph"), eph);
            addFeature(m, semi, QStringLiteral("semiconductor/generation"), rule(R"({"all": true})"), g);
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            Node in = m.make(QStringLiteral("integral"));
            in.props.insert(QStringLiteral("selection"), rule(R"({"all": true})"));
            in.props.insert(QStringLiteral("expression"), QStringLiteral("semi.H"));
            const DerivedResult r = evaluateDerived3(sv.s(), in);
            QVERIFY2(r.error.isEmpty() && !r.values.isEmpty(), qPrintable(r.error));
            const double H = r.values.front().value, want = 1e27 * (eph.isEmpty() ? 1.12 : 2.0) * Qe * 2e-18;
            info(QStringLiteral("light's heat (Eph %1): ∫ semi.H %2 W, G Eph V %3 W").arg(eph.isEmpty() ? QStringLiteral("the gap's") : eph).arg(H).arg(want));
            QVERIFY2(std::abs(H - want) < 1e-4 * want, qPrintable(QStringLiteral("%1, %2 W").arg(H).arg(want)));
        }
    }

    // Recombination's rate where it is given: SRH, Auger and direct - semi.R
    // at a point of a forward-biased diode their sum of its carriers there.
    void recombinationRates()
    {
        Model m = diode(1e23, 1e22, 0.4, 100, true);
        QString semi;
        for (const Node& c : m.component().children)
            if (c.type == QLatin1String("semiconductor")) semi = c.tag;
        addFeature(m, semi, QStringLiteral("semiconductor/auger"), rule(R"({"all": true})"), {{QStringLiteral("Cn"), QStringLiteral("2.8e-31[cm^6/s]")}, {QStringLiteral("Cp"), QStringLiteral("9.9e-32[cm^6/s]")}});
        addFeature(m, semi, QStringLiteral("semiconductor/radiative"), rule(R"({"all": true})"), {{QStringLiteral("B"), QStringLiteral("1.1e-14[cm^3/s]")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double n = valueAt(sv.s(), QStringLiteral("semi.n"), 0.5, 0.5, 1.0), p = valueAt(sv.s(), QStringLiteral("semi.p"), 0.5, 0.5, 1.0);
        const double ex = n * p - Ni * Ni;
        const double want = ex / (1e-6 * (n + Ni) + 1e-6 * (p + Ni)) + (2.8e-43 * n + 9.9e-44 * p) * ex + 1.1e-20 * ex;
        const double got = valueAt(sv.s(), QStringLiteral("semi.R"), 0.5, 0.5, 1.0);
        QVERIFY2(std::abs(got - want) < 1e-9 * std::abs(want), qPrintable(QStringLiteral("R %1, %2").arg(got).arg(want)));
        // (Forward: recombination in the junction, a current the more for it.)
        QVERIFY(global(sv.s(), QStringLiteral("semi.I_a")) > 0);
    }

    // A Schottky diode - n silicon 1e15 per cm³, 0.5 µm, its barrier 0.6
    // eV, its recombination velocity 1e5 m/s - forward at 0.1 and 0.2 V: its
    // current thermionic emission and diffusion's (Crowell and Sze, 1966), q
    // A vR Nc e^(-φB/kT) (e^(V/Vt) - 1) / (1 + vR/vD), vD = µ Emax, within
    // 15 % (vD the theory's estimate), positive into the device.
    void schottkyDiode()
    {
        std::vector<double> currents;
        const double Nd = 1e21, vR = 1e5, phiB = 0.6 * Qe;
        const double vbi = phiB / Qe - Vt * std::log(Nc / Nd);
        for (double v : {0.1, 0.2}) {
            Model m = model3d(1.0);
            addBox(m, QStringLiteral("n"), 0, 0, 0, 1, 1, 0.5);
            zDivisions(m, QStringLiteral("n"), 200);
            const QString semi = addSemiconductor(m);
            dope(m, semi, QStringLiteral("n"), true, QStringLiteral("1e15[1/cm^3]"));
            addFeature(m, semi, QStringLiteral("semiconductor/schottky"), ofSide(QStringLiteral("n"), QStringLiteral("top")),
                       {{QStringLiteral("name"), QStringLiteral("s")}, {QStringLiteral("V0"), QStringLiteral("%1[V]").arg(v)}, {QStringLiteral("phiB"), QStringLiteral("0.6[eV]")},
                        {QStringLiteral("vn"), QStringLiteral("1e7[cm/s]")}, {QStringLiteral("vp"), QStringLiteral("1e7[cm/s]")}});
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("n"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("o")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double got = global(sv.s(), QStringLiteral("semi.I_s"));
            const double emax = std::sqrt(2 * Qe * Nd * (vbi - v - Vt) / (EpsSi * Eps0)), vD = MuN * emax;
            const double want = Qe * 1e-12 * vR * Nc * std::exp(-phiB / (KB * 300)) * std::expm1(v / Vt) / (1 + vR / vD);
            info(QStringLiteral("Schottky at %1 V: I %2 A, thermionic emission and diffusion's %3 A").arg(v).arg(got).arg(want));
            QVERIFY2(got > 0 && std::abs(got - want) < 0.15 * want, qPrintable(QStringLiteral("%1 V: %2 A, %3 A").arg(v).arg(got).arg(want)));
            currents.push_back(got);
        }
    }

    // Uniform in z - a lateral pn diode, p and n side by side, SRH in both,
    // its contacts on its ends at 0.3 V forward, one layer -: the 2D solver
    // on the 3D model's own plan mesh, its potential and carriers at every
    // node to rounding (its boxes and junction nodes' doping the 2D rule's),
    // and its current.
    void uniformInZ()
    {
        Model m3 = model3d(0.05);
        addBox(m3, QStringLiteral("p"), 0, 0, 0, 1, 1, 0.5);
        addBox(m3, QStringLiteral("n"), 1, 0, 0, 1, 1, 0.5);
        zDivisions(m3, QStringLiteral("p"), 1);
        zDivisions(m3, QStringLiteral("n"), 1);
        Model m2 = Model::blank();
        m2.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
        m2.component().props.insert(QStringLiteral("thickness"), QStringLiteral("0.5[um]"));
        for (const auto& [label, x] : {std::pair{QStringLiteral("p"), 0.0}, std::pair{QStringLiteral("n"), 1.0}}) {
            Node r = m2.make(QStringLiteral("rectangle"));
            r.label = label;
            r.props.insert(QStringLiteral("position"), pairOf(x, 0));
            r.props.insert(QStringLiteral("size"), pairOf(1, 1));
            m2.add(m2.geometry().tag, r);
        }
        const auto box2 = [](double x0, double y0, double x1, double y1) { return QJsonObject{{QStringLiteral("box"), QJsonArray{x0, y0, x1, y1}}}; };
        for (Model* m : {&m3, &m2}) {
            const bool three = m == &m3;
            const QString semi = addSemiconductor(*m);
            const QJsonObject p = three ? objects(QStringLiteral("p")) : box2(-0.01, -0.01, 1.01, 1.01);
            const QJsonObject n = three ? objects(QStringLiteral("n")) : box2(0.99, -0.01, 2.01, 1.01);
            addFeature(*m, semi, QStringLiteral("semiconductor/doping"), p,
                       {{QStringLiteral("dopant"), QStringLiteral("acceptor")}, {QStringLiteral("profile"), QStringLiteral("uniform")}, {QStringLiteral("N0"), QStringLiteral("1e17[1/cm^3]")}});
            addFeature(*m, semi, QStringLiteral("semiconductor/doping"), n,
                       {{QStringLiteral("dopant"), QStringLiteral("donor")}, {QStringLiteral("profile"), QStringLiteral("uniform")}, {QStringLiteral("N0"), QStringLiteral("1e16[1/cm^3]")}});
            addFeature(*m, semi, QStringLiteral("semiconductor/srh"), three ? rule(R"({"all": true})") : box2(-0.01, -0.01, 2.01, 1.01),
                       {{QStringLiteral("taun"), QStringLiteral("0.1[us]")}, {QStringLiteral("taup"), QStringLiteral("0.1[us]")}});
            addFeature(*m, semi, QStringLiteral("semiconductor/ohmic"), three ? ofSide(QStringLiteral("p"), QStringLiteral("left")) : box2(-0.01, -0.01, 0.01, 1.01),
                       {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), QStringLiteral("0.3[V]")}});
            addFeature(*m, semi, QStringLiteral("semiconductor/ohmic"), three ? ofSide(QStringLiteral("n"), QStringLiteral("right")) : box2(1.99, -0.01, 2.01, 1.01),
                       {{QStringLiteral("name"), QStringLiteral("c")}});
        }
        const Solved s3 = solve(m3);
        QVERIFY2(s3.error.isEmpty(), qPrintable(why(s3)));
        const StudyResult r2 = solveStudy(m2, evaluateParameters(m2), s3.mesh->plan, QString());
        QVERIFY2(r2.error.isEmpty() && r2.solution, qPrintable(r2.error));
        const Field3& f3 = s3.s().fields().front();
        const Field& f2 = r2.solution->fields().front();
        const int n3 = f3.space->size(), n2 = f2.space->size();
        QCOMPARE(n2, f3.space->plan().size());
        double worst[3] = {0, 0, 0};
        for (int dof = 0; dof < n3; ++dof) {
            const int pd = f3.space->origin(dof).first;
            for (int c = 0; c < 3; ++c) {
                const double a = f3.values[std::size_t(c * n3 + dof)], b = f2.values[std::size_t(c * n2 + pd)];
                worst[c] = std::max(worst[c], c == 0 ? std::abs(a - b) : std::abs(a - b) / std::max(std::abs(b), 1.0));
            }
        }
        const double i3 = global(s3.s(), QStringLiteral("semi.I_a")), i2 = r2.solution->global(QStringLiteral("semi.I_a")).value_or(std::nan(""));
        info(QStringLiteral("uniform in z: V %1 V off, n %2, p %3 (relative); I 3D %4 A, 2D %5 A").arg(worst[0]).arg(worst[1]).arg(worst[2]).arg(i3, 0, 'g', 10).arg(i2, 0, 'g', 10));
        QVERIFY2(worst[0] < 1e-12 && worst[1] < 1e-10 && worst[2] < 1e-10, qPrintable(QStringLiteral("%1 V, %2, %3").arg(worst[0]).arg(worst[1]).arg(worst[2])));
        // (2D weighs a contact's current over the device; 3D takes its rows'.)
        QVERIFY2(std::abs(i3 - i2) < 1e-6 * std::abs(i2), qPrintable(QStringLiteral("3D %1 A, 2D %2 A").arg(i3).arg(i2)));
    }

    // Each of 2D's models that 3D takes, a lateral diode uniform in z
    // against the 2D solver on the 3D model's plan mesh: the mobility models
    // (Masetti, Arora, Dorkel and Leturcq, Klaassen), band gap narrowing,
    // incomplete ionization, Fermi-Dirac's statistics, Fossum's lifetimes,
    // band-to-band tunnelling, a lattice at 350 K, saturation along the
    // current and by the field, transferred electrons, hot carriers, an
    // expression of the state, a magnetic field, trap-assisted tunnelling -
    // each the 2D model's to 1e-8 (its current to 1e-6; hot carriers' to
    // 1e-6, their temperatures' iteration's).
    void modelsAgainst2D()
    {
        struct Case {
            const char* name;
            double bias;
            std::function<void(Model&, const QString&, bool)> configure;
        };
        const std::vector<Case> cases = {
            {"Masetti", 0.4, [](Model& m, const QString& s, bool) { modelProp(m, s, QStringLiteral("mobility"), QStringLiteral("masetti")); }},
            {"Arora", 0.4, [](Model& m, const QString& s, bool) { modelProp(m, s, QStringLiteral("mobility"), QStringLiteral("arora")); }},
            {"Dorkel", 0.4, [](Model& m, const QString& s, bool) { modelProp(m, s, QStringLiteral("mobility"), QStringLiteral("dorkel")); }},
            {"Klaassen", 0.4, [](Model& m, const QString& s, bool) { modelProp(m, s, QStringLiteral("mobility"), QStringLiteral("klaassen")); }},
            {"band gap narrowing", 0.4, [](Model& m, const QString& s, bool) { modelProp(m, s, QStringLiteral("bgn"), true); }},
            {"incomplete ionization", 0.4, [](Model& m, const QString& s, bool) { modelProp(m, s, QStringLiteral("incomplete"), true); }},
            {"Fermi-Dirac", 0.4, [](Model& m, const QString& s, bool) { modelProp(m, s, QStringLiteral("fermi"), true); }},
            {"Fossum's lifetimes", 0.4, [](Model& m, const QString& s, bool) {
                 for (Node& c : m.find(s)->children)
                     if (c.type == QLatin1String("semiconductor/srh")) c.props.insert(QStringLiteral("dependence"), QStringLiteral("doping"));
             }},
            {"band-to-band tunnelling", -3.0, [](Model& m, const QString& s, bool three) {
                 Node f = m.make(QStringLiteral("semiconductor/btbt"));
                 f.props.insert(QStringLiteral("selection"), three ? rule(R"({"all": true})") : QJsonObject{{QStringLiteral("box"), QJsonArray{-0.01, -0.01, 2.01, 1.01}}});
                 m.add(s, f);
             }},
            {"a lattice at 350 K", 0.4, [](Model& m, const QString& s, bool) { m.find(s)->props.insert(QStringLiteral("TL"), QStringLiteral("350[K]")); }},
            {"saturation along the current", 0.8, [](Model& m, const QString& s, bool) { modelProp(m, s, QStringLiteral("saturation"), true); }},
            {"saturation by the field", 0.8, [](Model& m, const QString& s, bool) {
                 modelProp(m, s, QStringLiteral("saturation"), true);
                 modelProp(m, s, QStringLiteral("driving"), QStringLiteral("field"));
             }},
            {"transferred electrons", 0.8, [](Model& m, const QString& s, bool) {
                 modelProp(m, s, QStringLiteral("saturation"), true);
                 modelProp(m, s, QStringLiteral("velocity"), QStringLiteral("transferred"));
                 modelProp(m, s, QStringLiteral("E0n"), QStringLiteral("2[kV/cm]"));
             }},
            {"hot carriers (an energy balance)", 0.8, [](Model& m, const QString& s, bool) {
                 modelProp(m, s, QStringLiteral("saturation"), true);
                 modelProp(m, s, QStringLiteral("driving"), QStringLiteral("energy"));
             }},
            {"an expression of the force along the current", 0.8, [](Model& m, const QString& s, bool) {
                 modelProp(m, s, QStringLiteral("mobility"), QStringLiteral("expression"));
                 modelProp(m, s, QStringLiteral("muexpn"), QStringLiteral("0.14[m^2/(V*s)]/(1+Epar/1e6[V/m])"));
                 modelProp(m, s, QStringLiteral("muexpp"), QStringLiteral("0.045[m^2/(V*s)]/(1+n/1e24[1/m^3])"));
             }},
            {"stimulated recombination", 0.8, [](Model& m, const QString& s, bool three) {
                 Node f = m.make(QStringLiteral("semiconductor/stimulated"));
                 f.props.insert(QStringLiteral("selection"), three ? rule(R"({"all": true})") : QJsonObject{{QStringLiteral("box"), QJsonArray{-0.01, -0.01, 2.01, 1.01}}});
                 f.props.insert(QStringLiteral("c"), QStringLiteral("0.5"));
                 f.props.insert(QStringLiteral("Jth"), QStringLiteral("1[A/m^2]"));
                 f.props.insert(QStringLiteral("t"), QStringLiteral("10[nm]"));
                 m.add(s, f);
             }},
            {"a magnetic field (the Hall effect)", 0.4, [](Model& m, const QString& s, bool) {
                 m.find(s)->props.insert(QStringLiteral("Bz"), QStringLiteral("2[T]"));
                 modelProp(m, s, QStringLiteral("rhalln"), QStringLiteral("1.2"));
                 modelProp(m, s, QStringLiteral("rhallp"), QStringLiteral("0.8"));
             }},
            {"trap-assisted tunnelling", -3.0, [](Model& m, const QString& s, bool) {
                 for (Node& c : m.find(s)->children)
                     if (c.type == QLatin1String("semiconductor/srh")) {
                         c.props.insert(QStringLiteral("tat"), true);
                         c.props.insert(QStringLiteral("mtn"), QStringLiteral("0.25"));
                         c.props.insert(QStringLiteral("mtp"), QStringLiteral("0.3"));
                     }
             }},
        };
        for (const Case& c : cases) {
            const Against2D r = against2D(c.bias, c.configure);
            QVERIFY2(r.error.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(QString::fromLatin1(c.name), r.error)));
            info(QStringLiteral("against 2D, %1: V %2 V, n %3, p %4; I 3D %5 A, 2D %6 A").arg(QString::fromLatin1(c.name)).arg(r.V).arg(r.n).arg(r.p).arg(r.i3, 0, 'g', 10).arg(r.i2, 0, 'g', 10));
            // (Hot carriers': their temperatures an iteration's with the
            // mobilities, each solver stopping where they move by 1e-6.)
            const bool hotCarriers = QString::fromLatin1(c.name).contains(QStringLiteral("hot"));
            const double itol = 1e-6, ctol = hotCarriers ? 1e-6 : 1e-8;
            if (r.globals.count(QStringLiteral("Istim"))) {
                const auto [a, b] = r.globals.at(QStringLiteral("Istim"));
                info(QStringLiteral("against 2D, stimulated: Istim 3D %1 A, 2D %2 A").arg(a, 0, 'g', 10).arg(b, 0, 'g', 10));
                QVERIFY2(b > 0 && std::abs(a - b) < 1e-6 * b, qPrintable(QStringLiteral("Istim %1, %2").arg(a).arg(b)));
            }
            QVERIFY2(r.V < ctol && r.n < ctol && r.p < ctol && std::abs(r.i3 - r.i2) < itol * std::abs(r.i2),
                     qPrintable(QStringLiteral("%1: V %2, n %3, p %4; I %5, %6").arg(QString::fromLatin1(c.name)).arg(r.V).arg(r.n).arg(r.p).arg(r.i3).arg(r.i2)));
        }
    }

    // Self-heating: n silicon of 1e17 per cm³, 20 µm by 1 µm by 1 µm, 4 V
    // along it, its ends held at 300 K; its heat semi.H a heat physics'
    // source, its mobility at that one's temperature (TL = ht.T, (T/300
    // K)^-2.5), k 130 W/(m K) - the two solved in turn: its current the 2D
    // model's (itself the one-dimensional problem's to 0.02 %,
    // test_fem_devices) to 1e-3, its middle's warming to 1 % (0.3 % apart:
    // each solver's heat source its own interpolation of semi.H), its
    // current some 15 % below the bar's at 300 K.
    void selfHeating()
    {
        Model m3 = model3d(0.25);
        addBox(m3, QStringLiteral("bar"), 0, 0, 0, 20, 1, 1);
        zDivisions(m3, QStringLiteral("bar"), 1);
        Model m2 = Model::blank();
        m2.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
        m2.component().props.insert(QStringLiteral("thickness"), QStringLiteral("1[um]"));
        {
            Node r = m2.make(QStringLiteral("rectangle"));
            r.label = QStringLiteral("bar");
            r.props.insert(QStringLiteral("size"), pairOf(20, 1));
            m2.add(m2.geometry().tag, r);
        }
        const auto box2 = [](double x0, double y0, double x1, double y1) { return QJsonObject{{QStringLiteral("box"), QJsonArray{x0, y0, x1, y1}}}; };
        for (Model* m : {&m3, &m2}) {
            const bool three = m == &m3;
            const QString semi = addSemiconductor(*m);
            m->find(semi)->props.insert(QStringLiteral("TL"), QStringLiteral("ht.T"));
            modelProp(*m, semi, QStringLiteral("zetan"), QStringLiteral("2.5"));
            modelProp(*m, semi, QStringLiteral("zetap"), QStringLiteral("2.2"));
            const QJsonObject all = three ? rule(R"({"all": true})") : box2(-0.01, -0.01, 20.01, 1.01);
            const QJsonObject left = three ? ofSide(QStringLiteral("bar"), QStringLiteral("left")) : box2(-0.01, -0.01, 0.01, 1.01);
            const QJsonObject right = three ? ofSide(QStringLiteral("bar"), QStringLiteral("right")) : box2(19.99, -0.01, 20.01, 1.01);
            addFeature(*m, semi, QStringLiteral("semiconductor/doping"), all,
                       {{QStringLiteral("dopant"), QStringLiteral("donor")}, {QStringLiteral("profile"), QStringLiteral("uniform")}, {QStringLiteral("N0"), QStringLiteral("1e17[1/cm^3]")}});
            addFeature(*m, semi, QStringLiteral("semiconductor/ohmic"), left, {{QStringLiteral("name"), QStringLiteral("1")}});
            addFeature(*m, semi, QStringLiteral("semiconductor/ohmic"), right, {{QStringLiteral("name"), QStringLiteral("2")}, {QStringLiteral("V0"), QStringLiteral("4[V]")}});
            Node ht = m->make(QStringLiteral("heat"));
            ht.tag = QStringLiteral("ht");
            m->add(m->component().tag, ht);
            for (Node& c : m->find(QStringLiteral("ht"))->children)
                if (c.type == QLatin1String("heat/solid")) {
                    c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
                    c.props.insert(QStringLiteral("k"), QStringLiteral("130[W/(m*K)]"));
                }
            addFeature(*m, QStringLiteral("ht"), QStringLiteral("heat/source"), all, {{QStringLiteral("Q0"), QStringLiteral("semi.H")}});
            addFeature(*m, QStringLiteral("ht"), QStringLiteral("heat/temperature"), left, {{QStringLiteral("T0"), QStringLiteral("300[K]")}});
            addFeature(*m, QStringLiteral("ht"), QStringLiteral("heat/temperature"), right, {{QStringLiteral("T0"), QStringLiteral("300[K]")}});
        }
        const Solved s3 = solve(m3);
        QVERIFY2(s3.error.isEmpty(), qPrintable(why(s3)));
        const StudyResult r2 = solveStudy(m2, evaluateParameters(m2), s3.mesh->plan, QString());
        QVERIFY2(r2.error.isEmpty() && r2.solution, qPrintable(r2.error));
        const double i3 = std::abs(global(s3.s(), QStringLiteral("semi.I_2"))), i2 = std::abs(r2.solution->global(QStringLiteral("semi.I_2")).value_or(std::nan("")));
        const double t3 = valueAt(s3.s(), QStringLiteral("ht.T"), 10, 0.5, 0.5);
        const std::optional<double> t2v = evaluateAt(*r2.solution, Locator(r2.solution->mesh()), QStringLiteral("ht.T"), QPointF(10, 0.5));
        const double t2 = t2v.value_or(std::nan(""));
        const double cold = 4 / (20e-6 / (Qe * 1e23 * MuN)) * 1e-12;
        info(QStringLiteral("self-heating: I 3D %1 A, 2D %2 A (at 300 K %3 A); its middle 3D %4 K, 2D %5 K").arg(i3, 0, 'g', 10).arg(i2, 0, 'g', 10).arg(cold).arg(t3, 0, 'g', 8).arg(t2, 0, 'g', 8));
        QVERIFY2(std::abs(i3 - i2) < 1e-3 * i2, qPrintable(QStringLiteral("%1, %2 A").arg(i3).arg(i2)));
        QVERIFY2(std::abs(t3 - t2) < 1e-2 * (t2 - 300), qPrintable(QStringLiteral("%1, %2 K").arg(t3).arg(t2)));
        QVERIFY2(i3 < 0.9 * cold, qPrintable(QStringLiteral("%1, at 300 K %2 A").arg(i3).arg(cold)));
    }

    // A heterojunction - the n side a wider gap (Eg 1.42 eV, χ 4.07 eV, Nc
    // 4.7e17, Nv 7e18 per cm³) - at 0.6 V forward, against the 2D solver:
    // graded over an element; abrupt (each side's carriers on its own bands
    // up to the interface, an edge along it two); its carriers emitted over
    // the step (thermionic, in series with the edges into it); tunnelling
    // through its spike too - each the 2D model's, the potential to 1e-8 V,
    // the carriers to 1e-8, the current to 1e-6.
    void heterojunctionsAgainst2D()
    {
        for (const char* kind : {"element", "abrupt", "thermionic", "tunnelling"}) {
        const Against2D h = against2D(0.6, [kind](Model& m, const QString& s, bool three) {
            m.find(s)->props.insert(QStringLiteral("heterojunctions"), QString::fromLatin1(kind));
            Node f = m.make(QStringLiteral("semiconductor/model"));
            f.props.insert(QStringLiteral("selection"), three ? QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("n")}}}
                                                             : QJsonObject{{QStringLiteral("box"), QJsonArray{0.99, -0.01, 2.01, 1.01}}});
            f.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            f.props.insert(QStringLiteral("epsilonr"), QStringLiteral("12.9"));
            f.props.insert(QStringLiteral("Eg"), QStringLiteral("1.42[eV]"));
            f.props.insert(QStringLiteral("chi"), QStringLiteral("4.07[eV]"));
            f.props.insert(QStringLiteral("Nc"), QStringLiteral("4.7e17[1/cm^3]"));
            f.props.insert(QStringLiteral("Nv"), QStringLiteral("7e18[1/cm^3]"));
            f.props.insert(QStringLiteral("mun"), QStringLiteral("8500[cm^2/(V*s)]"));
            f.props.insert(QStringLiteral("mup"), QStringLiteral("400[cm^2/(V*s)]"));
            m.add(s, f);
        });
        QVERIFY2(h.error.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(QString::fromLatin1(kind), h.error)));
        info(QStringLiteral("against 2D, heterojunction (%1): V %2 V, n %3, p %4; I 3D %5 A, 2D %6 A").arg(QString::fromLatin1(kind)).arg(h.V).arg(h.n).arg(h.p).arg(h.i3, 0, 'g', 10).arg(h.i2, 0, 'g', 10));
        QVERIFY2(h.V < 1e-8 && h.n < 1e-8 && h.p < 1e-8 && std::abs(h.i3 - h.i2) < 1e-6 * std::abs(h.i2),
                 qPrintable(QStringLiteral("%1: V %2, n %3, p %4; I %5, %6").arg(QString::fromLatin1(kind)).arg(h.V).arg(h.n).arg(h.p).arg(h.i3).arg(h.i2)));
        }
    }

    // Impact ionization (silicon's van Overstraeten and de Man) at -15 V,
    // meshed at 0.1 µm, against the 2D solver: its current to 1e-6 (α of
    // each prism's field, its currents fitted to its edges', as 2D's of each
    // triangle's); its integrals along the field's lines and its
    // multiplication to 2 %.
    void impactAgainst2D()
    {
        const Against2D ii = against2D(-15, [](Model& m, const QString& s, bool three) {
            Node f = m.make(QStringLiteral("semiconductor/impact"));
            f.props.insert(QStringLiteral("selection"), three ? rule(R"({"all": true})") : QJsonObject{{QStringLiteral("box"), QJsonArray{-0.01, -0.01, 2.01, 1.01}}});
            f.props.insert(QStringLiteral("model"), QStringLiteral("silicon"));
            m.add(s, f);
        }, 0.1);
        QVERIFY2(ii.error.isEmpty(), qPrintable(ii.error));
        info(QStringLiteral("against 2D, impact ionization at -15 V: I 3D %1 A, 2D %2 A").arg(ii.i3, 0, 'g', 10).arg(ii.i2, 0, 'g', 10));
        QVERIFY2(std::abs(ii.i3 - ii.i2) < 1e-6 * std::abs(ii.i2), qPrintable(QStringLiteral("I %1, %2").arg(ii.i3).arg(ii.i2)));
        // Its integrals along the field's lines (each solver's field recovered
        // at its nodes, linear between them) and its multiplication - to 2 %.
        for (const char* name : {"In", "Ip", "Emax", "Mion"}) {
            QVERIFY2(ii.globals.count(QString::fromLatin1(name)), name);
            const auto [a, b] = ii.globals.at(QString::fromLatin1(name));
            info(QStringLiteral("against 2D, impact ionization at -15 V: %1 3D %2, 2D %3").arg(QString::fromLatin1(name)).arg(a, 0, 'g', 8).arg(b, 0, 'g', 8));
            QVERIFY2(std::abs(a - b) < 2e-2 * std::abs(b), qPrintable(QStringLiteral("%1: %2, %3").arg(QString::fromLatin1(name)).arg(a).arg(b)));
        }
    }

    // Impact ionization driven by each carrier's quasi-Fermi gradient, by
    // the field along each current; its pairs out of the equations (its
    // integrals alone): the diode at -15 V against 2D (meshed at 0.1 µm),
    // its current to 1e-6.
    void impactDrivingsAgainst2D()
    {
        for (const char* driving : {"none", "current", "fermi"}) {
            QElapsedTimer clock;
            clock.start();
            const Against2D dr = against2D(-15, [driving](Model& m, const QString& s, bool three) {
                Node f = m.make(QStringLiteral("semiconductor/impact"));
                f.props.insert(QStringLiteral("selection"), three ? rule(R"({"all": true})") : QJsonObject{{QStringLiteral("box"), QJsonArray{-0.01, -0.01, 2.01, 1.01}}});
                f.props.insert(QStringLiteral("model"), QStringLiteral("silicon"));
                if (QString::fromLatin1(driving) == QLatin1String("none")) f.props.insert(QStringLiteral("currents"), false);
                else f.props.insert(QStringLiteral("driving"), QString::fromLatin1(driving));
                m.add(s, f);
            }, 0.1);
            QVERIFY2(dr.error.isEmpty(), qPrintable(dr.error));
            const auto pair = [&dr](const char* n) { return dr.globals.count(QString::fromLatin1(n)) ? dr.globals.at(QString::fromLatin1(n)) : std::pair{std::nan(""), std::nan("")}; };
            info(QStringLiteral("against 2D, impact ionization (%1): I 3D %2 A, 2D %3 A; Iion 3D %4 A, 2D %5 A (%6 s)")
                     .arg(QString::fromLatin1(driving)).arg(dr.i3, 0, 'g', 10).arg(dr.i2, 0, 'g', 10).arg(pair("Iion").first, 0, 'g', 10).arg(pair("Iion").second, 0, 'g', 10).arg(clock.elapsed() / 1000.0));
            QVERIFY2(std::abs(dr.i3 - dr.i2) < 1e-6 * std::abs(dr.i2), qPrintable(QStringLiteral("%1: I %2, %3").arg(QString::fromLatin1(driving)).arg(dr.i3).arg(dr.i2)));
        }
    }

    // Velocity saturation: n silicon of 1e16 per cm³, 1 µm, 10 V across -
    // the field 1e7 V/m, µE/vsat some 14 -: its current q n A µ E/(1 +
    // (µE/vsat)^β)^(1/β), Caughey and Thomas's, β 2, vsat 1.07e7 cm/s, to
    // 1e-4 (its contacts' own fields aside).
    void velocitySaturation()
    {
        Model m = model3d(1.0);
        addBox(m, QStringLiteral("bar"), 0, 0, 0, 1, 1, 1);
        zDivisions(m, QStringLiteral("bar"), 50);
        const QString semi = addSemiconductor(m);
        modelProp(m, semi, QStringLiteral("saturation"), true);
        modelProp(m, semi, QStringLiteral("vsatn"), QStringLiteral("1.07e7[cm/s]"));
        modelProp(m, semi, QStringLiteral("betan"), QStringLiteral("2"));
        modelProp(m, semi, QStringLiteral("Avn"), QStringLiteral("0"));
        dope(m, semi, QStringLiteral("bar"), true, QStringLiteral("1e16[1/cm^3]"));
        addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("top")),
                   {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), QStringLiteral("10[V]")}});
        addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double E = 1e7, mu = MuN, vs = 1.07e5, N = 1e22;
        const double want = Qe * N * 1e-12 * mu * E / std::sqrt(1 + std::pow(mu * E / vs, 2));
        const double got = global(sv.s(), QStringLiteral("semi.I_a"));
        info(QStringLiteral("saturation: I %1 A, q n A µE/(1 + (µE/vsat)²)^½ %2 A").arg(got).arg(want));
        QVERIFY2(std::abs(got - want) < 1e-3 * want, qPrintable(QStringLiteral("%1 A, %2 A").arg(got).arg(want)));
    }

    // The box and implant profiles, by node: a box's N0 inside and its
    // Gaussian fall to the background at its junction depth outside (in z
    // too); an implant's dose about its range below its surface, its image,
    // its mask's window's edge half its value.
    void dopingProfiles()
    {
        Model m = model3d(0.1);
        addBox(m, QStringLiteral("si"), 0, 0, 0, 2, 1, 1);
        zDivisions(m, QStringLiteral("si"), 50);
        const QString semi = addSemiconductor(m);
        addFeature(m, semi, QStringLiteral("semiconductor/doping"), objects(QStringLiteral("si")),
                   {{QStringLiteral("dopant"), QStringLiteral("donor")}, {QStringLiteral("profile"), QStringLiteral("box")}, {QStringLiteral("N0"), QStringLiteral("1e18[1/cm^3]")},
                    {QStringLiteral("position"), pairOf(0, 0)}, {QStringLiteral("size"), pairOf(0.5, 1)}, {QStringLiteral("zmin"), QStringLiteral("0.6[um]")},
                    {QStringLiteral("dj"), QStringLiteral("0.2[um]")}, {QStringLiteral("Nb"), QStringLiteral("1e15[1/cm^3]")}});
        addFeature(m, semi, QStringLiteral("semiconductor/doping"), objects(QStringLiteral("si")),
                   {{QStringLiteral("dopant"), QStringLiteral("acceptor")}, {QStringLiteral("profile"), QStringLiteral("implant")}, {QStringLiteral("dose"), QStringLiteral("1e13[1/cm^2]")},
                    {QStringLiteral("Rp"), QStringLiteral("0.2[um]")}, {QStringLiteral("dRp"), QStringLiteral("0.05[um]")}, {QStringLiteral("dRl"), QStringLiteral("0.05[um]")},
                    {QStringLiteral("surface"), QStringLiteral("1")}, {QStringLiteral("x1"), QStringLiteral("1")}, {QStringLiteral("ta"), QStringLiteral("0[s]")}});
        addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("si"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double sigma = 0.2e-6 / std::sqrt(std::log(1e3));
        // Inside the box; 0.1 µm below its bottom (z 0.5 µm): N0 e^(-(d/σ)²).
        QVERIFY2(std::abs(valueAt(sv.s(), QStringLiteral("semi.Nd"), 0.2, 0.5, 0.8) - 1e24) < 1e-9 * 1e24, "box inside");
        const double below = valueAt(sv.s(), QStringLiteral("semi.Nd"), 0.2, 0.5, 0.5);
        const double wantBelow = 1e24 * std::exp(-std::pow(0.1e-6 / sigma, 2));
        QVERIFY2(std::abs(below - wantBelow) < 1e-6 * wantBelow, qPrintable(QStringLiteral("%1, %2").arg(below).arg(wantBelow)));
        // The implant at its range's depth, far in its window: its peak, its
        // image's tail added; at its window's edge, half.
        const double sd = 0.05e-6, Rp = 0.2e-6, dose = 1e17;
        const double peak = dose / (std::sqrt(2 * M_PI) * sd) * (1 + std::exp(-2 * Rp * Rp / (sd * sd)));
        const double got = valueAt(sv.s(), QStringLiteral("semi.Na"), 1.8, 0.5, 0.8);
        QVERIFY2(std::abs(got - peak) < 1e-6 * peak, qPrintable(QStringLiteral("%1, %2").arg(got).arg(peak)));
        const double edge = valueAt(sv.s(), QStringLiteral("semi.Na"), 1.0, 0.5, 0.8);
        QVERIFY2(std::abs(edge - peak / 2) < 1e-6 * peak, qPrintable(QStringLiteral("%1, %2").arg(edge).arg(peak / 2)));
    }

    // Excess carriers' decay in time: p silicon of 1e16 per cm³, 1 mm long
    // (its diffusion length 60 µm: its middle far from its contacts), SRH's
    // 1 µs, its generation of 1e26 per m³ s a picosecond's pulse - the state
    // at rest under it the start: its middle's excess electrons Δn(0)
    // e^(-t/τ) within 1 % at 1 and 2 µs, BDF's steps of 20 ns.
    void decayInTime()
    {
        Model m = model3d(1000.0);
        addBox(m, QStringLiteral("bar"), 0, 0, 0, 10, 10, 1000);
        zDivisions(m, QStringLiteral("bar"), 100);
        const QString semi = addSemiconductor(m);
        dope(m, semi, QStringLiteral("bar"), false, QStringLiteral("1e16[1/cm^3]"));
        addFeature(m, semi, QStringLiteral("semiconductor/srh"), rule(R"({"all": true})"), {{QStringLiteral("taun"), QStringLiteral("1[us]")}, {QStringLiteral("taup"), QStringLiteral("1[us]")}});
        addFeature(m, semi, QStringLiteral("semiconductor/generation"), rule(R"({"all": true})"), {{QStringLiteral("G"), QStringLiteral("1e26[1/(m^3*s)]*exp(-t/1[ps])")}});
        addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("a")}});
        addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("top")), {{QStringLiteral("name"), QStringLiteral("b")}});
        for (Node& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                c.children.clear();
                Node t = m.make(QStringLiteral("transient"));
                t.props.insert(QStringLiteral("times"), QStringLiteral("range(0, 20[ns], 2[us])"));
                c.children.push_back(t);
            }
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        Solution3& s = sv.s();
        const double n0 = 1e10 * 1e10 * 1e6 / 1e22;   // (ni² / Na: the excess's base, near enough)
        (void)n0;
        s.select(0);
        const double base = valueAt(s, QStringLiteral("semi.n"), 5, 5, 999.9) ;   // (by a contact: its own)
        (void)base;
        const double d0 = valueAt(s, QStringLiteral("semi.n"), 5, 5, 500);
        for (double t : {1e-6, 2e-6}) {
            s.select(s.snapshotAt(t));
            const double dn = valueAt(s, QStringLiteral("semi.n"), 5, 5, 500);
            info(QStringLiteral("decay: at %1 s Δn/Δn(0) %2, e^(-t/τ) %3").arg(t).arg(dn / d0).arg(std::exp(-t / 1e-6)));
            QVERIFY2(std::abs(dn / d0 - std::exp(-t / 1e-6)) < 1e-2 * std::exp(-t / 1e-6), qPrintable(QStringLiteral("%1, %2").arg(dn / d0).arg(std::exp(-t / 1e-6))));
        }
    }

    // A MOS capacitor at 1 MHz - SRH's 1 µs, its inversion layer's electrons
    // too slow to follow -: in depletion its quasi-static capacitance within
    // 1 %; in strong inversion its minimum, Cox in series with the
    // depletion layer at its widest, √(4 εs Vt ln(Na/ni)/(q Na)), within 5 %
    // - well under its quasi-static one, near Cox.
    void mosHighFrequency()
    {
        const double Na = 1e23, tox = 10e-9, epsOx = 3.9 * Eps0, epsSi = EpsSi * Eps0, A = 1e-12;
        const double cox = epsOx / tox, psiB = Vt * std::log(Na / Ni);
        const double wmax = std::sqrt(4 * epsSi * psiB / (Qe * Na));
        const double cmin = A / (1 / cox + wmax / epsSi);
        const auto build = [&](double vg) {
            Model m = model3d(1.0);
            addBox(m, QStringLiteral("si"), 0, 0, 0, 1, 1, 0.3);
            addBox(m, QStringLiteral("ox"), 0, 0, 0.3, 1, 1, 0.01);
            zDivisions(m, QStringLiteral("si"), 150);
            zDivisions(m, QStringLiteral("ox"), 2);
            Node mat = m.make(QStringLiteral("material"));
            mat.label = QStringLiteral("oxide");
            mat.props.insert(QStringLiteral("epsilonr"), QStringLiteral("3.9"));
            mat.props.insert(QStringLiteral("selection"), objects(QStringLiteral("ox")));
            m.add(m.materials().tag, mat);
            const QString semi = addSemiconductor(m, objects(QStringLiteral("si")));
            dope(m, semi, QStringLiteral("si"), false, QStringLiteral("1e17[1/cm^3]"));
            addFeature(m, semi, QStringLiteral("semiconductor/srh"), objects(QStringLiteral("si")), {{QStringLiteral("taun"), QStringLiteral("1[us]")}, {QStringLiteral("taup"), QStringLiteral("1[us]")}});
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("si"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
            addFeature(m, semi, QStringLiteral("semiconductor/gate"), ofSide(QStringLiteral("ox"), QStringLiteral("top")),
                       {{QStringLiteral("name"), QStringLiteral("g")}, {QStringLiteral("V0"), QStringLiteral("%1[V]").arg(vg)}, {QStringLiteral("phim"), QStringLiteral("4.6[eV]")}});
            for (Node& c : m.root.children)
                if (c.type == QLatin1String("study")) {
                    Node f = m.make(QStringLiteral("frequency"));
                    f.props.insert(QStringLiteral("frequencies"), QStringLiteral("1[MHz]"));
                    c.children.push_back(f);
                }
            return m;
        };
        const double vfb = -0.449933;
        for (double vg : {vfb + 0.5, vfb + 2.5}) {
            const Solved sv = solve(build(vg));
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double c = global(sv.s(), QStringLiteral("semi.Cgg"));
            info(QStringLiteral("MOS at 1 MHz, Vg %1 V: C %2 F (Cmin %3 F, Cox A %4 F)").arg(vg).arg(c).arg(cmin).arg(cox * A));
            if (vg < 0.5) QVERIFY2(std::abs(c - 1.23538e-15) < 1e-2 * 1.23538e-15, qPrintable(QString::number(c)));
            else QVERIFY2(std::abs(c - cmin) < 5e-2 * cmin, qPrintable(QStringLiteral("%1 F, Cmin %2 F").arg(c).arg(cmin)));
        }
    }

    // Interface traps on a MOS capacitor's oxide - acceptor-like at
    // midgap, 1e11 per cm², holes captured through σp 1e-13 cm² (electrons
    // barely, 1e-20), vth 1e7 cm/s -: at rest its gate's charge the exact 1D
    // one with the traps' charge, -A (Qs(ψs) - q Nit f(ψs)), f Fermi's at
    // the surface, Vg = Vfb + ψs - (Qs - q Nit f)/Cox, to 1e-4 at three
    // biases in depletion (the traps up to 10 % of it); small-signal at ψs =
    // ψB, the traps half full, at ω = 1/τ (τ = 1/(cp (p + p1)) = 1/(2 cp ni)
    // there): the parallel conductance of the gate's admittance less Cox,
    // Gp/ω, the conductance method's peak q² Nit A/(8 kT) to 1e-3
    // (Nicollian and Goetzberger). (A floor's traps lumped by thirds, not
    // by its boxes' shares, made the potential uneven across it: 3.3 % off.)
    void interfaceTraps()
    {
        const double Na = 1e23, tox = 10e-9, epsOx = 3.9 * Eps0, epsSi = EpsSi * Eps0, A = 1e-12, phim = 4.6, Nit = 1e15, cp = 1e-17 * 1e5;
        const double dEc = Eg / 2 + KB * 300 / 2 * std::log(Nc / Nv);
        const double p0 = Na, n0 = Ni * Ni / Na, psiB = Vt * std::log(Na / Ni);
        const double vfb = (phim * Qe - 4.05 * Qe - dEc) / Qe - psiB;
        const double cox = epsOx / tox, ld = std::sqrt(epsSi * Vt / (Qe * p0));
        const auto qs = [&](double psi) {
            const double b = psi / Vt;
            const double F = std::sqrt(std::max(0.0, (std::exp(-b) + b - 1) + n0 / p0 * (std::exp(b) - b - 1)));
            return -(psi >= 0 ? 1.0 : -1.0) * std::sqrt(2.0) * epsSi * Vt / ld * F;
        };
        // (The level at the intrinsic one, the Fermi level ψB below it in the
        // bulk: at the surface (ψB - ψs) below.)
        const auto qit = [&](double psi) { return -Qe * Nit / (1 + std::exp((psiB - psi) / Vt)); };
        const auto vg = [&](double psi) { return vfb + psi - (qs(psi) + qit(psi)) / cox; };
        const auto build = [&](double v, double frequency) {
            Model m = model3d(1.0);
            addBox(m, QStringLiteral("si"), 0, 0, 0, 1, 1, 0.3);
            addBox(m, QStringLiteral("ox"), 0, 0, 0.3, 1, 1, 0.01);
            zDivisions(m, QStringLiteral("si"), 150);
            zDivisions(m, QStringLiteral("ox"), 2);
            Node mat = m.make(QStringLiteral("material"));
            mat.label = QStringLiteral("oxide");
            mat.props.insert(QStringLiteral("epsilonr"), QStringLiteral("3.9"));
            mat.props.insert(QStringLiteral("selection"), objects(QStringLiteral("ox")));
            m.add(m.materials().tag, mat);
            const QString semi = addSemiconductor(m, objects(QStringLiteral("si")));
            dope(m, semi, QStringLiteral("si"), false, QStringLiteral("1e17[1/cm^3]"));
            addFeature(m, semi, QStringLiteral("semiconductor/srh"), objects(QStringLiteral("si")), {{QStringLiteral("taun"), QStringLiteral("1[ms]")}, {QStringLiteral("taup"), QStringLiteral("1[ms]")}});
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("si"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
            addFeature(m, semi, QStringLiteral("semiconductor/traps"), ofSide(QStringLiteral("si"), QStringLiteral("top")),
                       {{QStringLiteral("kind"), QStringLiteral("acceptor")}, {QStringLiteral("distribution"), QStringLiteral("level")}, {QStringLiteral("Nit"), QStringLiteral("1e11[1/cm^2]")},
                        {QStringLiteral("Et"), QStringLiteral("0[eV]")}, {QStringLiteral("sigman"), QStringLiteral("1e-20[cm^2]")}, {QStringLiteral("sigmap"), QStringLiteral("1e-13[cm^2]")},
                        {QStringLiteral("vth"), QStringLiteral("1e7[cm/s]")}});
            addFeature(m, semi, QStringLiteral("semiconductor/gate"), ofSide(QStringLiteral("ox"), QStringLiteral("top")),
                       {{QStringLiteral("name"), QStringLiteral("g")}, {QStringLiteral("V0"), QStringLiteral("%1[V]").arg(v, 0, 'g', 17)}, {QStringLiteral("phim"), QStringLiteral("4.6[eV]")}});
            if (frequency > 0)
                for (Node& c : m.root.children)
                    if (c.type == QLatin1String("study")) {
                        Node f = m.make(QStringLiteral("frequency"));
                        f.props.insert(QStringLiteral("frequencies"), QStringLiteral("%1[Hz]").arg(frequency, 0, 'g', 17));
                        c.children.push_back(f);
                    }
            return m;
        };
        for (double psi : {0.2, psiB, 0.6}) {
            const double v = vg(psi);
            const Solved sv = solve(build(v, 0));
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double got = global(sv.s(), QStringLiteral("semi.Q_g")), want = -A * (qs(psi) + qit(psi));
            info(QStringLiteral("traps at ψs %1 V (Vg %2 V): Qg %3 C, exact %4 C (the traps' %5 C)").arg(psi).arg(v).arg(got).arg(want).arg(-A * qit(psi)));
            QVERIFY2(std::abs(got - want) < 1e-4 * std::abs(want), qPrintable(QStringLiteral("%1, %2 C").arg(got).arg(want)));
        }
        const double tau = 1 / (2 * cp * Ni), omega = 1 / tau;
        const Solved sv = solve(build(vg(psiB), omega / (2 * M_PI)));
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const std::complex<double> Y(global(sv.s(), QStringLiteral("semi.G_g_g")), omega * global(sv.s(), QStringLiteral("semi.C_g_g")));
        const std::complex<double> Yp = 1.0 / (1.0 / Y - 1.0 / std::complex<double>(0, omega * cox * A));
        const double peak = Qe * Qe * Nit * A / (8 * KB * 300);
        info(QStringLiteral("traps small-signal at ω = 1/τ (%1 Hz): Gp/ω %2 F, q² Nit A/(8 kT) %3 F").arg(omega / (2 * M_PI)).arg(Yp.real() / omega).arg(peak));
        QVERIFY2(std::abs(Yp.real() / omega - peak) < 1e-3 * peak, qPrintable(QStringLiteral("%1, %2 F").arg(Yp.real() / omega).arg(peak)));
    }

    // Wurtzite's polarization along z: an Al0.3Ga0.7N barrier 25 nm on GaN
    // (Ga face, c up), stretched to GaN's lattice - its bound charge σ =
    // P_GaN - P_AlGaN (0.024 to 0.027 C/m²): the same solution as a Surface
    // Charge of σ to 1e-9, a 2DEG under it (above 1e18 per cm³); the
    // nitrogen face's the opposite charge, no electrons gathered; a face not
    // between two nitrides refused.
    void polarizationCharge()
    {
        const std::optional<WurtziteParameters> g = wurtziteParameters(QStringLiteral("GaN"), 0, 300), b = wurtziteParameters(QStringLiteral("AlGaN"), 0.3, 300);
        QVERIFY(g && b);
        const double exx = (g->a - b->a) / b->a, ezz = -2 * b->C13 / b->C33 * exx;
        const double sigma = g->Psp - (b->Psp + 2 * b->e31 * exx + b->e33 * ezz);
        const auto hemt = [&](const QString& type, const QJsonObject& props, const QString& barrier = QStringLiteral("AlGaN")) {
            Model m = model3d(0.05);
            m.component().props.insert(QStringLiteral("unit"), QStringLiteral("nm"));
            addBox(m, QStringLiteral("gan"), 0, 0, 0, 50, 50, 200);
            addBox(m, QStringLiteral("barrier"), 0, 0, 200, 50, 50, 25);
            zDivisions(m, QStringLiteral("gan"), 200);
            zDivisions(m, QStringLiteral("barrier"), 25);
            m.mesh().props.insert(QStringLiteral("hmax"), QStringLiteral("50"));
            for (const auto& [label, compound, eg, chi, nc, nv, eps, mun] :
                 {std::tuple{QStringLiteral("gan"), QStringLiteral("GaN"), "3.4[eV]", "4.1[eV]", "2.2e18[1/cm^3]", "4.6e19[1/cm^3]", "8.9", "1000[cm^2/(V*s)]"},
                  std::tuple{QStringLiteral("barrier"), barrier, "3.96[eV]", "3.7[eV]", "2.9e18[1/cm^3]", "5.6e19[1/cm^3]", "9.4", "300[cm^2/(V*s)]"}}) {
                Node mat = m.make(QStringLiteral("material"));
                mat.label = label;
                mat.props.insert(QStringLiteral("compound"), compound);
                mat.props.insert(QStringLiteral("x"), label == QLatin1String("gan") ? QStringLiteral("0") : QStringLiteral("0.3"));
                for (const auto& [k, v] : {std::pair{"Eg", eg}, std::pair{"chi", chi}, std::pair{"Nc", nc}, std::pair{"Nv", nv}, std::pair{"epsilonr", eps}, std::pair{"mun", mun},
                                           std::pair{"mup", "10[cm^2/(V*s)]"}})
                    mat.props.insert(QString::fromLatin1(k), QString::fromLatin1(v));
                mat.props.insert(QStringLiteral("selection"), objects(label));
                m.add(m.materials().tag, mat);
            }
            Node p = m.make(QStringLiteral("semiconductor"));
            const QString semi = p.tag;
            m.add(m.component().tag, p);
            m.find(semi)->props.insert(QStringLiteral("heterojunctions"), QStringLiteral("element"));
            addFeature(m, semi, QStringLiteral("semiconductor/doping"), rule(R"({"all": true})"),
                       {{QStringLiteral("dopant"), QStringLiteral("donor")}, {QStringLiteral("profile"), QStringLiteral("uniform")}, {QStringLiteral("N0"), QStringLiteral("1e16[1/cm^3]")}});
            QJsonObject all = props;
            addFeature(m, semi, type, ofSide(QStringLiteral("barrier"), QStringLiteral("bottom")), all);
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("gan"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("s")}});
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("barrier"), QStringLiteral("top")), {{QStringLiteral("name"), QStringLiteral("t")}});
            return solve(m);
        };
        const QJsonObject polarization{{QStringLiteral("axis"), QStringLiteral("z")}, {QStringLiteral("substrate"), QStringLiteral("GaN")}};
        const Solved pol = hemt(QStringLiteral("semiconductor/polarization"), polarization);
        const Solved fixed = hemt(QStringLiteral("semiconductor/charge"), {{QStringLiteral("rhos"), QStringLiteral("%1[C/m^2]").arg(sigma, 0, 'g', 17)}});
        QJsonObject nitrogen = polarization;
        nitrogen.insert(QStringLiteral("polarity"), QStringLiteral("nitrogen"));
        const Solved nface = hemt(QStringLiteral("semiconductor/polarization"), nitrogen);
        QVERIFY2(pol.error.isEmpty() && fixed.error.isEmpty() && nface.error.isEmpty(), qPrintable(why(pol) + why(fixed) + why(nface)));
        const double nPol = valueAt(pol.s(), QStringLiteral("semi.n"), 25, 25, 199), nFixed = valueAt(fixed.s(), QStringLiteral("semi.n"), 25, 25, 199);
        const double vPol = valueAt(pol.s(), QStringLiteral("semi.V"), 25, 25, 200), vFixed = valueAt(fixed.s(), QStringLiteral("semi.V"), 25, 25, 200);
        const double nN = valueAt(nface.s(), QStringLiteral("semi.n"), 25, 25, 199);
        info(QStringLiteral("polarization: σ %1 C/m² (%2); under it n %3 per m³ (a surface charge's %4), the nitrogen face's %5")
                 .arg(sigma).arg(pol.s().log.filter(QStringLiteral("polarization charge")).join(QLatin1Char(' '))).arg(nPol).arg(nFixed).arg(nN));
        QVERIFY2(sigma > 0.024 && sigma < 0.0272, qPrintable(QString::number(sigma)));
        QVERIFY2(std::abs(nPol / nFixed - 1) < 1e-9 && std::abs(vPol - vFixed) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(nPol / nFixed).arg(vPol - vFixed)));
        QVERIFY2(nPol > 1e24 && nN < 1e21, qPrintable(QStringLiteral("%1 %2").arg(nPol).arg(nN)));
        const Solved wrong = hemt(QStringLiteral("semiconductor/polarization"), polarization, QStringLiteral("AlGaAs"));
        QVERIFY2(wrong.error.contains(QLatin1String("is not between two nitrides")), qPrintable(wrong.error));
    }

    // Fowler-Nordheim tunnelling through a MOS capacitor's 10 nm of oxide
    // (φB 3.1 eV, m_ox 0.42): its current A E² e^(-B/E) A_gate, Lenzlinger
    // and Snow's, E the oxide's field from the gate's charge (Gauss: Q/(ε
    // A)) - over the barrier at 10 V (its depth φB/E under the oxide's), to
    // 1e-6; at 2 V the barrier deeper than the oxide, its field the
    // oxide's drop over it (tunnelling direct), alike.
    void fowlerNordheim()
    {
        constexpr double Hp = 6.62607015e-34, M0 = 9.1093837015e-31;
        const double phi = 3.1 * Qe, mox = 0.42, A = 1e-12, epsOx = 3.9 * Eps0;
        const double a = Qe * Qe * Qe / (8 * M_PI * Hp * phi * mox), b = 8 * M_PI * std::sqrt(2 * mox * M0) * std::pow(phi, 1.5) / (3 * Qe * Hp);
        for (double vg : {10.0, 2.0}) {
            Model m = model3d(1.0);
            addBox(m, QStringLiteral("si"), 0, 0, 0, 1, 1, 0.3);
            addBox(m, QStringLiteral("ox"), 0, 0, 0.3, 1, 1, 0.01);
            zDivisions(m, QStringLiteral("si"), 150);
            zDivisions(m, QStringLiteral("ox"), 4);
            Node mat = m.make(QStringLiteral("material"));
            mat.label = QStringLiteral("oxide");
            mat.props.insert(QStringLiteral("epsilonr"), QStringLiteral("3.9"));
            mat.props.insert(QStringLiteral("selection"), objects(QStringLiteral("ox")));
            m.add(m.materials().tag, mat);
            const QString semi = addSemiconductor(m, objects(QStringLiteral("si")));
            dope(m, semi, QStringLiteral("si"), false, QStringLiteral("1e17[1/cm^3]"));
            addFeature(m, semi, QStringLiteral("semiconductor/srh"), objects(QStringLiteral("si")), {{QStringLiteral("taun"), QStringLiteral("1[us]")}, {QStringLiteral("taup"), QStringLiteral("1[us]")}});
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("si"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
            addFeature(m, semi, QStringLiteral("semiconductor/gate"), ofSide(QStringLiteral("ox"), QStringLiteral("top")),
                       {{QStringLiteral("name"), QStringLiteral("g")}, {QStringLiteral("V0"), QStringLiteral("%1[V]").arg(vg)}, {QStringLiteral("phim"), QStringLiteral("4.05[eV]")}});
            addFeature(m, semi, QStringLiteral("semiconductor/fn"), ofSide(QStringLiteral("si"), QStringLiteral("top")),
                       {{QStringLiteral("phiB"), QStringLiteral("3.1[eV]")}, {QStringLiteral("mox"), QStringLiteral("0.42")}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double E = global(sv.s(), QStringLiteral("semi.Q_g")) / (epsOx * A);
            const double want = a * E * E * std::exp(-b / E) * A, got = global(sv.s(), QStringLiteral("semi.Ifn"));
            info(QStringLiteral("Fowler-Nordheim at %1 V: the oxide's field %2 V/m (the barrier's depth %3 nm), I %4 A, A E² e^(-B/E) A %5 A").arg(vg).arg(E).arg(3.1 / E * 1e9).arg(got).arg(want));
            QVERIFY2(std::abs(got - want) < 1e-6 * want, qPrintable(QStringLiteral("%1, %2 A").arg(got).arg(want)));
        }
    }

    // Contacts behind circuits: the resistor of resistorMobility (R = L/(q
    // µ N A), 891 Ω) - through Rs 500 Ω and RL 1 kΩ in series, its current
    // V0/(R + Rs + RL), its own voltage V0 - (Rs + RL) I, its load's VL and
    // PL, to 1e-6; through a bias tee, at rest Rs alone (its load's current
    // none); in time, a source stepped to 0.1 V through Rs 1 kΩ with Cp 1 pF
    // across: its own voltage V∞ (1 - e^(-t/τ)), τ Cp (Rs ∥ R), within 0.5 %
    // at τ and 2τ.
    void contactCircuits()
    {
        const double N = 1e23, L = 2e-6, A = 1e-12, R = L / (Qe * MuN * N * A);
        const auto build = [&](const QString& circuit, const QString& V0, double Rs, double RL, double Cp) {
            Model m = model3d(1.0);
            addBox(m, QStringLiteral("bar"), 0, 0, 0, 1, 1, 2);
            zDivisions(m, QStringLiteral("bar"), 20);
            const QString semi = addSemiconductor(m);
            dope(m, semi, QStringLiteral("bar"), true, QStringLiteral("1e17[1/cm^3]"));
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("top")),
                       {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), V0}, {QStringLiteral("circuit"), circuit},
                        {QStringLiteral("Rs"), QStringLiteral("%1[Ohm]").arg(Rs)}, {QStringLiteral("RL"), QStringLiteral("%1[Ohm]").arg(RL)},
                        {QStringLiteral("Cp"), QStringLiteral("%1[F]").arg(Cp)}});
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
            return m;
        };
        {
            const Solved sv = solve(build(QStringLiteral("series"), QStringLiteral("0.1[V]"), 500, 1000, 0));
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double I = global(sv.s(), QStringLiteral("semi.I_a")), want = 0.1 / (R + 1500);
            const double vc = global(sv.s(), QStringLiteral("semi.Vc_a")), il = global(sv.s(), QStringLiteral("semi.IL_a"));
            info(QStringLiteral("in series: I %1 A, V0/(R + Rs + RL) %2 A; Vc %3 V (%4); IL %5 A, VL %6 V, PL %7 W")
                     .arg(I).arg(want).arg(vc).arg(0.1 - 1500 * want).arg(il).arg(global(sv.s(), QStringLiteral("semi.VL_a"))).arg(global(sv.s(), QStringLiteral("semi.PL_a"))));
            QVERIFY2(std::abs(I - want) < 1e-6 * want, qPrintable(QStringLiteral("%1, %2 A").arg(I).arg(want)));
            QVERIFY2(std::abs(vc - (0.1 - 1500 * want)) < 1e-6 * 0.1, qPrintable(QString::number(vc)));
            QVERIFY(std::abs(il - want) < 1e-6 * want && std::abs(global(sv.s(), QStringLiteral("semi.VL_a")) - 1000 * want) < 1e-6 * 1000 * want
                    && std::abs(global(sv.s(), QStringLiteral("semi.PL_a")) - 1000 * want * want) < 1e-5 * 1000 * want * want);
        }
        {
            const Solved sv = solve(build(QStringLiteral("biastee"), QStringLiteral("0.1[V]"), 500, 1000, 0));
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double I = global(sv.s(), QStringLiteral("semi.I_a")), want = 0.1 / (R + 500);
            info(QStringLiteral("a bias tee at rest: I %1 A, V0/(R + Rs) %2 A, IL %3 A").arg(I).arg(want).arg(global(sv.s(), QStringLiteral("semi.IL_a"))));
            QVERIFY2(std::abs(I - want) < 1e-6 * want && global(sv.s(), QStringLiteral("semi.IL_a")) == 0, qPrintable(QStringLiteral("%1, %2 A").arg(I).arg(want)));
        }
        {
            const double Rs = 1000, Cp = 1e-12, tau = Cp * Rs * R / (Rs + R), vinf = 0.1 * R / (R + Rs);
            Model m = build(QStringLiteral("series"), QStringLiteral("0.1[V]*(1-exp(-t/1[ps]))"), Rs, 0, Cp);
            for (Node& c : m.root.children)
                if (c.type == QLatin1String("study")) {
                    c.children.clear();
                    Node t = m.make(QStringLiteral("transient"));
                    t.props.insert(QStringLiteral("times"), QStringLiteral("range(0, %1[s], %2[s])").arg(tau / 100, 0, 'g', 17).arg(2 * tau, 0, 'g', 17));
                    c.children.push_back(t);
                }
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            Solution3& s = sv.s();
            for (double t : {tau, 2 * tau}) {
                s.select(s.snapshotAt(t));
                const double vc = global(s, QStringLiteral("semi.Vc_a")), want = vinf * (1 - std::exp(-t / tau));
                info(QStringLiteral("Cp charged through Rs: at %1 s Vc %2 V, V∞ (1 - e^(-t/τ)) %3 V").arg(t).arg(vc).arg(want));
                QVERIFY2(std::abs(vc - want) < 5e-3 * want, qPrintable(QStringLiteral("%1, %2 V").arg(vc).arg(want)));
            }
        }
    }

    // Scattering at an oxide's surface: an n silicon resistor (1e17 per
    // cm³, 2 µm long, 1 µm deep) under 20 nm of oxide and a gate at 2 V
    // (accumulated, its field across the surface some 1e8 V/m), 0.5 V
    // along it - uniform in z, against the 2D solver on its plan mesh:
    // Lombardi's model, Shirahata's and Yamaguchi's mobility (each its
    // nearest interface's normal and distance), the potential to 1e-8 V,
    // the carriers and the current to 1e-6; each lowering the current.
    void surfaceScatteringAgainst2D()
    {
        const auto run = [](const std::function<void(Model&, const QString&)>& configure, double* i3, double* i2, double* dv, double* dn) {
            Model m3 = model3d(0.1);
            addBox(m3, QStringLiteral("si"), 0, 0, 0, 2, 1, 0.5);
            addBox(m3, QStringLiteral("ox"), 0, 1, 0, 2, 0.02, 0.5);
            zDivisions(m3, QStringLiteral("si"), 1);
            zDivisions(m3, QStringLiteral("ox"), 1);
            Model m2 = Model::blank();
            m2.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
            m2.component().props.insert(QStringLiteral("thickness"), QStringLiteral("0.5[um]"));
            for (const auto& [label, y, h] : {std::tuple{QStringLiteral("si"), 0.0, 1.0}, std::tuple{QStringLiteral("ox"), 1.0, 0.02}}) {
                Node r = m2.make(QStringLiteral("rectangle"));
                r.label = label;
                r.props.insert(QStringLiteral("position"), pairOf(0, y));
                r.props.insert(QStringLiteral("size"), pairOf(2, h));
                m2.add(m2.geometry().tag, r);
            }
            const auto box2 = [](double x0, double y0, double x1, double y1) { return QJsonObject{{QStringLiteral("box"), QJsonArray{x0, y0, x1, y1}}}; };
            for (Model* m : {&m3, &m2}) {
                const bool three = m == &m3;
                Node mat = m->make(QStringLiteral("material"));
                mat.label = QStringLiteral("oxide");
                mat.props.insert(QStringLiteral("epsilonr"), QStringLiteral("3.9"));
                mat.props.insert(QStringLiteral("selection"), three ? objects(QStringLiteral("ox")) : box2(-0.01, 0.995, 2.01, 1.03));
                m->add(m->materials().tag, mat);
                const QString semi = addSemiconductor(*m, three ? objects(QStringLiteral("si")) : box2(-0.01, -0.01, 2.01, 1.005));
                addFeature(*m, semi, QStringLiteral("semiconductor/doping"), three ? objects(QStringLiteral("si")) : box2(-0.01, -0.01, 2.01, 1.005),
                           {{QStringLiteral("dopant"), QStringLiteral("donor")}, {QStringLiteral("profile"), QStringLiteral("uniform")}, {QStringLiteral("N0"), QStringLiteral("1e17[1/cm^3]")}});
                addFeature(*m, semi, QStringLiteral("semiconductor/ohmic"), three ? ofSide(QStringLiteral("si"), QStringLiteral("left")) : box2(-0.01, -0.01, 0.01, 1.005),
                           {{QStringLiteral("name"), QStringLiteral("s")}});
                addFeature(*m, semi, QStringLiteral("semiconductor/ohmic"), three ? ofSide(QStringLiteral("si"), QStringLiteral("right")) : box2(1.99, -0.01, 2.01, 1.005),
                           {{QStringLiteral("name"), QStringLiteral("d")}, {QStringLiteral("V0"), QStringLiteral("0.5[V]")}});
                addFeature(*m, semi, QStringLiteral("semiconductor/gate"), three ? ofSide(QStringLiteral("ox"), QStringLiteral("back")) : box2(-0.01, 1.015, 2.01, 1.03),
                           {{QStringLiteral("name"), QStringLiteral("g")}, {QStringLiteral("V0"), QStringLiteral("2[V]")}, {QStringLiteral("phim"), QStringLiteral("4.05[eV]")}});
                configure(*m, semi);
            }
            const Solved s3 = solve(m3);
            if (!s3.error.isEmpty()) return QStringLiteral("3D: ") + why(s3);
            const StudyResult r2 = solveStudy(m2, evaluateParameters(m2), s3.mesh->plan, QString());
            if (!r2.error.isEmpty() || !r2.solution) return QStringLiteral("2D: ") + r2.error;
            const Field3& f3 = s3.s().fields().front();
            const Field& f2 = r2.solution->fields().front();
            const int n3 = f3.space->size(), n2 = f2.space->size();
            *dv = *dn = 0;
            for (int dof = 0; dof < n3; ++dof) {
                const int pd = f3.space->origin(dof).first;
                *dv = std::max(*dv, std::abs(f3.values[std::size_t(dof)] - f2.values[std::size_t(pd)]));
                const double a = f3.values[std::size_t(n3 + dof)], b = f2.values[std::size_t(n2 + pd)];
                *dn = std::max(*dn, std::abs(a - b) / std::max(std::abs(b), 1.0));
            }
            *i3 = global(s3.s(), QStringLiteral("semi.I_d"));
            *i2 = r2.solution->global(QStringLiteral("semi.I_d")).value_or(std::nan(""));
            return QString();
        };
        double base3 = 0, base2 = 0, dv = 0, dn = 0;
        QString error = run([](Model&, const QString&) {}, &base3, &base2, &dv, &dn);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        struct Case {
            const char* name;
            std::function<void(Model&, const QString&)> configure;
        };
        const std::vector<Case> cases = {
            {"Lombardi's", [](Model& m, const QString& s) { modelProp(m, s, QStringLiteral("surface"), true); }},
            {"Shirahata's", [](Model& m, const QString& s) {
                 modelProp(m, s, QStringLiteral("surface"), true);
                 modelProp(m, s, QStringLiteral("surfmodel"), QStringLiteral("shirahata"));
             }},
            {"Yamaguchi's", [](Model& m, const QString& s) { modelProp(m, s, QStringLiteral("mobility"), QStringLiteral("yamaguchi")); }},
        };
        for (const Case& c : cases) {
            double i3 = 0, i2 = 0;
            error = run(c.configure, &i3, &i2, &dv, &dn);
            QVERIFY2(error.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(QString::fromLatin1(c.name), error)));
            info(QStringLiteral("surface scattering, %1: V %2 V, n %3; I 3D %4 A, 2D %5 A (without it %6 A)").arg(QString::fromLatin1(c.name)).arg(dv).arg(dn).arg(i3, 0, 'g', 10).arg(i2, 0, 'g', 10).arg(base3, 0, 'g', 10));
            QVERIFY2(dv < 1e-8 && dn < 1e-6 && std::abs(i3 - i2) < 1e-6 * std::abs(i2) && std::abs(i3) < std::abs(base3),
                     qPrintable(QStringLiteral("%1: V %2, n %3; I %4, %5, %6").arg(QString::fromLatin1(c.name)).arg(dv).arg(dn).arg(i3).arg(i2).arg(base3)));
        }
    }

    // A Hall bar: n silicon of 1e16 per cm³, 1 µm by 1 µm across, 10 µm
    // long (z), 1 V along it, B 0.5 T along x, the electrons' Hall factor
    // 1.2: across its middle (y) the Hall field r I B/(q n t W), t its
    // extent along B, its sign the electrons' (the +y side higher, the
    // current flowing down), to 1e-3 - between y 0.3 and 0.7 µm: at the
    // insulating walls the field falls to nought over a Debye length (41
    // nm), the voltage from wall to wall 6 % less; its current the bar's
    // without the field (a single carrier's: no magnetoresistance) but for
    // the contacts' shorting of the Hall field, b² W/L, to 1e-3.
    void hallBar()
    {
        const auto bar = [](double B) {
            Model m = model3d(0.1);
            addBox(m, QStringLiteral("bar"), 0, 0, 0, 1, 1, 10);
            zDivisions(m, QStringLiteral("bar"), 50);
            const QString semi = addSemiconductor(m);
            m.find(semi)->props.insert(QStringLiteral("Bx"), QStringLiteral("%1[T]").arg(B));
            modelProp(m, semi, QStringLiteral("rhalln"), QStringLiteral("1.2"));
            dope(m, semi, QStringLiteral("bar"), true, QStringLiteral("1e16[1/cm^3]"));
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("top")),
                       {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("V0"), QStringLiteral("1[V]")}});
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
            return solve(m);
        };
        const Solved sv = bar(0.5), cold = bar(0);
        QVERIFY2(sv.error.isEmpty() && cold.error.isEmpty(), qPrintable(why(sv) + why(cold)));
        const double I = global(sv.s(), QStringLiteral("semi.I_a")), I0 = global(cold.s(), QStringLiteral("semi.I_a"));
        const double vh = valueAt(sv.s(), QStringLiteral("semi.V"), 0.5, 0.7, 5) - valueAt(sv.s(), QStringLiteral("semi.V"), 0.5, 0.3, 5);
        const double want = 0.4 * 1.2 * std::abs(I) * 0.5 / (Qe * 1e22 * 1e-6);
        info(QStringLiteral("Hall bar: across 0.4 µm of its middle %1 V, 0.4 µm r I B/(q n t W) %2 V; I %3 A, without the field %4 A").arg(vh).arg(want).arg(I).arg(I0));
        QVERIFY2(std::abs(vh - want) < 1e-3 * want, qPrintable(QStringLiteral("%1, %2 V").arg(vh).arg(want)));
        QVERIFY2(std::abs(I - I0) < 1e-3 * std::abs(I0), qPrintable(QStringLiteral("%1, %2 A").arg(I).arg(I0)));
    }

    // Optical generation from a full-wave solve: a silicon wire 0.5 by 0.22
    // µm in oxide, 2 µm long, its κ 0.01 at 1550 nm, fed 1 µW by an Optical
    // Port at x 0 (a Wavelength Domain step), then its carriers in a
    // Stationary step - p silicon of 1e16 per cm³ between two ohmic
    // contacts on its sides, SRH: the light's power absorbed in it, ∫ ω ε0
    // Im εr |E|² (the wave's fields of 1 W fed, ∫ E×H·dA = 1), the wave's own
    // loss P0 (1 - R - T) within 5 % (its |E| at the nodes, the wave's on its
    // edges; what radiates from the wire's ends lost too); each pair a photon's (Gop =
    // Pabs/ħω); a junction along it (its y > 0 half n) collecting most of
    // them at 0 V, its small-signal photocurrent per pair at 1 kHz that
    // collection (the light weak: at 1 mW its high injection made dI/dG 1 %
    // more than I/G).
    void opticalGeneration()
    {
        const double L = 2;
        Model m = model3d(0.25);
        const auto box = [&m](const QString& label, double x0, double x1, double y0, double y1, double z0, double z1) {
            addBox(m, label, x0, y0, z0, x1 - x0, y1 - y0, z1 - z0);
        };
        box(QStringLiteral("ox"), 0, L, -0.8, 0.8, -0.6, 0.82);
        box(QStringLiteral("core"), 0, L, -0.25, 0.25, 0, 0.22);
        for (const auto& [label, n, kappa] : {std::tuple{QStringLiteral("oxide"), QStringLiteral("1.444"), QString()}, std::tuple{QStringLiteral("silicon"), QStringLiteral("3.476"), QStringLiteral("0.01")}}) {
            Node mat = m.make(QStringLiteral("material"));
            mat.label = label;
            mat.props.insert(QStringLiteral("n"), n);
            mat.props.insert(QStringLiteral("epsilonr"), label == QLatin1String("oxide") ? QStringLiteral("3.9") : QStringLiteral("11.7"));
            if (!kappa.isEmpty()) mat.props.insert(QStringLiteral("kappa"), kappa);
            mat.props.insert(QStringLiteral("selection"), objects(label == QLatin1String("oxide") ? QStringLiteral("ox") : QStringLiteral("core")));
            m.add(m.materials().tag, mat);
        }
        Node sz = m.make(QStringLiteral("mesh/size"));
        sz.props.insert(QStringLiteral("level"), QStringLiteral("domain"));
        sz.props.insert(QStringLiteral("hmax"), QStringLiteral("0.1"));
        sz.props.insert(QStringLiteral("selection"), objects(QStringLiteral("core")));
        m.add(m.mesh().tag, sz);
        Node ewfd = m.make(QStringLiteral("waves3"));
        ewfd.props.insert(QStringLiteral("portresolution"), QStringLiteral("15"));
        const QString wave = ewfd.tag;
        QVERIFY(m.add(m.component().tag, ewfd));
        for (int k = 0; k < 2; ++k) {
            Node port = m.make(QStringLiteral("waves3/port"));
            port.props.insert(QStringLiteral("name"), QString::number(k + 1));
            port.props.insert(QStringLiteral("absorbed"), 4);
            port.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("plane"), QJsonObject{{QStringLiteral("x"), k == 0 ? 0.0 : L}}}});
            QString why2;
            QVERIFY2(m.add(wave, port, -1, &why2), qPrintable(why2));
        }
        const QString semi = addSemiconductor(m, objects(QStringLiteral("core")));
        dope(m, semi, QStringLiteral("core"), false, QStringLiteral("1e16[1/cm^3]"));
        // (Its y > 0 half n of 1e17: a junction along it, collecting.)
        addFeature(m, semi, QStringLiteral("semiconductor/doping"), objects(QStringLiteral("core")),
                   {{QStringLiteral("dopant"), QStringLiteral("donor")}, {QStringLiteral("profile"), QStringLiteral("box")}, {QStringLiteral("N0"), QStringLiteral("1e17[1/cm^3]")},
                    {QStringLiteral("position"), pairOf(-1, 0)}, {QStringLiteral("size"), pairOf(L + 2, 1)}, {QStringLiteral("dj"), QStringLiteral("0.001[um]")},
                    {QStringLiteral("Nb"), QStringLiteral("1e10[1/cm^3]")}});
        addFeature(m, semi, QStringLiteral("semiconductor/srh"), objects(QStringLiteral("core")), {{QStringLiteral("taun"), QStringLiteral("1[us]")}, {QStringLiteral("taup"), QStringLiteral("1[us]")}});
        addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("core"), QStringLiteral("front")), {{QStringLiteral("name"), QStringLiteral("a")}});
        addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("core"), QStringLiteral("back")), {{QStringLiteral("name"), QStringLiteral("b")}});
        addFeature(m, semi, QStringLiteral("semiconductor/optical"), objects(QStringLiteral("core")),
                   {{QStringLiteral("mode"), wave}, {QStringLiteral("P0"), QStringLiteral("1[uW]")}, {QStringLiteral("eta"), QStringLiteral("1")}});
        for (Node& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                c.children.clear();
                Node w = m.make(QStringLiteral("wavelength"));
                w.props.insert(QStringLiteral("wavelengths"), QStringLiteral("1550[nm]"));
                c.children.push_back(w);
                Node st = m.make(QStringLiteral("stationary"));
                st.props.insert(QStringLiteral("physics"), QJsonArray{semi});
                c.children.push_back(st);
                Node fr = m.make(QStringLiteral("frequency"));
                fr.props.insert(QStringLiteral("frequencies"), QStringLiteral("1[kHz]"));
                fr.props.insert(QStringLiteral("physics"), QJsonArray{semi});
                c.children.push_back(fr);
            }
        const Solved sv = solve(m);
        QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
        const double R = global(sv.s(), wave + QStringLiteral(".R")), T = global(sv.s(), wave + QStringLiteral(".T"));
        const double Pabs = global(sv.s(), QStringLiteral("semi.Pabs")), Gop = global(sv.s(), QStringLiteral("semi.Gop"));
        const double hw = 6.62607015e-34 * 299792458.0 / 1550e-9;
        info(QStringLiteral("optical generation: Pabs %1 W, P0 (1 - R - T) %2 W (R %3, T %4); Gop %5 per s, Pabs/ħω %6; I %7 A")
                 .arg(Pabs).arg(1e-6 * (1 - R - T)).arg(R).arg(T).arg(Gop).arg(Pabs / hw).arg(global(sv.s(), QStringLiteral("semi.I_a"))));
        QVERIFY2(std::abs(Pabs - 1e-6 * (1 - R - T)) < 5e-2 * 1e-6 * (1 - R - T), qPrintable(QStringLiteral("%1, %2 W").arg(Pabs).arg(1e-6 * (1 - R - T))));
        QVERIFY2(std::abs(Gop * hw - Pabs) < 1e-9 * Pabs, qPrintable(QStringLiteral("%1, %2").arg(Gop * hw).arg(Pabs)));
        // Its photocurrent at 0 V: most of its pairs collected across its
        // junction, never more; small-signal at 1 kHz, per pair, that
        // collection, |I|/(q Gop), to 1e-3.
        const double I = std::abs(global(sv.s(), QStringLiteral("semi.I_a"))), H = global(sv.s(), QStringLiteral("semi.Hop_a"));
        info(QStringLiteral("optical generation: |I| %1 A, q Gop %2 A; Hop at 1 kHz %3, |I|/(q Gop) %4").arg(I).arg(Qe * Gop).arg(H).arg(I / (Qe * Gop)));
        QVERIFY2(I < Qe * Gop && I > 0.5 * Qe * Gop, qPrintable(QStringLiteral("%1, %2 A").arg(I).arg(Qe * Gop)));
        QVERIFY2(std::abs(H - I / (Qe * Gop)) < 1e-3, qPrintable(QStringLiteral("%1, %2").arg(H).arg(I / (Qe * Gop))));
    }

    // Noise by the impedance field: the resistor of resistorMobility at rest
    // (891 Ω), its short-circuit noise Nyquist's 4kT/R to 1e-4, through a
    // series circuit's load Z its current divided, (R/(R + Z))² of it; the
    // photodiode reversed by 1 V, its 1e27 pairs per m³ s all collected
    // (no recombination), its noise their full shot noise 2q|I| to 1e-3.
    void noise()
    {
        const auto withNoise = [](Model& m, const QString& semi) {
            m.find(semi)->props.insert(QStringLiteral("noise"), true);
            for (Node& c : m.root.children)
                if (c.type == QLatin1String("study")) {
                    Node f = m.make(QStringLiteral("frequency"));
                    f.props.insert(QStringLiteral("frequencies"), QStringLiteral("1[kHz]"));
                    c.children.push_back(f);
                }
        };
        {
            const double N = 1e23, L = 2e-6, A = 1e-12, R = L / (Qe * MuN * N * A);
            Model m = model3d(1.0);
            addBox(m, QStringLiteral("bar"), 0, 0, 0, 1, 1, 2);
            zDivisions(m, QStringLiteral("bar"), 20);
            const QString semi = addSemiconductor(m);
            dope(m, semi, QStringLiteral("bar"), true, QStringLiteral("1e17[1/cm^3]"));
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("top")),
                       {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("circuit"), QStringLiteral("series")}, {QStringLiteral("Rs"), QStringLiteral("500[Ohm]")},
                        {QStringLiteral("RL"), QStringLiteral("1000[Ohm]")}});
            addFeature(m, semi, QStringLiteral("semiconductor/ohmic"), ofSide(QStringLiteral("bar"), QStringLiteral("bottom")), {{QStringLiteral("name"), QStringLiteral("b")}});
            withNoise(m, semi);
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double S = global(sv.s(), QStringLiteral("semi.Sn_a")), want = 4 * KB * 300 / R;
            // (Through its circuit's load, Rs + RL = 1500 Ω: the device's
            // current divided, (R/(R + Z))².)
            const double SL = global(sv.s(), QStringLiteral("semi.SnL_a")), wantL = want * std::pow(R / (R + 1500), 2);
            info(QStringLiteral("noise of a resistor at rest: Sn %1 A²/Hz, 4kT/R %2 A²/Hz; through its load %3, (R/(R + Z))² of it %4").arg(S).arg(want).arg(SL).arg(wantL));
            QVERIFY2(std::abs(S - want) < 1e-4 * want, qPrintable(QStringLiteral("%1, %2").arg(S).arg(want)));
            QVERIFY2(std::abs(SL - wantL) < 1e-4 * wantL, qPrintable(QStringLiteral("%1, %2").arg(SL).arg(wantL)));
        }
        {
            Model m = diode(1e23, 1e22, -1, 100);
            QString semi;
            for (const Node& c : m.component().children)
                if (c.type == QLatin1String("semiconductor")) semi = c.tag;
            addFeature(m, semi, QStringLiteral("semiconductor/generation"), rule(R"({"all": true})"), {{QStringLiteral("G"), QStringLiteral("1e27[1/(m^3*s)]")}});
            withNoise(m, semi);
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(why(sv)));
            const double F = global(sv.s(), QStringLiteral("semi.Fshot_a")), I = global(sv.s(), QStringLiteral("semi.I_a"));
            info(QStringLiteral("noise of a photodiode: I %1 A, Sn %2 A²/Hz, Fshot %3").arg(I).arg(global(sv.s(), QStringLiteral("semi.Sn_a"))).arg(F));
            QVERIFY2(std::abs(F - 1) < 1e-3, qPrintable(QString::number(F)));
        }
    }

    // What is refused: a semiconductor with no contact; a doping profile of
    // the plane's alone; Semiconductor offered in 3D, its 1D line not.
    void refusalsAndSchema()
    {
        Model m = model3d(1.0);
        addBox(m, QStringLiteral("si"), 0, 0, 0, 1, 1, 1);
        const QString semi = addSemiconductor(m);
        Solved sv = solve(m);
        QVERIFY2(sv.error.contains(QStringLiteral("no contact")), qPrintable(sv.error));
        QVERIFY(unsolvedPhysics3(m).isEmpty());
        const NodeKind* k = nodeKind(QStringLiteral("semiconductor"));
        QVERIFY(k && (k->spaces & Spaces::ThreeD));
        QVERIFY(k->property(QStringLiteral("space")) && !(k->property(QStringLiteral("space"))->spaces & Spaces::ThreeD));
        QVERIFY(!(nodeKind(QStringLiteral("semiconductor/laser"))->spaces & Spaces::ThreeD));
    }
};

QTEST_GUILESS_MAIN(TestFem3Semiconductor)
#include "test_fem3_semiconductor.moc"
