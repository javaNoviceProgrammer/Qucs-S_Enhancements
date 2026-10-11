/*
 * test_fem_waves.cpp - the multiphysics solver's waves against what is
 *                      known: the eigensolver, a rectangular waveguide's
 *                      modes, a dielectric slab's (its transcendental
 *                      equation), a coax's TEM mode, a lossy filling, a
 *                      thermo-optic shift; waves in the plane - a
 *                      parallel-plate guide's S-parameters, an iris's power
 *                      kept, a PML that does not reflect; adaptive
 *                      meshes; the examples
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_edge.h"
#include "fem_export.h"
#include "fem_results.h"

#include <Eigen/Dense>

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtTest>

#include <cmath>
#include <complex>

using namespace qucs_s::fem;

namespace {

constexpr double C0 = 299792458.0;
using cd = std::complex<double>;
/// The L-shaped gap's energy (J per metre, 1 V) on a uniform mesh of
/// 104,215 triangles (hmax 0.01 mm), linear elements.
constexpr double FineCornerEnergy = 1.132892e-11;
/// The iris's S21 at 20 GHz on a uniform mesh of 34,488 triangles (hmax
/// 0.2 mm), quadratic elements.
const std::complex<double> FineIrisS21(-0.29350589, 0.20824795);

bool slowBuild()
{
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

double global(Solution& s, const QString& name)
{
    const std::optional<double> v = s.global(name);
    return v ? *v : NAN;
}

/// The effective indices of a solution's modes, at its first frequency.
std::vector<double> indices(Solution& s, const QString& tag = QStringLiteral("emw"))
{
    std::vector<double> out;
    for (int k = 0; k < s.snapshotCount(); ++k) {
        if (s.snapshotFrequency(k) != s.snapshotFrequency(0)) break;
        s.select(k);
        out.push_back(global(s, tag + QStringLiteral(".neff")));
    }
    return out;
}

/// A symmetric slab's TE0: tan(κ d/2) = γ/κ (κ in the core, γ outside).
double slabTE0(double n1, double n2, double d, double lambda)
{
    const double k0 = 2 * M_PI / lambda;
    double lo = n2 + 1e-9, hi = n1 - 1e-9;
    auto g = [&](double n) {
        const double kappa = k0 * std::sqrt(n1 * n1 - n * n), gamma = k0 * std::sqrt(n * n - n2 * n2);
        return std::tan(kappa * d / 2) - gamma / kappa;
    };
    for (int i = 0; i < 200; ++i) {
        const double mid = (lo + hi) / 2;
        // (κ d/2 below π/2 for the first: tan rises from 0 as n falls)
        const double kappa = k0 * std::sqrt(n1 * n1 - mid * mid);
        if (kappa * d / 2 >= M_PI / 2 || g(mid) > 0) lo = mid;
        else hi = mid;
    }
    return (lo + hi) / 2;
}

/// A rectangular metal waveguide a × b (mm), its modes at \a f.
QByteArray rectangular(const QString& f, int modes, const QString& fill = QStringLiteral("\"n\": \"1\""), int order = 2,
                       const QString& guess = QStringLiteral("1"))
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "mm",
        "geometry": [{"type": "rectangle", "name": "guide", "size": ["22.86", "10.16"]}],
        "materials": [{"label": "fill", %3, "selection": {"all": true}}],
        "physics": [{"type": "modes", "tag": "emw", "order": "%4"}],
        "mesh": {"size": "normal", "custom": true, "hmax": "1"}}],
      "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "%1", "modes": %2, "neff": "%5"}]}]})json")
        .arg(f)
        .arg(modes)
        .arg(fill)
        .arg(order)
        .arg(guess)
        .toUtf8();
}

} // namespace

class TestFemWaves : public QObject
{
    Q_OBJECT

private slots:
    void eigensolver()
    {
        // A diagonal of 1 ... 50 and a little coupling: its largest five.
        const int n = 50;
        Eigen::MatrixXcd A = Eigen::MatrixXcd::Zero(n, n);
        for (int i = 0; i < n; ++i) {
            A(i, i) = double(i + 1);
            if (i + 1 < n) A(i, i + 1) = A(i + 1, i) = 0.1;
        }
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> exact(A);
        const EigenResult r = largestEigenvalues(n, 5, [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) { y = A * x; });
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.values.size()), 5);
        for (int i = 0; i < 5; ++i) {
            QVERIFY(std::abs(r.values[std::size_t(i)] - exact.eigenvalues()(n - 1 - i)) < 1e-9);
            QVERIFY(((A * r.vectors[std::size_t(i)]) - r.values[std::size_t(i)] * r.vectors[std::size_t(i)]).norm() < 1e-7);
        }
        // Non-Hermitian, complex: against a dense solver.
        Eigen::MatrixXcd B = A;
        for (int i = 0; i + 2 < n; ++i) B(i, i + 2) = cd(0.05, 0.3);
        Eigen::ComplexEigenSolver<Eigen::MatrixXcd> dense(B);
        std::vector<cd> all(dense.eigenvalues().data(), dense.eigenvalues().data() + n);
        std::sort(all.begin(), all.end(), [](cd a, cd b) { return std::abs(a) > std::abs(b); });
        const EigenResult rb = largestEigenvalues(n, 3, [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) { y = B * x; });
        QCOMPARE(int(rb.values.size()), 3);
        for (int i = 0; i < 3; ++i) QVERIFY(std::abs(rb.values[std::size_t(i)] - all[std::size_t(i)]) < 1e-8 * std::abs(all[std::size_t(i)]));
    }

    void rectangularWaveguide()
    {
        // WR-90 at 20 GHz: TE10, TE20, TE01, then TE11 and TM11 (one cutoff).
        const Run r = run(modelOf(rectangular(QStringLiteral("20[GHz]"), 5)));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        QVERIFY(s.modeAnalysis);
        const std::vector<double> got = indices(s);
        QCOMPARE(int(got.size()), 5);
        const double a = 22.86e-3, b = 10.16e-3, f = 20e9;
        auto neff = [&](int m, int n) { return std::sqrt(1 - std::pow(C0 / (2 * f), 2) * (std::pow(m / a, 2) + std::pow(n / b, 2))); };
        const double expected[5] = {neff(1, 0), neff(2, 0), neff(0, 1), neff(1, 1), neff(1, 1)};
        for (int i = 0; i < 5; ++i)
            QVERIFY2(std::abs(got[std::size_t(i)] - expected[i]) < 2e-4,
                     qPrintable(QStringLiteral("mode %1: %2 against %3").arg(i + 1).arg(got[std::size_t(i)]).arg(expected[i])));
        // TE10: E along y, its power 1 W, nothing along z.
        s.select(0);
        const Locator locator(s.mesh());
        const double ey = std::hypot(*evaluateAt(s, locator, QStringLiteral("emw.Ey"), {11.43, 5.08}),
                                     *evaluateAt(s, locator, QStringLiteral("emw.Ey_im"), {11.43, 5.08}));
        const double ex = std::hypot(*evaluateAt(s, locator, QStringLiteral("emw.Ex"), {11.43, 5.08}),
                                     *evaluateAt(s, locator, QStringLiteral("emw.Ex_im"), {11.43, 5.08}));
        QVERIFY(ey > 100 * ex);
        QVERIFY(*evaluateAt(s, locator, QStringLiteral("emw.normE"), {1, 5.08}) < 0.2 * ey);
        // A lossless mode at a phase of nought: real, not as the eigensolver
        // happened to turn it.
        QVERIFY(std::abs(*evaluateAt(s, locator, QStringLiteral("emw.Ey_im"), {11.43, 5.08})) < 1e-6 * ey);
        Node power;
        power.type = QStringLiteral("integral");
        power.props.insert(QStringLiteral("expression"), QStringLiteral("emw.Sz"));
        power.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
        const DerivedResult p = evaluateDerived(s, power);
        QVERIFY2(p.error.isEmpty(), qPrintable(p.error));
        QVERIFY2(std::abs(p.values.at(0).value - 1) < 1e-6, qPrintable(QString::number(p.values.at(0).value)));
        // Its wave impedance Ey/Hx = η0/neff.
        const double hx = std::hypot(*evaluateAt(s, locator, QStringLiteral("emw.Hx"), {11.43, 5.08}),
                                     *evaluateAt(s, locator, QStringLiteral("emw.Hx_im"), {11.43, 5.08}));
        const double eta0 = 376.730313;
        QVERIFY2(std::abs(ey / hx / (eta0 / expected[0]) - 1) < 1e-3, qPrintable(QString::number(ey / hx)));
        // Linear elements, coarser: still near.
        const Run lin = run(modelOf(rectangular(QStringLiteral("20[GHz]"), 3, QStringLiteral("\"n\": \"1\""), 1)));
        QVERIFY2(lin.error.isEmpty(), qPrintable(lin.error));
        const std::vector<double> l = indices(*lin.solution);
        QVERIFY(std::abs(l.at(0) - expected[0]) < 5e-3 && std::abs(l.at(0) - expected[0]) > std::abs(got[0] - expected[0]));
    }

    void lossyFilling()
    {
        // Filled with εr = 2.2 (1 - j 0.01): β² = k0² εr - (π/a)², complex
        // (its wave equation by permittivity).
        // (About 1.5: TE10's 1.33 is nearer than TE20's 0.69.)
        Model m = modelOf(rectangular(QStringLiteral("10[GHz]"), 1, QStringLiteral(R"("epsilonr": "2.2", "tand": "0.01")"), 2, QStringLiteral("1.5")));
        Node* phys = nullptr;
        for (Node& c : m.component().children)
            if (c.type == QLatin1String("modes")) phys = &c;
        QVERIFY(phys);
        for (Node& f : phys->children)
            if (f.type == QLatin1String("modes/wave")) f.props.insert(QStringLiteral("constitutive"), QStringLiteral("epsilonr"));
        const Run lr = run(m);
        QVERIFY2(lr.error.isEmpty(), qPrintable(lr.error));
        Solution& s = *lr.solution;
        s.select(0);
        const double f = 10e9, a = 22.86e-3, k0 = 2 * M_PI * f / C0;
        const cd beta = std::sqrt(k0 * k0 * cd(2.2, -0.022) - std::pow(M_PI / a, 2));
        const double neff = global(s, QStringLiteral("emw.neff")), alpha = global(s, QStringLiteral("emw.alpha"));
        QVERIFY2(std::abs(neff - beta.real() / k0) < 2e-4, qPrintable(QStringLiteral("%1 against %2").arg(neff).arg(beta.real() / k0)));
        QVERIFY2(std::abs(alpha / -beta.imag() - 1) < 2e-3, qPrintable(QStringLiteral("α %1 against %2").arg(alpha).arg(-beta.imag())));
        QVERIFY(std::abs(global(s, QStringLiteral("emw.loss")) - alpha * 20 / std::log(10.0)) < 1e-9);
    }

    void dielectricSlab()
    {
        // A silicon slab 220 nm thick in oxide, across the window (its sides
        // conductors: the TE slab mode, E along x, meets them at right
        // angles): its TE0 at 1550 nm against the slab's equation.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "oxide", "position": ["0", "-1.5"], "size": ["0.4", "3.22"]},
                         {"type": "rectangle", "name": "core", "size": ["0.4", "0.22"]}],
            "materials": [{"label": "oxide", "n": "1.444", "selection": {"only": ["oxide"]}},
                          {"label": "silicon", "n": "3.476", "selection": {"only": ["core"]}}],
            "physics": [{"type": "modes", "tag": "emw"}],
            "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "0.02"},
                                                  {"type": "size", "level": "domain", "selection": {"only": ["oxide"]}, "hmax": "0.08"}]}}],
          "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "3"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        s.select(0);
        const double expected = slabTE0(3.476, 1.444, 0.22e-6, 1.55e-6);
        const double got = global(s, QStringLiteral("emw.neff"));
        qInfo("slab TE0: neff %.6f, the slab's equation %.6f", got, expected);
        QVERIFY2(std::abs(got - expected) < 2e-5, qPrintable(QStringLiteral("%1 against %2").arg(got).arg(expected)));
        // Its wavelength said back.
        QVERIFY(std::abs(global(s, QStringLiteral("emw.lambda0")) - 1.55e-6) < 1e-15);
        QVERIFY(std::abs(s.frequency() - C0 / 1.55e-6) < 1);
    }

    void coaxTEM()
    {
        // A coax filled with n = 1.45: its TEM mode's neff is n.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "circle", "name": "fill", "radius": "3"}, {"type": "circle", "name": "inner", "radius": "1"}],
            "materials": [{"label": "fill", "n": "1.45", "selection": {"only": ["fill"]}}],
            "physics": [{"type": "modes", "tag": "emw", "selection": {"only": ["fill"]}}],
            "mesh": {"size": "fine"}}],
          "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1[GHz]", "modes": 1, "neff": "1.45"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        r.solution->select(0);
        const double got = global(*r.solution, QStringLiteral("emw.neff"));
        QVERIFY2(std::abs(got - 1.45) < 1e-4, qPrintable(QString::number(got, 'g', 10)));
    }

    void thermoOptic()
    {
        // The slab warmed 10 K in a thermo-optic silicon: as the index raised
        // by dn/dT × 10 K.
        auto slab = [](const QString& core) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "oxide", "position": ["0", "-1.5"], "size": ["0.4", "3.22"]},
                             {"type": "rectangle", "name": "core", "size": ["0.4", "0.22"]}],
                "materials": [{"label": "oxide", "n": "1.444", "selection": {"only": ["oxide"]}},
                              {"label": "silicon", "n": "3.476", "dndT": "1.86e-4[1/K]", "selection": {"only": ["core"]}}],
                "physics": [{"type": "modes", "tag": "emw", "features": [%1]}],
                "mesh": {"size": "normal", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "0.03"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "3"}]}]})json")
                .arg(core)
                .toUtf8();
        };
        const Run warm = run(modelOf(slab(QStringLiteral(
            R"({"type": "wave", "thermooptic": true, "T": "303.15[K]", "selection": {"only": ["core"]}})"))));
        QVERIFY2(warm.error.isEmpty(), qPrintable(warm.error));
        const Run raised = run(modelOf(slab(QStringLiteral(
            R"({"type": "wave", "source": "user", "n": "3.476 + 1.86e-3", "selection": {"only": ["core"]}})"))));
        QVERIFY2(raised.error.isEmpty(), qPrintable(raised.error));
        const Run cold = run(modelOf(slab(QString())));
        QVERIFY2(cold.error.isEmpty(), qPrintable(cold.error));
        warm.solution->select(0);
        raised.solution->select(0);
        cold.solution->select(0);
        const double w = global(*warm.solution, QStringLiteral("emw.neff")), r = global(*raised.solution, QStringLiteral("emw.neff"));
        const double c = global(*cold.solution, QStringLiteral("emw.neff"));
        QVERIFY(std::abs(w - r) < 1e-9);
        QVERIFY(w - c > 1e-3 && w - c < 1.86e-3);
    }

    void parallelPlate()
    {
        // An H-plane guide 10 mm wide (Ez: sin(πy/w)), 30 mm long, a port at
        // each end: S21 = e^-jβL, S11 nought; Hz: its TEM mode, β = k0.
        auto guide = [](const QString& pol, const QString& extra) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "mm",
                "geometry": [{"type": "rectangle", "name": "guide", "size": ["30", "10"]} %2],
                "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
                "physics": [{"type": "waves", "tag": "ewfd", "polarization": "%1", "features": [
                  {"type": "port", "name": "1", "selection": {"of": ["guide"], "side": "left"}},
                  {"type": "port", "name": "2", "selection": {"of": ["guide"], "side": "right"}}]}],
                "mesh": {"size": "fine", "custom": true, "hmax": "0.8"}}],
              "studies": [{"type": "study", "steps": [{"type": "frequency", "frequencies": "20[GHz]"}]}]})json")
                .arg(pol, extra)
                .toUtf8();
        };
        const Run r = run(modelOf(guide(QStringLiteral("ez"), QString())));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        const double k0 = 2 * M_PI * 20e9 / C0;
        const double beta = std::sqrt(k0 * k0 - std::pow(M_PI / 10e-3, 2));
        QVERIFY2(std::abs(global(s, QStringLiteral("ewfd.beta_1")) / beta - 1) < 1e-4,
                 qPrintable(QStringLiteral("%1 against %2").arg(global(s, QStringLiteral("ewfd.beta_1"))).arg(beta)));
        const cd s21(global(s, QStringLiteral("ewfd.S21")), global(s, QStringLiteral("ewfd.S21_im")));
        const cd s11(global(s, QStringLiteral("ewfd.S11")), global(s, QStringLiteral("ewfd.S11_im")));
        const cd expected = std::exp(cd(0, -beta * 30e-3));
        qInfo("parallel plate: S21 %.5f%+.5fj (e^-jβL %.5f%+.5fj), |S11| %.2e", s21.real(), s21.imag(), expected.real(), expected.imag(), std::abs(s11));
        QVERIFY(std::abs(s21 - expected) < 2e-3);
        QVERIFY(std::abs(s11) < 2e-3);
        QVERIFY(std::abs(global(s, QStringLiteral("ewfd.S12")) - global(s, QStringLiteral("ewfd.S21"))) < 1e-6);
        // Hz: the TEM mode.
        const Run h = run(modelOf(guide(QStringLiteral("hz"), QString())));
        QVERIFY2(h.error.isEmpty(), qPrintable(h.error));
        QVERIFY(std::abs(global(*h.solution, QStringLiteral("ewfd.beta_1")) / k0 - 1) < 1e-6);
        const cd h21(global(*h.solution, QStringLiteral("ewfd.S21")), global(*h.solution, QStringLiteral("ewfd.S21_im")));
        QVERIFY(std::abs(h21 - std::exp(cd(0, -k0 * 30e-3))) < 2e-3);
    }

    void irisKeepsPower()
    {
        // An inductive iris across the guide: |S11|² + |S21|² = 1, S12 = S21.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "name": "guide", "size": ["40", "10"]},
                         {"type": "rectangle", "name": "iris1", "position": ["19.5", "0"], "size": ["1", "3"]},
                         {"type": "rectangle", "name": "iris2", "position": ["19.5", "7"], "size": ["1", "3"]},
                         {"type": "difference", "name": "hollow", "input": ["guide"], "tools": ["iris1", "iris2"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
            "physics": [{"type": "waves", "tag": "ewfd", "features": [
              {"type": "port", "name": "1", "selection": {"of": ["hollow"], "side": "left"}},
              {"type": "port", "name": "2", "selection": {"of": ["hollow"], "side": "right"}}]}],
            "mesh": {"size": "fine", "custom": true, "hmax": "0.8"}}],
          "studies": [{"type": "study", "steps": [{"type": "frequency", "frequencies": "17[GHz], 20[GHz]"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        QCOMPARE(s.snapshotCount(), 2);
        for (int k = 0; k < 2; ++k) {
            s.select(k);
            const double a11 = global(s, QStringLiteral("ewfd.S11_abs")), a21 = global(s, QStringLiteral("ewfd.S21_abs"));
            qInfo("iris at %.0f GHz: |S11| %.4f, |S21| %.4f, sum of squares %.6f", s.frequency() / 1e9, a11, a21, a11 * a11 + a21 * a21);
            QVERIFY(a11 > 0.1);
            QVERIFY(std::abs(a11 * a11 + a21 * a21 - 1) < 1e-3);
            QVERIFY(std::abs(global(s, QStringLiteral("ewfd.S12")) - global(s, QStringLiteral("ewfd.S21"))) < 1e-4);
            QVERIFY(std::abs(global(s, QStringLiteral("ewfd.S12_im")) - global(s, QStringLiteral("ewfd.S21_im"))) < 1e-4);
        }
    }

    void pmlMatches()
    {
        // The guide ended by a PML: nothing comes back to its port.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "name": "guide", "size": ["30", "10"]},
                         {"type": "rectangle", "name": "absorber", "position": ["30", "0"], "size": ["15", "10"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
            "physics": [{"type": "waves", "tag": "ewfd", "features": [
              {"type": "pml", "selection": {"only": ["absorber"]}},
              {"type": "port", "name": "1", "selection": {"of": ["guide"], "side": "left"}}]}],
            "mesh": {"size": "fine", "custom": true, "hmax": "0.6"}}],
          "studies": [{"type": "study", "steps": [{"type": "frequency", "frequencies": "20[GHz]"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double s11 = global(*r.solution, QStringLiteral("ewfd.S11_abs"));
        qInfo("PML: |S11| %.2e", s11);
        QVERIFY2(s11 < 0.02, qPrintable(QString::number(s11)));
    }

    void adaptiveMeshRefinement()
    {
        // An L-shaped plate, its potential 0 on the sides of its reentrant
        // corner and 1 on the far ones: the field infinite at the corner. Its
        // energy on a mesh refined where the error is against a uniform fine
        // one's.
        auto plate = [](const QString& mesh, bool adaptive) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "mm",
                "geometry": [{"type": "rectangle", "name": "square", "size": ["2", "2"]},
                             {"type": "rectangle", "name": "cut", "position": ["1", "1"], "size": ["1", "1"]},
                             {"type": "difference", "name": "plate", "input": ["square"], "tools": ["cut"]}],
                "materials": [{"label": "m", "epsilonr": "1", "selection": {"all": true}}],
                "physics": [{"type": "electrostatics", "tag": "es", "order": "1", "features": [
                  {"type": "ground", "selection": {"box": [0.99, 0.99, 2.01, 1.01]}},
                  {"type": "ground", "selection": {"box": [0.99, 0.99, 1.01, 2.01]}},
                  {"type": "potential", "V0": "1[V]", "selection": {"box": [-0.01, -0.01, 0.01, 2.01]}},
                  {"type": "potential", "V0": "1[V]", "selection": {"box": [-0.01, -0.01, 2.01, 0.01]}}]}],
                "mesh": %1}],
              "studies": [{"type": "study", "steps": [{"type": "stationary", "adaptive": %2, "adaptsteps": 4}]}]})json")
                .arg(mesh, adaptive ? QStringLiteral("true") : QStringLiteral("false"))
                .toUtf8();
        };
        const QString coarse = QStringLiteral(R"({"size": "normal", "custom": true, "hmax": "0.25"})");
        const Run fine = run(modelOf(plate(QStringLiteral(R"({"size": "normal", "custom": true, "hmax": "0.01"})"), false)));
        QVERIFY2(fine.error.isEmpty(), qPrintable(fine.error));
        const Run first = run(modelOf(plate(coarse, false)));
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
        const Run adapted = run(modelOf(plate(coarse, true)));
        QVERIFY2(adapted.error.isEmpty(), qPrintable(adapted.error));
        const double ref = global(*fine.solution, QStringLiteral("es.W")), w0 = global(*first.solution, QStringLiteral("es.W"));
        const double wa = global(*adapted.solution, QStringLiteral("es.W"));
        const int n0 = int(first.solution->mesh().triangles.size()), na = int(adapted.solution->mesh().triangles.size());
        const int nf = int(fine.solution->mesh().triangles.size());
        qInfo("adaptive: W %.6e on %d triangles, %.6e on %d refined (fine %.6e on %d); %s", w0, n0, wa, na, ref, nf,
              qPrintable(adapted.solution->log.filter(QStringLiteral("Adaptive")).join(QString())));
        QVERIFY(na > n0 && na < nf / 4);
        QVERIFY2(std::abs(wa - ref) < std::abs(w0 - ref) / 4, qPrintable(QStringLiteral("%1 %2").arg(std::abs(wa - ref)).arg(std::abs(w0 - ref))));
        // Its smallest triangles at the corner.
        const Mesh& m = adapted.solution->mesh();
        double smallest = 1e300;
        QPointF where;
        for (const auto& t : m.triangles) {
            const QPointF a = m.nodes[std::size_t(t[0])], b = m.nodes[std::size_t(t[1])], c = m.nodes[std::size_t(t[2])];
            const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y()));
            if (area < smallest) {
                smallest = area;
                where = (a + b + c) / 3.0;
            }
        }
        QVERIFY2(std::hypot(where.x() - 1, where.y() - 1) < 0.1, qPrintable(QStringLiteral("%1 %2").arg(where.x()).arg(where.y())));
    }

    void adaptiveInFrequency()
    {
        // The iris at 20 GHz on a coarse mesh refined where the field's error
        // is (its corners): S21 near a fine mesh's.
        auto iris = [](const QString& hmax, bool adaptive) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "mm",
                "geometry": [{"type": "rectangle", "name": "guide", "size": ["40", "10"]},
                             {"type": "rectangle", "name": "iris1", "position": ["19.5", "0"], "size": ["1", "3"]},
                             {"type": "rectangle", "name": "iris2", "position": ["19.5", "7"], "size": ["1", "3"]},
                             {"type": "difference", "name": "hollow", "input": ["guide"], "tools": ["iris1", "iris2"]}],
                "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
                "physics": [{"type": "waves", "tag": "ewfd", "features": [
                  {"type": "port", "name": "1", "selection": {"of": ["hollow"], "side": "left"}},
                  {"type": "port", "name": "2", "selection": {"of": ["hollow"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%1"}}],
              "studies": [{"type": "study", "steps": [{"type": "frequency", "frequencies": "20[GHz]", "adaptive": %2, "adaptsteps": 4}]}]})json")
                .arg(hmax, adaptive ? QStringLiteral("true") : QStringLiteral("false"))
                .toUtf8();
        };
        auto s21 = [](const Run& r) {
            return cd(global(*r.solution, QStringLiteral("ewfd.S21")), global(*r.solution, QStringLiteral("ewfd.S21_im")));
        };
        // (A sanitized build: the fine mesh's S21 as an optimized one finds
        // it, 34,488 triangles; its complex LU takes minutes there.)
        cd ref = FineIrisS21;
        int nf = 34488;
        if (!slowBuild()) {
            const Run fine = run(modelOf(iris(QStringLiteral("0.2"), false)));
            QVERIFY2(fine.error.isEmpty(), qPrintable(fine.error));
            ref = s21(fine);
            nf = int(fine.solution->mesh().triangles.size());
            qInfo("the fine iris: S21 %.8f%+.8fj", ref.real(), ref.imag());
            QVERIFY(std::abs(ref - FineIrisS21) < 1e-4);   // (another platform's mesh a little different)
        }
        const Run coarse = run(modelOf(iris(QStringLiteral("2.5"), false)));
        QVERIFY2(coarse.error.isEmpty(), qPrintable(coarse.error));
        const Run adapted = run(modelOf(iris(QStringLiteral("2.5"), true)));
        QVERIFY2(adapted.error.isEmpty(), qPrintable(adapted.error));
        const double e0 = std::abs(s21(coarse) - ref), ea = std::abs(s21(adapted) - ref);
        const int n0 = int(coarse.solution->mesh().triangles.size()), na = int(adapted.solution->mesh().triangles.size());
        qInfo("adaptive in frequency: S21 off by %.2e on %d triangles, %.2e on %d refined (fine: %d); %s", e0, n0, ea, na, nf,
              qPrintable(adapted.solution->log.filter(QStringLiteral("Adaptive")).join(QString())));
        QVERIFY(na > n0 && na < nf / 3);
        QVERIFY2(ea < e0 / 3, qPrintable(QStringLiteral("%1 %2").arg(ea).arg(e0)));
    }

    void theExamples()
    {
        // Phase 4's examples, solved as they come, against what they say.
        const QString dir = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
        auto load = [&](const QString& name) {
            QFile f(dir + name);
            if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
            Model m = modelOf(f.readAll());
            m.fileName = dir + name;
            return m;
        };
        QElapsedTimer timer;
        {
            // WR-90 at 20 GHz: TE10, TE20, TE01, TE11 and TM11.
            timer.start();
            const Run r = run(load(QStringLiteral("wr90_modes.qfem")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const std::vector<double> got = indices(*r.solution);
            QCOMPARE(int(got.size()), 5);
            const double a = 22.86e-3, b = 10.16e-3, f = 20e9;
            auto neff = [&](int m, int n) { return std::sqrt(1 - (std::pow(m / a, 2) + std::pow(n / b, 2)) * C0 * C0 / (4 * f * f)); };
            const double want[5] = {neff(1, 0), neff(2, 0), neff(0, 1), neff(1, 1), neff(1, 1)};
            QStringList each;
            for (int k = 0; k < 5; ++k) {
                each << QStringLiteral("%1 (%2)").arg(got[std::size_t(k)], 0, 'f', 5).arg(want[k], 0, 'f', 5);
                QVERIFY2(std::abs(got[std::size_t(k)] - want[k]) < 1e-4, qPrintable(each.join(QLatin1String(", "))));
            }
            qInfo("WR-90: %s; %lld ms", qPrintable(each.join(QLatin1String(", "))), timer.elapsed());
        }
        // (The SOI wire and the phase shifter are solved in an optimized
        // build only: a sanitized Debug one takes minutes over each.)
        if (!slowBuild()) {
            // The SOI wire: TE0 about 2.44 at 1550 nm, TM0 below; both falling
            // with the wavelength; nothing lost but to the layers' little.
            timer.start();
            const Run r = run(load(QStringLiteral("soi_waveguide_modes.qfem")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.solution;
            QCOMPARE(s.snapshotCount(), 6);
            std::vector<double> n, loss;
            for (int k = 0; k < 6; ++k) {
                s.select(k);
                n.push_back(global(s, QStringLiteral("emw.neff")));
                loss.push_back(global(s, QStringLiteral("emw.loss")));
            }
            const int at1550 = 2;
            QStringList each;
            for (std::size_t k = 0; k < n.size(); ++k) each << QStringLiteral("%1 (%2 dB/m)").arg(n[k], 0, 'f', 4).arg(loss[k], 0, 'g', 2);
            qInfo("SOI: TE0, TM0 %s (1500, 1550, 1600 nm); %s; %lld ms", qPrintable(each.join(QLatin1String(", "))),
                  qPrintable(s.log.filter(QStringLiteral("left out")).value(0)), timer.elapsed());
            QVERIFY2(std::abs(n[std::size_t(at1550)] - 2.445) < 0.01, qPrintable(QString::number(n[std::size_t(at1550)])));
            QVERIFY(n[std::size_t(at1550) + 1] > 1.6 && n[std::size_t(at1550) + 1] < 1.9);
            QVERIFY(n[0] > n[2] && n[2] > n[4]);
            QVERIFY(n[1] > n[3] && n[3] > n[5]);
            // A plot at a wavelength: 1550 nm's second mode.
            SolutionSet set;
            set.solutions = r.study.solutions;
            const std::vector<Instance> at = instances(set, false, 0, QString(), QStringLiteral("1550[nm]"), 2);
            QCOMPARE(int(at.size()), 1);
            QCOMPARE(at.front().snapshot, 3);
            QCOMPARE(instances(set, false, 0, QString(), QStringLiteral("199.86[THz]"), 1).front().snapshot, 0);
            // (Nothing lost, but to the layers: TE0's tail barely reaches them,
            // TM0's a little - a PML turns an evanescent tail's phase, not
            // its size, so its β moves either way by about that.)
            for (std::size_t k = 0; k < loss.size(); ++k)
                QVERIFY2(std::abs(loss[k]) < (k % 2 ? 1.0 : 1e-3), qPrintable(each.join(QLatin1String(", "))));
        }
        if (!slowBuild()) {
            // The thermo-optic phase shifter: neff rising with the power, π
            // at about 24 mW over a millimetre.
            timer.start();
            const Run r = run(load(QStringLiteral("thermo_optic_modes.qfem")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            std::vector<double> n;
            for (const auto& s : r.study.solutions) {
                s->select(s->snapshotCount() - 1);
                n.push_back(global(*s, QStringLiteral("emw.neff")));
            }
            QCOMPARE(int(n.size()), 4);
            const double perMilliwatt = (n.back() - n.front()) / 30;
            const double pi = 1.55e-6 / (2 * 1e-3 * perMilliwatt);
            QStringList each;
            for (double v : n) each << QString::number(v, 'f', 6);
            qInfo("thermo-optic: neff %s; π at %.1f mW; %lld ms", qPrintable(each.join(QLatin1String(", "))), pi, timer.elapsed());
            for (std::size_t k = 1; k < n.size(); ++k) QVERIFY(n[k] > n[k - 1]);
            QVERIFY2(std::abs(n.front() - 2.44) < 0.05, qPrintable(QString::number(n.front())));
            QVERIFY2(pi > 18 && pi < 35, qPrintable(QString::number(pi)));
        }
        {
            // The two-iris filter: no loss, passing near 10 GHz; its S-matrix
            // as a Touchstone file.
            timer.start();
            Model m = load(QStringLiteral("iris_filter.qfem"));
            const int count = slowBuild() ? 3 : 41;
            if (slowBuild())
                for (Node& step : m.find(m.studies().front()->tag)->children)
                    if (step.type == QLatin1String("frequency"))
                        step.props.insert(QStringLiteral("frequencies"), QStringLiteral("8[GHz], 10.1[GHz], 12[GHz]"));
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.solution;
            QCOMPARE(s.snapshotCount(), count);
            double best = 0, at = 0, worst = 1;
            for (int k = 0; k < count; ++k) {
                s.select(k);
                const double a11 = global(s, QStringLiteral("ewfd.S11_abs")), a21 = global(s, QStringLiteral("ewfd.S21_abs"));
                QVERIFY2(std::abs(a11 * a11 + a21 * a21 - 1) < 1e-3, qPrintable(QString::number(a11 * a11 + a21 * a21)));
                if (a21 > best) {
                    best = a21;
                    at = s.frequency();
                }
                worst = std::min(worst, a21);
            }
            qInfo("iris filter: |S21| at most %.4f at %.2f GHz, at least %.4f; %lld ms", best, at / 1e9, worst, timer.elapsed());
            QVERIFY(best > 0.99);
            QVERIFY2(at > 9.5e9 && at < 10.5e9, qPrintable(QString::number(at)));
            QVERIFY(worst < 0.7);
            SolutionSet set;
            set.solutions = r.study.solutions;
            const ExportText e = exportText(m, set, *m.find(QStringLiteral("ts1")), QStringLiteral("26.1.8"));
            QVERIFY2(e.error.isEmpty(), qPrintable(e.error));
            QCOMPARE(e.suffix, QStringLiteral("s2p"));
            QVERIFY(e.text.contains(QStringLiteral("# Hz S RI R 50")));
            QCOMPARE(e.text.count(QRegularExpression(QStringLiteral("^[0-9]"), QRegularExpression::MultilineOption)), count);
        }
        {
            // The corner: the adapted mesh's energy near a far finer one's.
            timer.start();
            const Model m = load(QStringLiteral("adaptive_corner.qfem"));
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double w = global(*r.solution, QStringLiteral("es.W"));
            const int n = int(r.solution->mesh().triangles.size());
            // (A sanitized build: the fine mesh's energy as an optimized one
            // finds it, 104,215 triangles.)
            double ref = FineCornerEnergy;
            int nf = 104215;
            if (!slowBuild()) {
                Model fine = m;
                fine.find(fine.mesh().tag)->props.insert(QStringLiteral("hmax"), QStringLiteral("0.01"));
                for (Node& step : fine.find(fine.studies().front()->tag)->children) step.props.insert(QStringLiteral("adaptive"), false);
                const Run f = run(fine);
                QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
                ref = global(*f.solution, QStringLiteral("es.W"));
                nf = int(f.solution->mesh().triangles.size());
                QVERIFY(std::abs(ref / FineCornerEnergy - 1) < 5e-4);   // (another platform's mesh a little different)
            }
            qInfo("corner: W %.6e on %d triangles, %.6e on %d; %s; %lld ms", w, n, ref, nf,
                  qPrintable(r.solution->log.filter(QStringLiteral("Adaptive")).join(QString())), timer.elapsed());
            QVERIFY2(std::abs(w / ref - 1) < 2e-3, qPrintable(QString::number(w / ref - 1)));
            QVERIFY(n < nf / 20);
        }
    }
};

QTEST_GUILESS_MAIN(TestFemWaves)
#include "test_fem_waves.moc"
