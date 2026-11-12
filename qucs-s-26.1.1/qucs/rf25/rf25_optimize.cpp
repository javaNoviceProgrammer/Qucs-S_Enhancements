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
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>

#include <thread>

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

Model withParameters(const Model& model, const QHash<QString, QString>& overrides)
{
    Model out = model;
    Node* node = out.root.child(QStringLiteral("rf_parameters"));
    if (node == nullptr || overrides.isEmpty()) return out;
    QJsonArray rows = node->props.value(QLatin1String("rows")).toArray();
    QSet<QString> done;
    for (int i = 0; i < rows.size(); ++i) {
        QJsonArray cells = rows.at(i).toArray();
        const QString name = cells.at(0).toString().trimmed();
        if (!overrides.contains(name)) continue;
        while (cells.size() < 2) cells.append(QString());
        cells[1] = overrides.value(name);
        rows[i] = cells;
        done.insert(name);
    }
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
        if (!done.contains(it.key())) rows.append(QJsonArray{it.key(), it.value()});
    node->props.insert(QStringLiteral("rows"), rows);
    return out;
}

QString siText(double value, const fem::Dim& dim)
{
    const QString number = QString::number(value, 'g', 17);
    return dim.isNone() ? number : number + QLatin1Char('[') + fem::dimName(dim) + QLatin1Char(']');
}

void RunControl::requestStop()
{
    stop = true;
    std::lock_guard<std::mutex> lock(a_mutex);
    for (SolveJob* job : std::as_const(a_active)) job->stop = true;
}

void RunControl::begin(SolveJob* job)
{
    std::lock_guard<std::mutex> lock(a_mutex);
    a_active.insert(job);
    if (stop.load()) job->stop = true;
}

void RunControl::end(SolveJob* job)
{
    std::lock_guard<std::mutex> lock(a_mutex);
    a_active.remove(job);
}

namespace {

QString cellText(const QJsonValue& v)
{
    if (v.isString()) return v.toString().trimmed();
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 17);
    return QString();
}

/// \a text as a value of \a dim in \a ps; none and why.
std::optional<double> valueOf(const QString& text, const fem::ParameterScope& ps, const fem::Dim& dim, const QString& what, QString* why)
{
    QString bad, other;
    const std::optional<double> v = fem::evaluateConstant(text, ps.scope, dim, &bad, &other);
    if (!v || !std::isfinite(*v)) {
        *why = tr("%1: \"%2\" is not a number%3").arg(what, text, bad.isEmpty() ? QString() : QStringLiteral(": ") + bad);
        return std::nullopt;
    }
    if (!other.isEmpty()) {
        *why = tr("%1: \"%2\" - %3").arg(what, text, other);
        return std::nullopt;
    }
    return v;
}

QJsonArray complexRows(const QList<QList<QList<Complex>>>& m)
{
    QJsonArray out;
    for (const auto& k : m) {
        QJsonArray rows;
        for (const auto& row : k) {
            QJsonArray cells;
            for (const Complex& c : row) cells << c.real() << c.imag();
            rows.append(cells);
        }
        out.append(rows);
    }
    return out;
}

QList<QList<QList<Complex>>> complexOf(const QJsonArray& a)
{
    QList<QList<QList<Complex>>> out;
    for (const QJsonValue& k : a) {
        QList<QList<Complex>> rows;
        for (const QJsonValue& row : k.toArray()) {
            const QJsonArray cells = row.toArray();
            QList<Complex> r;
            for (int i = 0; i + 1 < cells.size(); i += 2) r << Complex(cells.at(i).toDouble(), cells.at(i + 1).toDouble());
            rows << r;
        }
        out << rows;
    }
    return out;
}

QJsonArray numbers(const QList<double>& values)
{
    QJsonArray out;
    for (double v : values) out.append(std::isfinite(v) ? QJsonValue(v) : QJsonValue());
    return out;
}

QList<double> numbersOf(const QJsonArray& a)
{
    QList<double> out;
    for (const QJsonValue& v : a) out << (v.isDouble() ? v.toDouble() : kNaN);
    return out;
}

} // namespace

