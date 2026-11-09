/*
 * test_fem_crystal.cpp - the crystal implant's binary-collision Monte Carlo
 *                        (the implant step's "crystal" profile): its
 *                        scattering table against the Coulomb limit, its
 *                        amorphous range against the tables, channelling on
 *                        axis, its dose kept, its runs the same for a seed;
 *                        through a process flow against Cai et al.'s SIMS
 *                        (the review of 8 November 2026), the tilt not
 *                        projected twice, its cache, its cancel, what it
 *                        refuses; the dual Pearson's report and moments
 *                        along the normal
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_model.h"
#include "fem_process.h"
#include "fem_process_p.h"

#include <QElapsedTimer>
#include <QFile>
#include <QtTest>

#include <cmath>
#include <map>

using namespace qucs_s::fem;

namespace {

/// A sanitized build's (QUCS_TEST_SLOW too): fewer ions there.
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

process::Target silicon()
{
    process::Target t;
    t.parts.push_back({14, 28.085, 1.0});
    t.atoms = 2.329e3 / (28.085 * 1.66053906660e-27);
    t.formula = QStringLiteral("Si");
    return t;
}

process::Target oxide()
{
    process::Target t;
    t.parts.push_back({14, 28.085, 1.0 / 3});
    t.parts.push_back({8, 15.999, 2.0 / 3});
    t.atoms = 3 * 2.2e3 / (60.08 * 1.66053906660e-27);
    t.formula = QStringLiteral("Si O2");
    return t;
}

/// Phosphorus into (100) silicon under 16 Å of native oxide, as the review ran it.
process::CrystalRun phosphorus(double keV, double tiltDeg, double rotDeg, double divDeg, long ions)
{
    process::CrystalRun r;
    r.Z1 = 15;
    r.M1 = 30.97376;
    r.energy = keV * 1e3;
    r.tilt = tiltDeg * M_PI / 180;
    r.rotation = rotDeg * M_PI / 180;
    r.divergence = divDeg * M_PI / 180;
    r.crystal = silicon();
    r.cover.push_back({QStringLiteral("Silicon dioxide"), oxide(), 1.6e-9});
    r.ions = ions;
    return r;
}

/// A model in µm of one process flow, its steps (its parameters, a JSON list).
Model flowModel(const QString& steps, const QString& flowProps, const QString& parameters = QStringLiteral("[]"))
{
    const QString json = QStringLiteral(R"({"format": "qucs-s multiphysics 1", "parameters": %3,
      "components": [{"unit": "um", "geometry": [{"type": "process", "tag": "pf1", "label": "flow", %1, "features": [%2]},
                                                {"type": "form union", "tag": "fin"}]}]})")
                             .arg(flowProps, steps, parameters);
    QString why;
    const std::optional<Model> m = Model::fromJson(json.toUtf8(), &why);
    if (!m) qWarning("%s", qPrintable(why));
    return m.value_or(Model());
}

ProcessRun runFlow(const Model& m, const std::function<bool(double, const QString&)>& progress = {})
{
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    return runProcess(*m.find(QStringLiteral("pf1")), m, *ps, 1e-6, {}, progress);
}

QString said(const ProcessRun& r)
{
    QStringList lines;
    for (const ProcessStepReport& s : r.steps)
        lines << s.name + QStringLiteral(": ") + s.said + (s.warnings.isEmpty() ? QString() : QStringLiteral(" [") + s.warnings.join(QStringLiteral("; ")) + QLatin1Char(']'));
    return r.error + QLatin1Char('\n') + lines.join(QLatin1Char('\n'));
}

/// Silicon (100) under a native oxide of 1.6 nm, then an implant of P (its
/// properties given); the window narrow, the grid's rows 1 nm to \a depth.
Model implantFlow(const QString& implant, double depth = 1.6, const QString& orientation = QStringLiteral("100"),
                  const QString& material = QStringLiteral("Silicon"), double width = 0.2)
{
    return flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "si", "material": "%3", "thickness": "3", "orientation": "%2"},
        {"type": "deposit", "tag": "nox", "label": "native", "material": "Silicon dioxide", "thickness": "1.6[nm]", "mode": "planar"},
        {"type": "implant", "tag": "imp", "label": "P_implant", "species": "P", %1})")
                         .arg(implant, orientation, material),
                     QStringLiteral(R"("width": "%2", "grid": "0.2[nm]", "dx": "%3", "dy": "1[nm]", "depth": "%1")").arg(depth).arg(width).arg(width / 2 > 0.25 ? 0.25 : width / 2));
}

/// The run a flow of implantFlow makes: its targets the library's.
process::CrystalRun flowsRun(const Model& m, double keV, double tilt, double rot, long ions)
{
    process::CrystalRun r = phosphorus(keV, tilt, rot, 1, ions);
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    r.crystal = *process::targetOf(QStringLiteral("Silicon"), m, *ps);
    r.cover.front().target = *process::targetOf(QStringLiteral("Silicon dioxide"), m, *ps);
    return r;
}

/// A species down the column at \a x (µm from the window's left; the first
/// when below 0): (depth below y = 0 µm, cm⁻³), the silicon's cells. (A
/// tilted beam's: a column far enough right that its deep cells are fed
/// by paths that came in through the window's top.)
std::vector<std::pair<double, double>> column(const ProcessRun& r, const QString& species, double x = -1)
{
    std::vector<std::pair<double, double>> out;
    const ProcessDopants* d = r.state.dopants.get();
    if (!d) return out;
    const ProcessDopants::Field* f = d->field(species);
    if (!f) return out;
    int i = 0;
    while (x >= 0 && i + 1 < d->nx() && d->xe[std::size_t(i + 1)] - d->xe.front() <= x) ++i;
    for (int j = d->ny() - 1; j >= 0; --j) {
        const double y = 0.5 * (d->ye[std::size_t(j)] + d->ye[std::size_t(j + 1)]);
        if (y > 0) continue;
        out.emplace_back(-y, f->total[d->index(i, j)] * 1e-6);
    }
    return out;
}

/// All of a species over the window (cm⁻²).
double dose(const ProcessRun& r, const QString& species)
{
    const ProcessDopants* d = r.state.dopants.get();
    const ProcessDopants::Field* f = d ? d->field(species) : nullptr;
    if (!f) return 0;
    double sum = 0;
    for (int j = 0; j < d->ny(); ++j)
        for (int i = 0; i < d->nx(); ++i)
            sum += f->total[d->index(i, j)] * (d->xe[std::size_t(i + 1)] - d->xe[std::size_t(i)]) * (d->ye[std::size_t(j + 1)] - d->ye[std::size_t(j)]) * 1e-12;
    return sum / ((d->xe.back() - d->xe.front()) * 1e-6) * 1e-4;
}

/// A profile's moments (µm): mean, straggle.
std::pair<double, double> moments(const std::vector<std::pair<double, double>>& p)
{
    double m0 = 0, m1 = 0, m2 = 0;
    for (const auto& [z, c] : p) {
        m0 += c;
        m1 += c * z;
    }
    const double mean = m1 / m0;
    for (const auto& [z, c] : p) m2 += c * (z - mean) * (z - mean);
    return {mean, std::sqrt(m2 / m0)};
}

/// The review's measure against SIMS: rms and largest |log10(model/SIMS)|
/// over the SIMS points above 1e15 cm⁻³ deeper than 10 nm - a profile that
/// has ended counts as 1e14 there.
std::pair<double, double> decades(const std::vector<std::pair<double, double>>& model, const std::vector<std::pair<double, double>>& sims)
{
    double sum = 0, worst = 0;
    int n = 0;
    for (const auto& [x, y] : sims) {
        if (x < 0.01 || y < 1e15) continue;
        double v = 1e14;
        for (std::size_t k = 0; k + 1 < model.size(); ++k)
            if (model[k].first <= x && x <= model[k + 1].first) {
                const double a = std::max(model[k].second, 1e10), b = std::max(model[k + 1].second, 1e10);
                const double t = (x - model[k].first) / (model[k + 1].first - model[k].first);
                v = std::max(1e14, std::exp((1 - t) * std::log(a) + t * std::log(b)));
                break;
            }
        const double e = std::log10(v / y);
        sum += e * e;
        worst = std::max(worst, std::abs(e));
        ++n;
    }
    return {n ? std::sqrt(sum / n) : 99.0, worst};
}

} // namespace

class TestFemCrystal : public QObject
{
    Q_OBJECT

private slots:
    // The scattering in the Coulomb limit (ε large, b small: the closest
    // approach well inside the screening length): tan(θ/2) = 1/(2εb), r0 =
    // k + √(k² + b²), k = 1/(2ε) - the integral itself to 1.5/ε (the
    // screening's constant term), its table (the Monte Carlo's) on and
    // between its points to 1 %.
    void scatteringInTheCoulombLimit()
    {
        for (const double eps : {1e3, 1e4, 1e5})
            for (const double b : {1e-5, 1e-4, 1e-3}) {
                if (eps * b < 0.5) continue;   // (θ near π: no limit to take)
                const process::Scattering s = process::universalScattering(eps, b);
                const double theta = 2 * std::atan(1 / (2 * eps * b)), k = 1 / (2 * eps), r0 = k + std::sqrt(k * k + b * b);
                const QString what = QStringLiteral("ε %1 b %2: θ %3 (Coulomb %4), r0 %5 (%6)").arg(eps).arg(b).arg(s.theta, 0, 'g', 10).arg(theta, 0, 'g', 10).arg(s.r0, 0, 'g', 10).arg(r0, 0, 'g', 10);
                // (The screening's constant term shifts the energy by about 1/ε.)
                QVERIFY2(std::abs(s.theta / theta - 1) < 1.5 / eps && std::abs(s.r0 / r0 - 1) < 1.5 / eps, qPrintable(what));
            }
        // The table: on its points (b every 0.01) and between them in ε.
        for (const double le : {3.0, 3.31, 4.0, 4.57, 4.99})
            for (const double b : {0.01, 0.02, 0.03}) {
                const double eps = std::pow(10.0, le);
                const process::Scattering t = process::tabulatedScattering(eps, b);
                const double theta = 2 * std::atan(1 / (2 * eps * b));
                QVERIFY2(std::abs(t.theta / theta - 1) < 0.01, qPrintable(QStringLiteral("ε %1 b %2: tabled θ %3, Coulomb %4").arg(eps).arg(b).arg(t.theta).arg(theta)));
            }
        // A head-on collision turns it back; a distant one barely.
        QVERIFY(std::abs(process::universalScattering(1, 1e-9).theta - M_PI) < 1e-3);
        QVERIFY(process::universalScattering(1, 30).theta < 1e-5);
    }

    // Amorphous silicon (the crystal taken as amorphous), phosphorus at 100
    // keV: Rp 0.131 ± 0.003 µm, ΔRp 0.047 ± 0.002 µm - Plummer et al.'s LSS
    // table has 0.129 and 0.0466; nothing in the model fitted to it.
    void amorphousRangeOfPhosphorus()
    {
        process::CrystalRun r = phosphorus(100, 0, 0, 1, slowBuild() ? 5000 : 20000);
        r.amorphous = true;
        r.depth = 1e-6;
        QString why;
        const auto p = process::crystalImplant(r, {}, &why);
        QVERIFY2(p, qPrintable(why));
        const QString what = QStringLiteral("Rp %1 µm, ΔRp %2 µm (%3 ions, %4 s)").arg(p->Rp * 1e6).arg(p->dRp * 1e6).arg(p->ions).arg(p->ms / 1e3);
        qInfo("amorphous P at 100 keV: %s", qPrintable(what));
        QVERIFY2(std::abs(p->Rp * 1e6 - 0.131) <= 0.003 && std::abs(p->dRp * 1e6 - 0.047) <= 0.002, qPrintable(what));
        // Its ions all somewhere: backscattered, in the oxide, in the
        // silicon, past the depth followed.
        double all = 0;
        for (double c : p->counts) all += c;
        QCOMPARE(long(std::llround(all)) + p->back + p->beyond, p->ions);
    }

    // Channelling: on the ⟨100⟩ axis the profile reaches far deeper at 1e-3
    // of its peak than off every axis and plane (7° tilt, 22° rotation). The
    // crystal's axes for each surface: the ⟨100⟩ axis entered from (110) at
    // 45° the same profile along the path as from (100) on axis; on each
    // surface's own axis deeper than off it.
    void channellingOnAxisGoesDeeper()
    {
        const long n = slowBuild() ? 4000 : 20000;
        QString why;
        auto run = [&](double tilt, double rot, const QString& orientation, bool bare = false) {
            process::CrystalRun r = phosphorus(100, tilt, rot, 1, n);
            r.orientation = orientation;
            r.depth = 2e-6;
            // (Bare: no oxide - at 45° it is √2 thicker along the path.)
            if (bare) r.cover.clear();
            return process::crystalImplant(r, {}, &why);
        };
        const auto on = run(0, 0, QStringLiteral("100")), off = run(7, 22, QStringLiteral("100"));
        QVERIFY2(on && off, qPrintable(why));
        const QString what = QStringLiteral("1e-3 of the peak: on <100> %1 µm, 7°/22° %2 µm; Rp %3, %4").arg(on->tail * 1e6).arg(off->tail * 1e6).arg(on->Rp * 1e6).arg(off->Rp * 1e6);
        qInfo("%s", qPrintable(what));
        QVERIFY2(on->tail > 1.3 * off->tail && on->Rp > 1.5 * off->Rp, qPrintable(what));
        const double k = std::cos(M_PI / 4);
        const auto side = run(45, 0, QStringLiteral("110"), true), bare = run(0, 0, QStringLiteral("100"), true);
        QVERIFY2(side && bare, qPrintable(why));
        const QString along = QStringLiteral("<100> along the path from (100) Rp %1 µm, tail %2; from (110) at 45° %3, %4").arg(bare->Rp * 1e6).arg(bare->tail * 1e6).arg(side->Rp / k * 1e6).arg(side->tail / k * 1e6);
        qInfo("%s", qPrintable(along));
        QVERIFY2(std::abs(side->Rp / k / bare->Rp - 1) < 0.04 && std::abs(side->tail / k / bare->tail - 1) < 0.06, qPrintable(along));
        // (On (110) its own axis reaches barely deeper at 1e-3 of the peak
        // than off it - the model's ⟨110⟩ channelling weak, said in its
        // warning -, within a few thousand ions' noise: its Rp, 40 % deeper.)
        for (const QString& o : {QStringLiteral("110"), QStringLiteral("111")}) {
            const auto a = run(0, 0, o), b = run(7, 22, o);
            QVERIFY2(a && b && a->Rp > 1.2 * b->Rp,
                     qPrintable(QStringLiteral("(%1): on axis tail %2 µm Rp %3, off %4, %5").arg(o).arg(a ? a->tail * 1e6 : 0).arg(a ? a->Rp * 1e6 : 0).arg(b ? b->tail * 1e6 : 0).arg(b ? b->Rp * 1e6 : 0)));
        }
    }

    // The same seed the same ions, on one thread or many; another seed others.
    void aSeedIsItsProfile()
    {
        process::CrystalRun r = phosphorus(50, 0, 0, 1, 1500);
        r.depth = 1e-6;
        QString why;
        r.threads = 1;
        const auto one = process::crystalImplant(r, {}, &why);
        r.threads = 7;
        const auto many = process::crystalImplant(r, {}, &why);
        process::clearCrystalCache();
        const auto again = process::crystalImplant(r, {}, &why);
        r.seed = 2;
        const auto other = process::crystalImplant(r, {}, &why);
        QVERIFY2(one && many && again && other, qPrintable(why));
        QVERIFY(one->counts == many->counts && one->sumX2 == many->sumX2 && one->back == many->back);
        QVERIFY(again->counts == one->counts);
        QVERIFY(other->counts != one->counts);
    }

    // Through a flow: the dose kept - in the silicon and its oxide, plus
    // what was backscattered (said), the dose to 1e-6 (the lateral spread's
    // tails past 5σ left out, as every implant's) -; the oxide holds
    // what the Monte Carlo stopped in its cover; the report the run's.
    void aFlowKeepsItsDose()
    {
        const Model m = implantFlow(QStringLiteral(R"("energy": "50", "dose": "1e13", "profile": "crystal", "ions": "3000")"), 1.0);
        const ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const QString s = said(r);
        QVERIFY2(s.contains(QLatin1String("Monte Carlo (crystal) in Silicon (100)")) && s.contains(QLatin1String("3000 ions")), qPrintable(s));
        process::CrystalRun run = flowsRun(m, 50, 0, 0, 3000);
        QString why;
        bool cached = false;
        const auto p = process::crystalImplant(run, {}, &why, &cached);
        QVERIFY2(p && cached, qPrintable(why + QStringLiteral(" - not the flow's run: ") + run.key()));
        const double back = double(p->back) / double(p->ions);
        const double held = dose(r, QStringLiteral("P"));
        QVERIFY2(std::abs((held + back * 1e13) / 1e13 - 1) < 1e-6,
                 qPrintable(QStringLiteral("held %1, backscattered %2, off by %3: %4").arg(held, 0, 'g', 12).arg(back * 1e13, 0, 'g', 12).arg((held + back * 1e13) / 1e13 - 1).arg(s)));
        double inCover = 0;
        for (int k = 0; k < p->coverBins; ++k) inCover += p->counts[std::size_t(k)];
        QVERIFY2(back == 0 || s.contains(QStringLiteral("%1 % backscattered").arg(formatNumber(100 * back, 3))), qPrintable(s));
        // The silicon's share, said, as the run's.
        QVERIFY2(s.contains(QStringLiteral("%1 % in Silicon").arg(formatNumber(100 * p->inCrystal, 3))), qPrintable(s + QStringLiteral(" (%1)").arg(p->inCrystal)));
        QVERIFY(inCover >= 0 && p->remaining(0) == 1 - back);
    }

    // A tilted crystal implant: its profile along the normal is the run's -
    // the tilt in the run, not projected again by the march (30°: cos 30°
    // would take 13 % off its depth).
    void theTiltIsNotProjectedTwice()
    {
        const Model m = implantFlow(QStringLiteral(R"("energy": "100", "dose": "1e13", "tilt": "30", "rotation": "22", "profile": "crystal", "ions": "%1")")
                                        .arg(slowBuild() ? 2000 : 6000),
                                    1.6, QStringLiteral("100"), QStringLiteral("Silicon"), 3.0);
        const ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        process::CrystalRun run = flowsRun(m, 100, 30, 22, slowBuild() ? 2000 : 6000);
        QString why;
        bool cached = false;
        const auto p = process::crystalImplant(run, {}, &why, &cached);
        QVERIFY2(p && cached, qPrintable(why));
        const auto [Rp, dRp] = moments(column(r, QStringLiteral("P"), 2.0));
        const QString what = QStringLiteral("the flow's Rp %1 µm, ΔRp %2; the run's %3, %4; %5").arg(Rp).arg(dRp).arg(p->Rp * 1e6).arg(p->dRp * 1e6).arg(said(r));
        QVERIFY2(std::abs(Rp / (p->Rp * 1e6) - 1) < 0.03 && std::abs(dRp / (p->dRp * 1e6) - 1) < 0.05, qPrintable(what));
        QVERIFY2(said(r).contains(QStringLiteral("rotated 22°")), qPrintable(said(r)));
        // Given through parameters with their units (tilt = 30[deg]): the
        // same angles, the same run - they were taken as 30° in radians
        // read as degrees (0.524°).
        const Model viaParameters = flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "3"},
            {"type": "deposit", "tag": "nox", "label": "native", "material": "Silicon dioxide", "thickness": "1.6[nm]", "mode": "planar"},
            {"type": "implant", "tag": "imp", "label": "P_implant", "species": "P", "energy": "100", "dose": "1e13", "tilt": "tilt", "rotation": "rot", "profile": "crystal", "ions": "%1"})")
                                      .arg(slowBuild() ? 2000 : 6000),
                                  QStringLiteral(R"("width": "3", "grid": "0.2[nm]", "dx": "0.25", "dy": "1[nm]", "depth": "1.6")"),
                                  QStringLiteral(R"([{"name": "t0", "expression": "30[deg]"}, {"name": "tilt", "expression": "t0"}, {"name": "rot", "expression": "22[deg]"}])"));
        const ProcessRun q = runFlow(viaParameters);
        QVERIFY2(q.ok() && said(q).contains(QStringLiteral("tilted 30°")) && said(q).contains(QStringLiteral("rotated 22°")) && said(q).contains(QLatin1String("from the cache")),
                 qPrintable(said(q)));
    }

    // Against Cai et al.'s SIMS, through the flow (silicon (100), 16 Å of
    // oxide, 1° divergence, nothing fitted): the review's measure, rms in
    // decades. Its dual Pearsons fitted to the runs had 0.07, 0.07, 0.44,
    // 0.87, 0.10, 0.13; the histogram itself (40k ions here) 0.10, 0.07,
    // 0.71, 1.01, 0.12, 0.20 - on axis at 100 and 200 keV the model's tail
    // ends short of the deepest SIMS points, which a fit's tail ran on past.
    // The Pearson of the tables (no channelling) misses each by more. Each
    // figure a row, each its own of QtTest's 300 s: the six in one went past
    // it on CI's macOS runner of three processors (fig1 to fig6 252 s there,
    // 17 s on an M2 Ultra; fig7 not reached).
    void againstTheSims_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<double>("bound");
        const QList<std::pair<const char*, double>> figures = {{"fig1", 0.15}, {"fig2", 0.15}, {"fig3", 0.8}, {"fig4", 1.15}, {"fig6", 0.2}, {"fig7", 0.25}};
        for (const auto& [name, bound] : figures) {
            if (slowBuild() && QByteArray(name) != "fig1" && QByteArray(name) != "fig6") continue;
            QTest::newRow(name) << QString::fromLatin1(name) << bound;
        }
    }
    void againstTheSims()
    {
        QFETCH(QString, name);
        QFETCH(double, bound);
        QFile f(QStringLiteral(QUCS_TEST_DATA "/cai1998_phosphorus_sims.csv"));
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        struct Case {
            double keV = 0, dose = 0, tilt = 0, rot = 0;
            std::vector<std::pair<double, double>> sims;
        };
        std::map<QString, Case> cases;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            const QStringList w = line.split(QLatin1Char(','));
            Case& c = cases[w[0]];
            c.keV = w[1].toDouble();
            c.dose = w[2].toDouble();
            c.tilt = w[3].toDouble();
            c.rot = w[4].toDouble();
            c.sims.emplace_back(w[5].toDouble(), w[6].toDouble());
        }
        QCOMPARE(int(cases.size()), 6);
        QVERIFY(cases.count(name));
        const Case& c = cases.at(name);
        const double depth = c.keV > 150 ? 2.0 : c.keV > 30 ? 1.4 : 0.6;
        const QString props = QStringLiteral(R"("energy": "%1", "dose": "%2", "tilt": "%3", "rotation": "%4")").arg(c.keV).arg(c.dose).arg(c.tilt).arg(c.rot);
        QElapsedTimer clock;
        clock.start();
        // (Tilted: a window wide enough that the column read is fed by
        // paths that came in through its top.)
        const double width = c.tilt > 0 ? 2.5 : 0.2, x = c.tilt > 0 ? 2.0 : -1;
        const long ions = slowBuild() ? 10000 : 40000;
        const ProcessRun mc = runFlow(implantFlow(props + QStringLiteral(R"(, "profile": "crystal", "ions": "%1")").arg(ions), depth,
                                                  QStringLiteral("100"), QStringLiteral("Silicon"), width));
        const double ms = double(clock.elapsed());
        const ProcessRun lss = runFlow(implantFlow(props, depth, QStringLiteral("100"), QStringLiteral("Silicon"), width));
        QVERIFY2(mc.ok() && lss.ok(), qPrintable(said(mc) + said(lss)));
        const auto [rms, worst] = decades(column(mc, QStringLiteral("P"), x), c.sims);
        const auto [rmsL, worstL] = decades(column(lss, QStringLiteral("P"), x), c.sims);
        qInfo("%s, %g keV %g°/%g°: crystal %.2f / %.2f decades (%.1f s), LSS tables %.2f / %.2f", qPrintable(name), c.keV, c.tilt, c.rot, rms, worst, ms / 1e3, rmsL, worstL);
        QVERIFY2(rms < bound && rms < rmsL - 0.3, qPrintable(QStringLiteral("%1: %2 decades rms (%3 the tables'): %4").arg(name).arg(rms).arg(rmsL).arg(said(mc))));
    }

    // Built again, the flow takes its run from the cache (said); stopped
    // through its progress, the run stops at once - cancelled.
    void itsCacheAndItsCancel()
    {
        process::clearCrystalCache();
        const Model m = implantFlow(QStringLiteral(R"("energy": "15", "dose": "1e13", "profile": "crystal", "ions": "2000", "seed": "7")"), 0.6);
        const ProcessRun first = runFlow(m);
        QElapsedTimer clock;
        clock.start();
        const ProcessRun second = runFlow(m);
        QVERIFY2(first.ok() && second.ok(), qPrintable(said(first) + said(second)));
        QVERIFY2(!said(first).contains(QLatin1String("from the cache")) && said(second).contains(QLatin1String("from the cache")), qPrintable(said(first) + said(second)));
        QVERIFY(column(first, QStringLiteral("P")) == column(second, QStringLiteral("P")));
        // Stopped in the Monte Carlo: its step's first word goes, then false.
        int told = 0;
        clock.restart();
        const ProcessRun stopped = runFlow(implantFlow(QStringLiteral(R"("energy": "200", "dose": "1e13", "profile": "crystal", "ions": "5000000")"), 2.0),
                                           [&told](double, const QString& what) { return !what.contains(QLatin1String("P_implant")) || told++ < 1; });
        QVERIFY2(!stopped.ok() && stopped.error.contains(QLatin1String("cancelled")) && clock.elapsed() < 20000,
                 qPrintable(stopped.error + QStringLiteral(" after %1 ms").arg(clock.elapsed())));
    }

    // What it holds for (item 9): silicon alone, Z from 3; phosphorus into
    // (100) from 15 to 200 keV said nothing of; boron, another surface, an
    // energy outside, warned of; too fast or too slow, refused.
    void whatItHoldsFor()
    {
        auto trust = [](int Z, double M, double keV, const QString& orientation, const process::Target& crystal, QStringList* w) {
            process::CrystalRun r;
            r.Z1 = Z;
            r.M1 = M;
            r.energy = keV * 1e3;
            r.orientation = orientation;
            r.crystal = crystal;
            return process::crystalTrust(r, w);
        };
        QStringList w;
        QVERIFY(trust(15, 30.97, 100, QStringLiteral("100"), silicon(), &w).isEmpty() && w.isEmpty());
        QVERIFY(trust(15, 30.97, 15, QStringLiteral("100"), silicon(), &w).isEmpty() && w.isEmpty());
        QVERIFY(trust(5, 11.0, 40, QStringLiteral("100"), silicon(), &w).isEmpty() && w.join(QString()).contains(QLatin1String("not been checked")));
        w.clear();
        QVERIFY(trust(15, 30.97, 100, QStringLiteral("110"), silicon(), &w).isEmpty() && w.join(QString()).contains(QLatin1String("(100)")));
        w.clear();
        QVERIFY(trust(15, 30.97, 400, QStringLiteral("100"), silicon(), &w).isEmpty() && w.join(QString()).contains(QLatin1String("15 to 200")));
        w.clear();
        QVERIFY(trust(5, 11.0, 600, QStringLiteral("100"), silicon(), &w).isEmpty() && w.join(QString()).contains(QLatin1String("Fermi velocity")));
        QVERIFY(trust(5, 11.0, 1500, QStringLiteral("100"), silicon(), &w).contains(QLatin1String("too fast")));
        QVERIFY(trust(15, 30.97, 0.5, QStringLiteral("100"), silicon(), &w).contains(QLatin1String("binary collisions")));
        QVERIFY(trust(2, 4.0, 50, QStringLiteral("100"), silicon(), &w).contains(QStringLiteral("3 ≤ Z ≤ 92")));
        process::Target gaas;
        gaas.parts = {{31, 69.72, 0.5}, {33, 74.92, 0.5}};
        gaas.atoms = 4.42e28;
        gaas.formula = QStringLiteral("Ga As");
        QVERIFY(trust(14, 28.0, 100, QStringLiteral("100"), gaas, &w).contains(QLatin1String("silicon's lattice")));
        // Through a flow: a gallium arsenide wafer refused, said where.
        const ProcessRun inp = runFlow(implantFlow(QStringLiteral(R"("energy": "100", "dose": "1e13", "profile": "crystal", "ions": "100")"), 1.0,
                                                   QStringLiteral("100"), QStringLiteral("Gallium arsenide")));
        QVERIFY2(!inp.ok() && inp.error.contains(QLatin1String("silicon's lattice")), qPrintable(said(inp)));
        // And a Pearson profile through the same: as before.
        QVERIFY2(runFlow(implantFlow(QStringLiteral(R"("energy": "100", "dose": "1e13")"), 1.0, QStringLiteral("100"), QStringLiteral("Gallium arsenide"))).ok(), "GaAs, Pearson");
    }

    // F2: a dual Pearson's report says its channelled part. F3: moments
    // given along the normal with a tilt are not projected again - its peak
    // at their Rp, not Rp cos 30°.
    void dualReportAndMomentsAlongTheNormal()
    {
        const ProcessRun dual = runFlow(implantFlow(QStringLiteral(R"("energy": "100", "dose": "1e13", "profile": "dual",
            "channel": [["Silicon", "0.4338", "0.40411[um]", "0.14506[um]", "-0.376", "3.269"]])")));
        QVERIFY2(dual.ok() && said(dual).contains(QStringLiteral("channelled 43.4 %: Rp 0.404 µm, ΔRp 0.145 µm")), qPrintable(said(dual)));
        auto peak = [](const ProcessRun& r) {
            double best = 0, at = 0;
            for (const auto& [z, c] : column(r, QStringLiteral("P"), 2.0))
                if (c > best) {
                    best = c;
                    at = z;
                }
            return at;
        };
        const QString given = QStringLiteral(R"("energy": "100", "dose": "1e13", "tilt": "30", "profile": "gaussian",
            "moments": [["Silicon", "0.2[um]", "0.02[um]", "0", "3", "0.03[um]"]])");
        // (Bare silicon: under even a native oxide Ryssel's dose matching moves
        // a profile this narrow - a finding of its own, the write-up's.)
        auto bare = [](const QString& implant) {
            return flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "3"},
                {"type": "implant", "tag": "imp", "label": "P_implant", "species": "P", %1})").arg(implant),
                             QStringLiteral(R"("width": "3", "dx": "0.25", "dy": "1[nm]", "depth": "0.6")"));
        };
        const ProcessRun beam = runFlow(bare(given));
        const ProcessRun normal = runFlow(bare(given + QStringLiteral(R"(, "normal": true)")));
        QVERIFY2(beam.ok() && normal.ok(), qPrintable(said(beam) + said(normal)));
        const QString what = QStringLiteral("peaks: along the beam %1 µm, along the normal %2 µm; %3").arg(peak(beam)).arg(peak(normal)).arg(said(normal));
        QVERIFY2(std::abs(peak(beam) - 0.2 * std::cos(M_PI / 6)) < 0.004 && std::abs(peak(normal) - 0.2) < 0.004, qPrintable(what));
        QVERIFY2(said(normal).contains(QLatin1String("along the normal")), qPrintable(said(normal)));
    }
};

QTEST_GUILESS_MAIN(TestFemCrystal)
#include "test_fem_crystal.moc"
