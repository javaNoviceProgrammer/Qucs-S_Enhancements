/*
 * test_fem_devices.cpp - semiconductor devices beyond drift and diffusion
 *                        at its simplest: impact ionization (breakdown,
 *                        avalanche gain), a MOSFET's mobility (Lombardi's
 *                        surface scattering, saturation along the current)
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
};

QTEST_GUILESS_MAIN(TestFemDevices)
#include "test_fem_devices.moc"
