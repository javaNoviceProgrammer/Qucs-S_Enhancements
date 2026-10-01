/*
 * erc.h - electrical rule check of a schematic, before a simulation
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef ERC_H
#define ERC_H

#include <QList>
#include <QPoint>
#include <QString>

#include <functional>

class Schematic;

/*!
 * The checks the simulator would fail on, or silently do something
 * else than intended, made before it runs and shown with a place to
 * click: no ground, component pins connected to nothing, wire ends
 * connected to nothing, two components of one name, no simulation
 * block; a wire's end or a pin on another net's wire without a
 * junction, two nets' wires over each other; parts that reach no ground
 * (floating), nets that reach it only through capacitors or current
 * sources (no DC path). Same-named labels and ground symbols count as
 * connections. notes() tells what is fine if meant: crossings, a label
 * on one pin alone.
 */
namespace qucs_s::erc {

enum class Severity { Error, Warning };

struct Issue {
    Severity severity;
    QString message;    ///< "R1: pin 2 is connected to nothing"
    QPoint where;       ///< model coordinates of the place to show
    QString component;  ///< the component's name, when one is meant
    QString file = QString();   ///< the schematic file the issue is in (its document name)
    bool operator==(const Issue& o) const
    { return severity == o.severity && message == o.message && where == o.where && component == o.component && file == o.file; }
};

/// The issues of \a doc, errors first, in the order they were found;
/// each carries the document's name as its file.
QList<Issue> check(Schematic* doc);

/// What the wires of \a doc show and do not do - a wire's end or a pin on
/// another net's wire mid-way, two nets' wires over each other (part of
/// check()), and wires of two nets crossing without a junction (a note):
/// for a tool that tells what a wire it drew did.
QList<Issue> wiring(Schematic* doc);

/// What is fine if meant, and worth a look when not (not part of check(),
/// where a drawing would be full of them): wires of two nets crossing
/// without a junction - no connection there; a net label on one pin alone
/// - a node named to be plotted or read by an expression, or a label that
/// was to match another. Warnings, each with its place.
QList<Issue> notes(Schematic* doc);

/// The subcircuit files \a doc uses directly (Subcircuit components, as
/// absolute paths, each once) - those that are there: a file not found is
/// an error of check(), not a path to read.
QStringList subcircuitFiles(Schematic* doc);

/// A subcircuit's own findings (check()).
struct SubcircuitFindings {
    QString file;
    QList<Issue> issues;
};

/// The findings of each subcircuit \a doc uses, at any depth, each file
/// once, depth first in the order they are met: an open document as it is,
/// unsaved changes included (\a open finds it by its file, or gives
/// nullptr), the others read from disk. A file that does not load is one
/// error; a file that uses one above it (a -> b -> a) is an error of the
/// file that closes the cycle.
QList<SubcircuitFindings> checkSubcircuits(Schematic* doc, const std::function<Schematic*(const QString&)>& open);

/// What a pin's name says it is: "supply" (VCC, VEE, VDD, V+, POSRAIL ...),
/// "input" (INP, IN-, NONINV, POSIN, IN1+, and one of one pin: IN, IN1,
/// VIN) or "output" (OUT, VOUT, OUT1, OUTA); empty for any other name. The
/// check reads pins so (an op-amp's input by the names of a pair only).
QString pinRole(const QString& name);

/// How many of \a issues are errors.
int errorCount(const QList<Issue>& issues);
/// What a simulation of \a doc runs besides the simulator, with the
/// user's rights: each active System command part's command (in a shell,
/// after each run), each line of ngspice text that runs one (shell,
/// system, !) - a custom simulation's, Nutmeg's, .spiceinit's -, and the
/// Octave script the Document Settings run after each simulation. Each
/// said so ("CMD1 runs a command in a shell after each simulation: touch
/// x"); check() warns of each.
QStringList commandsRun(Schematic* doc);

} // namespace qucs_s::erc

#endif // ERC_H
