/*
 * test_fem_quantum.cpp - the multiphysics solver's quantum wells against
 *                        what is known: an infinite well's levels, a finite
 *                        one's (BenDaniel and Duke's matching), a box's in
 *                        2D with its masses along and across the growth
 *                        axis, a harmonic oscillator's, the Stark shift,
 *                        strain's shifts, the alloys' gaps and lattices, 1D
 *                        against 2D, absorption's and gain's spectra
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_materials.h"
#include "fem_results.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QtTest>

#include <cmath>
#include <functional>
#include <vector>

using namespace qucs_s::fem;

namespace {

constexpr double Q = 1.602176634e-19, Hbar = 1.054571817e-34, M0 = 9.1093837015e-31, KB = 1.380649e-23;
constexpr double HB2M = 0.0380998212;   // ħ²/2m0, eV nm²

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

Run run(const Model& model)
{
    Run r;
    r.parameters = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *r.parameters);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.study = runStudy(model, r.parameters, g.topology, nullptr, QString());
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

double eV(const Solution& s, const QString& name)
{
    return global(s, name) / Q;
}

double at(const Solution& s, const QString& expression, QPointF p)
{
    const Locator locator(s.mesh());
    const std::optional<double> v = evaluateAt(s, locator, expression, p);
    return v ? *v : NAN;
}

/// A root of f between a and b (f's signs differ there), by bisection.
double root(const std::function<double(double)>& f, double a, double b)
{
    double fa = f(a);
    for (int i = 0; i < 200; ++i) {
        const double m = (a + b) / 2, fm = f(m);
        if ((fm < 0) == (fa < 0)) {
            a = m;
            fa = fm;
        } else b = m;
    }
    return (a + b) / 2;
}

/// A one-domain well of its own bands (nm): \a height along y, the growth
/// axis; its physics' extra properties in \a physics.
QByteArray box(const QString& width, const QString& height, const QString& physics, const QString& ec, const QString& steps,
               const QString& hmax = QStringLiteral("2"))
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "nm",
        "geometry": [{"type": "rectangle", "name": "well", "size": ["%1", "%2"]}],
        "physics": [{"type": "quantum", "tag": "qw" %3, "features": [
          {"type": "material", "source": "user", "Ec": "%4", "Ev": "-1.5[eV]", "me": "0.067", "mhh": "0.35", "mlh": "0.09",
           "mhhp": "0.11", "mlhp": "0.21", "Ep": "25[eV]", "nr": "3.6", "selection": {"all": true}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%6"}}],
      "studies": [{"type": "study", "steps": [%5]}]})json")
        .arg(width, height, physics, ec, steps, hmax)
        .toUtf8();
}

/// A well of a compound between barriers of another (nm, along y).
QByteArray heterostructure(const QString& well, const QString& wellX, const QString& barrier, const QString& barrierX, const QString& lw,
                           const QString& lb, const QString& physics, const QString& steps, const QString& width = QStringLiteral("10"))
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Lw", "expression": "%5[nm]"}, {"name": "Lb", "expression": "%6[nm]"}],
      "components": [{"unit": "nm",
        "geometry": [{"type": "rectangle", "name": "below", "size": ["%9", "Lb"]},
                     {"type": "rectangle", "name": "well", "position": ["0", "Lb"], "size": ["%9", "Lw"]},
                     {"type": "rectangle", "name": "above", "position": ["0", "Lb + Lw"], "size": ["%9", "Lb"]}],
        "materials": [{"material": "barrier", "compound": "%3", "x": "%4", "selection": {"only": ["below", "above"]}},
                      {"material": "well", "compound": "%1", "x": "%2", "selection": {"only": ["well"]}}],
        "physics": [{"type": "quantum", "tag": "qw" %7}],
        "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
      "studies": [{"type": "study", "steps": [%8]}]})json")
        .arg(well, wellX, barrier, barrierX, lw, lb, physics, steps, width)
        .toUtf8();
}

} // namespace

class TestFemQuantum : public QObject
{
    Q_OBJECT

private slots:
    void anInfiniteWell()
    {
        // ψ = 0 at both ends of 10 nm: E_n = n² π² ħ²/(2 m L²), each carrier's.
        const Run r = run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
                                      QStringLiteral(R"({"type": "states", "states": 3})"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        QCOMPARE(s.snapshotCount(), 3);
        for (int n = 1; n <= 3; ++n) {
            const double e = HB2M * n * n * M_PI * M_PI / (0.067 * 100), hh = -1.5 - HB2M * n * n * M_PI * M_PI / (0.35 * 100);
            const double fe = eV(s, QStringLiteral("qw.Ee%1").arg(n)), fh = eV(s, QStringLiteral("qw.Ehh%1").arg(n));
            qInfo("infinite well: e%d %.9f eV (%.9f), hh%d %.9f (%.9f)", n, fe, e, n, fh, hh);
            QVERIFY2(std::abs(fe / e - 1) < 1e-6, qPrintable(QString::number(fe / e - 1)));
            QVERIFY2(std::abs((fh + 1.5) / (hh + 1.5) - 1) < 1e-6, qPrintable(QString::number((fh + 1.5) / (hh + 1.5) - 1)));
        }
        // Its density integrates to 1 (per metre along the line); e1 a sine.
        Solution& w = *r.solution;
        for (const Variable& v : w.variables())
            if (v.name == QLatin1String("qw.P_e")) QVERIFY(v.dim == Dim::of(-1));
        w.select(0);
        const double peak = at(w, QStringLiteral("qw.P_e"), {10, 5});
        QVERIFY2(std::abs(peak / (2 / 10e-9) - 1) < 1e-6, qPrintable(QString::number(peak * 10e-9 / 2)));
        QVERIFY(std::abs(at(w, QStringLiteral("qw.psi_e"), {3, 2.5}) - std::sin(M_PI * 0.25)) < 1e-6);
        QCOMPARE(global(w, QStringLiteral("qw.state")), 1.0);
        QVERIFY(std::abs(global(w, QStringLiteral("qw.pos_e")) - 5e-9) < 1e-15);
        // The overlaps: e_n with hh_n whole, with hh_m none.
        QVERIFY(std::abs(global(w, QStringLiteral("qw.Oe1hh1")) - 1) < 1e-9);
        QVERIFY(global(w, QStringLiteral("qw.Oe1hh2")) < 1e-12);
    }

    void aFiniteWell()
    {
        // GaAs 10 nm between Al0.3Ga0.7As (20 nm each, their far ends hard
        // walls): BenDaniel and Duke's (k/mw) tan(kL/2) = κ/mb, and the odd
        // states' -(k/mw) cot(kL/2) = κ/mb.
        const Run r = run(modelOf(heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"),
                                                  QStringLiteral("10"), QStringLiteral("20"), QString(),
                                                  QStringLiteral(R"({"type": "states", "states": 2})"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const BandParameters w = *bandParameters(QStringLiteral("GaAs"), 0, 0, 300), b = *bandParameters(QStringLiteral("AlGaAs"), 0.3, 0, 300);
        for (int carrier = 0; carrier < 2; ++carrier) {
            // Electrons; heavy holes (their masses along the axis).
            const double V0 = carrier == 0 ? (b.VBO + b.Eg) - (w.VBO + w.Eg) : w.VBO - b.VBO;
            const double mw = carrier == 0 ? w.me : 1 / (w.gamma1 - 2 * w.gamma2), mb = carrier == 0 ? b.me : 1 / (b.gamma1 - 2 * b.gamma2);
            for (int n = 1; n <= 2; ++n) {
                auto f = [&](double E) {
                    const double k = std::sqrt(E * mw / HB2M), kappa = std::sqrt((V0 - E) * mb / HB2M);
                    return n == 1 ? k / mw * std::sin(k * 5) - kappa / mb * std::cos(k * 5) : -k / mw * std::cos(k * 5) - kappa / mb * std::sin(k * 5);
                };
                // (Bracketed below the infinite well's level.)
                const double hi = std::min(V0 * 0.999999, HB2M * n * n * M_PI * M_PI / (mw * 100));
                const double expected = root(f, n == 1 ? 1e-9 : HB2M * M_PI * M_PI / (mw * 100), hi);
                const double found = carrier == 0 ? eV(s, QStringLiteral("qw.Ee%1").arg(n)) - (w.VBO + w.Eg)
                                                  : w.VBO - eV(s, QStringLiteral("qw.Ehh%1").arg(n));
                qInfo("finite well, %s%d: %.7f eV above its band edge (BenDaniel and Duke %.7f; V0 %.4f)", carrier == 0 ? "e" : "hh", n, found,
                      expected, V0);
                QVERIFY2(std::abs(found / expected - 1) < 1e-6, qPrintable(QString::number(found / expected - 1)));
            }
        }
        QCOMPARE(global(s, QStringLiteral("qw.bound_e")), 1.0);
    }

    void aBoxIn2D()
    {
        // 10 by 6 nm, hard walls: E = ħ²π²/2 (n²/(m_x a²) + m²/(m_y b²)) - the
        // heavy holes' masses across (x) and along (y) the axis.
        const QString hmax = slowBuild() ? QStringLiteral("0.6") : QStringLiteral("0.3");
        const Run r = run(modelOf(box(QStringLiteral("10"), QStringLiteral("6"), QStringLiteral(R"(, "space": "2d")"), QStringLiteral("0[eV]"),
                                      QStringLiteral(R"({"type": "states", "states": 4})"), hmax)));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const double e[4] = {1.0 / 100 + 1.0 / 36, 4.0 / 100 + 1.0 / 36, 9.0 / 100 + 1.0 / 36, 1.0 / 100 + 4.0 / 36};
        for (int k = 0; k < 4; ++k) {
            const double expected = HB2M * M_PI * M_PI * e[k] / 0.067, found = eV(s, QStringLiteral("qw.Ee%1").arg(k + 1));
            qInfo("box: e%d %.7f eV (%.7f)", k + 1, found, expected);
            QVERIFY2(std::abs(found / expected - 1) < 2e-4, qPrintable(QString::number(found / expected - 1)));
        }
        const double hh = HB2M * M_PI * M_PI * (1.0 / (0.11 * 100) + 1.0 / (0.35 * 36));
        const double fh = -1.5 - eV(s, QStringLiteral("qw.Ehh1"));
        qInfo("box: hh1 %.7f eV below the band (%.7f)", fh, hh);
        QVERIFY2(std::abs(fh / hh - 1) < 2e-4, qPrintable(QString::number(fh / hh - 1)));
    }

    void aHarmonicOscillator()
    {
        // Ec = ½ m ω² (y - 30 nm)², ħω 40 meV: E_n = ħω (n + ½).
        const double omega = 0.04 * Q / Hbar;
        const QString ec = QStringLiteral("0.5*0.067*%1[kg]*(%2[1/s])^2*(y - 30[nm])^2").arg(M0, 0, 'g', 12).arg(omega, 0, 'g', 12);
        const Run r = run(modelOf(box(QStringLiteral("10"), QStringLiteral("60"), QString(), ec, QStringLiteral(R"({"type": "states", "states": 3})"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        for (int n = 0; n < 3; ++n) {
            const double found = eV(*r.solution, QStringLiteral("qw.Ee%1").arg(n + 1)), expected = 0.04 * (n + 0.5);
            qInfo("oscillator: e%d %.8f eV (%.8f)", n + 1, found, expected);
            QVERIFY2(std::abs(found / expected - 1) < 1e-6, qPrintable(QString::number(found / expected - 1)));
        }
    }

    void theStarkShift()
    {
        // A field across an infinite well lowers its first level by
        // (15 - π²)/(24 π⁴) m q² F² L⁴/ħ² (second order: Bastard's).
        auto e1 = [](const QString& field) {
            const Run r = run(modelOf(box(QStringLiteral("10"), QStringLiteral("10"), QStringLiteral(R"(, "F": "%1", "heavy": false, "light": false)").arg(field),
                                          QStringLiteral("0[eV]"), QStringLiteral(R"({"type": "states", "states": 1})"))));
            if (!r.error.isEmpty()) qFatal("%s", qPrintable(r.error));
            return global(*r.solution, QStringLiteral("qw.Ee1"));
        };
        const double F = 1e6, L = 10e-9;
        const double shift = e1(QStringLiteral("1e6[V/m]")) - e1(QStringLiteral("0[V/m]"));
        const double expected = -(15 - M_PI * M_PI) / (24 * std::pow(M_PI, 4)) * 0.067 * M0 * Q * Q * F * F * std::pow(L, 4) / (Hbar * Hbar);
        qInfo("Stark shift: %.5g eV (second order %.5g)", shift / Q, expected / Q);
        QVERIFY2(std::abs(shift / expected - 1) < 0.01, qPrintable(QString::number(shift / expected)));
    }

    void strainOnASubstrate()
    {
        // In0.2Ga0.8As on GaAs: compressed in the plane, ε = (a_GaAs - a)/a;
        // Ec up by ac (2ε + ε⊥), the heavy holes above the light ones (Bir
        // and Pikus).
        const Run r = run(modelOf(heterostructure(QStringLiteral("InGaAs"), QStringLiteral("0.2"), QStringLiteral("GaAs"), QStringLiteral("0"),
                                                  QStringLiteral("8"), QStringLiteral("15"), QStringLiteral(R"(, "substrate": "GaAs")"),
                                                  QStringLiteral(R"({"type": "states", "states": 1})"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const BandParameters w = *bandParameters(QStringLiteral("InGaAs"), 0.2, 0, 300), g = *bandParameters(QStringLiteral("GaAs"), 0, 0, 300);
        const double exx = (g.a0 - w.a0) / w.a0, ezz = -2 * w.C12 / w.C11 * exx, vol = 2 * exx + ezz;
        const double P = -w.av * vol, Qe = -w.b * (exx - ezz);
        const QPointF mid(5, 19);
        const double ec = at(s, QStringLiteral("qw.Ec"), mid) / Q, hh = at(s, QStringLiteral("qw.Ehh"), mid) / Q, lh = at(s, QStringLiteral("qw.Elh"), mid) / Q;
        const double lhExpected = w.VBO - P + 0.5 * (Qe - w.Delta + std::sqrt(w.Delta * w.Delta + 2 * w.Delta * Qe + 9 * Qe * Qe));
        qInfo("strain: ε %.5f; Ec %.5f eV (%.5f), Ehh %.5f (%.5f), Elh %.5f (%.5f); the gap %.4f eV, %.4f unstrained; e1-hh1 at %.1f nm",
              at(s, QStringLiteral("qw.strain"), mid), ec, w.VBO + w.Eg + w.ac * vol, hh, w.VBO - P - Qe, lh, lhExpected, ec - hh, w.Eg,
              global(s, QStringLiteral("qw.lambda")) * 1e9);
        QVERIFY(std::abs(at(s, QStringLiteral("qw.strain"), mid) - exx) < 1e-12);
        QVERIFY(exx < -0.01 && hh > lh);
        QVERIFY(std::abs(ec - (w.VBO + w.Eg + w.ac * vol)) < 1e-9);
        QVERIFY(std::abs(hh - (w.VBO - P - Qe)) < 1e-9);
        QVERIFY(std::abs(lh - lhExpected) < 1e-9);
        // The barriers on their own lattice: unstrained.
        QVERIFY(std::abs(at(s, QStringLiteral("qw.strain"), {5, 3})) < 1e-15);
        // An 8 nm In0.2Ga0.8As well emits near 980 nm.
        QVERIFY(global(s, QStringLiteral("qw.lambda")) > 950e-9 && global(s, QStringLiteral("qw.lambda")) < 1020e-9);
    }

    void theAlloys()
    {
        // What the literature has: their gaps at 300 K (eV) and lattices.
        struct Known {
            const char* compound;
            double x, y, eg, tolerance, a0;   // a0: of the lattice it is matched to (0: none)
        };
        const double inp = bandParameters(QStringLiteral("InP"), 0, 0, 300)->a0, gaas = bandParameters(QStringLiteral("GaAs"), 0, 0, 300)->a0;
        const Known known[] = {
            {"GaAs", 0, 0, 1.4225, 0.002, 0},
            {"InP", 0, 0, 1.3529, 0.002, 0},
            {"AlGaAs", 0.3, 0, 1.80, 0.05, gaas},
            {"InGaAs", 0.53, 0, 0.75, 0.02, inp},
            {"InAlAs", 0.52, 0, 1.46, 0.03, inp},
            {"GaInP", 0.51, 0, 1.92, 0.06, gaas},   // (disordered: Vurgaftman's bowing puts it at the top)
            {"InGaAsP", 0.28, 0.6, 0.961, 0.02, inp},   // (Nahory et al.: 1.35 - 0.72 y + 0.12 y²)
            {"AlGaInAs", 0.2, 0.27, 1.00, 0.06, inp},
        };
        for (const Known& k : known) {
            QString why;
            const std::optional<BandParameters> p = bandParameters(QString::fromLatin1(k.compound), k.x, k.y, 300, &why);
            QVERIFY2(p, qPrintable(why));
            qInfo("%s (%g, %g): Eg %.4f eV (%.3f), a0 %.5f nm, me %.4f", k.compound, k.x, k.y, p->Eg, k.eg, p->a0 * 1e9, p->me);
            QVERIFY2(std::abs(p->Eg - k.eg) < k.tolerance, k.compound);
            if (k.a0 > 0) QVERIFY2(std::abs(p->a0 / k.a0 - 1) < 2e-3, k.compound);
        }
        // An alloy's corners are its binaries.
        QVERIFY(std::abs(bandParameters(QStringLiteral("InGaAsP"), 0, 1, 300)->Eg - bandParameters(QStringLiteral("InAs"), 0, 0, 300)->Eg) < 1e-12);
        QVERIFY(std::abs(bandParameters(QStringLiteral("AlGaInAs"), 0, 1, 300)->Eg - bandParameters(QStringLiteral("GaAs"), 0, 0, 300)->Eg) < 1e-12);
        QVERIFY(!bandParameters(QStringLiteral("AlGaAs"), 1.2, 0, 300));
        QVERIFY(!bandParameters(QStringLiteral("Unobtainium"), 0, 0, 300));
        // Other physics read an alloy's properties at its composition.
        const QString eg = compoundProperty(QStringLiteral("InGaAs"), 0.53, 0, QStringLiteral("Eg"));
        QVERIFY(eg.endsWith(QLatin1String("[eV]")));
        QVERIFY(!compoundProperty(QStringLiteral("InGaAs"), 0.53, 0, QStringLiteral("Nc")).isEmpty());
        QVERIFY(compoundProperty(QStringLiteral("InGaAs"), 0.53, 0, QStringLiteral("BH")).isEmpty());
    }

    void lineAgainstPlane()
    {
        // The GaAs well across a strip 20 nm wide, its sides free (zero
        // flux): the 2D states those of the 1D line.
        const QString steps = QStringLiteral(R"({"type": "states", "states": 2})");
        const Run one = run(modelOf(heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"),
                                                    QStringLiteral("8"), QStringLiteral("8"), QString(), steps, QStringLiteral("20"))));
        QVERIFY2(one.error.isEmpty(), qPrintable(one.error));
        const QString flux = QStringLiteral(R"(, "space": "2d", "features": [{"type": "flux", "selection": {"of": ["below", "well", "above"], "side": "left"}},
            {"type": "flux", "selection": {"of": ["below", "well", "above"], "side": "right"}}])");
        QByteArray json = heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("8"),
                                          QStringLiteral("8"), flux, steps, QStringLiteral("20"));
        json.replace(R"("hmax": "2")", slowBuild() ? R"("hmax": "0.8")" : R"("hmax": "0.5")");
        const Run two = run(modelOf(json));
        QVERIFY2(two.error.isEmpty(), qPrintable(two.error));
        for (const char* name : {"qw.Ee1", "qw.Ehh1", "qw.Elh1"}) {
            const double a = eV(*one.solution, QString::fromLatin1(name)), b = eV(*two.solution, QString::fromLatin1(name));
            qInfo("%s: %.6f eV along the line, %.6f in the plane", name, a, b);
            QVERIFY2(std::abs(a - b) < 2e-5, name);
        }
        // In the plane the next state is the first's with a node across the
        // strip: ħ²π²/(2 m W²) higher (m GaAs's, most of it in the well).
        const double lateral = eV(*two.solution, QStringLiteral("qw.Ee2")) - eV(*two.solution, QStringLiteral("qw.Ee1"));
        qInfo("across the strip: %.6f eV (%.6f)", lateral, HB2M * M_PI * M_PI / (0.067 * 400));
        QVERIFY(std::abs(lateral / (HB2M * M_PI * M_PI / (0.067 * 400)) - 1) < 0.03);
    }

    void absorption()
    {
        // The infinite well's spectrum: each e_n to h_n step (overlap 1)
        // broadened by a Lorentzian, of the height π q² ħ/(n c ε0 m0² E) |ê·p|²
        // m_r/(π ħ² L).
        const double gamma = 0.005, L = 10e-9;
        const Run r = run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
                                      QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.5[eV], 2.1[eV], 13)", "states": 3,
                                                        "linewidth": "5[meV]"})x"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        QCOMPARE(s.snapshotCount(), 13);
        QCOMPARE(s.photonAxis, 1);
        QVERIFY(std::abs(global(s, QStringLiteral("qw.Lw")) - L) < 1e-15);
        const double eps0 = 8.8541878188e-12, c0 = 299792458.0, Ep = 25 * Q;
        for (int k = 0; k < s.snapshotCount(); k += 3) {
            s.select(k);
            const double E = global(s, QStringLiteral("qw.Ephoton"));
            double te = 0, tm = 0;
            for (int n = 1; n <= 3; ++n) {
                const double ee = HB2M * n * n * M_PI * M_PI / (0.067 * 100);
                for (int c = 0; c < 2; ++c) {
                    const double mz = c == 0 ? 0.35 : 0.09, mp = c == 0 ? 0.11 : 0.21;
                    const double eh = -1.5 - HB2M * n * n * M_PI * M_PI / (mz * 100);
                    const double Et = (ee - eh) * Q, mr = 1 / (1 / 0.067 + 1 / mp) * M0;
                    const double K = M_PI * Q * Q * Hbar / (3.6 * c0 * eps0 * M0 * M0 * E) * (M0 * Ep / 6) * mr / (M_PI * Hbar * Hbar * L)
                                     * (0.5 + std::atan((E - Et) / (gamma * Q)) / M_PI);
                    te += K * (c == 0 ? 1.5 : 0.5);
                    tm += K * (c == 0 ? 0 : 2);
                }
            }
            const double fte = global(s, QStringLiteral("qw.alphaTE")), ftm = global(s, QStringLiteral("qw.alphaTM"));
            qInfo("absorption at %.3f eV: TE %.6g /cm (%.6g), TM %.6g (%.6g)", E / Q, fte / 100, te / 100, ftm / 100, tm / 100);
            QVERIFY2(std::abs(fte / te - 1) < 1e-4, qPrintable(QString::number(fte / te)));
            QVERIFY2(std::abs(ftm / tm - 1) < 1e-4, qPrintable(QString::number(ftm / tm)));
        }
        // With next to no carriers the occupations' integral (in cells) is
        // the step's own.
        const Run few = run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
                                        QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.5[eV], 2.1[eV], 13)", "states": 3,
                                                             "linewidth": "5[meV]", "density": "1[1/cm^3]"})x"))));
        QVERIFY2(few.error.isEmpty(), qPrintable(few.error));
        for (int k = 0; k < s.snapshotCount(); k += 2) {
            s.select(k);
            few.solution->select(k);
            const double a = global(s, QStringLiteral("qw.alphaTE")), b = global(*few.solution, QStringLiteral("qw.alphaTE"));
            QVERIFY2(std::abs(b / a - 1) < 2e-3, qPrintable(QString::number(b / a)));
        }
    }

    void gain()
    {
        // 3e18 cm⁻³ of carriers in the infinite well: gain below the quasi-
        // Fermi levels' separation, absorption above it (Bernard and
        // Duraffourg) - the crossing at qw.dF; the carriers as asked.
        const Run r = run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
                                      QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.60[eV], 1.80[eV], 401)", "states": 4,
                                                        "linewidth": "0.5[meV]", "density": "3e18[1/cm^3]"})x"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        s.select(0);
        const double dF = global(s, QStringLiteral("qw.dF")), Fc = global(s, QStringLiteral("qw.Fc"));
        // The electrons: Σ m kT/(πħ²) ln(1 + e^((Fc - E_n)/kT)) = n L.
        const double kT = KB * 300;
        double sheet = 0;
        for (int n = 1; n <= 4; ++n) sheet += 0.067 * M0 * kT / (M_PI * Hbar * Hbar) * std::log1p(std::exp((Fc - global(s, QStringLiteral("qw.Ee%1").arg(n))) / kT));
        QVERIFY2(std::abs(sheet / (3e24 * 10e-9) - 1) < 1e-6, qPrintable(QString::number(sheet / 3e24 / 10e-9)));
        double crossing = NAN, peak = 0;
        for (int k = 1; k < s.snapshotCount(); ++k) {
            s.select(k - 1);
            const double g0 = global(s, QStringLiteral("qw.gTE")), e0 = global(s, QStringLiteral("qw.Ephoton"));
            s.select(k);
            const double g1 = global(s, QStringLiteral("qw.gTE")), e1 = global(s, QStringLiteral("qw.Ephoton"));
            peak = std::max(peak, g1);
            if (g0 > 0 && g1 <= 0 && std::isnan(crossing)) crossing = e0 + (e1 - e0) * g0 / (g0 - g1);
        }
        qInfo("gain: peak %.0f /cm; it crosses 0 at %.4f eV, the quasi-Fermi levels %.4f eV apart", peak / 100, crossing / Q, dF / Q);
        QVERIFY(peak > 0);
        QVERIFY2(std::abs(crossing - dF) < 2e-3 * Q, qPrintable(QString::number((crossing - dF) / Q)));
    }

    void theExamples()
    {
        const QString dir = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
        auto load = [&](const QString& name) {
            QFile f(dir + name);
            if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
            Model m = modelOf(f.readAll());
            m.fileName = dir + name;
            return m;
        };
        auto solve = [&](const Model& m, const QString& study) {
            const std::shared_ptr<ParameterScope> p = evaluateParameters(m);
            const GeometryBuild g = buildGeometry(m, *p);
            if (!g.ok()) qFatal("%s: its geometry", qPrintable(m.fileName));
            StudyRun r = runStudy(m, p, g.topology, nullptr, study);
            if (!r.ok()) qFatal("%s, %s: %s", qPrintable(m.fileName), qPrintable(study), qPrintable(r.error));
            return r;
        };
        auto spectrumAt = [](Solution& s, double eV, const char* name) {
            int best = 0;
            for (int k = 0; k < s.snapshotCount(); ++k)
                if (std::abs(s.snapshotFrequency(k) - eV * Q / 6.62607015e-34) < std::abs(s.snapshotFrequency(best) - eV * Q / 6.62607015e-34)) best = k;
            s.select(best);
            return global(s, QString::fromLatin1(name));
        };
        QElapsedTimer timer;
        {
            // The GaAs well at 300 K: e1-hh1 near 1.46 eV (849 nm); its absorption
            // several thousand per cm (a Lorentzian's tail below).
            timer.start();
            const Model m = load(QStringLiteral("gaas_quantum_well.qfem"));
            const StudyRun states = solve(m, QStringLiteral("std1"));
            Solution& s = *states.solutions.front();
            QCOMPARE(s.snapshotCount(), 4);
            const double e11 = eV(s, QStringLiteral("qw.Ee1hh1")), lambda = global(s, QStringLiteral("qw.lambda"));
            const StudyRun spectrum = solve(m, QStringLiteral("std2"));
            Solution& a = *spectrum.solutions.front();
            const double te = spectrumAt(a, e11 + 0.05, "qw.alphaTE"), below = spectrumAt(a, e11 - 0.05, "qw.alphaTE");
            qInfo("GaAs well: e1-hh1 %.4f eV (%.1f nm), overlap %.3f; TE absorption %.0f /cm above it, %.0f below; %lld ms", e11, lambda * 1e9,
                  global(s, QStringLiteral("qw.Oe1hh1")), te / 100, below / 100, timer.elapsed());
            QVERIFY(e11 > 1.44 && e11 < 1.48);
            QVERIFY(global(s, QStringLiteral("qw.Oe1hh1")) > 0.95);
            QVERIFY(te > 3e5 && te < 2e6 && below < 0.3 * te);
        }
        {
            // The MQW: each well's own transition red-shifts and its overlap
            // falls as F rises (the lowest and highest states of the stack, in
            // different wells, much more so); the absorption of all of them.
            timer.start();
            const Model m = load(QStringLiteral("ingaas_mqw_qcse.qfem"));
            const StudyRun states = solve(m, QStringLiteral("std1"));
            QCOMPARE(int(states.solutions.size()), 3);
            std::vector<double> direct, overlap, indirect;
            for (const auto& s : states.solutions) {
                s->select(0);
                direct.push_back(eV(*s, QStringLiteral("qw.Edir")));
                overlap.push_back(global(*s, QStringLiteral("qw.Odir")));
                indirect.push_back(eV(*s, QStringLiteral("qw.Ee1hh1")));
            }
            const StudyRun spectrum = solve(m, QStringLiteral("std2"));
            QCOMPARE(int(spectrum.solutions.size()), 3);
            const double above = spectrumAt(*spectrum.solutions.front(), direct[0] + 0.03, "qw.alphaTE");
            qInfo("MQW: its wells' own e1-hh1 %.4f, %.4f, %.4f eV (%.0f nm unbiased), overlaps %.3f, %.3f, %.3f; the stack's e1-hh1 %.4f, "
                  "%.4f, %.4f; TE absorption %.0f /cm above the edge; %lld ms",
                  direct[0], direct[1], direct[2], 1239.84 / direct[0], overlap[0], overlap[1], overlap[2], indirect[0], indirect[1], indirect[2],
                  above / 100, timer.elapsed());
            QVERIFY(1239.84 / direct[0] > 1450 && 1239.84 / direct[0] < 1600);
            QVERIFY(direct[1] < direct[0] && direct[2] < direct[1] && direct[0] - direct[2] > 0.005);
            QVERIFY(overlap[1] < overlap[0] && overlap[2] < overlap[1] && overlap[0] > 0.95);
            QVERIFY(std::abs(indirect[0] - direct[0]) < 1e-3 && indirect[2] < direct[2] - 0.3);
            QVERIFY(above > 3e5 && above < 2e6);
        }
        {
            // The strained laser: TE gain near 980 nm, far above TM; more with more carriers.
            timer.start();
            const Model m = load(QStringLiteral("strained_laser_gain.qfem"));
            const StudyRun gain = solve(m, QStringLiteral("std1"));
            QCOMPARE(int(gain.solutions.size()), 4);
            std::vector<double> peak, at, tm;
            for (const auto& s : gain.solutions) {
                double best = -1e300, where = 0, bestTM = -1e300;
                for (int k = 0; k < s->snapshotCount(); ++k) {
                    s->select(k);
                    const double g = global(*s, QStringLiteral("qw.gTE"));
                    if (g > best) {
                        best = g;
                        where = 299792458.0 / s->snapshotFrequency(k);
                    }
                    bestTM = std::max(bestTM, global(*s, QStringLiteral("qw.gTM")));
                }
                peak.push_back(best);
                at.push_back(where);
                tm.push_back(bestTM);
            }
            qInfo("laser: TE gain peaks %.0f, %.0f, %.0f, %.0f /cm (at %.1f nm with 3e18 cm⁻³; TM %.0f); %lld ms", peak[0] / 100, peak[1] / 100,
                  peak[2] / 100, peak[3] / 100, at[2] * 1e9, tm[2] / 100, timer.elapsed());
            for (std::size_t k = 1; k < peak.size(); ++k) QVERIFY(peak[k] > peak[k - 1]);
            QVERIFY(peak[2] > 1e5 && peak[2] < 1e6);
            QVERIFY(at[2] > 940e-9 && at[2] < 1020e-9);
            QVERIFY(tm[2] < 0.3 * peak[2]);
        }
        {
            // The wire: its second electron state one node across the width.
            timer.start();
            Model m = load(QStringLiteral("quantum_wire.qfem"));
            if (slowBuild()) {
                Node* mesh = m.find(m.mesh().tag);
                mesh->props.insert(QStringLiteral("hmax"), QStringLiteral("5"));
                for (Node& size : mesh->children) size.props.insert(QStringLiteral("hmax"), QStringLiteral("1.6"));
            }
            const StudyRun states = solve(m, QStringLiteral("std1"));
            Solution& s = *states.solutions.front();
            const double e1 = eV(s, QStringLiteral("qw.Ee1")), e2 = eV(s, QStringLiteral("qw.Ee2"));
            s.select(1);
            const double left = at(s, QStringLiteral("qw.psi_e"), {-5, 0}), right = at(s, QStringLiteral("qw.psi_e"), {5, 0});
            const StudyRun spectrum = solve(m, QStringLiteral("std2"));
            Solution& a = *spectrum.solutions.front();
            s.select(0);
            const double first = eV(s, QStringLiteral("qw.Ee1hh1"));
            const double te = spectrumAt(a, first + 0.01, "qw.alphaTE");
            qInfo("wire: e1 %.4f, e2 %.4f eV; the second state %.3f and %.3f either side; TE absorption %.0f /cm just above e1-hh1 (%.4f eV); %lld ms",
                  e1, e2, left, right, te / 100, first, timer.elapsed());
            QVERIFY(e2 - e1 > 0.005 && e2 - e1 < 0.05);
            QVERIFY(left * right < 0);
            QVERIFY(te > 0);
        }
    }
};

QTEST_MAIN(TestFemQuantum)
#include "test_fem_quantum.moc"
