/*
 * fem_quantum_testing.h - what the quantum wells' tests share
 *                         (test_fem_quantum, test_fem_kp8,
 *                         test_fem_nitrides): a model of JSON, its first
 *                         study solved, its globals; the wells they build
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_QUANTUM_TESTING_H
#define QUCS_FEM_QUANTUM_TESTING_H

#include "fem_results.h"

#include <QtTest>

#include <cmath>
#include <functional>
#include <memory>
#include <optional>

namespace femtest {

using namespace qucs_s::fem;

inline constexpr double Q = 1.602176634e-19, Hbar = 1.054571817e-34, M0 = 9.1093837015e-31, KB = 1.380649e-23;
inline constexpr double HB2M = 0.0380998212;   // ħ²/2m0, eV nm²

inline bool slowBuild()
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

inline Model modelOf(const QByteArray& json)
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

inline Run run(const Model& model)
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

inline double global(const Solution& s, const QString& name)
{
    const std::optional<double> v = s.global(name);
    return v ? *v : NAN;
}

inline double eV(const Solution& s, const QString& name)
{
    return global(s, name) / Q;
}

inline double at(const Solution& s, const QString& expression, QPointF p)
{
    const Locator locator(s.mesh());
    const std::optional<double> v = evaluateAt(s, locator, expression, p);
    return v ? *v : NAN;
}

/// A root of f between a and b (f's signs differ there), by bisection.
inline double root(const std::function<double(double)>& f, double a, double b)
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
inline QByteArray box(const QString& width, const QString& height, const QString& physics, const QString& ec, const QString& steps,
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
inline QByteArray lighter(QByteArray json)
{
    if (!slowBuild()) return json;
    json.replace(R"("tag": "qw")", R"("tag": "qw", "h1d": "0.25[nm]")");
    json.replace(R"("type": "spectrum")", R"("type": "spectrum", "kpoints": 41)");
    return json;
}

/// A well of a compound between barriers of another (nm, along y).
inline QByteArray heterostructure(const QString& well, const QString& wellX, const QString& barrier, const QString& barrierX, const QString& lw,
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

/// A box 50 nm wide of a material whose remote bands leave the conduction
/// band a free electron's (1/me 1 + (Ep/3)(2/Eg + 1/(Eg + Δ)): A_c 1) and
/// the valence bands γ1' 1, γ2' γ3' 0 (γ1 1 + Ep/3Eg, γ2 γ3 Ep/6Eg) - Eg
/// 1.5 eV, Δ 0.34, Ep 25: its bands Kane's (kp8AgainstKane). Its line's
/// elements 0.25 nm, under the sanitizers 0.5.
inline constexpr double KaneEg = 1.5, KaneD = 0.34, KaneEp = 25, KaneMe = 1 / (1 + KaneEp / 3 * (2 / KaneEg + 1 / (KaneEg + KaneD)));

inline QByteArray kaneBox(const char* valence, const QString& step)
{
    const double g1 = 1 + KaneEp / (3 * KaneEg), g2 = KaneEp / (6 * KaneEg);
    QByteArray json = box(QStringLiteral("20"), QStringLiteral("50"),
                          QStringLiteral(R"(, "valence": "%1", "h1d": "%2[nm]")").arg(QLatin1String(valence), slowBuild() ? QStringLiteral("0.5") : QStringLiteral("0.25")),
                          QStringLiteral("0[eV]"), step);
    json.replace(R"("me": "0.067")", QStringLiteral(R"("me": "%1")").arg(KaneMe, 0, 'g', 12).toUtf8());
    json.replace(R"("mhh": "0.35")", QStringLiteral(R"("mhh": "%1")").arg(1 / (g1 - 2 * g2), 0, 'g', 12).toUtf8());
    json.replace(R"("mlh": "0.09")", QStringLiteral(R"("mlh": "%1")").arg(1 / (g1 + 2 * g2), 0, 'g', 12).toUtf8());
    json.replace(R"("nr": "3.6")", QStringLiteral(R"("nr": "3.6", "gamma3": "%1", "Delta": "%2[eV]")").arg(g2, 0, 'g', 12).arg(KaneD).toUtf8());
    return json;
}

/// Chuang and Chang's GaN (their 1997 paper's, Lumerical's 6x6 benchmark's)
/// as a user material, a wurtzite one; \a more added to it.
inline QString chuangGaN(const QString& more = QString())
{
    return QStringLiteral(R"("source": "user", "crystal": "wurtzite", "Ec": "3.542376[eV]", "Ev": "0.108256[eV]", "me": "0.2",)"
                          R"( "A": "-6.56, -0.91, 5.65, -2.83, -3.13, -4.86", "D": "-0.89, 4.27, 5.18, -2.59, -0.70, -5.10", "a1": "-4.08[eV]",)"
                          R"( "a2": "-4.08[eV]", "Dcr": "0.021[eV]", "Dso": "0.016[eV]", "Ep": "14.0[eV]", "C13": "106[GPa]", "C33": "398[GPa]",)"
                          R"( "nr": "3.25")")
           + more;
}

/// A box of one material (a feature's text), \a width nm along the axis.
inline QByteArray oneBox(const QString& material, const QString& physics, const QString& steps, const QString& width = QStringLiteral("10"))
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "nm",
        "geometry": [{"type": "rectangle", "name": "box", "size": ["10", "%1"]}],
        "physics": [{"type": "quantum", "tag": "qw" %2, "features": [{"type": "material", %3, "selection": {"all": true}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
      "studies": [{"type": "study", "steps": [%4]}]})json")
        .arg(width, physics, material, steps)
        .toUtf8();
}


} // namespace femtest

#endif // QUCS_FEM_QUANTUM_TESTING_H
