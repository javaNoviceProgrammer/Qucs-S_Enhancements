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

class Schematic;

/*!
 * The checks the simulator would fail on, or silently do something
 * else than intended, made before it runs and shown with a place to
 * click: no ground, component pins connected to nothing, wire ends
 * connected to nothing, two components of one name, no simulation
 * block. Same-named labels and ground symbols count as connections.
 */
namespace qucs_s::erc {

enum class Severity { Error, Warning };

struct Issue {
    Severity severity;
    QString message;    ///< "R1: pin 2 is connected to nothing"
    QPoint where;       ///< model coordinates of the place to show
    QString component;  ///< the component's name, when one is meant
    bool operator==(const Issue& o) const
    { return severity == o.severity && message == o.message && where == o.where && component == o.component; }
};

/// The issues of \a doc, errors first, in the order they were found.
QList<Issue> check(Schematic* doc);

/// How many of \a issues are errors.
int errorCount(const QList<Issue>& issues);

} // namespace qucs_s::erc

#endif // ERC_H
