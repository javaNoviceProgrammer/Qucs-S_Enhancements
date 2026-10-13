/*
 * test_fem_devices.cpp - semiconductor devices beyond drift and diffusion
 *                        at its simplest: impact ionization (breakdown,
 *                        avalanche gain), a MOSFET's mobility (Lombardi's
 *                        surface scattering, saturation along the current),
 *                        heavy doping (Fermi-Dirac, band gap narrowing,
 *                        dopants ionized in part), interface traps,
 *                        Schrödinger-Poisson, the carriers' heat and
 *                        self-heating, tunnelling (band to band,
 *                        Fowler-Nordheim), implants and their anneal
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_geometry.h"
#include "fem_mesh.h"
#include "fem_model.h"
#include "fem_results.h"
#include "fem_solver.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QtTest>

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <vector>

using namespace qucs_s::fem;

namespace {

constexpr double Q = 1.602176634e-19, Eps0 = 8.8541878188e-12;

bool slowBuild()
{
    // (QUCS_TEST_SLOW: a slow build's path in any, to check it quickly.)
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

Model modelOf(const QByteArray& json)
{
    QString why;
    std::optional<Model> m = Model::fromJson(json, &why);
    if (!m) qFatal("not a model: %s", qPrintable(why));
    if (!why.isEmpty()) qWarning("%s", qPrintable(why));
    return *m;
}

struct Run {
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<Solution> solution;
    StudyRun study;
    QString error;
};

Run run(const Model& model, const QString& study = {})
{
    Run r;
    r.parameters = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *r.parameters);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.study = runStudy(model, r.parameters, g.topology, nullptr, study);
    if (!r.study.ok()) {
        r.error = r.study.error;
        return r;
    }
    r.solution = r.study.solutions.back();
    return r;
}

double global(const Solution& s, const QString& name)
{
    const std::optional<double> v = s.global(name);
    return v ? *v : NAN;
}

/// A one-sided abrupt silicon junction along x (µm): p⁺ 1e19 cm⁻³ 0.5 wide,
/// then n-type \a N over \a L; ohmic contacts at its ends (the anode a at
/// Va); \a features the semiconductor's more, \a steps its study's, \a hmax
/// the mesh's.
QByteArray diode(const QString& N, const QString& L, const QString& features, const QString& steps, const QString& hmax)
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Va", "expression": "0[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "label": "pplus", "position": ["-0.5", "0"], "size": ["0.5", "0.2"]},
                     {"type": "rectangle", "label": "nside", "position": ["0", "0"], "size": ["%2", "0.2"]}],
        "materials": [{"material": "Silicon", "selection": {"all": true}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "1e19[1/cm^3]", "selection": {"only": ["pplus"]}},
          {"type": "doping", "dopant": "donor", "N0": "%1", "selection": {"only": ["nside"]}},
          {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["pplus"], "side": "left"}},
          {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nside"], "side": "right"}} %3]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%5"}}],
      "studies": [{"type": "study", "steps": [%4]}]})json")
        .arg(N, L, features, steps, hmax)
        .toUtf8();
}

/// An n-channel MOSFET (µm), its sizes \a g's: a channel g.L long under 10
/// nm of oxide on p-type \a Na, n⁺ 1e20 cm⁻³ source and drain g.w wide and
/// 0.1 deep, an n⁺ polysilicon gate at Vg, the drain at Vd; \a model the
/// semiconductor model's more, \a steps its study's; the channel's top
/// g.layer (and the oxide) meshed at g.fine, the rest coarser.
struct MosSizes {
    double L, w, depth, layer, fine, near, far;
};
QByteArray mosfet(const QString& Na, const QString& model, const QString& steps, const MosSizes& g)
{
    auto n = [](double v) { return QString::number(v, 'g', 10); };
    const double width = g.L + 2 * g.w;
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Vg", "expression": "0[V]"}, {"name": "Vd", "expression": "0.1[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "label": "src", "position": ["0", "-0.1"], "size": ["%5", "0.1"]},
                     {"type": "rectangle", "label": "drn", "position": ["%6", "-0.1"], "size": ["%5", "0.1"]},
                     {"type": "rectangle", "label": "chan", "position": ["%5", "-%8"], "size": ["%7", "%8"]},
                     {"type": "rectangle", "label": "under", "position": ["%5", "-0.1"], "size": ["%7", "%9"]},
                     {"type": "rectangle", "label": "bulk", "position": ["0", "-%10"], "size": ["%11", "%12"]},
                     {"type": "rectangle", "label": "oxide", "position": ["0", "0"], "size": ["%11", "0.01"]}],
        "materials": [{"material": "Silicon", "selection": {"only": ["src", "drn", "chan", "under", "bulk"]}},
                      {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "%1", "selection": {"only": ["src", "drn", "chan", "under", "bulk"]}},
          {"type": "doping", "dopant": "donor", "N0": "1e20[1/cm^3]", "selection": {"only": ["src", "drn"]}},
          {"type": "model", "mobility": "doping" %2, "selection": {"all": true}},
          {"type": "gate", "name": "g", "V0": "Vg", "phim": "4.05[eV]", "selection": {"of": ["oxide"], "side": "top"}},
          {"type": "ohmic", "name": "s", "V0": "0[V]", "selection": {"of": ["src"], "side": "left"}},
          {"type": "ohmic", "name": "d", "V0": "Vd", "selection": {"of": ["drn"], "side": "right"}},
          {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["bulk"], "side": "bottom"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%14", "features": [
          {"type": "size", "level": "domain", "selection": {"only": ["chan", "oxide"]}, "hmax": "%4"},
          {"type": "size", "level": "domain", "selection": {"only": ["src", "drn", "under"]}, "hmax": "%13"}]}}],
      "studies": [{"type": "study", "steps": [%3]}]})json")
        .arg(Na, model, steps, n(g.fine), n(g.w), n(g.w + g.L), n(g.L), n(g.layer), n(0.1 - g.layer))
        .arg(n(g.depth), n(width), n(g.depth - 0.1), n(g.near), n(g.far))
        .toUtf8();
}

/// Lombardi's surface mobility (m²/(V s)) for silicon's electrons: µbulk
/// with acoustic phonons and the surface's roughness at E⊥ (V/m), doping N
/// (1/m³), d from the interface (m), lcrit 10 nm, 300 K.
double lombardiElectrons(double bulk, double across, double N, double d)
{
    across = std::max(across, 100.0);
    const double ac = 4.75e5 / across + 1e-4 * 1.74e5 * std::pow(std::max(N * 1e-6, 1.0), 0.125) / (300 * std::cbrt(across / 100));
    const double sr = 5.82e14 / (across * across), D = std::exp(-d / 1e-8);
    return 1 / (1 / bulk + D / ac + D / sr);
}

/// A silicon bar along x (µm), 0.2 high: \a body its regions and doping
/// (features), ohmic contacts l at x = 0 (at Va) and r at its end;
/// \a model the semiconductor model's more, \a steps its study's.
QByteArray siliconBar(const QString& geometry, const QString& doping, const QString& model, const QString& steps, const QString& hmax)
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Va", "expression": "0[V]"}],
      "components": [{"unit": "um",
        "geometry": [%1],
        "materials": [{"material": "Silicon", "selection": {"all": true}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          %2,
          {"type": "model" %3, "selection": {"all": true}},
          {"type": "ohmic", "name": "l", "V0": "Va", "selection": {"of": ["left"], "side": "left"}},
          {"type": "ohmic", "name": "r", "V0": "0[V]", "selection": {"of": ["right"], "side": "right"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%5"}}],
      "studies": [{"type": "study", "steps": [%4]}]})json")
        .arg(geometry, doping, model, steps, hmax)
        .toUtf8();
}

/// A MOS capacitor (µm): 10 nm of oxide on p-type 1e17 cm⁻³ silicon 0.4
/// deep, \a width wide, an n⁺ polysilicon gate g at Vg, the substrate b;
/// \a traps its interface's features, \a steps its study's; its top 20
/// nm and the oxide meshed at \a fine.
QByteArray mosCapacitor(const QString& traps, const QString& steps, const QString& width = QStringLiteral("0.1"),
                        const QString& fine = QStringLiteral("0.002"))
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Vg", "expression": "0[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "bulk", "position": ["0", "-0.4"], "size": ["%3", "0.38"]},
                     {"type": "rectangle", "name": "surface", "position": ["0", "-0.02"], "size": ["%3", "0.02"]},
                     {"type": "rectangle", "name": "oxide", "size": ["%3", "0.01"]}],
        "materials": [{"material": "Silicon", "selection": {"only": ["bulk", "surface"]}},
                      {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["bulk", "surface"]}},
          {"type": "gate", "name": "g", "V0": "Vg", "phim": "4.05[eV]", "selection": {"of": ["oxide"], "side": "top"}},
          {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["bulk"], "side": "bottom"}} %1]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "0.02", "features": [
          {"type": "size", "level": "domain", "selection": {"only": ["surface", "oxide"]}, "hmax": "%4"}]}}],
      "studies": [{"type": "study", "steps": [%2]}]})json")
        .arg(traps, steps, width, fine)
        .toUtf8();
}

/// A pn junction (µm): p 1e18 cm⁻³ and n 1e16, 1 µm each, 0.2 high, its
/// mobilities by doping, lifetimes 10 ns, the anode a at \a va; meshed at
/// \a hmax.
QByteArray diodeHeat(const QString& va, const QString& hmax)
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "p", "size": ["1", "0.2"]},
                     {"type": "rectangle", "name": "n", "position": ["1", "0"], "size": ["1", "0.2"]}],
        "materials": [{"material": "Silicon", "selection": {"all": true}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "1e18[1/cm^3]", "selection": {"only": ["p"]}},
          {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"only": ["n"]}},
          {"type": "model", "mobility": "doping", "selection": {"all": true}},
          {"type": "srh", "source": "user", "taun": "10[ns]", "taup": "10[ns]", "selection": {"all": true}},
          {"type": "ohmic", "name": "a", "V0": "%1", "selection": {"of": ["p"], "side": "left"}},
          {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "right"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
        .arg(va, hmax)
        .toUtf8();
}

/// \a expression at (x, y) (µm) of \a s.
double at1(const Solution& s, const QString& expression, double x, double y)
{
    const Locator locator(s.mesh());
    const std::optional<double> v = evaluateAt(s, locator, expression, QPointF(x, y));
    return v ? *v : NAN;
}

/// Fermi-Dirac's integral of order 1/2, normalized (e^η where carriers are
/// few): Simpson's rule in t = √ε, fine.
double fermiHalf(double eta)
{
    const double T = std::sqrt(std::max(eta, 0.0) + 70);
    const int m = 20000;
    const double h = T / m;
    double sum = 0;
    for (int j = 0; j <= m; ++j) {
        const double t = j * h, w = (j == 0 || j == m) ? 1 : (j % 2 ? 4 : 2);
        sum += w * t * t / (1 + std::exp(t * t - eta));
    }
    return 4 / std::sqrt(M_PI) * sum * h / 3;
}

/// η where fermiHalf(η) = u (halved).
double fermiHalfInverse(double u)
{
    double lo = std::log(u) - 1, hi = std::max(std::log(u), 0.0) + 2 * std::pow(u, 2.0 / 3) + 2;
    for (int k = 0; k < 100; ++k) {
        const double mid = (lo + hi) / 2;
        (fermiHalf(mid) < u ? lo : hi) = mid;
    }
    return (lo + hi) / 2;
}

/// Silicon's ionization rates (van Overstraeten and de Man), 1/m at E V/m.
void siliconAlphas(double E, double* an, double* ap)
{
    *an = E > 1e3 ? 7.03e7 * std::exp(-1.231e8 / E) : 0;
    *ap = E > 1e3 ? (E <= 4e7 ? 1.582e8 * std::exp(-2.036e8 / E) : 6.71e7 * std::exp(-1.693e8 / E)) : 0;
}

/// A one-sided abrupt junction's ionization integrals at its highest field
/// \a Emax: its field falling straight to nought over W = εs Emax/(q N)
/// (electrons from the junction into the n side, holes back).
void triangleIntegrals(double Emax, double N, double* In, double* Ip)
{
    const int n = 20000;
    const double W = 11.7 * Eps0 * Emax / (Q * N);
    std::vector<double> an(n + 1), ap(n + 1);
    for (int i = 0; i <= n; ++i) siliconAlphas(Emax * (1 - double(i) / n), &an[std::size_t(i)], &ap[std::size_t(i)]);
    const double dx = W / n;
    // Electrons from x = 0 on; holes from W back.
    double K = 0, total = 0;
    for (int i = 1; i <= n; ++i) {
        const std::size_t a = std::size_t(i - 1), b = std::size_t(i);
        const double before = an[a] * std::exp(-K);
        K += 0.5 * ((an[a] - ap[a]) + (an[b] - ap[b])) * dx;
        total += 0.5 * (before + an[b] * std::exp(-K)) * dx;
    }
    *In = total;
    K = 0;
    total = 0;
    for (int i = n - 1; i >= 0; --i) {
        const std::size_t a = std::size_t(i + 1), b = std::size_t(i);
        const double before = ap[a] * std::exp(-K);
        K += 0.5 * ((ap[a] - an[a]) + (ap[b] - an[b])) * dx;
        total += 0.5 * (before + ap[b] * std::exp(-K)) * dx;
    }
    *Ip = total;
}

/// The depletion approximation's breakdown voltage of a one-sided abrupt
/// silicon junction (p⁺ 1e19 cm⁻³ on N, m⁻³): where its triangle's
/// electrons' integral reaches 1, the field's highest √(2 q N (V + Vbi -
/// 2 Vt)/εs).
double triangleBreakdown(double N)
{
    const double vt = 1.380649e-23 * 300 / Q, ni = 9.65e15, vbi = vt * std::log(1e25 * N / (ni * ni));
    double lo = 1, hi = 1000;
    for (int i = 0; i < 60; ++i) {
        const double V = (lo + hi) / 2, Emax = std::sqrt(2 * Q * N * (V + vbi - 2 * vt) / (11.7 * Eps0));
        double In = 0, Ip = 0;
        triangleIntegrals(Emax, N, &In, &Ip);
        (In < 1 ? lo : hi) = V;
    }
    return lo;
}

/// A bar of n-type silicon (1e17 cm⁻³, µ 0.14 m²/(V s)) \a L long, its ends
/// at 300 K, \a V across it, heating itself: the one-dimensional problem,
/// -k T'' = J² ρ(T), ρ = ρ300 (T/300 K)^2.5, V = J ∫ρ dx - T by finite
/// differences (Picard's in ρ(T)), J by the secant's on V. Its current
/// density (A/m²), its middle's temperature in \a Tmid.
double selfHeated(double L, double V, double* Tmid)
{
    const double k = 130, n = 1e23, mu = 0.14;
    const double rho0 = 1 / (Q * n * mu);
    const int m = 2000;
    const double h = L / m;
    auto voltage = [&](double J) {
        std::vector<double> T(m + 1, 300.0);
        for (int it = 0; it < 200; ++it) {
            // -k (T[i-1] - 2T[i] + T[i+1])/h² = J² ρ(T[i]) (the Thomas algorithm).
            std::vector<double> a(m + 1, -1), b(m + 1, 2), c(m + 1, -1), d(m + 1, 0);
            for (int i = 1; i < m; ++i) d[i] = J * J * rho0 * std::pow(T[i] / 300, 2.5) * h * h / k;
            b[0] = b[m] = 1;
            c[0] = a[m] = 0;
            d[0] = d[m] = 300;
            for (int i = 1; i <= m; ++i) {
                const double w = a[i] / b[i - 1];
                b[i] -= w * c[i - 1];
                d[i] -= w * d[i - 1];
            }
            std::vector<double> next(m + 1);
            next[m] = d[m] / b[m];
            for (int i = m - 1; i >= 0; --i) next[i] = (d[i] - c[i] * next[i + 1]) / b[i];
            double change = 0;
            for (int i = 0; i <= m; ++i) change = std::max(change, std::abs(next[i] - T[i]));
            T = next;
            if (change < 1e-10) break;
        }
        double sum = 0;
        for (int i = 0; i < m; ++i) sum += 0.5 * (std::pow(T[i] / 300, 2.5) + std::pow(T[i + 1] / 300, 2.5)) * rho0 * h;
        return std::pair{J * sum, T[m / 2]};
    };
    double J0 = V / (rho0 * L), J1 = 0.9 * J0;
    double v0 = voltage(J0).first - V, v1 = voltage(J1).first - V;
    for (int it = 0; it < 50 && std::abs(v1) > 1e-12; ++it) {
        const double J2 = J1 - v1 * (J1 - J0) / (v1 - v0);
        J0 = J1;
        v0 = v1;
        J1 = J2;
        v1 = voltage(J1).first - V;
    }
    if (Tmid) *Tmid = voltage(J1).second;
    return J1;
}

/// Example \a name of the multiphysics examples, read as it is shipped.
Model exampleModel(const QString& name)
{
    const QString dir = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
    QFile f(dir + name);
    if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
    Model m = modelOf(f.readAll());
    m.fileName = dir + name;
    return m;
}

/// Every node of \a type in \a m given \a key's \a value.
void setAll(Model& m, const QString& type, const QString& key, const QJsonValue& value)
{
    std::function<void(Node&)> walk = [&](Node& n) {
        if (n.type == type) n.props.insert(key, value);
        for (Node& c : n.children) walk(c);
    };
    for (Node& n : m.root.children) walk(n);
}

/// \a m's sweeps over \a name alone, at \a values (\a unit).
void sweepOnly(Model& m, const QString& name, const QString& values, const QString& unit)
{
    QJsonArray rows;
    rows.append(QJsonArray{name, values, unit});
    setAll(m, QStringLiteral("sweep"), QStringLiteral("parameters"), rows);
}

} // namespace

class TestFemDevices : public QObject
{
    Q_OBJECT

private slots:
    void aSweepStartsFromTheLast()
    {
        // A sweep's next point starts from the last one's solution (its
        // semiconductor's values and contacts): from -10 V to -11 V in a few
        // steps, not from equilibrium in a dozen again - a solved field's
        // values, seven arrays by node, were compared with a fresh one's
        // room for its unknowns, and none was taken. A strip's right angles
        // across its boundaries (an edge's weight nought) are no edges left
        // out.
        const Run r = run(modelOf(diode(QStringLiteral("1e16[1/cm^3]"), QStringLiteral("4"), QString(),
                                        QStringLiteral(R"({"type": "sweep", "parameters": [["Va", "-10, -11", "V"]]}, {"type": "stationary"})"),
                                        QStringLiteral("0.1"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 2);
        auto steps = [](const Solution& s) {
            static const QRegularExpression reached(QStringLiteral("reached in (\\d+) steps"));
            for (const QString& line : s.log)
                if (const QRegularExpressionMatch m = reached.match(line); m.hasMatch()) return m.captured(1).toInt();
            return 1;
        };
        const int first = steps(*r.study.solutions[0]), second = steps(*r.study.solutions[1]);
        qInfo("to -10 V from equilibrium in %d steps, then to -11 V in %d", first, second);
        QVERIFY(first >= 10);
        QVERIFY2(second <= 5, qPrintable(QString::number(second)));
        for (const auto& point : r.study.solutions)
            for (const QString& line : point->log) QVERIFY2(!line.contains(QLatin1String("not Delaunay")), qPrintable(line));
    }

    void aJunctionsBreakdown()
    {
        // An abrupt one-sided silicon junction breaks down where the
        // ionization integral along its field reaches 1 - silicon's α's (van
        // Overstraeten and de Man's), the line through the highest field.
        // Its breakdown the depletion approximation's - the field falling
        // straight from its highest, the triangle's integrals - to 2 %; the
        // two integrals reach 1 together (a breakdown is one). Against Sze
        // and Gibbons' universal
        // BV ≈ 60 (Eg/1.1 eV)^1.5 (N/1e16 cm⁻³)^-0.75 V - 61.6 V at 1e16,
        // 11.0 V at 1e17 -, fitted to older α's (Lee's): these α's give 16 %
        // less at 1e16, 26 % more at 1e17 (the same integrals of the
        // triangle find 51.7 and 13.9 V); within 30 %.
        const bool slow = slowBuild();
        struct Case {
            const char* N;
            const char* L;
            const char* volts;
            double sze;
            const char* hmax;
        };
        // (Its points about the breakdown; a sanitized build's mesh coarser.)
        const Case cases[] = {{"1e16[1/cm^3]", "4", "-46, -50, -54, -58", 61.6, slow ? "0.1" : "0.05"},
                              {"1e17[1/cm^3]", "1", "-12, -13, -14, -15", 11.0, slow ? "0.04" : "0.02"},
                              {"1e16[1/cm^3]", "4", "-46, -50, -54, -58", 61.6, "0.25"}};
        for (const Case& c : cases) {
            const QString ii = QStringLiteral(R"(, {"type": "impact", "currents": false, "selection": {"all": true}})");
            const QString steps = QStringLiteral(R"({"type": "sweep", "parameters": [["Va", "%1", "V"]]}, {"type": "stationary"})").arg(QLatin1String(c.volts));
            const Run r = run(modelOf(diode(QLatin1String(c.N), QLatin1String(c.L), ii, steps, QLatin1String(c.hmax))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            std::vector<double> V, In, Ip;
            QStringList said;
            const double N = QString::fromLatin1(c.N).section(QLatin1Char('['), 0, 0).toDouble() * 1e6;
            for (const auto& point : r.study.solutions) {
                V.push_back(global(*point, QStringLiteral("semi.V_a")));
                In.push_back(global(*point, QStringLiteral("semi.In")));
                Ip.push_back(global(*point, QStringLiteral("semi.Ip")));
                said << QStringLiteral("%1 V: In %2, Ip %3, Emax %4 kV/cm over %5 µm")
                            .arg(V.back())
                            .arg(In.back(), 0, 'f', 4)
                            .arg(Ip.back(), 0, 'f', 4)
                            .arg(global(*point, QStringLiteral("semi.Emax")) / 1e5, 0, 'f', 1)
                            .arg(global(*point, QStringLiteral("semi.Lion")) * 1e6, 0, 'f', 2);
            }
            qInfo("%s: %s", c.N, qPrintable(said.join(QLatin1String("; "))));
            // Where In reaches 1 (in its logarithm, between the points).
            double BV = NAN, IpThere = NAN;
            for (std::size_t k = 1; k < V.size(); ++k)
                if (In[k - 1] < 1 && In[k] >= 1) {
                    const double f = (0 - std::log(In[k - 1])) / (std::log(In[k]) - std::log(In[k - 1]));
                    BV = -(V[k - 1] + f * (V[k] - V[k - 1]));
                    IpThere = std::exp(std::log(Ip[k - 1]) + f * (std::log(Ip[k]) - std::log(Ip[k - 1])));
                }
            const double analytic = triangleBreakdown(N);
            qInfo("%s: breakdown at %.2f V (the depletion approximation's %.2f; Sze and Gibbons' %.1f: %+.0f %%), Ip there %.4f", c.N, BV, analytic,
                  c.sze, 100 * (BV / c.sze - 1), IpThere);
            QVERIFY2(std::abs(BV / analytic - 1) < (slow ? 0.04 : 0.02), qPrintable(QString::number(BV / analytic)));
            QVERIFY2(std::abs(BV / c.sze - 1) < 0.3, qPrintable(QString::number(BV)));
            QVERIFY2(std::abs(IpThere - 1) < 0.02, qPrintable(QString::number(IpThere)));
        }
    }

    void anAvalancheGain()
    {
        // Light in the junction's n side, beyond its depletion: its holes
        // enter the field at its n end - the photocurrent multiplied as the
        // holes' integral says, 1/(1 - Ip), the ionization's pairs in the
        // currents (an APD's gain); without them, as it is. Lifetimes long
        // (1 µs): the light's holes reach the field or the cathode alike.
        // (A sanitized build: one voltage, a coarser mesh.)
        const bool slow = slowBuild();
        const QString hmax = slow ? QStringLiteral("0.12") : QStringLiteral("0.05");
        const QString light = QStringLiteral(R"x(, {"type": "generation", "G": "G0*(x > 3.1[um])*(x < 3.6[um])", "selection": {"all": true}},
          {"type": "srh", "source": "user", "taun": "1[us]", "taup": "1[us]", "selection": {"all": true}})x");
        QStringList said;
        const std::vector<const char*> biases = slow ? std::vector<const char*>{"-48"} : std::vector<const char*>{"-46", "-48"};
        for (const char* volts : biases) {
            double photo[2] = {0, 0}, Mp = NAN;
            for (int on = 0; on < 2; ++on) {
                const QString ii = QStringLiteral(R"(, {"type": "impact", "currents": %1, "selection": {"all": true}})").arg(on ? QStringLiteral("true") : QStringLiteral("false"));
                QByteArray json = diode(QStringLiteral("1e16[1/cm^3]"), QStringLiteral("4"), ii + light,
                                        QStringLiteral(R"x({"type": "sweep", "parameters": [["G0", "0, 1e25", "1/(m^3*s)"]]}, {"type": "stationary"})x"), hmax);
                json.replace(R"("parameters": [{"name": "Va", "expression": "0[V]"}])",
                             QStringLiteral(R"x("parameters": [{"name": "Va", "expression": "%1[V]"}, {"name": "G0", "expression": "0[1/(m^3*s)]"}])x").arg(QLatin1String(volts)).toUtf8());
                const Run r = run(modelOf(json));
                QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                QCOMPARE(int(r.study.solutions.size()), 2);
                const double dark = global(*r.study.solutions.front(), QStringLiteral("semi.I_k")), lit = global(*r.study.solutions.back(), QStringLiteral("semi.I_k"));
                photo[on] = lit - dark;
                if (!on) Mp = global(*r.study.solutions.back(), QStringLiteral("semi.Mp"));
            }
            const double M = photo[1] / photo[0];
            said << QStringLiteral("%1 V: photocurrent %2 A, multiplied %3 A - %4 times (1/(1 - Ip) %5; 1 - 1/M %6, Ip %7)")
                        .arg(QLatin1String(volts))
                        .arg(photo[0], 0, 'g', 5)
                        .arg(photo[1], 0, 'g', 5)
                        .arg(M, 0, 'f', 3)
                        .arg(Mp, 0, 'f', 3)
                        .arg(1 - 1 / M, 0, 'f', 4)
                        .arg(1 - 1 / Mp, 0, 'f', 4);
            // (Near breakdown M = 1/(1 - Ip) magnifies: 1 % in Ip is 6 % in M
            // at 6. The ionization's probability, 1 - 1/M, to 2 %.)
            QVERIFY2(Mp > 1.5 && std::abs((1 - 1 / M) / (1 - 1 / Mp) - 1) < (slow ? 0.04 : 0.02), qPrintable(said.join(QLatin1String("; "))));
        }
        qInfo("avalanche: %s", qPrintable(said.join(QLatin1String("; "))));
        // Beyond breakdown (51.7 V): no solution - an error, not a study
        // gone on with nothing solved (a failure at rest was said as a
        // warning: its words "did not converge").
        QByteArray beyond = diode(QStringLiteral("1e16[1/cm^3]"), QStringLiteral("4"), QStringLiteral(R"(, {"type": "impact", "selection": {"all": true}})"),
                                  QStringLiteral(R"({"type": "stationary"})"), hmax);
        beyond.replace(R"({"name": "Va", "expression": "0[V]"})", R"({"name": "Va", "expression": "-60[V]"})");
        const Run broken = run(modelOf(beyond));
        QVERIFY2(broken.error.contains(QLatin1String("no solution found")), qPrintable(broken.error));
    }

    void aMosfetsMobility()
    {
        // An n-channel MOSFET (1 µm, 10 nm of oxide, p-type 1e17 cm⁻³; a
        // sanitized build's 0.5 µm, meshed more coarsely, at 1.5 V alone) at
        // Vd 0.1 V, in its linear range:
        // - Lombardi's surface scattering as solved: each triangle's mobility
        //   (semi.mun) the formula's at its field across the interface - the
        //   electrons' mean across the channel to 2 %;
        // - saturated by the force along the current, the drain current the
        //   unsaturated one's less the channel's own saturation, 1/√(1 + (µ
        //   F/vsat)²) at its middle's mobility and force, to 0.3 % - the
        //   gate's field across the channel (0.5 MV/cm, µE/vsat 20) heats
        //   nothing; saturated by the whole field, cut to a fraction (the
        //   review's run: some 20 times);
        // - the effective mobility Id/(W Qinv F) - Qinv by Gauss's law, the
        //   gate's charge less the depletion's, F the force along the
        //   channel - on the universal curve's scale, 300-600 cm²/Vs at 0.3
        //   to 0.7 MV/cm, falling as the effective field Eeff = (Qb +
        //   Qinv/2)/εs rises.
        const bool slow = slowBuild();
        const MosSizes g = slow ? MosSizes{0.4, 0.1, 0.25, 0.02, 0.006, 0.04, 0.1} : MosSizes{1, 0.3, 0.6, 0.03, 0.003, 0.02, 0.05};
        const double mid = g.w + g.L / 2, dx = g.L / 20;
        const double Na = 1e23, es = 11.7 * Eps0;
        const double bulk = 1e-4 * (68.5 + (1400 - 68.5) / (1 + std::pow(Na / 9.2e22, 0.711)));
        auto solve = [&](const QString& volts, const QString& model) {
            const QString sweep = QStringLiteral(R"({"type": "sweep", "parameters": [["Vg", "%1", "V"]]}, {"type": "stationary"})").arg(volts);
            Run r = run(modelOf(mosfet(QStringLiteral("1e17[1/cm^3]"), model, sweep, g)));
            if (!r.error.isEmpty()) qWarning("%s", qPrintable(r.error));
            return r;
        };
        // (A sanitized build: 1.5 V alone - each solve there some 40 s.)
        const Run lombardi = solve(slow ? QStringLiteral("1.5") : QStringLiteral("1.5, 3"), QStringLiteral(R"(, "surface": true, "saturation": true)"));
        QVERIFY2(lombardi.error.isEmpty(), qPrintable(lombardi.error));
        QCOMPARE(int(lombardi.study.solutions.size()), slow ? 1 : 2);
        QStringList said;
        double lastE = 0, lastMu = INFINITY, own = NAN;
        for (const auto& point : lombardi.study.solutions) {
            const Solution& s = *point;
            const Locator locator(s.mesh());
            auto at = [&](const QString& e, double x, double y) {
                const std::optional<double> v = evaluateAt(s, locator, e, QPointF(x, y));
                return v ? *v : NAN;
            };
            const double Id = std::abs(global(s, QStringLiteral("semi.I_d")));
            // Across the channel at its middle (x 0.8 µm): the electrons'
            // mobility as solved and as Lombardi's, by their numbers; the
            // depletion's charge.
            double stored = 0, formula = 0, weight = 0, Qb = 0;
            const int n = 200;
            for (int k = 0; k < n; ++k) {
                const double y = -(k + 0.5) * 0.02 / n;
                const double ne = at(QStringLiteral("semi.n"), mid, y);
                stored += ne * at(QStringLiteral("semi.mun"), mid, y);
                formula += ne * lombardiElectrons(bulk, std::abs(at(QStringLiteral("semi.Ey"), mid, y)), Na, -y * 1e-6);
                weight += ne;
            }
            for (int k = 0; k < n; ++k) {
                const double y = -(k + 0.5) * 0.25 / n;
                Qb += Q * (Na - at(QStringLiteral("semi.p"), mid, y)) * 0.25e-6 / n;
            }
            const double Es = 3.9 / 11.7 * std::abs(at(QStringLiteral("semi.Ey"), mid, 0.005));
            const double Qinv = es * Es - Qb, Eeff = (Qb + Qinv / 2) / es;
            const double F = std::abs(at(QStringLiteral("semi.phin"), mid + dx, -0.001) - at(QStringLiteral("semi.phin"), mid - dx, -0.001)) / (2 * dx * 1e-6);
            const double muEff = Id / (Qinv * F);
            said << QStringLiteral("Vg %1 V: Id %2 A/m, the channel's mobility %3 cm²/Vs (Lombardi's %4), Qinv %5 C/m², Eeff %6 MV/cm, µeff %7 cm²/Vs")
                        .arg(global(s, QStringLiteral("semi.V_g")))
                        .arg(Id, 0, 'g', 5)
                        .arg(stored / weight * 1e4, 0, 'f', 1)
                        .arg(formula / weight * 1e4, 0, 'f', 1)
                        .arg(Qinv, 0, 'g', 4)
                        .arg(Eeff / 1e8, 0, 'f', 3)
                        .arg(muEff * 1e4, 0, 'f', 1);
            QVERIFY2(std::abs(stored / formula - 1) < 0.02, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(Eeff > 0.3e8 && Eeff < 0.8e8 && muEff > 300e-4 && muEff < 600e-4, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(Eeff > lastE && muEff < lastMu, qPrintable(said.join(QLatin1String("; "))));
            lastE = Eeff;
            lastMu = muEff;
            // (The channel's own saturation at its mobility and force: 1/√(1 +
            // (µ F/vsat)²).)
            if (!std::isfinite(own)) own = 1 / std::sqrt(1 + std::pow(stored / weight * F / 1e5, 2));
        }
        // Saturation: by the force along the current, by the whole field.
        const double along = std::abs(global(*lombardi.study.solutions.front(), QStringLiteral("semi.I_d")));
        const Run unsaturated = solve(QStringLiteral("1.5"), QStringLiteral(R"(, "surface": true)"));
        const Run whole = solve(QStringLiteral("1.5"), QStringLiteral(R"(, "surface": true, "saturation": true, "driving": "field")"));
        QVERIFY2(unsaturated.error.isEmpty() && whole.error.isEmpty(), qPrintable(unsaturated.error + whole.error));
        const double free = std::abs(global(*unsaturated.solution, QStringLiteral("semi.I_d")));
        const double byField = std::abs(global(*whole.solution, QStringLiteral("semi.I_d")));
        said << QStringLiteral("at 1.5 V saturated along the current %1 of the unsaturated current (the channel's own %2), by the whole field %3")
                    .arg(along / free, 0, 'f', 4)
                    .arg(own, 0, 'f', 4)
                    .arg(byField / free, 0, 'f', 4);
        qInfo("MOSFET: %s", qPrintable(said.join(QLatin1String("; "))));
        QVERIFY2(std::abs(along / free - own) < 3e-3, qPrintable(said.join(QLatin1String("; "))));
        QVERIFY2(byField / free < 0.3, qPrintable(said.join(QLatin1String("; "))));
    }

    void aDegenerateStep()
    {
        // An n⁺ (1e20 cm⁻³) to n (1e17) step in silicon, 1.5 µm each, its
        // carriers Boltzmann's, Fermi-Dirac's, the gap narrowed (Slotboom's,
        // Klaassen's values), and both:
        // - the potential's step from the n⁺ side's middle to the n side's:
        //   Vt (η⁺ - η) - (ΔEg⁺ - ΔEg)/2q, η = ℱ⁻¹(N/Nc) (Boltzmann's ln(N/Nc))
        //   - to 10 µV;
        // - the n⁺ side's holes: p n/ni² = e^(ΔEg/kT) γ, γ = ℱ(η)/e^η (0.30 at
        //   1e20) - to 1e-4;
        // - at ±1 mV the currents opposite, to 1e-4 (the junction's own
        //   second order 3e-6): the contacts and the fluxes as the statistics
        //   (a contact's potential by Boltzmann's on Fermi-Dirac's carriers,
        //   Vt ln γ off - 31 mV at 1e20 -, would drive a current at nought);
        // - at 1 kHz its conductance the dc currents' slope, to 1e-6 (the
        //   small-signal carriers δn = n ℱ'/ℱ (δV - δφn)).
        constexpr double KB = 1.380649e-23;
        const double Vt = KB * 300 / Q, Nc = 2.8e25, Nv = 1.04e25, ni = std::sqrt(Nc * Nv) * std::exp(-1.12 / (2 * Vt));
        auto narrowing = [](double N) {   // eV
            const double l = std::log(N / 1.3e23);
            return 6.92e-3 * (l + std::sqrt(l * l + 0.5));
        };
        const QString geometry = QStringLiteral(R"({"type": "rectangle", "label": "left", "size": ["1.5", "0.2"]},
                     {"type": "rectangle", "label": "right", "position": ["1.5", "0"], "size": ["1.5", "0.2"]})");
        const QString doping = QStringLiteral(R"({"type": "doping", "dopant": "donor", "N0": "1e20[1/cm^3]", "selection": {"only": ["left"]}},
          {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"only": ["right"]}})");
        const QString steps = QStringLiteral(R"({"type": "sweep", "parameters": [["Va", "0, -1e-3, 1e-3", "V"]]}, {"type": "stationary"},
          {"type": "frequency", "frequencies": "1[kHz]"})");
        QStringList said;
        for (const bool fd : {false, true})
            for (const bool bgn : {false, true}) {
                QString model;
                if (fd) model += QStringLiteral(R"(, "fermi": true)");
                if (bgn) model += QStringLiteral(R"(, "bgn": true)");
                Run r = run(modelOf(siliconBar(geometry, doping, model, steps, QStringLiteral("0.05"))));
                QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                QCOMPARE(int(r.study.solutions.size()), 3);
                Solution& s = *r.study.solutions[0];
                s.select(0);
                const double G = global(s, QStringLiteral("semi.Gll"));
                s.select(-1);
                r.study.solutions[1]->select(-1);
                r.study.solutions[2]->select(-1);
                const Locator locator(s.mesh());
                auto at = [&](const QString& e, double x) {
                    const std::optional<double> v = evaluateAt(s, locator, e, QPointF(x, 0.1));
                    return v ? *v : NAN;
                };
                const double etaPlus = fd ? fermiHalfInverse(1e26 / Nc) : std::log(1e26 / Nc);
                const double eta = fd ? fermiHalfInverse(1e23 / Nc) : std::log(1e23 / Nc);
                const double gPlus = bgn ? narrowing(1e26) : 0, g = bgn ? narrowing(1e23) : 0;
                const double step = Vt * (etaPlus - eta) - (gPlus - g) / 2;
                const double gamma = fd ? fermiHalf(etaPlus) / std::exp(etaPlus) : 1;
                const double pn = std::exp(gPlus / Vt) * gamma;
                const double dV = at(QStringLiteral("semi.V"), 0.75) - at(QStringLiteral("semi.V"), 2.25);
                const double holes = at(QStringLiteral("semi.p"), 0.75) * at(QStringLiteral("semi.n"), 0.75) / (ni * ni);
                const double Iminus = global(*r.study.solutions[1], QStringLiteral("semi.I_l"));
                const double Iplus = global(*r.study.solutions[2], QStringLiteral("semi.I_l"));
                said << QStringLiteral("%1%2: the step %3 V (%4), p n/ni² %5 (%6), at ±1 mV %7 and %8 A, G %9 S (%10)")
                            .arg(fd ? QStringLiteral("Fermi-Dirac") : QStringLiteral("Boltzmann"), bgn ? QStringLiteral(" narrowed") : QString())
                            .arg(dV, 0, 'f', 6)
                            .arg(step, 0, 'f', 6)
                            .arg(holes, 0, 'g', 6)
                            .arg(pn, 0, 'g', 6)
                            .arg(Iminus, 0, 'g', 6)
                            .arg(Iplus, 0, 'g', 6)
                            .arg(G, 0, 'g', 8)
                            .arg((Iplus - Iminus) / 2e-3, 0, 'g', 8);
                QVERIFY2(std::abs(dV - step) < 1e-5, qPrintable(said.join(QLatin1String("; "))));
                QVERIFY2(std::abs(holes / pn - 1) < 1e-4, qPrintable(said.join(QLatin1String("; "))));
                QVERIFY2(Iplus > 0 && std::abs(Iplus + Iminus) < 1e-4 * Iplus, qPrintable(said.join(QLatin1String("; "))));
                QVERIFY2(std::abs(G / ((Iplus - Iminus) / 2e-3) - 1) < 1e-6, qPrintable(said.join(QLatin1String("; "))));
            }
        qInfo("degenerate step: %s", qPrintable(said.join(QLatin1String("; "))));
    }

    void aDegenerateAccumulation()
    {
        // 10 nm of oxide on n-type 1e20 cm⁻³ silicon, the gate at 1 V: its
        // electrons accumulated at the surface, degenerate. Its small-signal
        // capacitance at 1 kHz the quasi-static one (dQ/dV at equilibrium)
        // to 1e-4, each carrier's change by Fermi-Dirac's - n ℱ'/ℱ in the
        // first, Nc ℱ' in the second; the substrate's current the gate's,
        // C_bg = -C_gg to 1e-4 (its carriers' change read back as the system
        // has it); and Fermi-Dirac's below Boltzmann's, the band filling as
        // the surface potential rises.
        QStringList said;
        double boltzmann = NAN;
        for (const bool fd : {false, true}) {
            const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "bulk", "position": ["0", "-0.2"], "size": ["0.1", "0.18"]},
                             {"type": "rectangle", "name": "surface", "position": ["0", "-0.02"], "size": ["0.1", "0.02"]},
                             {"type": "rectangle", "name": "oxide", "size": ["0.1", "0.01"]}],
                "materials": [{"material": "Silicon", "selection": {"only": ["bulk", "surface"]}},
                              {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e20[1/cm^3]", "selection": {"only": ["bulk", "surface"]}},
                  {"type": "model", "fermi": %1, "selection": {"all": true}},
                  {"type": "gate", "name": "g", "V0": "1[V]", "phim": "4.05[eV]", "selection": {"of": ["oxide"], "side": "top"}},
                  {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["bulk"], "side": "bottom"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.02", "features": [
                  {"type": "size", "level": "domain", "selection": {"only": ["surface", "oxide"]}, "hmax": "0.002"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}, {"type": "frequency", "frequencies": "1[kHz]"}]}]})json")
                                         .arg(fd ? QStringLiteral("true") : QStringLiteral("false"))
                                         .toUtf8();
            const Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double C = global(*r.solution, QStringLiteral("semi.Cgg")), C0 = global(*r.solution, QStringLiteral("semi.C0gg"));
            const double Cbg = global(*r.solution, QStringLiteral("semi.Cbg"));
            const double cox = 3.9 * Eps0 / 10e-9 * 0.1e-6;
            said << QStringLiteral("%1: C %2 of Cox at 1 kHz, %3 quasi-static, the substrate's %4")
                        .arg(fd ? QStringLiteral("Fermi-Dirac") : QStringLiteral("Boltzmann"))
                        .arg(C / cox, 0, 'f', 5)
                        .arg(C0 / cox, 0, 'f', 5)
                        .arg(Cbg / cox, 0, 'f', 5);
            QVERIFY2(std::abs(C / C0 - 1) < 1e-4, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(std::abs(Cbg / C + 1) < 1e-4, qPrintable(said.join(QLatin1String("; "))));
            if (!fd) boltzmann = C;
            else QVERIFY2(C < boltzmann, qPrintable(said.join(QLatin1String("; "))));
        }
        qInfo("degenerate accumulation: %s", qPrintable(said.join(QLatin1String("; "))));
    }

    void aDegenerateChannel()
    {
        // A gated film: 20 nm of n-type 1e19 cm⁻³ silicon, 0.2 µm long, under
        // 10 nm of oxide, the gate at 2 V accumulating it (degenerate at the surface),
        // the drain at 0.1 V. Its transconductance at 1 kHz the dc one, from
        // the gate ±10 mV, to 1e-5 - the small-signal currents along the
        // channel each step's change with the carriers', Fermi-Dirac's ln γ
        // in it. (A sanitized build: Fermi-Dirac's alone.)
        QStringList said;
        for (const bool fd : {false, true}) {
            if (slowBuild() && !fd) continue;
            const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "parameters": [{"name": "Vg", "expression": "2[V]"}],
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "film", "position": ["0", "-0.02"], "size": ["0.2", "0.02"]},
                             {"type": "rectangle", "name": "oxide", "size": ["0.2", "0.01"]}],
                "materials": [{"material": "Silicon", "selection": {"only": ["film"]}},
                              {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e19[1/cm^3]", "selection": {"only": ["film"]}},
                  {"type": "model", "fermi": %1, "selection": {"all": true}},
                  {"type": "gate", "name": "g", "V0": "Vg", "phim": "4.05[eV]", "selection": {"of": ["oxide"], "side": "top"}},
                  {"type": "ohmic", "name": "s", "V0": "0[V]", "selection": {"of": ["film"], "side": "left"}},
                  {"type": "ohmic", "name": "d", "V0": "0.1[V]", "selection": {"of": ["film"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.003"}}],
              "studies": [{"type": "study", "steps": [{"type": "sweep", "parameters": [["Vg", "2, 1.99, 2.01", "V"]]},
                                                      {"type": "stationary"}, {"type": "frequency", "frequencies": "1[kHz]"}]}]})json")
                                         .arg(fd ? QStringLiteral("true") : QStringLiteral("false"))
                                         .toUtf8();
            Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QCOMPARE(int(r.study.solutions.size()), 3);
            Solution& s = *r.study.solutions[0];
            s.select(0);
            const double gm = global(s, QStringLiteral("semi.Gdg"));
            r.study.solutions[1]->select(-1);
            r.study.solutions[2]->select(-1);
            const double dc = (global(*r.study.solutions[2], QStringLiteral("semi.I_d")) - global(*r.study.solutions[1], QStringLiteral("semi.I_d"))) / 0.02;
            said << QStringLiteral("%1: gm %2 S at 1 kHz, %3 from the dc currents").arg(fd ? QStringLiteral("Fermi-Dirac") : QStringLiteral("Boltzmann")).arg(gm, 0, 'g', 7).arg(dc, 0, 'g', 7);
            QVERIFY2(std::abs(gm / dc - 1) < 1e-5, qPrintable(said.join(QLatin1String("; "))));
        }
        qInfo("degenerate channel: %s", qPrintable(said.join(QLatin1String("; "))));
    }

    void interfaceTrapsStretchOut()
    {
        // The MOS capacitor at rest from flat band to weak inversion, with
        // and without 5e11 cm⁻² eV⁻¹ of interface traps (acceptor-like above
        // the intrinsic level, donor-like below, ±0.5 eV): each gate voltage
        // Vfb + ψs - (Qs(ψs) + Qit(ψs))/Cox at the surface potential solved,
        // Qs the exact one-dimensional charge, Qit = -q² Dit (ψs - φB) (the
        // traps filled to the Fermi level, neutral where it meets the
        // intrinsic one) - to 2 mV; the quasi-static capacitance Cox in
        // series with Cs + q² Dit to 0.5 %. The C-V curve stretched: from
        // -0.9 to 0 V the gate carries ψs 0.504 V, not 0.565.
        constexpr double KB = 1.380649e-23;
        const double Vt = KB * 300 / Q, Na = 1e23, es = 11.7 * Eps0, cox = 3.9 * Eps0 / 10e-9;
        const double ni = std::sqrt(2.8e25 * 1.04e25) * std::exp(-1.12 / (2 * Vt));
        const double dEc = 1.12 / 2 + Vt / 2 * std::log(2.8 / 1.04), phiB = Vt * std::log(Na / ni), vfb = -dEc - phiB;
        const double Dit = 5e15 / Q;   // 1/(m² J)
        auto Qs = [&](double psi) {    // C/m², the semiconductor's (exact, Boltzmann's)
            const double b = psi / Vt, r = ni * ni / (Na * Na);
            const double F = std::sqrt(std::max(std::exp(-b) + b - 1 + r * (std::exp(b) - b - 1), 0.0));
            return -(psi >= 0 ? 1 : -1) * std::sqrt(2 * es * KB * 300 * Na) * F;
        };
        const QString traps = QStringLiteral(R"(, {"type": "traps", "kind": "acceptor", "distribution": "uniform", "Dit": "5e11[1/(cm^2*eV)]",
            "Elow": "0[eV]", "Ehigh": "0.5[eV]", "levels": 32, "selection": {"of": ["surface"], "side": "top"}},
          {"type": "traps", "kind": "donor", "distribution": "uniform", "Dit": "5e11[1/(cm^2*eV)]", "Elow": "-0.5[eV]", "Ehigh": "0[eV]",
            "levels": 32, "selection": {"of": ["surface"], "side": "top"}})");
        const QString steps = QStringLiteral(R"({"type": "sweep", "parameters": [["Vg", "-0.9, -0.6, -0.3, 0", "V"]]}, {"type": "stationary"},
          {"type": "frequency", "frequencies": "1[MHz]"})");
        QStringList said;
        double reach[2] = {0, 0};
        for (const bool with : {false, true}) {
            const Run r = run(modelOf(mosCapacitor(with ? traps : QString(), steps)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QCOMPARE(int(r.study.solutions.size()), 4);
            for (std::size_t k = 0; k < r.study.solutions.size(); ++k) {
                Solution& s = *r.study.solutions[k];
                s.select(0);
                const double C0 = global(s, QStringLiteral("semi.C0gg")) / 0.1e-6;   // F/m²
                s.select(-1);
                const Locator locator(s.mesh());
                auto at = [&](double y) {
                    const std::optional<double> v = evaluateAt(s, locator, QStringLiteral("semi.V"), QPointF(0.05, y));
                    return v ? *v : NAN;
                };
                const double vg = std::vector<double>{-0.9, -0.6, -0.3, 0}[k];
                const double psi = at(-1e-7) - at(-0.399);
                const double Qit = with ? -Q * Dit * Q * (psi - phiB) : 0.0;
                const double expected = vfb + psi - (Qs(psi) + Qit) / cox;
                // Quasi-statically: Cox in series with Cs + Cit, Cs = -dQs/dψs,
                // Cit = q² Dit.
                const double Cs = -(Qs(psi + 1e-4) - Qs(psi - 1e-4)) / 2e-4, Cit = with ? Q * Q * Dit : 0.0;
                const double Clf = 1 / (1 / cox + 1 / (Cs + Cit));
                said << QStringLiteral("%1 at %2 V: ψs %3 V, the gate's %4 V by it; C0 %5 of Cox (%6)")
                            .arg(with ? QStringLiteral("traps") : QStringLiteral("none"))
                            .arg(vg)
                            .arg(psi, 0, 'f', 4)
                            .arg(expected, 0, 'f', 4)
                            .arg(C0 / cox, 0, 'f', 4)
                            .arg(Clf / cox, 0, 'f', 4);
                QVERIFY2(std::abs(expected - vg) < 2e-3, qPrintable(said.join(QLatin1String("; "))));
                QVERIFY2(std::abs(C0 / Clf - 1) < 5e-3, qPrintable(said.join(QLatin1String("; "))));
                if (k == 0) reach[with] -= psi;
                if (k == 3) reach[with] += psi;
            }
        }
        said << QStringLiteral("ψs from -0.9 to 0 V: %1 V without, %2 V with").arg(reach[0], 0, 'f', 3).arg(reach[1], 0, 'f', 3);
        qInfo("stretch-out: %s", qPrintable(said.join(QLatin1String("; "))));
        QVERIFY(reach[1] < 0.95 * reach[0]);
    }

    void interfaceTrapsConductance()
    {
        // The conductance method: one level at the intrinsic one, 1e11 cm⁻²,
        // σ 1e-16 cm², vth 1e7 cm/s, the MOS capacitor depleted at -0.5 V.
        // Gp/ω from the gate's admittance - ω Cox² G/(G² + ω² (Cox - C)²) -
        // the level's Cit ωτ/(1 + ω²τ²) at a tenth, a third, once, three and
        // ten times 1/2πτ: τ = 1/(cn (n + n1) + cp (p + p1)) and Cit = q N
        // (cn (1 - f) n + cp f p)/(Vt/τ) at the surface's carriers - to 0.5 %;
        // the substrate's admittance the gate's (the traps' charge in it); at
        // 1e-4 of it the quasi-static capacitance.
        constexpr double KB = 1.380649e-23;
        const double Vt = KB * 300 / Q, ni = std::sqrt(2.8e25 * 1.04e25) * std::exp(-1.12 / (2 * Vt));
        const double W = 0.1e-6, cox = 3.9 * Eps0 / 10e-9 * W, N = 1e15, c = 1e-20 * 1e5;
        const QString trap = QStringLiteral(R"(, {"type": "traps", "kind": "acceptor", "distribution": "level", "Nit": "1e11[1/cm^2]", "Et": "0[eV]",
            "sigman": "1e-16[cm^2]", "sigmap": "1e-16[cm^2]", "vth": "1e7[cm/s]", "selection": {"of": ["surface"], "side": "top"}})");
        // First its surface at rest, for τ; then the admittance about 1/2πτ.
        const Run rest = run(modelOf(mosCapacitor(trap, QStringLiteral(R"({"type": "stationary"})")).replace(R"("expression": "0[V]")", R"("expression": "-0.5[V]")")));
        QVERIFY2(rest.error.isEmpty(), qPrintable(rest.error));
        const Locator locator(rest.solution->mesh());
        auto at = [&](const QString& e) {
            const std::optional<double> v = evaluateAt(*rest.solution, locator, e, QPointF(0.05, -1e-7));
            return v ? *v : NAN;
        };
        const double n = at(QStringLiteral("semi.n")), p = at(QStringLiteral("semi.p"));
        const double D = c * (n + ni) + c * (p + ni), f = (c * n + c * ni) / D;
        const double tau = 1 / D, Cit = Q * N * (c * (1 - f) * n + c * f * p) / (Vt * D) * W;
        const double f0 = 1 / (2 * M_PI * tau);
        const std::vector<double> times = {1e-4, 0.1, 0.3, 1, 3, 10};
        QStringList list;
        for (double k : times) list << QString::number(k * f0, 'g', 10);
        const QString steps = QStringLiteral(R"({"type": "stationary"}, {"type": "frequency", "frequencies": "%1"})").arg(list.join(QLatin1String(", ")));
        Run r = run(modelOf(mosCapacitor(trap, steps).replace(R"("expression": "0[V]")", R"("expression": "-0.5[V]")")));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        QStringList said;
        said << QStringLiteral("surface n %1, p %2 m⁻³: τ %3 s (1/2πτ %4 Hz), Cit %5 F").arg(n, 0, 'g', 4).arg(p, 0, 'g', 4).arg(tau, 0, 'g', 4).arg(f0, 0, 'g', 4).arg(Cit, 0, 'g', 4);
        // (At 1e-4 of 1/2πτ the traps follow: C the quasi-static C0, to 1e-4.)
        s.select(0);
        said << QStringLiteral("C %1, C0 %2 F at %3 Hz").arg(global(s, QStringLiteral("semi.Cgg")), 0, 'g', 7).arg(global(s, QStringLiteral("semi.C0gg")), 0, 'g', 7).arg(s.snapshotValue(0), 0, 'g', 3);
        QVERIFY2(std::abs(global(s, QStringLiteral("semi.Cgg")) / global(s, QStringLiteral("semi.C0gg")) - 1) < 1e-4, qPrintable(said.join(QLatin1String("; "))));
        for (int k = 1; k < s.snapshotCount(); ++k) {
            s.select(k);
            const double w = 2 * M_PI * s.snapshotValue(k), wt = w * tau;
            const double C = global(s, QStringLiteral("semi.Cgg")), G = global(s, QStringLiteral("semi.Ggg"));
            const double Gp = w * cox * cox * G / (G * G + w * w * (cox - C) * (cox - C));
            const double expected = Cit * wt / (1 + wt * wt);
            said << QStringLiteral("%1 Hz: Gp/ω %2 F (%3)").arg(s.snapshotValue(k), 0, 'g', 4).arg(Gp, 0, 'g', 5).arg(expected, 0, 'g', 5);
            QVERIFY2(std::abs(Gp / expected - 1) < 5e-3, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(std::abs(global(s, QStringLiteral("semi.Cbg")) / C + 1) < 1e-6 && std::abs(global(s, QStringLiteral("semi.Gbg")) / G + 1) < 1e-6,
                     qPrintable(said.join(QLatin1String("; "))));
        }
        qInfo("conductance method: %s", qPrintable(said.join(QLatin1String("; "))));
    }

    void interfaceTrapsGenerate()
    {
        // A pn junction along a surface (1e17 cm⁻³ each side, 1 µm each),
        // under oxide, its surface's traps one level at the intrinsic one
        // (1e11 cm⁻², σ 1e-16 cm², vth 1e7 cm/s), none in its bulk; reversed
        // by 1 V. Its current all the traps' generation: q ∫ U along the
        // surface, U = N cn cp (n p - ni²)/(cn (n + n1) + cp (p + p1)) at the
        // carriers solved - to 1 %.
        constexpr double KB = 1.380649e-23;
        const double Vt = KB * 300 / Q, ni = std::sqrt(2.8e25 * 1.04e25) * std::exp(-1.12 / (2 * Vt)), N = 1e15, c = 1e-15;
        const Run r = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "p", "position": ["0", "-0.1"], "size": ["1", "0.1"]},
                         {"type": "rectangle", "name": "n", "position": ["1", "-0.1"], "size": ["1", "0.1"]},
                         {"type": "rectangle", "name": "oxide", "size": ["2", "0.01"]}],
            "materials": [{"material": "Silicon", "selection": {"only": ["p", "n"]}},
                          {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "features": [
              {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["p"]}},
              {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"only": ["n"]}},
              {"type": "traps", "kind": "acceptor", "distribution": "level", "Nit": "1e11[1/cm^2]", "Et": "0[eV]", "sigman": "1e-16[cm^2]",
               "sigmap": "1e-16[cm^2]", "vth": "1e7[cm/s]", "selection": {"of": ["p", "n"], "side": "top"}},
              {"type": "ohmic", "name": "a", "V0": "-1[V]", "selection": {"of": ["p"], "side": "left"}},
              {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "right"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.02"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                                      .toUtf8()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const Locator locator(s.mesh());
        auto at = [&](const QString& e, double x) {
            const std::optional<double> v = evaluateAt(s, locator, e, QPointF(x, -1e-7));
            return v ? *v : NAN;
        };
        // ∫ U dx along the surface, Simpson's at 5 nm.
        const int m = 400;
        double sum = 0;
        for (int k = 0; k <= m; ++k) {
            const double x = 2.0 * k / m, w = (k == 0 || k == m) ? 1 : (k % 2 ? 4 : 2);
            const double n = at(QStringLiteral("semi.n"), std::clamp(x, 1e-6, 2 - 1e-6)), p = at(QStringLiteral("semi.p"), std::clamp(x, 1e-6, 2 - 1e-6));
            sum += w * N * c * c * (n * p - ni * ni) / (c * (n + ni) + c * (p + ni));
        }
        const double generated = Q * sum * (2e-6 / m) / 3;   // A (a metre deep)
        const double Ia = global(s, QStringLiteral("semi.I_a")), Ik = global(s, QStringLiteral("semi.I_k"));
        qInfo("surface generation: I_a %.5g A, I_k %.5g A; q ∫ U along the surface %.5g A", Ia, Ik, generated);
        QVERIFY2(generated < 0 && std::abs(Ik / -generated - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(Ik).arg(generated)));
        QVERIFY(std::abs(Ia + Ik) < 1e-9 * std::abs(Ik));
    }

    void schrodingerPoisson()
    {
        // The MOS capacitor in strong inversion (2 V), its top 20 nm's
        // electrons a Schrödinger equation's (one mass, 0.916 m0; ψ nought at
        // the oxide and 20 nm down; its line's elements 0.1 nm, a sanitized
        // build's 0.2), the two solved in turn to rest:
        // - the semiconductor's electrons the states' (qw.n) to 1e-3;
        // - none at the oxide (ψ nought there), where the classical ones are
        //   most;
        // - their centroid Fang and Howard's 3/b, b = (12 m q² N*/(εs ħ²))^⅓,
        //   N* = Ndepl + 11/32 Ninv - to 15 % (theirs a variational ground
        //   state's);
        // - fewer of them than classically at the same gate voltage (the
        //   centroid's distance a capacitance in series).
        constexpr double Hbar = 1.054571817e-34, M0 = 9.1093837015e-31;
        const double Na = 1e23, es = 11.7 * Eps0, m = 0.916 * M0;
        const QString semi = QStringLiteral(R"(, {"type": "quantum", "electrons": true, "selection": {"only": ["surface"]}})");
        const QString qw = QStringLiteral(R"(,
          {"type": "quantum", "tag": "qw", "space": "1d", "axis": "y", "at": "0.005[um]", "h1d": "%1", "electrons": true, "heavy": false, "light": false,
           "V": "semi.V", "etan": "semi.Efn - semi.Ec", "selection": {"only": ["surface"]}, "features": [
            {"type": "material", "source": "user", "Ec": "0[eV]", "Ev": "-1.12[eV]", "me": "0.916", "selection": {"all": true}}]})")
                               .arg(slowBuild() ? QStringLiteral("0.2[nm]") : QStringLiteral("0.1[nm]"));
        QStringList said;
        double classical = NAN, quantum = NAN;
        for (const bool sp : {false, true}) {
            // (Narrow - it is one-dimensional -, its top meshed at 0.5 nm: the
            // states' carriers change over a nanometre at the oxide.)
            QByteArray json = mosCapacitor(sp ? semi : QString(),
                                           sp ? QStringLiteral(R"({"type": "states", "states": 6, "consistent": true})") : QStringLiteral(R"({"type": "stationary"})"),
                                           QStringLiteral("0.01"), QStringLiteral("0.0005"));
            json.replace(R"("expression": "0[V]")", R"("expression": "2[V]")");
            // (The quantum wells physics after the semiconductor's.)
            if (sp) json.replace(QByteArray("]}],\n        \"mesh\""), QByteArray("]}") + qw.toUtf8() + QByteArray("],\n        \"mesh\""));
            const Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.study.solutions.front();
            if (sp) {
                int turns = 0;
                static const QRegularExpression in(QStringLiteral("Schrödinger-Poisson in (\\d+) turns"));
                for (const QString& line : s.log)
                    if (const QRegularExpressionMatch mt = in.match(line); mt.hasMatch()) {
                        turns = mt.captured(1).toInt();
                        said << line;
                    }
                QVERIFY2(turns > 0 && turns <= 20, qPrintable(s.log.join(QLatin1String("; "))));
            }
            const Locator locator(s.mesh());
            auto at = [&](const QString& e, double y) {
                const std::optional<double> v = evaluateAt(s, locator, e, QPointF(0.005, y));
                return v ? *v : NAN;
            };
            // The semiconductor's electrons the states' at its nodes in the
            // region (each to 1e-3, above a thousandth of the most: between
            // them each is interpolated its own way).
            if (sp) {
                double most = 0;
                std::vector<std::pair<QPointF, double>> nodes;
                for (const QPointF& p : s.mesh().nodes)
                    if (p.y() < -1e-9 && p.y() > -0.02 + 1e-9) {
                        const std::optional<double> n = evaluateAt(s, locator, QStringLiteral("semi.n"), p);
                        if (n) {
                            nodes.emplace_back(p, *n);
                            most = std::max(most, *n);
                        }
                    }
                int compared = 0;
                for (const auto& [p, n] : nodes) {
                    if (n < 1e-3 * most) continue;
                    const double states = evaluateAt(s, locator, QStringLiteral("qw.n"), p).value_or(NAN);
                    QVERIFY2(std::abs(n / states - 1) < 1e-3, qPrintable(QStringLiteral("at (%1, %2): semi.n %3, qw.n %4").arg(p.x()).arg(p.y()).arg(n).arg(states)));
                    ++compared;
                }
                QVERIFY(compared > 20);
                // At rest its quasi-Fermi level flat through the region, the
                // quantum potential in its band (what the states read).
                const double top = evaluateAt(s, locator, QStringLiteral("semi.Efn"), QPointF(0.005, -0.001)).value_or(NAN);
                const double down = evaluateAt(s, locator, QStringLiteral("semi.Efn"), QPointF(0.005, -0.015)).value_or(NAN);
                QVERIFY2(std::abs(top - down) < 1e-6 * Q, qPrintable(QStringLiteral("Efn %1 and %2 eV").arg(top / Q).arg(down / Q)));
            }
            // Across the top 20 nm: the electrons (the states', smooth; the
            // classical ones), their centroid; the depletion's charge.
            const int k = slowBuild() ? 100 : 400;
            double ninv = 0, moment = 0, peak = 0, Qd = 0;
            for (int j = 0; j < k; ++j) {
                const double y = -(j + 0.5) * 0.02 / k, n = at(sp ? QStringLiteral("qw.n") : QStringLiteral("semi.n"), y);
                ninv += n * 0.02e-6 / k;
                moment += n * -y * 1e-6 * 0.02e-6 / k;
                peak = std::max(peak, n);
            }
            for (int j = 0; j < k; ++j) Qd += (Na - at(QStringLiteral("semi.p"), -(j + 0.5) * 0.3 / k)) * 0.3e-6 / k;
            const double zavg = moment / ninv, surface = at(sp ? QStringLiteral("qw.n") : QStringLiteral("semi.n"), -1e-5);
            const double Nstar = Qd + 11.0 / 32 * ninv, b = std::cbrt(12 * m * Q * Q * Nstar / (es * Hbar * Hbar));
            said << QStringLiteral("%1: Ninv %2 cm⁻², its centroid %3 nm (Fang and Howard's %4), at the oxide %5 of its most")
                        .arg(sp ? QStringLiteral("Schrödinger-Poisson") : QStringLiteral("classical"))
                        .arg(ninv * 1e-4, 0, 'g', 4)
                        .arg(zavg * 1e9, 0, 'f', 3)
                        .arg(3 / b * 1e9, 0, 'f', 3)
                        .arg(surface / peak, 0, 'g', 3);
            if (sp) {
                QVERIFY2(surface < 0.05 * peak, qPrintable(said.join(QLatin1String("; "))));
                QVERIFY2(std::abs(zavg / (3 / b) - 1) < 0.15, qPrintable(said.join(QLatin1String("; "))));
                quantum = ninv;
            } else {
                QVERIFY2(surface > 0.99 * peak, qPrintable(said.join(QLatin1String("; "))));
                classical = ninv;
            }
        }
        // 0.1 µm wide, meshed at 2 nm, the plain turns crawled at 0.96 a turn
        // (past 60): Anderson's mixing brings it to rest in 30. (Not on a
        // sanitized build: its 26 turns some 200 s there.)
        if (!slowBuild()) {
            QByteArray json = mosCapacitor(semi, QStringLiteral(R"({"type": "states", "states": 6, "consistent": true, "iterations": 30})"),
                                           QStringLiteral("0.1"), QStringLiteral("0.002"));
            json.replace(R"("expression": "0[V]")", R"("expression": "2[V]")");
            QString wide = qw;
            wide.replace(QStringLiteral(R"("at": "0.005[um]")"), QStringLiteral(R"("at": "0.05[um]")"));
            json.replace(QByteArray("]}],\n        \"mesh\""), QByteArray("]}") + wide.toUtf8() + QByteArray("],\n        \"mesh\""));
            const Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            bool rested = false;
            for (const QString& line : r.study.solutions.front()->log)
                if (line.contains(QStringLiteral("Schrödinger-Poisson in"))) {
                    rested = true;
                    said << QStringLiteral("at 2 nm: ") + line;
                }
            QVERIFY2(rested, qPrintable(r.study.warnings.join(QLatin1String("; "))));
        }
        qInfo("Schrödinger-Poisson: %s", qPrintable(said.join(QLatin1String("; "))));
        QVERIFY(quantum < classical);
    }

    void schrodingerPoissonValleys()
    {
        // The same MOS capacitor at 2 V, its electrons silicon's on (100):
        // the two valleys 0.916 m0 along the axis (0.190 in the plane) and
        // the four 0.190 (0.417) - two ladders, solved in turn with the
        // semiconductor to rest:
        // - its electrons the states' at its nodes to 1e-3, the two sets'
        //   electrons per area their sum;
        // - each set's centroid Fang and Howard's 3/b of its own mass along
        //   the axis (the four valleys' lighter: farther out), and the
        //   layer's their mean by the sets' electrons, to 15 %;
        // - farther out than one valley of 0.916 makes it (the earlier
        //   test's) - by 10 % or more.
        constexpr double Hbar = 1.054571817e-34, M0 = 9.1093837015e-31;
        const double Na = 1e23, es = 11.7 * Eps0;
        const QString semi = QStringLiteral(R"(, {"type": "quantum", "electrons": true, "selection": {"only": ["surface"]}})");
        QStringList said;
        double centroid[2] = {NAN, NAN};
        for (const bool valleys : {false, true}) {
            const QString qw = QStringLiteral(R"(,
              {"type": "quantum", "tag": "qw", "space": "1d", "axis": "y", "at": "0.005[um]", "h1d": "%1", "electrons": true, "heavy": false, "light": false,
               "V": "semi.V", "etan": "semi.Efn - semi.Ec", "selection": {"only": ["surface"]}, "features": [
                {"type": "material", "source": "user", "Ec": "0[eV]", "Ev": "-1.12[eV]", "me": "0.916", "valleys": "%2", "selection": {"all": true}}]})")
                                   .arg(slowBuild() ? QStringLiteral("0.2[nm]") : QStringLiteral("0.1[nm]"), valleys ? QStringLiteral("Si(100)") : QString());
            QByteArray json = mosCapacitor(semi, QStringLiteral(R"({"type": "states", "states": 6, "consistent": true})"), QStringLiteral("0.01"),
                                           QStringLiteral("0.0005"));
            json.replace(R"("expression": "0[V]")", R"("expression": "2[V]")");
            json.replace(QByteArray("]}],\n        \"mesh\""), QByteArray("]}") + qw.toUtf8() + QByteArray("],\n        \"mesh\""));
            const Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.study.solutions.front();
            QVERIFY2(s.log.join(QLatin1Char(' ')).contains(QStringLiteral("Schrödinger-Poisson in")), qPrintable(s.log.join(QLatin1String("; "))));
            const Locator locator(s.mesh());
            auto at = [&](const QString& e, double y) {
                const std::optional<double> v = evaluateAt(s, locator, e, QPointF(0.005, y));
                return v ? *v : NAN;
            };
            double most = 0;
            std::vector<std::pair<QPointF, double>> nodes;
            for (const QPointF& p : s.mesh().nodes)
                if (p.y() < -1e-9 && p.y() > -0.02 + 1e-9)
                    if (const std::optional<double> n = evaluateAt(s, locator, QStringLiteral("semi.n"), p)) {
                        nodes.emplace_back(p, *n);
                        most = std::max(most, *n);
                    }
            for (const auto& [p, n] : nodes) {
                if (n < 1e-3 * most) continue;
                const double states = evaluateAt(s, locator, QStringLiteral("qw.n"), p).value_or(NAN);
                QVERIFY2(std::abs(n / states - 1) < 1e-3, qPrintable(QStringLiteral("at (%1, %2): semi.n %3, qw.n %4").arg(p.x()).arg(p.y()).arg(n).arg(states)));
            }
            const int k = slowBuild() ? 100 : 400;
            double ninv = 0, moment = 0, Qd = 0;
            for (int j = 0; j < k; ++j) {
                const double y = -(j + 0.5) * 0.02 / k, n = at(QStringLiteral("qw.n"), y);
                ninv += n * 0.02e-6 / k;
                moment += n * -y * 1e-6 * 0.02e-6 / k;
            }
            for (int j = 0; j < k; ++j) Qd += (Na - at(QStringLiteral("semi.p"), -(j + 0.5) * 0.3 / k)) * 0.3e-6 / k;
            centroid[valleys] = moment / ninv;
            const double Nstar = Qd + 11.0 / 32 * ninv;
            auto fangHoward = [&](double mz) { return 3 / std::cbrt(12 * mz * M0 * Q * Q * Nstar / (es * Hbar * Hbar)); };
            s.select(0);
            const double Ns = s.global(QStringLiteral("qw.Ns")).value_or(NAN);
            if (!valleys) {
                said << QStringLiteral("one valley: Ninv %1 cm⁻² (qw.Ns %2), its centroid %3 nm").arg(ninv * 1e-4, 0, 'g', 4).arg(Ns * 1e-4, 0, 'g', 4).arg(centroid[0] * 1e9, 0, 'f', 3);
                continue;
            }
            const double N1 = s.global(QStringLiteral("qw.Nv1")).value_or(NAN), N2 = s.global(QStringLiteral("qw.Nv2")).value_or(NAN);
            const double expected = (N1 * fangHoward(0.916) + N2 * fangHoward(0.190)) / (N1 + N2);
            said << QStringLiteral("Si(100): Ninv %1 cm⁻² (qw.Ns %2), %3 % in the two valleys, %4 % in the four; its centroid %5 nm (Fang and Howard's "
                                   "%6 and %7 by the sets' electrons: %8)")
                        .arg(ninv * 1e-4, 0, 'g', 4)
                        .arg(Ns * 1e-4, 0, 'g', 4)
                        .arg(100 * N1 / Ns, 0, 'f', 1)
                        .arg(100 * N2 / Ns, 0, 'f', 1)
                        .arg(centroid[1] * 1e9, 0, 'f', 3)
                        .arg(fangHoward(0.916) * 1e9, 0, 'f', 3)
                        .arg(fangHoward(0.190) * 1e9, 0, 'f', 3)
                        .arg(expected * 1e9, 0, 'f', 3);
            QVERIFY2(std::abs((N1 + N2) / Ns - 1) < 1e-12, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(std::abs(Ns / ninv - 1) < 0.02, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(N1 > N2 && N2 > 0.05 * Ns, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(std::abs(centroid[1] / expected - 1) < 0.15, qPrintable(said.join(QLatin1String("; "))));
        }
        qInfo("Schrödinger-Poisson in valleys: %s", qPrintable(said.join(QLatin1String("; "))));
        QVERIFY2(centroid[1] > 1.1 * centroid[0], qPrintable(said.join(QLatin1String("; "))));
    }

    void schrodingerPoissonHoles()
    {
        // n-type silicon (1e17 cm⁻³) under the gate at -2.5 V: holes
        // inverted, its top 20 nm's heavy and light holes (0.49 and 0.16 m0)
        // a Schrödinger equation's, solved in turn with it to rest - its
        // holes the states' at its nodes to 1e-3, none at the oxide, fewer
        // than classically.
        const QString semi = QStringLiteral(R"(, {"type": "quantum", "electrons": false, "holes": true, "selection": {"only": ["surface"]}})");
        const QString qw = QStringLiteral(R"(,
          {"type": "quantum", "tag": "qw", "space": "1d", "axis": "y", "at": "0.005[um]", "h1d": "%1", "electrons": false, "heavy": true, "light": true,
           "V": "semi.V", "etap": "semi.Ev - semi.Efp", "selection": {"only": ["surface"]}, "features": [
            {"type": "material", "source": "user", "Ec": "1.12[eV]", "Ev": "0[eV]", "me": "0.26", "mhh": "0.49", "mlh": "0.16", "selection": {"all": true}}]})")
                               .arg(slowBuild() ? QStringLiteral("0.2[nm]") : QStringLiteral("0.1[nm]"));
        QStringList said;
        double sheet[2] = {NAN, NAN};
        for (const bool sp : {false, true}) {
            // (A sanitized build: meshed at 1 nm, 6 states of each.)
            const bool slow = slowBuild();
            QByteArray json = mosCapacitor(sp ? semi : QString(),
                                           sp ? QStringLiteral(R"({"type": "states", "states": %1, "consistent": true})").arg(slow ? 6 : 8) : QStringLiteral(R"({"type": "stationary"})"),
                                           QStringLiteral("0.01"), slow ? QStringLiteral("0.001") : QStringLiteral("0.0005"));
            json.replace(R"("expression": "0[V]")", R"("expression": "-2.5[V]")");
            json.replace(R"("dopant": "acceptor")", R"("dopant": "donor")");
            if (sp) json.replace(QByteArray("]}],\n        \"mesh\""), QByteArray("]}") + qw.toUtf8() + QByteArray("],\n        \"mesh\""));
            const Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.study.solutions.front();
            const Locator locator(s.mesh());
            auto at = [&](const QString& e, double y) {
                const std::optional<double> v = evaluateAt(s, locator, e, QPointF(0.005, y));
                return v ? *v : NAN;
            };
            if (sp) {
                bool rested = false;
                for (const QString& line : s.log) rested = rested || line.contains(QStringLiteral("Schrödinger-Poisson in"));
                QVERIFY2(rested, qPrintable(s.log.join(QLatin1String("; "))));
                double most = 0;
                std::vector<std::pair<QPointF, double>> nodes;
                for (const QPointF& p : s.mesh().nodes)
                    if (p.y() < -1e-9 && p.y() > -0.02 + 1e-9)
                        if (const std::optional<double> h = evaluateAt(s, locator, QStringLiteral("semi.p"), p)) {
                            nodes.emplace_back(p, *h);
                            most = std::max(most, *h);
                        }
                int compared = 0;
                for (const auto& [p, h] : nodes) {
                    if (h < 1e-3 * most) continue;
                    const double states = evaluateAt(s, locator, QStringLiteral("qw.p"), p).value_or(NAN);
                    QVERIFY2(std::abs(h / states - 1) < 1e-3, qPrintable(QStringLiteral("at (%1, %2): semi.p %3, qw.p %4").arg(p.x()).arg(p.y()).arg(h).arg(states)));
                    ++compared;
                }
                QVERIFY(compared > 20);
            }
            double total = 0, peak = 0;
            const int k = slowBuild() ? 100 : 400;
            for (int j = 0; j < k; ++j) {
                const double h = at(sp ? QStringLiteral("qw.p") : QStringLiteral("semi.p"), -(j + 0.5) * 0.02 / k);
                total += h * 0.02e-6 / k;
                peak = std::max(peak, h);
            }
            const double surface = at(sp ? QStringLiteral("qw.p") : QStringLiteral("semi.p"), -1e-5);
            said << QStringLiteral("%1: %2 holes /cm², at the oxide %3 of their most").arg(sp ? QStringLiteral("Schrödinger-Poisson") : QStringLiteral("classical")).arg(total * 1e-4, 0, 'g', 4).arg(surface / peak, 0, 'g', 3);
            if (sp) {
                QVERIFY2(surface < 0.05 * peak, qPrintable(said.join(QLatin1String("; "))));
                // The wells' holes per area (qw.Ps, along its line) the same, to
                // 1 % (qw.p's interpolation on the mesh).
                s.select(0);
                const double Ps = global(s, QStringLiteral("qw.Ps"));
                said << QStringLiteral("qw.Ps %1 /cm²").arg(Ps * 1e-4, 0, 'g', 4);
                QVERIFY2(std::abs(Ps / total - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
            }
            sheet[sp] = total;
        }
        qInfo("Schrödinger-Poisson, holes: %s", qPrintable(said.join(QLatin1String("; "))));
        QVERIFY(sheet[1] < sheet[0] && sheet[1] > 0.5 * sheet[0]);
    }

    void theCarriersHeat()
    {
        // A pn junction (p 1e18 cm⁻³, n 1e16, 1 µm each, mobilities by doping,
        // lifetimes 10 ns) forward at 0.6 V: the heat its carriers leave, ∫
        // semi.H - their Joule heat, each edge's currents through their
        // quasi-Fermi potentials' drop, and the pairs' q R (φp - φn) -, the
        // power it takes, I V, to 1e-3.
        // (A sanitized build's mesh 20 nm, not 10.)
        const Run r = run(modelOf(diodeHeat(QStringLiteral("0.6[V]"), slowBuild() ? QStringLiteral("0.02") : QStringLiteral("0.01"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        Node heat;
        heat.type = QStringLiteral("integral");
        heat.props.insert(QStringLiteral("expression"), QStringLiteral("semi.H*1[m]"));
        heat.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
        const DerivedResult d = evaluateDerived(s, heat);
        QVERIFY2(!d.values.isEmpty(), qPrintable(d.error));
        const double H = d.values.front().value, IV = global(s, QStringLiteral("semi.I_a")) * 0.6;
        qInfo("the carriers' heat %.6g W, I V %.6g W", H, IV);
        QVERIFY2(IV > 0 && std::abs(H / IV - 1) < 1e-3, qPrintable(QStringLiteral("%1 %2").arg(H).arg(IV)));
    }

    void selfHeating()
    {
        // A bar of n-type silicon (1e17 cm⁻³), 20 µm by 1 µm, 4 V across it,
        // its ends held at 300 K; its heat (semi.H) a heat physics' source,
        // its mobility at that one's temperature (TL = ht.T: (T/300 K)^-2.5)
        // - the two solved in turn. Its current the one-dimensional
        // problem's, -k T'' = J² ρ(T), ρ = ρ300 (T/300 K)^2.5, V = J ∫ρ dx -
        // to 0.5 %; some 15 % below the bar's at 300 K.
        const double L = 20e-6, A = 1e-6, V = 4, rho0 = 1 / (Q * 1e23 * 0.14);
        double peak = NAN;
        const double expected = selfHeated(L, V, &peak) * A, cold = V / (rho0 * L) * A;
        const Run r = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "left", "size": ["10", "1"]},
                         {"type": "rectangle", "name": "right", "position": ["10", "0"], "size": ["10", "1"]}],
            "materials": [{"material": "Silicon", "selection": {"all": true}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "TL": "ht.T", "features": [
              {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
              {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["left"], "side": "left"}},
              {"type": "ohmic", "name": "2", "V0": "4[V]", "selection": {"of": ["right"], "side": "right"}}]},
             {"type": "heat", "tag": "ht", "features": [
              {"type": "source", "Q0": "semi.H", "selection": {"all": true}},
              {"type": "init", "T0": "300[K]", "selection": {"all": true}},
              {"type": "temperature", "T0": "300[K]", "selection": {"of": ["left"], "side": "left"}},
              {"type": "temperature", "T0": "300[K]", "selection": {"of": ["right"], "side": "right"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.25"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                                      .toUtf8()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double I = std::abs(global(*r.solution, QStringLiteral("semi.I_2")));
        const double Tmid = at1(*r.solution, QStringLiteral("ht.T"), 10, 0.5);
        qInfo("self-heating: %.6g A (the one-dimensional problem's %.6g, at 300 K %.6g); %.2f K at its middle (%.2f)", I, expected, cold, Tmid, peak);
        QVERIFY2(std::abs(I / expected - 1) < 5e-3, qPrintable(QStringLiteral("%1 %2").arg(I).arg(expected)));
        QVERIFY2(std::abs(Tmid - peak) < 0.01 * (peak - 300), qPrintable(QStringLiteral("%1 %2").arg(Tmid).arg(peak)));
    }

    void latticeTemperatureOneWay()
    {
        // The bar at 0.1 V, warmed by a heat source of its own (1e14 W/m³,
        // its ends at 300 K: T = 300 K + Q x (L - x)/2k), its mobility at
        // that temperature - the semiconductor reading the heat physics,
        // not read by it: solved after it all the same. Its current V/∫ρ,
        // ρ = ρ300 (T/300 K)^2.5, to 0.5 %.
        const double L = 20e-6, A = 1e-6, k = 130, n = 1e23, mu = 0.14, V = 0.1, Qh = 1e14;
        const double rho0 = 1 / (Q * n * mu);
        double sum = 0;
        const int m = 4000;
        for (int i = 0; i < m; ++i) {
            const double x = (i + 0.5) * L / m, T = 300 + Qh * x * (L - x) / (2 * k);
            sum += rho0 * std::pow(T / 300, 2.5) * L / m;
        }
        const double expected = V / sum * A;
        const Run r = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "left", "size": ["10", "1"]},
                         {"type": "rectangle", "name": "right", "position": ["10", "0"], "size": ["10", "1"]}],
            "materials": [{"material": "Silicon", "selection": {"all": true}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "TL": "ht.T", "features": [
              {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
              {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["left"], "side": "left"}},
              {"type": "ohmic", "name": "2", "V0": "0.1[V]", "selection": {"of": ["right"], "side": "right"}}]},
             {"type": "heat", "tag": "ht", "features": [
              {"type": "source", "Q0": "1e14[W/m^3]", "selection": {"all": true}},
              {"type": "init", "T0": "300[K]", "selection": {"all": true}},
              {"type": "temperature", "T0": "300[K]", "selection": {"of": ["left"], "side": "left"}},
              {"type": "temperature", "T0": "300[K]", "selection": {"of": ["right"], "side": "right"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.25"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                                      .toUtf8()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double I = std::abs(global(*r.solution, QStringLiteral("semi.I_2")));
        qInfo("one way: %.6g A (∫ρ's %.6g; at 300 K %.6g)", I, expected, V / (rho0 * L) * A);
        QVERIFY2(std::abs(I / expected - 1) < 5e-3, qPrintable(QStringLiteral("%1 %2").arg(I).arg(expected)));
    }

    void bandToBandTunnelling()
    {
        // A p⁺-i-n⁺ diode (1e20 cm⁻³, 50 nm of 1e15, 1e20; meshed at 2 nm, a
        // sanitized build's 4), reversed by 2 and
        // 2.3 V, its pairs made by band-to-band tunnelling alone (silicon's:
        // A 4e14 cm^-1/2 V^-5/2 s^-1, B 1.9e7 V/cm, γ 2.5). Generation alone:
        // - its current q Σ A E^γ e^(-B/E) over its triangles, each at its
        //   field (0.63 MV/cm through its i layer, a little more at its edges:
        //   24 % more current than the i layer's middle's alone), to 1 %;
        // Hurkx's:
        // - its current q ∫ G, G = -semi.R, to 1 %; 0.64 of generation
        //   alone's (the junctions' carriers spilling some 10 nm into the
        //   field: there (n p - ni²)/((n + ni)(p + ni)) about -ni/p);
        // - none at 0 V; without the feature only the diode's own (its
        //   contacts' minority carriers diffusing, 7e-17 A), under a
        //   hundredth of it.
        const double A = 4e14 * 1e6 * std::pow(1e-2, 2.5), B = 1.9e9, gamma = 2.5, W = 50e-9;
        auto G = [&](double E) { return A * std::pow(E, gamma) * std::exp(-B / E); };
        auto pin = [&](const QString& kind) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "parameters": [{"name": "Va", "expression": "0[V]"}],
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "name": "p", "position": ["-100", "0"], "size": ["100", "20"]},
                             {"type": "rectangle", "name": "i", "size": ["50", "20"]},
                             {"type": "rectangle", "name": "n", "position": ["50", "0"], "size": ["100", "20"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "acceptor", "N0": "1e20[1/cm^3]", "selection": {"only": ["p"]}},
                  {"type": "doping", "dopant": "donor", "N0": "1e15[1/cm^3]", "selection": {"only": ["i"]}},
                  {"type": "doping", "dopant": "donor", "N0": "1e20[1/cm^3]", "selection": {"only": ["n"]}} %1,
                  {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["p"], "side": "left"}},
                  {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
              "studies": [{"type": "study", "steps": [{"type": "sweep", "parameters": [["Va", "0, -2, -2.3", "V"]]}, {"type": "stationary"}]}]})json")
                .arg(kind.isEmpty() ? QString() : QStringLiteral(R"(, {"type": "btbt", "kind": "%1", "selection": {"all": true}})").arg(kind))
                .arg(slowBuild() ? QStringLiteral("4") : QStringLiteral("2"))
                .toUtf8();
        };
        QStringList said;
        double alone[3] = {0, 0, 0};
        for (const QString& kind : {QStringLiteral("generation"), QStringLiteral("hurkx")}) {
            const bool hurkx = kind == QLatin1String("hurkx");
            const Run r = run(modelOf(pin(kind)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QCOMPARE(int(r.study.solutions.size()), 3);
            double I[3], Emid[3];
            for (int k = 0; k < 3; ++k) {
                Solution& s = *r.study.solutions[std::size_t(k)];
                I[k] = global(s, QStringLiteral("semi.I_k"));
                Emid[k] = at1(s, QStringLiteral("semi.normE"), 25, 10);
                if (k == 0) continue;
                Node gen;
                gen.type = QStringLiteral("integral");
                gen.props.insert(QStringLiteral("expression"), QStringLiteral("-1.602176634e-19[C]*semi.R*1[m]"));
                gen.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
                const DerivedResult d = evaluateDerived(s, gen);
                QVERIFY2(!d.values.isEmpty(), qPrintable(d.error));
                // (Its own G over the triangles, each at its field - the
                // potential's linear in each: the field its own throughout.)
                double along = 0;
                const Locator locator(s.mesh());
                for (const auto& tri : s.mesh().triangles) {
                    if (hurkx) break;
                    const QPointF a = s.mesh().nodes[std::size_t(tri[0])], b = s.mesh().nodes[std::size_t(tri[1])], c = s.mesh().nodes[std::size_t(tri[2])];
                    const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2 * 1e-18;
                    const std::optional<double> E = evaluateAt(s, locator, QStringLiteral("semi.normE"), (a + b + c) / 3.0);
                    if (E) along += G(*E) * area;
                }
                const double qG = d.values.front().value, own = hurkx ? NAN : Q * along, middle = Q * G(Emid[k]) * W * 20e-9;
                said << QStringLiteral("%1 at %2 V: %3 A (q ∫ G %4; its own G by triangle %5; the i layer's middle's, %6 MV/cm, %7)")
                            .arg(kind)
                            .arg(k == 1 ? -2 : -2.3)
                            .arg(I[k], 0, 'g', 5)
                            .arg(qG, 0, 'g', 5)
                            .arg(own, 0, 'g', 5)
                            .arg(Emid[k] / 1e8, 0, 'f', 3)
                            .arg(middle, 0, 'g', 5);
                QVERIFY2(I[k] > 0 && std::abs(I[k] / qG - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
                if (!hurkx) {
                    QVERIFY2(std::abs(I[k] / own - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
                    alone[k] = I[k];
                } else
                    QVERIFY2(I[k] < 0.8 * alone[k] && I[k] > 0.5 * alone[k], qPrintable(said.join(QLatin1String("; "))));
            }
            if (hurkx) QCOMPARE(I[0], 0.0);
        }
        qInfo("band-to-band tunnelling: %s", qPrintable(said.join(QLatin1String("; "))));
        const Run off = run(modelOf(pin(QString())));
        QVERIFY2(off.error.isEmpty(), qPrintable(off.error));
        const double without = std::abs(global(*off.study.solutions.back(), QStringLiteral("semi.I_k")));
        qInfo("without tunnelling at -2.3 V: %.4g A", without);
        QVERIFY(without < 0.01 * alone[2]);
    }

    void fowlerNordheim()
    {
        // The MOS capacitor (10 nm of oxide) at 10 V, its oxide's field some
        // 10 MV/cm: the electrons tunnelling through it, semi.Ifn, J W at
        // that field, J = A E² e^(-B/E), A = q³ m0/(8π h φB m_ox) and B = 8π
        // √(2 m_ox) φB^(3/2)/(3 q h) (φB 3.1 eV, m_ox 0.42: 1.18e-6 A/V²,
        // 242 MV/cm), to 1 % (the field read at one point; J (2 + B/E)
        // times as sensitive) - a few mA/cm² at 10 MV/cm, as measured gate
        // oxides have it.
        constexpr double H = 6.62607015e-34, M0 = 9.1093837015e-31;
        const double phiB = 3.1 * Q, mox = 0.42;
        const double A = Q * Q * Q / (8 * M_PI * H * phiB * mox), B = 8 * M_PI * std::sqrt(2 * mox * M0) * std::pow(phiB, 1.5) / (3 * Q * H);
        QByteArray json = mosCapacitor(QStringLiteral(R"(, {"type": "fn", "selection": {"of": ["surface"], "side": "top"}})"),
                                       QStringLiteral(R"({"type": "stationary"})"));
        json.replace(R"("expression": "0[V]")", R"("expression": "10[V]")");
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double Eox = at1(*r.solution, QStringLiteral("semi.normE"), 0.05, 0.005);
        const double J = A * Eox * Eox * std::exp(-B / Eox), I = global(*r.solution, QStringLiteral("semi.Ifn"));
        qInfo("Fowler-Nordheim: A %.4g A/V², B %.4g MV/cm; the oxide at %.3f MV/cm, %.4g A/cm²: semi.Ifn %.6g A (J W %.6g)", A, B / 1e8, Eox / 1e8,
              J * 1e-4, I, J * 0.1e-6);
        QVERIFY(std::abs(A / 1.18e-6 - 1) < 0.01 && std::abs(B / 2.42e10 - 1) < 0.01);
        QVERIFY2(Eox > 0.8e9 && std::abs(I / (J * 0.1e-6) - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(I).arg(J * 0.1e-6)));
    }

    void implantAndAnneal()
    {
        // Boron implanted, 1e14 cm⁻² at a range of 0.1 µm (straggles 30 nm),
        // into n-type silicon (1e15 cm⁻³); then annealed at 1000 °C for 30
        // minutes (D 1.5e-14 cm²/s, boron's intrinsic: 2 D t's root 74 nm):
        // - down a column (0.02 µm wide, meshed at 5 nm), the Implant
        //   profile's acceptors the formula's - dose/(√(2π) σ) (e^(-(d -
        //   Rp)²/2σ²) + e^(-(d + Rp)²/2σ²)), σ² = ΔRp² + 2 D t -: their sum
        //   the dose to 1e-3, at the range to 1 %, the junction where the
        //   background is to 5 nm;
        // - through a mask's window (0.3 to 0.7 µm) its edges erf's: at an
        //   edge, annealed, half the middle's, to 1 %;
        // - a PDE diffusing the implant as implanted (∂u/∂t = ∇·(D ∇u), no
        //   flux at the surface) for those 30 minutes the annealed formula's,
        //   to 0.5 % - the formula's image a reflecting surface's.
        constexpr double KB_eV = 8.617333262e-5;
        const double dose = 1e18, Rp = 1e-7, dRp = 3e-8, D = 0.76e-4 * std::exp(-3.46 / (KB_eV * 1273.15)), t = 1800;
        const double sd = std::sqrt(dRp * dRp + 2 * D * t);
        auto depth = [&](double d, double sigma) {
            return d < 0 ? 0.0 : dose / (std::sqrt(2 * M_PI) * sigma)
                                   * (std::exp(-(d - Rp) * (d - Rp) / (2 * sigma * sigma)) + std::exp(-(d + Rp) * (d + Rp) / (2 * sigma * sigma)));
        };
        auto across = [&](double x, double sigma) { return 0.5 * (std::erf((x - 0.3e-6) / (std::sqrt(2.0) * sigma)) - std::erf((x - 0.7e-6) / (std::sqrt(2.0) * sigma))); };
        // A block of silicon (µm): \a width wide, \a deep deep, meshed at \a h; the implant annealed for \a time, through \a window.
        auto implant = [&](const QString& width, const QString& deep, const QString& h, const QString& time, const QString& window) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "si", "position": ["0", "-%5"], "size": ["%4", "%5"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e15[1/cm^3]", "selection": {"all": true}},
                  {"type": "doping", "dopant": "acceptor", "profile": "implant", "dose": "1e14[1/cm^2]", "Rp": "0.1[um]", "dRp": "30[nm]",
                   "dRl": "30[nm]" %2, "ta": "%1", "selection": {"all": true}},
                  {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["si"], "side": "bottom"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%3"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(time, window, h, width, deep)
                .toUtf8();
        };
        QStringList said;
        for (const bool annealed : {false, true}) {
            const double sigma = annealed ? sd : dRp;
            const Run r = run(modelOf(implant(QStringLiteral("0.02"), QStringLiteral("1"), QStringLiteral("0.005"),
                                              annealed ? QStringLiteral("30[min]") : QStringLiteral("0[s]"), QString())));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.solution;
            Node sum;
            sum.type = QStringLiteral("integral");
            sum.props.insert(QStringLiteral("expression"), QStringLiteral("semi.Na*1[m]"));
            sum.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
            const DerivedResult d = evaluateDerived(s, sum);
            QVERIFY2(!d.values.isEmpty(), qPrintable(d.error));
            const double total = d.values.front().value / 0.02e-6, peak = at1(s, QStringLiteral("semi.Na"), 0.01, -0.1), expected = depth(Rp, sigma);
            // The junction: where the acceptors fall to the donors, halved.
            const Locator locator(s.mesh());
            double lo = 0.1, hi = 0.99;
            for (int k = 0; k < 40; ++k) {
                const double mid = (lo + hi) / 2;
                (evaluateAt(s, locator, QStringLiteral("semi.Na"), QPointF(0.01, -mid)).value_or(0) > 1e21 ? lo : hi) = mid;
            }
            double jlo = Rp, jhi = 1e-6;
            for (int k = 0; k < 100; ++k) {
                const double mid = (jlo + jhi) / 2;
                (depth(mid, sigma) > 1e21 ? jlo : jhi) = mid;
            }
            said << QStringLiteral("%1: dose %2 /m² (%3), at the range %4 /m³ (%5), the junction %6 µm deep (%7)")
                        .arg(annealed ? QStringLiteral("annealed") : QStringLiteral("implanted"))
                        .arg(total, 0, 'g', 6)
                        .arg(dose, 0, 'g', 6)
                        .arg(peak, 0, 'g', 5)
                        .arg(expected, 0, 'g', 5)
                        .arg((lo + hi) / 2, 0, 'f', 4)
                        .arg((jlo + jhi) / 2 * 1e6, 0, 'f', 4);
            QVERIFY2(std::abs(total / dose - 1) < 1e-3, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(std::abs(peak / expected - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(std::abs((lo + hi) / 2 - (jlo + jhi) / 2 * 1e6) < 0.005, qPrintable(said.join(QLatin1String("; "))));
        }
        {
            const Run r = run(modelOf(implant(QStringLiteral("1"), QStringLiteral("0.3"), QStringLiteral("0.01"), QStringLiteral("30[min]"),
                                              QStringLiteral(R"(, "x1": "0.3", "x2": "0.7")"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double edge = at1(*r.solution, QStringLiteral("semi.Na"), 0.3, -0.1), middle = at1(*r.solution, QStringLiteral("semi.Na"), 0.5, -0.1);
            const double expected = across(0.3e-6, sd) / across(0.5e-6, sd);
            said << QStringLiteral("at the window's edge %1 of its middle (%2)").arg(edge / middle, 0, 'f', 4).arg(expected, 0, 'f', 4);
            QVERIFY2(std::abs(edge / middle / expected - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
        }
        // The anneal solved: a PDE along the depth (x, from the surface at 0).
        const QString u0 = QStringLiteral("1e18/(sqrt(2*pi)*3e-8)*(exp(-(X - 1e-7)*(X - 1e-7)/(2*9e-16)) + exp(-(X + 1e-7)*(X + 1e-7)/(2*9e-16)))");
        const Run line = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "si", "size": ["1", "0.01"]}],
            "physics": [{"type": "pde", "tag": "c", "space": "1d", "elements": 400, "features": [
              {"type": "coefficient", "c": "%1", "f": "0", "selection": {"all": true}},
              {"type": "init", "u0": "%2", "selection": {"all": true}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.01"}}],
          "studies": [{"type": "study", "steps": [{"type": "transient", "times": "range(0, 300, 1800)", "rtol": "1e-6"}]}]})json")
                                         .arg(QString::number(D, 'g', 12), QString(u0).replace(QStringLiteral("X"), QStringLiteral("(x/1[m])")))
                                         .toUtf8()));
        QVERIFY2(line.error.isEmpty(), qPrintable(line.error));
        Solution& ls = *line.solution;
        ls.select(ls.snapshotCount() - 1);
        for (double d : {0.0, 0.05, 0.1, 0.2, 0.3}) {
            const double u = at1(ls, QStringLiteral("u"), d, 0.005), expected = depth(d * 1e-6, sd);
            said << QStringLiteral("solved at %1 µm %2 (%3)").arg(d).arg(u, 0, 'g', 6).arg(expected, 0, 'g', 6);
            QVERIFY2(std::abs(u / expected - 1) < 5e-3, qPrintable(said.join(QLatin1String("; "))));
        }
        qInfo("implant and anneal: %s", qPrintable(said.join(QLatin1String("; "))));
    }

    void annealedDopingRead()
    {
        // The implant of implantAndAnneal as implanted, diffused by a PDE in
        // a step in time (30 minutes at boron's 1000 °C diffusivity), the
        // semiconductor's doping that PDE's u read as an expression in the
        // next step: the Implant profile's, annealed, to 1 %. (A column 0.02
        // µm wide, meshed at 5 nm, a sanitized build's 10.)
        constexpr double KB_eV = 8.617333262e-5;
        const double D = 0.76e-4 * std::exp(-3.46 / (KB_eV * 1273.15));
        const QString h = slowBuild() ? QStringLiteral("0.01") : QStringLiteral("0.005");
        const QString u0 = QStringLiteral("1e18/(sqrt(2*pi)*3e-8)*(exp(-(X - 1e-7)*(X - 1e-7)/(2*9e-16)) + exp(-(X + 1e-7)*(X + 1e-7)/(2*9e-16)))");
        QStringList said;
        // The semiconductor's doping that diffusion's, read as an expression.
        const Run both = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "si", "position": ["0", "-0.6"], "size": ["0.02", "0.6"]}],
            "materials": [{"material": "Silicon", "selection": {"all": true}}],
            "physics": [{"type": "pde", "tag": "c", "features": [
              {"type": "coefficient", "c": "%1", "f": "0", "selection": {"all": true}},
              {"type": "init", "u0": "%2", "selection": {"all": true}}]},
             {"type": "semiconductor", "tag": "semi", "features": [
              {"type": "doping", "dopant": "donor", "N0": "1e15[1/cm^3]", "selection": {"all": true}},
              {"type": "doping", "dopant": "acceptor", "profile": "expression", "N": "c.u*1[1/m^3]", "selection": {"all": true}},
              {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["si"], "side": "bottom"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "%3"}}],
          "studies": [{"type": "study", "steps": [{"type": "transient", "physics": ["c"], "times": "0, 1800", "rtol": "1e-4"},
                                                  {"type": "stationary", "physics": ["semi"]}]}]})json")
                                         .arg(QString::number(D, 'g', 12), QString(u0).replace(QStringLiteral("X"), QStringLiteral("(-y/1[m])")), h)
                                         .toUtf8()));
        QVERIFY2(both.error.isEmpty(), qPrintable(both.error));
        const Run feature = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "si", "position": ["0", "-0.6"], "size": ["0.02", "0.6"]}],
            "materials": [{"material": "Silicon", "selection": {"all": true}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "features": [
              {"type": "doping", "dopant": "donor", "N0": "1e15[1/cm^3]", "selection": {"all": true}},
              {"type": "doping", "dopant": "acceptor", "profile": "implant", "dose": "1e14[1/cm^2]", "Rp": "0.1[um]", "dRp": "30[nm]",
               "ta": "30[min]", "selection": {"all": true}},
              {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["si"], "side": "bottom"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "%1"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                                            .arg(h)
                                            .toUtf8()));
        QVERIFY2(feature.error.isEmpty(), qPrintable(feature.error));
        for (double d : {0.1, 0.25}) {
            const double fromPde = at1(*both.solution, QStringLiteral("semi.Na"), 0.01, -d), fromFeature = at1(*feature.solution, QStringLiteral("semi.Na"), 0.01, -d);
            said << QStringLiteral("at %1 µm the PDE's doping %2, the Implant profile's %3").arg(d).arg(fromPde, 0, 'g', 6).arg(fromFeature, 0, 'g', 6);
            QVERIFY2(std::abs(fromPde / fromFeature - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
        }
        qInfo("the annealed doping read: %s", qPrintable(said.join(QLatin1String("; "))));
    }

    void quantumCarriers()
    {
        // A flat well 10 nm wide (one material, ψ nought at its ends: an
        // infinite one), its electrons' Fermi level 50 meV above the
        // conduction band, its holes' 20 meV into the valence band (electrons 0.1 m0, heavy holes 0.5,
        // light 0.2), 12 states each: its carriers per area Σ (m kT/πħ²)
        // ln(1 + e^((Ef - Ei)/kT)), Ei = ħ²(iπ/L)²/2m - each carrier's - and
        // at its middle Σ (2/L) sin²(iπ/2) of each, to 1e-3.
        constexpr double Hbar = 1.054571817e-34, M0 = 9.1093837015e-31, KB = 1.380649e-23;
        const double L = 10e-9, kT = KB * 300;
        const Run r = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "name": "well", "size": ["2", "10"]}],
            "physics": [{"type": "quantum", "tag": "qw", "space": "1d", "axis": "y", "at": "1[nm]", "h1d": "0.2[nm]",
              "etan": "0.05[eV]", "etap": "0.02[eV]", "selection": {"all": true}, "features": [
              {"type": "material", "source": "user", "Ec": "1[eV]", "Ev": "0[eV]", "me": "0.1", "mhh": "0.5", "mlh": "0.2", "selection": {"all": true}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.5"}}],
          "studies": [{"type": "study", "steps": [{"type": "states", "states": 12}]}]})json")
                                      .toUtf8()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.study.solutions.front();
        const Locator locator(s.mesh());
        double sheet[2] = {0, 0};
        const int k = 2000;
        for (int j = 0; j < k; ++j) {
            const double y = (j + 0.5) * 10.0 / k;
            sheet[0] += evaluateAt(s, locator, QStringLiteral("qw.n"), QPointF(1, y)).value_or(NAN) * L / k;
            sheet[1] += evaluateAt(s, locator, QStringLiteral("qw.p"), QPointF(1, y)).value_or(NAN) * L / k;
        }
        const double middle[2] = {evaluateAt(s, locator, QStringLiteral("qw.n"), QPointF(1, 5)).value_or(NAN),
                                  evaluateAt(s, locator, QStringLiteral("qw.p"), QPointF(1, 5)).value_or(NAN)};
        double expected[2] = {0, 0}, atMiddle[2] = {0, 0};
        auto add = [&](int c, double m, double eta0) {
            for (int i = 1; i <= 12; ++i) {
                const double Ei = Hbar * Hbar * (i * M_PI / L) * (i * M_PI / L) / (2 * m * M0);
                const double e = eta0 - Ei / kT, per = m * M0 * kT / (M_PI * Hbar * Hbar) * (e > 30 ? e : std::log1p(std::exp(e)));
                expected[c] += per;
                atMiddle[c] += per * 2 / L * std::pow(std::sin(i * M_PI / 2), 2);
            }
        };
        add(0, 0.1, 0.05 * Q / kT);
        add(1, 0.5, 0.02 * Q / kT);
        add(1, 0.2, 0.02 * Q / kT);
        qInfo("a well's carriers: electrons %.6g /m² (%.6g), at its middle %.6g /m³ (%.6g); holes %.6g /m² (%.6g), %.6g /m³ (%.6g)", sheet[0],
              expected[0], middle[0], atMiddle[0], sheet[1], expected[1], middle[1], atMiddle[1]);
        for (int c = 0; c < 2; ++c) {
            QVERIFY2(std::abs(sheet[c] / expected[c] - 1) < 1e-3, qPrintable(QString::number(sheet[c] / expected[c])));
            QVERIFY2(std::abs(middle[c] / atMiddle[c] - 1) < 1e-3, qPrintable(QString::number(middle[c] / atMiddle[c])));
        }
    }

    // The semiconductor features' examples (B4 of the MQW and semiconductor
    // review), each solved as it comes - fewer points, a coarser mesh or
    // coarser times in a sanitized build - against what its description
    // says.
    // Avalanche breakdown: where the ionization integral reaches 1 the
    // depletion approximation's 51.7 V, to 2 % (4 a sanitized build's,
    // its mesh coarser); the holes' integral 1 there too.
    void exampleBreakdown()
    {
        const bool slow = slowBuild();
        QElapsedTimer timer;
        QStringList said;
        timer.start();
        Model m = exampleModel(QStringLiteral("avalanche_breakdown.qfem"));
        if (slow) {
            sweepOnly(m, QStringLiteral("Vr"), QStringLiteral("46, 50, 52, 54"), QStringLiteral("V"));
            m.find(m.mesh().tag)->props.insert(QStringLiteral("hmax"), QStringLiteral("0.1"));
        }
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        std::vector<double> V, In, Ip;
        for (const auto& point : r.study.solutions) {
            V.push_back(-global(*point, QStringLiteral("semi.V_a")));
            In.push_back(global(*point, QStringLiteral("semi.In")));
            Ip.push_back(global(*point, QStringLiteral("semi.Ip")));
        }
        double BV = NAN, IpThere = NAN;
        for (std::size_t k = 1; k < V.size(); ++k)
            if (In[k - 1] < 1 && In[k] >= 1) {
                const double f = -std::log(In[k - 1]) / (std::log(In[k]) - std::log(In[k - 1]));
                BV = V[k - 1] + f * (V[k] - V[k - 1]);
                IpThere = std::exp(std::log(Ip[k - 1]) + f * (std::log(Ip[k]) - std::log(Ip[k - 1])));
            }
        const double analytic = triangleBreakdown(1e22);
        said << QStringLiteral("avalanche: breaks down at %1 V (the triangle's %2), Ip there %3; %4 ms")
                    .arg(BV, 0, 'f', 2)
                    .arg(analytic, 0, 'f', 2)
                    .arg(IpThere, 0, 'f', 4)
                    .arg(timer.elapsed());
        QVERIFY2(std::abs(BV / analytic - 1) < (slow ? 0.04 : 0.02) && std::abs(IpThere - 1) < 0.02, qPrintable(said.join(QLatin1String("; "))));
        qInfo("%s", qPrintable(said.join(QLatin1String("; "))));
    }

    // Zener tunnelling (Hurkx's): from 2 V its current q ∫ G to 1 %
    // and between 0.45 and 0.8 of Kane's generation alone at each
    // triangle's field; nought at 0 V; five decades from 1.5 to 2.5 V.
    void exampleZenerTunnelling()
    {
        const bool slow = slowBuild();
        QElapsedTimer timer;
        QStringList said;
        timer.start();
        Model m = exampleModel(QStringLiteral("zener_tunnelling.qfem"));
        if (slow) m.find(m.mesh().tag)->props.insert(QStringLiteral("hmax"), QStringLiteral("4"));
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 6);
        const double A = 4e14 * 1e6 * std::pow(1e-2, 2.5), B = 1.9e9, depth = 1e-6;
        QCOMPARE(global(*r.study.solutions.front(), QStringLiteral("semi.I_k")), 0.0);
        std::vector<double> I;
        for (std::size_t k = 1; k < r.study.solutions.size(); ++k) {
            Solution& s = *r.study.solutions[k];
            I.push_back(global(s, QStringLiteral("semi.I_k")));
            Node gen;
            gen.type = QStringLiteral("integral");
            gen.props.insert(QStringLiteral("expression"), QStringLiteral("-1.602176634e-19[C]*semi.R*1[um]"));
            gen.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
            const DerivedResult d = evaluateDerived(s, gen);
            QVERIFY2(!d.values.isEmpty(), qPrintable(d.error));
            double kane = 0;
            const Locator locator(s.mesh());
            for (const auto& tri : s.mesh().triangles) {
                const QPointF a = s.mesh().nodes[std::size_t(tri[0])], b = s.mesh().nodes[std::size_t(tri[1])], c = s.mesh().nodes[std::size_t(tri[2])];
                const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2 * 1e-18;
                if (const std::optional<double> E = evaluateAt(s, locator, QStringLiteral("semi.normE"), (a + b + c) / 3.0); E && *E > 0)
                    kane += A * std::pow(*E, 2.5) * std::exp(-B / *E) * area;
            }
            kane *= Q * depth;
            said << QStringLiteral("zener at %1 V: %2 A (q ∫ G %3; %4 of Kane's alone)")
                        .arg(-0.5 * double(k))
                        .arg(I.back(), 0, 'g', 4)
                        .arg(d.values.front().value, 0, 'g', 4)
                        .arg(I.back() / kane, 0, 'f', 3);
            // (Below 1.5 V the diode's own leakage, 1e-22 A, more than its
            // tunnelling; at 1.5 V 4 % of it.)
            if (k < 4) continue;
            QVERIFY2(I.back() > 0 && std::abs(I.back() / d.values.front().value - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(I.back() / kane > 0.45 && I.back() / kane < 0.8, qPrintable(said.join(QLatin1String("; "))));
        }
        said << QStringLiteral("zener: from 1.5 to 2.5 V %1 times; %2 ms").arg(I[4] / I[2], 0, 'g', 3).arg(timer.elapsed());
        QVERIFY2(I[4] / I[2] > 1e4 && I[4] / I[2] < 1e6, qPrintable(said.join(QLatin1String("; "))));
        qInfo("%s", qPrintable(said.join(QLatin1String("; "))));
    }

    // Fowler-Nordheim: at each gate voltage semi.Ifn the oxide's
    // J = A E² e^(-B/E) over the gate (0.1 µm by 100 µm) to 1 %, its
    // Fowler-Nordheim plot's slope -B to 1 %.
    void exampleFowlerNordheim()
    {
        QElapsedTimer timer;
        QStringList said;
        timer.start();
        Model m = exampleModel(QStringLiteral("fowler_nordheim_oxide.qfem"));
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        constexpr double H = 6.62607015e-34, M0 = 9.1093837015e-31;
        const double phiB = 3.1 * Q, mox = 0.42, area = 0.1e-6 * 100e-6;
        const double A = Q * Q * Q / (8 * M_PI * H * phiB * mox), B = 8 * M_PI * std::sqrt(2 * mox * M0) * std::pow(phiB, 1.5) / (3 * Q * H);
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int n = 0;
        for (const auto& point : r.study.solutions) {
            const double E = at1(*point, QStringLiteral("semi.normE"), 0.05, 0.005), I = global(*point, QStringLiteral("semi.Ifn"));
            const double J = A * E * E * std::exp(-B / E);
            said << QStringLiteral("FN at %1 MV/cm: %2 A/cm² (%3)").arg(E / 1e8, 0, 'f', 2).arg(I / area * 1e-4, 0, 'g', 4).arg(J * 1e-4, 0, 'g', 4);
            QVERIFY2(std::abs(I / (J * area) - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
            const double x = 1 / E, y = std::log(I / (E * E));
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
            ++n;
        }
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        said << QStringLiteral("FN plot's slope %1 MV/cm (B %2); %3 ms").arg(-slope / 1e8, 0, 'f', 1).arg(B / 1e8, 0, 'f', 1).arg(timer.elapsed());
        QVERIFY2(n == 7 && std::abs(-slope / B - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
        qInfo("%s", qPrintable(said.join(QLatin1String("; "))));
    }

    // Interface traps' C-V: through depletion each gate voltage Vfb +
    // ψs - (Qs + Qit)/Cox at the surface potential solved, to 2 mV;
    // quasi-statically Cox in series with Cs + q² Dit to 0.5 %; from
    // -0.9 to 0 V ψs 0.504 V (without traps 0.565).
    void exampleInterfaceTraps()
    {
        QElapsedTimer timer;
        QStringList said;
        timer.start();
        Model m = exampleModel(QStringLiteral("interface_traps_cv.qfem"));
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double Vt = 1.380649e-23 * 300 / Q, Na = 1e23, es = 11.7 * Eps0, cox = 3.9 * Eps0 / 10e-9;
        const double ni = std::sqrt(2.8e25 * 1.04e25) * std::exp(-1.12 / (2 * Vt));
        const double dEc = 1.12 / 2 + Vt / 2 * std::log(2.8 / 1.04), phiB = Vt * std::log(Na / ni), vfb = -dEc - phiB;
        const double Dit = 5e15 / Q;
        auto Qs = [&](double psi) {
            const double b = psi / Vt, ratio = ni * ni / (Na * Na);
            const double F = std::sqrt(std::max(std::exp(-b) + b - 1 + ratio * (std::exp(b) - b - 1), 0.0));
            return -(psi >= 0 ? 1 : -1) * std::sqrt(2 * es * 1.380649e-23 * 300 * Na) * F;
        };
        double from = NAN, to = NAN;
        for (const auto& point : r.study.solutions) {
            Solution& s = *point;
            const double vg = s.sweptValues.value(QStringLiteral("Vg"));
            s.select(0);
            const double C0 = global(s, QStringLiteral("semi.C0gg")) / (0.1e-6 * 100e-6);
            s.select(-1);
            const double psi = at1(s, QStringLiteral("semi.V"), 0.05, -1e-7) - at1(s, QStringLiteral("semi.V"), 0.05, -0.399);
            if (std::abs(vg + 0.9) < 1e-9) from = psi;
            if (std::abs(vg) < 1e-9) to = psi;
            if (vg < -0.8 || vg > 0.01) continue;
            const double expected = vfb + psi - (Qs(psi) - Q * Dit * Q * (psi - phiB)) / cox;
            const double Cs = -(Qs(psi + 1e-4) - Qs(psi - 1e-4)) / 2e-4, Clf = 1 / (1 / cox + 1 / (Cs + Q * Q * Dit));
            said << QStringLiteral("traps at %1 V: ψs %2 V, the gate's %3 V by it; C0 %4 of Cox (%5)")
                        .arg(vg)
                        .arg(psi, 0, 'f', 4)
                        .arg(expected, 0, 'f', 4)
                        .arg(C0 / cox, 0, 'f', 4)
                        .arg(Clf / cox, 0, 'f', 4);
            QVERIFY2(std::abs(expected - vg) < 2e-3 && std::abs(C0 / Clf - 1) < 5e-3, qPrintable(said.join(QLatin1String("; "))));
        }
        said << QStringLiteral("traps: ψs from -0.9 to 0 V %1 V; %2 ms").arg(to - from, 0, 'f', 3).arg(timer.elapsed());
        QVERIFY2(std::abs(to - from - 0.504) < 0.01, qPrintable(said.join(QLatin1String("; "))));
        qInfo("%s", qPrintable(said.join(QLatin1String("; "))));
    }

    // Silicon's inversion layer in its two valley sets at 2 V: the
    // semiconductor's electrons the states' to 1e-3; some two thirds
    // in the lower ladder; their centroid each set's Fang and Howard's
    // by their electrons, to 15 %.
    void exampleInversionInValleys()
    {
        const bool slow = slowBuild();
        QElapsedTimer timer;
        QStringList said;
        timer.start();
        Model m = exampleModel(QStringLiteral("si_inversion_valleys.qfem"));
        sweepOnly(m, QStringLiteral("Vg"), QStringLiteral("2"), QStringLiteral("V"));
        if (slow) setAll(m, QStringLiteral("quantum"), QStringLiteral("h1d"), QStringLiteral("0.2[nm]"));
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.study.solutions.back();
        const Locator locator(s.mesh());
        double most = 0, worst = 0;
        std::vector<std::pair<QPointF, double>> nodes;
        for (const QPointF& p : s.mesh().nodes)
            if (p.y() < -1e-9 && p.y() > -0.02 + 1e-9)
                if (const std::optional<double> n = evaluateAt(s, locator, QStringLiteral("semi.n"), p)) {
                    nodes.emplace_back(p, *n);
                    most = std::max(most, *n);
                }
        for (const auto& [p, n] : nodes)
            if (n > 1e-3 * most) worst = std::max(worst, std::abs(n / evaluateAt(s, locator, QStringLiteral("qw.n"), p).value_or(NAN) - 1));
        double ninv = 0, moment = 0, Qd = 0;
        const int k = 400;
        for (int j = 0; j < k; ++j) {
            const double y = -(j + 0.5) * 0.02 / k, n = at1(s, QStringLiteral("qw.n"), 0.005, y);
            ninv += n * 0.02e-6 / k;
            moment += n * -y * 1e-6 * 0.02e-6 / k;
        }
        for (int j = 0; j < k; ++j) Qd += (1e23 - at1(s, QStringLiteral("semi.p"), 0.005, -(j + 0.5) * 0.3 / k)) * 0.3e-6 / k;
        constexpr double Hbar = 1.054571817e-34, M0 = 9.1093837015e-31;
        const double Nstar = Qd + 11.0 / 32 * ninv, es = 11.7 * Eps0;
        auto fangHoward = [&](double mz) { return 3 / std::cbrt(12 * mz * M0 * Q * Q * Nstar / (es * Hbar * Hbar)); };
        s.select(0);
        const double N1 = global(s, QStringLiteral("qw.Nv1")), N2 = global(s, QStringLiteral("qw.Nv2"));
        const double centroid = moment / ninv, expected = (N1 * fangHoward(0.916) + N2 * fangHoward(0.190)) / (N1 + N2);
        said << QStringLiteral("valleys: semi.n the states' to %1; %2 % in the two valleys; centroid %3 nm (Fang and Howard's %4); %5 ms")
                    .arg(worst, 0, 'g', 2)
                    .arg(100 * N1 / (N1 + N2), 0, 'f', 1)
                    .arg(centroid * 1e9, 0, 'f', 2)
                    .arg(expected * 1e9, 0, 'f', 2)
                    .arg(timer.elapsed());
        QVERIFY2(worst < 1e-3 && N1 / (N1 + N2) > 0.6 && N1 / (N1 + N2) < 0.72 && std::abs(centroid / expected - 1) < 0.15,
                 qPrintable(said.join(QLatin1String("; "))));
        qInfo("%s", qPrintable(said.join(QLatin1String("; "))));
    }

    // Boron implanted and annealed through a window (0.3 to 0.7 µm):
    // its dose 1e14 cm⁻² to 1e-3; down the window's middle at the range
    // dose/(√(2π) σ) (1 + e^(-2Rp²/σ²)) times the window's erf's, to 1
    // %; at an edge half the middle's, to 1 %; the junction there to 5
    // nm.
    void exampleImplantThroughAMask()
    {
        const bool slow = slowBuild();
        QElapsedTimer timer;
        QStringList said;
        timer.start();
        Model m = exampleModel(QStringLiteral("boron_implant_anneal.qfem"));
        if (slow) m.find(m.mesh().tag)->props.insert(QStringLiteral("hmax"), QStringLiteral("0.02"));
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        constexpr double KB_eV = 8.617333262e-5;
        const double dose = 1e18, Rp = 1e-7, D = 0.76e-4 * std::exp(-3.46 / (KB_eV * 1273.15)), sd = std::sqrt(9e-16 + 2 * D * 1800);
        auto across = [&](double x) { return 0.5 * (std::erf((x - 0.3e-6) / (std::sqrt(2.0) * sd)) - std::erf((x - 0.7e-6) / (std::sqrt(2.0) * sd))); };
        auto depth = [&](double d) {
            return dose / (std::sqrt(2 * M_PI) * sd) * (std::exp(-(d - Rp) * (d - Rp) / (2 * sd * sd)) + std::exp(-(d + Rp) * (d + Rp) / (2 * sd * sd)));
        };
        const DerivedResult total = evaluateDerived(s, m.results().child(QStringLiteral("derived"))->children.front());
        QVERIFY2(!total.values.isEmpty(), qPrintable(total.error));
        const double middle = at1(s, QStringLiteral("semi.Na"), 0.5, -0.1), edge = at1(s, QStringLiteral("semi.Na"), 0.3, -0.1);
        const Locator locator(s.mesh());
        double lo = 0.1, hi = 0.79;
        for (int k = 0; k < 40; ++k) {
            const double mid = (lo + hi) / 2;
            (evaluateAt(s, locator, QStringLiteral("semi.Na"), QPointF(0.5, -mid)).value_or(0) > 1e21 ? lo : hi) = mid;
        }
        double jlo = Rp, jhi = 0.79e-6;
        for (int k = 0; k < 100; ++k) {
            const double mid = (jlo + jhi) / 2;
            (depth(mid) * across(0.5e-6) > 1e21 ? jlo : jhi) = mid;
        }
        said << QStringLiteral("implant: dose %1 /cm² (1e14); at the range %2 /cm³ (%3); at the edge %4 of the middle (%5); junction %6 µm (%7); %8 ms")
                    .arg(total.values.front().value * 1e-4, 0, 'g', 6)
                    .arg(middle * 1e-6, 0, 'g', 5)
                    .arg(depth(Rp) * across(0.5e-6) * 1e-6, 0, 'g', 5)
                    .arg(edge / middle, 0, 'f', 4)
                    .arg(across(0.3e-6) / across(0.5e-6), 0, 'f', 4)
                    .arg((lo + hi) / 2, 0, 'f', 4)
                    .arg((jlo + jhi) / 2 * 1e6, 0, 'f', 4)
                    .arg(timer.elapsed());
        QVERIFY2(std::abs(total.values.front().value / dose - 1) < 1e-3, qPrintable(said.join(QLatin1String("; "))));
        QVERIFY2(std::abs(middle / (depth(Rp) * across(0.5e-6)) - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
        QVERIFY2(std::abs(edge / middle / (across(0.3e-6) / across(0.5e-6)) - 1) < 0.01, qPrintable(said.join(QLatin1String("; "))));
        QVERIFY2(std::abs((lo + hi) / 2 - (jlo + jhi) / 2 * 1e6) < 0.005, qPrintable(said.join(QLatin1String("; "))));
        qInfo("%s", qPrintable(said.join(QLatin1String("; "))));
    }

    // The resistor heating itself: at each voltage the one-dimensional
    // problem's current to 0.5 %, its middle's temperature to 1 % of
    // its rise.
    void exampleSelfHeating()
    {
        QElapsedTimer timer;
        QStringList said;
        timer.start();
        const Run r = run(exampleModel(QStringLiteral("self_heating_resistor.qfem")));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 4);
        for (const auto& point : r.study.solutions) {
            const double V = point->sweptValues.value(QStringLiteral("V0"));
            double Tmid = NAN;
            const double expected = selfHeated(20e-6, V, &Tmid) * 1e-12, I = std::abs(global(*point, QStringLiteral("semi.I_2")));
            const double T = at1(*point, QStringLiteral("ht.T"), 10, 0.5), cold = V * Q * 1e23 * 0.14 / 20e-6 * 1e-12;
            said << QStringLiteral("heating at %1 V: %2 mA (%3; %4 % below Ohm's), its middle %5 K (%6)")
                        .arg(V)
                        .arg(I * 1e3, 0, 'g', 5)
                        .arg(expected * 1e3, 0, 'g', 5)
                        .arg(100 * (1 - I / cold), 0, 'f', 1)
                        .arg(T, 0, 'f', 2)
                        .arg(Tmid, 0, 'f', 2);
            QVERIFY2(std::abs(I / expected - 1) < 5e-3 && std::abs(T - Tmid) < 0.01 * (Tmid - 300) + 1e-6, qPrintable(said.join(QLatin1String("; "))));
        }
        said << QStringLiteral("heating: %1 ms").arg(timer.elapsed());
        qInfo("%s", qPrintable(said.join(QLatin1String("; "))));
    }

    // The diode's reverse recovery at two lifetimes: its reverse current
    // some 120 to 180 times the forward one; between the lifetimes the charge
    // out (beyond the steady reverse current) differs as the charge
    // stored at rest does - the depletion layer's change the same for
    // both -, 0.85 to 1.02 of it (a sanitized build's, coarser, 0.8 to
    // 1.05).
    void exampleReverseRecovery()
    {
        const bool slow = slowBuild();
        QElapsedTimer timer;
        QStringList said;
        timer.start();
        Model m = exampleModel(QStringLiteral("diode_reverse_recovery.qfem"));
        if (slow) {
            m.find(m.mesh().tag)->props.insert(QStringLiteral("hmax"), QStringLiteral("0.25"));
            setAll(m, QStringLiteral("transient"), QStringLiteral("times"),
                QStringLiteral("range(0, 1[ps], 5[ps]), range(7[ps], 2[ps], 25[ps]), range(30[ps], 10[ps], 100[ps]), range(150[ps], 50[ps], 1.2[ns])"));
            setAll(m, QStringLiteral("transient"), QStringLiteral("rtol"), QStringLiteral("3e-3"));
        }
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 2);
        double stored[2] = {0, 0}, moved[2] = {0, 0};
        for (int which = 0; which < 2; ++which) {
            Solution& s = *r.study.solutions[std::size_t(which)];
            s.select(0);
            const double forward = global(s, QStringLiteral("semi.I_a"));
            for (const auto& [side, expression] : {std::pair{"pside", "1.602176634e-19[C]*(semi.n - semi.ni^2/1e22[1/m^3])*100[um]"},
                                                   std::pair{"nside", "1.602176634e-19[C]*(semi.p - semi.ni^2/1e22[1/m^3])*100[um]"}}) {
                Node integral;
                integral.type = QStringLiteral("integral");
                integral.props.insert(QStringLiteral("expression"), QString::fromLatin1(expression));
                integral.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QString::fromLatin1(side)}}});
                const DerivedResult d = evaluateDerived(s, integral);
                QVERIFY2(d.error.isEmpty() && d.values.size() == 1, qPrintable(d.error));
                stored[which] += d.values[0].value;
            }
            const int last = s.snapshotCount() - 1;
            s.select(last);
            const double reverse = global(s, QStringLiteral("semi.I_a"));
            double peak = 0, before = 0, tb = 0;
            for (int k = 0; k <= last; ++k) {
                s.select(k);
                const double t = s.snapshotTime(k), I = global(s, QStringLiteral("semi.I_a")) - reverse;
                if (k > 0) moved[which] -= 0.5 * (I + before) * (t - tb);
                peak = std::min(peak, I + reverse);
                before = I;
                tb = t;
            }
            said << QStringLiteral("recovery at τ %1 ns: forward %2 mA, its reverse at most %3 mA (%4 times; steady %5 mA); stored %6 pC, out %7 pC")
                        .arg(which ? 0.3 : 0.1)
                        .arg(forward * 1e3, 0, 'g', 4)
                        .arg(peak * 1e3, 0, 'g', 4)
                        .arg(-peak / forward, 0, 'f', 0)
                        .arg(reverse * 1e3, 0, 'g', 3)
                        .arg(stored[which] * 1e12, 0, 'g', 4)
                        .arg(moved[which] * 1e12, 0, 'g', 4);
            QVERIFY2(peak < -100 * forward, qPrintable(said.join(QLatin1String("; "))));
        }
        const double k = (moved[1] - moved[0]) / (stored[1] - stored[0]);
        said << QStringLiteral("recovery: the charge out between the lifetimes %1 of the charge stored's; %2 ms").arg(k, 0, 'f', 4).arg(timer.elapsed());
        QVERIFY2(k > (slow ? 0.8 : 0.85) && k < (slow ? 1.05 : 1.02), qPrintable(said.join(QLatin1String("; "))));
        qInfo("%s", qPrintable(said.join(QLatin1String("; "))));
    }

    void dopantsIonizedInPart()
    {
        // Uniform silicon bars at equilibrium, their dopants ionized in part
        // (45 meV, gd 2 for phosphorus and ga 4 for boron):
        // - 1e18 cm⁻³ of phosphorus: n = Nd/(1 + 2 n/n1), n1 = Nc e^(-45 meV/kT)
        //   - 0.763 of the donors;
        // - 1e18 cm⁻³ of boron: p = Na/(1 + 4 p/p1) - 0.485;
        // - 1e19 cm⁻³ of phosphorus, Fermi-Dirac's too: n = Nc ℱ(η), Nd⁺ =
        //   Nd/(1 + 2 e^(η + ΔEd/kT)) - the donors filled by the Fermi level,
        //   not by n.
        // Each to 1e-6, semi.Ndi or semi.Nai the carriers, the charge nought.
        constexpr double KB = 1.380649e-23;
        const double Vt = KB * 300 / Q, Nc = 2.8e25, Nv = 1.04e25, n1 = Nc * std::exp(-0.045 / Vt), p1 = Nv * std::exp(-0.045 / Vt);
        struct Case {
            const char* dopant;
            double N;
            bool fd;
        };
        QStringList said;
        for (const Case& c : {Case{"donor", 1e24, false}, Case{"acceptor", 1e24, false}, Case{"donor", 1e25, true}}) {
            const bool donor = QLatin1String(c.dopant) == QLatin1String("donor");
            double expected = 0;
            if (!c.fd) {
                // g x²/x1 + x - N = 0.
                const double a = (donor ? 2 / n1 : 4 / p1);
                expected = (-1 + std::sqrt(1 + 4 * a * c.N)) / (2 * a);
            } else {
                // Nc ℱ(η) = N/(1 + 2 e^η Nc/n1), halved in η.
                double lo = -10, hi = 10;
                for (int k = 0; k < 100; ++k) {
                    const double mid = (lo + hi) / 2;
                    (Nc * fermiHalf(mid) < c.N / (1 + 2 * std::exp(mid) * Nc / n1) ? lo : hi) = mid;
                }
                expected = Nc * fermiHalf((lo + hi) / 2);
            }
            const QString geometry = QStringLiteral(R"({"type": "rectangle", "label": "left", "size": ["1", "0.2"]},
                     {"type": "rectangle", "label": "right", "position": ["1", "0"], "size": ["1", "0.2"]})");
            const QString doping = QStringLiteral(R"({"type": "doping", "dopant": "%1", "N0": "%2", "selection": {"all": true}})")
                                       .arg(QLatin1String(c.dopant), QString::number(c.N, 'g', 6));
            const QString model = c.fd ? QStringLiteral(R"(, "incomplete": true, "fermi": true)") : QStringLiteral(R"(, "incomplete": true)");
            const Run r = run(modelOf(siliconBar(geometry, doping, model, QStringLiteral(R"({"type": "stationary"})"), QStringLiteral("0.1"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.solution;
            const Locator locator(s.mesh());
            auto at = [&](const QString& e) {
                const std::optional<double> v = evaluateAt(s, locator, e, QPointF(1, 0.1));
                return v ? *v : NAN;
            };
            const double carriers = at(donor ? QStringLiteral("semi.n") : QStringLiteral("semi.p"));
            const double ions = at(donor ? QStringLiteral("semi.Ndi") : QStringLiteral("semi.Nai"));
            const double rho = at(QStringLiteral("semi.rho"));
            said << QStringLiteral("%1 %2 m⁻³%3: %4 (%5, %6 of them), ionized %7, ρ %8 C/m³")
                        .arg(QLatin1String(c.dopant))
                        .arg(c.N, 0, 'g', 3)
                        .arg(c.fd ? QStringLiteral(" Fermi-Dirac's") : QString())
                        .arg(carriers, 0, 'g', 7)
                        .arg(expected, 0, 'g', 7)
                        .arg(expected / c.N, 0, 'f', 3)
                        .arg(ions, 0, 'g', 7)
                        .arg(rho, 0, 'g', 3);
            QVERIFY2(std::abs(carriers / expected - 1) < 1e-6, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(std::abs(ions / carriers - 1) < 1e-6, qPrintable(said.join(QLatin1String("; "))));
            QVERIFY2(std::abs(rho) < 1e-6 * Q * carriers, qPrintable(said.join(QLatin1String("; "))));
        }
        qInfo("ionized in part: %s", qPrintable(said.join(QLatin1String("; "))));
    }
};

QTEST_GUILESS_MAIN(TestFemDevices)
#include "test_fem_devices.moc"