QList<QList<double>> SweepPlan::points() const
{
    QList<QList<double>> out;
    if (axes.isEmpty()) return out;
    if (axes.size() == 1) {
        for (double v : axes.first().values) out << QList<double>{v};
        return out;
    }
    if (paired) {
        const int n = int(std::min(axes.at(0).values.size(), axes.at(1).values.size()));
        for (int i = 0; i < n; ++i) out << QList<double>{axes.at(0).values.at(i), axes.at(1).values.at(i)};
        return out;
    }
    for (double b : axes.at(1).values)
        for (double a : axes.at(0).values) out << QList<double>{a, b};
    return out;
}

const Node* sweepNode(const Model& model, const QString& tag)
{
    const Node* analyses = model.root.child(QStringLiteral("rf_analyses"));
    if (analyses == nullptr) return nullptr;
    for (const Node& n : analyses->children)
        if (n.type == QLatin1String("rf_sweep") && (tag.isEmpty() || n.tag == tag)) return &n;
    return nullptr;
}

std::optional<SweepPlan> sweepPlan(const Model& model, const Node& node, QString* error)
{
    QString local;
    QString& why = error != nullptr ? *error : local;
    SweepPlan plan;
    plan.tag = node.tag;
    plan.name = nameOf(node);
    const auto refuse = [&why](const QString& text) {
        why = text;
        return std::nullopt;
    };
    const std::shared_ptr<fem::ParameterScope> ps = fem::evaluateParameters(model.parameters());
    if (!ps->errors.isEmpty()) {
        QStringList list;
        for (auto it = ps->errors.cbegin(); it != ps->errors.cend(); ++it) list << tr("%1: %2").arg(it.key(), it.value());
        return refuse(tr("%1: the parameters: %2").arg(plan.name, list.join(QStringLiteral("; "))));
    }
    // The parameters it sweeps.
    for (const QJsonValue& row : valueOf(node, QStringLiteral("parameters")).toArray()) {
        const QJsonArray r = row.toArray();
        SweepAxis axis;
        axis.parameter = cellText(r.at(0));
        if (axis.parameter.isEmpty()) continue;
        if (!ps->values.contains(axis.parameter)) {
            QStringList known;
            for (const fem::Model::Parameter& p : model.parameters()) known << p.name;
            return refuse(tr("%1: there is no parameter %2 - the model has: %3").arg(plan.name, axis.parameter, known.isEmpty() ? tr("none") : known.join(QStringLiteral(", "))));
        }
        for (const SweepAxis& a : std::as_const(plan.axes))
            if (a.parameter == axis.parameter) return refuse(tr("%1: %2 is swept twice").arg(plan.name, axis.parameter));
        axis.dim = ps->dims.value(axis.parameter);
        const QString what = tr("%1: %2").arg(plan.name, axis.parameter);
        const QString list = cellText(r.at(5));
        if (!list.isEmpty()) {
            for (const QString& item : list.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const std::optional<double> v = valueOf(item.trimmed(), *ps, axis.dim, what, &why);
                if (!v) return std::nullopt;
                axis.values << *v;
            }
        } else {
            const QString fromText = cellText(r.at(1)), toText = cellText(r.at(2));
            if (fromText.isEmpty() || toText.isEmpty())
                return refuse(tr("%1: give it a From and a To, or a List of values (10[um], 20[um])").arg(what));
            const std::optional<double> from = valueOf(fromText, *ps, axis.dim, tr("%1's From").arg(what), &why);
            if (!from) return std::nullopt;
            const std::optional<double> to = valueOf(toText, *ps, axis.dim, tr("%1's To").arg(what), &why);
            if (!to) return std::nullopt;
            bool ok = true;
            const QString pointsText = cellText(r.at(3));
            const int points = pointsText.isEmpty() ? 5 : pointsText.toInt(&ok);
            if (!ok || points < 1 || points > 1000) return refuse(tr("%1: its Points are a whole number from 1 to 1000").arg(what));
            const bool log = cellText(r.at(4)).toLower().startsWith(QLatin1String("log"));
            if (log && !(*from > 0 && *to > 0)) return refuse(tr("%1: a logarithmic sweep wants From and To above nought").arg(what));
            for (int i = 0; i < points; ++i) {
                const double u = points == 1 ? 0 : double(i) / (points - 1);
                axis.values << (log ? *from * std::pow(*to / *from, u) : *from + u * (*to - *from));
            }
        }
        if (axis.values.isEmpty()) return refuse(tr("%1: no value to solve at").arg(what));
        plan.axes << axis;
    }
    if (plan.axes.isEmpty()) return refuse(tr("%1 sweeps no parameter: name one, with its values").arg(plan.name));
    if (plan.axes.size() > 2) return refuse(tr("%1 sweeps %2 parameters: one or two").arg(plan.name).arg(plan.axes.size()));
    plan.paired = textOf(node, QStringLiteral("combine")) == QLatin1String("paired");
    if (plan.paired && plan.axes.size() == 2 && plan.axes.at(0).values.size() != plan.axes.at(1).values.size())
        return refuse(tr("%1: paired, its two parameters want as many values each (%2 and %3)")
                          .arg(plan.name).arg(plan.axes.at(0).values.size()).arg(plan.axes.at(1).values.size()));
    if (plan.points().size() > 10000) return refuse(tr("%1 would solve %2 points: 10000 at most").arg(plan.name).arg(plan.points().size()));
    // What it reads at each point.
    for (const QJsonValue& row : valueOf(node, QStringLiteral("quantities")).toArray()) {
        const QJsonArray r = row.toArray();
        Reading reading;
        reading.expression = cellText(r.at(0));
        if (reading.expression.isEmpty()) continue;
        if (!overOf(cellText(r.at(1)), Reading::Over::Last, &reading.over) || reading.over == Reading::Over::Worst)
            return refuse(tr("%1: %2's Over is \"%3\" - last, min, max or at").arg(plan.name, reading.expression, cellText(r.at(1))));
        QString bad;
        if (!frequencyOf(cellText(r.at(2)), *ps, &reading.from, &bad) || !frequencyOf(cellText(r.at(3)), *ps, &reading.to, &bad)
            || !frequencyOf(cellText(r.at(4)), *ps, &reading.at, &bad))
            return refuse(tr("%1: %2: %3").arg(plan.name, reading.expression, bad));
        if (reading.over == Reading::Over::At && !std::isfinite(reading.at))
            return refuse(tr("%1: %2 is read at a frequency: give it in At").arg(plan.name, reading.expression));
        plan.quantities << reading;
    }
    if (plan.quantities.isEmpty()) return refuse(tr("%1 reads nothing at its points: give a quantity (S21_dB, Z0, C11)").arg(plan.name));
    bool ok = true;
    const QString parallel = cellText(valueOf(node, QStringLiteral("parallel")));
    plan.parallel = parallel.isEmpty() ? 1 : parallel.toInt(&ok);
    if (!ok || plan.parallel < 1 || plan.parallel > 64) return refuse(tr("%1: its Solves at once are a whole number from 1 to 64").arg(plan.name));
    return plan;
}

