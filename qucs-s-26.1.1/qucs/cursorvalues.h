/*
 * cursorvalues.h - a schematic's nets labelled with their values where a
 * diagram's marker is: at its time in a transient, its point of a sweep
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_CURSORVALUES_H
#define QUCS_CURSORVALUES_H

#include <QList>
#include <QPoint>
#include <QString>

class Marker;
class Schematic;

/*!
 * The DC bias labels show the operating point on the schematic; these
 * show the run a marker is on - a transient at the marker's time, an AC
 * or DC sweep at its point - on each named net, and follow the marker as
 * it moves. A net's voltage is the variable the run names after it
 * (ngspice's tran.v(out), Qucsator's out.Vt), at the marker's x - between
 * samples straight - and at its values of any further sweep.
 */
namespace qucs_s::cursor {

struct Value {
    QString net;
    QPoint anchor;     ///< where its label is on the schematic (a wire's or node's)
    double value = 0;  ///< a complex one's magnitude
    double phase = 0;  ///< in degrees; 0 for a real one
    bool complex = false;
    QString text;      ///< as drawn: "1.234 V", "2.1 V ∠-45°"
};

struct Reading {
    QString error;     ///< why there are none (empty: there are, or no net is named)
    QString x;         ///< the marker's variable: time, frequency, a swept source
    double at = 0;     ///< ... and its value
    QList<Value> values;
};

/// The marker the values follow on \a sch: the one chosen (choose()) while
/// it is there, else a marker selected, else the first; nullptr: none.
const Marker* source(const Schematic* sch);

/// Each named net of \a sch at \a marker's x, from the run of its trace.
Reading at(const Schematic* sch, const Marker* marker);

} // namespace qucs_s::cursor

#endif
