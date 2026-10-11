/*
 * test_fem_rays.cpp - the multiphysics solver's geometrical optics against
 *                     what is known: Snell's law and Fresnel's split, total
 *                     reflection, a lens's focus, a graded-index rod's
 *                     period, absorption and the power it leaves, a prism's
 *                     dispersion
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_problem.h"
#include "fem_results.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QtTest>

#include <cmath>
#include <map>

using namespace qucs_s::fem;

namespace {

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

std::vector<RayPath> paths(const Solution& s, const QString& tag = QStringLiteral("gop"))
{
    return rayPaths(s.fields()[std::size_t(s.fieldIndex(tag))]);
}

/// Where a path crosses y = \a y0 the \a k-th time (x in m), NaN if never.
double crossing(const RayPath& p, double y0, int k = 1)
{
    int seen = 0;
    for (std::size_t i = 0; i + 1 < p.points.size(); ++i) {
        const double ya = p.points[i][1] - y0, yb = p.points[i + 1][1] - y0;
        if ((ya < 0) != (yb < 0) && ya != yb) {
            if (++seen == k) return p.points[i][0] + (p.points[i + 1][0] - p.points[i][0]) * ya / (ya - yb);
        }
    }
    return NAN;
}

/// Air (x < 0) against glass n (x > 0), each 10 mm by 20; a ray from (-5,
/// -5 tan θ) at θ to the normal; detectors on the far walls.
QByteArray interface(double n1, double n2, double theta, const QString& physics = {})
{
    const double y0 = -5 * std::tan(theta * M_PI / 180);
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "mm",
        "geometry": [{"type": "rectangle", "name": "left", "position": ["-10", "-10"], "size": ["10", "20"]},
                     {"type": "rectangle", "name": "right", "position": ["0", "-10"], "size": ["10", "20"]}],
        "materials": [{"label": "one", "n": "%1", "selection": {"only": ["left"]}}, {"label": "two", "n": "%2", "selection": {"only": ["right"]}}],
        "physics": [{"type": "rays", "tag": "gop" %5, "features": [
          {"type": "point", "position": ["-5", "%3"], "angle": "%4", "spread": "0", "count": 1, "P0": "1[W]"},
          {"type": "detector", "name": "back", "stop": true, "selection": {"of": ["left"], "side": "left"}},
          {"type": "detector", "name": "back2", "stop": true, "selection": {"of": ["left"], "side": "bottom"}},
          {"type": "detector", "name": "back3", "stop": true, "selection": {"of": ["left"], "side": "top"}},
          {"type": "detector", "name": "through", "stop": true, "selection": {"of": ["right"], "side": "right"}},
          {"type": "detector", "name": "through2", "stop": true, "selection": {"of": ["right"], "side": "top"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
        .arg(n1)
        .arg(n2)
        .arg(y0, 0, 'g', 17)
        .arg(theta)
        .arg(physics)
        .toUtf8();
}

} // namespace

class TestFemRays : public QObject
{
    Q_OBJECT

private slots:
    void snellAndFresnel()
    {
        // 30° into n = 1.5: refracted at asin(sin 30°/1.5), the rest
        // reflected as Fresnel's s and p average (unpolarized).
        const Run r = run(modelOf(interface(1, 1.5, 30)));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const double ti = 30 * M_PI / 180, tt = std::asin(std::sin(ti) / 1.5);
        const double rs = (std::cos(ti) - 1.5 * std::cos(tt)) / (std::cos(ti) + 1.5 * std::cos(tt));
        const double rp = (1.5 * std::cos(ti) - std::cos(tt)) / (1.5 * std::cos(ti) + std::cos(tt));
        const double R = (rs * rs + rp * rp) / 2;
        const double through = global(s, QStringLiteral("gop.P_through")) + global(s, QStringLiteral("gop.P_through2"));
        const double back = global(s, QStringLiteral("gop.P_back")) + global(s, QStringLiteral("gop.P_back2"))
                            + global(s, QStringLiteral("gop.P_back3"));
        const double y = global(s, QStringLiteral("gop.y_through"));
        qInfo("Snell: through %.9f W (1 - R %.9f), back %.9f (R %.9f); at y %.9f mm (%.9f)", through, 1 - R, back, R, y * 1e3, 10 * std::tan(tt));
        QVERIFY(std::abs(through - (1 - R)) < 1e-12);
        QVERIFY(std::abs(back - R) < 1e-12);
        QVERIFY(std::abs(y * 1e3 - 10 * std::tan(tt)) < 1e-9);
        QCOMPARE(global(s, QStringLiteral("gop.N")), 1.0);
        QCOMPARE(global(s, QStringLiteral("gop.Ntraced")), 2.0);
        // s alone, p alone: their own coefficients; no reflected rays when asked.
        const Run p = run(modelOf(interface(1, 1.5, 30, QStringLiteral(R"(, "reflections": false)"))));
        QVERIFY2(p.error.isEmpty(), qPrintable(p.error));
        QCOMPARE(global(*p.solution, QStringLiteral("gop.Ntraced")), 1.0);
    }

    void totalReflection()
    {
        // From n = 1.5 into air at 50° (past 41.8°): all of it reflected.
        const Run r = run(modelOf(interface(1.5, 1, 50)));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const double through = global(s, QStringLiteral("gop.P_through")) + global(s, QStringLiteral("gop.P_through2"));
        const double back = global(s, QStringLiteral("gop.P_back")) + global(s, QStringLiteral("gop.P_back2"))
                            + global(s, QStringLiteral("gop.P_back3"));
        qInfo("total reflection: back %.12f W, through %.3g", back, through);
        QVERIFY(std::abs(back - 1) < 1e-12);
        QVERIFY(!(through > 0));
    }

    void aLensFocus()
    {
        // A plano-convex lens (R 50 mm, 5 mm thick, n 1.5), its curve to the
        // beam: f = R/(n - 1) = 100 mm, the focus f - t/n behind its flat
        // face; marginal rays focus nearer (spherical aberration).
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "name": "air", "position": ["-20", "-30"], "size": ["170", "60"]},
                         {"type": "circle", "name": "sphere", "center": ["50", "0"], "radius": "50"},
                         {"type": "rectangle", "name": "cut", "position": ["0", "-20"], "size": ["5", "40"]},
                         {"type": "intersection", "name": "lens", "input": ["sphere", "cut"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}, {"label": "glass", "n": "1.5", "selection": {"only": ["lens"]}}],
            "physics": [{"type": "rays", "tag": "gop", "reflections": false, "features": [
              {"type": "beam", "start": ["-10", "-1"], "end": ["-10", "1"], "angle": "0", "count": 10, "P0": "1[W]"},
              {"type": "beam", "start": ["-10", "14"], "end": ["-10", "15"], "angle": "0", "count": 1, "P0": "1[W]"}]}],
            "mesh": {"size": "normal"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const std::vector<RayPath> ps = paths(*r.solution);
        QCOMPARE(int(ps.size()), 11);
        double sum = 0;
        for (int k = 0; k < 10; ++k) sum += crossing(ps[std::size_t(k)], 0);
        const double paraxial = sum / 10 * 1e3, marginal = crossing(ps[10], 0) * 1e3;
        const double expected = 5 + 100 - 5 / 1.5;
        qInfo("lens: paraxial focus at %.4f mm (%.4f), a ray 14.5 mm off the axis at %.3f mm", paraxial, expected, marginal);
        QVERIFY2(std::abs(paraxial - expected) < 0.05, qPrintable(QString::number(paraxial)));
        QVERIFY(marginal < paraxial - 1);
    }

    void aGradedRod()
    {
        // n = n0 √(1 - (g y)²): a ray launched along x at y0 oscillates with
        // period 2π √(1 - (g y0)²)/g - here 62.517 mm - crossing the axis at
        // a quarter and three quarters of it.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "name": "rod", "position": ["0", "-3"], "size": ["100", "6"]}],
            "physics": [{"type": "rays", "tag": "gop", "features": [
              {"type": "medium", "source": "user", "n": "1.5*sqrt(1 - (0.1[1/mm]*y)^2)", "selection": {"all": true}},
              {"type": "point", "position": ["0.001", "1"], "angle": "0", "spread": "0", "count": 1, "P0": "1[W]"}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.5"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const std::vector<RayPath> ps = paths(*r.solution);
        QCOMPARE(int(ps.size()), 1);
        const double period = 2 * M_PI * std::sqrt(1 - 0.01) / 0.1;
        const double first = crossing(ps[0], 0, 1) * 1e3, second = crossing(ps[0], 0, 2) * 1e3;
        qInfo("graded rod: the axis crossed at %.4f, %.4f mm (%.4f, %.4f); %d points", first, second, period / 4, 3 * period / 4,
              int(ps[0].points.size()));
        QVERIFY2(std::abs(first - period / 4) < 0.02, qPrintable(QString::number(first)));
        QVERIFY2(std::abs(second - 3 * period / 4) < 0.05, qPrintable(QString::number(second)));
    }

    void absorption()
    {
        // A slab 10 mm thick, α = 4πκ/λ = 100 /m, coated (no reflection): e^-1
        // through; the rest left in it - gop.Q integrated over it.
        const double kappa = 100 * 550e-9 / (4 * M_PI);
        const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm", "thickness": "1[mm]",
            "geometry": [{"type": "rectangle", "name": "air", "position": ["-5", "-5"], "size": ["25", "10"]},
                         {"type": "rectangle", "name": "slab", "position": ["0", "-5"], "size": ["10", "10"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}},
                          {"label": "glass", "n": "1.5", "kappa": "%1", "selection": {"only": ["slab"]}}],
            "physics": [{"type": "rays", "tag": "gop", "features": [
              {"type": "beam", "start": ["-4", "-2"], "end": ["-4", "2"], "angle": "0", "count": 8, "P0": "2[W]"},
              {"type": "coating", "selection": {"between": [["air"], ["slab"]]}},
              {"type": "detector", "name": "out", "stop": true, "selection": {"of": ["air"], "side": "right"}}]}],
            "mesh": {"size": "normal"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                                    .arg(kappa, 0, 'g', 17)
                                    .toUtf8();
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const double out = global(s, QStringLiteral("gop.P_out")), absorbed = global(s, QStringLiteral("gop.Pabs"));
        Node integral;
        integral.type = QStringLiteral("integral");
        integral.props.insert(QStringLiteral("expression"), QStringLiteral("gop.Q"));
        // (Over the absorbing slab: where heat or a semiconductor would take it.)
        integral.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("slab")}}});
        const DerivedResult d = evaluateDerived(const_cast<Solution&>(s), integral);
        QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
        const double integrated = d.values.at(0).value * 1e-3;   // (per unit depth: the model 1 mm thick)
        qInfo("absorption: %.9f W through (2 e^-1 %.9f), %.9f absorbed, gop.Q's integral %.9f W", out, 2 * std::exp(-1.0), absorbed, integrated);
        QVERIFY(std::abs(out - 2 * std::exp(-1.0)) < 1e-9);
        QVERIFY(std::abs(absorbed - 2 * (1 - std::exp(-1.0))) < 1e-9);
        QVERIFY(std::abs(integrated / absorbed - 1) < 1e-6);
    }

    void mirrorsAndReleases()
    {
        // A mirror at 45° (R 0.9) turns a beam up into a detector it passes
        // (stop off) and the wall beyond; a point's fan of 90° all reaches the
        // mirror; rays from a boundary go along its normal; a Gaussian beam's
        // rays carry exp(-2r²/w²) of the power.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "name": "room", "position": ["-20", "-20"], "size": ["40", "40"]},
                         {"type": "polygon", "name": "glass", "points": [["5", "-5"], ["15", "-5"], ["15", "5"]]},
                         {"type": "rectangle", "name": "pane", "position": ["-20", "15"], "size": ["40", "2"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
            "physics": [{"type": "rays", "tag": "gop", "reflections": false, "features": [
              {"type": "mirror", "R": "0.9", "selection": {"between": [["room"], ["glass"]]}},
              {"type": "beam", "start": ["-15", "-2"], "end": ["-15", "2"], "angle": "0", "count": 9, "P0": "1[W]",
               "profile": "gaussian", "waist": "2[mm]"},
              {"type": "detector", "name": "up", "selection": {"between": [["room"], ["pane"]]}},
              {"type": "detector", "name": "top", "stop": true, "selection": {"of": ["pane"], "side": "top"}}]}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const double up = global(s, QStringLiteral("gop.P_up")), top = global(s, QStringLiteral("gop.P_top"));
        qInfo("mirror: %.6f W through the pane's face (0.9), %.6f at the top; %g rays there", up, top, global(s, QStringLiteral("gop.N_top")));
        QVERIFY(std::abs(up - 0.9) < 1e-9);
        QVERIFY(std::abs(top - 0.9) < 1e-9);
        // The beam's rays: Gaussian weights, the middle the strongest.
        const std::vector<RayPath> ps = paths(s);
        QCOMPARE(int(ps.size()), 9);
        double sum = 0;
        for (const RayPath& p : ps) sum += p.points.front()[2];
        QVERIFY(std::abs(sum - 1) < 1e-12);
        const double y0 = ps[0].points.front()[1] * 1e3, y4 = ps[4].points.front()[1] * 1e3;
        QVERIFY(std::abs(ps[0].points.front()[2] / ps[4].points.front()[2] - std::exp(-2 * (y0 - y4) * (y0 - y4) / 4)) < 1e-9);
        // The mirror's reflection is up: the paths end at y = 17 mm.
        for (const RayPath& p : ps) QVERIFY(std::abs(p.points.back()[1] * 1e3 - 17) < 1e-9);
        // A fan from a point and rays from a boundary.
        const QByteArray json2 = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "name": "room", "position": ["-10", "-10"], "size": ["20", "20"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
            "physics": [{"type": "rays", "tag": "gop", "features": [
              {"type": "point", "position": ["0", "0"], "angle": "0", "spread": "45", "count": 5, "P0": "1[W]"},
              {"type": "boundary", "count": 4, "P0": "2[W]", "selection": {"of": ["room"], "side": "bottom"}},
              {"type": "detector", "name": "right", "stop": true, "selection": {"of": ["room"], "side": "right"}},
              {"type": "detector", "name": "top", "stop": true, "selection": {"of": ["room"], "side": "top"}}]}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run f = run(modelOf(json2));
        QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
        const double right = global(*f.solution, QStringLiteral("gop.P_right")), topP = global(*f.solution, QStringLiteral("gop.P_top"));
        qInfo("releases: %.6f W at the right (the fan's 1), %.6f at the top (the boundary's 2)", right, topP);
        QVERIFY(std::abs(right - 1) < 1e-12);
        QVERIFY(std::abs(topP - 2) < 1e-12);
        QCOMPARE(global(*f.solution, QStringLiteral("gop.N_right")), 5.0);
        // The fan's rays at -45°..45°: at the right wall y = 10 tan θ.
        const std::vector<RayPath> fan = paths(*f.solution);
        QVERIFY(std::abs(fan[0].points.back()[1] * 1e3 + 10) < 1e-9 && std::abs(fan[4].points.back()[1] * 1e3 - 10) < 1e-9);
    }

    void aPrismsDispersion()
    {
        // An equilateral prism of Cauchy's glass, n = 1.5 + 4200 nm²/λ²: a ray
        // at 45° to its first face leaves deviated by 45° + θe - 60°, θe from
        // Snell's law at each face - blue more than red.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "name": "air", "position": ["-30", "-30"], "size": ["60", "60"]},
                         {"type": "polygon", "name": "prism", "points": [["-10", "-10/sqrt(3)"], ["10", "-10/sqrt(3)"], ["0", "20/sqrt(3)"]]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}},
                          {"label": "glass", "n": "1.5 + 4200[nm^2]/lambda0^2", "selection": {"only": ["prism"]}}],
            "physics": [{"type": "rays", "tag": "gop", "reflections": false, "features": [
              {"type": "point", "position": ["-20", "-2.887"], "angle": "15", "spread": "0", "count": 1, "P0": "1[W]",
               "lambda0": "450[nm], 650[nm]"}]}],
            "mesh": {"size": "normal"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const std::vector<RayPath> ps = paths(*r.solution);
        QCOMPARE(int(ps.size()), 2);
        for (const RayPath& p : ps) {
            const double lam = p.lambda * 1e9;
            const double n = 1.5 + 4200 / (lam * lam);
            // The first face's normal is at 150° (its outward), so a ray at 15°
            // meets it at 45°.
            const double t1 = std::asin(std::sin(45 * M_PI / 180) / n), t2 = 60 * M_PI / 180 - t1;
            const double te = std::asin(n * std::sin(t2));
            const double deviation = 45 * M_PI / 180 + te - 60 * M_PI / 180;
            const auto& a = p.points[p.points.size() - 2];
            const auto& b = p.points.back();
            const double out = std::atan2(b[1] - a[1], b[0] - a[0]);
            qInfo("prism at %.0f nm: n %.5f, leaves at %.6f° (15° - deviation %.6f°)", lam, n, out * 180 / M_PI, 15 - deviation * 180 / M_PI);
            QVERIFY2(std::abs(out - (15 * M_PI / 180 - deviation)) < 1e-9, qPrintable(QString::number(out * 180 / M_PI)));
        }
    }

    void theExamples()
    {
        // The rays' examples, solved as they come, against what they say.
        const QString dir = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
        auto load = [&](const QString& name) {
            QFile f(dir + name);
            if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
            Model m = modelOf(f.readAll());
            m.fileName = dir + name;
            return m;
        };
        auto bk7 = [](double lambda) {
            const double l2 = lambda * lambda * 1e12;
            return std::sqrt(1 + 1.03961212 * l2 / (l2 - 0.00600069867) + 0.231792344 * l2 / (l2 - 0.0200179144) + 1.01046945 * l2 / (l2 - 103.560653));
        };
        QElapsedTimer timer;
        {
            // The lens: its paraxial focus f - t/n behind it; the margin's nearer.
            timer.start();
            const Run r = run(load(QStringLiteral("lens_focus.qfem")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const std::vector<RayPath> ps = paths(*r.solution);
            QCOMPARE(int(ps.size()), 31);
            const double n = bk7(587.6e-9), f = 50 / (n - 1), focus = 6 + f - 6 / n;
            const double paraxial = (crossing(ps[14], 0) + crossing(ps[16], 0)) / 2 * 1e3, marginal = crossing(ps[0], 0) * 1e3;
            const double rms = global(*r.solution, QStringLiteral("gop.rms_screen")) * 1e3;
            qInfo("lens: n %.5f, focus %.3f mm (f - t/n %.3f), the edge's at %.2f mm; spot %.3f mm rms; %lld ms", n, paraxial, focus, marginal, rms,
                  timer.elapsed());
            QVERIFY(std::abs(paraxial - focus) < 0.05);
            QVERIFY(marginal < paraxial - 2);
            QCOMPARE(global(*r.solution, QStringLiteral("gop.N_screen")), 31.0);
            QVERIFY(rms > 0.1 && rms < 3);
        }
        {
            // The prism: each ray leaves as Snell's law at each face says, violet bent more.
            timer.start();
            const Run r = run(load(QStringLiteral("prism_dispersion.qfem")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            std::map<int, double> out;   // nm, the primary middle ray's exit angle
            for (const RayPath& p : paths(*r.solution)) {
                if (p.points.size() < 4 || p.status != 1) continue;
                const auto& a = p.points[p.points.size() - 2];
                const auto& b = p.points.back();
                const double angle = std::atan2(b[1] - a[1], b[0] - a[0]) * 180 / M_PI;
                const int nm = int(std::lround(p.lambda * 1e9));
                // (The refracted ray of the beam's middle: the second of each three.)
                if (p.index % 3 == 2 && !out.count(nm)) out[nm] = angle;
            }
            QCOMPARE(int(out.size()), 7);
            for (const auto& [nm, angle] : out) {
                const double n = bk7(nm * 1e-9);
                const double ti = 43 * M_PI / 180, t1 = std::asin(std::sin(ti) / n), t2 = 60 * M_PI / 180 - t1;
                const double te = std::asin(n * std::sin(t2));
                const double expected = 13 - (43 + te * 180 / M_PI - 60);
                QVERIFY2(std::abs(angle - expected) < 1e-6, qPrintable(QStringLiteral("%1 nm: %2 against %3").arg(nm).arg(angle).arg(expected)));
            }
            qInfo("prism: 400 nm leaves at %.3f°, 700 nm at %.3f° (%.2f° apart); %lld ms", out[400], out[700], out[700] - out[400], timer.elapsed());
        }
        {
            // The GRIN lens (sech: every ray one period): a quarter period long,
            // its spot on the far face a point.
            timer.start();
            const Run r = run(load(QStringLiteral("grin_lens.qfem")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double rms = global(*r.solution, QStringLiteral("gop.rms_face")) * 1e3;
            qInfo("GRIN lens: %g rays at the far face, rms %.4f mm (the beam 2.4 mm wide); %lld ms", global(*r.solution, QStringLiteral("gop.N_face")),
                  rms, timer.elapsed());
            QCOMPARE(global(*r.solution, QStringLiteral("gop.N_face")), 13.0);
            QVERIFY2(rms < 0.002, qPrintable(QString::number(rms)));
        }
        {
            // The thermal lens: the heat the rays leave, the power absorbed; the
            // spot narrowed as the power rises.
            timer.start();
            const Run r = run(load(QStringLiteral("thermal_lens.qfem")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QCOMPARE(int(r.study.solutions.size()), 3);
            std::vector<double> rms, tmax;
            for (const auto& s : r.study.solutions) {
                rms.push_back(global(*s, QStringLiteral("gop.rms_screen")) * 1e3);
                tmax.push_back(global(*s, QStringLiteral("ht.Tmax")) - 293.15);
                const double pin = global(*s, QStringLiteral("gop.Pin")), pabs = global(*s, QStringLiteral("gop.Pabs"));
                QVERIFY2(std::abs(pabs / pin - (1 - std::exp(-0.4))) < 0.01, qPrintable(QString::number(pabs / pin)));
                QVERIFY2(std::abs(global(*s, QStringLiteral("ht.P")) / pabs - 1) < 1e-3, qPrintable(QString::number(global(*s, QStringLiteral("ht.P")) / pabs)));
            }
            qInfo("thermal lens: spot %.4f, %.4f, %.4f mm rms; hottest %.2f, %.2f, %.2f K above; %lld ms", rms[0], rms[1], rms[2], tmax[0], tmax[1],
                  tmax[2], timer.elapsed());
            QVERIFY(rms[1] < rms[0] && rms[2] < rms[1]);
            QVERIFY(tmax[2] > tmax[1] && tmax[1] > tmax[0]);
        }
        {
            // The photodiode: its photocurrent near the ideal q λ/(hc) of the
            // power absorbed. (A sanitized build: a coarser mesh.)
            timer.start();
            Model m = load(QStringLiteral("photodiode.qfem"));
            if (slowBuild()) {
                Node* mesh = m.find(m.mesh().tag);
                mesh->props.insert(QStringLiteral("hmax"), QStringLiteral("2"));
                for (Node& size : mesh->children) size.props.insert(QStringLiteral("hmax"), QStringLiteral("0.3"));
            }
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double I = global(*r.solution, QStringLiteral("semi.I_a")), pabs = global(*r.solution, QStringLiteral("gop.Pabs"));
            const double ideal = 1.602176634e-19 * 633e-9 / (6.62607015e-34 * 299792458.0);
            qInfo("photodiode: I %.4g A, %.4g W absorbed: %.4f A/W (the ideal %.4f); %s; %lld ms", I, pabs, -I / 1e-3, ideal,
                  qPrintable(r.solution->log.filter(QStringLiteral("iteration")).join(QLatin1String("; "))), timer.elapsed());
            QVERIFY(std::abs(pabs / 1e-3 - 1) < 0.01);
            QVERIFY2(-I / 1e-3 > 0.75 * ideal && -I / 1e-3 < ideal, qPrintable(QString::number(-I / 1e-3)));
        }
    }
};

QTEST_GUILESS_MAIN(TestFemRays)
#include "test_fem_rays.moc"