QString analysisSetup(const Model& model, const Node& node)
{
    // The parameters it varies start from whatever they are: not what it is of.
    QSet<QString> varied;
    for (const char* key : {"parameters", "variables"})
        for (const QJsonValue& row : valueOf(node, QLatin1String(key)).toArray()) varied.insert(cellText(row.toArray().at(0)));
    Model m = model;
    if (Node* p = m.root.child(QStringLiteral("rf_parameters"))) {
        QJsonArray kept;
        for (const QJsonValue& row : p->props.value(QLatin1String("rows")).toArray())
            if (!varied.contains(cellText(row.toArray().at(0)))) kept.append(row);
        p->props.insert(QStringLiteral("rows"), kept);
    }
    // (Its own result, and the other analyses', are not what it is of.)
    if (Node* a = m.root.child(QStringLiteral("rf_analyses"))) a->children.clear();
    QJsonObject mine = node.props;
    mine.remove(QStringLiteral("result"));
    QJsonObject whole = m.toObject();
    whole.insert(QStringLiteral("analysis"), mine);
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(whole).toJson(QJsonDocument::Compact), QCryptographicHash::Md5).toHex());
}

QJsonObject SweepResult::toJson() const
{
    QJsonObject o;
    if (!error.isEmpty()) o.insert(QStringLiteral("error"), error);
    o.insert(QStringLiteral("setup"), setup);
    o.insert(QStringLiteral("columns"), QJsonArray::fromStringList(columns));
    o.insert(QStringLiteral("units"), QJsonArray::fromStringList(units));
    o.insert(QStringLiteral("parameters"), parameters);
    o.insert(QStringLiteral("solved"), solved);
    o.insert(QStringLiteral("failed"), failed);
    o.insert(QStringLiteral("stopped"), stopped);
    o.insert(QStringLiteral("seconds"), seconds);
    QJsonArray rows;
    for (const SweepPoint& p : points) {
        QJsonObject r;
        r.insert(QStringLiteral("values"), numbers(p.values));
        r.insert(QStringLiteral("measured"), numbers(p.measured));
        r.insert(QStringLiteral("solved"), p.solved);
        if (!p.why.isEmpty()) r.insert(QStringLiteral("why"), p.why);
        if (!p.frequencies.isEmpty()) {
            r.insert(QStringLiteral("frequencies"), numbers(p.frequencies));
            r.insert(QStringLiteral("ports"), QJsonArray::fromStringList(p.portNames));
            r.insert(QStringLiteral("references"), numbers(p.references));
            r.insert(QStringLiteral("s"), complexRows(p.s));
            r.insert(QStringLiteral("z"), complexRows(p.z));
        }
        rows.append(r);
    }
    o.insert(QStringLiteral("points"), rows);
    return o;
}

