/*
 * ngoptimize.h - the NgOpt component's ngspice command: `optimize`, the
 * parameter optimizer built into ngspice builds that have it (the
 * Ngspice_OpenVAF_Enhancements fork): what the component stores, the
 * command line it becomes, the lines that keep the optimum across the
 * netlist's resets, and the results ngspice prints
 *
 *   optimize (-param|-mparam|-dparam) <name> <init> <lo> <hi> ...
 *            -analysis <command ...>
 *            ( -minimize <expression>
 *              | -target <expr> <value> [<weight>] ... [-analysis ... -target ...] )
 *            [-constrain <expr> [-min <lo>] [-max <hi>] ...] [-ctol T]
 *            [-method de|pso|sa|cmaes|bayes|nm|tr|lm] [-swarmsize N] [-seed s]
 *            [-maxiter N] [-tol T] [-polish] [-starts k] [-verbose]
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_NGOPTIMIZE_H
#define QUCS_NGOPTIMIZE_H

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

class Component;
class Schematic;

namespace qucs_s::ngopt {

/// What a knob of the search is, and so how ngspice changes it.
enum class KnobKind {
    Param,      ///< a symbolic .param - what a Qucs equation defines (-dparam: alterparam + reset)
    Instance,   ///< a device or instance parameter: R1, @m1[w] (-param: alter)
    Model       ///< a .model parameter: @dmod[is] (-mparam: altermod)
};

/// "Knob=dparam|Cin|2.2n|470p|22n"
struct Knob {
    KnobKind kind = KnobKind::Param;
    QString name;
    QString init;
    QString lo;
    QString hi;

    static bool parse(const QString& value, Knob* knob);
    QString toString() const;
    /// "dparam", "param", "mparam" - the flag without its dash.
    static QString kindName(KnobKind kind);
    static KnobKind kindOf(const QString& name);
};

/// "Target=AC1|db(2*v(out))|-3.0103|1": an expression, evaluated on the
/// results of an analysis, and the value it should have (least squares).
struct Target {
    QString analysis;     ///< a simulation component's name, or an ngspice analysis command
    QString expression;
    QString value;
    QString weight;       ///< empty: 1

    static bool parse(const QString& value, Target* target);
    QString toString() const;
};

/// "Constraint=AC1|vdb(out)|-3|": an expression, evaluated on the results
/// of an analysis, held at or above a minimum and at or below a maximum
/// (one may be left out) while the objective is minimized or the targets
/// fitted - an augmented Lagrangian around the method.
struct Constraint {
    QString analysis;     ///< empty: the objective's (the first target's when fitting)
    QString expression;
    QString min;          ///< empty: no lower bound
    QString max;          ///< empty: no upper bound

    static bool parse(const QString& value, Constraint* constraint);
    QString toString() const;
};

/// Everything the component stores. With an expression to minimize the
/// search minimizes it after one analysis; without, it fits the targets.
struct Command {
    QString method = QStringLiteral("de");   ///< one of methods(); empty: ngspice's default
    QString maxIter;       ///< empty: ngspice's default
    QString tol;
    QString size;          ///< the population of pso, de and cmaes
    QString seed;
    bool verbose = false;
    QString analysis;      ///< of the expression to minimize
    QString minimize;
    bool polish = false;   ///< a global method's best point finished by a local one
    QString starts;        ///< more starts from Latin-hypercube points; empty: none
    QString ctol;          ///< how near its bound a constraint is met; empty: ngspice's 1e-4
    QList<Knob> knobs;
    QList<Target> targets;
    QList<Constraint> constraints;

    bool leastSquares() const { return minimize.trimmed().isEmpty(); }

    static Command read(const Component* component);
    /// Puts the command into the component's properties; what the
    /// schematic shows of them (display) is kept where it can be.
    void write(Component* component) const;
};

/// The methods, with what the dialog calls them: de, pso, sa, cmaes,
/// bayes (global), nm, tr, lm (local).
QList<QPair<QString, QString>> methods();
/// What a method does and when to choose it; empty for one that is not.
QString methodNote(const QString& method);
/// It searches the whole box (polished by a local method on request).
bool isGlobal(const QString& method);
/// It takes a population (Size): pso, de, cmaes.
bool takesPopulation(const QString& method);
/// What a property of the component holds, for Claude's description of
/// it (the properties are saved with their names, not a description).
QString propertyNote(const QString& name);

/// The analysis a stage runs: the command of the simulation component of
/// that name in the schematic ("AC1": "ac dec 20 100k 10meg") - switched
/// off too, when it is to run only there - else the text itself as an
/// ngspice command ("ac lin 1 1meg 1meg", "op").
QString analysisCommand(const Schematic* schematic, const QString& analysis);

/// The optimize line of \a command; false and why in \a error when it
/// cannot be written: no knob, a value that is not a number, nothing to
/// minimize or fit, more than eight analyses, lm without targets, a
/// method ngspice has not, a constraint without a bound or after another
/// analysis than an expression to minimize.
bool commandLine(const Command& command, const Schematic* schematic, QString* line, QString* error = nullptr);

/// ngspice leaves the circuit at the optimum; a reset (the netlist does
/// one after every simulation) re-reads the deck, which keeps a .param
/// changed by optimize but not an alter or an altermod. These lines keep
/// the in-place knobs' values in shell variables right after optimize
/// (\a index tells two NgOpt components apart), and put them back after
/// a reset. Empty when every knob is a .param.
QString carryLines(const Command& command, int index);
QString reapplyLines(const Command& command, int index);

/// What one optimize printed at its end.
struct Result {
    /// Why the search stopped, as ngspice's optimize_status says it:
    /// converged, completed (sa's cooling schedule ran out), maxiter (not
    /// converged), nosolve (no evaluation solved), unchanged (the objective
    /// never moved), interrupted (the best point so far), infeasible (a
    /// constraint was not met).
    QString status;
    bool interrupted = false;   ///< stopped by the user: the best point so far
    QString summary;            ///< "converged, objective = 1.2e-13 after 29 evaluations"
    QStringList notes;          ///< "the objective was ... at every one of the evaluations", ...
    QStringList constraints;    ///< "i(v1) <= -9e-06 -- -9.90197e-06, slack 9.02e-07", ...
    QStringList search;         ///< "start 3 of 3 won (cost 2.1e-12)", "polish converged -- cost ...", ...
    QList<QPair<QString, double>> values;   ///< the knobs, in the order given

    /// The search ended on its own criterion or schedule.
    bool settled() const { return status == QLatin1String("converged") || status == QLatin1String("completed"); }
};

/// The results in ngspice's output, one for each optimize, in order.
QList<Result> parseResults(const QString& output);
/// This ngspice has no optimize command.
bool unsupported(const QString& output);

} // namespace qucs_s::ngopt

#endif
