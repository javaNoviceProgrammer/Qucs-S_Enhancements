/*
 * test_fem_laser.cpp - the multiphysics solver's lasers: a semiconductor's
 *                      carriers and a resonator's mode solved together
 *                      (its Lasing feature, a Laser L-I step) against the
 *                      rate equations worked by hand
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_mesh.h"
#include "fem_problem.h"
#include "fem_results.h"

#include <QFile>
#include <QRegularExpression>
#include <QtTest>

#include <cmath>
#include <set>

using namespace qucs_s::fem;

namespace {

constexpr double C = 299792458.0, Q = 1.602176634e-19, Hbar = 1.054571817e-34;

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

/// A toy laser: a metal pillbox (radius 1 µm, 1 µm high) of GaAs whose
/// index is n 3.5 - 0.001j (Q n/2κ), its TM010 mode E_z = J0(2.405 r/a)
/// uniform along z - a 20 nm active layer at its middle holds 2 % of its
/// energy, Γ 0.02 exactly; a p-i-n across it (contacts top and bottom),
/// recombination only on the active layer, radiative (B), its gain linear
/// a (N - Ntr), N = √(n p), ng 3.5. \a steps the study's.
QByteArray pillbox(const QString& steps, const QString& more = {})
{
    return QStringLiteral(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um", "space": "axisymmetric",
        "geometry": [{"type": "rectangle", "name": "nclad", "position": ["0", "0"], "size": ["1", "0.49"]},
                     {"type": "rectangle", "name": "ring", "position": ["0", "0.49"], "size": ["1", "0.02"]},
                     {"type": "rectangle", "name": "active", "position": ["0", "0.49"], "size": ["0.2", "0.02"]},
                     {"type": "rectangle", "name": "pclad", "position": ["0", "0.51"], "size": ["1", "0.49"]}],
        "materials": [{"material": "Aluminium gallium arsenide", "x": "0.6", "n": "3.5", "kappa": "1e-3", "selection": {"only": ["nclad", "pclad", "ring"]}},
                      {"material": "Gallium arsenide", "n": "3.5", "kappa": "1e-3", "selection": {"only": ["active"]}}],
        "physics": [{"type": "resonator", "tag": "emr", "m": 0, "features": [{"type": "wave", "selection": {"all": true}}]},
                    {"type": "semiconductor", "tag": "semi", "features": [
          {"type": "doping", "dopant": "donor", "N0": "1e18[1/cm^3]", "selection": {"only": ["nclad"]}},
          {"type": "doping", "dopant": "acceptor", "N0": "1e18[1/cm^3]", "selection": {"only": ["pclad"]}},
          {"type": "radiative", "B": "1e-10[cm^3/s]", "selection": {"only": ["active"]}},
          {"type": "laser", "gain": "linear", "a": "5e-16[cm^2]", "Ntr": "2e18[1/cm^3]", "beta": "1e-4", "ng": "3.5" %2,
           "selection": {"only": ["active"]}},
          {"type": "ohmic", "name": "anode", "V0": "0[V]", "selection": {"of": ["pclad"], "side": "top"}},
          {"type": "ohmic", "name": "cathode", "V0": "0[V]", "selection": {"of": ["nclad"], "side": "bottom"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "0.1",
                 "features": [{"type": "layered", "hlateral": "0.05", "count": 4, "selection": {"all": true}}]}}],
      "studies": [{"type": "study", "steps": [{"type": "eigenfrequency", "physics": ["emr"], "count": 1, "around": "9.1[um]"}, %1]}]})json")
        .arg(steps, more)
        .toUtf8();
}

} // namespace

class TestFemLaser : public QObject
{
    Q_OBJECT

    /// The toy's L-I: its points (V), each its globals.
    struct Point {
        double V, I, S, Pout, Nact, G, Gnet, gmean, Istim, Isp;
    };
    static std::vector<Point> points(Solution& s)
    {
        std::vector<Point> out;
        for (int k = 0; k < s.snapshotCount(); ++k) {
            s.select(k);
            auto g = [&s](const char* name) { return global(s, QStringLiteral("semi.") + QLatin1String(name)); };
            out.push_back({s.snapshotValue(k), g("I_anode"), g("S"), g("Pout"), g("Nact"), g("G"), g("Gnet"), g("gmean"), g("Istim"), g("Isp")});
        }
        return out;
    }

private slots:
    // The toy laser against its rate equations: Γ the mode's share of its
    // energy on the disk, (h_a/h) b² (J0(kb)² + J1(kb)²)/(a² J1(ka)²); above
    // threshold the carriers clamped at Ntr + 1/(τp v Γ a) and the mode's
    // mean gain at 1/(τp v Γ); the disk's spontaneous current q B N² V;
    // below it S = τp β Rsp/(1 - τp G); the light η ħω S/τp, its slope ħω/q
    // times the differential efficiency, the photons' balance S/τp = G S +
    // β Rsp. The cavity's stored energy ½ ε0 Re ε h π a² J1(ka)²; each bias
    // point a solution, by voltage; the stimulated rate and the gain at each
    // point (semi.Rst, semi.gain) over the disk Istim/q and the mode's mean.
    void aToyLaserAsItsRateEquations()
    {
        QByteArray json = pillbox(QStringLiteral(R"j({"type": "laser", "physics": ["semi"], "contact": "anode", "voltages": "range(1.40, 0.01, 1.60)"})j"));
        json.replace(R"("studies": [)", R"("results": {"derived": [{"type": "integral", "tag": "rst1", "expression": "semi.Rst", "revolved": true, "selection": {"only": ["active"]}},
                                                                {"type": "average", "tag": "gav", "expression": "semi.gain", "revolved": true, "selection": {"only": ["active"]}}]}, "studies": [)");
        const Model model = modelOf(json);
        Run r = run(model);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        QCOMPARE(s.snapshotCount(), 21);
        QCOMPARE(s.snapshotName(), QStringLiteral("voltage"));
        QVERIFY(s.snapshotDim() == Dim::voltage());
        const std::vector<Point> p = points(s);
        // The mode: Γ, its energy.
        const double a = 1e-6, h = 1e-6, b = 0.2e-6, ha = 0.02e-6, j01 = 2.404825557695773, k = j01 / a;
        auto J = [](int n, double x) {
            double term = std::pow(x / 2, n) / std::tgamma(n + 1), sum = term;
            for (int m = 1; m < 40; ++m) {
                term *= -(x * x / 4) / (m * double(m + n));
                sum += term;
            }
            return sum;
        };
        const double gamma = ha / h * b * b * (J(0, k * b) * J(0, k * b) + J(1, k * b) * J(1, k * b)) / (a * a * J(1, j01) * J(1, j01));
        const double Gamma = global(s, QStringLiteral("semi.Gamma"));
        qInfo("Γ %.7g (Bessel's %.7g)", Gamma, gamma);
        QVERIFY(std::abs(Gamma / gamma - 1) < 1e-4);
        const double epsr = 3.5 * 3.5 - 1e-6, U = global(s, QStringLiteral("emr.U"));
        const double Uexact = 0.5 * 8.8541878188e-12 * epsr * h * M_PI * a * a * J(1, j01) * J(1, j01);
        qInfo("U %.7g J (%.7g)", U, Uexact);
        QVERIFY(std::abs(U / Uexact - 1) < 1e-3);
        // Threshold: the carriers clamped, the gain at gth.
        const double taup = global(s, QStringLiteral("semi.taup")), v = C / 3.5, ad = 5e-20, Ntr = 2e24, B = 1e-16;
        const double gth = 1 / (taup * v * Gamma), Nth = Ntr + gth / ad;
        QVERIFY(std::abs(global(s, QStringLiteral("semi.gth")) / gth - 1) < 1e-9);
        const double freq = global(s, QStringLiteral("emr.freq")), photon = Hbar * 2 * M_PI * freq;
        int lasing = 0;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const Point& q = p[i];
            // The photons' balance; the light out.
            QVERIFY(std::abs(q.S / taup - (q.G * q.S + 1e-4 * q.Isp / Q)) <= 1e-6 * (q.S / taup));
            QVERIFY(std::abs(q.Pout - photon * q.S / taup) <= 1e-9 * q.Pout);
            if (q.S > 100) {
                ++lasing;
                QVERIFY2(std::abs(q.Nact / Nth - 1) < 1e-3, qPrintable(QStringLiteral("%1: %2 (%3)").arg(q.V).arg(q.Nact).arg(Nth)));
                QVERIFY(std::abs(q.gmean / gth - 1) < 1e-4);
                const double Isp = Q * B * Nth * Nth * M_PI * b * b * ha;
                QVERIFY2(std::abs(q.Isp / Isp - 1) < 3e-3, qPrintable(QStringLiteral("%1 %2").arg(q.Isp).arg(Isp)));
            } else {
                // Below: the spontaneous photons' steady state.
                const double S = taup * 1e-4 * (q.Isp / Q) / (1 - taup * q.G);
                QVERIFY(std::abs(q.S / S - 1) < 1e-6);
                QVERIFY(q.Gnet < 0);
            }
        }
        QVERIFY2(lasing >= 5 && lasing <= 10, qPrintable(QString::number(lasing)));
        // The stimulated rate per volume (semi.Rst) over the disk: its photons
        // stimulated, Istim/q.
        s.select(s.snapshotCount() - 1);
        const DerivedResult rst = evaluateDerived(s, *model.find(QStringLiteral("rst1")));
        QVERIFY2(rst.error.isEmpty() && !rst.values.empty(), qPrintable(rst.error));
        qInfo("∫ semi.Rst dV %.6g /s, Istim/q %.6g /s", rst.values.front().value, p.back().Istim / Q);
        const DerivedResult gav = evaluateDerived(s, *model.find(QStringLiteral("gav")));
        QVERIFY2(gav.error.isEmpty() && !gav.values.empty(), qPrintable(gav.error));
        qInfo("semi.gain's mean on the disk %.7g per m, gmean %.7g", gav.values.front().value, p.back().gmean);
        QVERIFY(std::abs(gav.values.front().value / p.back().gmean - 1) < 2e-3);
        QVERIFY(std::abs(rst.values.front().value / (p.back().Istim / Q) - 1) < 1e-2);
        // Its slope: ħω/q times the stimulated current's share of the current.
        const Point& x0 = p[p.size() - 4];
        const Point& x1 = p.back();
        const double slope = (x1.Pout - x0.Pout) / (x1.I - x0.I), eta = (x1.Istim - x0.Istim) / (x1.I - x0.I);
        qInfo("slope %.5g W/A, ħω/q %.5g, differential efficiency %.5g", slope, photon / Q, eta);
        QVERIFY(eta > 0.98 && eta <= 1.0);
        QVERIFY(std::abs(slope / (eta * photon / Q) - 1) < 2e-3);
    }

    // Its gain logarithmic, of the electrons (the disk p-doped - the reason
    // to take them: p well above n there): at each point g0 ln((n + Ns)/(Ntr
    // + Ns)), the mode's mean clamped at gth; without ng the photons' speed
    // c/√Re ε.
    void aLogarithmicGain()
    {
        QByteArray json = pillbox(QStringLiteral(R"j({"type": "laser", "physics": ["semi"], "contact": "anode", "voltages": "range(1.50, 0.02, 1.62)"})j"),
                                  QStringLiteral(R"(, "g0": "4000[1/cm]", "Ns": "1e18[1/cm^3]", "carrier": "electrons")"));
        json.replace(R"("gain": "linear")", R"("gain": "logarithmic")");
        json.replace(R"(, "ng": "3.5")", "");
        json.replace(R"({"type": "radiative")", R"({"type": "doping", "dopant": "acceptor", "N0": "5e18[1/cm^3]", "selection": {"only": ["active"]}}, {"type": "radiative")");
        json.replace(R"("studies": [)", R"j("results": {"derived": [
            {"type": "maximum", "tag": "off", "expression": "abs(semi.gain - 4000[1/cm]*ln((semi.n + 1e18[1/cm^3])/(3e18[1/cm^3])))", "selection": {"only": ["active"]}},
            {"type": "average", "tag": "nav", "expression": "semi.n", "revolved": true, "selection": {"only": ["active"]}},
            {"type": "average", "tag": "pav", "expression": "semi.p", "revolved": true, "selection": {"only": ["active"]}}]}, "studies": [)j");
        const Model model = modelOf(json);
        Run r = run(model);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        s.select(s.snapshotCount() - 1);
        auto derived = [&](const char* tag) {
            const DerivedResult d = evaluateDerived(s, *model.find(QLatin1String(tag)));
            return d.error.isEmpty() && !d.values.empty() ? d.values.front().value : NAN;
        };
        const double gth = global(s, QStringLiteral("semi.gth")), gmean = global(s, QStringLiteral("semi.gmean"));
        const double v = C / std::sqrt(3.5 * 3.5 - 1e-6), taup = global(s, QStringLiteral("semi.taup")), Gamma = global(s, QStringLiteral("semi.Gamma"));
        qInfo("logarithmic: n %.5g, p %.5g on the disk; gmean %.7g, gth %.7g (%.7g); its form off by %.3g at most; S %.4g", derived("nav"), derived("pav"),
              gmean, gth, 1 / (taup * v * Gamma), derived("off"), global(s, QStringLiteral("semi.S")));
        QVERIFY(derived("pav") > 1.2 * derived("nav"));
        QVERIFY(global(s, QStringLiteral("semi.S")) > 100);
        QVERIFY(std::abs(gmean / gth - 1) < 1e-4);
        QVERIFY(derived("off") < 3e-3 * gth);   // (between nodes: the gain linear, n not)
        QVERIFY(std::abs(gth * taup * v * Gamma - 1) < 1e-9);
    }

    // The wells' gain table: a cubic in ln N (Fritsch and Carlson's) - a
    // gain linear in ln N exactly, its derivative too; a saturating table's
    // and one with plateaus monotone between their points, the derivative
    // the values' (Newton's method's); held beyond its ends, the derivative
    // nought.
    void theGainTable()
    {
        detail::GainTable t;
        for (int k = 0; k < 9; ++k) {
            t.N.push_back(1e23 * std::pow(100.0, k / 8.0));
            t.g.push_back(1e5 * std::log(t.N.back() / 1e24));
        }
        t.prepare();
        for (double N : {1.3e23, 7.7e23, 2.2e24, 9.1e24}) {
            double d = 0;
            const double g = t.at(N, &d);
            QVERIFY2(std::abs(g / 1e5 - std::log(N / 1e24)) < 1e-12, qPrintable(QStringLiteral("%1: %2").arg(N).arg(g)));
            QVERIFY(std::abs(d * N / 1e5 - 1) < 1e-12);
        }
        // A saturating table and one with plateaus (an absorption's floor,
        // the gain's ceiling): monotone between their points, within their
        // values - its slopes nought beside a plateau.
        for (const std::vector<double>& gs : {std::vector<double>{-4e5, -1e5, 2e5, 3.5e5, 4.2e5, 4.4e5, 4.45e5},
                                              std::vector<double>{-4e5, -4e5, -4e5, 1e5, 4e5, 4e5}}) {
            detail::GainTable s;
            for (std::size_t k = 0; k < gs.size(); ++k) {
                s.N.push_back(1e23 * std::pow(2.0, double(k)));
                s.g.push_back(gs[k]);
            }
            s.prepare();
            const double top = std::pow(2.0, double(gs.size() - 1));
            double last = -INFINITY;
            for (int i = 1; i < 600; ++i) {
                const double N = 1e23 * std::pow(top, i / 600.0), h = 1e-6 * N;
                double d = 0;
                const double g = s.at(N, &d);
                // (To rounding: a plateau's cubic is its value's to 1e-16.)
                const double ulp = 1e-9 * 4e5;
                QVERIFY2(g >= last - ulp && g >= gs.front() - ulp && g <= gs.back() + ulp, qPrintable(QStringLiteral("%1: %2 after %3").arg(N).arg(g).arg(last)));
                last = g;
                const double slope = (s.at(N + h, nullptr) - s.at(N - h, nullptr)) / (2 * h);
                QVERIFY2(std::abs(slope - d) <= 1e-5 * (std::abs(d) + 4e5 / N), qPrintable(QStringLiteral("%1: %2 %3").arg(N).arg(d).arg(slope)));
            }
            // (Exactly: QCOMPARE takes 1e-12 for nought.)
            double d = 1;
            QVERIFY(s.at(1e22, &d) == gs.front() && d == 0);
            d = 1;
            QVERIFY(s.at(1e26, &d) == gs.back() && d == 0);
        }
    }

    // The example vcsel_laser.qfem as its description says (six of its
    // voltages): the mode HE11 980.30 nm, Q 15 800; Γ 0.042, the gain at
    // threshold 409 per cm; the wells transparent at 8.6e17 per cm³;
    // threshold 0.13 mA at 1.40 V, 0.59 W/A, 1.1 mW at 2.1 mA (1.90 V);
    // dark at 1.30 V, lasing from 1.40 V. (A slow build: its geometry and
    // mesh alone - CI's Release builds solve it.)
    void theVcselLaserExample()
    {
        QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/vcsel_laser.qfem"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        Model model = modelOf(f.readAll());
        if (slowBuild()) {
            const std::shared_ptr<ParameterScope> ps = evaluateParameters(model);
            const GeometryBuild g = buildGeometry(model, *ps);
            QVERIFY(g.ok());
            const MeshBuild m = buildMesh(model, *ps, g.topology);
            QVERIFY2(m.mesh, qPrintable(m.error));
            QSKIP("its solve is a Release build's");
        }
        Node* step = nullptr;
        for (const Node* study : model.studies())
            for (const Node& st : study->children)
                if (st.type == QLatin1String("laser")) step = model.find(st.tag);
        QVERIFY(step);
        step->props.insert(QStringLiteral("voltages"), QStringLiteral("1.30, 1.38, 1.40, 1.44, 1.60, 1.90"));
        Run r = run(model);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        const QString log = s.log.join(QLatin1Char('\n'));
        const double lambda = global(s, QStringLiteral("emr.lambda0")), Qf = global(s, QStringLiteral("emr.Q"));
        qInfo("mode %.5f nm, Q %.0f, Γ %.5f, gth %.1f per cm", lambda * 1e9, Qf, global(s, QStringLiteral("semi.Gamma")), global(s, QStringLiteral("semi.gth")) / 100);
        QVERIFY(std::abs(lambda * 1e9 - 980.30) < 0.01 && std::abs(Qf / 15829 - 1) < 0.01);
        QVERIFY(std::abs(global(s, QStringLiteral("semi.Gamma")) / 0.04158 - 1) < 0.01);
        QVERIFY(std::abs(global(s, QStringLiteral("semi.gth")) / 4.0905e4 - 1) < 0.01);
        QRegularExpressionMatch tr = QRegularExpression(QStringLiteral("transparency ([0-9.e+]+) per cm")).match(log);
        QVERIFY2(tr.hasMatch() && std::abs(tr.captured(1).toDouble() / 8.577e17 - 1) < 0.01, qPrintable(log));
        const std::vector<Point> p = points(s);
        QCOMPARE(int(p.size()), 6);
        QVERIFY(p[0].S < 1 && p[0].Gnet < 0);
        for (std::size_t k = 2; k < p.size(); ++k) QVERIFY2(std::abs(p[k].gmean / 4.0905e4 - 1) < 1e-3, qPrintable(QString::number(p[k].gmean)));
        QVERIFY(std::abs(p.back().Pout / 1.142e-3 - 1) < 0.01 && std::abs(p.back().I / 2.06e-3 - 1) < 0.01);
        QRegularExpressionMatch th = QRegularExpression(QStringLiteral("threshold ([0-9.]+) µA at ([0-9.]+) V.*slope ([0-9.]+) W/A")).match(log);
        QVERIFY2(th.hasMatch(), qPrintable(log));
        qInfo("threshold %s µA at %s V, slope %s W/A", qPrintable(th.captured(1)), qPrintable(th.captured(2)), qPrintable(th.captured(3)));
        QVERIFY(std::abs(th.captured(1).toDouble() / 129 - 1) < 0.05 && std::abs(th.captured(2).toDouble() - 1.40) < 0.01
                && std::abs(th.captured(3).toDouble() / 0.589 - 1) < 0.02);
    }

    // The Eigenfrequency step's mode kept for the steps after: the one asked
    // (each of three: its number, its frequency), said; more than it found
    // refused.
    void theModeKept()
    {
        const QByteArray base = pillbox(QStringLiteral(R"j({"type": "stationary", "physics": ["semi"]})j"));
        auto keeping = [&base](int k) {
            QByteArray json = base;
            json.replace(R"("count": 1, "around": "9.1[um]")", QStringLiteral(R"("count": 3, "around": "6[um]", "keep": %1)").arg(k).toUtf8());
            return run(modelOf(json));
        };
        std::set<double> frequencies;
        for (int k = 1; k <= 3; ++k) {
            Run r = keeping(k);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QCOMPARE(int(global(*r.solution, QStringLiteral("emr.mode"))), k);
            QVERIFY(r.solution->log.join(QLatin1Char(' ')).contains(QStringLiteral("mode %1 kept").arg(k)));
            frequencies.insert(global(*r.solution, QStringLiteral("emr.freq")));
        }
        QCOMPARE(int(frequencies.size()), 3);
        Run r = keeping(4);
        QVERIFY2(r.error.contains(QLatin1String("keeps mode 4 of the 3")), qPrintable(r.error));
    }

    // Quantum wells about an axis: a line along it (y) planar wells across it
    // - a VCSEL's -, their states those of the same wells in a plane; along
    // r refused, said.
    void planarWellsAboutAnAxis()
    {
        auto wells = [](const QString& space, const QString& axis, const QString& at) {
            return QStringLiteral(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "nm" %1,
                "geometry": [{"type": "rectangle", "name": "below", "size": ["50", "20"]},
                             {"type": "rectangle", "name": "well", "position": ["0", "20"], "size": ["50", "8"]},
                             {"type": "rectangle", "name": "above", "position": ["0", "28"], "size": ["50", "20"]}],
                "materials": [{"material": "Gallium arsenide", "selection": {"only": ["below", "above"]}},
                              {"material": "Indium gallium arsenide", "x": "0.2", "selection": {"only": ["well"]}}],
                "physics": [{"type": "quantum", "tag": "qw", "space": "1d", "axis": "%2", "at": "%3", "substrate": "Gallium arsenide"}],
                "mesh": {"size": "normal"}}],
              "studies": [{"type": "study", "steps": [{"type": "states", "states": 2}]}]})json")
                .arg(space, axis, at)
                .toUtf8();
        };
        Run plane = run(modelOf(wells(QString(), QStringLiteral("y"), QStringLiteral("25"))));
        QVERIFY2(plane.error.isEmpty(), qPrintable(plane.error));
        Run axi = run(modelOf(wells(QStringLiteral(R"(, "space": "axisymmetric")"), QStringLiteral("y"), QStringLiteral("25"))));
        QVERIFY2(axi.error.isEmpty(), qPrintable(axi.error));
        for (const char* e : {"qw.Ee1", "qw.Ehh1", "qw.Ee2"}) {
            const double a = global(*plane.solution, QLatin1String(e)), b = global(*axi.solution, QLatin1String(e));
            QVERIFY2(std::isfinite(a) && a == b, qPrintable(QStringLiteral("%1: %2 %3").arg(QLatin1String(e)).arg(a).arg(b)));
        }
        Run along = run(modelOf(wells(QStringLiteral(R"(, "space": "axisymmetric")"), QStringLiteral("x"), QStringLiteral("25"))));
        QVERIFY2(along.error.contains(QLatin1String("lie across it")), qPrintable(along.error));
    }

    // Refused, and said: a Laser L-I step without a semiconductor that lases,
    // without its mode solved before, with a contact it has not; a cavity
    // that loses nothing; a step in time with lasing.
    void refusals()
    {
        const QString lis = QStringLiteral(R"j({"type": "laser", "physics": ["semi"], "contact": "anode", "voltages": "1.5"})j");
        // No Eigenfrequency step.
        QByteArray json = pillbox(lis);
        json.replace(R"({"type": "eigenfrequency", "physics": ["emr"], "count": 1, "around": "9.1[um]"}, )", "");
        Run r = run(modelOf(json));
        QVERIFY2(r.error.contains(QLatin1String("Eigenfrequency step before")), qPrintable(r.error));
        // A contact it has not.
        r = run(modelOf(pillbox(QStringLiteral(R"j({"type": "laser", "physics": ["semi"], "contact": "drain", "voltages": "1.5"})j"))));
        QVERIFY2(r.error.contains(QLatin1String("no metal contact drain")), qPrintable(r.error));
        // No lasing.
        json = pillbox(lis);
        json.replace(R"("type": "laser", "gain")", R"("type": "laser", "enabled": false, "gain")");
        r = run(modelOf(json));
        QVERIFY2(r.error.contains(QLatin1String("none of its physics lases")), qPrintable(r.error));
        // Lossless.
        json = pillbox(lis);
        json.replace(R"("kappa": "1e-3")", R"("kappa": "0")");
        r = run(modelOf(json));
        QVERIFY2(r.error.contains(QLatin1String("loses nothing")), qPrintable(r.error));
        // In time (a study of its own: not after an Eigenfrequency step's frequency).
        json = pillbox(QStringLiteral(R"j({"type": "transient", "physics": ["semi"], "times": "range(0, 1e-12, 2e-12)"})j"));
        json.replace(R"({"type": "eigenfrequency", "physics": ["emr"], "count": 1, "around": "9.1[um]"}, )", "");
        r = run(modelOf(json));
        QVERIFY2(r.error.contains(QLatin1String("lases at rest")), qPrintable(r.error));
    }
};

QTEST_GUILESS_MAIN(TestFemLaser)
#include "test_fem_laser.moc"
