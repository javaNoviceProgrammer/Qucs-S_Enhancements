/*
 * test_fem_waves.cpp - the multiphysics solver's waves against what is
 *                      known: the eigensolver, a rectangular waveguide's
 *                      modes, a dielectric slab's (its transcendental
 *                      equation), a coax's TEM mode, a lossy filling, a
 *                      thermo-optic shift; waves in the plane - a
 *                      parallel-plate guide's S-parameters, an iris's power
 *                      kept, a PML that does not reflect; a periodic cell's
 *                      reflectance and transmittance against Fresnel, thin
 *                      films' characteristic matrices and a grating's coupled
 *                      waves; adaptive meshes; the examples
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
#include <QJsonArray>
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

/// A unit cell \a width µm wide: layers from the bottom up (thickness in
/// µm, its material's properties: "\"n\": \"1.5\""), its sides periodic,
/// a periodic port fed at its top at \a angle (degrees), another at its
/// bottom; at the wavelength \a lambda (µm). \a extra: geometry, materials
/// and features added (JSON array items, each list led by a comma), and
/// mesh properties.
struct Extra {
    QString geometry, materials, features, mesh;
};
QByteArray cell(double width, const std::vector<std::pair<double, QString>>& layers, const QString& pol, double angle, double lambda,
                double hmax, const Extra& extra = {})
{
    QStringList geometry, materials, names;
    double y = 0;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const QString name = QStringLiteral("L%1").arg(i);
        names << QStringLiteral("\"%1\"").arg(name);
        geometry << QStringLiteral(R"({"type": "rectangle", "name": "%1", "position": ["0", "%2"], "size": ["%3", "%4"]})")
                        .arg(name)
                        .arg(y, 0, 'g', 17)
                        .arg(width, 0, 'g', 17)
                        .arg(layers[i].first, 0, 'g', 17);
        materials << QStringLiteral(R"({"label": "m%1", %2, "selection": {"only": ["%1"]}})").arg(name, layers[i].second);
        y += layers[i].first;
    }
    const QString top = QStringLiteral("L%1").arg(layers.size() - 1);
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [%1 %2],
        "materials": [%3 %4],
        "physics": [{"type": "waves", "tag": "ewfd", "polarization": "%5", "features": [
          {"type": "periodic", "selection": {"of": [%6], "side": ["left", "right"]}},
          {"type": "periodicport", "name": "in", "angle": "%7", "selection": {"of": ["%8"], "side": "top"}},
          {"type": "periodicport", "name": "out", "excite": false, "selection": {"of": ["L0"], "side": "bottom"}} %9]}],
        "mesh": {"size": "fine", "custom": true, "hmax": "%10" %11}}],
      "studies": [{"type": "study", "steps": [{"type": "frequency", "frequencies": "%12[THz]"}]}]})json")
        .arg(geometry.join(QLatin1String(", ")), extra.geometry, materials.join(QLatin1String(", ")), extra.materials, pol,
             names.join(QLatin1String(", ")))
        .arg(angle, 0, 'g', 17)
        .arg(top, extra.features)
        .arg(hmax, 0, 'g', 17)
        .arg(extra.mesh)
        .arg(C0 / (lambda * 1e-6) / 1e12, 0, 'g', 17)
        .toUtf8();
}

/// A film stack's R, T and A by Macleod's characteristic matrices and
/// tilted admittances (n - jκ): \a layers (index, thickness in µm) from the
/// incident side, between a lossless \a n0 and a substrate \a ns; TE or TM
/// at \a deg in n0, at \a lambda µm.
struct Rta {
    double R = 0, T = 0, A = 0;
};
Rta films(double n0, const std::vector<std::pair<cd, double>>& layers, cd ns, bool tm, double deg, double lambda)
{
    const double s0 = n0 * std::sin(deg * M_PI / 180);
    // n cosθ in each: its imaginary part not above 0 (a wave dying away).
    auto ncos = [s0](cd n) {
        cd c = n * std::sqrt(1.0 - (s0 / n) * (s0 / n));
        if (c.imag() > 1e-15 || (std::abs(c.imag()) <= 1e-15 && c.real() < 0)) c = -c;
        return c;
    };
    auto eta = [&](cd n) { return tm ? n * n / ncos(n) : ncos(n); };
    Eigen::Matrix2cd M = Eigen::Matrix2cd::Identity();
    for (const auto& [n, d] : layers) {
        const cd delta = 2 * M_PI * ncos(n) * d / lambda;
        const cd e = eta(n);
        Eigen::Matrix2cd Mj;
        Mj << std::cos(delta), cd(0, 1) * std::sin(delta) / e, cd(0, 1) * e * std::sin(delta), std::cos(delta);
        M = M * Mj;
    }
    const Eigen::Vector2cd bc = M * Eigen::Vector2cd(1, eta(ns));
    const cd e0 = eta(n0), den = e0 * bc(0) + bc(1);
    Rta out;
    out.R = std::norm((e0 * bc(0) - bc(1)) / den);
    out.T = 4 * e0.real() * eta(ns).real() / std::norm(den);
    out.A = 1 - out.R - out.T;
    return out;
}

/// A lamellar grating's diffraction efficiencies by rigorous coupled waves,
/// TE (Moharam, Grann, Pommet and Gaylord, JOSA A 12, 1068, 1995): from n1
/// at \a deg onto a layer \a depth µm deep - a ridge of \a nr a fraction \a
/// fill of the period \a period µm, the rest \a ng - over \a n3; 2N + 1
/// orders. Order m (along +x, as the cell's) at index m + N.
struct Efficiencies {
    std::vector<double> R, T;
};
Efficiencies rcwaTE(double n1, double nr, double ng, double n3, double period, double fill, double depth, double deg, double lambda, int N)
{
    const int n = 2 * N + 1;
    const double k0 = 2 * M_PI / lambda, K = 2 * M_PI / period, th = deg * M_PI / 180;
    // (Moharam's order i: k_xi = k0 n1 sinθ - i K; the cell's m = -i.)
    Eigen::VectorXcd kx(n), z1(n), z3(n);
    auto kz = [](double nn, cd k) {
        const cd v = nn * nn - k * k;
        return v.real() >= 0 ? std::sqrt(v) : cd(0, -1) * std::sqrt(-v);
    };
    for (int i = -N; i <= N; ++i) {
        kx(i + N) = n1 * std::sin(th) - double(i) * K / k0;
        z1(i + N) = kz(n1, kx(i + N));
        z3(i + N) = kz(n3, kx(i + N));
    }
    // ε(x) = Σ ε_h e^{jhKx}, the ridge from 0 to fill: ε_h = (1/Λ) ∫ ε e^{-jhKx}.
    auto epsh = [&](int h) -> cd {
        if (h == 0) return ng * ng + (nr * nr - ng * ng) * fill;
        const double a = 2 * M_PI * h * fill;
        return (nr * nr - ng * ng) * (std::exp(cd(0, -a)) - 1.0) / cd(0, -2 * M_PI * h);
    };
    Eigen::MatrixXcd A(n, n);
    for (int i = 0; i < n; ++i)
        for (int p = 0; p < n; ++p) A(i, p) = (i == p ? kx(i) * kx(i) : cd(0)) - epsh(i - p);
    Eigen::ComplexEigenSolver<Eigen::MatrixXcd> es(A);
    const Eigen::MatrixXcd W = es.eigenvectors();
    Eigen::VectorXcd q(n);
    for (int m = 0; m < n; ++m) {
        q(m) = std::sqrt(es.eigenvalues()(m));
        if (q(m).real() < 0) q(m) = -q(m);
    }
    const Eigen::MatrixXcd V = W * q.asDiagonal();
    Eigen::VectorXcd x(n);
    for (int m = 0; m < n; ++m) x(m) = std::exp(-k0 * q(m) * depth);
    const Eigen::MatrixXcd X = x.asDiagonal(), I = Eigen::MatrixXcd::Identity(n, n);
    const cd J(0, 1);
    // [R; c+; c-; T]: δ + R = W c+ + W X c-; j n1 cosθ δ - j Y1 R = V c+ - V X c-;
    // W X c+ + W c- = T; V X c+ - V c- = j Y3 T.
    Eigen::MatrixXcd M = Eigen::MatrixXcd::Zero(4 * n, 4 * n);
    Eigen::VectorXcd b = Eigen::VectorXcd::Zero(4 * n);
    M.block(0, 0, n, n) = -I;
    M.block(0, n, n, n) = W;
    M.block(0, 2 * n, n, n) = W * X;
    M.block(n, 0, n, n) = J * Eigen::MatrixXcd(z1.asDiagonal());
    M.block(n, n, n, n) = V;
    M.block(n, 2 * n, n, n) = -V * X;
    M.block(2 * n, n, n, n) = W * X;
    M.block(2 * n, 2 * n, n, n) = W;
    M.block(2 * n, 3 * n, n, n) = -I;
    M.block(3 * n, n, n, n) = V * X;
    M.block(3 * n, 2 * n, n, n) = -V;
    M.block(3 * n, 3 * n, n, n) = -J * Eigen::MatrixXcd(z3.asDiagonal());
    b(N) = 1;
    b(n + N) = J * n1 * std::cos(th);
    const Eigen::VectorXcd u = M.fullPivLu().solve(b);
    Efficiencies out;
    out.R.assign(std::size_t(n), 0);
    out.T.assign(std::size_t(n), 0);
    for (int i = -N; i <= N; ++i) {
        const int m = -i;
        out.R[std::size_t(m + N)] = std::norm(u(i + N)) * (z1(i + N) / (n1 * std::cos(th))).real();
        out.T[std::size_t(m + N)] = std::norm(u(3 * n + i + N)) * (z3(i + N) / (n1 * std::cos(th))).real();
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

/// A symmetric slab's TM0: tan(κ d/2) = (n1/n2)² γ/κ.
double slabTM0(double n1, double n2, double d, double lambda)
{
    const double k0 = 2 * M_PI / lambda;
    double lo = n2 + 1e-9, hi = n1 - 1e-9;
    for (int i = 0; i < 200; ++i) {
        const double mid = (lo + hi) / 2;
        const double kappa = k0 * std::sqrt(n1 * n1 - mid * mid), gamma = k0 * std::sqrt(mid * mid - n2 * n2);
        if (kappa * d / 2 >= M_PI / 2 || std::tan(kappa * d / 2) - (n1 * n1) / (n2 * n2) * gamma / kappa > 0) lo = mid;
        else hi = mid;
    }
    return (lo + hi) / 2;
}

/// A silicon slab 220 nm thick (its index \a core) in oxide across a window
/// \a width µm wide, 1.5 µm of oxide above and below: its walls \a sides
/// (none: metal), its mode step's more \a step; its core's confinement
/// factors g1 (|E|²), g2 (Sz) and g3 (the index's).
QByteArray slabWindow(const QString& sides, const QString& step, const QString& core, const QString& width = QStringLiteral("2"))
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "oxide", "position": ["0", "-1.5"], "size": ["%6", "3.22"]},
                     {"type": "rectangle", "name": "core", "size": ["%6", "0.22"]}],
        "materials": [{"label": "oxide", "n": "1.444", "selection": {"only": ["oxide"]}},
                      {"label": "silicon", "n": "%3", "selection": {"only": ["core"]}}],
        "physics": [{"type": "modes", "tag": "emw", "features": [%1]}],
        "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "%4"},
                                              {"type": "size", "level": "domain", "selection": {"only": ["oxide"]}, "hmax": "%5"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "3" %2}]}],
      "results": {"derived": [{"type": "confinement", "tag": "g1", "selection": {"only": ["core"]}},
                              {"type": "confinement", "tag": "g2", "selection": {"only": ["core"]}, "kind": "power"},
                              {"type": "confinement", "tag": "g3", "selection": {"only": ["core"]}, "kind": "index"}]}})json")
        .arg(sides, step, core, slowBuild() ? QStringLiteral("0.055") : QStringLiteral("0.02"), slowBuild() ? QStringLiteral("0.3") : QStringLiteral("0.08"), width)
        .toUtf8();
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

    // A silicon slab 220 nm thick in oxide across a window 2 µm wide, its
    // sides magnetic walls: a TE mode varying across the window (E along x)
    // outranks the slab's TM0 - the one found about the index; asked for
    // TM, the slab's TM0, its equation's, emw.TEfraction nought. 4 µm wide
    // (Release builds only: a sanitized one's mode solves take a minute):
    // some ten lateral TE modes above TM0, more than the first search finds
    // - sought until there is a TM one.
    void modesByPolarization()
    {
        const QString pmc = QStringLiteral(R"({"type": "pmc", "selection": {"of": ["oxide", "core"], "side": "left"}},
                                              {"type": "pmc", "selection": {"of": ["oxide", "core"], "side": "right"}})");
        const Run any = run(modelOf(slabWindow(pmc, QString(), QStringLiteral("3.476"))));
        const Run tm = run(modelOf(slabWindow(pmc, QStringLiteral(R"(, "polarization": "tm")"), QStringLiteral("3.476"))));
        QVERIFY2(any.error.isEmpty() && tm.error.isEmpty(), qPrintable(any.error + tm.error));
        any.solution->select(0);
        tm.solution->select(0);
        const double nAny = global(*any.solution, QStringLiteral("emw.neff")), teAny = global(*any.solution, QStringLiteral("emw.TEfraction"));
        const double nTM = global(*tm.solution, QStringLiteral("emw.neff")), teTM = global(*tm.solution, QStringLiteral("emw.TEfraction"));
        const double tm0 = slabTM0(3.476, 1.444, 0.22e-6, 1.55e-6);
        qInfo("magnetic sides: about n 3 a TE mode %.5f (TE fraction %.4f); asked for TM %.6f (%.4f), the slab's TM0 %.6f", nAny, teAny, nTM, teTM, tm0);
        QVERIFY(teAny > 0.99 && nAny > tm0 + 0.5);
        QVERIFY(teTM < 0.01);
        QVERIFY2(std::abs(nTM - tm0) < (slowBuild() ? 3e-4 : 1e-4), qPrintable(QString::number(nTM - tm0)));
        QVERIFY(tm.solution->log.join(QLatin1Char(' ')).contains(QLatin1String("TE modes left out")));
        if (slowBuild()) return;
        const Run wide = run(modelOf(slabWindow(pmc, QStringLiteral(R"(, "polarization": "tm")"), QStringLiteral("3.476"), QStringLiteral("4"))));
        QVERIFY2(wide.error.isEmpty(), qPrintable(wide.error));
        wide.solution->select(0);
        const double nWide = global(*wide.solution, QStringLiteral("emw.neff"));
        qInfo("4 µm wide, asked for TM: %.6f (TE fraction %.4f)", nWide, global(*wide.solution, QStringLiteral("emw.TEfraction")));
        QVERIFY(global(*wide.solution, QStringLiteral("emw.TEfraction")) < 0.01);
        QVERIFY2(std::abs(nWide - tm0) < 1e-4, qPrintable(QString::number(nWide - tm0)));
    }

    // The slab's window with metal sides, TE asked: the TE0 slab mode (E
    // along x meets them at right angles) and its confinement factors -
    // - |E|² in the core: (a + sin 2κa/2κ)/(a + sin 2κa/2κ + cos²κa/γ), the
    //   slab's (a half its thickness);
    // - Sz's share the same (a TE mode's Sz is |Ex|²'s);
    // - the index's, (c ε0/2) ∫ n |E|²/∫ Sz, n_core/neff times that, and
    //   (Release builds) what neff moves for the core's index moved by 1e-3.
    void confinementFactors()
    {
        const Model te = modelOf(slabWindow(QString(), QStringLiteral(R"(, "polarization": "te")"), QStringLiteral("3.476")));
        const Run r = run(te);
        const Run moved = slowBuild() ? Run() : run(modelOf(slabWindow(QString(), QStringLiteral(R"(, "polarization": "te")"), QStringLiteral("3.477"))));
        QVERIFY2(r.error.isEmpty() && moved.error.isEmpty(), qPrintable(r.error + moved.error));
        r.solution->select(0);
        if (moved.solution) moved.solution->select(0);
        const double neff = global(*r.solution, QStringLiteral("emw.neff")), te0 = slabTE0(3.476, 1.444, 0.22e-6, 1.55e-6);
        auto derived = [&](const char* tag) {
            const DerivedResult d = evaluateDerived(*r.solution, *te.find(QString::fromLatin1(tag)));
            return d.values.isEmpty() ? NAN : d.values.front().value;
        };
        const double g1 = derived("g1"), g2 = derived("g2"), g3 = derived("g3");
        const double k0 = 2 * M_PI / 1.55e-6, a = 0.11e-6;
        const double kappa = k0 * std::sqrt(3.476 * 3.476 - te0 * te0), gamma = k0 * std::sqrt(te0 * te0 - 1.444 * 1.444);
        const double inCore = a + std::sin(2 * kappa * a) / (2 * kappa), exact = inCore / (inCore + std::pow(std::cos(kappa * a), 2) / gamma);
        const double slope = moved.solution ? (global(*moved.solution, QStringLiteral("emw.neff")) - neff) / 1e-3 : NAN;
        qInfo("TE0 %.6f (the slab's %.6f), TE fraction %.5f; Γ %.5f (the slab's %.5f), of Sz %.5f, the index's %.5f (n/neff Γ %.5f, dneff/dn %.5f)", neff,
              te0, global(*r.solution, QStringLiteral("emw.TEfraction")), g1, exact, g2, g3, 3.476 / neff * g1, slope);
        QVERIFY2(std::abs(neff - te0) < (slowBuild() ? 1e-4 : 3e-5), qPrintable(QString::number(neff - te0)));
        QVERIFY(global(*r.solution, QStringLiteral("emw.TEfraction")) > 0.999);
        QVERIFY2(std::abs(g1 / exact - 1) < 1e-3, qPrintable(QString::number(g1 / exact)));
        QVERIFY2(std::abs(g2 / g1 - 1) < 1e-3, qPrintable(QString::number(g2 / g1)));
        QVERIFY2(std::abs(g3 / (3.476 / neff * g1) - 1) < 1e-3, qPrintable(QString::number(g3 / (3.476 / neff * g1))));
        if (moved.solution) QVERIFY2(std::abs(slope / g3 - 1) < 3e-3, qPrintable(QString::number(slope / g3)));
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

    void modeStepSolvesWhatItReads()
    {
        // A core heated, its oxide's k(T) nonlinear, the mode thermo-optic:
        // a mode step that solves the heat too takes Newton's method to the
        // end (a step without a Maximum iterations had one Newton step) - the
        // same as a stationary step before it; asked for one step, it fails.
        auto model = [](const QString& steps) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "oxide", "position": ["-1", "-1.5"], "size": ["2.4", "3.22"]},
                             {"type": "rectangle", "name": "core", "size": ["0.4", "0.22"]}],
                "materials": [{"label": "oxide", "n": "1.444", "k": "1.4[W/(m*K)]*(T/293.15[K])^3", "selection": {"only": ["oxide"]}},
                              {"label": "silicon", "n": "3.476", "dndT": "1.86e-4[1/K]", "k": "1.4[W/(m*K)]*(T/293.15[K])^3", "selection": {"only": ["core"]}}],
                "physics": [{"type": "heat", "tag": "ht", "features": [
                               {"type": "source", "Q0": "3e15[W/m^3]", "selection": {"only": ["core"]}},
                               {"type": "temperature", "T0": "293.15[K]", "selection": {"exterior": true}}]},
                            {"type": "modes", "tag": "emw", "features": [
                               {"type": "wave", "thermooptic": true, "T": "ht.T", "selection": {"only": ["core"]}}]}],
                "mesh": {"size": "normal", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "0.04"}]}}],
              "studies": [{"type": "study", "steps": [%1]}]})json")
                .arg(steps)
                .toUtf8();
        };
        const QString mode = QStringLiteral(R"x({"type": "mode", "physics": ["ht", "emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3" %1})x");
        const Run inOne = run(modelOf(model(mode.arg(QString()))));
        QVERIFY2(inOne.error.isEmpty(), qPrintable(inOne.error));
        const Run before = run(modelOf(model(QStringLiteral(R"x({"type": "stationary", "physics": ["ht"], "maxiter": 100},
            {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3"})x"))));
        QVERIFY2(before.error.isEmpty(), qPrintable(before.error));
        inOne.solution->select(0);
        before.solution->select(0);
        const double t1 = global(*inOne.solution, QStringLiteral("ht.Tmax")), t2 = global(*before.solution, QStringLiteral("ht.Tmax"));
        const double n1 = global(*inOne.solution, QStringLiteral("emw.neff")), n2 = global(*before.solution, QStringLiteral("emw.neff"));
        qInfo("a mode step's heat: Tmax %.6f K (%.6f a step before), neff %.9f (%.9f); %s", t1, t2, n1, n2,
              qPrintable(inOne.solution->log.filter(QStringLiteral("Newton")).join(QLatin1String("; "))));
        QVERIFY(t1 > 330);
        QVERIFY(std::abs(t1 - t2) < 1e-6 && std::abs(n1 - n2) < 1e-9);
        QVERIFY(inOne.solution->log.filter(QStringLiteral("did not converge")).isEmpty());
        QVERIFY(inOne.study.warnings.isEmpty());
        // One Newton step asked for: the step fails, saying why.
        const Run one = run(modelOf(model(mode.arg(QStringLiteral(R"(, "maxiter": 1)")))));
        QVERIFY(!one.error.isEmpty());
        QVERIFY2(one.error.contains(QLatin1String("did not converge")) && one.error.contains(QLatin1String("Maximum iterations")), qPrintable(one.error));
        // A stationary step's: its study's warnings say it (not the log alone).
        const Run rest = run(modelOf(model(QStringLiteral(R"x({"type": "stationary", "physics": ["ht"], "maxiter": 1})x"))));
        QVERIFY2(rest.error.isEmpty(), qPrintable(rest.error));
        QVERIFY2(rest.study.warnings.join(QLatin1String("; ")).contains(QLatin1String("did not converge")),
                 qPrintable(rest.study.warnings.join(QLatin1String("; "))));
    }

    void trackingAMode()
    {
        // A wide block and a narrow wire of silicon 2 µm apart: about n 3
        // the block's mode comes first; tracking the wire, the wire's - its
        // effective index the wire's alone, its share of |E|² emw.Gamma.
        auto model = [](bool block, const QString& track) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "oxide", "position": ["-2", "-1.5"], "size": ["5.8", "3.22"]},
                             {"type": "rectangle", "name": "wire", "size": ["0.4", "0.22"]} %1],
                "materials": [{"label": "oxide", "n": "1.444", "selection": {"all": true}},
                              {"label": "silicon", "n": "3.476", "selection": {"objects": [%4]}}],
                "physics": [{"type": "modes", "tag": "emw"}],
                "mesh": {"size": "%5", "features": [{"type": "size", "level": "domain", "selection": {"objects": [%4]}, "hmax": "%3"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "3" %2}]}]})json")
                .arg(block ? QStringLiteral(R"(, {"type": "rectangle", "name": "block", "position": ["2.4", "0"], "size": ["1", "0.22"]})") : QString(),
                     track, slowBuild() ? QStringLiteral("0.08") : QStringLiteral("0.04"),
                     block ? QStringLiteral(R"("wire", "block")") : QStringLiteral(R"("wire")"),
                     slowBuild() ? QStringLiteral("coarser") : QStringLiteral("normal"))
                .toUtf8();
        };
        const Run plain = run(modelOf(model(true, QString())));
        const Run tracked = run(modelOf(model(true, QStringLiteral(R"(, "track": {"only": ["wire"]})"))));
        const Run alone = run(modelOf(model(false, QString())));
        QVERIFY2(plain.error.isEmpty() && tracked.error.isEmpty() && alone.error.isEmpty(), qPrintable(plain.error + tracked.error + alone.error));
        for (const Run* r : {&plain, &tracked, &alone}) r->solution->select(0);
        const double np = global(*plain.solution, QStringLiteral("emw.neff")), nt = global(*tracked.solution, QStringLiteral("emw.neff"));
        const double na = global(*alone.solution, QStringLiteral("emw.neff")), gamma = global(*tracked.solution, QStringLiteral("emw.Gamma"));
        qInfo("tracking: about n 3 the block's mode %.6f; tracking the wire %.6f (the wire alone %.6f), its share %.3f", np, nt, na, gamma);
        QVERIFY(np > nt + 0.05);
        QVERIFY(std::abs(nt - na) < 2e-4);
        QVERIFY(gamma > 0.5 && gamma < 1);
        QVERIFY(!plain.solution->global(QStringLiteral("emw.Gamma")));
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
            // Reflectance and transmittance: port 1's, |S11|² and |S21|²; nothing absorbed.
            QVERIFY(std::abs(global(s, QStringLiteral("ewfd.R")) - a11 * a11) < 1e-12);
            QVERIFY(std::abs(global(s, QStringLiteral("ewfd.T")) - a21 * a21) < 1e-12);
            QVERIFY(std::abs(global(s, QStringLiteral("ewfd.A"))) < 1e-3);
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

    // ---- Periodic cells: Floquet's condition, periodic ports, R and T

    void periodicFresnel()
    {
        // Air over glass in a cell 0.4 µm wide at λ 1 µm (the zeroth order
        // alone propagates): Fresnel's R and T, TE (Ez) and TM (Hz), and the
        // zeroth orders' fields at the ports' lines - the interface 1 µm
        // from each: r e^-2jβ1 d, t e^-j(β1 + β2) d (t = 1 + r: TE's E, TM's H).
        const double n1 = 1, n2 = 1.5, lambda = 1, d = 1e-6, k0 = 2 * M_PI / (lambda * 1e-6);
        // (A sanitized build: a mesh twice as coarse, its errors some ten
        // times larger - t_0's 6.4e-5.)
        const bool slow = slowBuild();
        const double hmax = slow ? 0.04 : 0.02, tolR = slow ? 5e-6 : 1e-6, tolr = slow ? 2e-5 : 1e-6, tolt = slow ? 1e-4 : 1e-5;
        for (const QString& pol : {QStringLiteral("ez"), QStringLiteral("hz")})
            for (double deg : {0.0, 30.0, 60.0}) {
                const Run r = run(modelOf(cell(0.4, {{1.0, QStringLiteral(R"("n": "1.5")")}, {1.0, QStringLiteral(R"("n": "1")")}}, pol, deg, lambda, hmax)));
                QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                Solution& s = *r.solution;
                const double ti = deg * M_PI / 180, tt = std::asin(n1 * std::sin(ti) / n2);
                const double c1 = std::cos(ti), c2 = std::cos(tt);
                const cd rf = pol == QLatin1String("ez") ? (n1 * c1 - n2 * c2) / (n1 * c1 + n2 * c2) : (n2 * c1 - n1 * c2) / (n2 * c1 + n1 * c2);
                const double Tf = pol == QLatin1String("ez") ? std::norm(1.0 + rf) * n2 * c2 / (n1 * c1)
                                                             : std::norm(1.0 + rf) * (c2 / n2) / (c1 / n1);
                const cd r0e = rf * std::exp(cd(0, -2 * k0 * n1 * c1 * d));
                const cd t0e = (1.0 + rf) * std::exp(cd(0, -k0 * (n1 * c1 + n2 * c2) * d));
                const double R = global(s, QStringLiteral("ewfd.R")), T = global(s, QStringLiteral("ewfd.T"));
                const cd r0(global(s, QStringLiteral("ewfd.r_0")), global(s, QStringLiteral("ewfd.r_0_im")));
                const cd t0(global(s, QStringLiteral("ewfd.t_0")), global(s, QStringLiteral("ewfd.t_0_im")));
                qInfo("%s at %g°: R %.7f (Fresnel %.7f), T %.7f (%.7f), |r0 - r| %.1e, |t0 - t| %.1e", qPrintable(pol), deg, R, std::norm(rf), T, Tf,
                      std::abs(r0 - r0e), std::abs(t0 - t0e));
                QVERIFY2(std::abs(R - std::norm(rf)) < tolR, qPrintable(QStringLiteral("%1 %2°: R %3").arg(pol).arg(deg).arg(R)));
                QVERIFY2(std::abs(T - Tf) < tolR, qPrintable(QStringLiteral("%1 %2°: T %3").arg(pol).arg(deg).arg(T)));
                QVERIFY(std::abs(global(s, QStringLiteral("ewfd.A"))) < 1e-9);
                // (t's phase over 2 µm, one in glass: some 2.5e-6 off on this mesh)
                QVERIFY2(std::abs(r0 - r0e) < tolr && std::abs(t0 - t0e) < tolt,
                         qPrintable(QStringLiteral("%1 %2").arg(std::abs(r0 - r0e)).arg(std::abs(t0 - t0e))));
                QCOMPARE(global(s, QStringLiteral("ewfd.R_0")), R);
                QCOMPARE(global(s, QStringLiteral("ewfd.T_0")), T);
                QVERIFY(std::isnan(global(s, QStringLiteral("ewfd.R_1"))));   // (none propagates)
                QVERIFY(std::abs(global(s, QStringLiteral("ewfd.kF")) - k0 * n1 * std::sin(ti)) < 1e-6 * k0);
                // The orders that propagate, and their angles, said.
                QVERIFY2(s.log.join(QLatin1Char('\n')).contains(QStringLiteral("out: 0 (%1°)").arg(tt * 180 / M_PI, 0, 'f', 2)),
                         qPrintable(s.log.join(QLatin1Char('\n'))));
            }
    }

    void periodicThinFilms()
    {
        // A film 0.3 µm thick of n 2 on glass (n 1.5), from air at λ 1 µm -
        // and the film absorbing (n 2 - 0.1j): R, T and A against the
        // characteristic matrices, TE and TM, at 0° and 45°.
        for (const bool lossy : {false, true})
            for (const QString& pol : {QStringLiteral("ez"), QStringLiteral("hz")})
                for (double deg : {0.0, 45.0}) {
                    const QString film = lossy ? QStringLiteral(R"("n": "2", "kappa": "0.1")") : QStringLiteral(R"("n": "2")");
                    // (A sanitized build: twice as coarse - 2.1e-5 off.)
                    const double tol = slowBuild() ? 5e-5 : 2e-6;
                    const Run r = run(modelOf(cell(0.4, {{0.8, QStringLiteral(R"("n": "1.5")")}, {0.3, film}, {0.8, QStringLiteral(R"("n": "1")")}}, pol, deg, 1.0,
                                                   slowBuild() ? 0.04 : 0.02)));
                    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                    Solution& s = *r.solution;
                    const Rta e = films(1.0, {{cd(2, lossy ? -0.1 : 0), 0.3}}, 1.5, pol == QLatin1String("hz"), deg, 1.0);
                    const double R = global(s, QStringLiteral("ewfd.R")), T = global(s, QStringLiteral("ewfd.T")), A = global(s, QStringLiteral("ewfd.A"));
                    qInfo("%s film, %s at %g°: R %.6f (%.6f), T %.6f (%.6f), A %.6f (%.6f)", lossy ? "absorbing" : "clear", qPrintable(pol), deg, R, e.R, T, e.T, A, e.A);
                    QVERIFY2(std::abs(R - e.R) < tol && std::abs(T - e.T) < tol && std::abs(A - e.A) < tol,
                             qPrintable(QStringLiteral("%1 %2° %3: R %4 T %5 A %6").arg(pol).arg(deg).arg(lossy).arg(R).arg(T).arg(A)));
                    if (!lossy) QVERIFY(std::abs(A) < 1e-9);
                }
    }

    void gratingOrders()
    {
        // A lamellar grating (ridges of n 1.5, half the period of 1 µm, 0.3 µm
        // deep, on glass) lit from air at 10°, λ 0.6328 µm, TE: three
        // reflected orders (-1, 0, 1) and five transmitted (-2 to 2), each
        // against rigorous coupled waves (81 orders); none lost.
        const double lambda = 0.6328, deg = 10;
        // (the coupled waves, a homogeneous layer: the characteristic matrix's)
        const Efficiencies flat = rcwaTE(1.0, 1.5, 1.5, 1.5, 1.0, 0.5, 0.3, deg, lambda, 10);
        QVERIFY(std::abs(flat.R[10] - films(1.0, {}, 1.5, false, deg, lambda).R) < 1e-9);
        const Efficiencies ref = rcwaTE(1.0, 1.5, 1.0, 1.5, 1.0, 0.5, 0.3, deg, lambda, 40);
        Extra ridge;
        ridge.geometry = QStringLiteral(R"(, {"type": "rectangle", "name": "ridge", "position": ["0", "0.6"], "size": ["0.5", "0.3"]})");
        ridge.materials = QStringLiteral(R"(, {"label": "mridge", "n": "1.5", "selection": {"objects": ["ridge"]}})");
        const Run r = run(modelOf(cell(1.0, {{0.6, QStringLiteral(R"("n": "1.5")")}, {0.3, QStringLiteral(R"("n": "1")")}, {0.8, QStringLiteral(R"("n": "1")")}},
                                       QStringLiteral("ez"), deg, lambda, slowBuild() ? 0.03 : 0.015, ridge)));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        const auto order = [](int m) { return m < 0 ? QStringLiteral("n%1").arg(-m) : QString::number(m); };
        double sum = 0;
        int reflected = 0, transmitted = 0;
        for (int m = -3; m <= 3; ++m) {
            const double R = global(s, QStringLiteral("ewfd.R_") + order(m)), T = global(s, QStringLiteral("ewfd.T_") + order(m));
            const double Rr = ref.R[std::size_t(m + 40)], Tr = ref.T[std::size_t(m + 40)];
            qInfo("order %+d: R %.6f (coupled waves %.6f), T %.6f (%.6f)", m, R, Rr, T, Tr);
            QCOMPARE(std::isnan(R), Rr == 0);
            QCOMPARE(std::isnan(T), Tr == 0);
            if (!std::isnan(R)) {
                ++reflected;
                sum += R;
                QVERIFY2(std::abs(R - Rr) < 5e-4, qPrintable(QStringLiteral("R %1: %2 against %3").arg(m).arg(R).arg(Rr)));
            }
            if (!std::isnan(T)) {
                ++transmitted;
                sum += T;
                QVERIFY2(std::abs(T - Tr) < 5e-4, qPrintable(QStringLiteral("T %1: %2 against %3").arg(m).arg(T).arg(Tr)));
            }
        }
        QCOMPARE(reflected, 3);
        QCOMPARE(transmitted, 5);
        QVERIFY2(std::abs(sum - 1) < 1e-9 && std::abs(global(s, QStringLiteral("ewfd.A"))) < 1e-9, qPrintable(QString::number(sum - 1)));
        QVERIFY(std::abs(global(s, QStringLiteral("ewfd.R")) + global(s, QStringLiteral("ewfd.T")) - 1) < 1e-9);
        // The ports 50 nm from it, in its near field: each order the same -
        // the orders that die away taken out by the ports as well.
        Extra close = ridge;
        close.geometry = QStringLiteral(R"(, {"type": "rectangle", "name": "ridge", "position": ["0", "0.05"], "size": ["0.5", "0.3"]})");
        const Run c = run(modelOf(cell(1.0, {{0.05, QStringLiteral(R"("n": "1.5")")}, {0.3, QStringLiteral(R"("n": "1")")}, {0.05, QStringLiteral(R"("n": "1")")}},
                                       QStringLiteral("ez"), deg, lambda, slowBuild() ? 0.02 : 0.01, close)));
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        for (int m = -2; m <= 2; ++m) {
            const double T = global(*c.solution, QStringLiteral("ewfd.T_") + order(m)), Tr = ref.T[std::size_t(m + 40)];
            QVERIFY2(std::abs(T - Tr) < 5e-4, qPrintable(QStringLiteral("near: T %1: %2 against %3").arg(m).arg(T).arg(Tr)));
        }
        for (int m = -1; m <= 1; ++m) {
            const double R = global(*c.solution, QStringLiteral("ewfd.R_") + order(m)), Rr = ref.R[std::size_t(m + 40)];
            QVERIFY2(std::abs(R - Rr) < 5e-4, qPrintable(QStringLiteral("near: R %1: %2 against %3").arg(m).arg(R).arg(Rr)));
        }
    }

    void periodicSidesMeshedAlike()
    {
        // A cell whose left wall has a point of its own (a block in the glass
        // against it): its two walls still meshed node for node, and Fresnel's
        // R as before - no warning of an interpolation.
        Extra block;
        block.geometry = QStringLiteral(R"(, {"type": "rectangle", "name": "block", "position": ["0", "0.37"], "size": ["0.11", "0.23"]})");
        block.materials = QStringLiteral(R"(, {"label": "mblock", "n": "1.5", "selection": {"objects": ["block"]}})");
        const Run r = run(modelOf(cell(0.4, {{1.0, QStringLiteral(R"("n": "1.5")")}, {1.0, QStringLiteral(R"("n": "1")")}}, QStringLiteral("hz"), 40, 1.0, 0.03, block)));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        const QString log = s.log.join(QLatin1Char('\n'));
        QVERIFY2(!log.contains(QLatin1String("meshed apart")), qPrintable(log));
        const double ti = 40 * M_PI / 180, tt = std::asin(std::sin(ti) / 1.5);
        const double rp = (1.5 * std::cos(ti) - std::cos(tt)) / (1.5 * std::cos(ti) + std::cos(tt));
        QVERIFY2(std::abs(global(s, QStringLiteral("ewfd.R")) - rp * rp) < 1e-6, qPrintable(QString::number(global(s, QStringLiteral("ewfd.R")))));
        // The mesh's walls: each node on one, one on the other 0.4 µm across.
        const Mesh& mesh = s.mesh();
        std::vector<double> left, right;
        for (const QPointF& p : mesh.nodes) {
            if (std::abs(p.x()) < 1e-9) left.push_back(p.y());
            if (std::abs(p.x() - 0.4) < 1e-9) right.push_back(p.y());
        }
        std::sort(left.begin(), left.end());
        std::sort(right.begin(), right.end());
        QCOMPARE(left.size(), right.size());
        for (std::size_t i = 0; i < left.size(); ++i) QVERIFY(std::abs(left[i] - right[i]) < 1e-9);
    }

    void periodicRefusals()
    {
        auto error = [](const QByteArray& json) { return run(modelOf(json)).error; };
        const std::vector<std::pair<double, QString>> stack{{1.0, QStringLiteral(R"("n": "1.5")")}, {1.0, QStringLiteral(R"("n": "1")")}};
        // Two plane waves fed.
        QString why = error(cell(0.4, stack, QStringLiteral("ez"), 0, 1, 0.05).replace(R"("excite": false, )", ""));
        QVERIFY2(why.contains(QLatin1String("is fed already")), qPrintable(why));
        // None fed.
        why = error(cell(0.4, stack, QStringLiteral("ez"), 0, 1, 0.05).replace(R"("name": "in", )", R"("name": "in", "excite": false, )"));
        QVERIFY2(why.contains(QLatin1String("no periodic port is fed")), qPrintable(why));
        // The walls not one the other moved: one wall and the top, a run.
        why = error(cell(0.4, stack, QStringLiteral("ez"), 0, 1, 0.05).replace(R"("side": ["left", "right"])", R"("side": ["left", "top"])"));
        QVERIFY2(why.contains(QLatin1String("not two sides apart")), qPrintable(why));
        // Continuity with a plane wave at 30°: its kF disagrees.
        why = error(cell(0.4, stack, QStringLiteral("ez"), 30, 1, 0.05).replace(R"({"type": "periodic", )", R"({"type": "periodic", "floquet": "continuity", )"));
        QVERIFY2(why.contains(QLatin1String("Floquet, the fed periodic port's")), qPrintable(why));
        // ... a kF given, its own: accepted, the same R as the port's.
        const double kF = 2 * M_PI / 1e-6 * std::sin(30 * M_PI / 180);
        const Run given = run(modelOf(cell(0.4, stack, QStringLiteral("ez"), 30, 1, 0.05)
                                          .replace(R"({"type": "periodic", )", QStringLiteral(R"({"type": "periodic", "floquet": "given", "kx": "%1[1/m]", )").arg(kF, 0, 'g', 17).toUtf8())));
        const Run port = run(modelOf(cell(0.4, stack, QStringLiteral("ez"), 30, 1, 0.05)));
        QVERIFY2(given.error.isEmpty() && port.error.isEmpty(), qPrintable(given.error + port.error));
        QVERIFY(std::abs(global(*given.solution, QStringLiteral("ewfd.R")) - global(*port.solution, QStringLiteral("ewfd.R"))) < 1e-12);
        // No periodic condition.
        QByteArray noSides = cell(0.4, stack, QStringLiteral("ez"), 0, 1, 0.05);
        noSides.replace(R"({"type": "periodic", "selection": {"of": ["L0", "L1"], "side": ["left", "right"]}},)", "");
        why = error(noSides);
        QVERIFY2(why.contains(QLatin1String("needs a Periodic Condition")), qPrintable(why));
        // A port half the period long, and one over two media.
        Extra halves;
        halves.geometry = QStringLiteral(R"(, {"type": "rectangle", "name": "half", "position": ["0", "1"], "size": ["0.2", "1"]})");
        halves.materials = QStringLiteral(R"(, {"label": "mhalf", "n": "1.2", "selection": {"objects": ["half"]}})");
        why = error(cell(0.4, stack, QStringLiteral("ez"), 0, 1, 0.05, halves));
        QVERIFY2(why.contains(QLatin1String("not the same all along it")), qPrintable(why));
        QByteArray shortPort = cell(0.4, stack, QStringLiteral("ez"), 0, 1, 0.05, halves);
        shortPort.replace(R"("selection": {"of": ["L1"], "side": "top"})", R"("selection": {"of": ["half"], "side": "top"})");
        shortPort.replace(R"("n": "1.2")", R"("n": "1")");
        why = error(shortPort);
        QVERIFY2(why.contains(QLatin1String("needs a Periodic Condition")), qPrintable(why));
        // With a waveguide's port.
        QByteArray mixed = cell(0.4, stack, QStringLiteral("ez"), 0, 1, 0.05);
        mixed.replace(R"({"type": "periodicport", "name": "out", )", R"({"type": "port", "name": "out", )");
        why = error(mixed);
        QVERIFY2(why.contains(QLatin1String("periodic ports and ports together")), qPrintable(why));
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
        {
            // The grating: at 10° each order as the coupled waves have it,
            // nothing lost at any angle; the +1st reflected at 0°, gone past
            // grazing at 40° (sin θ + λ/P > 1 from 21.5°).
            timer.start();
            Model m = load(QStringLiteral("grating_orders.qfem"));
            // (A sanitized build: three of its angles.)
            const bool slow = slowBuild();
            if (slow) {
                QJsonArray rows;   // (by append: a braced list of one list is a copy to some compilers)
                rows.append(QJsonArray{QStringLiteral("theta"), QStringLiteral("0, 10, 40"), QString()});
                for (Node& step : m.find(m.studies().front()->tag)->children)
                    if (step.type == QLatin1String("sweep")) step.props.insert(QStringLiteral("parameters"), rows);
            }
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QCOMPARE(int(r.study.solutions.size()), slow ? 3 : 9);
            for (const auto& point : r.study.solutions)
                QVERIFY(std::abs(global(*point, QStringLiteral("ewfd.R")) + global(*point, QStringLiteral("ewfd.T")) - 1) < 1e-9);
            Solution& at10 = *r.study.solutions[slow ? 1 : 2];
            const Efficiencies ref = rcwaTE(1.0, 1.5, 1.0, 1.5, 1.0, 0.5, 0.3, 10, 0.6328, 40);
            for (const auto& [name, value] : std::vector<std::pair<QString, double>>{{QStringLiteral("R_0"), ref.R[40]}, {QStringLiteral("T_0"), ref.T[40]},
                                                                                     {QStringLiteral("T_n1"), ref.T[39]}, {QStringLiteral("T_1"), ref.T[41]}})
                QVERIFY2(std::abs(global(at10, QStringLiteral("ewfd.") + name) - value) < 5e-4,
                         qPrintable(QStringLiteral("%1 %2 against %3").arg(name).arg(global(at10, QStringLiteral("ewfd.") + name)).arg(value)));
            QVERIFY(!std::isnan(global(*r.study.solutions.front(), QStringLiteral("ewfd.R_1"))));
            QVERIFY(std::isnan(global(*r.study.solutions.back(), QStringLiteral("ewfd.R_1"))));
            QVERIFY(!std::isnan(global(*r.study.solutions.back(), QStringLiteral("ewfd.R_n1"))));
            qInfo("grating: at 10° R_0 %.5f, T_0 %.5f, T_n1 %.5f, T_1 %.5f; %lld ms", global(at10, QStringLiteral("ewfd.R_0")), global(at10, QStringLiteral("ewfd.T_0")),
                  global(at10, QStringLiteral("ewfd.T_n1")), global(at10, QStringLiteral("ewfd.T_1")), timer.elapsed());
        }
    }
};

QTEST_GUILESS_MAIN(TestFemWaves)
#include "test_fem_waves.moc"
