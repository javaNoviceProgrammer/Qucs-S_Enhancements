/*
 * test_fem_modulator.cpp - the multiphysics solver's modulator physics
 *                          against what is known: free carriers' index
 *                          (Drude's, band filling) and absorption in a
 *                          III-V, a mode's change by perturbation
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_export.h"
#include "fem_materials.h"
#include "fem_mesh.h"
#include "fem_optics.h"
#include "fem_problem.h"
#include "fem_results.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QtTest>

#include <cmath>
#include <cstring>
#include <functional>

using namespace qucs_s::fem;

namespace {

constexpr double Q = 1.602176634e-19, Hbar = 1.054571817e-34, M0 = 9.1093837015e-31, Eps0 = 8.8541878188e-12, C = 299792458.0,
                 KB = 1.380649e-23;

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

/// The modes' elements: a slow build's linear (Whitney's), else quadratic.
QString order()
{
    return slowBuild() ? QStringLiteral("1") : QStringLiteral("2");
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

double at(const Solution& s, const QString& expression, QPointF p)
{
    const Locator locator(s.mesh());
    const std::optional<double> v = evaluateAt(s, locator, expression, p);
    return v ? *v : NAN;
}

double relative(double a, double b)
{
    return std::abs(a - b) / std::max(std::abs(b), 1e-300);
}

/// A guide (µm): a core 2 by 0.5 (material \a core) in a cladding of n 3.17,
/// 4 by 3; the modes physics' features \a features, one mode at \a lambda.
/// \a extra: the mode step's more; \a before: steps before it (a sweep of V).
QByteArray guide(const QString& features, const QString& lambda = QStringLiteral("1550[nm]"),
                 const QString& core = QStringLiteral(R"("material": "Indium phosphide", "n": "3.3")"), const QString& extra = {},
                 const QString& before = {})
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "V", "expression": "0[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "clad", "position": ["-2", "-1.5"], "size": ["4", "3"]},
                     {"type": "rectangle", "name": "core", "position": ["-1", "-0.25"], "size": ["2", "0.5"]}],
        "materials": [{"label": "clad", "n": "3.17", "selection": {"only": ["clad"]}},
                      {%3, "selection": {"only": ["core"]}}],
        "physics": [{"type": "modes", "tag": "emw", "order": "%7", "features": [%1]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%5"}}],
      "studies": [{"type": "study", "steps": [%6 {"type": "mode", "frequency": "%2", "modes": 1, "neff": "3.3" %4}]}]})json")
        .arg(features, lambda, core, extra, slowBuild() ? QStringLiteral("0.3") : QStringLiteral("0.1"), before, order())
        .toUtf8();
}

/// A rib (µm): a slab of n 3.35 (mqw, 0.4 thick) across the window on a
/// substrate of n 3.17, a ridge of n 3.17 (2 wide, 1.2 high) on it, air
/// round it; the mode step's \a step (its more).
QByteArray rib(const QString& step, const QString& before = {})
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "V", "expression": "0[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "air", "position": ["-3", "-1.5"], "size": ["6", "3.5"]},
                     {"type": "rectangle", "name": "substrate", "position": ["-3", "-1.5"], "size": ["6", "1.5"]},
                     {"type": "rectangle", "name": "mqw", "position": ["-3", "0"], "size": ["6", "0.2"]},
                     {"type": "rectangle", "name": "ridge", "position": ["-1", "0.2"], "size": ["2", "1.4"]}],
        "materials": [{"label": "air", "n": "1", "selection": {"only": ["air"]}},
                      {"label": "InP", "n": "3.17", "selection": {"only": ["substrate"]}},
                      {"label": "SCH", "n": "3.26", "selection": {"only": ["ridge"]}},
                      {"label": "Q", "n": "3.35 + 0.002*V/1[V]", "selection": {"only": ["mqw"]}}],
        "physics": [{"type": "modes", "tag": "emw", "order": "%4"}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
      "studies": [{"type": "study", "steps": [%3 {"type": "mode", "frequency": "1550[nm]", "neff": "3.3" %1}]}]})json")
        .arg(step, slowBuild() ? QStringLiteral("0.18") : QStringLiteral("0.12"), before, order())
        .toUtf8();
}

/// A GaAs well 8 nm between Al0.3GaAs barriers 10 nm (nm, along y) in the
/// middle of a guide's core (1.5 µm by 0.6, n 3.55, in 3.4); a quantum
/// wells physics on the well and its barriers, its field \a F; the wave
/// feature on the well \a well; a Spectrum step from 1.40 to 2.0 eV, then
/// a mode at 1.46 eV - \a spectrum: the spectrum alone, the modes off. The
/// stack a Layered Region (50 nm along, two rows in each layer).
QByteArray stackGuide(const QString& well, const QString& F = QStringLiteral("0[V/m]"), bool spectrum = false, const QString& energies = {})
{
    const double step = slowBuild() ? 0.02 : 0.01;
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Fw", "expression": "0[V/m]"}],
      "components": [{"unit": "nm",
        "geometry": [{"type": "rectangle", "name": "clad", "position": ["-1500", "-1000"], "size": ["3000", "2000"]},
                     {"type": "rectangle", "name": "core", "position": ["-750", "-300"], "size": ["1500", "600"]},
                     {"type": "rectangle", "name": "below", "position": ["-750", "-14"], "size": ["1500", "10"]},
                     {"type": "rectangle", "name": "well", "position": ["-750", "-4"], "size": ["1500", "8"]},
                     {"type": "rectangle", "name": "above", "position": ["-750", "4"], "size": ["1500", "10"]}],
        "materials": [{"label": "clad", "n": "3.4", "selection": {"only": ["clad"]}},
                      {"label": "core", "n": "3.55", "selection": {"only": ["core"]}},
                      {"material": "barrier", "compound": "AlGaAs", "x": "0.3", "n": "3.55", "selection": {"only": ["below", "above"]}},
                      {"material": "well", "compound": "GaAs", "n": "3.6", "selection": {"only": ["well"]}}],
        "physics": [{"type": "quantum", "tag": "qw", "F": "%2", "h1d": "%5", "selection": {"only": ["below", "well", "above"]}},
                    {"type": "modes", "tag": "emw", "enabled": %10, "order": "%11", "features": [%1]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%6",
                 "features": [{"type": "layered", "hlateral": "50", "count": 2, "selection": {"only": ["below", "well", "above"]}}]}}],
      "studies": [{"type": "study", "steps": [
        {"type": "spectrum", "tag": "sp1", "enabled": %3, "energies": "%4", "states": %7, "linewidth": "%8", "lineshape": "gaussian", "continuum": "bound"},
        {"type": "mode", "enabled": %10, "frequency": "%9[Hz]", "modes": 1, "neff": "3.55"}]}]})json")
        .arg(well, F, spectrum ? QStringLiteral("true") : QStringLiteral("false"),
             energies.isEmpty() ? QStringLiteral("linspace(1.40[eV], 2.0[eV], %1)").arg(qRound(0.6 / step) + 1) : energies,
             slowBuild() ? QStringLiteral("0.25[nm]") : QStringLiteral("0.1[nm]"), slowBuild() ? QStringLiteral("150") : QStringLiteral("100"))
        .arg(slowBuild() ? 4 : 8)
        .arg(slowBuild() ? QStringLiteral("15[meV]") : QStringLiteral("6[meV]"))
        .arg(1.46 * Q / 6.62607015e-34, 0, 'g', 17)
        .arg(spectrum ? QStringLiteral("false") : QStringLiteral("true"))
        .arg(order())
        .toUtf8();
}

/// A stack (nm) of \a layers under a mesa 2 µm wide: wells 6 nm (n 3.45) and
/// barriers 8 nm (3.3) from y 0 up, an InP ridge 1 µm on it, InP below, air
/// round it; the mesh's features \a mesh (custom, hmax 200 nm), a mode at
/// 1550 nm.
QByteArray stack(int layers, const QString& mesh, const QString& extra = {})
{
    QStringList geometry, wells, barriers;
    geometry << QStringLiteral(R"({"type": "rectangle", "name": "air", "position": ["-3000", "-2000"], "size": ["6000", "4000"]})")
             << QStringLiteral(R"({"type": "rectangle", "name": "substrate", "position": ["-3000", "-2000"], "size": ["6000", "2000"]})");
    double y = 0;
    for (int k = 0; k < layers; ++k) {
        const double t = k % 2 ? 8 : 6;
        geometry << QStringLiteral(R"({"type": "rectangle", "name": "L%1", "position": ["-1000", "%2"], "size": ["2000", "%3"]})").arg(k).arg(y).arg(t);
        (k % 2 ? barriers : wells) << QStringLiteral("\"L%1\"").arg(k);
        y += t;
    }
    geometry << QStringLiteral(R"({"type": "rectangle", "name": "ridge", "position": ["-1000", "%1"], "size": ["2000", "1000"]})").arg(y);
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "nm",
        "geometry": [%1],
        "materials": [{"label": "air", "n": "1", "selection": {"only": ["air"]}},
                      {"label": "InP", "n": "3.17", "selection": {"only": ["substrate", "ridge"]}},
                      {"label": "well", "n": "3.45", "selection": {"only": [%2]}},
                      {"label": "barrier", "n": "3.3", "selection": {"only": [%3]}}],
        "physics": [{"type": "modes", "tag": "emw", "order": "%6"}],
        "mesh": {"size": "normal", "custom": true, "hmax": "200", "features": [%4]}}],
      "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "3.3", "polarization": "te" %5}]}]})json")
        .arg(geometry.join(QLatin1String(", ")), wells.join(QLatin1String(", ")), barriers.join(QLatin1String(", ")), mesh, extra, order())
        .toUtf8();
}

/// Its layers as a Layered Region (\a lateral nm, \a count across each).
QString layeredMesh(int layers, double lateral, int count)
{
    QStringList names;
    for (int k = 0; k < layers; ++k) names << QStringLiteral("\"L%1\"").arg(k);
    return QStringLiteral(R"({"type": "layered", "hlateral": "%1", "count": %2, "selection": {"only": [%3]}})")
        .arg(lateral)
        .arg(count)
        .arg(names.join(QLatin1String(", ")));
}

/// A Model's mesh alone.
MeshBuild meshOf(const QByteArray& json)
{
    const Model m = modelOf(json);
    const std::shared_ptr<ParameterScope> p = evaluateParameters(m);
    const GeometryBuild g = buildGeometry(m, *p);
    return buildMesh(m, *p, g.topology);
}

/// A silicon pn junction (µm): p from y 0 to 0.2, n from 0.2 to 0.4 - 2 µm
/// wide, or (\a mesa) the n layer 4 wide under a p mesa of 2 -, the anode
/// on the p layer's bottom at Va, the cathode on the n layer's top; the
/// physics' \a space ("1d" with \a more), the mesh's features \a mesh (a
/// mesh of \a hmax), the study's \a steps.
QByteArray junction1d(const QString& space, const QString& mesh, bool mesa = false, const QString& steps = QStringLiteral(R"({"type": "stationary"})"),
                      const QString& more = {}, const QString& hmax = QStringLiteral("0.5"))
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Va", "expression": "0.5[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "p", "position": ["-1", "0"], "size": ["2", "0.2"]},
                     {"type": "rectangle", "name": "n", "position": ["%4", "0.2"], "size": ["%5", "0.2"]}],
        "materials": [{"material": "Silicon", "selection": {"all": true}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "space": "%1" %6, "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["p"]}},
          {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"only": ["n"]}},
          {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["p"], "side": "bottom"}},
          {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "top"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%7", "features": [%2]}}],
      "studies": [{"type": "study", "steps": [%3]}]})json")
        .arg(space, mesh, steps, mesa ? QStringLiteral("-2") : QStringLiteral("-1"), mesa ? QStringLiteral("4") : QStringLiteral("2"), more, hmax)
        .toUtf8();
}

/// A wave feature on the core: \a flags and N 1e18, P 5e17 per cm³.
QString onCore(const QString& flags)
{
    return QStringLiteral(R"({"type": "wave", %1, "Ncarriers": "1e18[1/cm^3]", "Pcarriers": "5e17[1/cm^3]", "selection": {"only": ["core"]}})")
        .arg(flags);
}

// InP's (Vurgaftman, Meyer and Ram-Mohan's): me, γ1, γ2, Ep; its gap at 300 K;
// the library's edge A (1/m), Aspnes and Studna's fitted.
constexpr double InpMe = 0.0795, InpGamma1 = 5.08, InpGamma2 = 1.60, InpEp = 20.7 * Q, InpGap = 1.344 * Q, InpEdge = 1.16e7;

double heavyHoles()
{
    return 1 / (InpGamma1 - 2 * InpGamma2);
}
double lightHoles()
{
    return 1 / (InpGamma1 + 2 * InpGamma2);
}

/// Drude's Δn of N electrons and P holes (per m³) at λ in a medium of index n.
double drude(double N, double P, double lambda, double n, double me, double mh)
{
    return -Q * Q * lambda * lambda / (8 * M_PI * M_PI * C * C * Eps0 * n * M0) * (N / me + P / mh);
}

/// ∫ f over [a, b], Gauss-Legendre's (16 points) on \a pieces equal pieces.
double gauss(const std::function<double(double)>& f, double a, double b, int pieces)
{
    static const double x[8] = {0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,
                                0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499};
    static const double w[8] = {0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
                                0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541};
    double sum = 0;
    const double h = (b - a) / pieces;
    for (int k = 0; k < pieces; ++k) {
        const double mid = a + (k + 0.5) * h;
        for (int i = 0; i < 8; ++i) sum += w[i] * (f(mid - 0.5 * h * x[i]) + f(mid + 0.5 * h * x[i]));
    }
    return sum * 0.5 * h;
}

} // namespace

class TestFemModulator : public QObject
{
    Q_OBJECT

private slots:
    /// A III-V's plasma dispersion, Drude's of its material's masses, at the
    /// mode's wavelength; the masses given; silicon's and given coefficients.
    void drudeOfTheMaterialsMasses()
    {
        const double N = 1e24, P = 5e23;
        const double mhh = heavyHoles(), mlh = lightHoles();
        const double mh = (std::pow(mhh, 1.5) + std::pow(mlh, 1.5)) / (std::sqrt(mhh) + std::sqrt(mlh));
        const QPointF middle(0, 0);
        Run with;
        for (const double lambda : {1.55e-6, 1.3e-6}) {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("plasma": true, "plasmamodel": "drude")")),
                                            QStringLiteral("%1[nm]").arg(lambda * 1e9))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            if (lambda == 1.55e-6) with = r;
            const double dn = at(*r.solution, QStringLiteral("emw.dncarriers"), middle);
            QVERIFY2(relative(dn, drude(N, P, lambda, 3.3, InpMe, mh)) < 1e-9, qPrintable(QString::number(dn)));
            // (Some five times silicon's per electron at 1550 nm: 8.8e-22 cm³.)
            if (lambda == 1.55e-6) QVERIFY(std::abs(drude(1e24, 0, lambda, 3.3, InpMe, mh)) / 1e18 > 4.5 * 8.8e-22);
            // Only where the feature is: the cladding has none.
            QCOMPARE(at(*r.solution, QStringLiteral("emw.dncarriers"), QPointF(0, 1)), 0.0);
            QCOMPARE(at(*r.solution, QStringLiteral("emw.alphacarriers"), middle), 0.0);
        }
        // Its masses given.
        {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("plasma": true, "plasmamodel": "drude", "mepl": "0.08", "mhpl": "0.5")")))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QVERIFY(relative(at(*r.solution, QStringLiteral("emw.dncarriers"), middle), drude(N, P, 1.55e-6, 3.3, 0.08, 0.5)) < 1e-9);
        }
        // Silicon's (Soref and Bennett's), as it was.
        {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("plasma": true)")))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QVERIFY(relative(at(*r.solution, QStringLiteral("emw.dncarriers"), middle), -(8.8e-22 * 1e18 + 8.5e-18 * std::pow(5e17, 0.8))) < 1e-9);
            QVERIFY(relative(at(*r.solution, QStringLiteral("emw.alphacarriers"), middle), (8.5e-18 * 1e18 + 6.0e-18 * 5e17) * 100) < 1e-9);
        }
        // Coefficients given.
        {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("plasma": true, "plasmamodel": "user", "dndN": "-4e-21[cm^3]", "dndP": "-1.5e-21[cm^3]")")))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QVERIFY(relative(at(*r.solution, QStringLiteral("emw.dncarriers"), middle), -(4e-21 * 1e18 + 1.5e-21 * 5e17)) < 1e-9);
        }
        // The mode slower without them: its index lowered by them.
        const Run without = run(modelOf(guide(QString())));
        QVERIFY2(without.error.isEmpty(), qPrintable(without.error));
        const double shift = global(*with.solution, QStringLiteral("emw.neff")) - global(*without.solution, QStringLiteral("emw.neff"));
        QVERIFY2(shift < 0 && shift > drude(N, P, 1.55e-6, 3.3, InpMe, mh), qPrintable(QString::number(shift)));
        // A material without masses: said.
        const Run none = run(modelOf(guide(onCore(QStringLiteral(R"("plasma": true, "plasmamodel": "drude")")), QStringLiteral("1550[nm]"),
                                           QStringLiteral(R"("label": "plain", "n": "3.3")"))));
        QVERIFY2(none.error.contains(QLatin1String("me")), qPrintable(none.error));
    }

    /// Free-carrier absorption: the material's cross-sections, as λ²; given
    /// ones; a material without them, silicon's index with it: said.
    void freeCarrierAbsorption()
    {
        const QPointF middle(0, 0);
        for (const double lambda : {1.55e-6, 1.3e-6}) {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("fca": true)")), QStringLiteral("%1[nm]").arg(lambda * 1e9))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            // InP's 1e-18 and 2e-17 cm²: 1 + 10 per cm at 1550 nm.
            const double alpha = (1e-18 * 1e18 + 2e-17 * 5e17) * 100 * std::pow(lambda / 1.55e-6, 2);
            QVERIFY2(relative(at(*r.solution, QStringLiteral("emw.alphacarriers"), middle), alpha) < 1e-9,
                     qPrintable(QString::number(at(*r.solution, QStringLiteral("emw.alphacarriers"), middle))));
            QCOMPARE(at(*r.solution, QStringLiteral("emw.dncarriers"), middle), 0.0);
            // The mode loses power: some of the core's α.
            const double loss = 2 * global(*r.solution, QStringLiteral("emw.neff_im")) * 2 * M_PI / lambda;
            QVERIFY2(loss > 0.3 * alpha && loss < alpha, qPrintable(QString::number(loss / alpha)));
        }
        {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("fca": true, "fcasource": "user", "sigmaFCn": "3e-18[cm^2]", "sigmaFCp": "1e-17[cm^2]")")))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QVERIFY(relative(at(*r.solution, QStringLiteral("emw.alphacarriers"), middle), (3e-18 * 1e18 + 1e-17 * 5e17) * 100) < 1e-9);
        }
        // Drude's index with it: both.
        {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("plasma": true, "plasmamodel": "drude", "fca": true)")))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QVERIFY(at(*r.solution, QStringLiteral("emw.dncarriers"), middle) < 0);
            QVERIFY(relative(at(*r.solution, QStringLiteral("emw.alphacarriers"), middle), 1100) < 1e-9);
        }
        const Run none = run(modelOf(guide(onCore(QStringLiteral(R"("fca": true)")), QStringLiteral("1550[nm]"),
                                           QStringLiteral(R"("label": "plain", "n": "3.3")"))));
        QVERIFY2(none.error.contains(QLatin1String("sigmaFCn")), qPrintable(none.error));
        const Run twice = run(modelOf(guide(onCore(QStringLiteral(R"("plasma": true, "fca": true)")))));
        QVERIFY2(twice.error.contains(QLatin1String("count twice")), qPrintable(twice.error));
    }

    /// Band filling against its integrals done here: degenerate electrons
    /// (4 K) bleach the edge up to their Fermi level, the index below the gap
    /// Kramers-Kronig's of that; few carriers (300 K) as N; holes; the table.
    void bandFillingAgainstItsIntegral()
    {
        BandFillingMedium m;
        m.Eg = InpGap;
        m.me = InpMe;
        m.mhh = heavyHoles();
        m.mlh = lightHoles();
        m.Ep = InpEp;
        m.n = 3.2;
        const double mr[2] = {m.me * m.mhh / (m.me + m.mhh), m.me * m.mlh / (m.me + m.mlh)};
        // α_j = s_j √(E - Eg)/E.
        double s[2];
        for (int j = 0; j < 2; ++j) s[j] = Q * Q * m.Ep * std::pow(2 * mr[j] * M0 / (Hbar * Hbar), 1.5) * Hbar / (12 * M_PI * m.n * C * Eps0 * M0);
        // Degenerate: N = (2 me m0 EF/ħ²)^{3/2}/3π²; bleached where the
        // electron's energy x mr/me is below EF.
        m.T = 4;
        const double N = 1e24;
        const double EF = std::pow(3 * M_PI * M_PI * N, 2.0 / 3) * Hbar * Hbar / (2 * m.me * M0);
        for (const double lambda : {1.55e-6, 1.3e-6, 1.0e-6}) {
            const double E = 2 * M_PI * Hbar * C / lambda;
            // Δn = (cħ/π) ∫ Δα(E')/(E'² - E²) dE', x = E' - Eg = u².
            double integral = 0;
            for (int j = 0; j < 2; ++j) {
                const double top = std::sqrt(EF * m.me / mr[j]);
                integral += gauss([&](double u) {
                    const double Ep = m.Eg + u * u;
                    return -s[j] * u / Ep / (Ep * Ep - E * E) * 2 * u;
                }, 0, top, 200);
            }
            const double expected = C * Hbar / M_PI * integral;
            const IndexChange got = bandFilling(m, E, N, false);
            QVERIFY2(relative(got.dn, expected) < 2e-3, qPrintable(QStringLiteral("%1 nm: %2, %3").arg(lambda * 1e9).arg(got.dn).arg(expected)));
            QCOMPARE(got.dalpha, 0.0);
        }
        // Above the gap: all its absorption bleached, short of the Fermi level.
        {
            const double x = 0.3 * EF * mr[1] / m.me, E = m.Eg + x;
            const IndexChange got = bandFilling(m, E, N, false);
            QVERIFY(relative(got.dalpha, -(s[0] + s[1]) * std::sqrt(x) / E) < 1e-6);
            // Beyond it, none.
            QVERIFY(std::abs(bandFilling(m, m.Eg + 3 * EF, N, false).dalpha) < 1e-12 * std::abs(got.dalpha));
            // The index continuous across the gap.
            // (Kramers-Kronig's of a √ onset: a √ cusp there, so 1e-8 apart.)
            const double below = bandFilling(m, m.Eg * (1 - 1e-8), N, false).dn, above = bandFilling(m, m.Eg * (1 + 1e-8), N, false).dn;
            QVERIFY2(relative(above, below) < 2e-3, qPrintable(QStringLiteral("%1 %2").arg(below).arg(above)));
        }
        // Few carriers at 300 K: Boltzmann's, f = (N/Nc) e^(-x mr/me kT), the index as N.
        m.T = 300;
        const double kT = KB * m.T, Nc = 2 * std::pow(m.me * M0 * kT / (2 * M_PI * Hbar * Hbar), 1.5);
        const double E = 2 * M_PI * Hbar * C / 1.3e-6;
        for (const double few : {1e21, 1e22}) {
            double integral = 0;
            for (int j = 0; j < 2; ++j)
                integral += gauss([&](double u) {
                    const double x = u * u, Ep = m.Eg + x;
                    return -s[j] * u / Ep * (few / Nc) * std::exp(-x * mr[j] / (m.me * kT)) / (Ep * Ep - E * E) * 2 * u;
                }, 0, std::sqrt(60 * kT * m.me / mr[1]), 400);
            const double expected = C * Hbar / M_PI * integral;
            const double got = bandFilling(m, E, few, false).dn;
            QVERIFY2(relative(got, expected) < 5e-3, qPrintable(QStringLiteral("%1: %2, %3").arg(few).arg(got).arg(expected)));
        }
        // Holes: their Fermi level of the heavy and light holes' states.
        {
            const double holes = 1e24;
            const IndexChange h = bandFilling(m, E, holes, true), e = bandFilling(m, E, holes, false);
            // (Heavier: fewer of their states filled - a third of the electrons'.)
            QVERIFY2(h.dn < 0 && h.dn > 0.5 * e.dn, qPrintable(QStringLiteral("%1 %2").arg(h.dn).arg(e.dn)));
            // Degenerate (4 K): both bands filled to one Fermi level, N =
            // (2 m0 EF/ħ²)^{3/2} (mhh^{3/2} + mlh^{3/2})/3π²; band j bleached
            // where its hole's energy x m_rj/m_j is below it.
            BandFillingMedium cold = m;
            cold.T = 4;
            const double mh[2] = {m.mhh, m.mlh};
            const double EFh = std::pow(3 * M_PI * M_PI * holes / (std::pow(m.mhh, 1.5) + std::pow(m.mlh, 1.5)), 2.0 / 3) * Hbar * Hbar / (2 * M0);
            const double E13 = 2 * M_PI * Hbar * C / 1.3e-6;
            double integral = 0;
            for (int j = 0; j < 2; ++j)
                integral += gauss([&](double u) {
                    const double Ep = m.Eg + u * u;
                    return -s[j] * u / Ep / (Ep * Ep - E13 * E13) * 2 * u;
                }, 0, std::sqrt(EFh * mh[j] / mr[j]), 200);
            const double expected = C * Hbar / M_PI * integral, got = bandFilling(cold, E13, holes, true).dn;
            QVERIFY2(relative(got, expected) < 2e-3, qPrintable(QStringLiteral("holes: %1, %2").arg(got).arg(expected)));
        }
        // Sommerfeld's enhancement: 2π√(R/x)/(1 - e^{-2π√(R/x)}) - at x = R,
        // far above (1 + a/2 + a²/12, a = 2π√(R/x)), and finite at the edge
        // (√x S → 2π√R).
        {
            const double R = 5e-3 * Q;
            QVERIFY(relative(sommerfeld(R, R), 2 * M_PI / (1 - std::exp(-2 * M_PI))) < 1e-12);
            QVERIFY(relative(sommerfeld(1e4 * R, R), 1 + M_PI / 100 + std::pow(2 * M_PI / 100, 2) / 12) < 1e-6);
            QVERIFY(relative(std::sqrt(1e-8 * R) * sommerfeld(1e-8 * R, R), 2 * M_PI * std::sqrt(R)) < 1e-9);
            QCOMPARE(sommerfeld(R, 0), 1.0);
            // Degenerate electrons with it (εr 12.4): their block's integral
            // here, each band's α times its own Rydberg's enhancement.
            BandFillingMedium x = m;
            x.T = 4;
            x.epsr = 12.4;
            for (const double lambda : {1.55e-6, 1.3e-6}) {
                const double Eph = 2 * M_PI * Hbar * C / lambda;
                double integral = 0;
                for (int j = 0; j < 2; ++j) {
                    const double Ry = 13.605693 * Q * mr[j] / (12.4 * 12.4);
                    integral += gauss([&](double u) {
                        const double Ep = m.Eg + u * u;
                        // (√x S, finite at u 0: 2π√Ry.)
                        const double a = 2 * M_PI * std::sqrt(Ry) / std::max(u, 1e-300);
                        const double rootS = u > 0 ? a * u / (1 - std::exp(-a)) : 2 * M_PI * std::sqrt(Ry);
                        return -s[j] * rootS / Ep / (Ep * Ep - Eph * Eph) * 2 * u;
                    }, 0, std::sqrt(EF * m.me / mr[j]), 400);
                }
                const double expected = C * Hbar / M_PI * integral, got = bandFilling(x, Eph, N, false).dn;
                QVERIFY2(relative(got, expected) < 2e-3, qPrintable(QStringLiteral("Sommerfeld, %1 nm: %2, %3").arg(lambda * 1e9).arg(got).arg(expected)));
                // (More than without it.)
                BandFillingMedium y = x;
                y.epsr = 0;
                QVERIFY(std::abs(got) > std::abs(bandFilling(y, Eph, N, false).dn));
            }
            // Above the gap, short of the Fermi level: -Σ α_j S_j exactly.
            const double xs = 0.3 * EF * mr[1] / m.me, Es = m.Eg + xs;
            double alpha = 0;
            for (int j = 0; j < 2; ++j) {
                const double Ry = 13.605693 * Q * mr[j] / (12.4 * 12.4), a = 2 * M_PI * std::sqrt(Ry / xs);
                alpha += s[j] * std::sqrt(xs) / Es * a / (1 - std::exp(-a));
            }
            QVERIFY(relative(bandFilling(x, Es, N, false).dalpha, -alpha) < 1e-6);
        }
        // The table, between its points, as the integral.
        const BandFillingTable table(m, E, false);
        for (const double between : {3.7e22, 2.2e23, 8.1e23, 4.4e24})
            QVERIFY2(relative(table.at(between).dn, bandFilling(m, E, between, false).dn) < 3e-3, qPrintable(QString::number(between)));
        QVERIFY(relative(table.at(1e18).dn, table.at(1e19).dn / 10) < 1e-12);
    }

    /// InP's edge as measured: Aspnes and Studna's k (1983) from 1.5 to 1.7 eV
    /// as the library's A √((E - Eg)/1 eV)(1 eV/E) to 4 %; Kane's Ep alone
    /// some 0.4 of it, with Sommerfeld's enhancement 0.6.
    void inpEdgeAsMeasured()
    {
        const Node inp = materialNode(Model::blank(), *findMaterial(QStringLiteral("Indium phosphide")));
        const Scope& scope = Scope::builtins();
        const double A = evaluateConstant(detail::Problem::materialProperty(inp, QStringLiteral("Afk"), &scope), scope, Dim::of(-1)).value_or(NAN);
        QVERIFY(relative(A, InpEdge) < 1e-12);
        // (Their wavelength in µm, k.)
        const double rows[3][2] = {{0.8266, 0.203}, {0.7749, 0.218}, {0.7293, 0.242}};
        BandFillingMedium m;
        m.Eg = InpGap;
        m.me = InpMe;
        m.mhh = heavyHoles();
        m.mlh = lightHoles();
        m.Ep = InpEp;
        m.n = 3.45;
        m.T = 4;
        for (const auto& row : rows) {
            const double lambda = row[0] * 1e-6, measured = 4 * M_PI * row[1] / lambda;
            const double E = 2 * M_PI * Hbar * C / lambda, x = (E - InpGap) / Q;
            const double fitted = A * std::sqrt(x) / (E / Q);
            QVERIFY2(relative(fitted, measured) < 0.04, qPrintable(QStringLiteral("%1 eV: %2 per cm, measured %3").arg(E / Q).arg(fitted / 100).arg(measured / 100)));
            // The bands' own: all bleached (10^21 per cm³ at 4 K): -Δα is α.
            BandFillingMedium plain = m, excitons = m;
            excitons.epsr = 12.4;
            const double a0 = -bandFilling(plain, E, 1e27, false).dalpha, a1 = -bandFilling(excitons, E, 1e27, false).dalpha;
            qInfo("%.3f eV: measured %.4g per cm, A's %.4g, Ep's %.4g (%.2f), with Sommerfeld's %.4g (%.2f)", E / Q, measured / 100, fitted / 100,
                  a0 / 100, a0 / measured, a1 / 100, a1 / measured);
            QVERIFY(a0 / measured > 0.3 && a0 / measured < 0.5);
            QVERIFY(a1 / measured > 0.5 && a1 / measured < 0.75);
        }
        // What each makes of 10^18 electrons per cm³ at 1550 nm (300 K, n 3.3).
        BandFillingMedium measuredEdge = m, ep = m, epS = m;
        measuredEdge.T = ep.T = epS.T = 300;
        measuredEdge.A = InpEdge;
        ep.n = epS.n = 3.3;
        epS.epsr = 12.4;
        const double E = 2 * M_PI * Hbar * C / 1.55e-6;
        qInfo("band filling of 1e18 electrons per cm3 at 1550 nm: %.3g (the measured edge), %.3g (Ep), %.3g (Ep, Sommerfeld's)",
              bandFilling(measuredEdge, E, 1e24, false).dn, bandFilling(ep, E, 1e24, false).dn, bandFilling(epS, E, 1e24, false).dn);
    }

    /// Band filling in a mode: the material's gap, masses and Ep at the
    /// mode's photon energy; given per carrier; bleaching no more than the
    /// medium absorbs.
    void bandFillingInTheMode()
    {
        const QPointF middle(0, 0);
        // (The library's InP: its measured edge, A.)
        BandFillingMedium m;
        m.Eg = InpGap;
        m.me = InpMe;
        m.mhh = heavyHoles();
        m.mlh = lightHoles();
        m.A = InpEdge;
        for (const double lambda : {1.55e-6, 1.3e-6}) {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("bandfilling": true)")), QStringLiteral("%1[nm]").arg(lambda * 1e9))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double E = 2 * M_PI * Hbar * C / lambda;
            const double expected = bandFilling(m, E, 1e24, false).dn + bandFilling(m, E, 5e23, true).dn;
            const double dn = at(*r.solution, QStringLiteral("emw.dncarriers"), middle);
            QVERIFY2(relative(dn, expected) < 3e-3, qPrintable(QStringLiteral("%1 %2").arg(dn).arg(expected)));
            QCOMPARE(at(*r.solution, QStringLiteral("emw.alphacarriers"), middle), 0.0);
        }
        // Electrons alone.
        {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("bandfilling": true, "bfholes": false)")))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double E = 2 * M_PI * Hbar * C / 1.55e-6;
            QVERIFY(relative(at(*r.solution, QStringLiteral("emw.dncarriers"), middle), bandFilling(m, E, 1e24, false).dn) < 3e-3);
        }
        // InP of its compound alone (no measured edge): Kane's Ep, with
        // Sommerfeld's enhancement of its εr 12.4 - or without, asked.
        for (const bool excitons : {true, false}) {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("bandfilling": true, "bfexciton": %1)").arg(excitons ? "true" : "false")),
                                            QStringLiteral("1550[nm]"),
                                            QStringLiteral(R"("label": "InP alone", "compound": "InP", "bands": "measured", "n": "3.3")"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            BandFillingMedium k = m;
            k.A = 0;
            k.Ep = InpEp;
            k.epsr = excitons ? 12.4 : 0;   // (n 1: the tables' - as 1/n)
            const double E = 2 * M_PI * Hbar * C / 1.55e-6;
            const double expected = (bandFilling(k, E, 1e24, false).dn + bandFilling(k, E, 5e23, true).dn) / 3.3;
            QVERIFY2(relative(at(*r.solution, QStringLiteral("emw.dncarriers"), middle), expected) < 3e-3,
                     qPrintable(QStringLiteral("%1 %2").arg(at(*r.solution, QStringLiteral("emw.dncarriers"), middle)).arg(expected)));
        }
        // Given per carrier.
        {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("bandfilling": true, "bfmodel": "user", "Pbfn": "-2e-21[cm^3]", "Pbfp": "-0.5e-21[cm^3]")")))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QVERIFY(relative(at(*r.solution, QStringLiteral("emw.dncarriers"), middle), -(2e-21 * 1e18 + 0.5e-21 * 5e17)) < 1e-9);
        }
        // Above InP's gap (900 nm): the edge's absorption bleached - no more
        // than the medium has (κ 1e-4, some 14 per cm), all of it (κ 0.2) less.
        const double lambda = 0.9e-6, E = 2 * M_PI * Hbar * C / lambda;
        const double bleach = (bandFilling(m, E, 1e24, false).dalpha + bandFilling(m, E, 5e23, true).dalpha);
        QVERIFY(bleach < -1e5);
        for (const double kappa : {1e-4, 0.2}) {
            const Run r = run(modelOf(guide(onCore(QStringLiteral(R"("bandfilling": true)")), QStringLiteral("900[nm]"),
                                            QStringLiteral(R"("material": "Indium phosphide", "n": "3.3", "kappa": "%1")").arg(kappa))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double absorbs = 4 * M_PI * kappa / lambda;
            const double got = at(*r.solution, QStringLiteral("emw.alphacarriers"), middle);
            QVERIFY2(relative(got, std::max(bleach, -absorbs)) < 3e-3, qPrintable(QStringLiteral("%1 %2 %3").arg(got).arg(bleach).arg(absorbs)));
            // (Held - its κ 1e-4 less than the bleaching: said, its Δn the
            // whole edge's; κ 0.2 more: nothing to say.)
            QCOMPARE(r.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("bleaches more absorption")), kappa < 0.1);
        }
        // A material without a gap: said.
        const Run none = run(modelOf(guide(onCore(QStringLiteral(R"("bandfilling": true)")), QStringLiteral("1550[nm]"),
                                           QStringLiteral(R"("label": "plain", "n": "3.3")"))));
        QVERIFY2(none.error.contains(QLatin1String("Eg")), qPrintable(none.error));
    }

    /// The perturbation's Δneff and Δα against the mode solved again with
    /// the carriers; only where it is asked; what it refuses.
    void perturbationAgainstTheSolve()
    {
        const Model model = modelOf(guide(onCore(QStringLiteral(R"("plasma": true, "plasmamodel": "drude", "fca": true)"))));
        const Run with = run(model), without = run(modelOf(guide(QString())));
        QVERIFY2(with.error.isEmpty() && without.error.isEmpty(), qPrintable(with.error + without.error));
        Node pert = model.make(QStringLiteral("perturbation"));
        pert.props.insert(QStringLiteral("length"), QStringLiteral("2[mm]"));
        const DerivedResult d = evaluateDerived(*with.solution, pert);
        QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
        auto value = [](const DerivedResult& r, const QString& name) {
            for (const DerivedValue& v : r.values)
                if (v.name == name) return v.value;
            return double(NAN);
        };
        const double k0 = 2 * M_PI / 1.55e-6;
        const double exact = global(*with.solution, QStringLiteral("emw.neff")) - global(*without.solution, QStringLiteral("emw.neff"));
        const double dneff = value(d, QStringLiteral("dneff"));
        // To the first order: of the field with them, or without them (Δn
        // Drude's there, the core's alone) - and their mean to the second.
        Node before = pert;
        const double mhh = heavyHoles(), mlh = lightHoles();
        const double mh = (std::pow(mhh, 1.5) + std::pow(mlh, 1.5)) / (std::sqrt(mhh) + std::sqrt(mlh));
        before.props.insert(QStringLiteral("dn"), QString::number(drude(1e24, 5e23, 1.55e-6, 3.3, InpMe, mh), 'g', 17));
        before.props.insert(QStringLiteral("dalpha"), QString());
        before.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("core")}}});
        const double first = value(evaluateDerived(*without.solution, before), QStringLiteral("dneff"));
        QVERIFY2(relative(dneff, exact) < 1.5e-2 && relative(first, exact) < 1.5e-2, qPrintable(QStringLiteral("%1 %2 %3").arg(dneff).arg(first).arg(exact)));
        QVERIFY2(relative(0.5 * (dneff + first), exact) < 1e-3, qPrintable(QStringLiteral("%1 %2 %3").arg(dneff).arg(first).arg(exact)));
        // The guide lossless without them: all its loss theirs - the solve's
        // absorption 2 (n0 + Δn) κ, the first order's 2 n0 κ: the same but
        // for that second-order part.
        const double alpha = 2 * k0 * global(*with.solution, QStringLiteral("emw.neff_im"));
        const double n0 = 3.3, dn = at(*with.solution, QStringLiteral("emw.dncarriers"), QPointF(0, 0));
        QVERIFY2(relative(value(d, QStringLiteral("dalpha")) * (n0 + dn) / n0, alpha) < 1e-5,
                 qPrintable(QStringLiteral("%1 %2").arg(value(d, QStringLiteral("dalpha"))).arg(alpha)));
        QVERIFY(relative(value(d, QStringLiteral("dloss")), 10 / std::log(10.0) * value(d, QStringLiteral("dalpha"))) < 1e-12);
        QVERIFY(relative(value(d, QStringLiteral("dphi")), k0 * dneff * 2e-3) < 1e-12);
        // In the cladding: none.
        pert.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("clad")}}});
        const DerivedResult clad = evaluateDerived(*with.solution, pert);
        QVERIFY2(clad.error.isEmpty() && value(clad, QStringLiteral("dneff")) == 0, qPrintable(clad.error));
        // A Δn of a unit, a Δα without one: said.
        pert.props.insert(QStringLiteral("dn"), QStringLiteral("1[m]"));
        QVERIFY(evaluateDerived(*with.solution, pert).error.contains(QLatin1String("not")));
        pert.props.insert(QStringLiteral("dn"), QStringLiteral("emw.dncarriers"));
        pert.props.insert(QStringLiteral("dalpha"), QStringLiteral("2"));
        QVERIFY(evaluateDerived(*with.solution, pert).error.contains(QLatin1String("not")));
    }

    /// Along a bias sweep: each point's phase and VπL from the first; the
    /// carriers as V (Δneff linear in it, the field as good as the same),
    /// VπL Drude's over the core's Γ.
    void perturbationAlongABiasSweep()
    {
        const Model model = modelOf(guide(QStringLiteral(R"x({"type": "wave", "plasma": true, "plasmamodel": "drude", "Ncarriers": "1e17[1/cm^3]*(1 + V/1[V])", "Pcarriers": "0[1/cm^3]", "selection": {"only": ["core"]}})x"),
                                          QStringLiteral("1550[nm]"), QStringLiteral(R"("material": "Indium phosphide", "n": "3.3")"), QString(),
                                          QStringLiteral(R"({"type": "sweep", "parameters": [["V", "0, 1, 3", "V"]]},)")));
        const Run r = run(model);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        SolutionSet set;
        set.solutions = r.study.solutions;
        QCOMPARE(int(set.solutions.size()), 3);
        Node pert = model.make(QStringLiteral("perturbation"));
        pert.props.insert(QStringLiteral("length"), QStringLiteral("1[mm]"));
        pert.props.insert(QStringLiteral("bias"), QStringLiteral("V"));
        const std::vector<Instance> each = instances(set, true);
        QCOMPARE(int(each.size()), 3);
        std::vector<DerivedResult> values;
        for (const Instance& at : each) values.push_back(evaluateDerived(*select(set, at), pert));
        perturbationAlongSweep(set, each, pert, values);
        auto value = [&](int i, const QString& name) {
            for (const DerivedValue& v : values[std::size_t(i)].values)
                if (v.name == name) return v.value;
            return double(NAN);
        };
        for (int i = 0; i < 3; ++i) QVERIFY2(values[std::size_t(i)].error.isEmpty(), qPrintable(values[std::size_t(i)].error));
        QCOMPARE(value(1, QStringLiteral("V")), 1.0);
        QCOMPARE(value(2, QStringLiteral("V")), 3.0);
        // The first point: the reference, no VπL of its own.
        QVERIFY(std::isnan(value(0, QStringLiteral("VpiL"))) && std::isnan(value(0, QStringLiteral("dphi_sweep"))));
        const double d1 = value(1, QStringLiteral("dneff")) - value(0, QStringLiteral("dneff")),
                     d3 = value(2, QStringLiteral("dneff")) - value(0, QStringLiteral("dneff"));
        QVERIFY(relative(value(1, QStringLiteral("VpiL")), 1.55e-6 * 1 / (2 * std::abs(d1))) < 1e-12);
        QVERIFY(relative(value(2, QStringLiteral("VpiL")), 1.55e-6 * 3 / (2 * std::abs(d3))) < 1e-12);
        QVERIFY(relative(value(2, QStringLiteral("dphi_sweep")), 2 * M_PI / 1.55e-6 * d3 * 1e-3) < 1e-12);
        QVERIFY2(values[2].values.back().text.endsWith(QStringLiteral("V·cm")), qPrintable(values[2].values.back().name + QStringLiteral(" ") + values[2].values.back().text));
        // A bias of no unit: its VπL in cm too (it was said in m, "(bias)·m").
        {
            std::vector<DerivedResult> plain;
            for (const Instance& at : each) plain.push_back(evaluateDerived(*select(set, at), pert));
            for (DerivedResult& d : plain)
                for (DerivedValue& v : d.values)
                    if (v.name == QLatin1String("V")) v.dim = Dim();
            perturbationAlongSweep(set, each, pert, plain);
            QString said;
            for (const DerivedValue& v : plain[2].values)
                if (v.name == QLatin1String("VpiL")) said = v.text;
            QVERIFY2(said == formatNumber(value(2, QStringLiteral("VpiL")) * 100, 4) + QStringLiteral(" cm (times the bias's unit)"), qPrintable(said));
        }
        // Linear in V: one VπL (but for the field's own change, 0.3 % at 3 V);
        // Drude's Δn per volt times the core's Γ (index).
        QVERIFY2(relative(value(2, QStringLiteral("VpiL")), value(1, QStringLiteral("VpiL"))) < 5e-3,
                 qPrintable(QStringLiteral("%1 %2").arg(value(1, QStringLiteral("VpiL"))).arg(value(2, QStringLiteral("VpiL")))));
        Node conf = model.make(QStringLiteral("confinement"));
        conf.props.insert(QStringLiteral("kind"), QStringLiteral("index"));
        conf.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("core")}}});
        const double gamma = evaluateDerived(*select(set, each[0]), conf).values.value(0).value;
        const double mh = 1;   // (no holes)
        const double perVolt = gamma * drude(1e23, 0, 1.55e-6, 3.3, InpMe, mh);
        QVERIFY2(relative(d1, perVolt) < 3e-3, qPrintable(QStringLiteral("%1 %2").arg(d1).arg(perVolt)));
    }

    /// Which modes a mode step keeps and which it chooses: the most in the
    /// MQW alone keeps a hybrid lateral mode over the rib's TE0 (the review's
    /// wrong Γ); a TE fraction of 0.9, an index above, the loss below;
    /// the mode chosen - what plots and derived values take by default.
    void modesKeptAndChosen()
    {
        auto solve = [](const QString& step, const QString& before = {}) {
            Run r = run(modelOf(rib(step, before)));
            if (!r.error.isEmpty()) qWarning("%s", qPrintable(r.error));
            return r;
        };
        auto at = [](const Run& r, int k, const char* name) {
            r.solution->select(k);
            return global(*r.solution, QString::fromLatin1(name));
        };
        // The most in the MQW: a hybrid mode (TE 0.85), not the TE0 - at 1/2
        // it is TE too. (A slow build: the rib solved 8 times, not 15 - the
        // checks of this marked.)
        std::vector<const char*> tracked = {R"(, "modes": 1, "track": {"only": ["mqw"]})"};
        if (!slowBuild()) tracked.push_back(R"(, "modes": 1, "track": {"only": ["mqw"]}, "polarization": "te")");
        for (const char* step : tracked) {
            const Run r = solve(QString::fromLatin1(step));
            QVERIFY(r.error.isEmpty());
            QVERIFY2(at(r, 0, "emw.neff") < 3.2 && at(r, 0, "emw.TEfraction") < 0.9, step);
        }
        // TE at 0.9 or more: the TE0.
        const Run te = solve(QStringLiteral(R"(, "modes": 1, "track": {"only": ["mqw"]}, "polarization": "te", "tefraction": "0.9")"));
        QVERIFY(te.error.isEmpty());
        // (TE 0.9997 here; sanitized, its coarser mesh, 0.998 here and under
        // 0.99 on CI's Linux mesh, its index the same 3.2263 - the hybrid
        // 0.85 and below 3.2.)
        const double te0 = at(te, 0, "emw.neff"), teFraction = at(te, 0, "emw.TEfraction");
        qInfo("the TE0 %.6f, TE %.5f", te0, teFraction);
        QVERIFY2(te0 > 3.22 && teFraction > (slowBuild() ? 0.95 : 0.99), qPrintable(QStringLiteral("%1, TE %2").arg(te0).arg(teFraction)));
        // TM at 0.1 or less: the TM0, a few 1e-4 below it. Above 3.2: the two
        // the rib guides, the most in the MQW of them the TE0. (Slow: neither.)
        if (!slowBuild()) {
            const Run tm = solve(QStringLiteral(R"(, "modes": 1, "polarization": "tm", "tefraction": "0.9")"));
            QVERIFY(tm.error.isEmpty());
            QVERIFY(at(tm, 0, "emw.TEfraction") < 0.1 && std::abs(at(tm, 0, "emw.neff") - te0) < 1e-3 && at(tm, 0, "emw.neff") < te0);
            const Run above = solve(QStringLiteral(R"(, "modes": 1, "track": {"only": ["mqw"]}, "minneff": "3.2")"));
            QVERIFY(above.error.isEmpty());
            QCOMPARE(at(above, 0, "emw.neff"), te0);
        }
        const Run all = solve(QStringLiteral(R"(, "modes": 6, "minneff": "3.2")"));
        QVERIFY(all.error.isEmpty());
        QCOMPARE(all.solution->snapshotCount(), 2);
        QVERIFY(all.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("below the effective index or above the loss")));
        // The mode chosen of four: the first, the most TE, the most TM, the
        // most in the ridge; a plot's (mode 0) and a derived value's at each.
        // (A slow build: the most TM alone, the one not the first.)
        std::vector<std::pair<const char*, int>> chooses = {{"tm", 2}};
        if (!slowBuild()) chooses = {{"first", 1}, {"te", 1}, {"tm", 2}};
        for (const auto& [choose, expected] : chooses) {
            const Run r = solve(QStringLiteral(R"(, "modes": 4, "choose": "%1")").arg(QLatin1String(choose)));
            QVERIFY(r.error.isEmpty());
            // (The fields left as the steps after take them: the one chosen.)
            QCOMPARE(r.solution->mode(), expected);
            // (A choice given - "first" too - its step's: "auto" that mode
            // alone; "first" said once listed every mode.)
            {
                SolutionSet given;
                given.solutions = r.study.solutions;
                QCOMPARE(modesAsked(given, QStringLiteral("auto")), -1);
            }
            SolutionSet set;
            set.solutions = r.study.solutions;
            const std::vector<Instance> shown = instances(set, false);
            QCOMPARE(int(shown.size()), 1);
            QVERIFY2(shown.front().mode == expected, choose);
            // (TE, TM: the first - of the highest index - TE-like, TM-like.)
            int like = -1;
            for (int k = 0; k < r.solution->snapshotCount(); ++k) {
                const double f = at(r, k, "emw.TEfraction");
                qInfo("%s: mode %d, n %.5f, TE %.4f", choose, k + 1, at(r, k, "emw.neff"), f);
                if (like < 0 && (std::strcmp(choose, "tm") == 0 ? f < 0.5 : f > 0.5)) like = k;
                QCOMPARE(r.solution->snapshotChosen(k), k + 1 == expected);
            }
            if (std::strcmp(choose, "first") != 0) QCOMPARE(like + 1, expected);
        }
        if (!slowBuild()) {
            const Run r = solve(QStringLiteral(R"(, "modes": 4, "track": {"only": ["ridge"]}, "choose": "share")"));
            QVERIFY(r.error.isEmpty());
            int most = 0;
            for (int k = 1; k < r.solution->snapshotCount(); ++k)
                if (at(r, k, "emw.Gamma") > at(r, most, "emw.Gamma")) most = k;
            QVERIFY(r.solution->snapshotChosen(most));
        }
        // TE: the TE-like of the highest index, not the most TE of all - a
        // rib 1.5 µm high on a substrate 2 µm deep, meshed finer in its MQW
        // and ridge, above the substrate's index: TE0 at 3.228 (TE 0.9993)
        // and the MQW's slab mode at 3.177 (0.9996) - the slab chosen once.
        if (!slowBuild()) {
            QByteArray json = rib(QStringLiteral(R"(, "modes": 4, "minneff": "3.17", "choose": "te")"));
            json.replace(R"("position": ["-3", "-1.5"], "size": ["6", "3.5"])", R"("position": ["-3", "-2"], "size": ["6", "5.5"])");
            json.replace(R"("position": ["-3", "-1.5"], "size": ["6", "1.5"])", R"("position": ["-3", "-2"], "size": ["6", "2"])");
            json.replace(R"("size": ["2", "1.4"])", R"("size": ["2", "1.5"])");
            json.replace(R"("mesh": {"size": "normal", "custom": true, "hmax": "0.12"})",
                         R"("mesh": {"size": "fine", "features": [{"type": "size", "hmax": "0.05", "level": "domain", "selection": {"only": ["mqw", "ridge"]}}]})");
            const Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            int chosen = -1, mostTE = 0;
            for (int k = 0; k < r.solution->snapshotCount(); ++k) {
                qInfo("the finer rib: mode %d, n %.5f, TE %.4f%s", k + 1, at(r, k, "emw.neff"), at(r, k, "emw.TEfraction"), r.solution->snapshotChosen(k) ? ", chosen" : "");
                if (r.solution->snapshotChosen(k)) chosen = k;
                if (at(r, k, "emw.TEfraction") > at(r, mostTE, "emw.TEfraction")) mostTE = k;
            }
            QVERIFY(chosen >= 0 && at(r, chosen, "emw.neff") > 3.22 && at(r, chosen, "emw.TEfraction") > 0.99);
            QVERIFY2(mostTE != chosen && at(r, mostTE, "emw.neff") < 3.2, "the most TE the MQW's slab mode, not chosen");
        }
        // Tracking along a sweep of the ridge's width: at each point the mode
        // most in the MQW about the step's own index - not about the last
        // one's (followed, it drifted to modes of half the share: at 2 µm
        // 3.103 for 3.168); the last point the ridge 2 µm wide solved alone.
        if (!slowBuild()) {
            QByteArray json = rib(QStringLiteral(R"(, "modes": 1, "track": {"only": ["mqw"]})"),
                                  QStringLiteral(R"({"type": "sweep", "parameters": [["w", "0.8, 1.4, 2", "um"]]},)"));
            json.replace(R"("parameters": [{"name": "V", "expression": "0[V]"}])", R"("parameters": [{"name": "V", "expression": "0[V]"}, {"name": "w", "expression": "2[um]"}])");
            json.replace(R"("position": ["-1", "0.2"], "size": ["2", "1.4"])", R"("position": ["-w/2", "0.2"], "size": ["w", "1.4"])");
            const Run swept = run(modelOf(json));
            const Run alone = solve(QStringLiteral(R"(, "modes": 1, "track": {"only": ["mqw"]})"));
            QVERIFY2(swept.error.isEmpty() && alone.error.isEmpty(), qPrintable(swept.error + alone.error));
            const Solution& last = *swept.study.solutions.back();
            const double n = global(last, QStringLiteral("emw.neff")), n1 = at(alone, 0, "emw.neff");
            qInfo("tracked along the widths: at 2 um %.5f (alone %.5f), its share %.3f", n, n1, global(last, QStringLiteral("emw.Gamma")));
            QVERIFY(std::abs(n - n1) < 1e-6);
            QVERIFY(last.log.join(QLatin1Char(' ')).contains(QLatin1String("not followed")));
            // The reviewer's: from 0.6 µm (its first a TM-like mode at 3.135),
            // a ridge 1.5 µm high meshed finer - followed, the 2 µm point
            // drifted to 3.103 (Γ 0.096); about its own index, 3.168 (Γ 0.2).
            QByteArray fine = json;
            fine.replace(R"("parameters": [["w", "0.8, 1.4, 2", "um"]])", R"("parameters": [["w", "0.6, 0.8, 1.0, 1.4, 2.0", "um"]])");
            fine.replace(R"("position": ["-3", "-1.5"], "size": ["6", "3.5"])", R"("position": ["-3", "-2"], "size": ["6", "5.5"])");
            fine.replace(R"("position": ["-3", "-1.5"], "size": ["6", "1.5"])", R"("position": ["-3", "-2"], "size": ["6", "2"])");
            fine.replace(R"("size": ["w", "1.4"])", R"("size": ["w", "1.5"])");
            fine.replace(R"("mesh": {"size": "normal", "custom": true, "hmax": "0.12"})",
                         R"("mesh": {"size": "fine", "features": [{"type": "size", "hmax": "0.05", "level": "domain", "selection": {"only": ["mqw", "ridge"]}}]})");
            const Run reviewer = run(modelOf(fine));
            QVERIFY2(reviewer.error.isEmpty(), qPrintable(reviewer.error));
            const Solution& two = *reviewer.study.solutions.back();
            qInfo("the reviewer's widths: at 2 um %.5f, its share %.3f", global(two, QStringLiteral("emw.neff")), global(two, QStringLiteral("emw.Gamma")));
            QVERIFY(global(two, QStringLiteral("emw.neff")) > 3.16 && global(two, QStringLiteral("emw.Gamma")) > 0.15);
        }
        // Along a sweep, at each point the one chosen alone.
        {
            const int points = slowBuild() ? 2 : 3;
            const Run r = solve(QStringLiteral(R"(, "modes": 3, "choose": "tm")"),
                                QStringLiteral(R"({"type": "sweep", "parameters": [["V", "%1", "V"]]},)").arg(slowBuild() ? QStringLiteral("0, 2") : QStringLiteral("0, 1, 2")));
            QVERIFY(r.error.isEmpty());
            SolutionSet set;
            set.solutions = r.study.solutions;
            const std::vector<Instance> chosen = instances(set, true, 0, {}, {}, -1);
            QCOMPARE(int(chosen.size()), points);
            for (const Instance& i : chosen) {
                Solution* s = select(set, i);
                QVERIFY(global(*s, QStringLiteral("emw.TEfraction")) < 0.1);
            }
            QCOMPARE(int(instances(set, true).size()), 3 * points);
        }
        // Right after its solve, its last mode asked: that mode's fields and
        // globals - the step left the chosen one's (mode 1's) after the last
        // snapshot, and the selection took them as the last's.
        {
            const Run r = solve(QStringLiteral(R"(, "modes": 2)"));
            QVERIFY(r.error.isEmpty());
            Solution& s = *r.solution;
            s.select(s.snapshotCount() - 1);
            const double first = global(s, QStringLiteral("emw.neff"));
            s.select(0);
            s.select(s.snapshotCount() - 1);
            QCOMPARE(first, global(s, QStringLiteral("emw.neff")));
            QCOMPARE(s.mode(), 2);
        }
        // Guided: a lossy substrate's modes left out by their loss - of the
        // four nearest 3.3 the rib's two below 1 dB/cm (0.78, 0.81), the
        // others 1.7 and 1.9; kept, those two alone of all it finds.
        {
            QByteArray json = rib(QStringLiteral(R"(, "modes": 4)"));
            json.replace(R"("n": "3.17", "selection": {"only": ["substrate"]})", R"("n": "3.17", "kappa": "2e-5", "selection": {"only": ["substrate"]})");
            const Run lossy = run(modelOf(json));
            QVERIFY2(lossy.error.isEmpty(), qPrintable(lossy.error));
            int below = 0;
            for (int k = 0; k < lossy.solution->snapshotCount(); ++k) below += at(lossy, k, "emw.loss") / 100 < 1 ? 1 : 0;
            QCOMPARE(below, 2);
            json.replace(R"("modes": 4)", R"("modes": 4, "maxloss": "1")");
            const Run guided = run(modelOf(json));
            QVERIFY2(guided.error.isEmpty(), qPrintable(guided.error));
            QCOMPARE(guided.solution->snapshotCount(), 2);
            for (int k = 0; k < 2; ++k) QVERIFY(at(guided, k, "emw.loss") / 100 < 1 && at(guided, k, "emw.neff") > 3.2);
        }
        // Refused, and said.
        for (const auto& [step, said] : {std::pair{R"(, "choose": "share")", "track"}, std::pair{R"(, "polarization": "te", "tefraction": "1.5")", "TE fraction"},
                                         std::pair{R"(, "maxloss": "-1")", "loss"}}) {
            const Run r = run(modelOf(rib(QString::fromLatin1(step))));
            QVERIFY2(r.error.contains(QLatin1String(said)), qPrintable(r.error));
        }
        // A plot group's mode: the one chosen unless it says.
        QCOMPARE(modelOf(rib(QString())).make(QStringLiteral("plotgroup")).integer(QStringLiteral("mode")), 0);
    }

    /// Fields on a grid: CSV, NumPy, columns - x itself as a check of where
    /// each point is, a mode's |E| as the solution has it, NaN outside; at the
    /// mesh's nodes; each point of a sweep; what it refuses.
    void fieldOnAGrid()
    {
        const Model model = modelOf(guide(onCore(QStringLiteral(R"("plasma": true, "plasmamodel": "drude")"))));
        const Run r = run(model);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        SolutionSet set;
        set.solutions = r.study.solutions;
        Node fx = model.make(QStringLiteral("fieldexport"));
        fx.props.insert(QStringLiteral("expressions"), QStringLiteral("x; E = emw.normE; emw.dncarriers"));
        fx.props.insert(QStringLiteral("from"), QJsonArray{QStringLiteral("-2"), QStringLiteral("-1.5")});
        fx.props.insert(QStringLiteral("to"), QJsonArray{QStringLiteral("2.5"), QStringLiteral("1.5")});
        fx.props.insert(QStringLiteral("nx"), 10);
        fx.props.insert(QStringLiteral("ny"), 7);
        const ExportText csv = exportText(model, set, fx, QStringLiteral("test"));
        QVERIFY2(csv.error.isEmpty(), qPrintable(csv.error));
        QCOMPARE(csv.suffix, QStringLiteral("csv"));
        const QStringList lines = csv.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 1 + 70);
        QCOMPARE(lines[0], QStringLiteral("x (um),y (um),x (m),E (V/m),emw.dncarriers"));
        const Locator locator(r.solution->mesh());
        int outside = 0;
        for (int n = 0; n < 70; ++n) {
            const QStringList c = lines[n + 1].split(QLatin1Char(','));
            QCOMPARE(c.size(), 5);
            const double x = c[0].toDouble(), y = c[1].toDouble();
            QVERIFY(std::abs(x - (-2 + 4.5 * (n % 10) / 9)) < 1e-9 && std::abs(y - (-1.5 + 3.0 * (n / 10) / 6)) < 1e-9);
            if (x > 2 + 1e-9) {
                // (Beyond the model: none.)
                QVERIFY(c[2] == QLatin1String("nan") && c[3] == QLatin1String("nan"));
                ++outside;
                continue;
            }
            QVERIFY(std::abs(c[2].toDouble() - x * 1e-6) < 1e-15);
            QVERIFY(relative(c[3].toDouble(), *evaluateAt(*r.solution, locator, QStringLiteral("emw.normE"), QPointF(x, y))) < 1e-8);
        }
        QCOMPARE(outside, 7);
        // NumPy's: (expressions, y, x), doubles; its header.
        fx.props.insert(QStringLiteral("format"), QStringLiteral("npy"));
        const ExportText npy = exportText(model, set, fx, QStringLiteral("test"));
        QVERIFY2(npy.error.isEmpty() && npy.text.isEmpty(), qPrintable(npy.error));
        QVERIFY(npy.data.startsWith(QByteArray("\x93NUMPY\x01\x00", 8)));
        const int length = quint8(npy.data[8]) | (quint8(npy.data[9]) << 8);
        QCOMPARE((10 + length) % 64, 0);
        const QByteArray header = npy.data.mid(10, length);
        QVERIFY2(header.startsWith("{'descr': '<f8', 'fortran_order': False, 'shape': (3, 7, 10), }") && header.endsWith('\n'), header.constData());
        QCOMPARE(npy.data.size(), 10 + length + 8 * 3 * 7 * 10);
        auto element = [&](int k, int j, int i) {
            double v;
            std::memcpy(&v, npy.data.constData() + 10 + length + 8 * ((k * 7 + j) * 10 + i), 8);
            return v;
        };
        QVERIFY(std::abs(element(0, 3, 4) - (-2 + 4.5 * 4 / 9) * 1e-6) < 1e-15);
        QVERIFY(std::isnan(element(1, 2, 9)));
        QVERIFY(relative(element(1, 5, 2), lines[1 + 5 * 10 + 2].split(QLatin1Char(','))[3].toDouble()) < 1e-11);
        // Its points' x and y beside it (its _xy.npy): (2, y, x), the
        // geometry's unit - the values' file has none.
        {
            const QByteArray& xy = npy.coordinates;
            QVERIFY(xy.startsWith(QByteArray("\x93NUMPY\x01\x00", 8)));
            const int hl = quint8(xy[8]) | (quint8(xy[9]) << 8);
            QVERIFY2(xy.mid(10, hl).startsWith("{'descr': '<f8', 'fortran_order': False, 'shape': (2, 7, 10), }"), xy.mid(10, hl).constData());
            auto at = [&](int c, int j, int i) {
                double v;
                std::memcpy(&v, xy.constData() + 10 + hl + 8 * ((c * 7 + j) * 10 + i), 8);
                return v;
            };
            const QStringList row = lines[1 + 5 * 10 + 2].split(QLatin1Char(','));
            QVERIFY(std::abs(at(0, 5, 2) - row[0].toDouble()) < 1e-12 && std::abs(at(1, 5, 2) - row[1].toDouble()) < 1e-12);
            QVERIFY(npy.summary.contains(QLatin1String("_xy.npy")));
        }
        // Columns: # before the header, a blank line after each grid row.
        fx.props.insert(QStringLiteral("format"), QStringLiteral("dat"));
        const ExportText dat = exportText(model, set, fx, QStringLiteral("test"));
        QVERIFY2(dat.error.isEmpty(), qPrintable(dat.error));
        QCOMPARE(dat.suffix, QStringLiteral("dat"));
        QVERIFY(dat.text.startsWith(QLatin1String("# a 10 by 7 grid\n# x (um)\ty (um)")));
        QCOMPARE(dat.text.count(QStringLiteral("\n\n")), 6);
        // At the mesh's nodes: each, as it is.
        fx.props.insert(QStringLiteral("where"), QStringLiteral("nodes"));
        fx.props.insert(QStringLiteral("format"), QStringLiteral("csv"));
        const ExportText nodes = exportText(model, set, fx, QStringLiteral("test"));
        QVERIFY2(nodes.error.isEmpty(), qPrintable(nodes.error));
        const QStringList rows = nodes.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(rows.size(), 1 + int(r.solution->mesh().nodes.size()));
        for (int n : {0, 17, int(r.solution->mesh().nodes.size()) - 1}) {
            const QStringList c = rows[n + 1].split(QLatin1Char(','));
            const QPointF p = r.solution->mesh().nodes[std::size_t(n)];
            QVERIFY(std::abs(c[0].toDouble() - p.x()) < 1e-9 && std::abs(c[2].toDouble() - p.x() * 1e-6) < 1e-15);
        }
        // Each point of a sweep: (points, expressions, y, x).
        {
            const Model swept = modelOf(guide(QStringLiteral(R"x({"type": "wave", "plasma": true, "plasmamodel": "drude", "Ncarriers": "1e17[1/cm^3]*(1 + V/1[V])", "Pcarriers": "0[1/cm^3]", "selection": {"only": ["core"]}})x"),
                                              QStringLiteral("1550[nm]"), QStringLiteral(R"("material": "Indium phosphide", "n": "3.3")"), QString(),
                                              QStringLiteral(R"({"type": "sweep", "parameters": [["V", "0, 1, 3", "V"]]},)")));
            const Run sr = run(swept);
            QVERIFY2(sr.error.isEmpty(), qPrintable(sr.error));
            SolutionSet ss;
            ss.solutions = sr.study.solutions;
            Node each = swept.make(QStringLiteral("fieldexport"));
            each.props.insert(QStringLiteral("expressions"), QStringLiteral("emw.dncarriers"));
            each.props.insert(QStringLiteral("solutions"), QStringLiteral("all"));
            each.props.insert(QStringLiteral("format"), QStringLiteral("npy"));
            each.props.insert(QStringLiteral("nx"), 3);
            each.props.insert(QStringLiteral("ny"), 3);
            const ExportText e = exportText(swept, ss, each, QStringLiteral("test"));
            QVERIFY2(e.error.isEmpty(), qPrintable(e.error));
            QVERIFY(e.data.contains("'shape': (3, 1, 3, 3)"));
            // The middle point of each: the core's Δn, as V.
            const int len = quint8(e.data[8]) | (quint8(e.data[9]) << 8);
            for (int p = 0; p < 3; ++p) {
                double v;
                std::memcpy(&v, e.data.constData() + 10 + len + 8 * (p * 9 + 4), 8);
                QVERIFY(relative(v, drude(1e23 * (1 + (p == 2 ? 3 : p)), 0, 1.55e-6, 3.3, InpMe, 1)) < 1e-9);
            }
            each.props.insert(QStringLiteral("format"), QStringLiteral("csv"));
            const ExportText c = exportText(swept, ss, each, QStringLiteral("test"));
            QVERIFY2(c.text.section(QLatin1Char('\n'), 0, 0).count(QStringLiteral("emw.dncarriers")) == 3, qPrintable(c.text.section(QLatin1Char('\n'), 0, 0)));
        }
        // Refused, and said.
        fx.props.insert(QStringLiteral("expressions"), QString());
        QVERIFY(exportText(model, set, fx, QStringLiteral("test")).error.contains(QLatin1String("expression")));
        fx.props.insert(QStringLiteral("expressions"), QStringLiteral("emw.nothing"));
        QVERIFY(!exportText(model, set, fx, QStringLiteral("test")).error.isEmpty());
        fx.props.insert(QStringLiteral("expressions"), QStringLiteral("x"));
        fx.props.insert(QStringLiteral("where"), QStringLiteral("grid"));
        fx.props.insert(QStringLiteral("to"), QJsonArray{QStringLiteral("-3"), QStringLiteral("1")});
        QVERIFY(exportText(model, set, fx, QStringLiteral("test")).error.contains(QLatin1String("corner")));
        // One column, along a line: its corners' x - their middle where they
        // differ (refused, and nx 1 divided by nought: the update check of
        // 21 October, 6).
        fx.props.insert(QStringLiteral("expressions"), QStringLiteral("E = emw.normE"));
        fx.props.insert(QStringLiteral("nx"), 1);
        fx.props.insert(QStringLiteral("ny"), 4);
        for (const char* right : {"0.5", "0.7"}) {
            fx.props.insert(QStringLiteral("from"), QJsonArray{QStringLiteral("0.3"), QStringLiteral("-1.5")});
            fx.props.insert(QStringLiteral("to"), QJsonArray{QLatin1String(right), QStringLiteral("1.5")});
            if (!std::strcmp(right, "0.5")) fx.props.insert(QStringLiteral("from"), QJsonArray{QStringLiteral("0.5"), QStringLiteral("-1.5")});
            const ExportText column = exportText(model, set, fx, QStringLiteral("test"));
            QVERIFY2(column.error.isEmpty(), qPrintable(column.error));
            const QStringList rows = column.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            QCOMPARE(rows.size(), 1 + 4);
            for (int n = 0; n < 4; ++n) {
                const QStringList c = rows[n + 1].split(QLatin1Char(','));
                QVERIFY2(std::abs(c[0].toDouble() - 0.5) < 1e-12 && std::abs(c[1].toDouble() - (-1.5 + n)) < 1e-12, qPrintable(rows[n + 1]));
                QVERIFY(relative(c[2].toDouble(), *evaluateAt(*r.solution, locator, QStringLiteral("emw.normE"), QPointF(0.5, -1.5 + n))) < 1e-8);
            }
        }
        // (Two columns at one x: refused still.)
        fx.props.insert(QStringLiteral("nx"), 2);
        fx.props.insert(QStringLiteral("from"), QJsonArray{QStringLiteral("0.5"), QStringLiteral("-1.5")});
        fx.props.insert(QStringLiteral("to"), QJsonArray{QStringLiteral("0.5"), QStringLiteral("1.5")});
        QVERIFY(exportText(model, set, fx, QStringLiteral("test")).error.contains(QLatin1String("corner")));
    }

    /// An alloy's composition solved for its gap: on InP's lattice by
    /// Vegard's law, its gap hc/λg at 300 K, its masses of that composition;
    /// the 1.3 µm AlGaInAs of the 300 K fit, the review's 0.993 eV of
    /// Vurgaftman's at that composition and 0.954 solved; not reached: said.
    void compositionOfAGap()
    {
        // Vurgaftman's lattice constants (Å, 300 K).
        const double InP = 5.8697, InAs = 6.0583, GaAs = 5.65325, GaP = 5.4505, AlAs = 5.6611;
        const double target = 1.23984198 / 1.3;   // (eV: hc/1.3 µm)
        auto property = [](const QString& json, const char* key, double T = 300) {
            const Node m = modelOf(QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "r", "size": ["1", "1"]}], "materials": [{%1, "selection": {"all": true}}]}]})")
                                       .arg(json)
                                       .toUtf8())
                                .materials()
                                .children.front();
            MaterialScope scope(Scope::builtins(), T);
            const QString text = detail::Problem::materialProperty(m, QString::fromLatin1(key), &scope, T);
            const Expression e = Expression::compile(text, scope);
            return e.isValid() && e.isConstant() ? (e.dim() == Dim::of(2, 1, -2) ? e.constant() / Q : e.constant()) : double(NAN);
        };
        // InGaAsP (In1-x Gax Asy P1-y), the 300 K fit's: Nahory's y within
        // 0.02 (its 7 meV), x as Nahory's lattice match of it.
        const QString q13 = QStringLiteral(R"("label": "Q", "compound": "InGaAsP", "composition": "gap", "lambdag": "1.3[um]", "bands": "measured")");
        const double x = property(q13, "x"), y = property(q13, "y");
        const double a = (1 - x) * (1 - y) * InP + (1 - x) * y * InAs + x * (1 - y) * GaP + x * y * GaAs;
        QVERIFY2(std::abs(a - InP) < 1e-9 * InP, qPrintable(QStringLiteral("%1 %2 %3").arg(x).arg(y).arg(a)));
        QVERIFY2(std::abs(property(q13, "Eg") - target) < 1e-6, qPrintable(QString::number(property(q13, "Eg"))));
        const double nahory = (0.72 - std::sqrt(0.72 * 0.72 - 4 * 0.12 * (1.35 - target))) / (2 * 0.12);
        QVERIFY2(std::abs(y - nahory) < 0.02 && std::abs(x - 0.1894 * y / (0.4184 - 0.013 * y)) < 2e-3,
                 qPrintable(QStringLiteral("x %1 y %2 Nahory's y %3").arg(x).arg(y).arg(nahory)));
        // Its masses, offsets: the compound's at that composition.
        for (const char* key : {"me", "gamma1", "VBO", "Ep", "ac"}) {
            const QString text = compoundProperty(QStringLiteral("InGaAsP"), x, y, QString::fromLatin1(key), BandSet::Measured);
            const double expected = evaluateConstant(text, Scope::builtins(), Expression::compile(text, Scope::builtins()).dim()).value_or(NAN);
            const double got = property(q13, key) * (std::strcmp(key, "me") && std::strcmp(key, "gamma1") ? Q : 1);
            QVERIFY2(std::abs(got - expected) <= 1e-7 * std::abs(expected), key);
        }
        // Its gap at another temperature: the compound's there (Varshni's), not hc/λg.
        QVERIFY(property(q13, "Eg", 77) > target + 0.05);
        // AlGaInAs's 1.3 µm of the fit: Al0.165 Ga0.305 In0.53 As.
        const QString a13 = QStringLiteral(R"("label": "A", "compound": "AlGaInAs", "composition": "gap", "lambdag": "1.3[um]", "bands": "measured")");
        const double ax = property(a13, "x"), ay = property(a13, "y");
        QVERIFY2(std::abs(ax - 0.165) < 0.005 && std::abs(ay - 0.305) < 0.005, qPrintable(QStringLiteral("%1 %2").arg(ax).arg(ay)));
        QVERIFY(std::abs(ax * AlAs + ay * GaAs + (1 - ax - ay) * InAs - InP) < 1e-9 * InP);
        // Vurgaftman's: at that composition 0.993 eV; solved, 0.954 there too.
        const QString given = QStringLiteral(R"("label": "G", "compound": "AlGaInAs", "x": "0.165", "y": "0.305")");
        QVERIFY2(std::abs(property(given, "Eg") - 0.993) < 1e-3, qPrintable(QString::number(property(given, "Eg"))));
        const QString vurgaftman = QStringLiteral(R"("label": "V", "compound": "AlGaInAs", "composition": "gap", "lambdag": "1.3[um]")");
        QVERIFY(std::abs(property(vurgaftman, "Eg") - target) < 1e-6);
        QVERIFY(property(vurgaftman, "x") < ax - 0.005);
        // A ternary, its gap alone: the fit's In0.53Ga0.47As at 0.750 eV.
        const QString t = QStringLiteral(R"("label": "T", "compound": "InGaAs", "composition": "gap", "lambdag": "%1[um]", "bands": "measured")")
                              .arg(1.23984198 / 0.750, 0, 'g', 12);
        QVERIFY2(std::abs(property(t, "x") - 0.53) < 0.002, qPrintable(QString::number(property(t, "x"))));
        // Not reached - above InP's gap on its lattice: said, with the gaps there.
        QString why;
        QVERIFY(!compositionForGap(QStringLiteral("InGaAsP"), 1.4587, QStringLiteral("InP"), BandSet::Measured, &why));
        QVERIFY2(why.contains(QLatin1String("not reached")) && why.contains(QLatin1String("1.344")), qPrintable(why));
        QVERIFY(!compositionForGap(QStringLiteral("GaAs"), 1.4, QStringLiteral("InP"), BandSet::Measured, &why));
        // Its gap as an energy Eg, not a wavelength: the same composition
        // (refused once - λg alone taken).
        {
            Model blank = Model::blank();
            Node byWavelength = blank.make(QStringLiteral("material")), byEnergy = blank.make(QStringLiteral("material"));
            for (Node* n : {&byWavelength, &byEnergy}) {
                n->props.insert(QStringLiteral("compound"), QStringLiteral("InGaAsP"));
                n->props.insert(QStringLiteral("composition"), QStringLiteral("gap"));
                n->props.insert(QStringLiteral("bands"), QStringLiteral("measured"));
            }
            byWavelength.props.insert(QStringLiteral("lambdag"), QStringLiteral("1.3[um]"));
            byEnergy.props.insert(QStringLiteral("Eg"), QStringLiteral("%1[eV]").arg(6.62607015e-34 * 299792458.0 / 1.3e-6 / 1.602176634e-19, 0, 'g', 17));
            QString why;
            const auto a = detail::Problem::solvedComposition(byWavelength, nullptr, &why);
            const auto b = detail::Problem::solvedComposition(byEnergy, nullptr, &why);
            QVERIFY2(a && b, qPrintable(why));
            QVERIFY(std::abs(a->first - b->first) < 1e-9 && std::abs(a->second - b->second) < 1e-9);
            byEnergy.props.remove(QStringLiteral("Eg"));
            QVERIFY(!detail::Problem::solvedComposition(byEnergy, nullptr, &why) && why.contains(QLatin1String("Eg")));
        }
        // In a model: the composition in its log, its gap in band filling;
        // one not reached, a problem.
        QByteArray json = guide(onCore(QStringLiteral(R"("bandfilling": true)")), QStringLiteral("1550[nm]"),
                                QStringLiteral(R"("label": "Q", "compound": "InGaAsP", "composition": "gap", "lambdag": "1.3[um]", "bands": "measured", "n": "3.4")"));
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY2(r.solution->log.join(QLatin1Char('\n')).contains(QStringLiteral("Q: InGaAsP of its gap - x = %1, y = %2").arg(formatNumber(x, 5), formatNumber(y, 5))),
                 qPrintable(r.solution->log.join(QLatin1Char('\n'))));
        BandFillingMedium m;
        m.Eg = target * Q;
        m.me = property(q13, "me");
        const double g1 = property(q13, "gamma1"), g2 = property(q13, "gamma2");
        m.mhh = 1 / (g1 - 2 * g2);
        m.mlh = 1 / (g1 + 2 * g2);
        m.Ep = property(q13, "Ep") * Q;
        m.epsr = property(q13, "epsilonr");   // (Sommerfeld's, of the composition's εr)
        QVERIFY(m.epsr > 10 && m.epsr < 15);
        const double E = 2 * M_PI * Hbar * C / 1.55e-6;
        const double expected = (bandFilling(m, E, 1e24, false).dn + bandFilling(m, E, 5e23, true).dn) / 3.4;
        QVERIFY2(relative(at(*r.solution, QStringLiteral("emw.dncarriers"), QPointF(0, 0)), expected) < 3e-3,
                 qPrintable(QStringLiteral("%1 %2").arg(at(*r.solution, QStringLiteral("emw.dncarriers"), QPointF(0, 0))).arg(expected)));
        json.replace("1.3[um]", "0.85[um]");
        const Run none = run(modelOf(json));
        QVERIFY2(none.error.contains(QLatin1String("not reached")), qPrintable(none.error));
    }

    /// A quantum wells' absorption and index in a mode, of the field at each
    /// point: at a table's field, as the wells' own spectrum there (α, and
    /// Δn from F0 by Kramers-Kronig); between, between; the mode's loss and
    /// index moved by it; what it refuses.
    void wellsAbsorptionOfTheField()
    {
        const QString wells = QStringLiteral(R"({"type": "wave", "qwabsorption": true, "Fqw": "Fw", "Fmax": "50[kV/cm]", "Fpoints": 3, "selection": {"only": ["well"]}})");
        // The wells' own spectrum at 0 and 50 kV/cm (1.46 eV, below the edge).
        auto spectrum = [](const char* F) {
            const Run r = run(modelOf(stackGuide(QString(), QString::fromLatin1(F), true)));
            if (!r.error.isEmpty()) qWarning("%s", qPrintable(r.error));
            double v[2] = {NAN, NAN};
            for (int k = 0; r.solution && k < r.solution->snapshotCount(); ++k) {
                r.solution->select(k);
                if (std::abs(global(*r.solution, QStringLiteral("qw.Ephoton")) / Q - 1.46) < 1e-9) {
                    v[0] = global(*r.solution, QStringLiteral("qw.alphaTE"));
                    v[1] = global(*r.solution, QStringLiteral("qw.dnTE"));
                }
            }
            return std::pair{v[0], v[1]};
        };
        const auto [a0, n0] = spectrum("0[V/m]");
        const auto [a50, n50] = spectrum("5e6[V/m]");
        QVERIFY2(std::isfinite(a0) && std::isfinite(a50) && a50 > a0, qPrintable(QStringLiteral("%1 %2").arg(a0).arg(a50)));
        // In the mode, the field 50 kV/cm: a node of the table.
        auto mode = [&](const char* F, const QString& feature) {
            QByteArray json = stackGuide(feature);
            json.replace(R"("name": "Fw", "expression": "0[V/m]")", QByteArray(R"("name": "Fw", "expression": ")") + F + "\"");
            return run(modelOf(json));
        };
        const QPointF centre(0, 0);
        const Run on = mode("5e6[V/m]", wells), off = mode("0[V/m]", wells);
        QVERIFY2(on.error.isEmpty() && off.error.isEmpty(), qPrintable(on.error + off.error));
        const double alpha = at(*on.solution, QStringLiteral("emw.alphaqwTE"), centre), dn = at(*on.solution, QStringLiteral("emw.dnqwTE"), centre);
        qInfo("the well at 1.46 eV: alpha TE %.6g per cm at 0, %.6g at 50 kV/cm; dn TE %.6g", a0 / 100, a50 / 100, n50 - n0);
        QVERIFY2(relative(alpha, a50) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(alpha).arg(a50)));
        QVERIFY2(std::abs(dn - (n50 - n0)) < 1e-6 * std::abs(n50 - n0) + 1e-12, qPrintable(QStringLiteral("%1 %2").arg(dn).arg(n50 - n0)));
        QVERIFY(relative(at(*off.solution, QStringLiteral("emw.alphaqwTE"), centre), a0) < 1e-6);
        QCOMPARE(at(*off.solution, QStringLiteral("emw.dnqwTE"), centre), 0.0);
        // The photon within rounding of one of the spectrum's energies (its
        // frequency to ten digits; 1.46 eV one of them): the same Δn - not
        // the Kramers-Kronig integral's beside an interval of no width.
        {
            const double f = 1.46 * Q / 6.62607015e-34;
            QByteArray json = stackGuide(wells);
            json.replace(R"("name": "Fw", "expression": "0[V/m]")", R"("name": "Fw", "expression": "5e6[V/m]")");
            const QByteArray full = QString::number(f, 'g', 17).toUtf8(), ten = QString::number(f, 'g', 10).toUtf8();
            QVERIFY(json.contains(full) && full != ten && std::abs(ten.toDouble() / f - 1) > 1e-12);
            json.replace(full, ten);
            const Run rounded = run(modelOf(json));
            QVERIFY2(rounded.error.isEmpty(), qPrintable(rounded.error));
            const double dn10 = at(*rounded.solution, QStringLiteral("emw.dnqwTE"), centre);
            qInfo("dn TE with the frequency to ten digits %.6g (%.6g)", dn10, n50 - n0);
            QVERIFY2(std::abs(dn10 - (n50 - n0)) < 1e-3 * std::abs(n50 - n0), qPrintable(QStringLiteral("%1 %2").arg(dn10).arg(n50 - n0)));
        }
        // Not in the barriers or the core: none.
        QCOMPARE(at(*on.solution, QStringLiteral("emw.alphaqwTE"), QPointF(0, 100)), 0.0);
        // The mode loses more and is slower with the field (the edge moved toward it).
        const double k0 = 1.46 * Q / (Hbar * C);
        const double loss = 2 * k0 * global(*on.solution, QStringLiteral("emw.neff_im")), lossOff = 2 * k0 * global(*off.solution, QStringLiteral("emw.neff_im"));
        QVERIFY2(loss > lossOff && lossOff > 0, qPrintable(QStringLiteral("%1 %2").arg(loss).arg(lossOff)));
        if (!slowBuild()) {
            // (Without the wells' absorption: the well's bulk GaAs's - its
            // compound's κ at 1.46 eV, above its gap -, in its place with it:
            // the bulk's no more than the wells'; the well's κ given nought,
            // none at all.)
            const Run bulk = mode("0[V/m]", QString());
            const double lossBulk = 2 * k0 * global(*bulk.solution, QStringLiteral("emw.neff_im"));
            QVERIFY2(bulk.error.isEmpty() && lossBulk > 0 && std::abs(lossBulk - lossOff) > 1e-3 * lossOff,
                     qPrintable(QStringLiteral("%1 %2 %3").arg(bulk.error).arg(lossBulk).arg(lossOff)));
            QByteArray clear = stackGuide(QString());
            clear.replace(R"("compound": "GaAs", "n": "3.6",)", R"("compound": "GaAs", "n": "3.6", "kappa": "0",)");
            const Run none = run(modelOf(clear));
            QVERIFY2(none.error.isEmpty() && global(*none.solution, QStringLiteral("emw.neff_im")) < 1e-12, qPrintable(none.error));
            // The wells' with the well's κ nought: as with its bulk κ there
            // (not added to it).
            QByteArray withWells = stackGuide(wells);
            withWells.replace(R"("compound": "GaAs", "n": "3.6",)", R"("compound": "GaAs", "n": "3.6", "kappa": "0",)");
            const Run offClear = run(modelOf(withWells));
            QVERIFY2(offClear.error.isEmpty() && relative(2 * k0 * global(*offClear.solution, QStringLiteral("emw.neff_im")), lossOff) < 1e-9,
                     qPrintable(offClear.error));
        }
        // Its share: Γ of the well times α (the first order), the solve's.
        Node pert = modelOf(stackGuide(wells)).make(QStringLiteral("perturbation"));
        pert.props.insert(QStringLiteral("dn"), QStringLiteral("emw.dnqwTE"));
        pert.props.insert(QStringLiteral("dalpha"), QStringLiteral("emw.alphaqwTE"));
        const DerivedResult d = evaluateDerived(*on.solution, pert);
        double dalpha = NAN;
        for (const DerivedValue& v : d.values)
            if (v.name == QLatin1String("dalpha")) dalpha = v.value;
        qInfo("the mode: loss %.6g per m at 50 kV/cm, %.6g at 0; the perturbation's %.6g", loss, lossOff, dalpha);
        QVERIFY2(relative(dalpha, loss) < 2e-3, qPrintable(QStringLiteral("%1 %2").arg(dalpha).arg(loss)));
        // Between the table's fields (0, 50): linear between; -50 as +50 (a
        // square well); beyond the table, held.
        const Run between = mode("2.5e6[V/m]", wells);
        QVERIFY2(between.error.isEmpty(), qPrintable(between.error));
        const double a25 = at(*between.solution, QStringLiteral("emw.alphaqwTE"), centre);
        QVERIFY2(relative(a25, 0.5 * (a0 + a50)) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(a25).arg(0.5 * (a0 + a50))));
        // (A slow build: these two left out.)
        if (!slowBuild()) {
            const Run back = mode("-5e6[V/m]", wells), beyond = mode("8e6[V/m]", wells);
            QVERIFY2(back.error.isEmpty() && beyond.error.isEmpty(), qPrintable(back.error + beyond.error));
            QVERIFY(relative(at(*beyond.solution, QStringLiteral("emw.alphaqwTE"), centre), a50) < 1e-6);
            QVERIFY2(relative(at(*back.solution, QStringLiteral("emw.alphaqwTE"), centre), a50) < 1e-3,
                     qPrintable(QString::number(at(*back.solution, QStringLiteral("emw.alphaqwTE"), centre))));
        }
        // The log says the table.
        QVERIFY(on.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("the wells qw at 3 fields")));
        // Refused, and said: no wells of that tag, a photon outside the window.
        QString other = wells;
        other.replace(QStringLiteral("\"Fw\""), QStringLiteral("\"Fw\", \"qwtag\": \"q2\""));
        QVERIFY(run(modelOf(stackGuide(other))).error.contains(QLatin1String("q2")));
        if (!slowBuild()) {
            const Run outside = run(modelOf(stackGuide(wells, QStringLiteral("0[V/m]"), false, QStringLiteral("linspace(1.50[eV], 2.0[eV], 11)"))));
            QVERIFY2(outside.error.contains(QLatin1String("window")), qPrintable(outside.error));
        }
    }

    /// The quadratic electro-optic effect: Δn = ½ n³ R (|E|² - E0²) along each
    /// axis - TE's and TM's R - the mode moved by its share of it.
    void kerrAlongEachAxis()
    {
        auto solve = [](const QString& kerr) {
            return run(modelOf(guide(kerr.isEmpty() ? QString()
                                                    : QStringLiteral(R"({"type": "wave", "kerr": true, %1, "Ex": "0[V/m]", "Ey": "1e7[V/m]", "selection": {"only": ["core"]}})").arg(kerr),
                                     QStringLiteral("1550[nm]"), QStringLiteral(R"("label": "Q", "n": "3.3")"))));
        };
        const Run none = solve(QString());
        const Run te = solve(QStringLiteral(R"("Rkerr": "2e-19[m^2/V^2]; 1e-19[m^2/V^2]; 2e-19[m^2/V^2]")"));
        const Run shifted = solve(QStringLiteral(R"("Rkerr": "2e-19[m^2/V^2]; 1e-19[m^2/V^2]; 2e-19[m^2/V^2]", "E0kerr": "1e7[V/m]")"));
        QVERIFY2(none.error.isEmpty() && te.error.isEmpty() && shifted.error.isEmpty(), qPrintable(none.error + te.error + shifted.error));
        // The guide's TE0 (E along x): ½ 3.3³ 2e-19 1e14 = 3.59e-4 there, by its Γ.
        Node conf = modelOf(guide(QString())).make(QStringLiteral("confinement"));
        conf.props.insert(QStringLiteral("kind"), QStringLiteral("index"));
        conf.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("core")}}});
        const double gamma = evaluateDerived(*none.solution, conf).values.value(0).value;
        const double dn = 0.5 * 3.3 * 3.3 * 3.3 * 2e-19 * 1e14;
        const double shift = global(*te.solution, QStringLiteral("emw.neff")) - global(*none.solution, QStringLiteral("emw.neff"));
        QVERIFY2(global(*te.solution, QStringLiteral("emw.TEfraction")) > 0.99, "TE");
        QVERIFY2(relative(shift, gamma * dn) < 5e-3, qPrintable(QStringLiteral("%1 %2").arg(shift).arg(gamma * dn)));
        // At the field the index is given at: none.
        QVERIFY(std::abs(global(*shifted.solution, QStringLiteral("emw.neff")) - global(*none.solution, QStringLiteral("emw.neff"))) < 1e-10);
        // A permittivity, not an index: said; R of a wrong unit: said.
        QByteArray eps = guide(QStringLiteral(R"({"type": "wave", "kerr": true, "constitutive": "epsilonr", "selection": {"only": ["core"]}})"),
                               QStringLiteral("1550[nm]"), QStringLiteral(R"("label": "Q", "epsilonr": "10.89")"));
        QVERIFY(run(modelOf(eps)).error.contains(QLatin1String("refractive index")));
        const Run unit = solve(QStringLiteral(R"("Rkerr": "1e-19[m/V]")"));
        QVERIFY2(unit.error.contains(QStringLiteral("m²/V²")), qPrintable(unit.error));
        QVERIFY(solve(QStringLiteral(R"("Rkerr": "1e-19; 2e-19")")).error.contains(QLatin1String("three")));
    }

    /// A stack of thin layers as a Layered Region: a grid of stretched
    /// triangles - so many across each layer, the lateral size along them -
    /// not refined; far fewer triangles than isotropic ones, the same mode;
    /// a junction across two layers' current as on an isotropic mesh.
    void layeredStacks()
    {
        // Forty layers: 40 columns of 50 nm, two rows in each - 6400 in the
        // stack; the model 12 000 or so, against 88 000 isotropic.
        {
            // (A slow build: the isotropic mesh, 88 000 triangles, left out.)
            const MeshBuild lay = meshOf(stack(40, layeredMesh(40, 50, 2))), iso = slowBuild() ? lay : meshOf(stack(40, QString()));
            QVERIFY2(iso.mesh && lay.mesh && lay.warnings.isEmpty(), qPrintable(iso.error + lay.error + lay.warnings.join(QLatin1Char(' '))));
            const Topology& topo = *lay.mesh->topology;
            int inStack = 0;
            for (std::size_t t = 0; t < lay.mesh->triangles.size(); ++t) {
                const int d = lay.mesh->domain[t];
                const QString owner = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
                if (!owner.startsWith(QLatin1Char('L'))) continue;
                ++inStack;
                // Each a half of a 50 nm by half-layer cell, on the grid's lines.
                const int k = owner.mid(1).toInt();
                const double t0 = (k / 2) * 14.0 + (k % 2 ? 6 : 0), h = (k % 2 ? 8 : 6) / 2.0;
                const auto& v = lay.mesh->triangles[t];
                const QPointF a = lay.mesh->nodes[std::size_t(v[0])], b = lay.mesh->nodes[std::size_t(v[1])], c = lay.mesh->nodes[std::size_t(v[2])];
                QVERIFY(std::abs(std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2 - 50 * h / 2) < 1e-6 * 50 * h);
                // Each cell split the one way: its diagonal lower left to upper right.
                for (const auto& [p0, p1] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
                    const QPointF e = p1 - p0;
                    if (std::abs(e.x()) > 1e-6 && std::abs(e.y()) > 1e-6) QVERIFY2(e.x() * e.y() > 0, qPrintable(QStringLiteral("L%1").arg(k)));
                }
                for (const QPointF& p : {a, b, c}) {
                    const double row = (p.y() - t0) / h, column = (p.x() + 1000) / 50;
                    QVERIFY2(std::abs(row - std::round(row)) < 1e-6 && std::abs(column - std::round(column)) < 1e-6,
                             qPrintable(QStringLiteral("L%1 (%2, %3)").arg(k).arg(p.x()).arg(p.y())));
                }
            }
            qInfo("forty layers: %zu triangles isotropic, %zu layered (%d in the stack)", iso.mesh->triangles.size(), lay.mesh->triangles.size(), inStack);
            QCOMPARE(inStack, 40 * 40 * 2 * 2);
            QVERIFY2(slowBuild() || iso.mesh->triangles.size() > 5 * lay.mesh->triangles.size(),
                     qPrintable(QStringLiteral("%1 %2").arg(iso.mesh->triangles.size()).arg(lay.mesh->triangles.size())));
        }
        // A layer split (an aperture's edge at 2 um in a 5 um stack) where
        // the lateral size divides the widths apart (2/0.06, 3/0.06, 5/0.06):
        // the stack's one set of columns - 34 to the edge, 50 after it - in
        // every layer and on every boundary, the base's two layers from the
        // split too; the cells whole, no sliver. (Each layer's width
        // divided apart: 84 columns beside 34 + 50.)
        {
            const MeshBuild split = meshOf(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um", "geometry": [
                {"type": "rectangle", "name": "base", "position": ["0", "-0.1"], "size": ["5", "0.1"]},
                {"type": "rectangle", "name": "below", "size": ["5", "0.1"]},
                {"type": "rectangle", "name": "aperture", "position": ["0", "0.1"], "size": ["2", "0.03"]},
                {"type": "rectangle", "name": "oxide", "position": ["2", "0.1"], "size": ["3", "0.03"]},
                {"type": "rectangle", "name": "above", "position": ["0", "0.13"], "size": ["5", "0.1"]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.5",
                         "features": [{"type": "layered", "hlateral": "0.06", "count": 2, "selection": {"all": true}}]}}]})");
            QVERIFY2(split.mesh, qPrintable(split.error));
            auto column = [](double x) {
                const double a = x * 34 / 2, b = (x - 2) * 50 / 3;
                return (x <= 2 + 1e-9 && std::abs(a - std::round(a)) < 1e-6) || (x >= 2 - 1e-9 && std::abs(b - std::round(b)) < 1e-6);
            };
            double least = std::numeric_limits<double>::infinity();
            for (const auto& v : split.mesh->triangles) {
                const QPointF a = split.mesh->nodes[std::size_t(v[0])], b = split.mesh->nodes[std::size_t(v[1])], c = split.mesh->nodes[std::size_t(v[2])];
                least = std::min(least, std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2);
                for (const QPointF& p : {a, b, c}) QVERIFY2(column(p.x()), qPrintable(QStringLiteral("(%1, %2)").arg(p.x(), 0, 'g', 12).arg(p.y())));
            }
            // (The smallest cell's half: 2/34 by 0.015, halved.)
            QVERIFY2(least > 0.99 * (2.0 / 34) * 0.015 / 2, qPrintable(QString::number(least)));
            QCOMPARE(int(split.mesh->triangles.size()), 4 * 84 * 2 * 2);   // (four layers, 84 columns, two rows)
        }
        // The TE0 of four layers: the same on the layered mesh (two rows, 50
        // nm; four, 25) as on the isotropic one, to 2e-6. (A slow build: the
        // two layered ones alike, the isotropic one left out.)
        {
            const Run coarse = run(modelOf(stack(4, layeredMesh(4, 50, 2)))), fine = run(modelOf(stack(4, layeredMesh(4, 25, 4))));
            const Run iso = slowBuild() ? fine : run(modelOf(stack(4, QString())));
            QVERIFY2(iso.error.isEmpty() && coarse.error.isEmpty() && fine.error.isEmpty(), qPrintable(iso.error + coarse.error + fine.error));
            const double n0 = global(*iso.solution, QStringLiteral("emw.neff"));
            qInfo("four layers: neff %.9f isotropic (%zu triangles), %.9f layered 50 nm x 2 (%zu), %.9f 25 nm x 4 (%zu)", n0,
                  iso.solution->mesh().triangles.size(), global(*coarse.solution, QStringLiteral("emw.neff")), coarse.solution->mesh().triangles.size(),
                  global(*fine.solution, QStringLiteral("emw.neff")), fine.solution->mesh().triangles.size());
            // (A slow build's linear elements: 6e-6 apart.)
            for (const Run* r : {&coarse, &fine})
                QVERIFY2(std::abs(global(*r->solution, QStringLiteral("emw.neff")) - n0) < (slowBuild() ? 2e-5 : 2e-6),
                         qPrintable(QStringLiteral("%1 %2").arg(global(*r->solution, QStringLiteral("emw.neff")), 0, 'g', 10).arg(n0, 0, 'g', 10)));
            if (!slowBuild()) QVERIFY(fine.solution->mesh().triangles.size() * 2 < iso.solution->mesh().triangles.size());
        }
        // A pn junction across two layers 0.2 µm thick, 2 µm wide, its
        // contacts below and above: its current at 0.5 V on a layered mesh
        // (20 rows, 0.1 µm) as on an isotropic one (0.02 µm).
        {
            auto junction = [](const QString& mesh) {
                return run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
                  "components": [{"unit": "um",
                    "geometry": [{"type": "rectangle", "name": "p", "size": ["2", "0.2"]},
                                 {"type": "rectangle", "name": "n", "position": ["0", "0.2"], "size": ["2", "0.2"]}],
                    "materials": [{"material": "Silicon", "selection": {"all": true}}],
                    "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                      {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["p"]}},
                      {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"only": ["n"]}},
                      {"type": "ohmic", "name": "a", "V0": "0.5[V]", "selection": {"of": ["p"], "side": "bottom"}},
                      {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "top"}}]}],
                    "mesh": {"size": "normal", "custom": true, "hmax": "%1", "features": [%2]}}],
                  "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                                       .arg(mesh.isEmpty() ? QStringLiteral("0.02") : QStringLiteral("0.5"), mesh)
                                       .toUtf8()));
            };
            const Run iso = junction(QString());
            // (0.1 µm along, 10 nm across: the middle rows' circumcentres
            // encroach on no side - the grid kept out of the angles'
            // refinement, not by its sides alone.)
            const Run lay = junction(QStringLiteral(R"({"type": "layered", "hlateral": "0.1", "count": 20, "selection": {"only": ["p", "n"]}})"));
            QVERIFY2(iso.error.isEmpty() && lay.error.isEmpty(), qPrintable(iso.error + lay.error));
            const double Iiso = global(*iso.solution, QStringLiteral("semi.I_a")), Ilay = global(*lay.solution, QStringLiteral("semi.I_a"));
            qInfo("junction: %.6g A isotropic (%zu triangles), %.6g A layered (%zu)", Iiso, iso.solution->mesh().triangles.size(), Ilay,
                  lay.solution->mesh().triangles.size());
            QVERIFY2(Iiso > 0 && relative(Ilay, Iiso) < 5e-3, qPrintable(QStringLiteral("%1 %2").arg(Ilay).arg(Iiso)));
            QVERIFY2(lay.solution->mesh().triangles.size() == std::size_t(20 * 20 * 2 * 2),
                     qPrintable(QString::number(lay.solution->mesh().triangles.size())));
        }
        // Not a rectangle: said, and meshed as any other.
        {
            QByteArray json = stack(2, QStringLiteral(R"({"type": "layered", "selection": {"only": ["ridge", "L0"]}})"));
            json.replace(R"({"type": "rectangle", "name": "ridge", "position": ["-1000", "14"], "size": ["2000", "1000"]})",
                         R"({"type": "circle", "name": "ridge", "position": ["0", "514"], "r": "500"})");
            const MeshBuild m = meshOf(json);
            QVERIFY2(m.mesh && m.warnings.join(QLatin1Char(' ')).contains(QLatin1String("not a rectangle")), qPrintable(m.error + m.warnings.join(QLatin1Char(' '))));
        }
    }

    /// A sweep of two parameters, the outer stepping (the update check of 21
    /// October, 6: the background doping outer, the bias inner - the next
    /// doping's equilibrium walked from -10 V found none in 200 steps, the
    /// sweep stopped): its first point starts from that inner value's at the
    /// outer value before, all four solved, the third as solved alone; a
    /// point that finds no solution from the one before (a sweep of
    /// specified pairs: -10 V then 0 V) solved from the start, said.
    void aSweepsOuterStepStartsWell()
    {
        auto pin = [](const char* sweep) {
            return modelOf(QStringLiteral(R"json({"format": "qucs-s multiphysics 1",
              "parameters": [{"name": "Va", "expression": "0[V]"}, {"name": "Nb", "expression": "1e15[1/cm^3]"}],
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "name": "n", "size": ["10", "100"]},
                             {"type": "rectangle", "name": "i", "position": ["0", "100"], "size": ["10", "9.8"]},
                             {"type": "rectangle", "name": "p", "position": ["0", "109.8"], "size": ["10", "100"]}],
                "materials": [{"material": "Aluminium gallium arsenide", "x": "0.3", "Eg": "1.798[eV]", "chi": "3.855[eV]", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "space": "1d", "axis": "y", "at": "5", "h1d": "1[nm]", "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e18[1/cm^3]", "selection": {"only": ["n"]}},
                  {"type": "doping", "dopant": "acceptor", "N0": "1e18[1/cm^3]", "selection": {"only": ["p"]}},
                  {"type": "doping", "dopant": "acceptor", "N0": "Nb", "selection": {"only": ["i"]}},
                  {"type": "srh", "selection": {"all": true}},
                  {"type": "ohmic", "name": "cathode", "V0": "0[V]", "selection": {"of": ["n"], "side": "bottom"}},
                  {"type": "ohmic", "name": "anode", "V0": "Va", "selection": {"of": ["p"], "side": "top"}}]}],
                "mesh": {"size": "normal"}}],
              "studies": [{"type": "study", "steps": [%1 {"type": "stationary"}]}]})json")
                               .arg(QLatin1String(sweep))
                               .toUtf8());
        };
        const Run both = run(pin(R"({"type": "sweep", "parameters": [["Nb", "1e14, 1e15", "1/cm^3"], ["Va", "0, -10", "V"]]},)"));
        QVERIFY2(both.error.isEmpty(), qPrintable(both.error));
        QCOMPARE(both.study.solutions.size(), std::size_t(4));
        const QString said = both.study.warnings.join(QLatin1Char('\n'));
        QVERIFY2(!said.contains(QLatin1String("no solution from the point before")), qPrintable(said));
        const Run alone = run(pin(""));
        QVERIFY2(alone.error.isEmpty(), qPrintable(alone.error));
        for (const double y : {50.0, 104.9, 160.0})
            QVERIFY2(std::abs(at(*both.study.solutions[2], QStringLiteral("semi.V"), QPointF(5, y)) - at(*alone.solution, QStringLiteral("semi.V"), QPointF(5, y))) < 1e-6,
                     qPrintable(QString::number(y)));
        // (From -10 V to the next doping's 0 V: none from there, solved from
        // the start.)
        const Run pairs = run(pin(R"({"type": "sweep", "combination": "specified", "parameters": [["Nb", "1e14, 1e15", "1/cm^3"], ["Va", "-10, 0", "V"]]},)"));
        QVERIFY2(pairs.error.isEmpty(), qPrintable(pairs.error));
        QCOMPARE(pairs.study.solutions.size(), std::size_t(2));
        const QString again = pairs.study.warnings.join(QLatin1Char('\n'));
        QVERIFY2(again.contains(QLatin1String("Nb = 1e+15 1/cm^3, Va = 0 V: no solution from the point before (")) && again.contains(QLatin1String("): solved from the start")),
                 qPrintable(again));
        QVERIFY(std::abs(at(*pairs.study.solutions[1], QStringLiteral("semi.V"), QPointF(5, 104.9)) - at(*alone.solution, QStringLiteral("semi.V"), QPointF(5, 104.9))) < 1e-6);
    }

    /// A semiconductor solved in 1D: along its line, on a strip of its own,
    /// each node taking the line's values at its coordinate - the 2D
    /// solution of a layered stack (uniform across it, away from its sides)
    /// at its nodes; a coarse mesh
    /// as interpolated; under a mesa, its currents the mesa's through the
    /// stack (anode's and cathode's alike); a bias sweep; what it refuses.
    void semiconductorIn1D()
    {
        const QString layers = QStringLiteral(R"({"type": "layered", "hlateral": "0.25", "count": 40, "selection": {"only": ["p", "n"]}})");
        const QString line = QStringLiteral(R"(, "h1d": "5[nm]")");
        const Run plane = run(modelOf(junction1d(QStringLiteral("2d"), layers)));
        const Run one = run(modelOf(junction1d(QStringLiteral("1d"), layers, false, QStringLiteral(R"({"type": "stationary"})"), line)));
        QVERIFY2(plane.error.isEmpty() && one.error.isEmpty(), qPrintable(plane.error + one.error));
        const double I2 = global(*plane.solution, QStringLiteral("semi.I_a")), I1 = global(*one.solution, QStringLiteral("semi.I_a"));
        qInfo("junction at 0.5 V: %.8g A in 2D, %.8g A in 1D", I2, I1);
        QVERIFY2(I2 > 0 && relative(I1, I2) < 1e-5, qPrintable(QStringLiteral("%1 %2").arg(I1).arg(I2)));
        QVERIFY(relative(global(*one.solution, QStringLiteral("semi.I_k")), -I1) < 1e-6);
        QVERIFY(one.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("solved in 1D along x = 0 um")));
        // At each node of the 2D solution's, its sides' too (a node where the
        // layers meet takes their dopings by its circumcentric box: on a side
        // as in the middle - by its triangles' areas a side's leaned 1/3 to
        // 2/3, and 1D its strip of eight columns to keep its middle clear):
        // V and the carriers to rounding.
        {
            const Field& f2 = plane.solution->fields()[std::size_t(plane.solution->fieldIndex(QStringLiteral("semi")))];
            const Field& f1 = one.solution->fields()[std::size_t(one.solution->fieldIndex(QStringLiteral("semi")))];
            const Mesh& mesh = plane.solution->mesh();
            QVERIFY(mesh.nodes == one.solution->mesh().nodes);
            const std::size_t N = std::size_t(f2.space->size());
            QCOMPARE(N, mesh.nodes.size());
            double dV = 0, dn = 0, largest = 0;
            for (std::size_t i = 0; i < N; ++i) largest = std::max({largest, f2.values[N + i], f2.values[2 * N + i]});
            for (std::size_t i = 0; i < N; ++i) {
                dV = std::max(dV, std::abs(f2.values[i] - f1.values[i]));
                for (const std::size_t c : {N + i, 2 * N + i})
                    if (f2.values[c] > 1e-6 * largest) dn = std::max(dn, relative(f1.values[c], f2.values[c]));
            }
            qInfo("the nodes: V within %.3g V, n and p within %.3g", dV, dn);
            QVERIFY2(dV < 1e-9 && dn < 1e-8, qPrintable(QStringLiteral("%1 %2").arg(dV).arg(dn)));
            // (Uniform across the 2D: its side's node as its middle's.)
            const QPointF side(-1, 0.2), middle(0, 0.2);
            QVERIFY(std::abs(at(*plane.solution, QStringLiteral("semi.V"), side) - at(*plane.solution, QStringLiteral("semi.V"), middle)) < 1e-9);
            // The unknowns said: its strip's (three nodes across), fewer than
            // the 2D's (nine) - not this mesh's.
            auto solvedOf = [](const Run& r) {
                const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("Solved (\\d+) degrees")).match(r.solution->log.join(QLatin1Char('\n')));
                return m.hasMatch() ? m.captured(1).toInt() : -1;
            };
            qInfo("solved: %d unknowns in 1D, %d in 2D", solvedOf(one), solvedOf(plane));
            QVERIFY(solvedOf(one) > 0 && solvedOf(one) < solvedOf(plane) / 2);
            // Its "at" not given: said - a warning, not a log line alone (the
            // update check of 8bd5709).
            QVERIFY(one.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("its \"at\" not given")));
            QVERIFY2(one.study.warnings.join(QLatin1Char('\n')).contains(QLatin1String("its \"at\" not given")), qPrintable(one.study.warnings.join(QLatin1Char('\n'))));
        }
        // The same y anywhere across: the line's values.
        QCOMPARE(at(*one.solution, QStringLiteral("semi.V"), QPointF(-0.9, 0.11)), at(*one.solution, QStringLiteral("semi.V"), QPointF(0.9, 0.11)));
        // On an isotropic mesh of its own (0.07 µm): the same current; its
        // nodes' values between the line's interpolated.
        {
            const Run coarse = run(modelOf(junction1d(QStringLiteral("1d"), QString(), false, QStringLiteral(R"({"type": "stationary"})"), line, QStringLiteral("0.07"))));
            QVERIFY2(coarse.error.isEmpty(), qPrintable(coarse.error));
            // (Said: what reads it here sees it as coarse as this mesh.)
            QVERIFY2(coarse.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("this mesh's nodes up to")),
                     qPrintable(coarse.solution->log.join(QLatin1Char('\n'))));
            QVERIFY2(coarse.study.warnings.join(QLatin1Char('\n')).contains(QLatin1String("this mesh's nodes up to")),
                     qPrintable(coarse.study.warnings.join(QLatin1Char('\n'))));
            QVERIFY(relative(global(*coarse.solution, QStringLiteral("semi.I_a")), I1) < 1e-9);
            // Its nodes', between the line's: V linear, the carriers in their
            // logarithm - as the 2D solution reads at those points.
            const Field& fc = coarse.solution->fields()[std::size_t(coarse.solution->fieldIndex(QStringLiteral("semi")))];
            const Mesh& cm = coarse.solution->mesh();
            const std::size_t N = cm.nodes.size();
            int compared = 0;
            for (std::size_t i = 0; i < N; ++i) {
                const QPointF q = cm.nodes[i];
                if (std::abs(q.x()) > 0.5) continue;
                // (Off the reference's rows: between two of its nodes.)
                const double row = q.y() / 0.005;
                if (std::abs(row - std::round(row)) < 0.05) continue;
                ++compared;
                QVERIFY2(std::abs(fc.values[i] - at(*plane.solution, QStringLiteral("semi.V"), QPointF(0, q.y()))) < 1e-4, qPrintable(QString::number(q.y())));
                for (const auto& [index, name] : {std::pair{N + i, "semi.n"}, std::pair{2 * N + i, "semi.p"}})
                    QVERIFY2(relative(fc.values[index], at(*plane.solution, QString::fromLatin1(name), QPointF(0, q.y()))) < 1e-3,
                             qPrintable(QStringLiteral("%1 at %2: %3 %4").arg(QLatin1String(name)).arg(q.y()).arg(fc.values[index]).arg(at(*plane.solution, QString::fromLatin1(name), QPointF(0, q.y())))));
            }
            QVERIFY(compared >= 2);
            // Between its nodes, at any point: the line's values (V linear,
            // the carriers in their logarithm, the field the line's slope) -
            // its 5 nm, not this mesh's 70 (the update check of 21 October,
            // 6: a model of "h1d" 0.5 nm read its fields on nodes 5 nm apart).
            double dV = 0, dn = 0, dE = 0;
            for (double y = 0.0031; y < 0.4; y += 0.0127) {
                const QPointF q(0.3, y), p(0, y);
                dV = std::max(dV, std::abs(at(*coarse.solution, QStringLiteral("semi.V"), q) - at(*plane.solution, QStringLiteral("semi.V"), p)));
                // (The bands too: of V.)
                dV = std::max(dV, std::abs(at(*coarse.solution, QStringLiteral("semi.Ec"), q) - at(*plane.solution, QStringLiteral("semi.Ec"), p)) / Q);
                for (const char* name : {"semi.n", "semi.p"})
                    dn = std::max(dn, relative(at(*coarse.solution, QLatin1String(name), q), at(*plane.solution, QLatin1String(name), p)));
                const double Ey = at(*plane.solution, QStringLiteral("semi.Ey"), p);
                if (std::abs(Ey) > 1e6) dE = std::max(dE, relative(at(*coarse.solution, QStringLiteral("semi.Ey"), q), Ey));
            }
            qInfo("between the coarse mesh's nodes: V and Ec within %.3g V, n and p %.3g, Ey %.3g of the 2D's", dV, dn, dE);
            QVERIFY2(dV < 1e-4 && dn < 1e-3 && dE < 0.02, qPrintable(QStringLiteral("%1 %2 %3").arg(dV).arg(dn).arg(dE)));
        }
        // Under a mesa (the n layer 4 µm wide): the stack's current, the
        // mesa's 2 µm - the cathode's as the anode's.
        {
            const Run mesa = run(modelOf(junction1d(QStringLiteral("1d"), QString(), true, QStringLiteral(R"({"type": "stationary"})"), line)));
            QVERIFY2(mesa.error.isEmpty(), qPrintable(mesa.error));
            QVERIFY(relative(global(*mesa.solution, QStringLiteral("semi.I_a")), I1) < 1e-9);
            QVERIFY(relative(global(*mesa.solution, QStringLiteral("semi.I_k")), -I1) < 1e-9);
            // (Said: a 2D solve's current spreads where the stack widens.)
            QVERIFY(mesa.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("spreads it")));
            // Beside the mesa (x 1.5, the n layer's): its y's values.
            QCOMPARE(at(*mesa.solution, QStringLiteral("semi.n"), QPointF(1.5, 0.3)), at(*mesa.solution, QStringLiteral("semi.n"), QPointF(0, 0.3)));
        }
        // A bias sweep: each point solved along its line.
        {
            const Run sweep = run(modelOf(junction1d(QStringLiteral("1d"), layers, false,
                                                     QStringLiteral(R"({"type": "sweep", "parameters": [["Va", "0.3, 0.5", "V"]]}, {"type": "stationary"})"), line)));
            QVERIFY2(sweep.error.isEmpty() && sweep.study.solutions.size() == 2, qPrintable(sweep.error));
            const double low = global(*sweep.study.solutions[0], QStringLiteral("semi.I_a")), high = global(*sweep.study.solutions[1], QStringLiteral("semi.I_a"));
            QVERIFY(relative(high, I1) < 1e-6 && low > 0 && high > 50 * low);
            // (Its second point from its first's strip - not from
            // equilibrium again, as the 2D's from its point before: the update
            // check of 8bd5709.)
            auto strip = [](const Solution& s) { return s.fields()[std::size_t(s.fieldIndex(QStringLiteral("semi")))].line; };
            auto steps = [](const Solution& s) {
                const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("voltages reached in (\\d+) steps")).match(s.log.join(QLatin1Char('\n')));
                return m.hasMatch() ? m.captured(1).toInt() : 1;   // (none said: one step - 0.5 V from 0.3 V's, since the hunt of 19 October, B4)
            };
            QVERIFY(strip(*sweep.study.solutions[0]) && strip(*sweep.study.solutions[1]) && strip(*one.solution));
            QVERIFY(strip(*sweep.study.solutions[1])->meshPtr() == strip(*sweep.study.solutions[0])->meshPtr());
            const int fromBefore = steps(*strip(*sweep.study.solutions[1])), fromRest = steps(*strip(*one.solution));
            qInfo("0.5 V along its line: %d steps from 0.3 V's, %d from equilibrium", fromBefore, fromRest);
            QVERIFY2(fromBefore > 0 && fromBefore < fromRest, qPrintable(QStringLiteral("%1 %2").arg(fromBefore).arg(fromRest)));
        }
        // A mesa's swept: its warning once, not once a point, with numbers -
        // not "some %" (the update check of 8bd5709).
        {
            const Run sweep = run(modelOf(junction1d(QStringLiteral("1d"), QString(), true,
                                                     QStringLiteral(R"({"type": "sweep", "parameters": [["Va", "0.3, 0.5, 0.7", "V"]]}, {"type": "stationary"})"), line)));
            QVERIFY2(sweep.error.isEmpty(), qPrintable(sweep.error));
            const QStringList spreads = sweep.study.warnings.filter(QLatin1String("spreads it"));
            QCOMPARE(int(spreads.size()), 1);
            QVERIFY2(spreads.front().contains(QLatin1String("4 to 11 %")) && !spreads.front().contains(QLatin1String("some %")), qPrintable(spreads.front()));
        }
        // Refused, and said: a step in frequency; a contact the line does not
        // reach (on a side); a line with a gap in it.
        {
            const Run f = run(modelOf(junction1d(QStringLiteral("1d"), layers, false,
                                                 QStringLiteral(R"({"type": "stationary"}, {"type": "frequency", "frequencies": "1[MHz]"})"), line)));
            QVERIFY2(f.error.contains(QLatin1String("at rest")), qPrintable(f.error));
            QByteArray side = junction1d(QStringLiteral("1d"), layers, false, QStringLiteral(R"({"type": "stationary"})"), line);
            side.replace(R"("selection": {"of": ["n"], "side": "top"})", R"("selection": {"of": ["n"], "side": "right"})");
            const Run s1 = run(modelOf(side));
            QVERIFY2(s1.error.contains(QLatin1String("not where its line ends")), qPrintable(s1.error));
            QByteArray gap = junction1d(QStringLiteral("1d"), QString(), false, QStringLiteral(R"({"type": "stationary"})"), line);
            gap.replace(R"("position": ["-1", "0.2"], "size": ["2", "0.2"])", R"("position": ["-1", "0.3"], "size": ["2", "0.2"])");
            const Run s2 = run(modelOf(gap));
            QVERIFY2(s2.error.contains(QLatin1String("leaves its domains")), qPrintable(s2.error));
        }
    }
};

QTEST_GUILESS_MAIN(TestFemModulator)
#include "test_fem_modulator.moc"
