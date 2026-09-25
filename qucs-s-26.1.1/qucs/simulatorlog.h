/*
 * simulatorlog.h - a simulator's errors and warnings, read out of what it
 *                  printed: the message, the netlist line, the part
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_SIMULATORLOG_H
#define QUCS_SIMULATORLOG_H

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace qucs_s::simlog {

/// An error or a warning of a simulator (ngspice, Xyce, SPICE OPUS) or
/// of Qucs-S's check before it.
struct Problem {
    enum Severity { Error, Warning };
    Severity severity = Error;
    QString message;       // what was said, its lines joined
    int line = 0;          // the netlist's line it names (from 1); 0: none
    QString netlistLine;   // that line
    QString component;     // the part it is about, by its name in Qucs-S
    QString node;          // a node it names
};

/// The errors and warnings in \a output, each once. \a netlist: the
/// netlist's lines, for a line the output names by its number only;
/// \a parts: each part's SPICE name, in lower case ("xpd1", "rl"), to its
/// name in the schematic ("PD1", "RL").
QList<Problem> problems(const QString& output, const QStringList& netlist = {},
                        const QHash<QString, QString>& parts = {});
QJsonArray toJson(const QList<Problem>& problems);

} // namespace qucs_s::simlog

#endif // QUCS_SIMULATORLOG_H
