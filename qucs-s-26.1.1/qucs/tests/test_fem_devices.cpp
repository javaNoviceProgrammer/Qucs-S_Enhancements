/*
 * test_fem_devices.cpp - semiconductor devices beyond drift and diffusion
 *                        at its simplest: impact ionization (breakdown,
 *                        avalanche gain), a MOSFET's mobility (Lombardi's
 *                        surface scattering, saturation along the current),
 *                        heavy doping (Fermi-Dirac, band gap narrowing,
 *                        dopants ionized in part), interface traps
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
#include <QJsonArray>
#include <QRegularExpression>
#include <QtTest>

#include <cmath>
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
/// deep, 0.1 wide, an n⁺ polysilicon gate g at Vg, the substrate b;
/// \a traps its interface's features, \a steps its study's.
QByteArray mosCapacitor(const QString& traps, const QString& steps)
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Vg", "expression": "0[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "bulk", "position": ["0", "-0.4"], "size": ["0.1", "0.38"]},
                     {"type": "rectangle", "name": "surface", "position": ["0", "-0.02"], "size": ["0.1", "0.02"]},
                     {"type": "rectangle", "name": "oxide", "size": ["0.1", "0.01"]}],
        "materials": [{"material": "Silicon", "selection": {"only": ["bulk", "surface"]}},
                      {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["bulk", "surface"]}},
          {"type": "gate", "name": "g", "V0": "Vg", "phim": "4.05[eV]", "selection": {"of": ["oxide"], "side": "top"}},
          {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["bulk"], "side": "bottom"}} %1]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "0.02", "features": [
          {"type": "size", "level": "domain", "selection": {"only": ["surface", "oxide"]}, "hmax": "0.002"}]}}],
      "studies": [{"type": "study", "steps": [%2]}]})json")
        .arg(traps, steps)
        .toUtf8();
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
