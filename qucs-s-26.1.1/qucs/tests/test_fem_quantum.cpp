/*
 * test_fem_quantum.cpp - the multiphysics solver's quantum wells against
 *                        what is known: an infinite well's levels, a finite
 *                        one's (BenDaniel and Duke's matching), a box's in
 *                        2D with its masses along and across the growth
 *                        axis, a harmonic oscillator's, the Stark shift,
 *                        strain's shifts, the alloys' gaps and lattices, 1D
 *                        against 2D, absorption's and gain's spectra; the
 *                        k·p valence bands against Luttinger and Kohn's and
 *                        across interfaces; excitons against published ones
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_edge.h"
#include "fem_materials.h"
#include "fem_results.h"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QtTest>

#include <array>
#include <cmath>
#include <complex>
#include <cstring>
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

/// Under the sanitizers a k·p test lighter: the line's elements 0.25 nm, the
/// spectrum's k at 41 values.
QByteArray lighter(QByteArray json)
{
    if (!slowBuild()) return json;
    json.replace(R"("tag": "qw")", R"("tag": "qw", "h1d": "0.25[nm]")");
    json.replace(R"("type": "spectrum")", R"("type": "spectrum", "kpoints": 41)");
    return json;
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

    void theAlloysAt300K()
    {
        // The 300 K fit against what was measured there: InP 1.344 eV,
        // In0.53Ga0.47As 0.750 (Goetz et al.), In0.52Al0.48As 1.46 (of 1.45
        // to 1.47), InGaAsP on InP's lattice Nahory et al.'s 1.35 - 0.72 y +
        // 0.12 y² (its ends InP's and InGaAs's: within 7 meV), the 1.3 µm
        // AlGaInAs 0.954 - where Vurgaftman's are 9-16 meV off. Varshni's
        // slope kept; the default still Vurgaftman's.
        const BandSet fit = BandSet::Measured;
        auto eg = [](const char* c, double x, double y, double T, BandSet set) { return bandParameters(QString::fromLatin1(c), x, y, T, nullptr, set)->Eg; };
        struct Known {
            const char* compound;
            double x, y, measured, tolerance;
        };
        const Known known[] = {{"InP", 0, 0, 1.344, 1e-6},          {"GaAs", 0, 0, 1.424, 1e-6},         {"InAs", 0, 0, 0.354, 1e-6},
                               {"InGaAs", 0.53, 0, 0.750, 1e-3},    {"InAlAs", 0.52, 0, 1.460, 1e-3},    {"InGaAsP", 0.28, 0.61, 0.9555, 7e-3},
                               {"AlGaInAs", 0.165, 0.305, 0.954, 1e-3}};
        for (const Known& k : known) {
            const double v = eg(k.compound, k.x, k.y, 300, BandSet::Vurgaftman), m = eg(k.compound, k.x, k.y, 300, fit);
            qInfo("%s (%g, %g): Vurgaftman's %.4f eV, the fit %.4f, measured %.4f", k.compound, k.x, k.y, v, m, k.measured);
            QVERIFY2(std::abs(m - k.measured) < k.tolerance, k.compound);
        }
        QVERIFY(std::abs(eg("InP", 0, 0, 300, BandSet::Vurgaftman) - 1.3529) < 1e-4);
        QCOMPARE(eg("InP", 0, 0, 300, BandSet::Vurgaftman), bandParameters(QStringLiteral("InP"), 0, 0, 300)->Eg);
        double worst = 0;
        for (int i = 1; i < 10; ++i) {
            const double y = i / 10.0, x = 0.1894 * y / (0.4184 - 0.013 * y);
            worst = std::max(worst, std::abs(eg("InGaAsP", x, y, 300, fit) - (1.35 - 0.72 * y + 0.12 * y * y)));
        }
        qInfo("InGaAsP on InP's lattice: within %.4f eV of Nahory's", worst);
        QVERIFY(worst < 7.5e-3);
        // At 77 K: Varshni's change from 300 K, either set.
        for (const char* c : {"InP", "GaAs"})
            QVERIFY(std::abs((eg(c, 0, 0, 77, fit) - eg(c, 0, 0, 300, fit)) - (eg(c, 0, 0, 77, BandSet::Vurgaftman) - eg(c, 0, 0, 300, BandSet::Vurgaftman))) < 1e-12);
        // Only the gaps: the masses, offsets and lattice the same.
        const BandParameters a = *bandParameters(QStringLiteral("InGaAs"), 0.53, 0, 300), b = *bandParameters(QStringLiteral("InGaAs"), 0.53, 0, 300, nullptr, fit);
        QVERIFY(a.me == b.me && a.VBO == b.VBO && a.a0 == b.a0 && a.gamma1 == b.gamma1);
        QCOMPARE(bandSetOf(QStringLiteral(" Measured ")), BandSet::Measured);
        QCOMPARE(bandSetOf(QString()), BandSet::Vurgaftman);
        QVERIFY(compoundProperty(QStringLiteral("InP"), 0, 0, QStringLiteral("Eg"), fit).startsWith(QLatin1String("1.344")));
    }

    void oneMaterialOneGap()
    {
        // One material, one gap in every physics: the semiconductor's and the
        // quantum wells' band edges 20 nm into a slab of it, at 300 K -
        // - the library's Indium phosphide: its measured 1.344 eV in both
        //   (its compound's 1.353 had been the wells');
        // - In0.53Ga0.47As with the 300 K fit: 0.750 in both;
        // - a quaternary given by its gap's wavelength, 1.3 µm: hc/λg;
        // - a gap given, the wells at 77 K: moved as the compound's moves.
        struct Case {
            const char* material;
            const char* T;
            double expected;   // eV
        };
        const double lambdaG = 6.62607015e-34 * 299792458.0 / 1.3e-6 / Q;
        const double inP77 = 1.344 + bandParameters(QStringLiteral("InP"), 0, 0, 77)->Eg - bandParameters(QStringLiteral("InP"), 0, 0, 300)->Eg;
        const Case cases[] = {{R"("material": "Indium phosphide")", "300[K]", 1.344},
                              {R"("material": "m", "compound": "InGaAs", "x": "0.53", "bands": "measured")", "300[K]",
                               bandParameters(QStringLiteral("InGaAs"), 0.53, 0, 300, nullptr, BandSet::Measured)->Eg},
                              {R"("material": "q", "compound": "InGaAsP", "x": "0.28", "y": "0.61", "lambdag": "1.3[um]")", "300[K]", lambdaG},
                              {R"("material": "Indium phosphide")", "77[K]", inP77}};
        for (const Case& c : cases) {
            const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "label": "slab", "size": ["40", "40"]}],
                "materials": [{%1, "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                              {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
                              {"type": "ohmic", "name": "l", "V0": "0[V]", "selection": {"of": ["slab"], "side": "left"}}]},
                            {"type": "quantum", "tag": "qw", "at": "20[nm]", "T": "%2"}],
                "mesh": {"size": "normal", "custom": true, "hmax": "5"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary", "physics": ["semi"]},
                                                      {"type": "states", "physics": ["qw"], "states": 1}]}]})json")
                                         .arg(QLatin1String(c.material), QLatin1String(c.T))
                                         .toUtf8();
            const Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.solution;
            const QPointF middle(20, 20);
            const double semi = (at(s, QStringLiteral("semi.Ec"), middle) - at(s, QStringLiteral("semi.Ev"), middle)) / Q;
            const double wells = (at(s, QStringLiteral("qw.Ec"), middle) - at(s, QStringLiteral("qw.Ev"), middle)) / Q;
            qInfo("%s at %s: the semiconductor's gap %.6f eV, the wells' %.6f (%.6f)", c.material, c.T, semi, wells, c.expected);
            QVERIFY2(std::abs(wells - c.expected) < 1e-6, c.material);
            if (QLatin1String(c.T) == QLatin1String("300[K]")) QVERIFY2(std::abs(semi - c.expected) < 1e-6, c.material);
        }
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

    void lineShapes()
    {
        // The infinite well's spectrum (absorption()'s) under each line
        // shape, γ its half width at half maximum: each step broadened by the
        // shape's cumulative - a Lorentzian's 1/2 + atan(x/γ)/π, a Gaussian's
        // erfc(-x/σ√2)/2 (σ = γ/√(2 ln 2)), a sech's (2/π) atan(e^(x/w)) (w =
        // γ/acosh 2) - all transitions summed. At the step's edge plus γ:
        // 0.75, 0.8805, 0.8333 of its height.
        const double gamma = 0.005 * Q, L = 10e-9, eps0 = 8.8541878188e-12, c0 = 299792458.0, Ep = 25 * Q;
        const double sigma = gamma / std::sqrt(2 * std::log(2.0)), w = gamma / std::acosh(2.0);
        const char* shapes[] = {"lorentzian", "gaussian", "sech"};
        for (int shape = 0; shape < 3; ++shape) {
            auto cdf = [&](double x) {
                if (shape == 1) return 0.5 * std::erfc(-x / (sigma * std::sqrt(2.0)));
                if (shape == 2) return 2 / M_PI * std::atan(std::exp(x / w));
                return 0.5 + std::atan(x / gamma) / M_PI;
            };
            const Run r = run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
                                          QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.5[eV], 2.1[eV], 61)", "states": 3,
                                                            "linewidth": "5[meV]", "lineshape": "%1"})x")
                                              .arg(QLatin1String(shapes[shape])))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.solution;
            double worst = 0;
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                const double E = global(s, QStringLiteral("qw.Ephoton"));
                double te = 0;
                for (int n = 1; n <= 3; ++n) {
                    const double ee = HB2M * n * n * M_PI * M_PI / (0.067 * 100);
                    for (int c = 0; c < 2; ++c) {
                        const double mz = c == 0 ? 0.35 : 0.09, mp = c == 0 ? 0.11 : 0.21;
                        const double eh = -1.5 - HB2M * n * n * M_PI * M_PI / (mz * 100);
                        const double Et = (ee - eh) * Q, mr = 1 / (1 / 0.067 + 1 / mp) * M0;
                        te += M_PI * Q * Q * Hbar / (3.6 * c0 * eps0 * M0 * M0 * E) * (M0 * Ep / 6) * mr / (M_PI * Hbar * Hbar * L) * cdf(E - Et)
                              * (c == 0 ? 1.5 : 0.5);
                    }
                }
                if (te > 1e-3 * 1e4) worst = std::max(worst, std::abs(global(s, QStringLiteral("qw.alphaTE")) / te - 1));
            }
            qInfo("%s: the spectrum its steps' to %.2g; at the edge plus γ %.4f of the step", shapes[shape], worst, cdf(gamma));
            QVERIFY2(worst < 1e-4, shapes[shape]);
        }
    }

    void excitons()
    {
        // The infinite well (10 nm: me 0.067, the heavy hole's in-plane mass
        // 0.11, εr 12.9), its e1-hh1 exciton, against the variational one
        // found another way: sin² envelopes, their separation's distribution
        // P(s), G(s, λ) = (4/λ²) ∫ ρ e^(-2ρ/λ)/√(ρ² + s²) dρ = (4s/λ²)
        // ((π/2)(H1(2s/λ) - Y1(2s/λ)) - 1) in Struve's and Bessel's functions,
        // the least of ħ²/(2µλ²) - q²/(4πε) ∫ P G ds (SciPy, its integrals to
        // 1e-10): bound by 8.31602 meV, 12.60235 nm across. And:
        // - with a Gaussian line, the peak below the edge K (4/πλ²) its
        //   density - the 1s line's strength |F(0)|² for each of the two
        //   spin pairs the continuum counts;
        // - above the edge the continuum raised by the 2D Sommerfeld factor
        //   2/(1 + e^(-2π √(R/ε))), R = Eb/4;
        // - pulled by 100 kV/cm (the quantum-confined Stark effect) less
        //   bound, its line lower;
        // - with carriers none (screened).
        const double L = 10e-9, epsr = 12.9, mr = 1 / (1 / 0.067 + 1 / 0.11) * M0, eps0 = 8.8541878188e-12;
        const double EbRef = 8.31602e-3 * Q, lambdaRef = 12.60235e-9;
        const double ry = 13.605693 * (mr / M0) / (epsr * epsr);   // eV
        // The edge: e1 + hh1 above the gap.
        const double Et = (1.5 + HB2M * M_PI * M_PI / (0.067 * 100) + HB2M * M_PI * M_PI / (0.35 * 100)) * Q;
        const double Ex = Et - EbRef;
        auto spectrum = [&](const QString& energies, const QString& physics, const QString& more) {
            return run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), physics, QStringLiteral("0[eV]"),
                                   QString::fromUtf8(R"x({"type": "spectrum", "energies": "%1", "states": 2, "linewidth": "0.5[meV]",
                                                     "lineshape": "gaussian", "excitons": true, "excitonmodel": "bastard", "epsr": "12.9" %2})x")
                                       .arg(energies, more))));
        };
        const QString around = QStringLiteral("linspace(%1[eV], %2[eV], 13)").arg(Ex / Q - 1.2e-3, 0, 'f', 7).arg(Ex / Q + 1.2e-3, 0, 'f', 7);
        const Run r = spectrum(around, QString(), QString());
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        s.select(0);
        const double Eb = global(s, QStringLiteral("qw.Eb")), radius = global(s, QStringLiteral("qw.aX")), ex = global(s, QStringLiteral("qw.Ex"));
        qInfo("exciton: bound by %.4f meV (%.4f), %.4f nm across (%.4f); %.2f of the 3D Rydberg %.3f meV (2D's 4)", Eb / Q * 1e3, EbRef / Q * 1e3,
              radius * 1e9, lambdaRef * 1e9, Eb / Q / ry, ry * 1e3);
        QVERIFY2(std::abs(Eb / EbRef - 1) < 1e-4, qPrintable(QString::number(Eb / EbRef)));
        QVERIFY2(std::abs(radius / lambdaRef - 1) < 1e-4, qPrintable(QString::number(radius / lambdaRef)));
        QVERIFY(std::abs(ex - (Et - Eb)) < 1e-6 * Q);
        QVERIFY(Eb / Q > ry && Eb / Q < 4 * ry);
        // The 1s line: K (2/πλ²) its Gaussian's density, TE 3/2 of |Mb|².
        const double c0 = 299792458.0, Ep = 25 * Q, sigma = 0.5e-3 * Q / std::sqrt(2 * std::log(2.0));
        double worst = 0;
        for (int k = 0; k < s.snapshotCount(); ++k) {
            s.select(k);
            const double E = global(s, QStringLiteral("qw.Ephoton"));
            const double K = M_PI * Q * Q * Hbar / (3.6 * c0 * eps0 * M0 * M0 * E) * (M0 * Ep / 6) / L;
            const double expected = K * 1.5 * 4 / (M_PI * radius * radius) * std::exp(-0.5 * std::pow((E - ex) / sigma, 2)) / (sigma * std::sqrt(2 * M_PI));
            worst = std::max(worst, std::abs(global(s, QStringLiteral("qw.alphaTE")) - expected) / (K * 1.5 * 4 / (M_PI * radius * radius) / (sigma * std::sqrt(2 * M_PI))));
        }
        qInfo("the 1s line: within %.2g of its peak", worst);
        QVERIFY2(worst < 1e-3, qPrintable(QString::number(worst)));
        // Above the edge: the continuum the factor S(ε) times the step's.
        const QString above = QStringLiteral("%1[eV], %2[eV]").arg((Et / Q) + 0.004, 0, 'f', 7).arg((Et / Q) + 0.008, 0, 'f', 7);
        const Run with = spectrum(above, QString(), QString());
        QString none = QString::fromUtf8(R"x({"type": "spectrum", "energies": "%1", "states": 2, "linewidth": "0.5[meV]", "lineshape": "gaussian"})x").arg(above);
        const Run without = run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"), none)));
        QVERIFY2(with.error.isEmpty() && without.error.isEmpty(), qPrintable(with.error + without.error));
        for (int k = 0; k < 2; ++k) {
            with.solution->select(k);
            without.solution->select(k);
            const double eps = global(*with.solution, QStringLiteral("qw.Ephoton")) - Et;
            const double S = 2 / (1 + std::exp(-2 * M_PI * std::sqrt(Eb / 4 / eps)));
            const double ratio = global(*with.solution, QStringLiteral("qw.alphaTE")) / global(*without.solution, QStringLiteral("qw.alphaTE"));
            qInfo("%.1f meV above the edge: %.5f times without excitons (S %.5f)", eps / Q * 1e3, ratio, S);
            QVERIFY2(std::abs(ratio / S - 1) < 2e-3, qPrintable(QString::number(ratio / S)));
        }
        // The light holes' too: TM below the e1-lh1 edge, a line above the
        // step's height where without excitons there is next to nothing.
        const double EtLh = (1.5 + HB2M * M_PI * M_PI / (0.067 * 100) + HB2M * M_PI * M_PI / (0.09 * 100)) * Q;
        const QString below = QStringLiteral("linspace(%1[eV], %2[eV], 57)").arg(EtLh / Q - 0.015, 0, 'f', 7).arg(EtLh / Q - 0.001, 0, 'f', 7);
        const Run lh = spectrum(below, QString(), QString());
        QVERIFY2(lh.error.isEmpty(), qPrintable(lh.error));
        double peakTM = 0;
        for (int k = 0; k < lh.solution->snapshotCount(); ++k) {
            lh.solution->select(k);
            peakTM = std::max(peakTM, global(*lh.solution, QStringLiteral("qw.alphaTM")));
        }
        const double mrLh = 1 / (1 / 0.067 + 1 / 0.21) * M0;
        const double stepTM = M_PI * Q * Q * Hbar / (3.6 * c0 * eps0 * M0 * M0 * EtLh) * (M0 * Ep / 6) / L * 2 * mrLh / (M_PI * Hbar * Hbar);
        qInfo("e1-lh1: TM below its edge at most %.4g /cm, its step %.4g", peakTM / 100, stepTM / 100);
        QVERIFY(peakTM > 2 * stepTM);
        // Pulled apart by a field: less bound, its line lower (the edge
        // lower still).
        const Run pulled = spectrum(around, QStringLiteral(R"(, "F": "1e7[V/m]")"), QString());
        QVERIFY2(pulled.error.isEmpty(), qPrintable(pulled.error));
        pulled.solution->select(0);
        const double EbF = global(*pulled.solution, QStringLiteral("qw.Eb")), exF = global(*pulled.solution, QStringLiteral("qw.Ex"));
        qInfo("at 100 kV/cm: bound by %.4f meV, its line %.3f meV lower", EbF / Q * 1e3, (ex - exF) / Q * 1e3);
        QVERIFY(EbF < Eb && exF < ex);
        // With carriers: screened, left out.
        const Run screened = spectrum(around, QString(), QStringLiteral(R"(, "density": "1e17[1/cm^3]")"));
        QVERIFY2(screened.error.isEmpty(), qPrintable(screened.error));
        screened.solution->select(0);
        QVERIFY(!screened.solution->global(QStringLiteral("qw.Eb")));
        QVERIFY(screened.solution->log.join(QLatin1Char(' ')).contains(QLatin1String("screen")));
    }

    void excitonsIn2D()
    {
        // Elliott's 2D limit: the 1s line carries 8 Eb of the bare
        // continuum's step (|φ(0)|² = 4 mr Eb/(πħ²) per spin pair against
        // the step's mr/(2πħ²)). The variational exciton reaches it as the
        // well thins - its kinetic energy ħ²/(2µλ²) its binding at last -,
        // here measured: the line's ∫ α E dE over the step's α E.
        auto ratio = [&](double L) {
            const double Et = (1.5 + HB2M * M_PI * M_PI / (0.067 * L * L) + HB2M * M_PI * M_PI / (0.35 * L * L)) * Q;
            const QString height = QString::number(L), hmax = QString::number(L / 20);
            const Run x = run(modelOf(box(QStringLiteral("20"), height, QString(), QStringLiteral("0[eV]"),
                                          QStringLiteral(R"x({"type": "spectrum", "energies": "%1[eV]", "states": 1, "linewidth": "0.5[meV]", "lineshape": "gaussian",
                                                           "excitons": true, "epsr": "12.9"})x").arg(Et / Q, 0, 'f', 7), hmax)));
            if (!x.error.isEmpty()) qFatal("%s", qPrintable(x.error));
            x.solution->select(0);
            const double Eb = global(*x.solution, QStringLiteral("qw.Eb")), Ex = global(*x.solution, QStringLiteral("qw.Ex"));
            const QString window = QStringLiteral("linspace(%1[eV], %2[eV], 241)").arg(Ex / Q - 0.003, 0, 'f', 7).arg(Ex / Q + 0.003, 0, 'f', 7);
            const Run line = run(modelOf(box(QStringLiteral("20"), height, QString(), QStringLiteral("0[eV]"),
                                             QStringLiteral(R"x({"type": "spectrum", "energies": "%1", "states": 1, "linewidth": "0.5[meV]", "lineshape": "gaussian",
                                                              "excitons": true, "epsr": "12.9"})x").arg(window), hmax)));
            // (The edge the solver's: Ex + Eb.)
            const Run step = run(modelOf(box(QStringLiteral("20"), height, QString(), QStringLiteral("0[eV]"),
                                             QStringLiteral(R"x({"type": "spectrum", "energies": "%1[eV]", "states": 1, "linewidth": "0.5[meV]", "lineshape": "gaussian"})x")
                                                 .arg((Ex + Eb) / Q + 0.005, 0, 'f', 7), hmax)));
            if (!line.error.isEmpty() || !step.error.isEmpty()) qFatal("%s", qPrintable(line.error + step.error));
            double integral = 0;
            for (int k = 1; k < line.solution->snapshotCount(); ++k) {
                line.solution->select(k - 1);
                const double e0 = global(*line.solution, QStringLiteral("qw.Ephoton")), a0 = global(*line.solution, QStringLiteral("qw.alphaTE"));
                line.solution->select(k);
                const double e1 = global(*line.solution, QStringLiteral("qw.Ephoton")), a1 = global(*line.solution, QStringLiteral("qw.alphaTE"));
                integral += 0.5 * (a0 * e0 + a1 * e1) * (e1 - e0);
            }
            step.solution->select(0);
            const double aStep = global(*step.solution, QStringLiteral("qw.alphaTE")) * global(*step.solution, QStringLiteral("qw.Ephoton"));
            const double r = integral / (aStep * 8 * Eb);
            qInfo("a well %.1f nm: bound by %.3f meV (2D's %.3f), %.3f nm across; the 1s line %.4f of 8 Eb times the step", L, Eb / Q * 1e3,
                  4 * 13.605693e3 * (1 / (1 / 0.067 + 1 / 0.11)) / (12.9 * 12.9), global(*x.solution, QStringLiteral("qw.aX")) * 1e9, r);
            return r;
        };
        const double thin = ratio(0.5), wide = ratio(4);
        ratio(1);
        QVERIFY2(std::abs(thin - 1) < 0.05, qPrintable(QString::number(thin)));
        QVERIFY(wide < thin);
    }

    void excitonsAgainstKhramtsov()
    {
        // Published: Khramtsov et al., "Excitons in square quantum wells:
        // microscopic modeling and experiment" (arXiv:1508.00480) -
        // GaAs/Al0.3Ga0.7As, their Table I: εr
        // 12.53, ΔEg 365.5 meV split 0.65/0.35, me 0.067, the heavy hole's
        // 0.377 along the axis and 0.111 across it, the barriers' masses the
        // well's. The heavy-hole exciton bound, exactly (the 3D Schrödinger
        // equation solved), by Rx(L) = (3.5431 L² + 146.694 L)/(L² + 11.1758 L
        // + 6.0473) meV to 0.07 meV; its radiative decay ħΓ0, measured (their
        // Table II), 36.6 ± 0.6 µeV at 14 nm and 35.4 ± 0.4 at 20 (Ep 28.8 eV,
        // n 3.6, Eg 1.520 eV). Here Bastard's one-parameter exciton: bound
        // within 7 % of the exact for 2 to 20 nm; its line's ∫α dE Lw/(2π) -
        // Ivchenko's absorbance 2Γ0Γ/((ω-ω0)² + (Γ+Γ0)²), its integral 2πΓ0
        // - within 15 % of what was measured.
        auto stack = [](double L, const QString& energies, const char* model) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "25"]},
                             {"type": "rectangle", "name": "well", "position": ["0", "25"], "size": ["10", "%1"]},
                             {"type": "rectangle", "name": "above", "position": ["0", "%2"], "size": ["10", "25"]}],
                "physics": [{"type": "quantum", "tag": "qw", "light": false, "features": [
                  {"type": "material", "source": "user", "Ec": "1.520[eV]", "Ev": "0[eV]", "me": "0.067", "mhh": "0.377", "mlh": "0.09",
                   "mhhp": "0.111", "mlhp": "0.21", "Ep": "28.8[eV]", "nr": "3.6", "epsilonr": "12.53", "selection": {"only": ["well"]}},
                  {"type": "material", "source": "user", "Ec": "1.757575[eV]", "Ev": "-0.127925[eV]", "me": "0.067", "mhh": "0.377", "mlh": "0.09",
                   "mhhp": "0.111", "mlhp": "0.21", "Ep": "28.8[eV]", "nr": "3.6", "epsilonr": "12.53", "selection": {"only": ["below", "above"]}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%3"}}],
              "studies": [{"type": "study", "steps": [{"type": "spectrum", "energies": "%4", "states": 1, "linewidth": "0.2[meV]",
                                                      "lineshape": "gaussian", "excitons": true, "excitonmodel": "%5"}]}]})json")
                .arg(L)
                .arg(25 + L)
                .arg(std::min(0.5, L / 10))
                .arg(energies, QLatin1String(model))
                .toUtf8();
        };
        auto exact = [](double L) { return (3.5431 * L * L + 146.694 * L) / (L * L + 11.1758 * L + 6.0473) * 1e-3 * Q; };
        double worstEb[2] = {0, 0}, worstG[2] = {0, 0};
        int offTheTable = 0;   // γ's found between the Coulomb table's nodes (0.025 apart)
        for (int model = 0; model < 2; ++model) {
            const char* name = model == 0 ? "two" : "bastard";
            for (double L : {2.0, 5.0, 10.0, 14.0, 20.0}) {
                const Run x = run(modelOf(stack(L, QStringLiteral("1.6[eV]"), name)));
                QVERIFY2(x.error.isEmpty(), qPrintable(x.error));
                x.solution->select(0);
                const double Eb = global(*x.solution, QStringLiteral("qw.Eb")), Ex = global(*x.solution, QStringLiteral("qw.Ex"));
                worstEb[model] = std::max(worstEb[model], std::abs(Eb / exact(L) - 1));
                QString radiative;
                if (L == 14 || L == 20) {
                    // The line alone: ±1.5 meV about it (its σ 0.085 meV; the
                    // continuum Eb above, the light holes' further).
                    const QString window = QStringLiteral("linspace(%1[eV], %2[eV], 301)").arg(Ex / Q - 0.0015, 0, 'f', 7).arg(Ex / Q + 0.0015, 0, 'f', 7);
                    const Run line = run(modelOf(stack(L, window, name)));
                    QVERIFY2(line.error.isEmpty(), qPrintable(line.error));
                    double integral = 0;
                    for (int k = 1; k < line.solution->snapshotCount(); ++k) {
                        line.solution->select(k - 1);
                        const double e0 = global(*line.solution, QStringLiteral("qw.Ephoton")), a0 = global(*line.solution, QStringLiteral("qw.alphaTE"));
                        line.solution->select(k);
                        const double e1 = global(*line.solution, QStringLiteral("qw.Ephoton")), a1 = global(*line.solution, QStringLiteral("qw.alphaTE"));
                        integral += 0.5 * (a0 + a1) * (e1 - e0);
                    }
                    const double G = integral * L * 1e-9 / (2 * M_PI), measured = (L == 14 ? 36.6e-6 : 35.4e-6) * Q;
                    worstG[model] = std::max(worstG[model], std::abs(G / measured - 1));
                    radiative = QStringLiteral("; ħΓ0 %1 µeV (measured %2)").arg(G / Q * 1e6, 0, 'f', 1).arg(measured / Q * 1e6, 0, 'f', 1);
                }
                const double gX = global(*x.solution, QStringLiteral("qw.gX"));
                if (model == 0 && std::abs(gX / 0.025 - std::round(gX / 0.025)) * 0.025 > 2e-3) ++offTheTable;
                qInfo("%s, %4.0f nm: bound by %.3f meV (exact %.3f), %.2f nm across, γ %.3f%s", name, L, Eb / Q * 1e3, exact(L) / Q * 1e3,
                      global(*x.solution, QStringLiteral("qw.aX")) * 1e9, global(*x.solution, QStringLiteral("qw.gX")), qPrintable(radiative));
            }
            qInfo("%s: bound within %.1f %% of the exact; the radiative decay within %.1f %% of what was measured", name, worstEb[model] * 100,
                  worstG[model] * 100);
        }
        QVERIFY2(worstEb[0] < 0.02, qPrintable(QString::number(worstEb[0])));
        QVERIFY2(worstG[0] < 0.1, qPrintable(QString::number(worstG[0])));
        QVERIFY2(worstEb[1] < 0.07, qPrintable(QString::number(worstEb[1])));
        // The table interpolated smoothly: γ found anywhere, not stuck at its
        // nodes (bilinear, each node a minimum of its own).
        QVERIFY2(offTheTable >= 3, qPrintable(QString::number(offTheTable)));
    }

    void kpBurtForeman()
    {
        // Across an interface the X-Z coupling in Burt and Foreman's order,
        // k (N+ kz + kz N-), N- = M - ħ²/2m0: an InGaAs-like well 6 nm (γ 10.70,
        // 4.26, 5.0) in InP-like barriers (5.0, 1.667, 2.1), 0.35 eV below,
        // k along [100], Δ 0 - the 6x6 its X, Y, Z twice - against linear
        // elements of the same order written here (Richardson's of 0.04 and
        // 0.02 nm: to 1e-8 eV), the highest three at 0.5 and 1.0 /nm;
        // symmetrized, (N kz + kz N)/2, they are tens of meV apart (at 1 /nm
        // two states in the gap). (Central differences are no reference
        // here: across the steps their odd-even modes wander with h.) And with InGaAs and InP's
        // own parameters (In0.53, 6x6, 0.34 eV split-off) no band rises
        // above the top at k = 0 to 2.6 /nm - symmetrized, one crossed into
        // the gap.
        const double b = 10.025, w = 6;
        struct Gamma {
            double g1, g2, g3, ev;
        };
        const Gamma well{(1 / 0.46 + 1 / 0.052) / 2, (1 / 0.052 - 1 / 0.46) / 4, 5.0, 0}, barrier{(1 / 0.6 + 1 / 0.12) / 2, (1 / 0.12 - 1 / 0.6) / 4, 2.1, -0.35};
        using cd = std::complex<double>;
        const cd I(0, 1);
        // Linear elements (nodes on the interfaces) of X, Y, Z: H = Ev +
        // diag(L, M, M) k² + kz diag(M, M, L) kz + k (X-Z: N+ kz + kz N-, Z-X: kz
        // N+ + N- kz), the highest three by shift and invert; from h and h/2
        // Richardson's.
        auto linear = [&](double h, double k, bool burtForeman) {
            std::vector<double> z{0.0};
            for (const auto& [lo, hi] : {std::pair{0.0, b}, std::pair{b, b + w}, std::pair{b + w, 2 * b + w}}) {
                const int m = int(std::lround((hi - lo) / h));
                for (int i = 1; i <= m; ++i) z.push_back(lo + (hi - lo) * i / m);
            }
            const int nodes = int(z.size()), n = nodes - 2, n3 = 3 * n;
            std::vector<Eigen::Triplet<cd>> th, tm;
            for (int e = 0; e + 1 < nodes; ++e) {
                const double he = z[std::size_t(e) + 1] - z[std::size_t(e)], mid = 0.5 * (z[std::size_t(e)] + z[std::size_t(e) + 1]);
                const Gamma& g = mid > b && mid < b + w ? well : barrier;
                const double L = -(g.g1 + 4 * g.g2), M = -(g.g1 - 2 * g.g2), N = -6 * g.g3;
                const double Nm = burtForeman ? M - 1 : N / 2, Np = N - Nm;
                const double kz[3] = {M, M, L}, k2[3] = {L, M, M};
                for (int a = 0; a < 2; ++a)
                    for (int c = 0; c < 2; ++c) {
                        const int i = e + a - 1, j = e + c - 1;
                        if (i < 0 || i >= n || j < 0 || j >= n) continue;
                        const double mass = (a == c ? 2.0 : 1.0) * he / 6, stiff = (a == c ? 1.0 : -1.0) / he;
                        const double right = c == 1 ? 0.5 : -0.5, left = a == 1 ? 0.5 : -0.5;   // ∫ φi φj', ∫ φi' φj
                        for (int o = 0; o < 3; ++o) {
                            th.emplace_back(o * n + i, o * n + j, HB2M * (kz[o] * stiff + k2[o] * k * k * mass) + g.ev * mass);
                            tm.emplace_back(o * n + i, o * n + j, mass);
                        }
                        th.emplace_back(0 * n + i, 2 * n + j, HB2M * -I * k * (Np * right - Nm * left));
                        th.emplace_back(2 * n + i, 0 * n + j, HB2M * -I * k * (Nm * right - Np * left));
                    }
            }
            Eigen::SparseMatrix<cd> H(n3, n3), Mm(n3, n3);
            H.setFromTriplets(th.begin(), th.end());
            Mm.setFromTriplets(tm.begin(), tm.end());
            const double sigma = 0.1;
            Eigen::SparseMatrix<cd> A = sigma * Mm - H;
            Eigen::SimplicialLDLT<Eigen::SparseMatrix<cd>> ldlt(A);
            const EigenResult r = largestEigenvalues(n3, 6, [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) { y = ldlt.solve(Mm * x); });
            std::vector<double> e;
            for (const cd& t : r.values) e.push_back(sigma - 1 / t.real());
            std::sort(e.rbegin(), e.rend());
            e.resize(3);
            return e;
        };
        auto reference = [&](double k, bool burtForeman) {
            const double h = slowBuild() ? 0.08 : 0.04;
            const std::vector<double> coarse = linear(h, k, burtForeman), fine = linear(h / 2, k, burtForeman);
            std::array<double, 3> out;
            for (int m = 0; m < 3; ++m) out[std::size_t(m)] = (4 * fine[std::size_t(m)] - coarse[std::size_t(m)]) / 3;
            return out;
        };
        auto user = [](const Gamma& g, double mhh, double mlh, const char* only) {
            (void)g;
            return QStringLiteral(R"({"type": "material", "source": "user", "Ec": "%1[eV]", "Ev": "%2[eV]", "me": "%3", "mhh": "%4", "mlh": "%5", "mhhp": "0.1",
                                     "mlhp": "0.1", "gamma3": "%6", "Delta": "0[eV]", "Ep": "20[eV]", "nr": "3.5", "selection": {"only": [%7]}})")
                .arg(g.ev + 0.8)
                .arg(g.ev)
                .arg(g.ev == 0 ? 0.04 : 0.08)
                .arg(mhh)
                .arg(mlh)
                .arg(g.g3)
                .arg(QLatin1String(only));
        };
        const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "%1"]},
                         {"type": "rectangle", "name": "well", "position": ["0", "%1"], "size": ["10", "%2"]},
                         {"type": "rectangle", "name": "above", "position": ["0", "%3"], "size": ["10", "%1"]}],
            "physics": [{"type": "quantum", "tag": "qw", "valence": "kp6", "inplane": "100", "h1d": "%6[nm]", "features": [%4, %5]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "1"}}],
          "studies": [{"type": "study", "steps": [{"type": "bands", "kmax": "1[1/nm]", "points": 3, "states": 3}]}]})json")
                                 .arg(b)
                                 .arg(w)
                                 .arg(b + w)
                                 .arg(user(well, 0.46, 0.052, R"("well")"), user(barrier, 0.6, 0.12, R"("below", "above")"))
                                 .arg(slowBuild() ? 0.2 : 0.1)
                                 .toUtf8();
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        double worst = 0, apart = 0;
        for (int point = 1; point < 3; ++point) {
            r.solution->select(point);
            const double k = global(*r.solution, QStringLiteral("qw.k")) * 1e-9;
            const std::array<double, 3> bf = reference(k, true), sym = reference(k, false);
            QStringList said;
            for (int m = 0; m < 3; ++m) {
                const double got = eV(*r.solution, QStringLiteral("qw.Ev%1").arg(2 * m + 1));
                worst = std::max(worst, std::abs(got - bf[std::size_t(m)]));
                apart = std::max(apart, std::abs(sym[std::size_t(m)] - bf[std::size_t(m)]));
                said << QStringLiteral("%1 (%2; symmetrized %3)").arg(got, 0, 'f', 5).arg(bf[std::size_t(m)], 0, 'f', 5).arg(sym[std::size_t(m)], 0, 'f', 5);
            }
            qInfo("k %.1f /nm: %s", k, qPrintable(said.join(QStringLiteral(", "))));
        }
        qInfo("Burt and Foreman's order: within %.2g eV of the linear elements; symmetrized %.2g eV apart", worst, apart);
        QVERIFY2(worst < 2e-5, qPrintable(QString::number(worst)));
        QVERIFY(apart > 1e-2);
        // No spurious band: In0.53Ga0.47As in InP.
        const Run s = run(modelOf(heterostructure(QStringLiteral("InGaAs"), QStringLiteral("0.53"), QStringLiteral("InP"), QStringLiteral("0"), QStringLiteral("6"),
                                                  QStringLiteral("10"), QStringLiteral(R"(, "valence": "kp6", "h1d": "0.25[nm]")"),
                                                  QStringLiteral(R"({"type": "bands", "kmax": "2.6[1/nm]", "points": %1, "states": 3})").arg(slowBuild() ? 9 : 27))));
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        s.solution->select(0);
        const double top = eV(*s.solution, QStringLiteral("qw.Ev1"));
        double highest = -1e9;
        for (int k = 1; k < s.solution->snapshotCount(); ++k) {
            s.solution->select(k);
            highest = std::max(highest, eV(*s.solution, QStringLiteral("qw.Ev1")));
        }
        qInfo("In0.53Ga0.47As in InP: the top %.4f eV at k = 0, at most %.4f beyond", top, highest);
        QVERIFY(highest < top + 1e-3);
    }

    void strainedWellsGain()
    {
        // The review's case for the k·p bands: InGaAs wells 6 nm on InP
        // (InP barriers), 3e18 cm⁻³ - compressed (x 0.68: the heavy holes on
        // top) their gain TE, stretched (x 0.38: the light holes on top, in
        // the k·p bands a camel's back) TM, either way; mixed, the holes'
        // states denser in the plane: less gain than the parabolic holes'.
        struct Peak {
            double te = 0, tm = 0, Ete = 0, Etm = 0;
        };
        auto peaks = [&](const char* x, const char* valence) {
            const Run r = run(modelOf(heterostructure(QStringLiteral("InGaAs"), QString::fromLatin1(x), QStringLiteral("InP"), QStringLiteral("0"), QStringLiteral("6"),
                                                      QStringLiteral("10"), QStringLiteral(R"(, "substrate": "InP", "h1d": "%1[nm]" %2)").arg(slowBuild() ? 0.5 : 0.25).arg(QLatin1String(valence)),
                                                      QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(0.70[eV], 1.10[eV], %1)", "states": %2,
                                                                        "kpoints": %3, "linewidth": "6[meV]", "density": "3e18[1/cm^3]"})x")
                                                          .arg(slowBuild() ? 41 : 201)
                                                          .arg(slowBuild() ? 1 : 3)
                                                          .arg(slowBuild() ? 21 : 121))));
            Peak p;
            if (!r.error.isEmpty()) qFatal("%s", qPrintable(r.error));
            for (int k = 0; k < r.solution->snapshotCount(); ++k) {
                r.solution->select(k);
                const double E = global(*r.solution, QStringLiteral("qw.Ephoton")), te = global(*r.solution, QStringLiteral("qw.gTE")), tm = global(*r.solution, QStringLiteral("qw.gTM"));
                if (te > p.te) p.te = te, p.Ete = E;
                if (tm > p.tm) p.tm = tm, p.Etm = E;
            }
            return p;
        };
        // (Under the sanitizers the compressed well's 6x6 alone, coarser.)
        for (const char* x : {"0.68", "0.38"}) {
            if (slowBuild() && !std::strcmp(x, "0.38")) continue;
            const Peak para = peaks(x, ""), kp6 = peaks(x, R"(, "valence": "kp6")");
            const Peak kp4 = slowBuild() ? kp6 : peaks(x, R"(, "valence": "kp4")");
            for (const auto& [name, p] : {std::pair{"parabolic", para}, std::pair{"k·p 4x4", kp4}, std::pair{"k·p 6x6", kp6}})
                qInfo("In%sGaAs on InP, %s: TE gain's peak %.0f /cm at %.4f eV, TM's %.0f /cm at %.4f eV", x, name, p.te / 100, p.Ete / Q, p.tm / 100, p.Etm / Q);
            const bool compressed = !std::strcmp(x, "0.68");
            for (const Peak& p : {para, kp4, kp6}) QVERIFY(compressed ? p.te > 3 * p.tm : p.tm > p.te);
            for (const Peak& p : {kp4, kp6}) QVERIFY(compressed ? p.te < 0.7 * para.te : p.tm < 0.3 * para.tm);
        }
    }

    void kpAgainstLuttingerKohn()
    {
        // The 4x4 k·p bands of a well 10 nm wide with hard walls (γ1 6.98,
        // γ2 2.06 of the masses 0.35 and 0.09 along the axis, γ3 2.93),
        // along k in the plane: against Luttinger and Kohn's 4x4 in the
        // |3/2, m> basis, H = Ev - (ħ²/2m0) [[P+Q, -S, R, 0], [-S†, P-Q, 0, R],
        // [R†, 0, P-Q, S], [0, R†, S†, P+Q]], P = γ1 (k² + kz²), Q = γ2 (k² -
        // 2 kz²), R = -√3 (γ2 (kx² - ky²) - 2i γ3 kx ky), S = 2√3 γ3 (kx - i ky)
        // kz - in the axial approximation R = -√3 γ̄ (kx - i ky)², γ̄ = (γ2 +
        // γ3)/2 -, in finite differences here (Richardson's of two grids).
        // The highest four states (two Kramers' pairs) to 5e-6 eV along [100]
        // and to 2e-6 along [110], axially and averaged over the plane (Gauss
        // and Legendre's four directions, 0 to 45°); the 6x6's with a
        // split-off 100 eV away the 4x4's.
        const double L = 10, g1 = (1 / 0.35 + 1 / 0.09) / 2, g2 = (1 / 0.09 - 1 / 0.35) / 4, g3 = 2.93;
        using cd = std::complex<double>;
        const cd I(0, 1);
        auto grid = [&](double k, double theta, bool axial, int N) {
            const double h = L / (N + 1), kx = k * std::cos(theta), ky = k * std::sin(theta);
            const cd kminus(kx, -ky);
            Eigen::MatrixXcd H = Eigen::MatrixXcd::Zero(4 * N, 4 * N);
            auto put = [&](int a, int b, int i, int j, cd v) { H(a * N + i, b * N + j) += v; };
            for (int i = 0; i < N; ++i)
                for (int j = std::max(0, i - 1); j <= std::min(N - 1, i + 1); ++j) {
                    // Each block an operator on the grid: kz² = -d²/dz², kz = -i d/dz.
                    const double kz2 = (i == j ? 2.0 : -1.0) / (h * h);
                    const cd kz = i == j ? cd(0) : -I * (j > i ? 1.0 : -1.0) / (2 * h);
                    const double same = i == j ? 1.0 : 0.0;
                    const cd P = g1 * (k * k * same + kz2), Q = g2 * (k * k * same - 2 * kz2);
                    const cd R = (axial ? -std::sqrt(3.0) * (g2 + g3) / 2 * kminus * kminus : -std::sqrt(3.0) * (g2 * (kx * kx - ky * ky) - 2.0 * I * g3 * kx * ky)) * same;
                    // (S = c kz, kz's operator Hermitian: S† = c* kz - not an element's conjugate.)
                    const cd c = 2 * std::sqrt(3.0) * g3 * kminus;
                    const cd S = c * kz, Sd = std::conj(c) * kz;
                    put(0, 0, i, j, P + Q);
                    put(0, 1, i, j, -S);
                    put(0, 2, i, j, R);
                    put(1, 0, i, j, -Sd);
                    put(1, 1, i, j, P - Q);
                    put(1, 3, i, j, R);
                    put(2, 0, i, j, std::conj(R));
                    put(2, 2, i, j, P - Q);
                    put(2, 3, i, j, S);
                    put(3, 1, i, j, std::conj(R));
                    put(3, 2, i, j, Sd);
                    put(3, 3, i, j, P + Q);
                }
            Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> es(-HB2M * H, Eigen::EigenvaluesOnly);
            std::array<double, 4> top;
            for (int m = 0; m < 4; ++m) top[std::size_t(m)] = -1.5 + es.eigenvalues()(4 * N - 1 - m);
            return top;
        };
        // (Richardson's of h and h/2: the differences' error as h², gone.)
        const int M = slowBuild() ? 39 : 79;
        auto reference = [&](double k, double theta, bool axial) {
            const std::array<double, 4> coarse = grid(k, theta, axial, M), fine = grid(k, theta, axial, 2 * M + 1);
            std::array<double, 4> out;
            for (int m = 0; m < 4; ++m) out[std::size_t(m)] = (4 * fine[std::size_t(m)] - coarse[std::size_t(m)]) / 3;
            return out;
        };
        auto bands = [&](const char* valence, const char* inplane, int points) {
            QByteArray json = box(QStringLiteral("20"), QStringLiteral("10"), QStringLiteral(R"(, "valence": "%1", "inplane": "%2")").arg(QLatin1String(valence), QLatin1String(inplane)),
                                  QStringLiteral("0[eV]"), QStringLiteral(R"({"type": "bands", "kmax": "1.2[1/nm]", "points": %1, "states": 2})").arg(points));
            json.replace(R"("nr": "3.6")", R"("nr": "3.6", "gamma3": "2.93", "Delta": "100[eV]")");
            return run(modelOf(json));
        };
        const Run r = bands("kp4", "100", slowBuild() ? 3 : 5), r6 = bands("kp6", "100", slowBuild() ? 3 : 5);
        QVERIFY2(r.error.isEmpty() && r6.error.isEmpty(), qPrintable(r.error + r6.error));
        double worst = 0, worst6 = 0;
        for (int point = 0; point < r.solution->snapshotCount(); ++point) {
            r.solution->select(point);
            r6.solution->select(point);
            const double k = global(*r.solution, QStringLiteral("qw.k")) * 1e-9;   // 1/nm
            const std::array<double, 4> expected = reference(k, 0, false);
            QStringList said;
            for (int m = 0; m < 4; ++m) {
                const double got = eV(*r.solution, QStringLiteral("qw.Ev%1").arg(m + 1)), got6 = eV(*r6.solution, QStringLiteral("qw.Ev%1").arg(m + 1));
                worst = std::max(worst, std::abs(got - expected[std::size_t(m)]));
                worst6 = std::max(worst6, std::abs(got6 - got));
                said << QStringLiteral("%1 (%2)").arg(got, 0, 'f', 6).arg(expected[std::size_t(m)], 0, 'f', 6);
            }
            qInfo("k %.2f /nm: %s", k, qPrintable(said.join(QStringLiteral(", "))));
        }
        qInfo("[100]: the 4x4 within %.2g eV of Luttinger and Kohn's; the 6x6 with Δ 100 eV within %.2g of the 4x4", worst, worst6);
        QVERIFY2(worst < (slowBuild() ? 5e-5 : 5e-6), qPrintable(QString::number(worst)));
        QVERIFY2(worst6 < 2e-4, qPrintable(QString::number(worst6)));
        // The other directions, the axial approximation and the average, at
        // 0.6 and 1.2 /nm (under the sanitizers at 1.2 alone, the average
        // left out).
        const double xg[4] = {-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526};
        const double wg[4] = {0.3478548451374538, 0.6521451548625461, 0.6521451548625461, 0.3478548451374538};
        for (const char* inplane : {"110", "axial", "average"}) {
            if (slowBuild() && !std::strcmp(inplane, "average")) continue;
            const Run d = bands("kp4", inplane, 3);
            QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
            double worstD = 0;
            QStringList said;
            for (int point = slowBuild() ? 2 : 1; point < 3; ++point) {
                d.solution->select(point);
                const double k = global(*d.solution, QStringLiteral("qw.k")) * 1e-9;
                std::array<double, 4> expected{};
                if (!std::strcmp(inplane, "average")) {
                    for (int g = 0; g < 4; ++g) {
                        const std::array<double, 4> e = reference(k, M_PI / 8 * (1 + xg[g]), false);
                        for (int m = 0; m < 4; ++m) expected[std::size_t(m)] += wg[g] / 2 * e[std::size_t(m)];
                    }
                } else
                    expected = reference(k, std::strcmp(inplane, "110") ? 0 : M_PI / 4, !std::strcmp(inplane, "axial"));
                for (int m = 0; m < 4; ++m) {
                    const double got = eV(*d.solution, QStringLiteral("qw.Ev%1").arg(m + 1));
                    worstD = std::max(worstD, std::abs(got - expected[std::size_t(m)]));
                    said << QStringLiteral("%1 (%2)").arg(got, 0, 'f', 6).arg(expected[std::size_t(m)], 0, 'f', 6);
                }
            }
            qInfo("%s: %s - within %.2g eV", inplane, qPrintable(said.join(QStringLiteral(", "))), worstD);
            QVERIFY2(worstD < (slowBuild() ? 3e-5 : 2e-6), qPrintable(QString::number(worstD)));
        }
    }

    void kpSpectrumUnmixed()
    {
        // γ2 = γ3 = 0: the heavy and light holes one mass, 1/γ1, nothing
        // mixing - the 4x4 k·p spectrum, its k grid's integral and the
        // line's tail past it, the parabolic one's (masses 1/γ1 along and
        // across), TE and TM.
        auto model = [](bool kp, bool excitons = false, const QString& inplane = QStringLiteral("axial")) {
            QByteArray json = box(QStringLiteral("20"), QStringLiteral("10"), kp ? QStringLiteral(R"(, "valence": "kp4", "inplane": "%1")").arg(inplane) : QString(), QStringLiteral("0[eV]"),
                                  excitons ? QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.55[eV], 1.95[eV], 401)", "states": 3, "linewidth": "1[meV]",
                                                                  "lineshape": "gaussian", "excitons": true, "epsr": "12.9"})x")
                                           : QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.55[eV], 1.95[eV], 41)", "states": 3, "linewidth": "5[meV]"})x"));
            json.replace(R"("mhh": "0.35", "mlh": "0.09")", R"("mhh": "0.2", "mlh": "0.2")");
            json.replace(R"("mhhp": "0.11", "mlhp": "0.21")", R"("mhhp": "0.2", "mlhp": "0.2", "gamma3": "0")");
            if (slowBuild()) json.replace(R"("states": 3)", R"("states": 2)");
            return lighter(json);
        };
        const Run para = run(modelOf(model(false))), kp = run(modelOf(model(true)));
        QVERIFY2(para.error.isEmpty() && kp.error.isEmpty(), qPrintable(para.error + kp.error));
        double worst = 0;
        for (int k = 0; k < para.solution->snapshotCount(); ++k) {
            para.solution->select(k);
            kp.solution->select(k);
            for (const char* pol : {"qw.alphaTE", "qw.alphaTM"}) {
                const double a = global(*para.solution, QString::fromLatin1(pol)), b = global(*kp.solution, QString::fromLatin1(pol));
                if (a > 1e3) worst = std::max(worst, std::abs(b / a - 1));
            }
        }
        qInfo("unmixed holes: the k·p spectrum the parabolic one's to %.2g", worst);
        QVERIFY2(worst < 1e-5, qPrintable(QString::number(worst)));
        // With excitons: each valence state's 1s line (its Kramers partner
        // the other spin pair) and the Sommerfeld factor over the k grid.
        // (Under the sanitizers not, nor the average over directions: the
        // carriers' along one.)
        if (!slowBuild()) {
            const Run paraX = run(modelOf(model(false, true))), kpX = run(modelOf(model(true, true)));
            QVERIFY2(paraX.error.isEmpty() && kpX.error.isEmpty(), qPrintable(paraX.error + kpX.error));
            double worstX = 0, most = 0;
            for (int k = 0; k < paraX.solution->snapshotCount(); ++k) {
                paraX.solution->select(k);
                most = std::max(most, global(*paraX.solution, QStringLiteral("qw.alphaTE")));
            }
            for (int k = 0; k < paraX.solution->snapshotCount(); ++k) {
                paraX.solution->select(k);
                kpX.solution->select(k);
                for (const char* pol : {"qw.alphaTE", "qw.alphaTM"})
                    worstX = std::max(worstX, std::abs(global(*kpX.solution, QString::fromLatin1(pol)) - global(*paraX.solution, QString::fromLatin1(pol))) / most);
            }
            paraX.solution->select(0);
            kpX.solution->select(0);
            const double EbPara = global(*paraX.solution, QStringLiteral("qw.Eb")), EbKp = global(*kpX.solution, QStringLiteral("qw.Eb"));
            qInfo("with excitons (e1-hh1 bound by %.4f meV, k·p %.4f): the k·p spectrum the parabolic one's to %.2g of its peak", EbPara / Q * 1e3, EbKp / Q * 1e3, worstX);
            QVERIFY2(std::abs(EbKp / EbPara - 1) < 1e-6, qPrintable(QString::number(EbKp / EbPara)));
            QVERIFY2(worstX < 1e-5, qPrintable(QString::number(worstX)));
        }
        // Averaged over the plane's directions (the bands isotropic here: each
        // its share; at 15° four states of one energy, which one vector's
        // Krylov space did not hold whole), and with 3e18 cm⁻³ - the holes
        // counted over each direction's k grid, each interval's occupation
        // integrated - the parabolic gain and holes' Fermi level.
        auto injected = [&](QByteArray json) {
            json.replace(R"("linewidth": "5[meV]"})", R"("linewidth": "5[meV]", "density": "3e18[1/cm^3]"})");
            return json;
        };
        const QString directions = slowBuild() ? QStringLiteral("axial") : QStringLiteral("average");
        const Run avg = slowBuild() ? kp : run(modelOf(model(true, false, directions)));
        const Run paraG = run(modelOf(injected(model(false)))), kpG = run(modelOf(injected(model(true, false, directions))));
        QVERIFY2(avg.error.isEmpty() && paraG.error.isEmpty() && kpG.error.isEmpty(), qPrintable(avg.error + paraG.error + kpG.error));
        double worstA = 0, worstG = 0, mostG = 0;
        for (int k = 0; k < para.solution->snapshotCount(); ++k) {
            para.solution->select(k);
            avg.solution->select(k);
            paraG.solution->select(k);
            kpG.solution->select(k);
            const double a = global(*para.solution, QStringLiteral("qw.alphaTE"));
            if (a > 1e3) worstA = std::max(worstA, std::abs(global(*avg.solution, QStringLiteral("qw.alphaTE")) / a - 1));
            mostG = std::max(mostG, std::abs(global(*paraG.solution, QStringLiteral("qw.gTE"))));
            worstG = std::max(worstG, std::abs(global(*kpG.solution, QStringLiteral("qw.gTE")) - global(*paraG.solution, QStringLiteral("qw.gTE"))));
        }
        paraG.solution->select(0);
        kpG.solution->select(0);
        const double FvPara = global(*paraG.solution, QStringLiteral("qw.Fv")), FvKp = global(*kpG.solution, QStringLiteral("qw.Fv"));
        qInfo("averaged over directions: to %.2g; with carriers the holes' Fermi level %.6f eV (%.6f), the gain to %.2g of its most", worstA,
              FvKp / Q, FvPara / Q, worstG / mostG);
        QVERIFY2(worstA < 1e-5, qPrintable(QString::number(worstA)));
        QVERIFY2(std::abs(FvKp - FvPara) < 1e-6 * Q, qPrintable(QString::number((FvKp - FvPara) / Q)));
        QVERIFY2(worstG < 1e-4 * mostG, qPrintable(QString::number(worstG / mostG)));
    }

    void kpGain()
    {
        // The k·p bands' gain (3e18 cm⁻³ in the infinite well, a GaAs-like
        // γ3), unbroadened: crossing 0 at the quasi-Fermi levels'
        // separation, as Bernard and Duraffourg have it.
        QByteArray json = box(QStringLiteral("20"), QStringLiteral("10"), QStringLiteral(R"(, "valence": "kp4")"), QStringLiteral("0[eV]"),
                              QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.60[eV], 1.80[eV], 201)", "states": %1, "linewidth": "0[eV]", "density": "3e18[1/cm^3]"})x")
                                  .arg(slowBuild() ? 2 : 4));
        json.replace(R"("nr": "3.6")", R"("nr": "3.6", "gamma3": "2.93")");
        const Run r = run(modelOf(lighter(json)));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        s.select(0);
        const double dF = global(s, QStringLiteral("qw.dF"));
        double crossing = NAN, peak = 0;
        for (int k = 1; k < s.snapshotCount(); ++k) {
            s.select(k - 1);
            const double g0 = global(s, QStringLiteral("qw.gTE")), e0 = global(s, QStringLiteral("qw.Ephoton"));
            s.select(k);
            const double g1 = global(s, QStringLiteral("qw.gTE")), e1 = global(s, QStringLiteral("qw.Ephoton"));
            peak = std::max(peak, g1);
            if (g0 > 0 && g1 <= 0 && std::isnan(crossing)) crossing = e0 + (e1 - e0) * g0 / (g0 - g1);
        }
        qInfo("k·p gain: peak %.0f /cm; it crosses 0 at %.4f eV, the quasi-Fermi levels %.4f eV apart", peak / 100, crossing / Q, dF / Q);
        QVERIFY(peak > 0);
        QVERIFY2(std::abs(crossing - dF) < 3e-4 * Q, qPrintable(QString::number((crossing - dF) / Q)));
        // The k grid as asked: 41 values (under the sanitizers the run's own).
        QByteArray coarse = json;
        coarse.replace(R"("type": "spectrum")", R"("type": "spectrum", "kpoints": 41)");
        const Run c = slowBuild() ? r : run(modelOf(coarse));
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        QVERIFY2(c.solution->log.join(QLatin1Char(' ')).contains(QLatin1String("at 41 values of k")), qPrintable(c.solution->log.join(QLatin1Char('\n'))));
    }

    void lorentzianTailsUnderTheGain()
    {
        // A GaAs laser well's gain (10 nm in Al0.25GaAs, 3e18 cm⁻³ at 50
        // kV/cm, a line 6.6 meV half wide): a Lorentzian's tails carry the
        // absorption of the transitions well above the gain's peak under it
        // (qw.alphaTEfar), the more the more states the step takes. From 2
        // states to 4 the peak falls (the review's 394 /cm against 304 with
        // the k·p bands): as a Gaussian line's does - the carriers spread
        // over more states, the quasi-Fermi levels nearer - and more, those
        // tails most of the more (77-89 %; the rest the tails of new
        // transitions nearer than 5 half widths). Past 10 % of the gain
        // warned, with how much; a Gaussian line's tails do not reach: none,
        // and no warning.
        struct Peak {
            double g[2] = {0, 0}, far[2] = {0, 0};   // TE, TM: the gain at its peak and what those transitions take off it (1/m)
            QString log;
            double share(int pol) const { return far[pol] / (g[pol] + far[pol]); }
        };
        auto gainOf = [&](const char* valence, int states, const char* shape) {
            const Run r = run(modelOf(lighter(heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.25"), QStringLiteral("10"),
                                                              QStringLiteral("10"), QStringLiteral(R"(, "F": "5e6[V/m]", "valence": "%1")").arg(QLatin1String(valence)),
                                                              QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.40[eV], 1.70[eV], 151)", "states": %1, "linewidth": "6.6[meV]", "lineshape": "%2", "density": "3e18[1/cm^3]"})x")
                                                                  .arg(states)
                                                                  .arg(QLatin1String(shape))))));
            Peak peak;
            if (!r.error.isEmpty()) {
                qWarning("%s", qPrintable(r.error));
                return peak;
            }
            Solution& s = *r.solution;
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                for (int pol = 0; pol < 2; ++pol) {
                    const QString P = pol == 0 ? QStringLiteral("TE") : QStringLiteral("TM");
                    const double g = global(s, QStringLiteral("qw.g") + P);
                    if (g > peak.g[pol]) {
                        peak.g[pol] = g;
                        peak.far[pol] = global(s, QStringLiteral("qw.alpha%1far").arg(P));
                    }
                }
            }
            peak.log = s.log.join(QLatin1Char('\n'));
            qInfo("%s, %d states, %s: the TE peak %.0f /cm, %.0f /cm taken off it; TM %.0f and %.0f", valence, states, shape, peak.g[0] / 100,
                  peak.far[0] / 100, peak.g[1] / 100, peak.far[1] / 100);
            return peak;
        };
        for (const char* valence : {"parabolic", "kp4"}) {
            if (slowBuild() && !std::strcmp(valence, "kp4")) continue;
            const Peak two = gainOf(valence, 2, "lorentzian"), four = gainOf(valence, 4, "lorentzian");
            const Peak two0 = gainOf(valence, 2, "gaussian"), four0 = gainOf(valence, 4, "gaussian");
            for (int pol = 0; pol < 2; ++pol) {
                QVERIFY(two.g[pol] > 0 && four.g[pol] > 0 && two.far[pol] > 0 && four.far[pol] > two.far[pol]);
                // The peak's fall: the Lorentzian's, without those tails, a
                // Gaussian's.
                const double lorentzian = four.g[pol] / two.g[pol], without = (four.g[pol] + four.far[pol]) / (two.g[pol] + two.far[pol]),
                             gaussian = four0.g[pol] / two0.g[pol];
                QVERIFY2(lorentzian < gaussian - 0.03, qPrintable(QStringLiteral("%1 against %2").arg(lorentzian).arg(gaussian)));
                QVERIFY2((without - lorentzian) / (gaussian - lorentzian) > 0.7 && without < gaussian + 0.005,
                         qPrintable(QStringLiteral("%1, %2 without, %3 a Gaussian's").arg(lorentzian).arg(without).arg(gaussian)));
            }
            // Warned past 10 %: the worse polarization, its share.
            for (const Peak& p : {two, four}) {
                const int worse = p.share(1) > p.share(0) ? 1 : 0;
                const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("above the (TE|TM) gain's peak \\([^)]*\\) take (\\d+) % off it")).match(p.log);
                QVERIFY2(m.hasMatch() == (p.share(worse) > 0.1), qPrintable(p.log));
                if (m.hasMatch()) {
                    QCOMPARE(m.captured(1), worse == 0 ? QStringLiteral("TE") : QStringLiteral("TM"));
                    QCOMPARE(m.captured(2).toInt(), qRound(100 * p.share(worse)));
                }
            }
            QVERIFY2(four.log.contains(QLatin1String("by their Lorentzian tails alone")), qPrintable(four.log));
            for (const Peak& p : {two0, four0}) {
                QVERIFY2(std::abs(p.far[0]) < 1e-3 * p.g[0] && std::abs(p.far[1]) < 1e-3 * p.g[1], qPrintable(QString::number(p.far[0])));
                QVERIFY2(!p.log.contains(QLatin1String("tails alone")), qPrintable(p.log));
            }
        }
    }

    void spontaneousEmission()
    {
        // Unbroadened, each transition's emission f_c (1 - f_v) its
        // absorption's f_v - f_c over e^((E - ΔF)/kT) - 1: the spontaneous
        // emission (n² E²/(π² ħ³ c²)) (2 αsp,TE + αsp,TM)/3 exactly that of
        // the gain - parabolic and k·p -, and each polarization's emission
        // coefficient (Lumerical's g_sp) its absorption's so.
        for (const char* valence : {"", R"(, "valence": "kp4")"}) {
            const Run r = run(modelOf(lighter(box(QStringLiteral("20"), QStringLiteral("10"), QString::fromLatin1(valence), QStringLiteral("0[eV]"),
                                                  QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.62[eV], 1.78[eV], 17)", "states": 3, "linewidth": "0[eV]", "density": "3e18[1/cm^3]"})x")))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.solution;
            const double kT = KB * 300, n = 3.6;
            double worst = 0, most = 0;
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                const double E = global(s, QStringLiteral("qw.Ephoton")), dF = global(s, QStringLiteral("qw.dF"));
                const double aTE = global(s, QStringLiteral("qw.alphaTE")), aTM = global(s, QStringLiteral("qw.alphaTM"));
                const double expected = n * n * E * E / (M_PI * M_PI * Hbar * Hbar * Hbar * 299792458.0 * 299792458.0) * (2 * aTE + aTM) / 3
                                        / (std::exp((E - dF) / kT) - 1);
                const double got = global(s, QStringLiteral("qw.rsp"));
                most = std::max(most, got);
                if (std::abs(expected) > 0) worst = std::max(worst, std::abs(got - expected) / std::max(std::abs(expected), 1e-6 * most));
                // The coefficients, each polarization's: the gain's own.
                const double x = std::expm1((E - dF) / kT);
                for (const auto& [sp, alpha] : {std::pair{"qw.gspTE", aTE}, std::pair{"qw.gspTM", aTM}}) {
                    const double g = global(s, QString::fromLatin1(sp));
                    if (alpha != 0) worst = std::max(worst, std::abs(g - alpha / x) / std::max(std::abs(alpha / x), 1e-9 * std::abs(global(s, QStringLiteral("qw.gspTE")))));
                }
            }
            qInfo("%s: the spontaneous emission its relation to the gain to %.2g (at most %.3g /(s cm³ eV)); in all %.3g /(s cm³)",
                  *valence ? "k·p" : "parabolic", worst, most * 1e-6 * Q, global(s, QStringLiteral("qw.Rsp")) * 1e-6);
            QVERIFY2(worst < 1e-6, qPrintable(QString::number(worst)));
            QVERIFY(most > 0);
        }
    }

    void kramersKronig()
    {
        // The index the window's absorption makes, (c ħ/π) P∫ α(E')/(E'² -
        // E²) dE': on the infinite well's Lorentzian steps (41 energies)
        // against the same principal value of their analytic α on 40001.
        const double gamma = 0.005 * Q, L = 10e-9, eps0 = 8.8541878188e-12, c0 = 299792458.0, Ep = 25 * Q;
        auto alpha = [&](double E) {
            double te = 0;
            for (int n = 1; n <= 3; ++n) {
                const double ee = HB2M * n * n * M_PI * M_PI / (0.067 * 100);
                for (int c = 0; c < 2; ++c) {
                    const double mz = c == 0 ? 0.35 : 0.09, mp = c == 0 ? 0.11 : 0.21;
                    const double eh = -1.5 - HB2M * n * n * M_PI * M_PI / (mz * 100);
                    const double Et = (ee - eh) * Q, mr = 1 / (1 / 0.067 + 1 / mp) * M0;
                    te += M_PI * Q * Q * Hbar / (3.6 * c0 * eps0 * M0 * M0 * E) * (M0 * Ep / 6) * mr / (M_PI * Hbar * Hbar * L)
                          * (0.5 + std::atan((E - Et) / gamma) / M_PI) * (c == 0 ? 1.5 : 0.5);
                }
            }
            return te;
        };
        const double lo = 1.5 * Q, hi = 2.1 * Q;
        const Run r = run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
                                      QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.5[eV], 2.1[eV], 241)", "states": 3, "linewidth": "5[meV]"})x"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        double worst = 0, most = 0;
        for (int k = 20; k < s.snapshotCount() - 20; k += 20) {
            s.select(k);
            const double E = global(s, QStringLiteral("qw.Ephoton"));
            // The principal value: the interval about E left out symmetrically.
            const int m = 40000;
            const double h = (hi - lo) / m, aE = alpha(E);
            double sum = 0;
            for (int i = 0; i <= m; ++i) {
                const double x = lo + i * h, w = (i == 0 || i == m) ? 0.5 : 1;
                const double d = x * x - E * E;
                sum += w * (std::abs(d) < 1e-40 ? 0.0 : (alpha(x) - aE) / d) * h;
            }
            sum += aE * std::log(std::abs((hi - E) * (lo + E) / ((hi + E) * (lo - E)))) / (2 * E);
            const double expected = c0 * Hbar / M_PI * sum, got = global(s, QStringLiteral("qw.dnTE"));
            qInfo("%.3f eV: n from the window %.6g (%.6g)", E / Q, got, expected);
            worst = std::max(worst, std::abs(got - expected));
            most = std::max(most, std::abs(expected));
        }
        QVERIFY2(worst < 1e-3 * most, qPrintable(QString::number(worst / most)));
        // Its top still absorbing: said.
        QVERIFY(s.log.join(QLatin1Char(' ')).contains(QLatin1String("Kramers-Kronig")));
    }

    void broadeningWithTheField()
    {
        // The field across the wells (their band edge's slope: the uniform
        // field's) adds to the broadening as asked.
        const Run r = run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), QStringLiteral(R"(, "F": "5e6[V/m]")"), QStringLiteral("0[eV]"),
                                      QString::fromUtf8(R"x({"type": "spectrum", "energies": "1.7[eV]", "states": 2, "linewidth": "4[meV]", "dlinewidth": "2[meV]/(1e7[V/m])"})x"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        r.solution->select(0);
        const double F = global(*r.solution, QStringLiteral("qw.Fw")), g = global(*r.solution, QStringLiteral("qw.linewidth"));
        qInfo("across the wells %.6g V/m; the broadening %.6f meV", F, g / Q * 1e3);
        QVERIFY(std::abs(F / 5e6 - 1) < 1e-6);
        QVERIFY(std::abs(g / Q - 0.005) < 1e-9);
    }

    void aBoundContinuum()
    {
        // A GaAs well 8 nm in Al0.3GaAs, continuum "bound": above the
        // barriers' gap their bulk absorption, (π q² ħ/(n c ε0 m0² E)) |Mb|²
        // Σ (1/2π²)(2 mr/ħ²)^(3/2) √(E - Eg) over the line, over their 20 nm
        // per the well; past the fade (0.1 eV) the well's bound steps gone,
        // GaAs's own bulk over its 8 nm in their place.
        const int energies = slowBuild() ? 41 : 101;
        auto spectrum = [&](const QString& well, const QString& F) {
            return run(modelOf(heterostructure(well, well == QLatin1String("GaAs") ? QStringLiteral("0") : QStringLiteral("0.3"), QStringLiteral("AlGaAs"),
                                               QStringLiteral("0.3"), QStringLiteral("8"), QStringLiteral("10"), QStringLiteral(R"(, "F": "%1")").arg(F),
                                               QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.40[eV], 2.4[eV], %1)", "states": 8, "linewidth": "6[meV]", "lineshape": "gaussian", "continuum": "bound"})x")
                                                   .arg(energies))));
        };
        const BandParameters b = *bandParameters(QStringLiteral("AlGaAs"), 0.3, 0, 300), w = *bandParameters(QStringLiteral("GaAs"), 0, 0, 300);
        const double sigma = 6e-3 * Q / std::sqrt(2 * std::log(2.0));
        auto bulk = [&](const BandParameters& m, double E) {
            // ∫ √ε G(E - Eg - ε) dε, the line's Gaussian (its half width 6 meV).
            const double Eg = m.Eg * Q, delta = E - Eg, reach = std::max(0.0, delta) + 12 * sigma;
            double root = 0;
            for (int k = 0; k < 20000; ++k) {
                const double eps = reach * (k + 0.5) / 20000, x = delta - eps;
                root += std::sqrt(eps) * std::exp(-0.5 * x * x / (sigma * sigma)) / (sigma * std::sqrt(2 * M_PI)) * reach / 20000;
            }
            const double mhh = std::cbrt(1 / (m.gamma1 - 2 * m.gamma2) * std::pow(1 / (m.gamma1 + m.gamma2), 2));
            const double mlh = std::cbrt(1 / (m.gamma1 + 2 * m.gamma2) * std::pow(1 / (m.gamma1 - m.gamma2), 2));
            double rho = 0;
            for (double mh : {mhh, mlh}) {
                const double mr = 1 / (1 / m.me + 1 / mh) * M0;
                rho += std::pow(2 * mr / (Hbar * Hbar), 1.5) / (2 * M_PI * M_PI) * root;
            }
            return M_PI * Q * Q * Hbar / (m.n * 299792458.0 * 8.8541878188e-12 * M0 * M0 * E) * (M0 * m.Ep * Q / 6) * rho;
        };
        const Run r = spectrum(QStringLiteral("GaAs"), QStringLiteral("0[V/m]"));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        s.select(s.snapshotCount() - 1);
        const double E = global(s, QStringLiteral("qw.Ephoton"));
        const double tm = global(s, QStringLiteral("qw.alphaTM")), te = global(s, QStringLiteral("qw.alphaTE"));
        const double expected = bulk(b, E) * 20 / 8 + bulk(w, E);
        qInfo("at %.3f eV, %.0f meV above the barriers' gap: TM %.6g /cm, TE %.6g; their bulk over 20 nm and GaAs's over 8, per the well, %.6g", E / Q,
              (E - b.Eg * Q) / Q * 1e3, tm / 100, te / 100, expected / 100);
        QVERIFY2(std::abs(te / expected - 1) < 1e-3, qPrintable(QString::number(te / expected - 1)));
        QVERIFY2(std::abs(tm / te - 1) < 1e-9, qPrintable(QString::number(tm / te - 1)));
        // Below the first transition of a state above its barriers (an
        // electron at the barriers' Ec to a hole at the well's Ev, or the
        // other way) the bound steps alone, the box's: GaAs's own bulk only
        // fading in as they fade out (a Gaussian line: none of either's tails
        // there).
        {
            QByteArray boxed = heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("8"),
                                               QStringLiteral("10"), QStringLiteral(R"(, "F": "0[V/m]")"),
                                               QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.40[eV], 2.4[eV], %1)", "states": 8, "linewidth": "6[meV]", "lineshape": "gaussian"})x")
                                                   .arg(energies));
            const Run box = run(modelOf(boxed));
            QVERIFY2(box.error.isEmpty(), qPrintable(box.error));
            double worst = 0;
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                box.solution->select(k);
                if (global(s, QStringLiteral("qw.Ephoton")) > (std::min(b.VBO + b.Eg - w.VBO, w.VBO + w.Eg - b.VBO) - 0.03) * Q) break;
                const double a1 = global(s, QStringLiteral("qw.alphaTE")), a2 = global(*box.solution, QStringLiteral("qw.alphaTE"));
                if (a2 > 1e3) worst = std::max(worst, std::abs(a1 / a2 - 1));
            }
            qInfo("below %.3f eV the bound continuum the box's to %.2g", std::min(b.VBO + b.Eg - w.VBO, w.VBO + w.Eg - b.VBO) - 0.03, worst);
            QVERIFY2(worst < 1e-9, qPrintable(QString::number(worst)));
        }
        // The barrier alone, nothing bound (the line all of one gap, the
        // wells' size its 28 nm): its bulk absorption whole from its gap
        // (through the fade too), the same in a field (its gap where the line
        // ends its own, not the edges' least).
        double worst = 0;
        for (const char* F : {"0[V/m]", "1e7[V/m]"}) {
            if (slowBuild() && !std::strcmp(F, "0[V/m]")) continue;   // (under the sanitizers the field's alone)
            const Run slab = spectrum(QStringLiteral("AlGaAs"), QString::fromLatin1(F));
            QVERIFY2(slab.error.isEmpty(), qPrintable(slab.error));
            QVERIFY2(slab.solution->log.join(QLatin1Char(' ')).contains(QLatin1String("a spectrum of 0 transitions")), qPrintable(slab.solution->log.join(QLatin1Char('\n'))));
            for (int k = 0; k < slab.solution->snapshotCount(); ++k) {
                slab.solution->select(k);
                const double e = global(*slab.solution, QStringLiteral("qw.Ephoton")), a = global(*slab.solution, QStringLiteral("qw.alphaTE"));
                if (e < (b.Eg + 0.02) * Q) continue;
                const double whole = bulk(b, e);
                if (k % 5 == 0) qInfo("the slab in %s at %.3f eV: %.6g /cm, its bulk %.6g", F, e / Q, a / 100, whole / 100);
                worst = std::max(worst, std::abs(a / whole - 1));
            }
        }
        QVERIFY2(worst < 1e-3, qPrintable(QString::number(worst)));
        // At 100 kV/cm the line's far ends 0.2 eV lower: a 10 nm well's
        // first states still confined by its own barriers, its e1-hh1 (and
        // exciton) kept - not by the far ends' edges, which drop them.
        QByteArray tilted = heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("10"),
                                            QStringLiteral("15"), QStringLiteral(R"(, "F": "1e7[V/m]")"),
                                            QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.40[eV], 1.60[eV], %1)", "states": %2, "linewidth": "3[meV]",
                                                              "lineshape": "gaussian", "excitons": true, "epsr": "12.9", "continuum": "bound"})x")
                                                .arg(slowBuild() ? 11 : 21)
                                                .arg(slowBuild() ? 2 : 4));
        const Run t = run(modelOf(tilted));
        QVERIFY2(t.error.isEmpty(), qPrintable(t.error));
        t.solution->select(0);
        const double Ex = global(*t.solution, QStringLiteral("qw.Ex")), Eb = global(*t.solution, QStringLiteral("qw.Eb"));
        qInfo("at 100 kV/cm: %s; the e1-hh1 exciton at %.4f eV, bound by %.3f meV", qPrintable(t.solution->log.filter(QStringLiteral("spectrum of")).join(QString())),
              Ex / Q, Eb / Q * 1e3);
        QVERIFY(std::isfinite(Ex) && Eb > 0);
    }

    void electrorefractionOfAnyWindow()
    {
        // Δn between 0 and 50 kV/cm below the edge (at 1.46 eV, on both
        // grids: 0.01 eV, a sanitized build's 0.02) the same whatever the
        // window's top past the barriers' gap and the fade: above them the
        // bulk's absorption alone, the field's none (the review's -6.7e-3
        // to 2.0 eV and -11.2e-3 to 3.0 eV before).
        // (A sanitized build: its line's elements 0.25 nm - six dense
        // eigenproblems of 0.1 nm's take minutes there -, 0.02 eV, 4 states,
        // a line 15 meV wide.)
        const double step = slowBuild() ? 0.02 : 0.01;
        auto spectrum = [&](double top, const char* F, const QString& fade = QString()) {
            const int n = qRound((top - 1.40) / step) + 1;
            return run(modelOf(lighter(heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("8"),
                                               QStringLiteral("10"), QStringLiteral(R"(, "F": "%1")").arg(QLatin1String(F)),
                                               QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.40[eV], %1[eV], %2)", "states": %4, "linewidth": "%5", "lineshape": "gaussian", "continuum": "bound"%3})x")
                                                   .arg(top)
                                                   .arg(n)
                                                   .arg(fade)
                                                   .arg(slowBuild() ? 4 : 8)
                                                   .arg(slowBuild() ? QStringLiteral("15[meV]") : QStringLiteral("6[meV]"))))));
        };
        auto at = [&](Solution& s, double E, const QString& name) {
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                if (std::abs(global(s, QStringLiteral("qw.Ephoton")) / Q - E) < 1e-9) return global(s, name);
            }
            return double(NAN);
        };
        double dn[2] = {NAN, NAN};
        const double tops[2] = {2.0, slowBuild() ? 2.4 : 2.6};
        for (int i = 0; i < 2; ++i) {
            const Run off = spectrum(tops[i], "0[V/m]"), on = spectrum(tops[i], "5e6[V/m]");
            QVERIFY2(off.error.isEmpty() && on.error.isEmpty(), qPrintable(off.error + on.error));
            dn[i] = at(*on.solution, 1.46, QStringLiteral("qw.dnTE")) - at(*off.solution, 1.46, QStringLiteral("qw.dnTE"));
            // The top's absorption the field's none; the index past it alike.
            const double a0 = at(*off.solution, tops[i], QStringLiteral("qw.alphaTE")), a1 = at(*on.solution, tops[i], QStringLiteral("qw.alphaTE"));
            const double t0 = at(*off.solution, 1.46, QStringLiteral("qw.dnTEtail")), t1 = at(*on.solution, 1.46, QStringLiteral("qw.dnTEtail"));
            qInfo("to %.1f eV: dn(1.46 eV) %.6g; at the top %.6g /cm and %.6g; past it %.6g and %.6g", tops[i], dn[i], a0 / 100, a1 / 100, t0, t1);
            QVERIFY2(std::abs(a1 / a0 - 1) < 1e-6, qPrintable(QString::number(a1 / a0 - 1)));
            QVERIFY(t0 > 0 && std::abs(t1 / t0 - 1) < 1e-6);
            const QString log = on.solution->log.join(QLatin1Char(' '));
            QVERIFY2(log.contains(QLatin1String("the same in any field")) && !log.contains(QLatin1String("Warning")), qPrintable(log));
        }
        QVERIFY2(std::abs(dn[0]) > 1e-5, qPrintable(QString::number(dn[0])));
        QVERIFY2(std::abs(dn[1] / dn[0] - 1) < 2e-3, qPrintable(QString::number(dn[1] / dn[0] - 1)));
        // The window's top short of the fade's end: warned, with that energy
        // (Al0.3GaAs's gap and 0.1 eV).
        const Run shortOf = spectrum(1.80, "0[V/m]");
        QVERIFY2(shortOf.error.isEmpty(), qPrintable(shortOf.error));
        const QString log = shortOf.solution->log.join(QLatin1Char(' '));
        const QString end = QString::number(bandParameters(QStringLiteral("AlGaAs"), 0.3, 0, 300)->Eg + 0.1, 'g', 5);
        QVERIFY2(log.contains(QLatin1String("Warning")) && log.contains(QStringLiteral("past the barriers' gap and the fade, %1").arg(end)), qPrintable(log + end));
        // A fade below 0 refused.
        const Run negative = spectrum(1.60, "0[V/m]", QStringLiteral(R"(, "fade": "-0.1[eV]")"));
        QVERIFY2(negative.error.contains(QLatin1String("its fade")), qPrintable(negative.error));
    }

    void fadingInEveryPath()
    {
        // The fade in each of the spectrum's paths - the parabolic bands'
        // steps, the k·p bands' pairs at each k - with a Lorentzian line (its
        // slow tails reaching far): the absorption at 1.70 eV the same whether
        // the window ends at 1.80 eV (short of the fade's end, 1.94) or 2.2
        // (each step reached to the fade's end either way, its tail past
        // there faded); past the fade the top's absorption the field's none
        // but for the Lorentzian tails of the steps below it (3e-5 at 2.2 eV:
        // a Gaussian's or a sech's none); qw.dnTEtail (c ħ/π) α_top ln((top +
        // E)/(top - E))/2E. (Under the sanitizers the parabolic path alone:
        // the k·p one's three spectra take minutes there.)
        for (const bool kp : {false, true}) {
            if (kp && slowBuild()) continue;
            auto spectrum = [&](double top, const char* F) {
                const int n = qRound((top - 1.40) / 0.02) + 1;
                return run(modelOf(heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"),
                                                   QStringLiteral("8"), QStringLiteral("10"),
                                                   QStringLiteral(R"(, "F": "%1"%2)").arg(QLatin1String(F), kp ? QStringLiteral(R"(, "valence": "kp4", "h1d": "0.2[nm]")") : QString()),
                                                   QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.40[eV], %1[eV], %2)", "states": 6, "linewidth": "6[meV]", "continuum": "bound"%3})x")
                                                       .arg(top)
                                                       .arg(n)
                                                       .arg(kp ? QStringLiteral(R"(, "kpoints": 31)") : QString()))));
            };
            auto at = [&](Solution& s, double E, const QString& name) {
                for (int k = 0; k < s.snapshotCount(); ++k) {
                    s.select(k);
                    if (std::abs(global(s, QStringLiteral("qw.Ephoton")) / Q - E) < 1e-9) return global(s, name);
                }
                return double(NAN);
            };
            const Run shortOf = spectrum(1.80, "5e6[V/m]"), past = spectrum(2.2, "5e6[V/m]"), flat = spectrum(2.2, "0[V/m]");
            QVERIFY2(shortOf.error.isEmpty() && past.error.isEmpty() && flat.error.isEmpty(), qPrintable(shortOf.error + past.error + flat.error));
            const double a = at(*shortOf.solution, 1.70, QStringLiteral("qw.alphaTE")), b = at(*past.solution, 1.70, QStringLiteral("qw.alphaTE"));
            const double top0 = at(*flat.solution, 2.2, QStringLiteral("qw.alphaTE")), top1 = at(*past.solution, 2.2, QStringLiteral("qw.alphaTE"));
            const double E = 1.46 * Q, hi = 2.2 * Q;
            const double tail = 299792458.0 * Hbar / M_PI * std::log((hi + E) / (hi - E)) / (2 * E) * top1;
            const double given = at(*past.solution, 1.46, QStringLiteral("qw.dnTEtail"));
            qInfo("%s: at 1.70 eV %.7g /cm to 1.80 eV, %.7g to 2.2; at 2.2 eV %.7g and %.7g in the field; the tail %.7g (%.7g)", kp ? "k·p" : "parabolic",
                  a / 100, b / 100, top0 / 100, top1 / 100, given, tail);
            QVERIFY2(std::abs(a / b - 1) < 1e-6, qPrintable(QString::number(a / b - 1)));
            QVERIFY2(std::abs(top1 / top0 - 1) < 1e-4, qPrintable(QString::number(top1 / top0 - 1)));
            QVERIFY2(std::abs(given / tail - 1) < 1e-9, qPrintable(QString::number(given / tail - 1)));
        }
    }

    void kpAndParabolicFadeAlike()
    {
        // The fade's reach in each path: at 77 K the parabolic steps' cells
        // go 0.3 eV above each (to 1.82 eV here), the k grid to the window's
        // top and 20 broadenings and 10 kT (or the holes' 0.15 eV and 15 kT:
        // heavy carriers, 0.6 and 1 m0, keep that short too) - to 1.95 eV,
        // short of the fade's end (2.0): each carried to it all the same. With γ2 = γ3 = 0 (heavy and light
        // holes one mass) the k·p spectrum the parabolic one's, continuum
        // bound, a Lorentzian's tails, to 3e-5 (the parabolic steps' cells
        // under the fade).
        auto model = [](bool kp) {
            QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "name": "below", "size": ["20", "10"]},
                             {"type": "rectangle", "name": "well", "position": ["0", "10"], "size": ["20", "8"]},
                             {"type": "rectangle", "name": "above", "position": ["0", "18"], "size": ["20", "10"]}],
                "physics": [{"type": "quantum", "tag": "qw", "T": "77[K]" %1, "features": [
                  {"type": "material", "source": "user", "Ec": "0.3[eV]", "Ev": "-1.6[eV]", "me": "0.7", "mhh": "1", "mlh": "1", "mhhp": "1",
                   "mlhp": "1", "gamma3": "0", "Ep": "25[eV]", "nr": "3.4", "selection": {"only": ["below", "above"]}},
                  {"type": "material", "source": "user", "Ec": "0[eV]", "Ev": "-1.5[eV]", "me": "0.6", "mhh": "1", "mlh": "1", "mhhp": "1",
                   "mlhp": "1", "gamma3": "0", "Ep": "25[eV]", "nr": "3.6", "selection": {"only": ["well"]}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
              "studies": [{"type": "study", "steps": [{"type": "spectrum", "energies": "linspace(1.55[eV], 1.62[eV], 8)", "states": 3,
                "linewidth": "5[meV]", "continuum": "bound"}]}]})json")
                                  .arg(kp ? QStringLiteral(R"(, "valence": "kp4")") : QString())
                                  .toUtf8();
            if (slowBuild()) json.replace(R"("states": 3)", R"("states": 2)");
            return lighter(json);
        };
        const Run para = run(modelOf(model(false))), kp = run(modelOf(model(true)));
        QVERIFY2(para.error.isEmpty() && kp.error.isEmpty(), qPrintable(para.error + kp.error));
        double worst = 0;
        for (int k = 0; k < para.solution->snapshotCount(); ++k) {
            para.solution->select(k);
            kp.solution->select(k);
            for (const char* pol : {"qw.alphaTE", "qw.alphaTM"}) {
                const double a = global(*para.solution, QString::fromLatin1(pol)), b = global(*kp.solution, QString::fromLatin1(pol));
                worst = std::max(worst, std::abs(b / a - 1));
            }
        }
        qInfo("faded at 77 K: the k·p spectrum the parabolic one's to %.2g", worst);
        // (A sanitized build's k grid of 41 values the k·p integral's own
        // error some 1e-3: there the paths run, their agreement looser.)
        QVERIFY2(worst < (slowBuild() ? 1e-2 : 3e-5), qPrintable(QString::number(worst)));
    }

    void valleyPresets()
    {
        // Stern and Howard's rule on the six Δ valleys of Si (ml 0.916, mt
        // 0.190): Ando, Fowler and Stern's table - (100) two valleys 0.916
        // along the axis, 0.190 in the plane, four 0.190 and 0.417; (110)
        // four 0.315, 0.324 and two 0.190, 0.417; (111) six 0.258, 0.358.
        struct Want {
            const char* text;
            std::vector<std::array<double, 3>> sets;   // g, mz, md
        };
        const Want si[] = {{"Si(100)", {{2, 0.916, 0.190}, {4, 0.190, 0.417}}},
                           {"Si(110)", {{4, 0.315, 0.324}, {2, 0.190, 0.417}}},
                           {"si (111)", {{6, 0.258, 0.358}}}};
        for (const Want& w : si) {
            QString why;
            const std::optional<std::vector<ValleySet>> v = valleySets(QString::fromLatin1(w.text), &why);
            QVERIFY2(v && v->size() == w.sets.size(), qPrintable(why));
            for (std::size_t k = 0; k < w.sets.size(); ++k) {
                const ValleySet& a = (*v)[k];
                qInfo("%s: %g valleys, mz %.4f, md %.4f", w.text, a.g, a.mz, a.md);
                QCOMPARE(a.g, w.sets[k][0]);
                QVERIFY(std::abs(a.mz - w.sets[k][1]) < 6e-4 && std::abs(a.md - w.sets[k][2]) < 6e-4);
            }
        }
        // Ge's four L valleys (ml 1.59, mt 0.0815): (111) one along it -
        // ml and mt themselves - and three at cos²θ = 1/9; (100) four at
        // 1/3; (110) two at 2/3 and two across it (mt along, √(ml mt) in
        // the plane). Each md √(ml mt²/mz), the plane's determinant.
        const double ml = 1.59, mt = 0.0815;
        auto tilted = [&](double c2) { return 1 / (c2 / ml + (1 - c2) / mt); };
        const Want ge[] = {{"Ge(111)", {{1, ml, mt}, {3, tilted(1.0 / 9), 0}}},
                           {"Ge(100)", {{4, tilted(1.0 / 3), 0}}},
                           {"Ge(110)", {{2, tilted(2.0 / 3), 0}, {2, mt, std::sqrt(ml * mt)}}}};
        for (const Want& w : ge) {
            const std::optional<std::vector<ValleySet>> v = valleySets(QString::fromLatin1(w.text));
            QVERIFY(v && v->size() == w.sets.size());
            for (std::size_t k = 0; k < w.sets.size(); ++k) {
                const ValleySet& a = (*v)[k];
                const double md = std::sqrt(ml * mt * mt / w.sets[k][1]);
                qInfo("%s: %g valleys, mz %.4f, md %.4f", w.text, a.g, a.mz, a.md);
                QCOMPARE(a.g, w.sets[k][0]);
                QVERIFY(std::abs(a.mz / w.sets[k][1] - 1) < 1e-12 && std::abs(a.md / md - 1) < 1e-12);
                if (w.sets[k][2] > 0) QVERIFY(std::abs(a.md / w.sets[k][2] - 1) < 1e-12);
            }
        }
        // Masses of one's own, sets of one's own; refused.
        const std::optional<std::vector<ValleySet>> own = valleySets(QStringLiteral("Si(100) 0.98 0.19"));
        QVERIFY(own && own->size() == 2 && (*own)[0].mz == 0.98 && std::abs((*own)[1].md - std::sqrt(0.98 * 0.19)) < 1e-12);
        const std::optional<std::vector<ValleySet>> sets = valleySets(QStringLiteral("2 0.916 0.19; 4 0.19 0.417 0.05"));
        QVERIFY(sets && sets->size() == 2 && (*sets)[1].g == 4 && (*sets)[1].dE == 0.05 && (*sets)[0].dE == 0);
        QVERIFY(valleySets(QString()) && valleySets(QString())->empty());
        for (const char* bad : {"Si(001)", "2 0.916", "2 -0.9 0.19", "Ge(100) 1.59"}) {
            QString why;
            QVERIFY2(!valleySets(QString::fromLatin1(bad), &why) && !why.isEmpty(), bad);
        }
    }

    void valleyLadders()
    {
        // Si(100)'s electrons against a wall in a field of 50 MV/m (a
        // triangular well): each valley set a ladder of Airy's levels of its
        // own mass along the axis, E_n - Ec(wall) = (ħ²/2mz)^⅓ (qF)^⅔ a_n,
        // merged by energy; filled to a flat Fermi level, each state's
        // electrons per area g md kT/(πħ²) ln(1 + e^((Ef - E)/kT)), the sets'
        // their sums.
        const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "name": "si", "size": ["10", "20"]}],
            "physics": [{"type": "quantum", "tag": "qw", "h1d": "%1", "F": "5e7[V/m]", "heavy": false, "light": false,
              "etan": "-0.25[V] + 5e7[V/m]*(10[nm] - y)", "features": [
              {"type": "material", "source": "user", "Ec": "0[eV]", "Ev": "-1.12[eV]", "me": "0.5", "valleys": "Si(100)", "selection": {"all": true}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.5"}}],
          "studies": [{"type": "study", "steps": [{"type": "states", "states": 3}]}]})json")
                                    .arg(slowBuild() ? QStringLiteral("0.1[nm]") : QStringLiteral("0.05[nm]"))
                                    .toUtf8();
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        QCOMPARE(s.snapshotCount(), 6);
        // (Each ladder's highest state checked for the carriers it holds: the
        // two valleys' third, near the Fermi level, 3 % of them.)
        QVERIFY2(s.log.join(QLatin1Char(' ')).contains(QLatin1String("in 2 valley sets")), qPrintable(s.log.join(QLatin1Char('\n'))));
        QVERIFY2(s.log.join(QLatin1Char(' ')).contains(QLatin1String("its highest e state found holds")), qPrintable(s.log.join(QLatin1Char('\n'))));
        s.select(0);
        const double airy[3] = {2.338107410459767, 4.087949444130970, 5.520559828095551};
        const double mz[2] = {0.916, 0.190}, md[2] = {0.190, std::sqrt(0.916 * 0.190)}, g[2] = {2, 4};
        const double kT = KB * 300 / Q, Ef = -0.25;
        int seen[2] = {0, 0};
        double last = -1e9, worst = 0, sets[2] = {0, 0}, all = 0;
        for (int k = 1; k <= 6; ++k) {
            const double E = eV(s, QStringLiteral("qw.Ee%1").arg(k));
            const int v = int(global(s, QStringLiteral("qw.Ve%1").arg(k))) - 1;
            QVERIFY(v == 0 || v == 1);
            const int n = seen[v]++;
            const double want = -0.5 + std::cbrt(HB2M / mz[v]) * std::pow(0.05, 2.0 / 3) * airy[n];
            const double N = global(s, QStringLiteral("qw.Ne%1").arg(k));
            const double filled = g[v] * md[v] * M0 * KB * 300 / (M_PI * Hbar * Hbar) * std::log1p(std::exp((Ef - E) / kT));
            qInfo("e%d: valley set %d's level %d, %.7f eV (Airy's %.7f); %.6g /cm² (%.6g)", k, v + 1, n + 1, E, want, N * 1e-4, filled * 1e-4);
            QVERIFY(E >= last);
            last = E;
            worst = std::max(worst, std::abs((E + 0.5) / (want + 0.5) - 1));
            QVERIFY2(std::abs(N / filled - 1) < 1e-9, qPrintable(QString::number(N / filled - 1)));
            sets[v] += N;
            all += N;
        }
        QVERIFY2(worst < 2e-5, qPrintable(QString::number(worst)));
        QCOMPARE(seen[0], 3);
        QCOMPARE(seen[1], 3);
        for (int v = 0; v < 2; ++v)
            QVERIFY(std::abs(global(s, QStringLiteral("qw.Nv%1").arg(v + 1)) / sets[v] - 1) < 1e-12);
        QVERIFY(std::abs(global(s, QStringLiteral("qw.Ns")) / all - 1) < 1e-12);
        // The electrons along the line the same: qw.n's integral (the mesh's
        // quadratic interpolation of it, 0.5 nm: to 1e-3).
        double integral = 0;
        const int m = 4000;
        for (int j = 0; j < m; ++j) integral += at(s, QStringLiteral("qw.n"), {5, (j + 0.5) * 20.0 / m}) * 20e-9 / m;
        qInfo("qw.n along the line %.6g /cm², qw.Ns %.6g", integral * 1e-4, all * 1e-4);
        QVERIFY2(std::abs(integral / all - 1) < 1e-3, qPrintable(QString::number(integral / all - 1)));
        // One valley set only of a direct gap's: refused a spectrum; a wire's refused.
        QByteArray spectrum = json;
        spectrum.replace(R"({"type": "states", "states": 3})", R"({"type": "spectrum", "energies": "1.2[eV]"})");
        QVERIFY(run(modelOf(spectrum)).error.contains(QLatin1String("indirect")));
        QByteArray wire = json;
        wire.replace(R"("tag": "qw",)", R"("tag": "qw", "space": "2d",)");
        QVERIFY(run(modelOf(wire)).error.contains(QLatin1String("valleys are a well's")));
        QByteArray bad = json;
        bad.replace("Si(100)", "Si(010)");
        QVERIFY(run(modelOf(bad)).error.contains(QLatin1String("its valleys")));
        // Sets of one's own, the second's edge 0.05 eV up: its ladder Airy's
        // raised by that.
        QByteArray raised = json;
        raised.replace("Si(100)", "2 0.916 0.190; 4 0.190 0.417 0.05");
        const Run up = run(modelOf(raised));
        QVERIFY2(up.error.isEmpty(), qPrintable(up.error));
        up.solution->select(0);
        int raisedLevels = 0;
        for (int k = 1; k <= 6; ++k) {
            if (int(global(*up.solution, QStringLiteral("qw.Ve%1").arg(k))) != 2) continue;
            const double E = eV(*up.solution, QStringLiteral("qw.Ee%1").arg(k));
            const double want = -0.5 + 0.05 + std::cbrt(HB2M / 0.190) * std::pow(0.05, 2.0 / 3) * airy[raisedLevels++];
            QVERIFY2(std::abs((E + 0.45) / (want + 0.45) - 1) < 2e-5, qPrintable(QStringLiteral("%1 %2").arg(E).arg(want)));
        }
        QCOMPARE(raisedLevels, 3);
        // A material's valleys (a compound's, read as every physics reads its
        // properties): GaAs's own mass in one set of two - the levels the
        // same, each state's electrons twice.
        const double me = bandParameters(QStringLiteral("GaAs"), 0, 0, 300)->me;
        auto gaas = [&](const QString& valleys) {
            QByteArray w = heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("8"),
                                           QStringLiteral("10"), QStringLiteral(R"(, "heavy": false, "light": false, "etan": "0.02[V]")"),
                                           QStringLiteral(R"({"type": "states", "states": 2})"));
            if (!valleys.isEmpty()) w.replace(R"("compound": "GaAs")", QStringLiteral(R"("compound": "GaAs", "valleys": "%1")").arg(valleys).toUtf8());
            return run(modelOf(w));
        };
        const Run one = gaas(QString()), two = gaas(QStringLiteral("2 %1 %1").arg(me, 0, 'g', 17));
        QVERIFY2(one.error.isEmpty() && two.error.isEmpty(), qPrintable(one.error + two.error));
        one.solution->select(0);
        two.solution->select(0);
        const double E1 = eV(*one.solution, QStringLiteral("qw.Ee1")), E2 = eV(*two.solution, QStringLiteral("qw.Ee1"));
        const double N1 = global(*one.solution, QStringLiteral("qw.Ne1")), N2 = global(*two.solution, QStringLiteral("qw.Ne1"));
        qInfo("GaAs's valleys from its material: e1 %.9f eV (%.9f), %.6g /cm² (%.6g, twice)", E2, E1, N2 * 1e-4, N1 * 1e-4);
        QVERIFY(std::abs(E2 - E1) < 1e-9 && std::abs(N2 / N1 - 2) < 1e-9);
    }

    void at77K()
    {
        // At 77 K: the compounds' gaps Varshni's there (GaAs 1.5076 eV, as
        // measured 1.508) and the well's states BenDaniel and Duke's of
        // them; the carriers' statistics kT's - the electrons' sheet, the
        // gain crossing 0 at the quasi-Fermi levels' separation, the
        // spontaneous emission's relation to the gain - and the gain higher
        // than at 300 K.
        const BandParameters w = *bandParameters(QStringLiteral("GaAs"), 0, 0, 77), b = *bandParameters(QStringLiteral("AlGaAs"), 0.3, 0, 77);
        qInfo("GaAs at 77 K: %.5f eV (Varshni %.5f; measured 1.508)", w.Eg, 1.519 - 0.5405e-3 * 77 * 77 / (77 + 204));
        QVERIFY(std::abs(w.Eg - (1.519 - 0.5405e-3 * 77 * 77 / (77 + 204))) < 1e-9);
        QVERIFY(std::abs(w.Eg - 1.508) < 1e-3);
        const Run r = run(modelOf(heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"),
                                                  QStringLiteral("10"), QStringLiteral("20"), QStringLiteral(R"(, "T": "77[K]")"),
                                                  QStringLiteral(R"({"type": "states", "states": 1})"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        for (int carrier = 0; carrier < 2; ++carrier) {
            const double V0 = carrier == 0 ? (b.VBO + b.Eg) - (w.VBO + w.Eg) : w.VBO - b.VBO;
            const double mw = carrier == 0 ? w.me : 1 / (w.gamma1 - 2 * w.gamma2), mb = carrier == 0 ? b.me : 1 / (b.gamma1 - 2 * b.gamma2);
            auto f = [&](double E) {
                const double k = std::sqrt(E * mw / HB2M), kappa = std::sqrt((V0 - E) * mb / HB2M);
                return k / mw * std::sin(k * 5) - kappa / mb * std::cos(k * 5);
            };
            const double expected = root(f, 1e-9, std::min(V0 * 0.999999, HB2M * M_PI * M_PI / (mw * 100)));
            const double found = carrier == 0 ? eV(*r.solution, QStringLiteral("qw.Ee1")) - (w.VBO + w.Eg) : w.VBO - eV(*r.solution, QStringLiteral("qw.Ehh1"));
            qInfo("77 K, %s1: %.7f eV above its band edge (BenDaniel and Duke %.7f)", carrier == 0 ? "e" : "hh", found, expected);
            QVERIFY2(std::abs(found / expected - 1) < 1e-6, qPrintable(QString::number(found / expected - 1)));
        }
        // The carriers at 77 and 300 K, unbroadened.
        double peaks[2] = {0, 0};
        for (int t = 0; t < 2; ++t) {
            const double T = t == 0 ? 77 : 300, kT = KB * T;
            const Run g = run(modelOf(box(QStringLiteral("20"), QStringLiteral("10"), QStringLiteral(R"(, "T": "%1[K]")").arg(T), QStringLiteral("0[eV]"),
                                          QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.56[eV], 1.76[eV], 201)", "states": 4,
                                                            "linewidth": "0[eV]", "density": "3e18[1/cm^3]"})x"))));
            QVERIFY2(g.error.isEmpty(), qPrintable(g.error));
            Solution& s = *g.solution;
            s.select(0);
            const double dF = global(s, QStringLiteral("qw.dF")), Fc = global(s, QStringLiteral("qw.Fc"));
            double sheet = 0;
            for (int n = 1; n <= 4; ++n) sheet += 0.067 * M0 * kT / (M_PI * Hbar * Hbar) * std::log1p(std::exp((Fc - global(s, QStringLiteral("qw.Ee%1").arg(n))) / kT));
            QVERIFY2(std::abs(sheet / (3e24 * 10e-9) - 1) < 1e-6, qPrintable(QString::number(sheet / 3e24 / 10e-9)));
            double crossing = NAN, worst = 0, most = 0;
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                const double E = global(s, QStringLiteral("qw.Ephoton")), g1 = global(s, QStringLiteral("qw.gTE"));
                peaks[t] = std::max(peaks[t], g1);
                if (k > 0 && std::isnan(crossing)) {
                    s.select(k - 1);
                    const double g0 = global(s, QStringLiteral("qw.gTE")), e0 = global(s, QStringLiteral("qw.Ephoton"));
                    if (g0 > 0 && g1 <= 0) crossing = e0 + (E - e0) * g0 / (g0 - g1);
                    s.select(k);
                }
                const double aTE = global(s, QStringLiteral("qw.alphaTE")), aTM = global(s, QStringLiteral("qw.alphaTM"));
                const double expected = 3.6 * 3.6 * E * E / (M_PI * M_PI * Hbar * Hbar * Hbar * 299792458.0 * 299792458.0) * (2 * aTE + aTM) / 3
                                        / std::expm1((E - dF) / kT);
                const double got = global(s, QStringLiteral("qw.rsp"));
                most = std::max(most, got);
                if (std::isfinite(expected) && expected != 0) worst = std::max(worst, std::abs(got - expected) / std::max(std::abs(expected), 1e-6 * most));
            }
            qInfo("%g K: the gain's peak %.0f /cm, crossing 0 at %.5f eV, the quasi-Fermi levels %.5f eV apart; the emission its relation to "
                  "the gain to %.2g",
                  T, peaks[t] / 100, crossing / Q, dF / Q, worst);
            QVERIFY2(std::abs(crossing - dF) < 5e-4 * Q, qPrintable(QString::number((crossing - dF) / Q)));
            QVERIFY2(worst < 1e-6, qPrintable(QString::number(worst)));
        }
        QVERIFY(peaks[0] > peaks[1]);
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
        if (!slowBuild()) {
            // The k·p laser: its top valence band falling with k (no spurious
            // one); TE gain at 980 nm or so, more with more carriers, below
            // the parabolic holes' of the strained laser; emission with them.
            // (Under the sanitizers not: its paths the other k·p tests'.)
            timer.start();
            const Model m = load(QStringLiteral("kp_laser_gain.qfem"));
            const StudyRun bands = solve(m, QStringLiteral("std1"));
            Solution& b = *bands.solutions.front();
            QCOMPARE(b.snapshotCount(), 21);
            b.select(0);
            const double top = eV(b, QStringLiteral("qw.Ev1"));
            double highest = -1e9;
            for (int k = 1; k < b.snapshotCount(); ++k) {
                b.select(k);
                highest = std::max(highest, eV(b, QStringLiteral("qw.Ev1")));
            }
            const StudyRun gain = solve(m, QStringLiteral("std2"));
            QCOMPARE(int(gain.solutions.size()), 2);
            std::vector<double> peak, at;
            double Rsp = 0;
            for (const auto& s : gain.solutions) {
                double best = -1e300, where = 0;
                for (int k = 0; k < s->snapshotCount(); ++k) {
                    s->select(k);
                    if (global(*s, QStringLiteral("qw.gTE")) > best) {
                        best = global(*s, QStringLiteral("qw.gTE"));
                        where = 299792458.0 / s->snapshotFrequency(k);
                    }
                }
                peak.push_back(best);
                at.push_back(where);
                Rsp = global(*s, QStringLiteral("qw.Rsp"));
            }
            qInfo("k·p laser: the top band %.4f eV at k = 0, at most %.4f beyond; TE gain peaks %.0f, %.0f /cm (at %.1f nm with 5e18 cm⁻³), "
                  "emission %.3g /(s cm³); %lld ms",
                  top, highest, peak[0] / 100, peak[1] / 100, at[1] * 1e9, Rsp * 1e-6, timer.elapsed());
            QVERIFY(highest < top);
            QVERIFY(peak[1] > peak[0] && peak[0] > 0);
            QVERIFY(at[1] > 940e-9 && at[1] < 1020e-9);
            QVERIFY(Rsp > 0);
        }
        if (!slowBuild()) {
            // The modulator: its exciton's line moves down with the field,
            // less bound, broader; Δn below the edge changes with it. (Under
            // the sanitizers not: its physics aBoundContinuum's and the
            // excitons'.)
            timer.start();
            const Model m = load(QStringLiteral("gaas_eam_excitons.qfem"));
            const StudyRun run = solve(m, QStringLiteral("std1"));
            QCOMPARE(int(run.solutions.size()), 3);
            std::vector<double> Ex, Eb, width, dn;
            for (const auto& s : run.solutions) {
                s->select(0);
                Ex.push_back(eV(*s, QStringLiteral("qw.Ex")));
                Eb.push_back(eV(*s, QStringLiteral("qw.Eb")));
                width.push_back(eV(*s, QStringLiteral("qw.linewidth")));
                dn.push_back(spectrumAt(*s, 1.44, "qw.dnTE"));
            }
            qInfo("modulator: the exciton at %.4f, %.4f, %.4f eV, bound by %.2f, %.2f, %.2f meV, broadened %.1f, %.1f, %.1f meV; Δn at 1.44 eV "
                  "%.2g, %.2g; %lld ms",
                  Ex[0], Ex[1], Ex[2], Eb[0] * 1e3, Eb[1] * 1e3, Eb[2] * 1e3, width[0] * 1e3, width[1] * 1e3, width[2] * 1e3, dn[1] - dn[0],
                  dn[2] - dn[0], timer.elapsed());
            QVERIFY(Ex[1] < Ex[0] && Ex[2] < Ex[1]);
            QVERIFY(Eb[1] < Eb[0] && Eb[2] < Eb[1] && Eb[0] > 0.007 && Eb[0] < 0.012);
            QVERIFY(width[2] > width[1] && width[1] > width[0]);
            QVERIFY(std::abs(dn[2] - dn[0]) > std::abs(dn[1] - dn[0]) && std::abs(dn[1] - dn[0]) > 0);
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
