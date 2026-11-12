/*
 * rf25_optimize.cpp - what an RF 2.5D model's Parametric Sweep and
 *                     Optimization read of a solution (rf25_optimize.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_optimize.h"

#include "rf25_sparams.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

fem::Dim dimOf(const char* unit)
{
    const std::optional<fem::Unit> u = fem::parseUnit(QLatin1String(unit));
    return u ? u->dim : fem::Dim();
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

} // namespace

SolutionValues solutionValues(const ModelSolution& solution, const Model& model)
{
    SolutionValues out;
    // The names, each once, in the order first met.
    const auto add = [&out](const QString& name, const fem::Dim& dim) {
        if (out.index(name) >= 0) return int(out.index(name));
        out.names << name;
        out.dims << dim;
        for (QVector<double>& row : out.at) row << kNaN;
        return int(out.names.size()) - 1;
    };
    const fem::Dim hertz = dimOf("Hz"), ohm = dimOf("Ohm"), farad = dimOf("F"), henry = dimOf("H"), second = dimOf("s");
    if (solution.wave && solution.wave->ok()) {
        const FullWave& w = *solution.wave;
        const int nf = int(w.frequencies.size());
        out.frequencies = w.frequencies;
        out.at = QList<QVector<double>>(nf);
        const int np = nf > 0 ? int(w.s.at(0).size()) : 0;
        const int f = add(QStringLiteral("f"), hertz);
        for (int k = 0; k < nf; ++k) out.at[k][f] = w.frequencies.at(k);
        const auto both = [&](const QString& stem, int i, int j, const QString& tail, const fem::Dim& dim, const auto& valueAt) {
            QStringList names{QStringLiteral("%1%2_%3%4").arg(stem).arg(i + 1).arg(j + 1).arg(tail)};
            if (np <= 9) names.prepend(QStringLiteral("%1%2%3%4").arg(stem).arg(i + 1).arg(j + 1).arg(tail));
            for (const QString& name : std::as_const(names)) {
                const int c = add(name, dim);
                for (int k = 0; k < nf; ++k) out.at[k][c] = valueAt(k);
            }
        };
        for (int i = 0; i < np; ++i)
            for (int j = 0; j < np; ++j) {
                const auto s = [&](int k) { return w.s.at(k).at(i).at(j); };
                const auto z = [&](int k) { return k < w.z.size() ? w.z.at(k).at(i).at(j) : Complex(kNaN, kNaN); };
                both(QStringLiteral("S"), i, j, QStringLiteral("_dB"), fem::Dim(), [&](int k) { return 20 * std::log10(std::abs(s(k))); });
                both(QStringLiteral("S"), i, j, QStringLiteral("_mag"), fem::Dim(), [&](int k) { return std::abs(s(k)); });
                both(QStringLiteral("S"), i, j, QStringLiteral("_deg"), fem::Dim(), [&](int k) { return std::arg(s(k)) * 180 / M_PI; });
                both(QStringLiteral("S"), i, j, QStringLiteral("_re"), fem::Dim(), [&](int k) { return s(k).real(); });
                both(QStringLiteral("S"), i, j, QStringLiteral("_im"), fem::Dim(), [&](int k) { return s(k).imag(); });
                both(QStringLiteral("Z"), i, j, QStringLiteral("_re"), ohm, [&](int k) { return z(k).real(); });
                both(QStringLiteral("Z"), i, j, QStringLiteral("_im"), ohm, [&](int k) { return z(k).imag(); });
                both(QStringLiteral("Z"), i, j, QStringLiteral("_mag"), ohm, [&](int k) { return std::abs(z(k)); });
                // The group delay: the phase unwrapped along the sweep, its
                // slope against omega - central between neighbours, one-sided
                // at the ends.
                if (nf >= 2) {
                    QVector<double> phase(nf);
                    for (int k = 0; k < nf; ++k) {
                        phase[k] = std::arg(s(k));
                        if (k > 0)
                            while (phase[k] - phase[k - 1] > M_PI) phase[k] -= 2 * M_PI;
                        if (k > 0)
                            while (phase[k] - phase[k - 1] < -M_PI) phase[k] += 2 * M_PI;
                    }
                    both(QStringLiteral("GD"), i, j, QString(), second, [&](int k) {
                        const int a = std::max(0, k - 1), b = std::min(nf - 1, k + 1);
                        const double dw = 2 * M_PI * (w.frequencies.at(b) - w.frequencies.at(a));
                        return dw > 0 ? -(phase[b] - phase[a]) / dw : kNaN;
                    });
                }
            }
        for (int i = 0; i < np; ++i) {
            const int c = add(QStringLiteral("VSWR%1").arg(i + 1), fem::Dim());
            for (int k = 0; k < nf; ++k) {
                const double g = std::abs(w.s.at(k).at(i).at(i));
                out.at[k][c] = g < 1 ? (1 + g) / (1 - g) : std::numeric_limits<double>::infinity();
            }
        }
        return out;
    }
    if (solution.extraction && solution.extraction->ok()) {
        const Extraction& e = *solution.extraction;
        // At the study's frequencies: R and G move with them, C and L do not.
        QString why;
        QList<double> fs = studySweep(model, &why);
        if (fs.isEmpty()) fs << topFrequency(model);
        out.frequencies = fs;
        out.at = QList<QVector<double>>(fs.size());
        const int f = add(QStringLiteral("f"), hertz);
        for (int k = 0; k < fs.size(); ++k) out.at[k][f] = fs.at(k);
        const int n = int(e.c.size());
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                QStringList names{QStringLiteral("C%1_%2").arg(i + 1).arg(j + 1)};
                if (n <= 9) names.prepend(QStringLiteral("C%1%2").arg(i + 1).arg(j + 1));
                for (const QString& name : std::as_const(names)) {
                    const int c = add(name, farad);
                    for (int k = 0; k < fs.size(); ++k) out.at[k][c] = e.c.at(i).at(j);
                }
            }
        // A uniform line: each net as a line, at each frequency.
        if (e.perMetre.fromMiddle && solution.length > 0)
            for (int net = 0; net < n; ++net) {
                const QString tail = net == 0 ? QString() : QStringLiteral("_%1").arg(net + 1);
                const int z0 = add(QStringLiteral("Z0") + tail, ohm), eps = add(QStringLiteral("eps_eff") + tail, fem::Dim());
                const int r = add(QStringLiteral("R") + tail, dimOf("Ohm/m")), l = add(QStringLiteral("L") + tail, dimOf("H/m"));
                const int g = add(QStringLiteral("G") + tail, dimOf("S/m")), c = add(QStringLiteral("C") + tail, dimOf("F/m"));
                const int a = add(QStringLiteral("alpha_dB") + tail, dimOf("1/m"));
                for (int k = 0; k < fs.size(); ++k) {
                    const fem::LineRLGC line = asLine(e, net, solution.length, fs.at(k));
                    if (!line.error.isEmpty()) continue;
                    out.at[k][z0] = std::abs(line.Zc);
                    out.at[k][eps] = line.epsEff;
                    out.at[k][r] = line.R;
                    out.at[k][l] = line.L;
                    out.at[k][g] = line.G;
                    out.at[k][c] = line.C;
                    out.at[k][a] = line.alphaDb;
                }
            }
        if (solution.inductance && solution.inductance->ok()) {
            const int l = add(QStringLiteral("Lloop"), henry), r = add(QStringLiteral("Rloop"), ohm);
            for (int k = 0; k < fs.size(); ++k) {
                out.at[k][l] = solution.inductance->l;
                out.at[k][r] = solution.inductance->rDc;
            }
        }
    }
    return out;
}

bool overOf(const QString& text, Reading::Over fallback, Reading::Over* over)
{
    const QString t = text.trimmed().toLower();
    if (t.isEmpty()) *over = fallback;
    else if (t == QLatin1String("worst")) *over = Reading::Over::Worst;
    else if (t == QLatin1String("last")) *over = Reading::Over::Last;
    else if (t == QLatin1String("at")) *over = Reading::Over::At;
    else if (t == QLatin1String("min") || t == QLatin1String("least")) *over = Reading::Over::Least;
    else if (t == QLatin1String("max") || t == QLatin1String("most")) *over = Reading::Over::Most;
    else return false;
    return true;
}

bool frequencyOf(const QString& text, const fem::ParameterScope& parameters, double* hertz, QString* why)
{
    *hertz = kNaN;
    if (text.trimmed().isEmpty()) return true;
    QString bad, other;
    const std::optional<double> v = fem::evaluateConstant(text, parameters.scope, dimOf("Hz"), &bad, &other);
    if (bad.isEmpty()) bad = other;   // (a unit that is not a frequency's)
    if (!v || !other.isEmpty() || !std::isfinite(*v) || *v < 0) {
        *why = tr("\"%1\" is no frequency%2").arg(text, bad.isEmpty() ? QString() : QStringLiteral(": ") + bad);
        return false;
    }
    *hertz = *v;
    return true;
}

bool seriesOf(const SolutionValues& values, const fem::ParameterScope& parameters, const QString& expression, double from, double to,
              QList<double>* frequencies, QList<double>* series, fem::Dim* dim, QString* why)
{
    frequencies->clear();
    series->clear();
    if (values.frequencies.isEmpty()) {
        *why = tr("the solution has no values to read");
        return false;
    }
    for (int k = 0; k < values.frequencies.size(); ++k) {
        const double f = values.frequencies.at(k);
        // (The window's ends in it, to a part in a billion.)
        if (std::isfinite(from) && f < from * (1 - 1e-9)) continue;
        if (std::isfinite(to) && f > to * (1 + 1e-9)) continue;
        fem::Scope scope(&parameters.scope);
        for (int i = 0; i < values.names.size(); ++i) scope.setConstant(values.names.at(i), values.at.at(k).at(i), values.dims.at(i));
        const fem::Expression e = fem::Expression::compile(expression, scope);
        if (!e.isValid()) {
            QStringList names = values.names;
            names.sort(Qt::CaseInsensitive);
            *why = tr("%1: %2 - the solution's values are: %3").arg(expression, e.error(), names.mid(0, 80).join(QStringLiteral(", ")));
            return false;
        }
        if (!e.isConstant()) {
            *why = tr("%1 is not one number at a frequency").arg(expression);
            return false;
        }
        frequencies->append(f);
        series->append(e.constant());
        *dim = e.dim();
    }
    if (series->isEmpty()) {
        *why = tr("%1: no frequency of the solution's is in its window").arg(expression);
        return false;
    }
    return true;
}

bool readingValue(const SolutionValues& values, const fem::ParameterScope& parameters, const Reading& reading,
                  const optimization::Goal& goal, double* value, fem::Dim* dim, QString* why)
{
    QList<double> fs, ys;
    // (At a frequency: the whole sweep read, the frequency between two of it.)
    const bool at = reading.over == Reading::Over::At;
    if (!seriesOf(values, parameters, reading.expression, at ? kNaN : reading.from, at ? kNaN : reading.to, &fs, &ys, dim, why)) return false;
    switch (reading.over) {
    case Reading::Over::Last:
        *value = ys.last();
        return true;
    case Reading::Over::Least:
        *value = *std::min_element(ys.cbegin(), ys.cend());
        return true;
    case Reading::Over::Most:
        *value = *std::max_element(ys.cbegin(), ys.cend());
        return true;
    case Reading::Over::At: {
        const double f = reading.at;
        if (!std::isfinite(f)) {
            *why = tr("%1 is read at a frequency, and none is given").arg(reading.expression);
            return false;
        }
        for (int k = 0; k < fs.size(); ++k) {
            if (std::abs(fs.at(k) - f) <= 1e-9 * std::max(1.0, f)) {
                *value = ys.at(k);
                return true;
            }
            if (k > 0 && fs.at(k - 1) < f && f < fs.at(k)) {
                const double u = (f - fs.at(k - 1)) / (fs.at(k) - fs.at(k - 1));
                *value = ys.at(k - 1) + u * (ys.at(k) - ys.at(k - 1));
                return true;
            }
        }
        *why = tr("%1 is read at %2 Hz, outside the solution's frequencies (%3 to %4 Hz)")
                   .arg(reading.expression)
                   .arg(f, 0, 'g', 6)
                   .arg(fs.first(), 0, 'g', 6)
                   .arg(fs.last(), 0, 'g', 6);
        return false;
    }
    case Reading::Over::Worst:
        break;
    }
    *value = ys.size() == 1 ? ys.first() : optimization::measure(goal, QVector<double>(ys.cbegin(), ys.cend()));
    return true;
}

} // namespace qucs_s::rf25
