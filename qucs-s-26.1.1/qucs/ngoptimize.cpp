/*
 * ngoptimize.cpp - the NgOpt component's ngspice command (see ngoptimize.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngoptimize.h"

#include <QCoreApplication>
#include <QHash>
#include <QRegularExpression>

#include <cmath>

#include "components/component.h"
#include "ngstatistics.h"
#include "ngsweep.h"
#include "schematic.h"
#include "valuereading.h"

namespace qucs_s::ngopt {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("NgOpt", text);
}

// The fixed properties, in their order (those since the first eight after
// them); knobs, targets and constraints follow them.
const char* const kFixed[] = {"Method", "MaxIter", "Tol", "Size", "Seed", "Verbose", "Analysis", "Minimize",
                              "Polish", "Starts", "CTol"};
const int kFixedCount = 11;

// The methods of ngspice's optimize that find one optimum (not nsga2's
// Pareto front of several objectives), global first.
const struct MethodInfo {
    const char* id;
    const char* name;
    const char* note;
    bool global;
    bool population;
} kMethods[] = {
    {"de", QT_TRANSLATE_NOOP("NgOpt", "Differential evolution (global)"),
     QT_TRANSLATE_NOOP("NgOpt", "A population of candidates built from differences of its members; finds the global "
                                "minimum of rugged objectives."),
     true, true},
    {"pso", QT_TRANSLATE_NOOP("NgOpt", "Particle swarm (global)"),
     QT_TRANSLATE_NOOP("NgOpt", "A swarm pulled toward its members' and its own best points; global."), true, true},
    {"sa", QT_TRANSLATE_NOOP("NgOpt", "Simulated annealing (global)"),
     QT_TRANSLATE_NOOP("NgOpt", "A single walker that accepts uphill steps while hot; global, one simulation a step. "
                                "The iterations are its cooling levels."),
     true, false},
    {"cmaes", QT_TRANSLATE_NOOP("NgOpt", "CMA-ES evolution strategy (global)"),
     QT_TRANSLATE_NOOP("NgOpt", "A Gaussian search that learns the scale and the correlation of the parameters from how "
                                "each generation ranks; the standard choice for 2 to 50 parameters, for parameters over "
                                "decades and for correlated ones. Automatic population: 4 + 3 ln(parameters)."),
     true, true},
    {"bayes", QT_TRANSLATE_NOOP("NgOpt", "Bayesian optimization (global, few simulations)"),
     QT_TRANSLATE_NOOP("NgOpt", "A Gaussian-process model of every simulation so far chooses the next by expected "
                                "improvement: tens of simulations where the population methods take thousands, for a slow "
                                "circuit and up to a dozen parameters. The iterations are its budget of simulations "
                                "(2000 at most)."),
     true, false},
    {"nm", QT_TRANSLATE_NOOP("NgOpt", "Nelder-Mead simplex (local)"),
     QT_TRANSLATE_NOOP("NgOpt", "A downhill simplex from the initial values; fast on smooth objectives, finds the "
                                "nearest minimum."),
     false, false},
    {"tr", QT_TRANSLATE_NOOP("NgOpt", "Trust region (local)"),
     QT_TRANSLATE_NOOP("NgOpt", "A quadratic model of the objective from the initial values, one simulation a step, the "
                                "bounds inside its steps: it runs along a bound rather than stopping at it; finds the "
                                "nearest minimum."),
     false, false},
    {"lm", QT_TRANSLATE_NOOP("NgOpt", "Levenberg-Marquardt least squares (local, targets only)"),
     QT_TRANSLATE_NOOP("NgOpt", "Gradient least squares: the fastest for fitting targets on smooth responses; "
                                "needs targets."),
     false, false},
};

const MethodInfo* methodInfo(const QString& method)
{
    for (const MethodInfo& m : kMethods)
        if (method == QLatin1String(m.id)) return &m;
    return nullptr;
}

// A value as ngspice reads it: "2.2n", "4.7 uH", "1e-9" are 2.2e-09,
// 4.7e-06, 1e-09. False for anything else.
bool spiceNumber(const QString& text, QString* out)
{
    const units::Reading r = units::read(text);
    if (r.kind != units::Reading::Number || !std::isfinite(r.value)) return false;
    *out = QString::number(r.value, 'g', 10);
    return true;
}

// An expression as one token: ngspice takes a -target or -constrain
// expression as a single word and a -minimize one up to the next
// "-<letter>" - so one that begins with a minus goes in parentheses
// (quotes stay part of a -target's word).
QString oneToken(const QString& expression)
{
    QString s = expression;
    s.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (s.startsWith(QLatin1Char('-'))) s = QLatin1Char('(') + s + QLatin1Char(')');
    return s;
}

QString carryVariable(int index, int knob)
{
    return QStringLiteral("ngopt_%1_%2").arg(index).arg(knob + 1);
}

} // namespace

// ---------------------------------------------------------------------------

QString Knob::kindName(KnobKind kind)
{
    switch (kind) {
    case KnobKind::Instance: return QStringLiteral("param");
    case KnobKind::Model: return QStringLiteral("mparam");
    case KnobKind::Param: break;
    }
    return QStringLiteral("dparam");
}

KnobKind Knob::kindOf(const QString& name)
{
    if (name == QLatin1String("param")) return KnobKind::Instance;
    if (name == QLatin1String("mparam")) return KnobKind::Model;
    return KnobKind::Param;
}

bool Knob::parse(const QString& value, Knob* knob)
{
    const QStringList f = value.split(QLatin1Char('|'));
    if (f.size() < 5) return false;
    knob->kind = kindOf(f.at(0).trimmed());
    knob->name = f.at(1).trimmed();
    knob->init = f.at(2).trimmed();
    knob->lo = f.at(3).trimmed();
    knob->hi = f.at(4).trimmed();
    return true;
}

QString Knob::toString() const
{
    return QStringList({kindName(kind), name, init, lo, hi}).join(QLatin1Char('|'));
}

bool Target::parse(const QString& value, Target* target)
{
    const QStringList f = value.split(QLatin1Char('|'));
    if (f.size() < 3) return false;
    target->analysis = f.at(0).trimmed();
    target->expression = f.at(1).trimmed();
    target->value = f.at(2).trimmed();
    target->weight = f.value(3).trimmed();
    return true;
}

QString Target::toString() const
{
    return QStringList({analysis, expression, value, weight}).join(QLatin1Char('|'));
}

bool Constraint::parse(const QString& value, Constraint* constraint)
{
    const QStringList f = value.split(QLatin1Char('|'));
    if (f.size() < 3) return false;
    constraint->analysis = f.at(0).trimmed();
    constraint->expression = f.at(1).trimmed();
    constraint->min = f.at(2).trimmed();
    constraint->max = f.value(3).trimmed();
    return true;
}

QString Constraint::toString() const
{
    return QStringList({analysis, expression, min, max}).join(QLatin1Char('|'));
}

Command Command::read(const Component* component)
{
    Command c;
    if (component == nullptr) return c;
    for (const Property* p : component->Props) {
        const QString& v = p->Value;
        if (p->Name == QLatin1String("Method")) c.method = v.trimmed();
        else if (p->Name == QLatin1String("MaxIter")) c.maxIter = v.trimmed();
        else if (p->Name == QLatin1String("Tol")) c.tol = v.trimmed();
        else if (p->Name == QLatin1String("Size")) c.size = v.trimmed();
        else if (p->Name == QLatin1String("Seed")) c.seed = v.trimmed();
        else if (p->Name == QLatin1String("Verbose")) c.verbose = v.trimmed() == QLatin1String("yes");
        else if (p->Name == QLatin1String("Analysis")) c.analysis = v.trimmed();
        else if (p->Name == QLatin1String("Minimize")) c.minimize = v.trimmed();
        else if (p->Name == QLatin1String("Polish")) c.polish = v.trimmed() == QLatin1String("yes");
        else if (p->Name == QLatin1String("Starts")) c.starts = v.trimmed();
        else if (p->Name == QLatin1String("CTol")) c.ctol = v.trimmed();
        else if (p->Name == QLatin1String("Knob")) {
            Knob k;
            if (Knob::parse(v, &k)) c.knobs << k;
        } else if (p->Name == QLatin1String("Target")) {
            Target t;
            if (Target::parse(v, &t)) c.targets << t;
        } else if (p->Name == QLatin1String("Constraint")) {
            Constraint k;
            if (Constraint::parse(v, &k)) c.constraints << k;
        }
    }
    return c;
}

void Command::write(Component* component) const
{
    // The display flags as they are, by name and occurrence.
    QHash<QString, QList<bool>> shown;
    for (const Property* p : component->Props) shown[p->Name] << p->display;
    QHash<QString, int> seen;
    auto display = [&](const QString& name, bool fallback) {
        const int i = seen[name]++;
        const QList<bool>& d = shown.value(name);
        return i < d.size() ? d.at(i) : fallback;
    };
    const auto yesNo = [](bool b) { return b ? QStringLiteral("yes") : QStringLiteral("no"); };
    const QStringList fixed = {method, maxIter, tol, size, seed, yesNo(verbose), analysis, minimize, yesNo(polish), starts, ctol};
    QList<Property*> props;
    for (int i = 0; i < kFixedCount; ++i) {
        const QString name = QString::fromLatin1(kFixed[i]);
        const bool fallback = name == QLatin1String("Method") || name == QLatin1String("Minimize");
        props << new Property(name, fixed.at(i), display(name, fallback), QString());
    }
    for (const Knob& k : knobs) props << new Property(QStringLiteral("Knob"), k.toString(), display("Knob", true), QString());
    for (const Target& t : targets)
        props << new Property(QStringLiteral("Target"), t.toString(), display("Target", true), QString());
    for (const Constraint& k : constraints)
        props << new Property(QStringLiteral("Constraint"), k.toString(), display("Constraint", true), QString());
    qDeleteAll(component->Props);
    component->Props = props;
}

QList<QPair<QString, QString>> methods()
{
    QList<QPair<QString, QString>> list;
    for (const MethodInfo& m : kMethods) list << qMakePair(QString::fromLatin1(m.id), tr(m.name));
    return list;
}

QString methodNote(const QString& method)
{
    const MethodInfo* m = methodInfo(method);
    return m != nullptr ? tr(m->note) : QString();
}

bool isGlobal(const QString& method)
{
    const MethodInfo* m = methodInfo(method);
    return m != nullptr && m->global;
}

bool takesPopulation(const QString& method)
{
    const MethodInfo* m = methodInfo(method);
    return m != nullptr && m->population;
}

QString propertyNote(const QString& name)
{
    if (name == QLatin1String("Method")) {
        QStringList each;
        for (const MethodInfo& m : kMethods)
            each << QStringLiteral("%1 - %2: %3").arg(QLatin1String(m.id), tr(m.name), tr(m.note));
        return tr("The search, one of: %1 Empty: ngspice's own choice (lm for targets, nm for an expression).")
            .arg(each.join(QLatin1Char(' ')));
    }
    if (name == QLatin1String("MaxIter"))
        return tr("Iterations (generations of pso, de and cmaes; sa's cooling levels; bayes's budget of simulations); "
                  "empty: 100");
    if (name == QLatin1String("Tol")) return tr("The tolerance it converges to; empty: 1e-6");
    if (name == QLatin1String("Size"))
        return tr("The population of de, pso and cmaes; empty: automatic (10 + 4 a parameter for de and pso, "
                  "4 + 3 ln(parameters) for cmaes)");
    if (name == QLatin1String("Seed"))
        return tr("A whole number: the random stream of de, pso, sa, cmaes, bayes and of Starts; empty: ngspice's fixed "
                  "one (a run repeats either way; another seed searches differently)");
    if (name == QLatin1String("Verbose")) return tr("yes: every iteration in the simulation console");
    if (name == QLatin1String("Analysis"))
        return tr("The analysis after which Minimize is evaluated: a simulation block's name (AC1), or an ngspice "
                  "analysis command (ac lin 1 1meg 1meg)");
    if (name == QLatin1String("Minimize"))
        return tr("An ngspice expression to minimize (its last value counts); empty: the Targets are fitted by least "
                  "squares");
    if (name == QLatin1String("Polish"))
        return tr("yes: a global method's best point is finished by a local one (tr for an expression, lm for "
                  "targets), for a local method's precision");
    if (name == QLatin1String("Starts"))
        return tr("A number k: the search also runs from k Latin-hypercube points of the box, each with a share of "
                  "the iterations; the best is polished. Empty: from the initial values only");
    if (name == QLatin1String("CTol"))
        return tr("How near its bound a Constraint counts as met, relative to the bound (at least 1); empty: 1e-4");
    return {};
}

QString analysisCommand(const Schematic* schematic, const QString& analysis)
{
    const QString a = analysis.trimmed();
    if (schematic != nullptr)
        for (Component* c : schematic->a_DocComps)
            if (c->isSimulation && c->Model != QLatin1String(".NGOPT") && !qucs_s::ngstats::isStatistics(c)
                && !qucs_s::ngsweep::isSweep(c)
                && c->Name.compare(a, Qt::CaseInsensitive) == 0) {
                // A simulation switched off still says what its analysis
                // is: run only by the command that names it.
                const int active = c->isActive;
                c->isActive = COMP_IS_ACTIVE;
                const QString command = c->getSpiceNetlist().trimmed().split(QLatin1Char('\n')).value(0).trimmed();
                c->isActive = active;
                return command;
            }
    return a;
}

bool commandLine(const Command& command, const Schematic* schematic, QString* line, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (command.knobs.isEmpty()) return fail(tr("no parameter to optimize"));
    const QString method = command.method.trimmed();
    if (!method.isEmpty() && methodInfo(method) == nullptr) {
        QStringList ids;
        for (const MethodInfo& m : kMethods) ids << QLatin1String(m.id);
        if (method == QLatin1String("nsga2"))
            return fail(tr("the method nsga2 finds a Pareto front of several objectives, not one optimum: the block "
                           "takes one of %1").arg(ids.join(QStringLiteral(", "))));
        return fail(tr("the method \"%1\" is not one of %2").arg(method, ids.join(QStringLiteral(", "))));
    }
    QStringList parts{QStringLiteral("optimize")};
    for (const Knob& k : command.knobs) {
        if (k.name.isEmpty() || k.name.contains(QRegularExpression(QStringLiteral("\\s"))))
            return fail(tr("a parameter needs a name without spaces"));
        QString init, lo, hi;
        if (!spiceNumber(k.init, &init))
            return fail(tr("%1: the initial value \"%2\" is not a number").arg(k.name, k.init));
        if (!spiceNumber(k.lo, &lo)) return fail(tr("%1: the minimum \"%2\" is not a number").arg(k.name, k.lo));
        if (!spiceNumber(k.hi, &hi)) return fail(tr("%1: the maximum \"%2\" is not a number").arg(k.name, k.hi));
        if (!(hi.toDouble() > lo.toDouble())) return fail(tr("%1: the maximum must be above the minimum").arg(k.name));
        parts << QLatin1Char('-') + Knob::kindName(k.kind) << k.name << init << lo << hi;
    }
    // Each constraint as ngspice takes it, after the analysis it is
    // measured on: its expression and bounds.
    QList<QPair<QString, QStringList>> constraints;   // (analysis, words)
    if (command.constraints.size() > 32) return fail(tr("more than 32 constraints (ngspice takes up to 32)"));
    for (const Constraint& k : command.constraints) {
        if (k.expression.trimmed().isEmpty()) return fail(tr("a constraint has no expression"));
        QStringList words{QStringLiteral("-constrain"), oneToken(k.expression)};
        QString lo, hi;
        if (!k.min.isEmpty() && !spiceNumber(k.min, &lo))
            return fail(tr("%1: the constraint's minimum \"%2\" is not a number").arg(k.expression, k.min));
        if (!k.max.isEmpty() && !spiceNumber(k.max, &hi))
            return fail(tr("%1: the constraint's maximum \"%2\" is not a number").arg(k.expression, k.max));
        if (lo.isEmpty() && hi.isEmpty()) return fail(tr("the constraint %1 has neither a minimum nor a maximum").arg(k.expression));
        if (!lo.isEmpty() && !hi.isEmpty() && lo.toDouble() > hi.toDouble())
            return fail(tr("%1: the constraint's maximum is below its minimum").arg(k.expression));
        if (!lo.isEmpty()) words << QStringLiteral("-min") << lo;
        if (!hi.isEmpty()) words << QStringLiteral("-max") << hi;
        constraints << qMakePair(k.analysis.trimmed().isEmpty() ? QString() : analysisCommand(schematic, k.analysis), words);
    }
    if (!command.leastSquares()) {
        const QString analysis = analysisCommand(schematic, command.analysis);
        if (analysis.isEmpty()) return fail(tr("no analysis for the expression to minimize"));
        if (method == QLatin1String("lm"))
            return fail(tr("Levenberg-Marquardt fits targets; it does not minimize an expression"));
        parts << QStringLiteral("-analysis") << analysis << QStringLiteral("-minimize") << oneToken(command.minimize);
        // (ngspice runs one analysis for an expression to minimize.)
        for (int i = 0; i < constraints.size(); ++i) {
            if (!constraints.at(i).first.isEmpty() && constraints.at(i).first != analysis)
                return fail(tr("the constraint %1 is after %2, but an expression to minimize has one analysis: leave its "
                               "analysis empty or give %3")
                                .arg(command.constraints.at(i).expression, command.constraints.at(i).analysis,
                                     command.analysis));
            parts << constraints.at(i).second;
        }
    } else {
        if (command.targets.isEmpty()) return fail(tr("nothing to minimize and no target to fit"));
        // A stage for each analysis, in the order they first come; each
        // target after the analysis it is measured on.
        QStringList stages;
        QList<QStringList> stageTargets;
        for (const Target& t : command.targets) {
            const QString analysis = analysisCommand(schematic, t.analysis);
            if (analysis.isEmpty()) return fail(tr("the target %1 has no analysis").arg(t.expression));
            if (t.expression.trimmed().isEmpty()) return fail(tr("a target has no expression"));
            QString value, weight;
            if (!spiceNumber(t.value, &value))
                return fail(tr("%1: the target value \"%2\" is not a number").arg(t.expression, t.value));
            if (!t.weight.isEmpty() && !spiceNumber(t.weight, &weight))
                return fail(tr("%1: the weight \"%2\" is not a number").arg(t.expression, t.weight));
            int stage = int(stages.indexOf(analysis));
            if (stage < 0) {
                stages << analysis;
                stageTargets << QStringList();
                stage = int(stages.size()) - 1;
            }
            stageTargets[stage] << QStringLiteral("-target") << oneToken(t.expression) << value;
            if (!weight.isEmpty()) stageTargets[stage] << weight;
        }
        // A constraint after its analysis's targets (one of no target's
        // analysis, a stage of its own; one of none, the first target's).
        for (const auto& [analysis, words] : std::as_const(constraints)) {
            int stage = analysis.isEmpty() ? 0 : int(stages.indexOf(analysis));
            if (stage < 0) {
                stages << analysis;
                stageTargets << QStringList();
                stage = int(stages.size()) - 1;
            }
            stageTargets[stage] << words;
        }
        if (stages.size() > 8) return fail(tr("more than 8 analyses (ngspice takes up to 8 stages)"));
        for (int i = 0; i < stages.size(); ++i) parts << QStringLiteral("-analysis") << stages.at(i) << stageTargets.at(i);
    }
    if (!method.isEmpty()) parts << QStringLiteral("-method") << method;
    auto option = [&](const char* flag, const QString& value, bool integer) {
        if (value.trimmed().isEmpty()) return true;
        bool ok = false;
        const double v = value.trimmed().toDouble(&ok);
        if (!ok || (integer && v != std::floor(v))) return false;
        parts << QString::fromLatin1(flag) << value.trimmed();
        return true;
    };
    if (!option("-maxiter", command.maxIter, true)) return fail(tr("the iterations \"%1\" are not a whole number").arg(command.maxIter));
    if (!option("-tol", command.tol, false)) return fail(tr("the tolerance \"%1\" is not a number").arg(command.tol));
    if (!option("-swarmsize", command.size, true)) return fail(tr("the population \"%1\" is not a whole number").arg(command.size));
    if (!option("-seed", command.seed, true)) return fail(tr("the seed \"%1\" is not a whole number").arg(command.seed));
    if (command.polish) parts << QStringLiteral("-polish");
    if (!option("-starts", command.starts, true) || (!command.starts.trimmed().isEmpty() && command.starts.trimmed().toDouble() < 1))
        return fail(tr("the starts \"%1\" are not a whole number of 1 or more").arg(command.starts));
    if (!command.constraints.isEmpty() && !command.ctol.trimmed().isEmpty()) {
        bool ok = false;
        const double v = command.ctol.trimmed().toDouble(&ok);
        if (!ok || !(v > 0)) return fail(tr("the constraint tolerance \"%1\" is not a number above 0").arg(command.ctol));
        parts << QStringLiteral("-ctol") << command.ctol.trimmed();
    }
    if (command.verbose) parts << QStringLiteral("-verbose");
    *line = parts.join(QLatin1Char(' '));
    return true;
}

QString carryLines(const Command& command, int index)
{
    QString s;
    for (int k = 0; k < command.knobs.size(); ++k) {
        const Knob& knob = command.knobs.at(k);
        if (knob.kind == KnobKind::Param) continue;
        // optimize publishes each knob's value as optimize_<name>.
        s += QStringLiteral("set %1 = \"$&optimize_%2\"\n").arg(carryVariable(index, k), knob.name.toLower());
    }
    return s;
}

QString reapplyLines(const Command& command, int index)
{
    QString s;
    for (int k = 0; k < command.knobs.size(); ++k) {
        const Knob& knob = command.knobs.at(k);
        if (knob.kind == KnobKind::Param) continue;
        s += QStringLiteral("%1 %2 = $%3\n")
                 .arg(knob.kind == KnobKind::Model ? QStringLiteral("altermod") : QStringLiteral("alter"),
                      knob.name.toLower(), carryVariable(index, k));
    }
    return s;
}

QList<Result> parseResults(const QString& output)
{
    // The line that ends a search says why it stopped (since the
    // enhanced ngspice's E-762; "converged" and the interrupt before).
    static const QRegularExpression summary(
        QStringLiteral("^optimize: ((converged|stopped at -maxiter|cooling schedule complete|NO SOLUTION|unchanged|"
                       "INTERRUPTED|INFEASIBLE)\\b.*)$"));
    static const QHash<QString, QString> statuses{
        {QStringLiteral("converged"), QStringLiteral("converged")},
        {QStringLiteral("stopped at -maxiter"), QStringLiteral("maxiter")},
        {QStringLiteral("cooling schedule complete"), QStringLiteral("completed")},
        {QStringLiteral("NO SOLUTION"), QStringLiteral("nosolve")},
        {QStringLiteral("unchanged"), QStringLiteral("unchanged")},
        {QStringLiteral("INTERRUPTED"), QStringLiteral("interrupted")},
        {QStringLiteral("INFEASIBLE"), QStringLiteral("infeasible")}};
    static const QRegularExpression note(QStringLiteral("^optimize: NOTE -- (.*)$"));
    static const QRegularExpression constraint(QStringLiteral("^optimize: constraint (.*)$"));
    static const QRegularExpression value(QStringLiteral("^\\s+(\\S+) = (\\S+)\\s*$"));
    // Before it: the start that won (-starts), the polish's outcome; a
    // search's first line begins another's.
    static const QRegularExpression step(QStringLiteral("^optimize: (start \\d+ of \\d+ won\\b.*|polish .* -- cost .*)$"));
    static const QRegularExpression begins(QStringLiteral("^optimize: \\d+ parameters?, "));
    QList<Result> results;
    QStringList steps;
    bool open = false;
    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed().isEmpty() ? QString() : QString(raw).remove(QLatin1Char('\r'));
        if (const QRegularExpressionMatch m = summary.match(line); m.hasMatch()) {
            Result r;
            r.summary = m.captured(1);
            r.status = statuses.value(m.captured(2));
            r.interrupted = r.status == QLatin1String("interrupted");
            r.search = steps;
            steps.clear();
            results << r;
            open = true;
            continue;
        }
        if (!open) {
            if (begins.match(line).hasMatch()) steps.clear();
            else if (const QRegularExpressionMatch m = step.match(line); m.hasMatch()) steps << m.captured(1);
            continue;
        }
        if (const QRegularExpressionMatch m = note.match(line); m.hasMatch()) {
            results.last().notes << m.captured(1);
            continue;
        }
        if (const QRegularExpressionMatch m = constraint.match(line); m.hasMatch()) {
            results.last().constraints << m.captured(1);
            continue;
        }
        if (const QRegularExpressionMatch m = value.match(line); m.hasMatch()) {
            bool ok = false;
            const double v = m.captured(2).toDouble(&ok);
            if (ok) {
                results.last().values << qMakePair(m.captured(1), v);
                continue;
            }
        }
        open = false;   // the values are over (the next search's first line follows)
    }
    return results;
}

bool unsupported(const QString& output)
{
    return output.contains(QLatin1String("optimize: no such command"));
}

} // namespace qucs_s::ngopt