SweepResult SweepResult::fromJson(const QJsonObject& o)
{
    SweepResult r;
    r.error = o.value(QStringLiteral("error")).toString();
    r.setup = o.value(QStringLiteral("setup")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("columns")).toArray()) r.columns << v.toString();
    for (const QJsonValue& v : o.value(QStringLiteral("units")).toArray()) r.units << v.toString();
    r.parameters = o.value(QStringLiteral("parameters")).toInt();
    r.solved = o.value(QStringLiteral("solved")).toInt();
    r.failed = o.value(QStringLiteral("failed")).toInt();
    r.stopped = o.value(QStringLiteral("stopped")).toBool();
    r.seconds = o.value(QStringLiteral("seconds")).toDouble();
    for (const QJsonValue& v : o.value(QStringLiteral("points")).toArray()) {
        const QJsonObject p = v.toObject();
        SweepPoint point;
        point.values = numbersOf(p.value(QStringLiteral("values")).toArray());
        point.measured = numbersOf(p.value(QStringLiteral("measured")).toArray());
        point.solved = p.value(QStringLiteral("solved")).toBool();
        point.why = p.value(QStringLiteral("why")).toString();
        point.frequencies = numbersOf(p.value(QStringLiteral("frequencies")).toArray());
        for (const QJsonValue& n : p.value(QStringLiteral("ports")).toArray()) point.portNames << n.toString();
        point.references = numbersOf(p.value(QStringLiteral("references")).toArray());
        point.s = complexOf(p.value(QStringLiteral("s")).toArray());
        point.z = complexOf(p.value(QStringLiteral("z")).toArray());
        r.points << point;
    }
    return r;
}

std::shared_ptr<FullWave> SweepResult::waveOf(int index) const
{
    if (index < 0 || index >= points.size()) return nullptr;
    const SweepPoint& p = points.at(index);
    if (p.frequencies.isEmpty() || p.s.isEmpty()) return nullptr;
    auto w = std::make_shared<FullWave>();
    w->frequencies = p.frequencies;
    w->portNames = p.portNames;
    w->references = p.references;
    w->s = p.s;
    w->z = p.z;
    return w;
}

