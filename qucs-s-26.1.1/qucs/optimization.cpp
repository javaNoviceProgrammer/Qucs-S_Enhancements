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
#include "datasetfile.h"

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
    // A binary one (datasetfile.h): its blocks, the same way.
    if (qucs_s::datasetfile::isBinary(file)) {
        qucs_s::datasetfile::BinaryReader data;
        if (!data.open(file, error)) return table;
        for (const qucs_s::datasetfile::Block& b : data.blocks()) {
            QVector<double> re, im;
            if (!data.values(b, &re, &im)) continue;
            if (std::any_of(im.cbegin(), im.cend(), [](double i) { return i != 0; }))
                for (qsizetype i = 0; i < re.size(); ++i) re[i] = std::hypot(re.at(i), im.at(i));
            table.insert(b.name(), re);
        }
        if (table.isEmpty() && error != nullptr) *error = tr("%1 holds no results").arg(file);
        return table;
    }
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

} // namespace qucs_s::optimization
