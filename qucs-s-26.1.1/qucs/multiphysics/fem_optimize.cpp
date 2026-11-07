/*
 * fem_optimize.cpp - a multiphysics model's optimization (see fem_optimize.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_optimize.h"

#include "fem_problem.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <random>

namespace qucs_s::fem {

using optimization::Evaluated;
using optimization::SearchOptions;
using optimization::SearchOutcome;
using optimization::SearchProblem;
using optimization::SearchStatus;
using SearchVariable = optimization::Variable;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// As ngspice's: its limits, each what a dense small matrix of the search holds.
constexpr int kMostVariables = 128;
constexpr int kMostTargets = 128;
constexpr int kMostLimits = 32;
constexpr int kMostObjectives = 8;
constexpr int kMostStages = 8;
constexpr int kMostPopulation = 10000;
constexpr int kMostIterations = 1000000;
constexpr int kMostSamples = 100000;
// What is held in memory of a long run: every solve up to here, then those that
// improved (the file keeps fewer: toJson).
constexpr std::size_t kMostRows = 20000;

QString tr(const char* text)
{
    return QCoreApplication::translate("MultiphysicsOptimization", text);
}

QString cellText(const QJsonValue& v)
{
    if (v.isString()) return v.toString().trimmed();
    if (v.isDouble()) return formatNumber(v.toDouble(), 15);
    if (v.isBool()) return v.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
    return QString();
}

/// A value as a parameter's expression: in SI, its unit after it.
QString valueText(double v, const Dim& dim)
{
    const QString number = QString::number(v, 'g', 17);
    return dim.isNone() ? number : number + QLatin1Char('[') + dimName(dim) + QLatin1Char(']');
}

/// A number of the settings: \a text as a plain number, an expression of the
/// parameters; \a fallback when empty. False and why where it is not one.
bool settingNumber(const Node& node, const QString& key, const ParameterScope& ps, double* out, QString* error)
{
    const NodeKind* kind = node.kind();
    const PropertyDef* def = kind ? kind->property(key) : nullptr;
    const QString text = cellText(node.value(key));
    if (text.isEmpty()) return true;
    QString why;
    const std::optional<double> v = evaluateConstant(text, ps.scope, Dim(), &why);
    if (!v || !std::isfinite(*v)) {
        *error = tr("%1: its %2 \"%3\" is not a number%4").arg(node.name(), def ? def->label : key, text, why.isEmpty() ? QString() : QStringLiteral(": ") + why);
        return false;
    }
    *out = *v;
    return true;
}

/// A whole number of the settings, within \a least and \a most.
bool settingInteger(const Node& node, const QString& key, int least, int most, int* out, QString* error)
{
    const NodeKind* kind = node.kind();
    const PropertyDef* def = kind ? kind->property(key) : nullptr;
    const QJsonValue v = node.value(key);
    double d = 0;
    bool whole = false;
    if (v.isDouble()) {
        d = v.toDouble();
        whole = std::isfinite(d) && d == std::floor(d);
    } else if (v.isString()) {
        d = v.toString().trimmed().toDouble(&whole);
        whole = whole && std::isfinite(d) && d == std::floor(d);
    } else if (v.isUndefined() || v.isNull()) {
        return true;
    }
    if (!whole || d < least || d > most) {
        *error = tr("%1: its %2 is %3 - a whole number from %4 to %5")
                     .arg(node.name(), def ? def->label : key, v.isDouble() || v.isString() ? cellText(v) : tr("not a number"))
                     .arg(least)
                     .arg(most);
        return false;
    }
    *out = int(d);
    return true;
}

/// \a text as a value of \a dim, its unit said: an expression in \a scope. None
/// and why in \a error where it is not a constant of that unit.
std::optional<double> rangeValue(const QString& text, const Scope& scope, const Dim& dim, const QString& what, QString* error)
{
    const Expression e = Expression::compile(text, scope);
    if (!e.isValid()) {
        *error = tr("%1: %2").arg(what, e.error());
        return std::nullopt;
    }
    if (!e.isConstant()) {
        *error = tr("%1: \"%2\" is not a constant").arg(what, text);
        return std::nullopt;
    }
    if (e.dim() != dim) {
        if (e.dim().isNone()) {
            *error = tr("%1: it is in %2: give it a unit (\"%3[%2]\")").arg(what, dimName(dim), text);
            return std::nullopt;
        }
        *error = tr("%1: \"%2\" is in %3, the parameter in %4").arg(what, text, dimName(e.dim()), dimName(dim));
        return std::nullopt;
    }
    if (!std::isfinite(e.constant())) {
        *error = tr("%1: \"%2\" is not finite").arg(what, text);
        return std::nullopt;
    }
    return e.constant();
}

/// The standard normal's quantile: by bisection, as few of them are asked.
double normalQuantile(double p)
{
    double lo = -8, hi = 8;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (0.5 * std::erfc(-mid / std::sqrt(2.0)) < p) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

const char* const kStrategies[] = {"DE/best/1/exp", "DE/rand/1/exp", "DE/rand-to-best/1/exp", "DE/best/2/exp", "DE/rand/2/exp",
                                   "DE/best/1/bin", "DE/rand/1/bin", "DE/rand-to-best/1/bin", "DE/best/2/bin", "DE/rand/2/bin"};

} // namespace

// ------------------------------------------------------------------ The plan

QString OptimizationObjective::kindName(Kind kind)
{
    switch (kind) {
    case Minimize: return QStringLiteral("MIN");
    case Maximize: return QStringLiteral("MAX");
    case Target: return QStringLiteral("TARGET");
    case LessEqual: return QStringLiteral("LE");
    case GreaterEqual: return QStringLiteral("GE");
    case Equal: return QStringLiteral("EQ");
    case Monitor: break;
    }
    return QStringLiteral("MON");
}

optimization::GoalType OptimizationObjective::goalType() const
{
    switch (kind) {
    case Minimize: return optimization::GoalType::Minimize;
    case Maximize: return optimization::GoalType::Maximize;
    case Target:
    case Equal: return optimization::GoalType::Equal;
    case LessEqual: return optimization::GoalType::LessEqual;
    case GreaterEqual: return optimization::GoalType::GreaterEqual;
    case Monitor: break;
    }
    return optimization::GoalType::Monitor;
}

std::vector<int> OptimizationPlan::active() const
{
    std::vector<int> out;
    for (int i = 0; i < int(variables.size()); ++i)
        if (variables[std::size_t(i)].active) out.push_back(i);
    return out;
}

std::vector<const Node*> optimizationNodes(const Model& model)
{
    std::vector<const Node*> out;
    for (const Node& c : model.root.children) {
        if (c.type == QLatin1String("optimization")) out.push_back(&c);
        if (c.type == QLatin1String("optimizations"))
            for (const Node& o : c.children) out.push_back(&o);
    }
    return out;
}

const Node* optimizationNode(const Model& model, const QString& tag)
{
    for (const Node* n : optimizationNodes(model))
        if (tag.isEmpty() || n->tag == tag) return n;
    return nullptr;
}

std::optional<OptimizationPlan> optimizationPlan(const Model& model, const Node& node, const ParameterScope& ps, QString* error)
{
    QString local;
    QString& why = error ? *error : local;
    auto refuse = [&why](const QString& text) {
        why = text;
        return std::nullopt;
    };
    OptimizationPlan plan;
    plan.tag = node.tag;
    plan.name = node.name();
    if (model.is3D()) return refuse(tr("%1: a 3D model's optimization is not built yet: a 2D model's is").arg(plan.name));
    if (!ps.errors.isEmpty()) {
        QStringList list;
        for (auto it = ps.errors.cbegin(); it != ps.errors.cend(); ++it) list << tr("%1: %2").arg(it.key(), it.value());
        return refuse(tr("%1: the parameters: %2").arg(plan.name, list.join(QLatin1String("; "))));
    }
    // The study.
    const std::vector<const Node*> studies = model.studies();
    const QString studyTag = node.text(QStringLiteral("study")).trimmed();
    const Node* study = nullptr;
    for (const Node* s : studies)
        if (studyTag.isEmpty() ? !study : s->tag == studyTag) study = s;
    if (!study) return refuse(studyTag.isEmpty() ? tr("%1: the model has no study to solve").arg(plan.name) : tr("%1: there is no study %2").arg(plan.name, studyTag));
    plan.study = study->tag;
    plan.stages << plan.study;

    // Its settings.
    const QString mode = node.text(QStringLiteral("mode")).trimmed();
    if (!mode.isEmpty() && mode != QLatin1String("optimize") && mode != QLatin1String("center"))
        return refuse(tr("%1: its mode is \"%2\" - optimize or center").arg(plan.name, mode));
    plan.center = mode == QLatin1String("center");
    QString method = node.text(QStringLiteral("method")).trimmed();
    if (method == QLatin1String("auto")) method.clear();
    if (!method.isEmpty() && !optimization::searchMethod(method)) {
        QStringList ids{QStringLiteral("auto")};
        for (const optimization::SearchMethod& m : optimization::searchMethods()) ids << QLatin1String(m.id);
        return refuse(tr("%1: its method is \"%2\" - one of %3").arg(plan.name, method, ids.join(QStringLiteral(", "))));
    }
    plan.search.method = method;
    if (!settingInteger(node, QStringLiteral("maxiter"), 1, kMostIterations, &plan.search.maxIter, &why)) return std::nullopt;
    if (!settingNumber(node, QStringLiteral("tol"), ps, &plan.search.tol, &why)) return std::nullopt;
    if (!(plan.search.tol > 0)) return refuse(tr("%1: its tolerance must be above 0").arg(plan.name));
    if (!settingInteger(node, QStringLiteral("population"), 0, kMostPopulation, &plan.search.population, &why)) return std::nullopt;
    {
        const QJsonValue v = node.value(QStringLiteral("seed"));
        const double s = v.isDouble() ? v.toDouble() : v.isString() ? v.toString().trimmed().toDouble() : 1;
        if (!std::isfinite(s) || s < 0 || s > 9e15 || s != std::floor(s)) return refuse(tr("%1: its seed is %2 - a whole number, 0 or more").arg(plan.name, cellText(v)));
        plan.search.seed = static_cast<unsigned long long>(s);
    }
    plan.search.polish = node.flag(QStringLiteral("polish"));
    if (!settingInteger(node, QStringLiteral("starts"), 0, 1000, &plan.search.starts, &why)) return std::nullopt;
    if (!settingNumber(node, QStringLiteral("ctol"), ps, &plan.search.ctol, &why)) return std::nullopt;
    if (!(plan.search.ctol > 0)) return refuse(tr("%1: how near a limit counts as met must be above 0").arg(plan.name));
    if (!settingNumber(node, QStringLiteral("fdstep"), ps, &plan.search.fdStep, &why)) return std::nullopt;
    if (!(plan.search.fdStep > 0 && plan.search.fdStep < 0.5)) return refuse(tr("%1: its finite difference must be above 0 and below 0.5 of a range").arg(plan.name));
    const QString strategy = node.text(QStringLiteral("strategy")).trimmed();
    {
        int found = 0;
        for (int i = 0; i < 10; ++i)
            if (strategy == QLatin1String(kStrategies[i])) found = i + 1;
        if (!strategy.isEmpty() && found == 0) return refuse(tr("%1: its variant \"%2\" is not one of differential evolution's").arg(plan.name, strategy));
        if (found > 0) plan.search.strategy = found;
    }
    if (!settingNumber(node, QStringLiteral("F"), ps, &plan.search.F, &why)) return std::nullopt;
    if (!(plan.search.F > 0 && plan.search.F <= 2)) return refuse(tr("%1: differential evolution's F must be above 0 and at most 2").arg(plan.name));
    if (!settingNumber(node, QStringLiteral("CR"), ps, &plan.search.CR, &why)) return std::nullopt;
    if (plan.search.CR < 0 || plan.search.CR > 1) return refuse(tr("%1: differential evolution's crossover must be from 0 to 1 (0: automatic)").arg(plan.name));
    if (!settingNumber(node, QStringLiteral("mincostvariance"), ps, &plan.search.minCostVariance, &why)) return std::nullopt;
    if (plan.search.minCostVariance < 0) return refuse(tr("%1: differential evolution's cost variance must not be below 0").arg(plan.name));
    const QString limits = node.text(QStringLiteral("constraints")).trimmed();
    if (!limits.isEmpty() && limits != QLatin1String("lagrangian") && limits != QLatin1String("penalty"))
        return refuse(tr("%1: its limits are held by \"%2\" - lagrangian or penalty").arg(plan.name, limits));
    plan.penalty = limits == QLatin1String("penalty");
    if (!settingNumber(node, QStringLiteral("objectiveweight"), ps, &plan.objectiveWeight, &why)) return std::nullopt;
    if (!settingNumber(node, QStringLiteral("constraintweight"), ps, &plan.constraintWeight, &why)) return std::nullopt;
    if (!(plan.objectiveWeight > 0) || !(plan.constraintWeight > 0)) return refuse(tr("%1: its weights must be above 0").arg(plan.name));
    if (!settingInteger(node, QStringLiteral("parallel"), 1, 64, &plan.parallel, &why)) return std::nullopt;
    if (!settingInteger(node, QStringLiteral("refresh"), 0, kMostIterations, &plan.refresh, &why)) return std::nullopt;
    if (!settingInteger(node, QStringLiteral("samples"), 2, kMostSamples, &plan.samples, &why)) return std::nullopt;
    plan.lhs = node.flag(QStringLiteral("lhs"));

    // The parameters it varies.
    QSet<QString> seen;
    const QJsonValue variablesValue = node.value(QStringLiteral("variables"));
    if (!variablesValue.isArray()) return refuse(tr("%1: its parameters to vary are not a table").arg(plan.name));
    const QJsonArray variableRows = variablesValue.toArray();
    for (const QJsonValue& row : variableRows) {
        if (!row.isArray()) return refuse(tr("%1: a row of its parameters to vary is not a list").arg(plan.name));
        const QJsonArray r = row.toArray();
        const QString name = cellText(r.at(0));
        if (name.isEmpty()) continue;
        if (!ps.values.contains(name)) {
            QStringList known;
            for (const Model::Parameter& p : model.parameters()) known << p.name.trimmed();
            return refuse(tr("%1: there is no parameter %2 - the model has: %3").arg(plan.name, name, known.isEmpty() ? tr("none") : known.join(QStringLiteral(", "))));
        }
        if (seen.contains(name)) return refuse(tr("%1: the parameter %2 is varied twice").arg(plan.name, name));
        seen.insert(name);
        const Dim dim = ps.dims.value(name);
        SearchVariable v;
        v.name = name;
        const QString minText = cellText(r.at(2)), maxText = cellText(r.at(3)), startText = cellText(r.at(1));
        if (minText.isEmpty() || maxText.isEmpty()) return refuse(tr("%1: %2 needs a minimum and a maximum").arg(plan.name, name));
        const std::optional<double> lo = rangeValue(minText, ps.scope, dim, tr("%1: %2's minimum").arg(plan.name, name), &why);
        if (!lo) return std::nullopt;
        const std::optional<double> hi = rangeValue(maxText, ps.scope, dim, tr("%1: %2's maximum").arg(plan.name, name), &why);
        if (!hi) return std::nullopt;
        if (!(*hi > *lo)) return refuse(tr("%1: %2: the maximum must be above the minimum").arg(plan.name, name));
        if (!std::isfinite(*hi - *lo)) return refuse(tr("%1: %2: its range is too wide to search").arg(plan.name, name));
        v.min = *lo;
        v.max = *hi;
        v.initial = ps.values.value(name);
        if (!startText.isEmpty()) {
            const std::optional<double> start = rangeValue(startText, ps.scope, dim, tr("%1: %2's start").arg(plan.name, name), &why);
            if (!start) return std::nullopt;
            v.initial = *start;
        }
        if (v.initial < v.min || v.initial > v.max) {
            plan.warnings << tr("%1 starts from %2, outside its range: from the nearest end").arg(name, formatQuantity(v.initial, dim));
            v.initial = std::clamp(v.initial, v.min, v.max);
        }
        const QString type = cellText(r.at(4));
        if (!type.isEmpty()) {
            if (!optimization::knownVariableType(type))
                return refuse(tr("%1: %2's type \"%3\" is not one of LIN_DOUBLE, LOG_DOUBLE, LIN_INT, LOG_INT, E3, E6, E12, E24, E48, E96, E192").arg(plan.name, name, type));
            v.type = type;
        }
        if (v.type.startsWith(QLatin1String("LOG_")) && !(v.min > 0))
            plan.warnings << tr("%1: a logarithmic scale needs a range above 0: on a linear one").arg(name);
        if (v.type.contains(QLatin1String("INT")) && std::ceil(v.min) > std::floor(v.max)) return refuse(tr("%1: %2: no whole number between its minimum and maximum").arg(plan.name, name));
        if (!optimization::eSeries(v.type).isEmpty() && !(v.min > 0)) return refuse(tr("%1: %2: a series of preferred values needs a range above 0").arg(plan.name, name));
        const QString active = cellText(r.at(5)).toLower();
        v.active = active != QLatin1String("no") && active != QLatin1String("false");
        plan.variables.push_back(v);
        plan.dims.push_back(dim);
    }
    if (plan.active().empty()) return refuse(tr("%1 varies no parameter: name one, with its minimum and maximum").arg(plan.name));
    if (int(plan.variables.size()) > kMostVariables) return refuse(tr("%1: more than %2 parameters").arg(plan.name).arg(kMostVariables));

    // What it asks of the results.
    int goals = 0, targets = 0, minmax = 0, constraints = 0;
    const QJsonValue objectivesValue = node.value(QStringLiteral("objectives"));
    if (!objectivesValue.isArray()) return refuse(tr("%1: its objectives are not a table").arg(plan.name));
    for (const QJsonValue& row : objectivesValue.toArray()) {
        if (!row.isArray()) return refuse(tr("%1: a row of its objectives is not a list").arg(plan.name));
        const QJsonArray r = row.toArray();
        OptimizationObjective o;
        o.expression = cellText(r.at(0));
        if (o.expression.isEmpty()) continue;
        const QString kind = cellText(r.at(1)).toUpper();
        if (kind == QLatin1String("MIN") || kind.isEmpty()) o.kind = OptimizationObjective::Minimize;
        else if (kind == QLatin1String("MAX")) o.kind = OptimizationObjective::Maximize;
        else if (kind == QLatin1String("TARGET")) o.kind = OptimizationObjective::Target;
        else if (kind == QLatin1String("LE")) o.kind = OptimizationObjective::LessEqual;
        else if (kind == QLatin1String("GE")) o.kind = OptimizationObjective::GreaterEqual;
        else if (kind == QLatin1String("EQ")) o.kind = OptimizationObjective::Equal;
        else if (kind == QLatin1String("MON")) o.kind = OptimizationObjective::Monitor;
        else return refuse(tr("%1: %2's kind \"%3\" is not one of MIN, MAX, TARGET, LE, GE, EQ, MON").arg(plan.name, o.expression, kind));
        o.valueText = cellText(r.at(2));
        if (o.kind == OptimizationObjective::Target || o.isLimit()) {
            if (o.valueText.isEmpty()) return refuse(tr("%1: %2 %3 needs a value").arg(plan.name, o.expression, kind));
            const Expression e = Expression::compile(o.valueText, ps.scope);
            if (!e.isValid()) return refuse(tr("%1: %2's value: %3").arg(plan.name, o.expression, e.error()));
            if (!e.isConstant() || !std::isfinite(e.constant())) return refuse(tr("%1: %2's value \"%3\" is not a constant").arg(plan.name, o.expression, o.valueText));
        }
        const QString weight = cellText(r.at(3));
        if (!weight.isEmpty()) {
            QString bad;
            const std::optional<double> w = evaluateConstant(weight, ps.scope, Dim(), &bad);
            if (!w || !std::isfinite(*w) || !(*w > 0)) return refuse(tr("%1: %2's weight \"%3\" must be a number above 0").arg(plan.name, o.expression, weight));
            o.weight = *w;
        }
        const QString over = cellText(r.at(4)).toLower();
        if (!over.isEmpty() && over != QLatin1String("worst") && over != QLatin1String("last"))
            return refuse(tr("%1: %2's \"over\" is \"%3\" - worst or last").arg(plan.name, o.expression, over));
        o.last = over == QLatin1String("last");
        o.study = cellText(r.at(5));
        if (!o.study.isEmpty()) {
            bool known = false;
            for (const Node* s : studies) known = known || s->tag == o.study;
            if (!known) return refuse(tr("%1: %2 is read from study %3, which the model has not").arg(plan.name, o.expression, o.study));
            if (!plan.stages.contains(o.study)) plan.stages << o.study;
        }
        goals += o.isGoal() ? 1 : 0;
        targets += o.kind == OptimizationObjective::Target ? 1 : 0;
        minmax += o.kind == OptimizationObjective::Minimize || o.kind == OptimizationObjective::Maximize ? 1 : 0;
        constraints += o.isLimit() ? 1 : 0;
        plan.objectives.push_back(o);
    }
    if (goals + constraints == 0) return refuse(tr("%1 has nothing to reach (only values to monitor, or none): give an objective, a target or a limit").arg(plan.name));
    if (targets > kMostTargets) return refuse(tr("%1: more than %2 targets").arg(plan.name).arg(kMostTargets));
    if (constraints > kMostLimits) return refuse(tr("%1: more than %2 limits").arg(plan.name).arg(kMostLimits));
    if (plan.stages.size() > kMostStages) return refuse(tr("%1: more than %2 studies").arg(plan.name).arg(kMostStages));

    // A parameter its study sweeps as well: the sweep's values are what is solved.
    for (const QString& tag : std::as_const(plan.stages)) {
        const Node* solved = model.find(tag);
        if (!solved) continue;
        for (const Node& step : solved->children) {
            if (step.type != QLatin1String("sweep") || !step.enabled) continue;
            for (const QJsonValue& row : step.value(QStringLiteral("parameters")).toArray())
                if (seen.contains(cellText(row.toArray().at(0))))
                    plan.warnings << tr("%1 is swept by %2 and varied here: the sweep's values are what is solved, so its value here changes nothing").arg(cellText(row.toArray().at(0)), solved->name());
        }
    }

    // What a method can take.
    const int active = int(plan.active().size());
    const bool lm = method == QLatin1String("lm") || (method.isEmpty() && !plan.center && targets > 0 && minmax == 0);
    if (plan.center) {
        if (method == QLatin1String("nsga2") || method == QLatin1String("tune") || method == QLatin1String("lm"))
            return refuse(tr("%1: design centering has one number to maximize, the worst Cpk: the method %2 does not take it").arg(plan.name, method));
        if (goals > 0) return refuse(tr("%1: design centering's specifications are limits (LE, GE): it cannot be combined with MIN, MAX or TARGET").arg(plan.name));
        for (const OptimizationObjective& o : plan.objectives)
            if (o.kind == OptimizationObjective::Equal) return refuse(tr("%1: %2: a specification has room: LE or GE, not EQ").arg(plan.name, o.expression));
        if (constraints == 0) return refuse(tr("%1: design centering needs at least one specification (LE or GE)").arg(plan.name));
        // The spreads.
        const QJsonArray rows = node.value(QStringLiteral("variation")).toArray();
        QSet<QString> spread;
        for (const QJsonValue& row : rows) {
            const QJsonArray r = row.toArray();
            const QString name = cellText(r.at(0));
            if (name.isEmpty()) continue;
            if (!ps.values.contains(name)) return refuse(tr("%1: there is no parameter %2 to vary about its value").arg(plan.name, name));
            if (spread.contains(name)) return refuse(tr("%1: the parameter %2 has two spreads").arg(plan.name, name));
            spread.insert(name);
            OptimizationVariation var;
            var.parameter = name;
            const QString distribution = cellText(r.at(1)).toLower();
            if (!distribution.isEmpty() && distribution != QLatin1String("gauss") && distribution != QLatin1String("uniform"))
                return refuse(tr("%1: %2's distribution \"%3\" - gauss or uniform").arg(plan.name, name, distribution));
            var.uniform = distribution == QLatin1String("uniform");
            QString text = cellText(r.at(2));
            if (text.isEmpty()) return refuse(tr("%1: %2 needs a spread (its sigma, or half its width): 0.1[mm], or 5 %").arg(plan.name, name));
            if (text.endsWith(QLatin1Char('%'))) {
                bool ok = false;
                const double share = text.chopped(1).trimmed().toDouble(&ok);
                if (!ok || !(share > 0) || !std::isfinite(share)) return refuse(tr("%1: %2's spread \"%3\" is not a share above 0").arg(plan.name, name, text));
                var.relative = true;
                var.spread = share / 100;
            } else {
                const std::optional<double> s = rangeValue(text, ps.scope, ps.dims.value(name), tr("%1: %2's spread").arg(plan.name, name), &why);
                if (!s) return std::nullopt;
                if (!(*s > 0)) return refuse(tr("%1: %2's spread must be above 0").arg(plan.name, name));
                var.spread = *s;
            }
            plan.variation.push_back(var);
        }
        if (plan.variation.empty()) return refuse(tr("%1: design centering needs at least one parameter's spread").arg(plan.name));
    } else {
        if (method == QLatin1String("nsga2")) {
            if (goals < 2) return refuse(tr("%1: NSGA-II needs at least two objectives (MIN, MAX or TARGET)").arg(plan.name));
            if (goals > kMostObjectives) return refuse(tr("%1: more than %2 objectives").arg(plan.name).arg(kMostObjectives));
            if (plan.search.polish || plan.search.starts > 0)
                return refuse(tr("%1: %2 cannot be combined with NSGA-II: a Pareto front has no single best point").arg(plan.name, plan.search.polish ? tr("polish") : tr("more starts")));
        } else if (method == QLatin1String("tune")) {
            if (targets != active || minmax > 0)
                return refuse(tr("%1: false position and Broyden's method need as many targets as parameters varied (%2 targets, %3 parameters, %4 MIN or MAX)")
                                  .arg(plan.name)
                                  .arg(targets)
                                  .arg(active)
                                  .arg(minmax));
        } else if (lm) {
            if (targets == 0 || minmax > 0) return refuse(tr("%1: Levenberg-Marquardt fits targets; it does not minimize an expression (MIN, MAX)").arg(plan.name));
            if (plan.penalty && constraints > 0) return refuse(tr("%1: Levenberg-Marquardt holds limits by an augmented Lagrangian, not by penalties").arg(plan.name));
        }
    }
    if (active > 12 && method == QLatin1String("bayes"))
        plan.warnings << tr("Bayesian optimization is for up to about a dozen parameters: its surrogate slows and loses its way past that");
    bool stepped = false;
    for (int i : plan.active()) {
        const QString& type = plan.variables[std::size_t(i)].type;
        stepped = stepped || type.contains(QLatin1String("INT")) || !optimization::eSeries(type).isEmpty();
    }
    if (stepped && (method == QLatin1String("lm") || method == QLatin1String("tr") || method == QLatin1String("tune") || (method.isEmpty() && lm)))
        plan.warnings << tr("whole numbers and preferred values are flat between their steps: a gradient or a quadratic model sees nothing there; a global method suits them");
    return plan;
}

// ------------------------------------------------------------------- The result

bool OptimizationResult::canApply(QString* why) const
{
    QString local;
    QString& reason = why ? *why : local;
    if (!error.isEmpty()) {
        reason = error;
        return false;
    }
    if (status == QLatin1String("nosolve")) {
        reason = QCoreApplication::translate("MultiphysicsOptimization", "No evaluation solved: there is nothing to write in.");
        return false;
    }
    if (status == QLatin1String("infeasible")) {
        reason = QCoreApplication::translate("MultiphysicsOptimization", "A limit could not be met: this is not written in (its values are in the result).");
        return false;
    }
    if (!hasBest() && front.empty()) {
        reason = QCoreApplication::translate("MultiphysicsOptimization", "It found no best point.");
        return false;
    }
    return true;
}

namespace {

QString rowText(const OptimizationRow& r)
{
    QStringList cells{QString::number(r.index)};
    auto number = [](double v) { return std::isfinite(v) ? QString::number(v, 'g', 10) : QStringLiteral("nan"); };
    for (double v : r.values) cells << number(v);
    for (double v : r.measured) cells << number(v);
    cells << number(r.cost) << (r.feasible ? QStringLiteral("1") : QStringLiteral("0")) << (r.best ? QStringLiteral("1") : QStringLiteral("0"));
    return cells.join(QLatin1Char(' '));
}

bool rowFrom(const QString& text, int values, int measured, OptimizationRow* row)
{
    const QStringList cells = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (cells.size() != 1 + values + measured + 3) return false;
    bool ok = false;
    row->index = cells.at(0).toInt(&ok);
    if (!ok) return false;
    auto number = [](const QString& s) {
        bool good = false;
        const double v = s.toDouble(&good);
        return good && std::isfinite(v) ? v : kNaN;
    };
    int at = 1;
    for (int i = 0; i < values; ++i) row->values.push_back(number(cells.at(at++)));
    for (int i = 0; i < measured; ++i) row->measured.push_back(number(cells.at(at++)));
    row->cost = number(cells.at(at++));
    row->feasible = cells.at(at++) == QLatin1String("1");
    row->best = cells.at(at++) == QLatin1String("1");
    return true;
}

QJsonArray textsOf(const QStringList& list)
{
    return QJsonArray::fromStringList(list);
}

QStringList textsIn(const QJsonValue& value)
{
    QStringList out;
    for (const QJsonValue& v : value.toArray())
        if (v.isString()) out << v.toString();
    return out;
}

QJsonArray valuesOf(const QList<OptimizationResult::Value>& list)
{
    QJsonArray out;
    for (const OptimizationResult::Value& v : list) {
        QJsonObject o{{QStringLiteral("name"), v.name}, {QStringLiteral("text"), v.text}};
        if (std::isfinite(v.value)) o.insert(QStringLiteral("value"), v.value);
        out.append(o);
    }
    return out;
}

QList<OptimizationResult::Value> valuesIn(const QJsonValue& value)
{
    QList<OptimizationResult::Value> out;
    for (const QJsonValue& v : value.toArray()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) continue;
        OptimizationResult::Value one;
        one.name = name;
        one.value = o.value(QStringLiteral("value")).isDouble() ? o.value(QStringLiteral("value")).toDouble() : kNaN;
        one.text = o.value(QStringLiteral("text")).toString();
        out << one;
    }
    return out;
}

} // namespace

QJsonObject OptimizationResult::toJson(int rows) const
{
    QJsonObject o;
    if (!error.isEmpty()) {
        o.insert(QStringLiteral("error"), error);
        return o;
    }
    o.insert(QStringLiteral("method"), method);
    o.insert(QStringLiteral("status"), status);
    o.insert(QStringLiteral("summary"), summary);
    o.insert(QStringLiteral("setup"), setup);
    o.insert(QStringLiteral("settled"), settled);
    o.insert(QStringLiteral("feasible"), feasible);
    o.insert(QStringLiteral("evaluations"), evaluations);
    o.insert(QStringLiteral("solves"), solves);
    o.insert(QStringLiteral("failed"), failed);
    o.insert(QStringLiteral("seconds"), seconds);
    o.insert(QStringLiteral("parameters"), valuesOf(parameters));
    o.insert(QStringLiteral("measured"), valuesOf(measured));
    o.insert(QStringLiteral("kinds"), textsOf(kinds));
    o.insert(QStringLiteral("units"), textsOf(units));
    if (!notes.isEmpty()) o.insert(QStringLiteral("notes"), textsOf(notes));
    if (!search.isEmpty()) o.insert(QStringLiteral("search"), textsOf(search));
    if (!constraints.isEmpty()) o.insert(QStringLiteral("constraints"), textsOf(constraints));
    if (std::isfinite(yield)) o.insert(QStringLiteral("yield"), yield);
    if (std::isfinite(cpk)) o.insert(QStringLiteral("cpk"), cpk);
    if (!front.empty()) {
        QJsonArray designs;
        for (const FrontPoint& p : front) {
            QJsonArray values, got;
            for (double v : p.values) values.append(v);
            for (double v : p.measured) got.append(std::isfinite(v) ? QJsonValue(v) : QJsonValue());
            designs.append(QJsonObject{{QStringLiteral("values"), values}, {QStringLiteral("measured"), got}});
        }
        o.insert(QStringLiteral("front"), designs);
    }
    o.insert(QStringLiteral("columns"), textsOf(columns));
    // Those that improved, and of the others as many as there is room for, the
    // latest: a long run's file does not grow without end, a line a solve.
    QJsonArray lines;
    const std::size_t room = std::size_t(std::max(rows, 0));
    std::size_t improving = 0;
    for (const OptimizationRow& r : history) improving += r.best ? 1 : 0;
    std::size_t others = history.size() - improving;
    std::size_t skip = others + improving > room && others > (room > improving ? room - improving : 0) ? others - (room > improving ? room - improving : 0) : 0;
    for (const OptimizationRow& r : history) {
        if (!r.best && skip > 0) {
            --skip;
            continue;
        }
        lines.append(rowText(r));
    }
    o.insert(QStringLiteral("history"), lines);
    return o;
}

OptimizationResult OptimizationResult::fromJson(const QJsonObject& o)
{
    OptimizationResult r;
    r.error = o.value(QStringLiteral("error")).toString();
    if (!r.error.isEmpty()) return r;
    r.method = o.value(QStringLiteral("method")).toString();
    r.status = o.value(QStringLiteral("status")).toString();
    r.summary = o.value(QStringLiteral("summary")).toString();
    r.setup = o.value(QStringLiteral("setup")).toString();
    r.settled = o.value(QStringLiteral("settled")).toBool();
    r.feasible = o.value(QStringLiteral("feasible")).toBool(true);
    r.evaluations = o.value(QStringLiteral("evaluations")).toInt();
    r.solves = o.value(QStringLiteral("solves")).toInt();
    r.failed = o.value(QStringLiteral("failed")).toInt();
    r.seconds = o.value(QStringLiteral("seconds")).toDouble();
    r.parameters = valuesIn(o.value(QStringLiteral("parameters")));
    r.measured = valuesIn(o.value(QStringLiteral("measured")));
    r.kinds = textsIn(o.value(QStringLiteral("kinds")));
    r.units = textsIn(o.value(QStringLiteral("units")));
    r.notes = textsIn(o.value(QStringLiteral("notes")));
    r.search = textsIn(o.value(QStringLiteral("search")));
    r.constraints = textsIn(o.value(QStringLiteral("constraints")));
    if (o.value(QStringLiteral("yield")).isDouble()) r.yield = o.value(QStringLiteral("yield")).toDouble();
    if (o.value(QStringLiteral("cpk")).isDouble()) r.cpk = o.value(QStringLiteral("cpk")).toDouble();
    r.columns = textsIn(o.value(QStringLiteral("columns")));
    for (const QJsonValue& v : o.value(QStringLiteral("front")).toArray()) {
        const QJsonObject p = v.toObject();
        FrontPoint point;
        for (const QJsonValue& x : p.value(QStringLiteral("values")).toArray()) point.values.push_back(x.isDouble() ? x.toDouble() : kNaN);
        for (const QJsonValue& x : p.value(QStringLiteral("measured")).toArray()) point.measured.push_back(x.isDouble() ? x.toDouble() : kNaN);
        r.front.push_back(std::move(point));
    }
    // (As many cells as its columns say; another row, not read.)
    const int columns = int(r.columns.size());
    const int measuredColumns = int(r.kinds.size());
    if (columns > 0 && measuredColumns <= columns)
        for (const QJsonValue& v : o.value(QStringLiteral("history")).toArray()) {
            OptimizationRow row;
            if (v.isString() && rowFrom(v.toString(), columns - measuredColumns, measuredColumns, &row)) r.history.push_back(std::move(row));
        }
    return r;
}

QString optimizationSetup(const Model& model, const Node& node)
{
    const QJsonObject whole = model.toObject();
    QJsonObject setup;
    // The parameters varied start from whatever they are: not what it is of.
    QSet<QString> varied;
    for (const QJsonValue& row : node.value(QStringLiteral("variables")).toArray()) varied.insert(cellText(row.toArray().at(0)));
    QJsonArray parameters;
    for (const QJsonValue& p : whole.value(QStringLiteral("parameters")).toArray())
        if (!varied.contains(p.toObject().value(QStringLiteral("name")).toString().trimmed())) parameters.append(p);
    setup.insert(QStringLiteral("parameters"), parameters);
    setup.insert(QStringLiteral("functions"), whole.value(QStringLiteral("functions")));
    setup.insert(QStringLiteral("components"), whole.value(QStringLiteral("components")));
    setup.insert(QStringLiteral("studies"), whole.value(QStringLiteral("studies")));
    QJsonObject mine = node.props;
    mine.remove(QStringLiteral("result"));
    setup.insert(QStringLiteral("optimization"), mine);
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(setup).toJson(QJsonDocument::Compact), QCryptographicHash::Md5).toHex());
}

QString parameterText(const Model& model, const ParameterScope& parameters, const QString& name, double value)
{
    static const QRegularExpression withUnit(QStringLiteral("^\\s*[-+]?[0-9.]+(?:[eE][-+]?[0-9]+)?\\s*\\[(.+)\\]\\s*$"));
    const Dim dim = parameters.dims.value(name);
    for (const Model::Parameter& p : model.parameters()) {
        if (p.name.trimmed() != name) continue;
        const QRegularExpressionMatch m = withUnit.match(p.expression);
        if (!m.hasMatch()) break;
        const std::optional<Unit> u = parseUnit(m.captured(1));
        if (u && u->dim == dim && u->scale != 0)
            return QStringLiteral("%1[%2]").arg(QString::number((value - u->offset) / u->scale, 'g', 12), m.captured(1));
        break;
    }
    return valueText(value, dim);
}

QJsonArray optimizationVariableRow(const Model& model, const ParameterScope& parameters, const QString& name)
{
    const double value = parameters.values.value(name);
    const double low = value > 0 ? value / 2 : value < 0 ? value * 2 : -1, high = value > 0 ? value * 2 : value < 0 ? value / 2 : 1;
    return QJsonArray{name, QString(), parameterText(model, parameters, name, low), parameterText(model, parameters, name, high), QStringLiteral("LIN_DOUBLE"), QStringLiteral("yes")};
}

bool applyOptimum(Model& model, const Node& node, int point, QStringList* written, QString* error)
{
    QString local;
    QString& why = error ? *error : local;
    const QJsonValue kept = node.value(QStringLiteral("result"));
    if (!kept.isObject() || kept.toObject().isEmpty()) {
        why = QCoreApplication::translate("MultiphysicsOptimization", "%1 has not been run: run it first.").arg(node.name());
        return false;
    }
    const OptimizationResult r = OptimizationResult::fromJson(kept.toObject());
    if (!r.canApply(&why)) return false;
    // The values to write: the optimum, or a design of its front.
    QList<QPair<QString, double>> values;
    if (!r.front.empty()) {
        if (point < 1 || point > int(r.front.size())) {
            why = QCoreApplication::translate("MultiphysicsOptimization", "A Pareto front has no one optimum: say which of its %1 designs (from 1).").arg(r.front.size());
            return false;
        }
        const OptimizationResult::FrontPoint& p = r.front[std::size_t(point - 1)];
        const int count = int(r.columns.size()) - int(r.kinds.size());
        for (int i = 0; i < count && i < int(p.values.size()); ++i) values << qMakePair(r.columns.at(i), p.values[std::size_t(i)]);
    } else {
        if (point != 0 && point != 1) {
            why = QCoreApplication::translate("MultiphysicsOptimization", "'point' is a Pareto front's: this has one optimum.");
            return false;
        }
        for (const OptimizationResult::Value& v : r.parameters) values << qMakePair(v.name, v.value);
    }
    if (values.isEmpty()) {
        why = QCoreApplication::translate("MultiphysicsOptimization", "It found no best point.");
        return false;
    }
    QList<Model::Parameter> parameters = model.parameters();
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(model);
    QStringList notThere, said;
    for (const auto& [name, value] : std::as_const(values)) {
        if (!std::isfinite(value)) continue;
        bool found = false;
        const QString text = parameterText(model, *ps, name, value);
        for (Model::Parameter& p : parameters) {
            if (p.name.trimmed() != name) continue;
            found = true;
            p.expression = text;
            said << QStringLiteral("%1 = %2").arg(name, text);
        }
        if (!found) notThere << name;
    }
    if (said.isEmpty()) {
        why = QCoreApplication::translate("MultiphysicsOptimization", "The model has none of its parameters (%1).").arg(notThere.join(QStringLiteral(", ")));
        return false;
    }
    model.setParameters(parameters);
    if (written) *written = said;
    if (!notThere.isEmpty()) why = QCoreApplication::translate("MultiphysicsOptimization", "The model no longer has: %1").arg(notThere.join(QStringLiteral(", ")));
    return true;
}

// -------------------------------------------------------------------- The run

namespace {

/// What a candidate's solve measured.
struct Measured {
    bool solved = false;
    bool stopped = false;     ///< stopped before it had an answer: nothing is known of it
    bool fatal = false;       ///< solved, but what is asked cannot be read: no candidate's could be
    QString why;
    std::vector<double> y;    ///< by objective
    std::vector<Dim> dims;
    double yield = kNaN, cpk = kNaN;
};

/// An expression of \a solution as it stands (its snapshot selected): its
/// globals and parameters, and the values of its Derived Values nodes it names
/// (lp1.Z0; a node of one value by its tag).
bool valueOn(Solution& solution, const QString& text, double* value, Dim* dim, QString* why)
{
    Scope local(&solution.scope());
    if (const Node* derived = solution.model().results().child(QStringLiteral("derived")))
        for (const Node& d : derived->children) {
            if (!d.enabled || !text.contains(d.tag)) continue;
            static const QRegularExpression name(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.]*$"));
            const DerivedResult r = evaluateDerived(solution, d);
            for (const DerivedValue& v : r.values)
                if (name.match(v.name).hasMatch()) local.setConstant(d.tag + QLatin1Char('.') + v.name, v.value, v.dim);
            if (r.values.size() == 1) local.setConstant(d.tag, r.values.first().value, r.values.first().dim);
        }
    const Expression e = Expression::compile(text, local);
    if (!e.isValid()) {
        *why = e.error();
        return false;
    }
    if (!e.isConstant()) {
        *why = tr("\"%1\" varies over the model: a global value, or a Derived Values node's, is one number").arg(text);
        return false;
    }
    *value = e.constant();
    *dim = e.dim();
    return true;
}

class Runner : public optimization::SearchEvaluator
{
public:
    Runner(const Model& model, const OptimizationPlan& plan, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Topology> topology,
           std::shared_ptr<const Mesh> mesh, const Progress& progress, const OptimizationListener& listener)
        : a_model(model), a_plan(plan), a_params(std::move(parameters)), a_topology(std::move(topology)), a_baseMesh(std::move(mesh)),
          a_progress(progress), a_listener(listener), a_active(plan.active())
    {
        const QJsonObject whole = model.toObject();
        const QJsonObject component = whole.value(QStringLiteral("components")).toArray().first().toObject();
        a_shaping.shaping.insert(QStringLiteral("geometry"), component.value(QStringLiteral("geometry")));
        a_shaping.shaping.insert(QStringLiteral("mesh"), component.value(QStringLiteral("mesh")));
        a_shaping.shaping.insert(QStringLiteral("functions"), whole.value(QStringLiteral("functions")));
        for (int i : a_active) a_columns << a_plan.variables[std::size_t(i)].name;
        for (const OptimizationObjective& o : a_plan.objectives) a_columns << o.expression;
        a_scale.assign(a_plan.objectives.size(), 1.0);
        if (a_plan.center) makeDraws();
    }

    OptimizationResult run();

    std::vector<Evaluated> evaluate(const std::vector<std::vector<double>>& points) override;
    bool stopRequested() override { return a_stop.load() || !a_fatal.isEmpty(); }
    void iterated(double fraction, int iteration, double best) override;
    void said(const QString& line) override;

private:
    using Key = std::vector<double>;

    Key valuesAt(const std::vector<double>& cube) const;
    std::vector<double> startCube() const;
    Measured solveAt(const Key& values);
    Measured solveOnce(const QHash<QString, QString>& overrides, const Progress& tell);
    Measured center(const Key& values);
    void record(const Key& values, const Measured& m);
    Evaluated evaluatedOf(const Measured& m) const;
    bool ready(const Measured& m);
    void makeDraws();
    bool told(double f, const QString& what);

    const Model& a_model;
    const OptimizationPlan& a_plan;
    std::shared_ptr<ParameterScope> a_params;
    std::shared_ptr<const Topology> a_topology;
    std::shared_ptr<const Mesh> a_baseMesh;
    const Progress& a_progress;
    const OptimizationListener& a_listener;
    const std::vector<int> a_active;
    SweepPlan a_shaping;
    QStringList a_columns;

    std::map<Key, Measured> a_cache;
    std::vector<double> a_scale;
    std::vector<double> a_values;      // objectives' values once read (SI)
    bool a_targetsRead = false;
    std::vector<Dim> a_dims;
    std::atomic<bool> a_stop{false};
    QString a_fatal;
    std::mutex a_mutex;                // what the solves at once tell, the base mesh, a_warnings
    bool a_baseTried = false;
    QString a_baseError;
    double a_fraction = 0;
    double a_estimated = 1;
    QString a_where;
    int a_evaluations = 0, a_solves = 0;
    QStringList a_warnings;
    QString a_firstFailure;
    std::vector<OptimizationRow> a_rows;
    double a_bestCost = std::numeric_limits<double>::infinity();
    bool a_bestFeasible = false;
    std::vector<std::vector<double>> a_draws;   // by sample, by spread: of a unit's size
};

void Runner::makeDraws()
{
    // Every candidate sees the same draws: its Cpk a function of it alone.
    const int samples = a_plan.samples, count = int(a_plan.variation.size());
    a_draws.assign(std::size_t(samples), std::vector<double>(std::size_t(count), 0.0));
    std::mt19937_64 rng(a_plan.search.seed ? a_plan.search.seed : 1);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    std::normal_distribution<double> normal(0.0, 1.0);
    for (int j = 0; j < count; ++j) {
        const bool flat = a_plan.variation[std::size_t(j)].uniform;
        if (a_plan.lhs) {
            // A Latin hypercube: one stratum per sample, shuffled.
            std::vector<int> strata(std::size_t(samples), 0);
            for (int s = 0; s < samples; ++s) strata[std::size_t(s)] = s;
            std::shuffle(strata.begin(), strata.end(), rng);
            for (int s = 0; s < samples; ++s) {
                const double p = (strata[std::size_t(s)] + uniform(rng)) / samples;
                a_draws[std::size_t(s)][std::size_t(j)] = flat ? 2 * p - 1 : normalQuantile(p);
            }
        } else {
            for (int s = 0; s < samples; ++s) a_draws[std::size_t(s)][std::size_t(j)] = flat ? 2 * uniform(rng) - 1 : normal(rng);
        }
    }
}

bool Runner::told(double f, const QString& what)
{
    std::lock_guard<std::mutex> lock(a_mutex);
    if (!a_progress) return !a_stop.load();
    const double each = 1.0 / std::max(a_estimated, 1.0);
    const double fraction = std::min(0.995, a_fraction + each * std::clamp(f, 0.0, 1.0));
    const bool go = a_progress(fraction, a_where.isEmpty() ? what : a_where + QStringLiteral(": ") + what);
    if (!go) a_stop = true;
    return go && !a_stop.load();
}

Runner::Key Runner::valuesAt(const std::vector<double>& cube) const
{
    Key values;
    values.reserve(a_plan.variables.size());
    std::size_t k = 0;
    for (const SearchVariable& v : a_plan.variables) {
        if (!v.active) {
            values.push_back(v.value(v.coordinate(v.initial)));
            continue;
        }
        const double u = k < cube.size() ? cube[k] : 0.0;
        ++k;
        values.push_back(v.value(v.lower() + std::clamp(u, 0.0, 1.0) * (v.upper() - v.lower())));
    }
    return values;
}

std::vector<double> Runner::startCube() const
{
    std::vector<double> cube;
    for (int i : a_active) {
        const SearchVariable& v = a_plan.variables[std::size_t(i)];
        const double width = v.upper() - v.lower();
        cube.push_back(width > 0 ? std::clamp((v.coordinate(v.initial) - v.lower()) / width, 0.0, 1.0) : 0.0);
    }
    return cube;
}

Measured Runner::solveOnce(const QHash<QString, QString>& overrides, const Progress& tell)
{
    Measured m;
    // The candidate as the model itself, not as parameters beside it: a study's
    // sweep reads each of its points' parameters from the model.
    Model candidate = a_model;
    QList<Model::Parameter> rows = candidate.parameters();
    for (Model::Parameter& p : rows) {
        const auto it = overrides.constFind(p.name.trimmed());
        if (it != overrides.constEnd()) p.expression = *it;
    }
    candidate.setParameters(rows);
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(candidate);
    if (!ps->errors.isEmpty()) {
        QStringList list;
        for (auto it = ps->errors.cbegin(); it != ps->errors.cend(); ++it) list << tr("%1: %2").arg(it.key(), it.value());
        m.why = tr("the parameters: %1").arg(list.join(QLatin1String("; ")));
        return m;
    }
    // Its own geometry and mesh where what it changes shapes them.
    QSet<QString> changed;
    for (auto it = ps->values.cbegin(); it != ps->values.cend(); ++it)
        if (!a_params->values.contains(it.key()) || a_params->values.value(it.key()) != it.value()) changed.insert(it.key());
    std::shared_ptr<const Topology> topology;
    std::shared_ptr<const Mesh> mesh;
    if (!a_shaping.reshapes(changed)) {
        std::lock_guard<std::mutex> lock(a_mutex);
        if (!a_baseTried) {
            // (Under the lock: the solves at once wait for the one mesh they share.)
            a_baseTried = true;
            if (!a_baseMesh) {
                if (!a_topology) {
                    const GeometryBuild g = buildGeometry(a_model, *a_params);
                    if (!g.ok()) {
                        const Node* n = a_model.find(g.failedAt);
                        a_baseError = tr("the geometry: %1: %2").arg(n ? n->name() : g.failedAt, g.errors.value(g.failedAt));
                    } else {
                        a_topology = g.topology;
                    }
                }
                if (a_topology) {
                    const MeshBuild built = buildMesh(a_model, *a_params, a_topology);
                    if (!built.mesh) a_baseError = tr("the mesh: %1").arg(built.error);
                    a_baseMesh = built.mesh;
                }
            }
        }
        if (!a_baseError.isEmpty()) {
            m.why = a_baseError;
            return m;
        }
        topology = a_topology;
        mesh = a_baseMesh;
    }
    std::vector<SolutionSet> sets(std::size_t(a_plan.stages.size()));
    for (int s = 0; s < a_plan.stages.size(); ++s) {
        if (a_stop.load()) {
            m.stopped = true;
            return m;
        }
        StudyRun run = runStudy(candidate, ps, topology, mesh, a_plan.stages.at(s), tell);
        if (a_stop.load()) {
            m.stopped = true;
            return m;
        }
        {
            std::lock_guard<std::mutex> lock(a_mutex);
            for (const QString& w : std::as_const(run.warnings))
                if (!a_warnings.contains(w)) a_warnings << w;
        }
        if (!run.ok()) {
            m.why = run.error;
            return m;
        }
        if (!run.stopped.isEmpty()) {
            m.why = run.stopped;
            return m;
        }
        if (!mesh) mesh = run.mesh;
        sets[std::size_t(s)].solutions = std::move(run.solutions);
        sets[std::size_t(s)].swept = run.swept;
    }
    // What is asked, read on its study's solutions: over all of them, or the last.
    m.y.assign(a_plan.objectives.size(), kNaN);
    m.dims.assign(a_plan.objectives.size(), Dim());
    for (std::size_t i = 0; i < a_plan.objectives.size(); ++i) {
        const OptimizationObjective& o = a_plan.objectives[i];
        const int stage = o.study.isEmpty() ? 0 : int(a_plan.stages.indexOf(o.study));
        const SolutionSet& set = sets[std::size_t(stage)];
        QString asked;
        const std::vector<Instance> at = instances(set, !o.last, 0, {}, {}, modesAsked(set, QStringLiteral("auto")), &asked);
        if (at.empty()) {
            m.why = asked.isEmpty() ? tr("%1: no solution to read it on").arg(o.expression) : asked;
            m.fatal = true;
            return m;
        }
        QVector<double> values;
        for (const Instance& in : at) {
            Solution* s = select(set, in);
            if (!s) continue;
            double v = kNaN;
            Dim d;
            QString why;
            if (!valueOn(*s, o.expression, &v, &d, &why)) {
                // (What the results have, to choose from.)
                QStringList names;
                for (const Variable& g : s->globals()) names << g.name;
                names.sort(Qt::CaseInsensitive);
                m.why = tr("%1: %2 - the results' global values are: %3").arg(o.expression, why, names.isEmpty() ? tr("none") : names.mid(0, 60).join(QStringLiteral(", ")));
                m.fatal = true;
                return m;
            }
            m.dims[i] = d;
            values << v;
        }
        // A sweep counts by its worst point, as an optimization component's goals.
        optimization::Goal goal;
        goal.type = o.goalType();
        goal.name = o.expression;
        goal.target = 0;
        if (o.kind == OptimizationObjective::Target || o.kind == OptimizationObjective::Equal) {
            // (Its value in the unit just read: this may be the first time it is known.)
            QString bad;
            const std::optional<double> t = evaluateConstant(o.valueText, a_params->scope, m.dims[i], &bad);
            goal.target = t ? *t : 0;
        }
        m.y[i] = values.size() == 1 ? values.first() : optimization::measure(goal, values);
    }
    m.solved = true;
    return m;
}

Measured Runner::center(const Key& values)
{
    // The candidate's design, each sample's draws about it: the same for every one.
    QHash<QString, double> design;
    for (std::size_t i = 0; i < values.size(); ++i) design.insert(a_plan.variables[i].name, values[i]);
    auto overridesOf = [this, &design](int s) {
        QHash<QString, QString> overrides;
        for (std::size_t i = 0; i < a_plan.variables.size(); ++i) {
            const SearchVariable& v = a_plan.variables[i];
            overrides.insert(v.name, valueText(design.value(v.name), a_plan.dims[i]));
        }
        for (std::size_t j = 0; j < a_plan.variation.size(); ++j) {
            const OptimizationVariation& var = a_plan.variation[j];
            const double base = design.contains(var.parameter) ? design.value(var.parameter) : a_params->values.value(var.parameter);
            const double spread = var.relative ? var.spread * std::abs(base) : var.spread;
            overrides.insert(var.parameter, valueText(base + spread * a_draws[std::size_t(s)][j], a_params->dims.value(var.parameter)));
        }
        return overrides;
    };
    const int samples = a_plan.samples, specs = int(a_plan.objectives.size());
    std::vector<Measured> each;
    each.resize(std::size_t(samples));
    const Progress tell = [this](double f, const QString& what) { return told(f, what); };
    for (int from = 0; from < samples && !a_stop.load(); from += a_plan.parallel) {
        const int to = std::min(samples, from + a_plan.parallel);
        {
            std::lock_guard<std::mutex> lock(a_mutex);
            a_where = tr("evaluation %1, sample %2 of %3").arg(a_evaluations + 1).arg(from + 1).arg(samples);
        }
        if (to - from == 1) {
            each[std::size_t(from)] = solveOnce(overridesOf(from), tell);
        } else {
            detail::Threads pool;
            for (int s = from; s < to; ++s) {
                const QHash<QString, QString> overrides = overridesOf(s);
                pool.start([this, s, overrides, &each, &tell] { each[std::size_t(s)] = solveOnce(overrides, tell); });
            }
            pool.join();
        }
    }
    Measured m;
    if (a_stop.load()) {
        m.stopped = true;
        return m;
    }
    std::vector<double> sum(std::size_t(specs), 0.0), squares(std::size_t(specs), 0.0);
    m.dims.assign(std::size_t(specs), Dim());
    int solved = 0, passed = 0;
    QString failure;
    for (const Measured& one : each) {
        if (one.fatal) return one;
        if (!one.solved) {
            if (failure.isEmpty()) failure = one.why;
            continue;
        }
        ++solved;
        bool pass = true;
        for (int i = 0; i < specs; ++i) {
            const double y = one.y[std::size_t(i)];
            const OptimizationObjective& o = a_plan.objectives[std::size_t(i)];
            m.dims[std::size_t(i)] = one.dims[std::size_t(i)];
            sum[std::size_t(i)] += y;
            squares[std::size_t(i)] += y * y;
            QString bad;
            const std::optional<double> limit = o.isLimit() ? evaluateConstant(o.valueText, a_params->scope, one.dims[std::size_t(i)], &bad) : std::nullopt;
            if (limit && ((o.kind == OptimizationObjective::LessEqual && y > *limit) || (o.kind == OptimizationObjective::GreaterEqual && y < *limit))) pass = false;
        }
        if (pass) ++passed;
    }
    if (solved == 0) {
        m.why = failure.isEmpty() ? tr("no sample solved") : failure;
        return m;
    }
    m.y.assign(std::size_t(specs), kNaN);
    double worst = 1e30;
    for (int i = 0; i < specs; ++i) {
        const OptimizationObjective& o = a_plan.objectives[std::size_t(i)];
        const double mean = sum[std::size_t(i)] / solved;
        m.y[std::size_t(i)] = mean;
        if (!o.isLimit()) continue;
        const double variance = squares[std::size_t(i)] / solved - mean * mean;
        const double sigma = variance > 0 ? std::sqrt(variance) : 0;
        QString bad;
        const std::optional<double> limit = evaluateConstant(o.valueText, a_params->scope, m.dims[std::size_t(i)], &bad);
        if (!limit) {
            m.fatal = true;
            m.why = tr("%1's value: %2").arg(o.expression, bad);
            return m;
        }
        double cpk;
        if (sigma < 1e-300) {
            // No spread: inside or out, so the search still moves it in.
            const bool within = o.kind == OptimizationObjective::LessEqual ? mean <= *limit : mean >= *limit;
            cpk = within ? 100.0 : -100.0;
        } else {
            cpk = o.kind == OptimizationObjective::LessEqual ? (*limit - mean) / (3 * sigma) : (mean - *limit) / (3 * sigma);
        }
        worst = std::min(worst, cpk);
    }
    m.solved = true;
    m.cpk = worst;
    m.yield = double(passed) / samples;
    return m;
}

Measured Runner::solveAt(const Key& values)
{
    if (a_plan.center) return center(values);
    QHash<QString, QString> overrides;
    for (std::size_t i = 0; i < values.size(); ++i) overrides.insert(a_plan.variables[i].name, valueText(values[i], a_plan.dims[i]));
    const Progress tell = [this](double f, const QString& what) { return told(f, what); };
    return solveOnce(overrides, tell);
}

/// What is asked read, once the results have said their units: each value
/// in its objective's, each scale from the first candidate solved. False, and
/// a_fatal, where one cannot be.
bool Runner::ready(const Measured& m)
{
    if (a_targetsRead) return true;
    a_values.assign(a_plan.objectives.size(), 0.0);
    a_dims = m.dims;
    for (std::size_t i = 0; i < a_plan.objectives.size(); ++i) {
        const OptimizationObjective& o = a_plan.objectives[i];
        if (o.kind == OptimizationObjective::Target || o.isLimit()) {
            const Expression e = Expression::compile(o.valueText, a_params->scope);
            if (e.isValid() && !e.dim().isNone() && e.dim() != m.dims[i]) {
                a_fatal = tr("%1 is in %2; its value \"%3\" in %4").arg(o.expression, dimName(m.dims[i]), o.valueText, dimName(e.dim()));
                return false;
            }
            QString why;
            const std::optional<double> v = evaluateConstant(o.valueText, a_params->scope, m.dims[i], &why);
            if (!v) {
                a_fatal = tr("%1's value \"%2\": %3").arg(o.expression, o.valueText, why);
                return false;
            }
            a_values[i] = *v;
        }
        // The scale: a value's size, else what the first solved measured, else 1.
        const double at = m.y[i];
        const double scale = (o.kind == OptimizationObjective::Target || o.isLimit()) && a_values[i] != 0 ? std::abs(a_values[i])
                             : std::isfinite(at) && at != 0                                              ? std::abs(at)
                                                                                                         : 1.0;
        a_scale[i] = scale;
    }
    a_targetsRead = true;
    return true;
}

Evaluated Runner::evaluatedOf(const Measured& m) const
{
    Evaluated e;
    e.stopped = m.stopped;
    if (!m.solved) return e;
    e.solved = true;
    if (a_plan.center) {
        e.objective = -m.cpk;
        return e;
    }
    const bool nsga = a_plan.search.method == QLatin1String("nsga2");
    double cost = 0, penalties = 0;
    bool anyGoal = false, allMet = true;
    for (std::size_t i = 0; i < a_plan.objectives.size(); ++i) {
        const OptimizationObjective& o = a_plan.objectives[i];
        const double y = m.y[i];
        if (o.kind == OptimizationObjective::Monitor) continue;
        if (!std::isfinite(y)) {
            e.solved = false;
            return e;
        }
        const double s = a_scale[i];
        switch (o.kind) {
        case OptimizationObjective::Minimize:
            anyGoal = true;
            cost += (a_plan.penalty ? a_plan.objectiveWeight : 1.0) * o.weight * y / s;
            if (nsga) e.objectives.push_back(o.weight * y / s);
            break;
        case OptimizationObjective::Maximize:
            anyGoal = true;
            cost -= (a_plan.penalty ? a_plan.objectiveWeight : 1.0) * o.weight * y / s;
            if (nsga) e.objectives.push_back(-o.weight * y / s);
            break;
        case OptimizationObjective::Target: {
            anyGoal = true;
            const double r = o.weight * (y - a_values[i]) / s;
            e.residuals.push_back(r);
            cost += r * r;
            if (nsga) e.objectives.push_back(std::abs(r));
            break;
        }
        default: {
            const double t = std::abs(a_values[i]) > 0 ? std::abs(a_values[i]) : 1.0;
            const double miss = o.kind == OptimizationObjective::LessEqual      ? std::max(0.0, y - a_values[i])
                                : o.kind == OptimizationObjective::GreaterEqual ? std::max(0.0, a_values[i] - y)
                                                                                : std::abs(y - a_values[i]);
            if (a_plan.penalty && !nsga) {
                penalties += a_plan.constraintWeight * o.weight * miss / t;
            } else {
                e.constraints.push_back(y);
            }
            allMet = allMet && miss / std::max(1.0, t) <= (a_plan.penalty ? 1e-9 : a_plan.search.ctol);
            break;
        }
        }
    }
    e.objective = cost + penalties;
    // (Only limits, all of them met: nothing to improve.)
    e.goalsMet = !anyGoal && allMet;
    return e;
}

void Runner::record(const Key& values, const Measured& m)
{
    ++a_solves;
    OptimizationRow row;
    row.index = a_solves;
    for (int i : a_active) row.values.push_back(values[std::size_t(i)]);
    row.measured = m.y;
    row.measured.resize(a_plan.objectives.size(), kNaN);
    row.why = m.why;
    if (m.solved && !a_fatal.isEmpty()) return;
    const Evaluated e = evaluatedOf(m);
    if (e.solved) {
        row.cost = e.objective;
        for (std::size_t i = 0; i < a_plan.objectives.size(); ++i) {
            const OptimizationObjective& o = a_plan.objectives[i];
            if (!o.isLimit()) continue;
            const double t = std::abs(a_values[i]) > 0 ? std::abs(a_values[i]) : 1.0;
            const double miss = o.kind == OptimizationObjective::LessEqual      ? std::max(0.0, m.y[i] - a_values[i])
                                : o.kind == OptimizationObjective::GreaterEqual ? std::max(0.0, a_values[i] - m.y[i])
                                                                                : std::abs(m.y[i] - a_values[i]);
            if (!a_plan.center && miss / std::max(1.0, t) > a_plan.search.ctol) row.feasible = false;
        }
        const bool better = (row.feasible && !a_bestFeasible) || (row.feasible == a_bestFeasible && row.cost < a_bestCost);
        if (better) {
            row.best = true;
            a_bestCost = row.cost;
            a_bestFeasible = row.feasible;
        }
    } else if (a_firstFailure.isEmpty() && !m.why.isEmpty()) {
        a_firstFailure = m.why;
    }
    if (a_rows.size() < kMostRows || row.best) a_rows.push_back(row);
    if (a_listener.row) a_listener.row(row, a_columns);
}

std::vector<Evaluated> Runner::evaluate(const std::vector<std::vector<double>>& points)
{
    std::vector<Key> keys;
    std::vector<std::size_t> todo;
    std::vector<Key> queued;
    for (std::size_t i = 0; i < points.size(); ++i) {
        keys.push_back(valuesAt(points[i]));
        if (a_cache.find(keys[i]) == a_cache.end() && std::find(queued.begin(), queued.end(), keys[i]) == queued.end()) {
            queued.push_back(keys[i]);
            todo.push_back(i);
        }
    }
    const std::size_t atOnce = a_plan.center ? 1 : std::size_t(std::max(a_plan.parallel, 1));
    for (std::size_t from = 0; from < todo.size() && !stopRequested(); from += atOnce) {
        const std::size_t to = std::min(todo.size(), from + atOnce);
        std::vector<Measured> found(to - from);
        {
            std::lock_guard<std::mutex> lock(a_mutex);
            a_where = tr("evaluation %1, best %2").arg(a_evaluations + 1).arg(a_bestCost < std::numeric_limits<double>::infinity() ? formatNumber(a_bestCost, 4) : tr("none yet"));
        }
        if (found.size() == 1) {
            found[0] = solveAt(keys[todo[from]]);
        } else {
            detail::Threads pool;
            for (std::size_t k = from; k < to; ++k) {
                const Key values = keys[todo[k]];
                pool.start([this, k, from, values, &found] { found[k - from] = solveAt(values); });
            }
            pool.join();
        }
        for (std::size_t k = from; k < to; ++k) {
            const Key& key = keys[todo[k]];
            Measured& m = found[k - from];
            if (m.stopped) continue;
            if (m.fatal) {
                a_fatal = m.why;
                continue;
            }
            if (m.solved && !ready(m)) continue;
            record(key, m);
            a_cache[key] = std::move(m);
            ++a_evaluations;
        }
        a_fraction = std::min(0.99, double(a_evaluations) / std::max(a_estimated, 1.0));
    }
    std::vector<Evaluated> out(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto it = a_cache.find(keys[i]);
        if (it == a_cache.end()) out[i].stopped = true;
        else out[i] = evaluatedOf(it->second);
    }
    return out;
}

void Runner::iterated(double fraction, int iteration, double best)
{
    a_fraction = std::max(a_fraction, fraction);
    if (a_plan.refresh > 0 && iteration % a_plan.refresh == 0 && a_listener.line)
        a_listener.line(tr("%1: iteration %2, best cost %3, %4 solves").arg(a_plan.name).arg(iteration).arg(formatNumber(best, 6)).arg(a_solves));
}

void Runner::said(const QString& line)
{
    if (a_listener.line) a_listener.line(tr("%1: %2").arg(a_plan.name, line));
}

OptimizationResult Runner::run()
{
    QElapsedTimer wall;
    wall.start();
    OptimizationResult result;
    result.setup = QString();
    const QString method = a_plan.search.method;
    // What is searched.
    SearchProblem problem;
    for (int i : a_active) problem.names << a_plan.variables[std::size_t(i)].name;
    problem.start = startCube();
    int targets = 0, goals = 0, minmax = 0;
    for (const OptimizationObjective& o : a_plan.objectives) {
        targets += o.kind == OptimizationObjective::Target ? 1 : 0;
        minmax += o.kind == OptimizationObjective::Minimize || o.kind == OptimizationObjective::Maximize ? 1 : 0;
        goals += o.isGoal() ? 1 : 0;
    }
    const bool leastSquares = !a_plan.center && targets > 0 && minmax == 0;
    if (leastSquares) {
        problem.residuals = targets;
        for (const OptimizationObjective& o : a_plan.objectives)
            if (o.kind == OptimizationObjective::Target) problem.wholeOff += o.weight * o.weight;
    }
    if (method == QLatin1String("nsga2")) problem.objectives = goals;
    SearchOptions options = a_plan.search;
    {
        // How far it may go: for its progress.
        const double n = double(problem.start.size());
        const double pop = options.population > 0 ? options.population : 10 + 4 * n;
        double each = options.maxIter;
        if (method == QLatin1String("de") || method == QLatin1String("pso") || method == QLatin1String("nsga2")) each = pop * (options.maxIter + 1);
        else if (method == QLatin1String("sa")) each = 13 + (8 + 4 * n) * options.maxIter;
        else if (method == QLatin1String("cmaes")) each = (options.population > 0 ? options.population : 4 + std::floor(3 * std::log(std::max(n, 1.0)))) * options.maxIter;
        else if (method == QLatin1String("tr")) each = 2 * n + 2 + options.maxIter;
        else if (method == QLatin1String("lm") || (method.isEmpty() && leastSquares)) each = 1 + (n + 3) * options.maxIter;
        else if (method == QLatin1String("bayes") || method == QLatin1String("tune")) each = options.maxIter;
        else each = n + 1 + 2.0 * options.maxIter;
        a_estimated = each * (a_plan.center ? 1.0 : 1.0);
    }

    // The start first: what is asked is checked on it, and it sets the scales.
    // (A start that does not solve is one bad point, not the end of the search;
    // what is asked that the results do not have is: no candidate would have it.)
    (void) evaluate({problem.start});
    if (!a_fatal.isEmpty()) {
        result.error = a_fatal;
        return result;
    }

    // Limits: by the core's augmented Lagrangian, by Deb's rule under NSGA-II -
    // or, the optimization component's way, as weighted penalties in the cost.
    if (!a_plan.penalty || method == QLatin1String("nsga2"))
        for (std::size_t i = 0; i < a_plan.objectives.size(); ++i) {
            const OptimizationObjective& o = a_plan.objectives[i];
            if (!o.isLimit() || a_plan.center) continue;
            optimization::SearchConstraint c;
            c.name = o.expression;
            const double value = a_targetsRead ? a_values[i] : [&] {
                // (The start did not solve: the value as it stands.)
                const Expression e = Expression::compile(o.valueText, a_params->scope);
                return e.isValid() && e.isConstant() ? e.constant() : 0.0;
            }();
            c.hasUpper = o.kind != OptimizationObjective::GreaterEqual;
            c.hasLower = o.kind != OptimizationObjective::LessEqual;
            c.upper = c.lower = value;
            problem.constraints.push_back(c);
        }

    const SearchOutcome outcome = stopRequested() && a_cache.empty() ? SearchOutcome() : optimization::search(problem, options, *this);
    if (!a_fatal.isEmpty()) {
        result.error = a_fatal;
        return result;
    }

    result.method = method.isEmpty() ? (leastSquares ? QStringLiteral("lm") : QStringLiteral("nm")) : method;
    result.status = a_cache.empty() && a_stop.load() ? optimization::statusWord(SearchStatus::Interrupted) : optimization::statusWord(outcome.status);
    result.settled = outcome.settled();
    result.feasible = outcome.feasible;
    result.evaluations = std::max(outcome.evaluations, a_evaluations);
    result.solves = a_solves * (a_plan.center ? a_plan.samples : 1);
    result.failed = outcome.failed;
    result.notes = outcome.notes;
    result.search = outcome.search;
    result.columns = a_columns;
    for (const OptimizationObjective& o : a_plan.objectives) result.kinds << OptimizationObjective::kindName(o.kind);
    for (int i : a_active) result.units << dimName(a_plan.dims[std::size_t(i)]);
    for (std::size_t i = 0; i < a_plan.objectives.size(); ++i) result.units << (i < a_dims.size() ? dimName(a_dims[i]) : QString());
    result.seconds = double(wall.elapsed()) / 1000;
    result.history = std::move(a_rows);
    for (const QString& w : std::as_const(a_plan.warnings))
        if (!result.notes.contains(w)) result.notes << w;
    for (const QString& w : std::as_const(a_warnings))
        if (!result.notes.contains(w)) result.notes << w;
    if (!a_firstFailure.isEmpty()) result.notes << tr("the first that did not solve: %1").arg(a_firstFailure);

    const auto measuredAt = [this](const Measured& m, QList<OptimizationResult::Value>* out) {
        for (std::size_t i = 0; i < a_plan.objectives.size(); ++i) {
            OptimizationResult::Value v;
            v.name = a_plan.objectives[i].expression;
            v.value = i < m.y.size() ? m.y[i] : kNaN;
            v.text = std::isfinite(v.value) ? formatQuantity(v.value, i < m.dims.size() ? m.dims[i] : Dim(), 6) : tr("not measured");
            *out << v;
        }
    };

    // The Pareto front.
    if (method == QLatin1String("nsga2")) {
        for (const optimization::ParetoPoint& p : outcome.front) {
            const auto it = a_cache.find(valuesAt(p.x));
            if (it == a_cache.end()) continue;
            OptimizationResult::FrontPoint point;
            for (int i : a_active) point.values.push_back(it->first[std::size_t(i)]);
            point.measured = it->second.y;
            result.front.push_back(std::move(point));
        }
        result.summary = tr("%1, %2 non-dominated designs after %3 evaluations").arg(outcome.phrase).arg(result.front.size()).arg(result.evaluations);
        return result;
    }

    // The best point.
    const auto it = outcome.best.empty() ? a_cache.end() : a_cache.find(valuesAt(outcome.best));
    if (it != a_cache.end() && it->second.solved) {
        for (std::size_t i = 0; i < a_plan.variables.size(); ++i) {
            const SearchVariable& v = a_plan.variables[i];
            if (!v.active) continue;
            OptimizationResult::Value one;
            one.name = v.name;
            one.value = it->first[i];
            one.text = formatQuantity(one.value, a_plan.dims[i], 6);
            result.parameters << one;
        }
        measuredAt(it->second, &result.measured);
        result.yield = it->second.yield;
        result.cpk = it->second.cpk;
    }

    // Each limit at the optimum.
    const auto limitText = [this](std::size_t i, bool upper, double bound) {
        return tr("%1 %2 %3").arg(a_plan.objectives[i].expression, upper ? QStringLiteral("<=") : QStringLiteral(">="), formatQuantity(bound, i < a_dims.size() ? a_dims[i] : Dim(), 6));
    };
    std::vector<std::size_t> limitRows;
    for (std::size_t i = 0; i < a_plan.objectives.size(); ++i)
        if (a_plan.objectives[i].isLimit()) limitRows.push_back(i);
    for (const optimization::ConstraintState& c : outcome.constraints) {
        if (std::size_t(c.constraint) >= limitRows.size()) continue;
        const std::size_t i = limitRows[std::size_t(c.constraint)];
        const Dim dim = i < a_dims.size() ? a_dims[i] : Dim();
        const QString head = limitText(i, c.upper, c.bound);
        const QString value = formatQuantity(c.value, dim, 6);
        switch (c.kind) {
        case optimization::ConstraintState::Violated:
            result.constraints << tr("%1 - %2, VIOLATED by %3").arg(head, value, formatQuantity(c.slack, dim, 3));
            break;
        case optimization::ConstraintState::Active:
            result.constraints << tr("%1 - %2, active (multiplier %3: moving the bound changes the cost by about that much per unit)").arg(head, value).arg(c.multiplier, 0, 'g', 4);
            break;
        case optimization::ConstraintState::AtBound:
            result.constraints << tr("%1 - %2, active (at the bound)").arg(head, value);
            break;
        case optimization::ConstraintState::Slack:
            result.constraints << tr("%1 - %2, slack %3").arg(head, value, formatQuantity(c.slack, dim, 4));
            break;
        }
    }
    // (By penalties, the core knows of no limit: each met or not, as a component's goals.)
    if (a_plan.penalty && !a_plan.center && it != a_cache.end() && it->second.solved)
        for (std::size_t i : limitRows) {
            const OptimizationObjective& o = a_plan.objectives[i];
            const double y = it->second.y[i], t = a_values[i];
            const bool met = o.kind == OptimizationObjective::LessEqual      ? y <= t
                             : o.kind == OptimizationObjective::GreaterEqual ? y >= t
                                                                             : std::abs(y - t) <= 1e-9 * std::max(1.0, std::abs(t));
            const Dim dim = i < a_dims.size() ? a_dims[i] : Dim();
            result.constraints << tr("%1 %2 %3 - %4, %5")
                                      .arg(o.expression, o.kind == OptimizationObjective::LessEqual ? QStringLiteral("<=") : o.kind == OptimizationObjective::GreaterEqual ? QStringLiteral(">=") : QStringLiteral("="),
                                           formatQuantity(t, dim, 6), formatQuantity(y, dim, 6), met ? tr("met") : tr("NOT MET"));
            result.feasible = result.feasible && met;
        }

    if (!result.hasBest()) {
        // (Nothing to give a value of.)
        result.summary = tr("%1 after %2 evaluations").arg(outcome.phrase.isEmpty() ? optimization::statusWord(SearchStatus::Interrupted) : outcome.phrase).arg(result.evaluations);
    } else if (a_plan.center && it != a_cache.end() && it->second.solved) {
        result.summary = tr("centering %1 - worst-case Cpk = %2, yield = %3 % (%4 samples), after %5 evaluations")
                             .arg(outcome.phrase, formatNumber(it->second.cpk, 4), formatNumber(100 * it->second.yield, 4))
                             .arg(a_plan.samples)
                             .arg(result.evaluations);
    } else if (leastSquares) {
        result.summary = tr("%1, sum-sq residual = %2 (rms %3) after %4 evaluations")
                             .arg(outcome.phrase, formatNumber(outcome.cost, 6), formatNumber(std::sqrt(std::max(outcome.cost, 0.0) / std::max(targets, 1)), 6))
                             .arg(result.evaluations);
    } else {
        result.summary = tr("%1, objective = %2 after %3 evaluations").arg(outcome.phrase, formatNumber(outcome.cost, 6)).arg(result.evaluations);
    }
    return result;
}

} // namespace

OptimizationResult runOptimization(const Model& model, const QString& tag, std::shared_ptr<ParameterScope> parameters,
                                   std::shared_ptr<const Topology> topology, std::shared_ptr<const Mesh> mesh, const Progress& progress,
                                   const OptimizationListener& listener)
{
    OptimizationResult result;
    const Node* node = optimizationNode(model, tag);
    if (!node) {
        result.error = tag.isEmpty() ? tr("the model has no optimization: add one to it") : tr("there is no optimization %1").arg(tag);
        return result;
    }
    if (!parameters) parameters = evaluateParameters(model);
    QString why;
    const std::optional<OptimizationPlan> plan = optimizationPlan(model, *node, *parameters, &why);
    if (!plan) {
        result.error = why;
        return result;
    }
    Runner runner(model, *plan, parameters, std::move(topology), std::move(mesh), progress, listener);
    result = runner.run();
    if (result.error.isEmpty()) result.setup = optimizationSetup(model, *node);
    return result;
}

} // namespace qucs_s::fem