SweepResult runSweep(const Model& model, std::shared_ptr<const qucs_s::layout::Layout> layout, const QString& called, const QString& folder,
                     const SweepPlan& plan, RunControl& control)
{
    SweepResult result;
    QElapsedTimer clock;
    clock.start();
    if (!layout) {
        result.error = tr("%1: the layout has not been read").arg(plan.name);
        return result;
    }
    for (const SweepAxis& a : plan.axes) {
        result.columns << a.parameter;
        result.units << (a.dim.isNone() ? QString() : fem::dimName(a.dim));
    }
    result.parameters = int(plan.axes.size());
    for (const Reading& q : plan.quantities) {
        result.columns << q.expression;
        result.units << QString();   // (known once the first point is read)
    }
    const QList<QList<double>> points = plan.points();
    result.points = QList<SweepPoint>(points.size());
    control.of = int(points.size());
    control.done = 0;
    const std::shared_ptr<fem::ParameterScope> base = fem::evaluateParameters(model.parameters());
    std::mutex mutex;
    QString fatal;
    const auto solveOne = [&](int i) {
        SweepPoint& point = result.points[i];
        point.values = points.at(i);
        point.measured = QList<double>(plan.quantities.size(), kNaN);
        QHash<QString, QString> overrides;
        for (int a = 0; a < plan.axes.size(); ++a) overrides.insert(plan.axes.at(a).parameter, siText(points.at(i).at(a), plan.axes.at(a).dim));
        const Model candidate = withParameters(model, overrides);
        SolveJob job;
        control.begin(&job);
        const ModelSolution solution = solveModel(job, candidate, *layout, called, folder);
        control.end(&job);
        if (solution.stopped || control.stop.load()) {
            point.why = tr("stopped");
            return;
        }
        if (!solution.ok()) {
            point.why = solution.problem;
            return;
        }
        const SolutionValues values = solutionValues(solution, candidate);
        const std::shared_ptr<fem::ParameterScope> ps = fem::evaluateParameters(candidate.parameters());
        for (int q = 0; q < plan.quantities.size(); ++q) {
            double v = kNaN;
            fem::Dim dim;
            QString why;
            if (!readingValue(values, *ps, plan.quantities.at(q), optimization::Goal(), &v, &dim, &why)) {
                std::lock_guard<std::mutex> lock(mutex);
                if (fatal.isEmpty()) fatal = tr("%1: %2").arg(plan.name, why);
                return;
            }
            point.measured[q] = v;
            std::lock_guard<std::mutex> lock(mutex);
            if (result.units.at(plan.axes.size() + q).isEmpty() && !dim.isNone()) result.units[plan.axes.size() + q] = fem::dimName(dim);
        }
        point.solved = true;
        if (solution.wave) {
            point.frequencies = solution.wave->frequencies;
            point.portNames = solution.wave->portNames;
            point.references = solution.wave->references;
            point.s = solution.wave->s;
            point.z = solution.wave->z;
        }
    };
    const int parallel = std::max(1, plan.parallel);
    for (int from = 0; from < points.size(); from += parallel) {
        if (control.stop.load() || !fatal.isEmpty()) break;
        const int to = std::min(int(points.size()), from + parallel);
        if (to - from == 1) {
            solveOne(from);
        } else {
            std::vector<std::thread> threads;
            for (int i = from; i < to; ++i) threads.emplace_back([&solveOne, i] { solveOne(i); });
            for (std::thread& t : threads) t.join();
        }
        control.done = to;
    }
    // What was done: the points not reached left out.
    QList<SweepPoint> reached;
    for (int i = 0; i < result.points.size(); ++i) {
        const SweepPoint& p = result.points.at(i);
        if (p.values.isEmpty()) continue;
        reached << p;
        if (p.solved) ++result.solved;
        else if (p.why != tr("stopped")) ++result.failed;
    }
    result.points = reached;
    result.stopped = control.stop.load();
    if (!fatal.isEmpty()) result.error = fatal;
    result.seconds = double(clock.elapsed()) / 1000;
    return result;
}

} // namespace qucs_s::rf25
