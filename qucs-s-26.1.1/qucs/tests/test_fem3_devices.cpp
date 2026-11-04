/*
 * test_fem3_devices.cpp - decision 8's devices in 3D against what is
 *                         published: a photodiode's interdigitated
 *                         fingers' capacitance (Lim and Moore's conformal
 *                         map, as Soole and Schumacher give it for MSM
 *                         photodetectors); a VCSEL array's thermal
 *                         resistance and crosstalk (the disc heated on a
 *                         half-space of Carslaw and Jaeger, its substrate's
 *                         bottom and walls by images); a TSV's resistance
 *                         (Katti et al.'s) and thermal resistances; a heater
 *                         over a ring (Manganelli et al.'s double racetrack:
 *                         its power per free spectral range); the device
 *                         examples
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_solver.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QtTest>

#include <cmath>

using namespace qucs_s::fem;

namespace {

constexpr double Eps0 = 8.8541878188e-12;

/// Whether this build is one whose solves are many times slower - a
/// sanitized one, as CI's Linux job is (QUCS_TEST_SLOW: its path in any).
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

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
}

QJsonObject objectsOf(const QStringList& names, const QString& side = {})
{
    QJsonArray of;
    for (const QString& n : names) of.append(n);
    QJsonObject o{{QStringLiteral("of"), of}};
    if (!side.isEmpty()) o.insert(QStringLiteral("side"), side);
    return o;
}

QJsonObject only(const QStringList& names)
{
    QJsonArray of;
    for (const QString& n : names) of.append(n);
    return QJsonObject{{QStringLiteral("only"), of}};
}

/// A 3D model in \a unit, its mesh's largest size \a hmax and growth.
Model model3d(const QString& unit, double hmax, double growth)
{
    Model m = Model::blank3d();
    m.component().props.insert(QStringLiteral("unit"), unit);
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QString::number(hmax, 'g', 17));
    m.mesh().props.insert(QStringLiteral("growth"), QString::number(growth, 'g', 17));
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

void addCylinder(Model& m, const QString& label, double x, double y, double r, double z, double h)
{
    Node n = m.make(QStringLiteral("cylinder"));
    n.label = label;
    n.props.insert(QStringLiteral("center"), pairOf(x, y));
    n.props.insert(QStringLiteral("radius"), QString::number(r, 'g', 17));
    n.props.insert(QStringLiteral("z"), QString::number(z, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    m.add(m.geometry().tag, n);
}

/// A physics of \a order, its domain feature the user's \a value (k, εr, σ).
QString addPhysics(Model& m, const QString& type, int order, const QString& value)
{
    Node p = m.make(type);
    p.props.insert(QStringLiteral("order"), QString::number(order));
    const QString tag = p.tag;
    m.add(m.component().tag, p);
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

/// A mesh's Size of \a hmax on the \a level's of \a selection.
void addSize(Model& m, const QString& level, const QJsonObject& selection, double hmax)
{
    Node s = m.make(QStringLiteral("mesh/size"));
    s.props.insert(QStringLiteral("level"), level);
    s.props.insert(QStringLiteral("selection"), selection);
    s.props.insert(QStringLiteral("hmax"), QString::number(hmax, 'g', 17));
    QString why;
    QVERIFY2(m.add(m.mesh().tag, s, -1, &why) != nullptr, qPrintable(why));
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

double global(const Solution3& s, const QString& name)
{
    return s.global(name).value_or(std::nan(""));
}

/// The complete elliptic integrals K and E of modulus \a k (0 ≤ k < 1), by
/// the arithmetic-geometric mean.
void elliptic(double k, double* K, double* E)
{
    double a = 1, b = std::sqrt(1 - k * k), sum = k * k / 2, power = 0.5;
    for (int i = 0; i < 60; ++i) {
        const double c = (a - b) / 2;
        power *= 2;
        sum += power * c * c;
        const double next = (a + b) / 2;
        b = std::sqrt(a * b);
        a = next;
        if (std::abs(c) < 1e-17) break;
    }
    *K = M_PI / (2 * a);
    if (E != nullptr) *E = *K * (1 - sum);
}

double ellipticK(double k)
{
    double K = 0;
    elliptic(k, &K, nullptr);
    return K;
}

/// The capacitance per unit length of one gap of an endless array of
/// coplanar fingers \a w wide at period \a p, alternately charged, on a
/// half-space of \a epsr under air: ε0 (1 + εr) K(k)/K(k'), k =
/// tan²(πw/4p) - C. Lim and R. Moore, "Properties of alternately charged
/// coplanar parallel strips by conformal mappings", IEEE Trans. Electron
/// Devices 15, 173 (1968), as J. B. D. Soole and H. Schumacher give it for
/// MSM photodetectors (IEEE J. Quantum Electron. 27, 737, 1991).
double fingerCapacitance(double w, double p, double epsr)
{
    const double k = std::pow(std::tan(M_PI * w / (4 * p)), 2);
    return Eps0 * (1 + epsr) * ellipticK(k) / ellipticK(std::sqrt(1 - k * k));
}

/// An MSM photodiode's fingers (µm, 1 wide at a period of 2) on GaAs under
/// air, one cell of the endless array: from the middle of a finger of one
/// comb to the middle of the next of the other, its walls there the
/// array's symmetry, each half finger 1 µm along y; the electrodes no
/// thicker than their faces (a thin box of air over each draws its outline
/// on the GaAs). Its capacitance per unit length (F/m), by order \a order
/// with \a hedge at the fingers' edges and 2.5 times that elsewhere.
double fingerCell(int order, double hedge, QString* error)
{
    const double p = 2, w = 1, H = 4, t = 0.25, length = 1;
    Model m = model3d(QStringLiteral("um"), 2.5 * hedge, 1.25);
    addBox(m, QStringLiteral("substrate"), 0, 0, 0, p, length, H);
    addBox(m, QStringLiteral("air"), 0, 0, H, p, length, H);
    addBox(m, QStringLiteral("fingerA"), 0, 0, H, w / 2, length, t);
    addBox(m, QStringLiteral("fingerB"), p - w / 2, 0, H, w / 2, length, t);
    const QString es = addPhysics(m, QStringLiteral("electrostatics"), order, QStringLiteral("1"));
    addFeature(m, es, QStringLiteral("electrostatics/charge"), only({QStringLiteral("substrate")}),
               QJsonObject{{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("epsilonr"), QStringLiteral("12.9")}});
    addFeature(m, es, QStringLiteral("electrostatics/terminal"), objectsOf({QStringLiteral("fingerA")}, QStringLiteral("bottom")),
               QJsonObject{{QStringLiteral("name"), QStringLiteral("1")}});
    addFeature(m, es, QStringLiteral("electrostatics/ground"), objectsOf({QStringLiteral("fingerB")}, QStringLiteral("bottom")));
    addSize(m, QStringLiteral("edge"), objectsOf({QStringLiteral("fingerA"), QStringLiteral("fingerB")}), hedge);
    const Solved sv = solve(m);
    if (!sv.error.isEmpty()) {
        *error = sv.error;
        return std::nan("");
    }
    return global(sv.s(), QStringLiteral("es.C11")) / (length * 1e-6);
}

/// Gauss-Legendre's \a n points and weights on [0, 1].
void gaussLegendre(int n, std::vector<double>* x, std::vector<double>* w)
{
    x->assign(std::size_t(n), 0);
    w->assign(std::size_t(n), 0);
    for (int i = 0; i < n; ++i) {
        double t = std::cos(M_PI * (i + 0.75) / (n + 0.5)), dp = 0;
        for (int it = 0; it < 100; ++it) {
            double p0 = 1, p1 = t;
            for (int k = 2; k <= n; ++k) {
                const double p2 = ((2 * k - 1) * t * p1 - (k - 1) * p0) / k;
                p0 = p1;
                p1 = p2;
            }
            dp = n * (t * p1 - p0) / (t * t - 1);
            const double dt = p1 / dp;
            t -= dt;
            if (std::abs(dt) < 1e-15) break;
        }
        (*x)[std::size_t(i)] = (1 - t) / 2;
        (*w)[std::size_t(i)] = 1 / ((1 - t * t) * dp * dp);
    }
}

/// The mean of \a f over a disc of radius \a a about (cx, cy): Gauss's
/// points in r, even ones about.
double discMean(const std::function<double(double, double)>& f, double cx, double cy, double a)
{
    std::vector<double> x, w;
    gaussLegendre(16, &x, &w);
    const int around = 32;
    double sum = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
        for (int j = 0; j < around; ++j) {
            const double r = a * x[i], th = 2 * M_PI * (j + 0.5) / around;
            sum += w[i] * x[i] * f(cx + r * std::cos(th), cy + r * std::sin(th));
        }
    return 2 * sum / around;
}

/// A disc of radius \a a heated by \a q (W/m²) on a half-space of \a k, the
/// rest of its face insulated: the rise at \a r on that face - H. S.
/// Carslaw and J. C. Jaeger, Conduction of Heat in Solids (2nd ed., 1959),
/// §8.2: (2qa/πk) E(r/a) within it, (2qr/πk)[E(a/r) - (1 - a²/r²) K(a/r)]
/// beyond.
double discRise(double r, double a, double q, double k)
{
    double K = 0, E = 0;
    if (r < a) {
        elliptic(r / a, &K, &E);
        return 2 * q * a / (M_PI * k) * E;
    }
    if (r == a) return 2 * q * a / (M_PI * k);
    elliptic(a / r, &K, &E);
    return 2 * q * r / (M_PI * k) * (E - (1 - a * a / (r * r)) * K);
}

/// Σ (-1)^n / √(ρ² + (2nL)²) over n ≠ 0, and the n = 0 term too where
/// \a withSource: a source on a face insulated, its images in the face L
/// below held - the means of the last two partial sums, an alternating
/// series'.
double imageColumn(double rho, double L, bool withSource)
{
    double partial = 0, last = 0;
    for (int n = 1; n <= 4000; ++n) {
        last = partial;
        partial += (n % 2 ? -2.0 : 2.0) / std::sqrt(rho * rho + 4.0 * n * n * L * L);
    }
    return (withSource ? 1 / rho : 0) + (partial + last) / 2;
}

struct Disc {
    double x, y, power;   // m, m, W
};

/// The rise at (x, y) on the top of a substrate W × W and L deep (m) of
/// conductivity \a k, its bottom held and the rest insulated, of discs of
/// radius \a a heated on its top: each Carslaw and Jaeger's, then its
/// images - in the bottom, every other one negative, and in the walls -,
/// those far enough to be points.
double substrateRise(const std::vector<Disc>& discs, double a, double k, double W, double L, double x, double y)
{
    double rise = 0;
    for (const Disc& d : discs) {
        if (d.power == 0) continue;
        const double q = d.power / (M_PI * a * a), point = d.power / (2 * M_PI * k);
        for (int i = -3; i <= 3; ++i)
            for (int sx : {1, -1})
                for (int j = -3; j <= 3; ++j)
                    for (int sy : {1, -1}) {
                        const double X = 2 * i * W + sx * d.x, Y = 2 * j * W + sy * d.y, rho = std::hypot(x - X, y - Y);
                        if (i == 0 && j == 0 && sx == 1 && sy == 1) rise += discRise(rho, a, q, k) + point * imageColumn(rho, L, false);
                        else rise += point * imageColumn(rho, L, true);
                    }
    }
    return rise;
}

/// A VCSEL array on GaAs (µm): 3 x 3 discs of radius 5 at a pitch of 25 on
/// the top of a substrate 300 x 300 and 150 deep - each disc 0.5 µm deep in
/// it, so that its top face is its own -, the middle one's top heated by
/// \a power, the bottom held at 300 K, the rest insulated; k 46 W/(m·K).
/// The plan 2 µm in a disc and 1 at its rim, 30 at most.
struct VcselArray {
    static constexpr double W = 300, L = 150, a = 5, pitch = 25, k = 46;
    Solved solved;
    std::vector<Disc> discs;   // m and W, as substrateRise takes them
};

VcselArray vcselModel(int order, double power)
{
    VcselArray v;
    Model m = model3d(QStringLiteral("um"), 30, 1.25);
    addBox(m, QStringLiteral("substrate"), 0, 0, 0, v.W, v.W, v.L);
    QStringList names;
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j) {
            const QString name = QStringLiteral("vcsel%1%2").arg(i + 2).arg(j + 2);
            const double x = v.W / 2 + i * v.pitch, y = v.W / 2 + j * v.pitch;
            addCylinder(m, name, x, y, v.a, v.L - 0.5, 0.5);
            names << name;
            v.discs.push_back({x * 1e-6, y * 1e-6, i == 0 && j == 0 ? power : 0.0});
        }
    const QString ht = addPhysics(m, QStringLiteral("heat"), order, QStringLiteral("%1[W/(m*K)]").arg(v.k));
    addFeature(m, ht, QStringLiteral("heat/temperature"), objectsOf({QStringLiteral("substrate")}, QStringLiteral("bottom")),
               QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
    addFeature(m, ht, QStringLiteral("heat/flux"), objectsOf({QStringLiteral("vcsel22")}, QStringLiteral("top")),
               QJsonObject{{QStringLiteral("kind"), QStringLiteral("general")},
                           {QStringLiteral("q0"), QStringLiteral("%1[W/m^2]").arg(power / (M_PI * v.a * v.a * 1e-12), 0, 'g', 17)}});
    addSize(m, QStringLiteral("domain"), only(names), 2);
    addSize(m, QStringLiteral("edge"), objectsOf(names), 1);
    v.solved = solve(m);
    return v;
}

/// The temperature of solution \a s at (x, y) (µm) on its top, \a top µm up.
double topValue(const Solution3& s, double x, double y, double top)
{
    PointEvaluator3 ev(s);
    if (!ev.at(Vec3{x * 1e-6, y * 1e-6, top * 1e-6 * (1 - 1e-12)})) return std::nan("");
    return ev.eval(Expression::compile(QStringLiteral("ht.T"), s.pointScope()));
}

/// A racetrack's band in an extrude (µm): a stadium of radius \a R about
/// (cx, cy), its straights \a S long along y, \a w wide, \a z0 up for \a h.
void addStadiumBand(Model& m, const QString& name, double cx, double cy, double R, double S, double w, double z0, double h)
{
    Node e = m.make(QStringLiteral("extrude"));
    e.label = name;
    e.props.insert(QStringLiteral("z"), QString::number(z0, 'g', 17));
    e.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    const auto rect = [&m](const QString& label, double x, double y, double wx, double wy) {
        Node r = m.make(QStringLiteral("rectangle"));
        r.label = label;
        r.props.insert(QStringLiteral("base"), QStringLiteral("center"));
        r.props.insert(QStringLiteral("position"), pairOf(x, y));
        r.props.insert(QStringLiteral("size"), pairOf(wx, wy));
        return r;
    };
    const auto circle = [&m](const QString& label, double x, double y, double r) {
        Node c = m.make(QStringLiteral("circle"));
        c.label = label;
        c.props.insert(QStringLiteral("center"), pairOf(x, y));
        c.props.insert(QStringLiteral("radius"), QString::number(r, 'g', 17));
        return c;
    };
    const auto join = [&m](const QString& type, const QString& label, const QStringList& input, const QStringList& tools = {}) {
        Node u = m.make(type);
        u.label = label;
        QJsonArray in, to;
        for (const QString& i : input) in.append(i);
        for (const QString& t : tools) to.append(t);
        u.props.insert(QStringLiteral("input"), in);
        if (!tools.isEmpty()) u.props.insert(QStringLiteral("tools"), to);
        return u;
    };
    const double ro = R + w / 2, ri = R - w / 2;
    e.children.push_back(rect(name + QStringLiteral("_or"), cx, cy, 2 * ro, S));
    e.children.push_back(circle(name + QStringLiteral("_ot"), cx, cy + S / 2, ro));
    e.children.push_back(circle(name + QStringLiteral("_ob"), cx, cy - S / 2, ro));
    e.children.push_back(join(QStringLiteral("union"), name + QStringLiteral("_o"), {name + QStringLiteral("_or"), name + QStringLiteral("_ot"), name + QStringLiteral("_ob")}));
    e.children.push_back(rect(name + QStringLiteral("_ir"), cx, cy, 2 * ri, S));
    e.children.push_back(circle(name + QStringLiteral("_it"), cx, cy + S / 2, ri));
    e.children.push_back(circle(name + QStringLiteral("_ib"), cx, cy - S / 2, ri));
    e.children.push_back(join(QStringLiteral("union"), name + QStringLiteral("_i"), {name + QStringLiteral("_ir"), name + QStringLiteral("_it"), name + QStringLiteral("_ib")}));
    e.children.push_back(join(QStringLiteral("difference"), name, {name + QStringLiteral("_o")}, {name + QStringLiteral("_i")}));
    QString why;
    if (m.add(m.geometry().tag, e, -1, &why) == nullptr) qWarning("%s", qPrintable(why));
}

/// The mean of \a text over the domains of solution \a s owned by \a object.
double domainMean(const Solution3& s, const QString& object, const QString& text)
{
    const Topology3& t = s.topology();
    const int owner = int(t.objectNames.indexOf(object));
    std::vector<char> in(t.domains.size(), 0);
    for (std::size_t d = 0; d < t.domains.size(); ++d) in[d] = t.domains[d].owner == owner ? 1 : 0;
    const Expression e = Expression::compile(text, s.pointScope());
    const Space3& sp = *s.fields().front().space;
    PointEvaluator3 ev(s);
    double sum = 0, volume = 0;
    for (int el = 0; el < sp.elements(); ++el) {
        if (!in[std::size_t(s.mesh().domain[std::size_t(el)])]) continue;
        for (const auto& q : prismRule(4, 3).points) {
            const Space3::Map mp = sp.map(el, q[0], q[1], q[2]);
            ev.at(el, q[0], q[1], q[2], mp.x);
            const double wgt = q[3] * std::abs(mp.det);
            sum += wgt * ev.eval(e);
            volume += wgt;
        }
    }
    return volume > 0 ? sum / volume : std::nan("");
}

/// The group index of a silicon strip \a w x 220 nm in silica at 1550 nm -
/// TE0's, the library's dispersive silicon and silica - by the 2D mode
/// solver, its core meshed at \a hcore µm.
double stripGroupIndex(double w, double hcore, QString* error)
{
    const QByteArray json = QStringLiteral(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "oxide", "base": "center", "size": ["4", "4"]},
                     {"type": "rectangle", "name": "core", "base": "center", "size": ["%1", "0.22"]}],
        "materials": [{"material": "Silicon dioxide", "selection": {"only": ["oxide"]}},
                      {"material": "Silicon", "selection": {"only": ["core"]}}],
        "physics": [{"type": "modes", "tag": "emw"}],
        "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "%2"},
                                              {"type": "size", "level": "domain", "selection": {"only": ["oxide"]}, "hmax": "0.15"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "3"}]}]})json")
                               .arg(w).arg(hcore).toUtf8();
    std::optional<Model> m = Model::fromJson(json, error);
    if (!m) return std::nan("");
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(*m);
    const GeometryBuild g = buildGeometry(*m, *ps);
    if (!g.ok()) {
        *error = QStringList(g.errors.values()).join(QStringLiteral("; "));
        return std::nan("");
    }
    StudyRun r = runStudy(*m, ps, g.topology, nullptr, QString(), {});
    if (!r.ok()) {
        *error = r.error;
        return std::nan("");
    }
    Solution& sol = *r.solutions.back();
    sol.select(0);
    return sol.global(QStringLiteral("emw.ng")).value_or(std::nan(""));
}

/// C. L. Manganelli et al.'s double racetrack (µm): two racetracks \a L
/// long, of 3 µm bends, 410 x 220 nm, 250 nm apart, on 2 µm of buried oxide
/// over a substrate 100 µm deep and 80 across held at 300 K below; over
/// each a heater loop 120 nm thick 600 nm above it (its 434 nm drawn as the
/// waveguide's 410: else the plan holds slivers 12 nm wide all along, 3
/// million prisms where there are 150,000, for 0.1 % in the answer), 320 nm
/// of silica over all, its top insulated; each loop making 1 mW. Silicon
/// 148, silica 1.38, Ti/TiN 29 W/(m·K).
Solved ringHeater(int order, double hedge, double L)
{
    const double R = 3, w = 0.41, S = (L - 2 * M_PI * R) / 2, Hs = 100, W = 80;
    const double zw = Hs + 2, zh = zw + 0.22 + 0.6, ztop = zh + 0.12 + 0.32, dx = (2 * R + w + 0.25) / 2;
    Model m = model3d(QStringLiteral("um"), 8, 1.3);
    addBox(m, QStringLiteral("substrate"), -W / 2, -W / 2, 0, W, W, Hs);
    addBox(m, QStringLiteral("oxide"), -W / 2, -W / 2, Hs, W, W, ztop - Hs);
    addStadiumBand(m, QStringLiteral("wg1"), -dx, 0, R, S, w, zw, 0.22);
    addStadiumBand(m, QStringLiteral("wg2"), dx, 0, R, S, w, zw, 0.22);
    addStadiumBand(m, QStringLiteral("heater1"), -dx, 0, R, S, w, zh, 0.12);
    addStadiumBand(m, QStringLiteral("heater2"), dx, 0, R, S, w, zh, 0.12);
    const QString ht = addPhysics(m, QStringLiteral("heat"), order, QStringLiteral("1.38[W/(m*K)]"));
    addFeature(m, ht, QStringLiteral("heat/solid"), only({QStringLiteral("substrate"), QStringLiteral("wg1"), QStringLiteral("wg2")}),
               QJsonObject{{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("k"), QStringLiteral("148[W/(m*K)]")}});
    addFeature(m, ht, QStringLiteral("heat/solid"), only({QStringLiteral("heater1"), QStringLiteral("heater2")}),
               QJsonObject{{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("k"), QStringLiteral("29[W/(m*K)]")}});
    for (const char* h : {"heater1", "heater2"})
        addFeature(m, ht, QStringLiteral("heat/source"), only({QLatin1String(h)}),
                   QJsonObject{{QStringLiteral("kind"), QStringLiteral("power")}, {QStringLiteral("P0"), QStringLiteral("1[mW]")}});
    addFeature(m, ht, QStringLiteral("heat/temperature"), objectsOf({QStringLiteral("substrate")}, QStringLiteral("bottom")),
               QJsonObject{{QStringLiteral("T0"), QStringLiteral("300[K]")}});
    addSize(m, QStringLiteral("edge"), objectsOf({QStringLiteral("wg1"), QStringLiteral("wg2"), QStringLiteral("heater1"), QStringLiteral("heater2")}), hedge);
    return solve(m);
}

/// An example of examples/multiphysics3d solved as it is.
Solved example(const QString& name, QString* error)
{
    QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics3d/") + name);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = name + QStringLiteral(" not read");
        return {};
    }
    const std::optional<Model> m = Model::fromJson(f.readAll(), error);
    if (!m) return {};
    Solved sv = solve(*m);
    if (!sv.error.isEmpty()) *error = sv.error;
    return sv;
}

} // namespace

class TestFem3Devices : public QObject
{
    Q_OBJECT

private slots:
    // A photodiode's interdigitated fingers on GaAs, 1 µm wide at a period of
    // 2: the capacitance of a gap of the endless array converges on Lim and
    // Moore's. The electrodes have no thickness, so their edges are singular
    // (the potential as r^½) and the error falls as h, not h² - from above,
    // as a Galerkin solve's energy does, at order 1 and 2 alike: the mesh
    // halved, the error halved, and the two finest extrapolated at that rate
    // within 0.3 %. (In µm, where the system's entries are ε0 times some 1e-6:
    // multigrid took its right-hand side for nought, and the capacitance came
    // out 7 to 21 times too large, the more as the mesh was refined.)
    void photodiodeFingers()
    {
        const double formula = fingerCapacitance(1, 2, 12.9);
        // (At w = s it is ε0 (1 + εr) / 2: K(k) = K(k') there.)
        QVERIFY(std::abs(formula - Eps0 * 13.9 / 2) < 1e-12 * formula);
        QString why;
        double c1[3];
        const double h[3] = {0.2, 0.1, 0.05};
        for (int i = 0; i < 3; ++i) {
            c1[i] = fingerCell(1, h[i], &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
        }
        const double ratio = (c1[0] - c1[1]) / (c1[1] - c1[2]), extrapolated = 2 * c1[2] - c1[1];
        const QString said = QStringLiteral("order 1: %1, %2, %3 F/m (its differences' ratio %4, extrapolated %5), the formula %6")
                                 .arg(c1[0]).arg(c1[1]).arg(c1[2]).arg(ratio).arg(extrapolated).arg(formula);
        qInfo("%s", qPrintable(said));
        QVERIFY2(c1[0] > c1[1] && c1[1] > c1[2] && c1[2] > formula && c1[2] < 1.04 * formula, qPrintable(said));
        QVERIFY2(ratio > 1.5 && ratio < 2.5, qPrintable(said));
        QVERIFY2(std::abs(extrapolated - formula) < 3e-3 * formula, qPrintable(said));
        // (Order 2 on the finer mesh too where solves are quick.)
        const int order2 = slowBuild() ? 1 : 2;
        double c2[2];
        for (int i = 0; i < order2; ++i) {
            c2[i] = fingerCell(2, h[i], &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
        }
        const QString said2 = QStringLiteral("order 2: %1 F/m%2").arg(c2[0]).arg(order2 > 1 ? QStringLiteral(", %1").arg(c2[1]) : QString());
        qInfo("%s", qPrintable(said2));
        QVERIFY2(c2[0] > formula && c2[0] < 1.025 * formula && c2[0] < c1[1], qPrintable(said2));
        if (order2 > 1) {
            QVERIFY2(c2[0] > c2[1] && c2[1] > formula && c2[1] < 1.012 * formula, qPrintable(said2));
            QVERIFY2(std::abs(2 * c2[1] - c2[0] - formula) < 3e-3 * formula, qPrintable(said2));
        }
    }

    // A VCSEL array's thermal resistance and crosstalk: the middle one of 3 x 3
    // heated, each a uniform flux on its disc, on a GaAs substrate held below.
    // Its rise at its middle and its mean over its disc, and its neighbours'
    // at d and d√2 - their cross-resistances, R_AB = ΔT_A / P_B - are Carslaw
    // and Jaeger's disc on a half-space with the substrate's bottom and
    // walls by images: order 2 within 0.05 % (its disc's mean, its rim's
    // kink in it, 0.3 %); order 1 on these stretched prisms short of them by
    // 1 to 3 %. The lone disc on a half-space - P/(πka) at its middle,
    // 8P/(3π²ka) its mean - is said beside.
    void vcselArray()
    {
        const double P = 0.01;
        for (int order : {1, 2}) {
            // (Order 2's 585,000 unknowns some 14 s: not under the sanitizers.)
            if (order == 2 && slowBuild()) continue;
            const VcselArray v = vcselModel(order, P);
            QVERIFY2(v.solved.error.isEmpty(), qPrintable(v.solved.error));
            const Solution3& s = v.solved.s();
            // Multigrid's own: order 2's stretched prisms are no M-matrix, and
            // with an ILU(0) smoother the conjugate gradients stalled (the
            // incomplete Cholesky's finishing them, four minutes on).
            const QString log = s.log.join(QStringLiteral(" | "));
            QVERIFY2(log.contains(QStringLiteral("algebraic multigrid")) && !log.contains(QStringLiteral("did not converge")), qPrintable(log));
            const double W = v.W, L = v.L, a = v.a, k = v.k;
            const auto fe = [&s, L](double x, double y) { return topValue(s, x * 1e6, y * 1e6, L) - 300; };
            const std::vector<Disc>& discs = v.discs;
            const auto exact = [&discs, a, k, W, L](double x, double y) { return substrateRise(discs, a * 1e-6, k, W * 1e-6, L * 1e-6, x, y); };
            const double cx = W / 2 * 1e-6, cy = W / 2 * 1e-6, d = v.pitch * 1e-6;
            struct Check {
                const char* what;
                double got, want, within;
            };
            const double tolerance = order == 2 ? 5e-4 : 2e-2;
            const Check checks[] = {
                {"its middle", fe(cx, cy), exact(cx, cy), tolerance},
                {"its disc's mean", discMean(fe, cx, cy, a * 1e-6), discMean(exact, cx, cy, a * 1e-6), order == 2 ? 3e-3 : 3.5e-2},
                {"a neighbour's middle at d", fe(cx + d, cy), exact(cx + d, cy), tolerance},
                {"a neighbour's middle at d√2", fe(cx + d, cy + d), exact(cx + d, cy + d), tolerance},
            };
            const double halfSpace = P / (M_PI * k * a * 1e-6), halfSpaceMean = 8 * P / (3 * M_PI * M_PI * k * a * 1e-6);
            for (const Check& c : checks) {
                const QString said = QStringLiteral("order %1, %2: %3 K, the discs and images %4 K (%5 %; a lone disc on a half-space: "
                                                    "%6 K at its middle, %7 K its mean)")
                                         .arg(order).arg(QString::fromUtf8(c.what)).arg(c.got, 0, 'f', 5).arg(c.want, 0, 'f', 5)
                                         .arg(100 * (c.got / c.want - 1), 0, 'f', 3).arg(halfSpace, 0, 'f', 4).arg(halfSpaceMean, 0, 'f', 4);
                qInfo("%s", qPrintable(said));
                QVERIFY2(std::abs(c.got - c.want) < c.within * c.want, qPrintable(said));
            }
            // The heat made the flux over its disc's face: order 1's a polygon,
            // 0.7 % short of the circle (and so of the rise too); the series
            // below a lone disc's on a half-space, the bottom drawing heat.
            const double made = global(s, QStringLiteral("ht.P"));
            QVERIFY2(std::abs(made - P) < (order == 2 ? 1e-4 : 1e-2) * P && made <= P * (1 + 1e-9),
                     qPrintable(s.globalText(QStringLiteral("ht.P"))));
            QVERIFY(exact(cx, cy) < halfSpace && discMean(exact, cx, cy, a * 1e-6) < halfSpaceMean);
        }
    }

    // A TSV: a copper via 5 µm across and 25 µm deep in a 0.2 µm oxide liner.
    // Its resistance ρh/(πa²), as G. Katti, M. Stucchi, K. De Meyer and W.
    // Dehaene give it (IEEE Trans. Electron Devices 57, 256, 2010, beside
    // the liner's capacitance test_fem3_solve holds); its copper's thermal
    // resistance h/(kπa²) along it; and its liner's across, ln(b/a)/(2πkh),
    // the via heated within: the axis that and P/(4πk_Cu h) above the held
    // outer wall. Order 2 on the arcs within 0.1 %; order 1's polygon
    // short of the circle, within 5 % and further off.
    void tsv()
    {
        const double a = 2.5, b = 2.7, h = 25, sigma = 5.96e7, kCu = 400, kOx = 1.4, P = 1e-3, T0 = 293.15;
        const double area = M_PI * a * a * 1e-12;
        const double R = h * 1e-6 / (sigma * area), Rth = h * 1e-6 / (kCu * area);
        const double rise = P * (std::log(b / a) / (2 * M_PI * kOx * h * 1e-6) + 1 / (4 * M_PI * kCu * h * 1e-6));
        double got[3][3] = {};
        for (int order : {1, 2}) {
            Model m = model3d(QStringLiteral("um"), 1, 1.3);
            addCylinder(m, QStringLiteral("via"), 0, 0, a, 0, h);
            const QString ec = addPhysics(m, QStringLiteral("currents"), order, QStringLiteral("%1[S/m]").arg(sigma));
            addFeature(m, ec, QStringLiteral("currents/terminal"), objectsOf({QStringLiteral("via")}, QStringLiteral("top")),
                       QJsonObject{{QStringLiteral("name"), QStringLiteral("1")}});
            addFeature(m, ec, QStringLiteral("currents/ground"), objectsOf({QStringLiteral("via")}, QStringLiteral("bottom")));
            const QString ht = addPhysics(m, QStringLiteral("heat"), order, QStringLiteral("%1[W/(m*K)]").arg(kCu));
            addFeature(m, ht, QStringLiteral("heat/flux"), objectsOf({QStringLiteral("via")}, QStringLiteral("top")),
                       QJsonObject{{QStringLiteral("kind"), QStringLiteral("general")},
                                   {QStringLiteral("q0"), QStringLiteral("%1[W/m^2]").arg(P / area, 0, 'g', 17)}});
            addFeature(m, ht, QStringLiteral("heat/temperature"), objectsOf({QStringLiteral("via")}, QStringLiteral("bottom")),
                       QJsonObject{{QStringLiteral("T0"), QStringLiteral("%1[K]").arg(T0, 0, 'g', 17)}});
            const Solved sv = solve(m);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            got[order][0] = global(sv.s(), QStringLiteral("ec.R"));
            got[order][1] = global(sv.s(), QStringLiteral("ht.Rth"));

            Model l = model3d(QStringLiteral("um"), 0.5, 1.3);
            addCylinder(l, QStringLiteral("liner"), 0, 0, b, 0, h);
            addCylinder(l, QStringLiteral("via"), 0, 0, a, 0, h);
            const QString lt = addPhysics(l, QStringLiteral("heat"), order, QStringLiteral("%1[W/(m*K)]").arg(kOx));
            addFeature(l, lt, QStringLiteral("heat/solid"), only({QStringLiteral("via")}),
                       QJsonObject{{QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("k"), QStringLiteral("%1[W/(m*K)]").arg(kCu)}});
            addFeature(l, lt, QStringLiteral("heat/source"), only({QStringLiteral("via")}),
                       QJsonObject{{QStringLiteral("kind"), QStringLiteral("power")}, {QStringLiteral("P0"), QStringLiteral("%1[W]").arg(P)}});
            // The outer wall held: every outer face, then the top and the
            // bottom insulated (the later wins).
            QJsonArray up, down;
            for (double v : {0.0, 0.0, 1.0}) up.append(v);
            for (double v : {0.0, 0.0, -1.0}) down.append(v);
            addFeature(l, lt, QStringLiteral("heat/temperature"), QJsonObject{{QStringLiteral("exterior"), true}},
                       QJsonObject{{QStringLiteral("T0"), QStringLiteral("%1[K]").arg(T0, 0, 'g', 17)}});
            addFeature(l, lt, QStringLiteral("heat/insulation"), QJsonObject{{QStringLiteral("normal"), up}});
            addFeature(l, lt, QStringLiteral("heat/insulation"), QJsonObject{{QStringLiteral("normal"), down}});
            const Solved lv = solve(l);
            QVERIFY2(lv.error.isEmpty(), qPrintable(lv.error));
            got[order][2] = global(lv.s(), QStringLiteral("ht.Tmax")) - T0;
        }
        const double want[3] = {R, Rth, rise};
        const char* what[3] = {"its resistance (Ohm)", "its copper's thermal resistance (K/W)", "its axis's rise over the liner (K)"};
        for (int i = 0; i < 3; ++i) {
            const QString said = QStringLiteral("%1: order 2 %2, order 1 %3, the formula %4").arg(QLatin1String(what[i])).arg(got[2][i]).arg(got[1][i]).arg(want[i]);
            qInfo("%s", qPrintable(said));
            QVERIFY2(std::abs(got[2][i] - want[i]) < 1e-3 * want[i], qPrintable(said));
            QVERIFY2(std::abs(got[1][i] - want[i]) < 5e-2 * want[i] && std::abs(got[1][i] - want[i]) > std::abs(got[2][i] - want[i]), qPrintable(said));
        }
    }





    // The heater over a ring: C. L. Manganelli et al.'s double racetrack
    // (IEEE Photon. Technol. Lett. 30, 1601, 2018) - two coupled racetracks
    // of a 410 x 220 nm strip on 2 µm of buried oxide, each under a Ti/TiN
    // loop 600 nm above it -, whose 3D thermo-electrical simulation took
    // 21.2 mW per free spectral range per ring with dn/dT 1.8e-4 /K, and
    // measurement 20 to 22. One FSR is ΔT = λ/(L dn/dT), whatever the group
    // index (Δλ = λ ΔT dn/dT / n_g, FSR = λ²/(n_g L)); L is c/(n_g FSR) of
    // its 2.4 THz, n_g the strip's TE0 by the 2D mode solver. Both heaters
    // on, each ring's mean rise per mW: within 5 % of 21.2 at orders 1 and
    // 2. (The paper gives no bend radius, leads or substrate: bends of 2.5
    // to 3.5 µm at the same length move it 5 % either way, the substrate's
    // depth and the chip's width 2 %.)
    void heaterOverARing()
    {
        QString why;
        const double ng = stripGroupIndex(0.41, slowBuild() ? 0.03 : 0.015, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        const double L = 2.99792458e8 / (ng * 2.4e12) * 1e6;   // µm
        const double perFsr = 1.55e-6 / (L * 1e-6 * 1.8e-4);   // K
        qInfo("the strip's group index %.4f: the racetrack %.3f um long, an FSR %.1f K", ng, L, perFsr);
        QVERIFY2(ng > 4.3 && ng < 4.45, qPrintable(QString::number(ng)));
        double fsr[3] = {};
        for (int order : {1, 2}) {
            // (Order 2's 600,000 unknowns some 13 s: not under the sanitizers.)
            if (order == 2 && slowBuild()) continue;
            const Solved sv = ringHeater(order, 0.3, L);
            QVERIFY2(sv.error.isEmpty(), qPrintable(sv.error));
            const QString log = sv.s().log.join(QStringLiteral(" | "));
            QVERIFY2(!log.contains(QStringLiteral("did not converge")), qPrintable(log));
            const double one = domainMean(sv.s(), QStringLiteral("wg1"), QStringLiteral("ht.T")) - 300;
            const double two = domainMean(sv.s(), QStringLiteral("wg2"), QStringLiteral("ht.T")) - 300;
            const double loop = global(sv.s(), QStringLiteral("ht.Tmax")) - 300;
            fsr[order] = perFsr / one;
            const QString said = QStringLiteral("order %1: the racetracks %2 and %3 K per mW of each, the loops' peak %4 K: %5 mW per FSR "
                                                "per ring (the paper's simulation 21.2, its measurement 20 to 22)")
                                     .arg(order).arg(one, 0, 'f', 3).arg(two, 0, 'f', 3).arg(loop, 0, 'f', 2).arg(fsr[order], 0, 'f', 2);
            qInfo("%s", qPrintable(said));
            QVERIFY2(std::abs(one - two) < 1e-3 * one, qPrintable(said));
            QVERIFY2(loop > 1.5 * one, qPrintable(said));
            QVERIFY2(std::abs(fsr[order] - 21.2) < 0.05 * 21.2, qPrintable(said));
        }
        if (!slowBuild()) QVERIFY2(std::abs(fsr[1] - fsr[2]) < 0.02 * fsr[2], qPrintable(QStringLiteral("%1, %2").arg(fsr[1]).arg(fsr[2])));
    }

    // The device examples (examples/multiphysics3d) solved as they are, each
    // beside its formula: the ring heater's tuning the paper's (as
    // heaterOverARing), its racetracks alike; the VCSEL array's 90 mW made (its discs' polygons a
    // little short), the middle VCSEL the hottest and its neighbours' heat in
    // its rise - over P/(πka), a lone disc's on a half-space; the MSM
    // photodiode's capacitance above its gaps' share by Lim and Moore, the bus
    // bars, the tips and the outer fingers adding theirs; the TSV's
    // resistance Katti's ρh/(πa²) within 0.1 % (order 2), and its thermal
    // resistance below its copper's alone: the silicon beside it carries
    // some of the heat down.
    void theExamples()
    {
        QString why;
        // (The array's 146,000 prisms, the photodiode's million and the ring
        // heater's 150,000 in Release alone: under the sanitizers the first
        // two ran past QtTest's 300 s here.)
        if (!slowBuild()) {
            const Solved sv = example(QStringLiteral("vcsel_array.qfem3d"), &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
            const Solution3& s = sv.s();
            const double made = global(s, QStringLiteral("ht.P"));
            QVERIFY2(made <= 0.09 * (1 + 1e-9) && made > 0.09 * 0.99, qPrintable(s.globalText(QStringLiteral("ht.P"))));
            const double middle = topValue(s, 150, 150, 150) - 293.15, edge = topValue(s, 175, 150, 150) - 293.15,
                         corner = topValue(s, 175, 175, 150) - 293.15;
            // (A lone VCSEL on a half-space of GaAs at 300 K, k 55 W/(m·K).)
            const double lone = 0.01 / (M_PI * 55 * 5e-6);
            const QString said = QStringLiteral("the VCSELs' rises: %1 K in the middle, %2 K at an edge, %3 K at a corner; alone %4 K")
                                     .arg(middle).arg(edge).arg(corner).arg(lone);
            qInfo("%s", qPrintable(said));
            QVERIFY2(middle > edge && edge > corner && corner > 0.9 * lone && middle > 1.3 * lone && middle < 2 * lone, qPrintable(said));
        }
        if (!slowBuild()) {
            const Solved sv = example(QStringLiteral("msm_photodiode.qfem3d"), &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
            const double c = global(sv.s(), QStringLiteral("es.C11"));
            // (Seven gaps, each 11 µm along the fingers' overlap.)
            const double gaps = 7 * 11e-6 * fingerCapacitance(1, 2, 12.9);
            const QString said = QStringLiteral("the photodiode's capacitance %1 F, its gaps' share %2 F").arg(c).arg(gaps);
            qInfo("%s", qPrintable(said));
            QVERIFY2(c > gaps && c < 1.6 * gaps, qPrintable(said));
        }
        if (!slowBuild()) {
            const Solved sv = example(QStringLiteral("ring_heater.qfem3d"), &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
            const Solution3& s = sv.s();
            QVERIFY2(std::abs(global(s, QStringLiteral("ht.P")) - 2e-3) < 1e-9, qPrintable(s.globalText(QStringLiteral("ht.P"))));
            const double one = domainMean(s, QStringLiteral("ring1_band"), QStringLiteral("ht.T")) - 293.15;
            const double two = domainMean(s, QStringLiteral("ring2_band"), QStringLiteral("ht.T")) - 293.15;
            // (One FSR 301 K, as heaterOverARing has it; the library's
            // silicon and silica, k of their temperature, a little off its.)
            const double perFsr = 1.55e-6 / (28.573e-6 * 1.8e-4), fsr = perFsr / one;
            const QString said = QStringLiteral("the ring heater: its racetracks %1 and %2 K per mW: %3 mW per FSR per ring").arg(one).arg(two).arg(fsr);
            qInfo("%s", qPrintable(said));
            QVERIFY2(std::abs(one - two) < 1e-3 * one && std::abs(fsr - 21.2) < 0.1 * 21.2, qPrintable(said));
        }
        {
            const Solved sv = example(QStringLiteral("tsv.qfem3d"), &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
            const double area = M_PI * 2.5e-6 * 2.5e-6, R = 25e-6 / (5.998e7 * area), copper = 25e-6 / (400 * area);
            const double r = global(sv.s(), QStringLiteral("ec.R")), rth = global(sv.s(), QStringLiteral("ht.Rth"));
            const QString said = QStringLiteral("the TSV: %1 Ohm (Katti's %2), %3 K/W (its copper alone %4)").arg(r).arg(R).arg(rth).arg(copper);
            qInfo("%s", qPrintable(said));
            QVERIFY2(std::abs(r - R) < 1e-3 * R, qPrintable(said));
            QVERIFY2(rth < copper && rth > 0.25 * copper, qPrintable(said));
        }
    }
};

QTEST_GUILESS_MAIN(TestFem3Devices)
#include "test_fem3_devices.moc"
