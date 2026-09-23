/*
 * optimization.cpp - what an optimization component asks for, and the
 * search that answers it with a SPICE simulator (see optimization.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "optimization.h"

#include <QCoreApplication>
#include <QFile>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

#include "components/component.h"
#include "schematic.h"
#include "valuereading.h"

namespace qucs_s::optimization {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Optimization", text);
}

// A number of the component: "3.9E-07", "100e-9", "56p", "1 k".
bool readNumber(const QString& text, double* value)
{
    const units::Reading r = units::read(text);
    if (r.kind != units::Reading::Number) return false;
    *value = r.value;
    return std::isfinite(*value);
}

bool isSeries(const QString& type)
{
    return !eSeries(type).isEmpty();
}

bool isInteger(const QString& type)
{
    return type == QLatin1String("LIN_INT") || type == QLatin1String("LOG_INT");
}

} // namespace

// ---------------------------------------------------------------------------
// Variable

bool Variable::parse(const QString& value, Variable* variable, QString* error)
{
    const QStringList f = value.split(QLatin1Char('|'));
    auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (f.size() < 6) return fail(tr("the variable \"%1\" is incomplete").arg(value));
    Variable v;
    v.name = f.at(0).trimmed();
    if (v.name.isEmpty()) return fail(tr("a variable has no name"));
    v.active = f.at(1).trimmed() != QLatin1String("no");
    if (!readNumber(f.at(2), &v.initial))
        return fail(tr("%1: the initial value \"%2\" is not a number").arg(v.name, f.at(2)));
    if (!readNumber(f.at(3), &v.min))
        return fail(tr("%1: the minimum \"%2\" is not a number").arg(v.name, f.at(3)));
    if (!readNumber(f.at(4), &v.max))
        return fail(tr("%1: the maximum \"%2\" is not a number").arg(v.name, f.at(4)));
    if (v.min > v.max) return fail(tr("%1: the minimum is above the maximum").arg(v.name));
    v.type = f.at(5).trimmed();
    *variable = v;
    return true;
}

QString Variable::toString(int precision) const
{
    return QStringList({name, active ? QStringLiteral("yes") : QStringLiteral("no"),
                        QString::number(initial, 'g', precision), QString::number(min, 'g', precision),
                        QString::number(max, 'g', precision), type})
        .join(QLatin1Char('|'));
}

bool Variable::logarithmic() const
{
    const bool logType = type.startsWith(QLatin1String("LOG_")) || isSeries(type);
    return logType && min > 0 && max > 0;
}

double Variable::lower() const
{
    return logarithmic() ? std::log10(min) : min;
}

double Variable::upper() const
{
    return logarithmic() ? std::log10(max) : max;
}

double Variable::coordinate(double v) const
{
    v = std::clamp(v, min, max);
    return logarithmic() ? std::log10(v) : v;
}

double Variable::value(double c) const
{
    double v = logarithmic() ? std::pow(10.0, c) : c;
    v = std::clamp(v, min, max);
    if (isInteger(type)) {
        v = std::round(v);
        if (v < min) v = std::ceil(min);
        if (v > max) v = std::floor(max);
    } else if (const QList<double> series = eSeries(type); !series.isEmpty() && v > 0) {
        // The preferred value nearest to it within the bounds; the nearest
        // at all if there is none between them.
        double best = std::numeric_limits<double>::quiet_NaN();
        double distance = std::numeric_limits<double>::infinity();
        const int from = int(std::floor(std::log10(min > 0 ? min : v))) - 1;
        const int to = int(std::floor(std::log10(max > 0 ? max : v))) + 1;
        for (int e = from; e <= to; ++e) {
            for (double s : series) {
                const double candidate = s * std::pow(10.0, e);
                if (candidate < min * (1 - 1e-9) || candidate > max * (1 + 1e-9)) continue;
                const double d = std::abs(std::log10(candidate / v));
                if (d < distance) {
                    distance = d;
                    best = candidate;
                }
            }
        }
        v = std::isnan(best) ? nearestInSeries(series, v) : best;
    }
    return v;
}

QList<double> eSeries(const QString& name)
{
    static const QList<double> e3 = {1.0, 2.2, 4.7};
    static const QList<double> e6 = {1.0, 1.5, 2.2, 3.3, 4.7, 6.8};
    static const QList<double> e12 = {1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2};
    static const QList<double> e24 = {1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                                      3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1};
    if (name == QLatin1String("E3")) return e3;
    if (name == QLatin1String("E6")) return e6;
    if (name == QLatin1String("E12")) return e12;
    if (name == QLatin1String("E24")) return e24;
    int n = 0;
    if (name == QLatin1String("E48")) n = 48;
    else if (name == QLatin1String("E96")) n = 96;
    else if (name == QLatin1String("E192")) n = 192;
    else return {};
    // Three significant digits of the geometric series - but for the
    // standard's one exception, 9.19 of E192 where the formula gives 9.20.
    QList<double> values;
    for (int i = 0; i < n; ++i) {
        double v = std::round(std::pow(10.0, double(i) / n) * 100.0) / 100.0;
        if (n == 192 && i == 185) v = 9.19;
        values << v;
    }
    return values;
}

double nearestInSeries(const QList<double>& series, double value)
{
    if (series.isEmpty() || !(value > 0)) return value;
    const double decade = std::pow(10.0, std::floor(std::log10(value)));
    const double m = value / decade;
    double best = series.first() * decade;
    double distance = std::numeric_limits<double>::infinity();
    // The decade's values and the next decade's first one.
    for (int i = 0; i <= series.size(); ++i) {
        const double s = i < series.size() ? series.at(i) : series.first() * 10;
        const double d = std::abs(std::log10(s / m));
        if (d < distance) {
            distance = d;
            best = s * decade;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Goal

bool Goal::parse(const QString& value, Goal* goal, QString* error)
{
    const QStringList f = value.split(QLatin1Char('|'));
    auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (f.size() < 2) return fail(tr("the goal \"%1\" is incomplete").arg(value));
    Goal g;
    g.name = f.at(0).trimmed();
    if (g.name.isEmpty()) return fail(tr("a goal has no name"));
    const QString type = f.at(1).trimmed();
    if (type == QLatin1String("MIN")) g.type = GoalType::Minimize;
    else if (type == QLatin1String("MAX")) g.type = GoalType::Maximize;
    else if (type == QLatin1String("LE")) g.type = GoalType::LessEqual;
    else if (type == QLatin1String("GE")) g.type = GoalType::GreaterEqual;
    else if (type == QLatin1String("EQ")) g.type = GoalType::Equal;
    else g.type = GoalType::Monitor;
    if (g.isConstraint()) {
        if (f.size() < 3 || !readNumber(f.at(2), &g.target))
            return fail(tr("%1: the value \"%2\" is not a number").arg(g.name, f.value(2)));
    }
    *goal = g;
    return true;
}

QString Goal::typeName(GoalType type)
{
    switch (type) {
    case GoalType::Minimize: return QStringLiteral("MIN");
    case GoalType::Maximize: return QStringLiteral("MAX");
    case GoalType::LessEqual: return QStringLiteral("LE");
    case GoalType::GreaterEqual: return QStringLiteral("GE");
    case GoalType::Equal: return QStringLiteral("EQ");
    case GoalType::Monitor: break;
    }
    return QStringLiteral("MON");
}

bool Goal::isConstraint() const
{
    return type == GoalType::LessEqual || type == GoalType::GreaterEqual || type == GoalType::Equal;
}

bool Goal::isObjective() const
{
    return type == GoalType::Minimize || type == GoalType::Maximize;
}

bool Goal::met(double y) const
{
    if (!std::isfinite(y)) return false;
    switch (type) {
    case GoalType::LessEqual: return y <= target;
    case GoalType::GreaterEqual: return y >= target;
    case GoalType::Equal: return std::abs(y - target) <= 1e-9 * std::max(1.0, std::abs(target));
    default: return true;
    }
}

QString Goal::describe(double y) const
{
    const QString v = std::isfinite(y) ? number(y) : tr("not measured");
    switch (type) {
    case GoalType::Minimize: return tr("%1 = %2 (minimize)").arg(name, v);
    case GoalType::Maximize: return tr("%1 = %2 (maximize)").arg(name, v);
    case GoalType::Monitor: return tr("%1 = %2").arg(name, v);
    default: break;
    }
    const QString op = type == GoalType::LessEqual ? QStringLiteral("<=")
                       : type == GoalType::GreaterEqual ? QStringLiteral(">=") : QStringLiteral("=");
    return tr("%1 = %2 (%3 %4, %5)").arg(name, v, op, number(target), met(y) ? tr("met") : tr("not met"));
}

// ---------------------------------------------------------------------------
// Settings, Problem

Settings Settings::parse(const QString& value)
{
    Settings s;
    const QStringList f = value.split(QLatin1Char('|'));
    auto integer = [&](int i, int* out, int least) {
        bool ok = false;
        const int v = f.value(i).trimmed().toInt(&ok);
        if (ok && v >= least) *out = v;
    };
    auto real = [&](int i, double* out) {
        double v = 0;
        if (readNumber(f.value(i), &v)) *out = v;
    };
    integer(0, &s.method, 1);
    if (s.method > 10) s.method = 3;
    integer(1, &s.generations, 1);
    integer(2, &s.refresh, 1);
    integer(3, &s.population, 1);
    real(4, &s.F);
    real(5, &s.CR);
    integer(6, &s.seed, 0);
    real(7, &s.minCostVariance);
    real(8, &s.objectiveWeight);
    real(9, &s.constraintWeight);
    s.CR = std::clamp(s.CR, 0.0, 1.0);
    return s;
}

QString Settings::methodName() const
{
    static const char* const names[] = {"DE/best/1/exp", "DE/rand/1/exp", "DE/rand-to-best/1/exp",
                                        "DE/best/2/exp", "DE/rand/2/exp", "DE/best/1/bin",
                                        "DE/rand/1/bin", "DE/rand-to-best/1/bin", "DE/best/2/bin",
                                        "DE/rand/2/bin"};
    return QString::fromLatin1(names[std::clamp(method, 1, 10) - 1]);
}

bool Problem::read(const Component* optimization, Problem* problem, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (optimization == nullptr || optimization->Props.size() < 2)
        return fail(tr("no optimization component"));
    Problem p;
    p.component = optimization->Name;
    p.simulation = optimization->Props.at(0)->Value.trimmed();
    p.settings = Settings::parse(optimization->Props.at(1)->Value);
    for (int i = 2; i < optimization->Props.size(); ++i) {
        const Property* prop = optimization->Props.at(i);
        QString why;
        if (prop->Name == QLatin1String("Var")) {
            Variable v;
            if (!Variable::parse(prop->Value, &v, &why)) return fail(why);
            p.variables << v;
        } else if (prop->Name == QLatin1String("Goal")) {
            Goal g;
            if (!Goal::parse(prop->Value, &g, &why)) return fail(why);
            p.goals << g;
        }
    }
    if (p.variables.isEmpty()) return fail(tr("%1 has no variables").arg(p.component));
    if (std::none_of(p.variables.begin(), p.variables.end(), [](const Variable& v) { return v.active; }))
        return fail(tr("%1: none of the variables is to be optimized").arg(p.component));
    if (std::none_of(p.goals.begin(), p.goals.end(), [](const Goal& g) { return g.type != GoalType::Monitor; }))
        return fail(tr("%1 has no goal to reach (only goals to monitor, or none)").arg(p.component));
    *problem = p;
    return true;
}

QVector<double> Problem::lower() const
{
    QVector<double> x;
    for (const Variable& v : variables)
        if (v.active) x << v.lower();
    return x;
}

QVector<double> Problem::upper() const
{
    QVector<double> x;
    for (const Variable& v : variables)
        if (v.active) x << v.upper();
    return x;
}

QVector<double> Problem::initialCoordinates() const
{
    QVector<double> x;
    for (const Variable& v : variables)
        if (v.active) x << v.coordinate(v.initial);
    return x;
}

QMap<QString, double> Problem::values(const QVector<double>& coordinates) const
{
    QMap<QString, double> m;
    int k = 0;
    for (const Variable& v : variables) {
        if (v.active && k < coordinates.size()) m.insert(v.name, v.value(coordinates.at(k++)));
        else m.insert(v.name, v.initial);
    }
    return m;
}

// ---------------------------------------------------------------------------
// Dataset

namespace {

bool readValue(const QString& text, double* re, double* im)
{
    *im = 0;
    const int j = text.indexOf(QLatin1Char('j'));
    bool ok = false;
    if (j < 0) {
        *re = text.toDouble(&ok);
        return ok;
    }
    // "<re>+j<im>" or "<re>-j<im>" (or "+j<im>" alone)
    if (j == 0) return false;
    const QChar sign = text.at(j - 1);
    if (sign != QLatin1Char('+') && sign != QLatin1Char('-')) return false;
    const QString rePart = text.left(j - 1);
    *re = rePart.isEmpty() ? 0 : rePart.toDouble(&ok);
    if (!rePart.isEmpty() && !ok) return false;
    const double i = text.mid(j + 1).toDouble(&ok);
    if (!ok) return false;
    *im = sign == QLatin1Char('-') ? -i : i;
    return true;
}

} // namespace

QHash<QString, QVector<double>> readDataset(const QString& file, QString* error)
{
    QHash<QString, QVector<double>> table;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error != nullptr) *error = tr("cannot read %1").arg(file);
        return table;
    }
    QString name;
    QVector<double> re, im;
    bool complex = false;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1String("<indep ")) || line.startsWith(QLatin1String("<dep "))) {
            const QStringList head = line.mid(1, line.size() - 2).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            name = head.value(1);
            re.clear();
            im.clear();
            complex = false;
        } else if (line.startsWith(QLatin1String("</"))) {
            if (!name.isEmpty()) {
                QVector<double> values = re;
                if (complex)
                    for (int i = 0; i < values.size(); ++i) values[i] = std::hypot(re.at(i), im.at(i));
                table.insert(name, values);
            }
            name.clear();
        } else if (!name.isEmpty() && !line.startsWith(QLatin1Char('<'))) {
            double r = 0, i = 0;
            if (!readValue(line, &r, &i)) {
                r = std::numeric_limits<double>::quiet_NaN();
                i = 0;
            }
            re << r;
            im << i;
            if (i != 0) complex = true;
        }
    }
    if (table.isEmpty() && error != nullptr) *error = tr("%1 holds no results").arg(file);
    return table;
}

QString resolve(const QStringList& names, const QString& goal, const QString& analysis)
{
    for (const QString& n : names)
        if (n.compare(goal, Qt::CaseInsensitive) == 0) return n;
    // The name without the analysis the simulator put before it: "ac.gain"
    // is "gain", "ac.v(vo)" is "v(vo)".
    auto bare = [](const QString& n, QString* prefix) {
        const int dot = n.indexOf(QLatin1Char('.'));
        const int paren = n.indexOf(QLatin1Char('('));
        if (dot <= 0 || (paren >= 0 && paren < dot)) {
            prefix->clear();
            return n;
        }
        *prefix = n.left(dot);
        return n.mid(dot + 1);
    };
    // The goal itself; else its voltage or current - ngspice keeps an
    // expression of a voltage one, and writes vo = mag(v(out)) as v(vo).
    const QStringList forms = {goal, QStringLiteral("v(%1)").arg(goal), QStringLiteral("i(%1)").arg(goal)};
    for (const QString& form : forms) {
        QStringList candidates;
        QStringList prefixes;
        for (const QString& n : names) {
            QString prefix;
            if (bare(n, &prefix).compare(form, Qt::CaseInsensitive) != 0) continue;
            candidates << n;
            prefixes << prefix;
        }
        if (candidates.size() == 1) return candidates.first();
        if (!analysis.isEmpty())
            for (int i = 0; i < candidates.size(); ++i)
                if (prefixes.at(i).compare(analysis, Qt::CaseInsensitive) == 0) return candidates.at(i);
        if (!candidates.isEmpty()) return QString();   // several, none of them the analysis'
    }
    return QString();
}

QString analysisPrefix(const QString& model)
{
    if (model == QLatin1String(".AC")) return QStringLiteral("ac");
    if (model == QLatin1String(".TR")) return QStringLiteral("tran");
    if (model == QLatin1String(".DC") || model == QLatin1String(".SW")) return QStringLiteral("dc");
    return QString();
}

double measure(const Goal& goal, const QVector<double>& values)
{
    QVector<double> v;
    for (double x : values)
        if (std::isfinite(x)) v << x;
    if (v.isEmpty() || v.size() != values.size()) return std::numeric_limits<double>::quiet_NaN();
    switch (goal.type) {
    case GoalType::Maximize:
    case GoalType::GreaterEqual:
        return *std::min_element(v.begin(), v.end());
    case GoalType::Equal:
        return *std::max_element(v.begin(), v.end(), [&](double a, double b) {
            return std::abs(a - goal.target) < std::abs(b - goal.target);
        });
    default:
        return *std::max_element(v.begin(), v.end());
    }
}

double cost(const QList<Goal>& goals, const QVector<double>& measured, const QVector<double>& scale,
            const Settings& settings)
{
    double c = 0;
    for (int i = 0; i < goals.size(); ++i) {
        const Goal& g = goals.at(i);
        if (g.type == GoalType::Monitor) continue;
        const double y = measured.value(i, std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(y)) return std::numeric_limits<double>::infinity();
        const double s = scale.value(i, 1.0);
        const double t = std::abs(g.target) > 0 ? std::abs(g.target) : 1.0;
        switch (g.type) {
        case GoalType::Minimize: c += settings.objectiveWeight * y / s; break;
        case GoalType::Maximize: c -= settings.objectiveWeight * y / s; break;
        case GoalType::LessEqual: c += settings.constraintWeight * std::max(0.0, y - g.target) / t; break;
        case GoalType::GreaterEqual: c += settings.constraintWeight * std::max(0.0, g.target - y) / t; break;
        case GoalType::Equal: c += settings.constraintWeight * std::abs(y - g.target) / t; break;
        case GoalType::Monitor: break;
        }
    }
    return c;
}

QVector<double> scaleOf(const QVector<double>& measured)
{
    QVector<double> s;
    for (double y : measured) s << (std::isfinite(y) && y != 0 ? std::abs(y) : 1.0);
    return s;
}

// ---------------------------------------------------------------------------
// DifferentialEvolution

DifferentialEvolution::DifferentialEvolution(const QVector<double>& lower, const QVector<double>& upper,
                                             const Settings& settings)
    : a_lower(lower), a_upper(upper), a_settings(settings),
      // Five others besides the member itself: what DE/x/2 draws.
      a_np(std::max(settings.population, 6)), a_dim(int(lower.size())),
      a_rng(static_cast<std::mt19937::result_type>(settings.seed))
{
}

double DifferentialEvolution::uniform(double lo, double hi)
{
    if (!(hi > lo)) return lo;
    return std::uniform_real_distribution<double>(lo, hi)(a_rng);
}

int DifferentialEvolution::pick(QList<int>* taken)
{
    std::uniform_int_distribution<int> d(0, a_np - 1);
    int r;
    do {
        r = d(a_rng);
    } while (taken->contains(r));
    taken->append(r);
    return r;
}

void DifferentialEvolution::start(const QVector<double>& x0, double cost0)
{
    a_population.clear();
    a_cost.clear();
    a_pending.clear();
    QVector<double> first(a_dim);
    for (int d = 0; d < a_dim; ++d) first[d] = std::clamp(x0.value(d), a_lower.at(d), a_upper.at(d));
    a_population << first;
    a_cost << cost0;
    for (int i = 1; i < a_np; ++i) {
        QVector<double> x(a_dim);
        for (int d = 0; d < a_dim; ++d) x[d] = uniform(a_lower.at(d), a_upper.at(d));
        a_population << x;
        a_cost << std::numeric_limits<double>::infinity();
    }
    a_generation = -1;
    a_best = 0;
}

QList<DifferentialEvolution::Candidate> DifferentialEvolution::next()
{
    a_pending.clear();
    if (a_population.isEmpty() || finished()) return a_pending;
    if (a_generation < 0) {
        for (int i = 1; i < a_np; ++i) a_pending << Candidate{i, a_population.at(i)};
    } else {
        for (int i = 0; i < a_np; ++i) a_pending << Candidate{i, trial(i)};
    }
    return a_pending;
}

QVector<double> DifferentialEvolution::trial(int i)
{
    const QVector<double>& x = a_population.at(i);
    const QVector<double>& best = a_population.at(a_best);
    QList<int> taken{i};
    const int r1 = pick(&taken), r2 = pick(&taken), r3 = pick(&taken), r4 = pick(&taken), r5 = pick(&taken);
    const auto& p1 = a_population.at(r1);
    const auto& p2 = a_population.at(r2);
    const auto& p3 = a_population.at(r3);
    const auto& p4 = a_population.at(r4);
    const auto& p5 = a_population.at(r5);
    const double F = a_settings.F;
    const int base = (a_settings.method - 1) % 5;   // the mutation; 1..5 exponential, 6..10 binomial
    auto mutant = [&](int d) {
        switch (base) {
        case 0: return best[d] + F * (p1[d] - p2[d]);                          // best/1
        case 1: return p1[d] + F * (p2[d] - p3[d]);                            // rand/1
        case 2: return x[d] + F * (best[d] - x[d]) + F * (p1[d] - p2[d]);      // rand-to-best/1
        case 3: return best[d] + F * (p1[d] + p2[d] - p3[d] - p4[d]);          // best/2
        default: return p5[d] + F * (p1[d] + p2[d] - p3[d] - p4[d]);           // rand/2
        }
    };
    QVector<double> t = x;
    std::uniform_int_distribution<int> start(0, std::max(a_dim - 1, 0));
    int n = start(a_rng);
    if (a_settings.method <= 5) {   // exponential crossover: a run of coordinates
        int L = 0;
        do {
            t[n] = mutant(n);
            n = (n + 1) % a_dim;
            ++L;
        } while (uniform(0, 1) < a_settings.CR && L < a_dim);
    } else {                        // binomial: each coordinate, the last one surely
        for (int L = 0; L < a_dim; ++L) {
            if (uniform(0, 1) < a_settings.CR || L == a_dim - 1) t[n] = mutant(n);
            n = (n + 1) % a_dim;
        }
    }
    // Out of bounds: halfway from where the member is to the bound.
    for (int d = 0; d < a_dim; ++d) {
        if (t[d] < a_lower.at(d)) t[d] = (x[d] + a_lower.at(d)) / 2;
        if (t[d] > a_upper.at(d)) t[d] = (x[d] + a_upper.at(d)) / 2;
    }
    return t;
}

void DifferentialEvolution::report(const QVector<double>& costs)
{
    for (int k = 0; k < a_pending.size() && k < costs.size(); ++k) {
        const Candidate& c = a_pending.at(k);
        const double v = std::isnan(costs.at(k)) ? std::numeric_limits<double>::infinity() : costs.at(k);
        if (a_generation < 0 || v <= a_cost.at(c.member)) {
            a_population[c.member] = c.x;
            a_cost[c.member] = v;
        }
    }
    a_pending.clear();
    ++a_generation;
    updateBest();
}

void DifferentialEvolution::updateBest()
{
    for (int i = 0; i < a_cost.size(); ++i)
        if (a_cost.at(i) < a_cost.at(a_best)) a_best = i;
}

double DifferentialEvolution::costVariance() const
{
    double sum = 0, sum2 = 0;
    int n = 0;
    for (double c : a_cost) {
        if (!std::isfinite(c)) return std::numeric_limits<double>::infinity();
        sum += c;
        sum2 += c * c;
        ++n;
    }
    if (n == 0) return std::numeric_limits<double>::infinity();
    const double mean = sum / n;
    return std::max(0.0, sum2 / n - mean * mean);
}

bool DifferentialEvolution::converged() const
{
    return a_generation >= 1 && costVariance() < a_settings.minCostVariance;
}

bool DifferentialEvolution::finished() const
{
    return a_generation >= a_settings.generations || converged();
}

// ---------------------------------------------------------------------------
// NetlistScope

QStringList simulationsOf(const Schematic* schematic, const QString& simulation)
{
    QStringList all, kept;
    const Component* named = nullptr;
    for (Component* c : schematic->a_DocComps) {
        if (!c->isSimulation || c->Model == QLatin1String(".Opt")) continue;
        all << c->Name.toLower();
        if (c->Name.compare(simulation, Qt::CaseInsensitive) == 0) named = c;
    }
    if (simulation.isEmpty() || named == nullptr) return all;
    // Down the sweeps: a .SW names the simulation it runs.
    for (const Component* c = named; c != nullptr;) {
        kept << c->Name.toLower();
        const Component* inner = nullptr;
        if (c->Model == QLatin1String(".SW") && !c->Props.isEmpty())
            for (Component* o : schematic->a_DocComps)
                if (o->isSimulation && o->Name.compare(c->Props.at(0)->Value, Qt::CaseInsensitive) == 0
                    && !kept.contains(o->Name.toLower()))
                    inner = o;
        c = inner;
    }
    // A Fourier analysis goes with its transient.
    for (Component* c : schematic->a_DocComps)
        if (c->Model == QLatin1String(".FOURIER") && !c->Props.isEmpty()
            && kept.contains(c->Props.at(0)->Value.toLower()))
            kept << c->Name.toLower();
    return kept;
}

NetlistScope::NetlistScope(Schematic* schematic, const Problem& problem, const QMap<QString, double>& values,
                           bool allSimulations)
    : a_values(values)
{
    if (schematic == nullptr) return;
    const QStringList run = simulationsOf(schematic, allSimulations ? QString() : problem.simulation);
    QStringList defined;
    for (Component* c : schematic->a_DocComps) {
        if (c->Model == QLatin1String(".Opt")) {
            a_saved << Saved{c, -1, QString(), c->isActive};
            c->isActive = COMP_IS_OPEN;
            continue;
        }
        if (c->isSimulation && c->isActive == COMP_IS_ACTIVE && !run.contains(c->Name.toLower())) {
            a_saved << Saved{c, -1, QString(), c->isActive};
            c->isActive = COMP_IS_OPEN;
            continue;
        }
        if (c->isActive != COMP_IS_ACTIVE) continue;
        int count = 0;
        if (c->Model == QLatin1String("Eqn")) count = c->Props.size() - 1;   // the last one is "Export"
        else if (c->Model == QLatin1String("SpicePar") || c->Model == QLatin1String("SpGlobPar"))
            count = c->Props.size();
        for (int i = 0; i < count; ++i) {
            Property* p = c->Props.at(i);
            for (auto it = values.cbegin(); it != values.cend(); ++it) {
                if (p->Name.compare(it.key(), Qt::CaseInsensitive) != 0) continue;
                a_saved << Saved{c, i, p->Value, c->isActive};
                p->Value = number(it.value());
                defined << it.key();
            }
        }
    }
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (!defined.contains(it.key())) a_undefined << it.key();
}

NetlistScope::~NetlistScope()
{
    // Backwards: a property saved twice gets its first value back.
    for (int i = a_saved.size() - 1; i >= 0; --i) {
        const Saved& s = a_saved.at(i);
        if (s.property < 0) s.component->isActive = s.active;
        else if (s.property < s.component->Props.size()) s.component->Props.at(s.property)->Value = s.value;
    }
}

QString NetlistScope::extraParameters() const
{
    QString s;
    for (const QString& name : a_undefined)
        s += QStringLiteral(".PARAM %1=%2\n").arg(name, number(a_values.value(name)));
    return s;
}

QString number(double value)
{
    return QString::number(value, 'g', 7);
}

} // namespace qucs_s::optimization
