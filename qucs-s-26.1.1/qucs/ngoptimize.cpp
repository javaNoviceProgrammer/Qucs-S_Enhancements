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

// The fixed properties, in their order; knobs and targets follow them.
const char* const kFixed[] = {"Method", "MaxIter", "Tol", "Size", "Seed", "Verbose", "Analysis", "Minimize"};
const int kFixedCount = 8;

// A value as ngspice reads it: "2.2n", "4.7 uH", "1e-9" are 2.2e-09,
// 4.7e-06, 1e-09. False for anything else.
bool spiceNumber(const QString& text, QString* out)
{
    const units::Reading r = units::read(text);
    if (r.kind != units::Reading::Number || !std::isfinite(r.value)) return false;
    *out = QString::number(r.value, 'g', 10);
    return true;
}

// An expression as one token: ngspice takes a -target expression as a
// single word and a -minimize one up to the next "-<letter>".
QString oneToken(const QString& expression)
{
    QString s = expression;
    s.remove(QRegularExpression(QStringLiteral("\\s+")));
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
        else if (p->Name == QLatin1String("Knob")) {
            Knob k;
            if (Knob::parse(v, &k)) c.knobs << k;
        } else if (p->Name == QLatin1String("Target")) {
            Target t;
            if (Target::parse(v, &t)) c.targets << t;
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
    const QStringList fixed = {method,  maxIter, tol, size, seed, verbose ? QStringLiteral("yes") : QStringLiteral("no"),
                               analysis, minimize};
    QList<Property*> props;
    for (int i = 0; i < kFixedCount; ++i) {
        const QString name = QString::fromLatin1(kFixed[i]);
        const bool fallback = name == QLatin1String("Method") || name == QLatin1String("Minimize");
        props << new Property(name, fixed.at(i), display(name, fallback), QString());
    }
    for (const Knob& k : knobs) props << new Property(QStringLiteral("Knob"), k.toString(), display("Knob", true), QString());
    for (const Target& t : targets)
        props << new Property(QStringLiteral("Target"), t.toString(), display("Target", true), QString());
    qDeleteAll(component->Props);
    component->Props = props;
}

QList<QPair<QString, QString>> methods()
{
    return {{QStringLiteral("de"), tr("Differential evolution (global)")},
            {QStringLiteral("pso"), tr("Particle swarm (global)")},
            {QStringLiteral("sa"), tr("Simulated annealing (global)")},
            {QStringLiteral("nm"), tr("Nelder-Mead simplex (local)")},
            {QStringLiteral("lm"), tr("Levenberg-Marquardt least squares (local, targets only)")}};
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
    if (!command.leastSquares()) {
        const QString analysis = analysisCommand(schematic, command.analysis);
        if (analysis.isEmpty()) return fail(tr("no analysis for the expression to minimize"));
        if (command.method == QLatin1String("lm"))
            return fail(tr("Levenberg-Marquardt fits targets; it does not minimize an expression"));
        parts << QStringLiteral("-analysis") << analysis << QStringLiteral("-minimize") << oneToken(command.minimize);
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
        if (stages.size() > 8) return fail(tr("more than 8 analyses (ngspice takes up to 8 stages)"));
        for (int i = 0; i < stages.size(); ++i) parts << QStringLiteral("-analysis") << stages.at(i) << stageTargets.at(i);
    }
    if (!command.method.isEmpty()) parts << QStringLiteral("-method") << command.method;
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
    static const QRegularExpression summary(
        QStringLiteral("^optimize: ((converged|INTERRUPTED -- best point so far)\\b.*)$"));
    static const QRegularExpression note(QStringLiteral("^optimize: NOTE -- (.*)$"));
    static const QRegularExpression value(QStringLiteral("^\\s+(\\S+) = (\\S+)\\s*$"));
    QList<Result> results;
    bool open = false;
    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed().isEmpty() ? QString() : QString(raw).remove(QLatin1Char('\r'));
        if (const QRegularExpressionMatch m = summary.match(line); m.hasMatch()) {
            Result r;
            r.summary = m.captured(1);
            r.interrupted = m.captured(2).startsWith(QLatin1String("INTERRUPTED"));
            results << r;
            open = true;
            continue;
        }
        if (!open) continue;
        if (const QRegularExpressionMatch m = note.match(line); m.hasMatch()) {
            results.last().notes << m.captured(1);
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
        open = false;   // the values are over
    }
    return results;
}

bool unsupported(const QString& output)
{
    return output.contains(QLatin1String("optimize: no such command"));
}

} // namespace qucs_s::ngopt
