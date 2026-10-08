/*
 * probe.h - cross-probing: what is clicked on the schematic (a net, a
 * part's pin, a part) put into a diagram as its voltage, its current or
 * its power, and the net a trace shows found again on the schematic
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef QUCS_PROBE_H
#define QUCS_PROBE_H

#include <QPoint>
#include <QString>
#include <QStringList>

#include <optional>

class Component;
class Diagram;
class Graph;
class Node;
class Schematic;
class Wire;

namespace qucs_s::probe {

/// What a click probes: a net's voltage, the current into a part's pin,
/// or a part's power.
enum class Kind { Voltage, Current, Power };

struct Target {
    Kind kind = Kind::Voltage;
    Wire* wire = nullptr;            ///< a voltage: a wire of the net,
    Node* node = nullptr;            ///< ... or a node of it
    Component* component = nullptr;  ///< a current or a power: the part
    int pin = -1;                    ///< a current: its pin (from 0)
};

/// What is at \a at, in the schematic's coordinates: a part's pin end, a
/// wire, a node or a net label, else a part's body. None on empty paper,
/// a ground, a port, a simulation or an equation.
std::optional<Target> at(Schematic* sch, const QPoint& at);

/// The name the netlist gives the net of \a target ("out", "_net3"; "0"
/// for ground), the netlist written to find it - the nodes' names kept as
/// they were (a DC bias shown keeps its values). Empty with \a error.
QString netName(Schematic* sch, const Target& target, QString* error);

/// The vector the simulator writes for \a target: v(out); i(v1), @r1[i],
/// @q1[ic] (the device's current at the terminal of the pin, as SPICE
/// orders them); @r1[p]. Empty, and why in \a error, when there is none:
/// a part of no device of its own (a subcircuit), Qucsator.
QString vectorOf(Schematic* sch, const Target& target, QString* error);

/// The vectors a probe asks the netlist to save beyond the labelled nets
/// (currents and powers): written as .options savecurrents, .save all
/// with the powers, and each analysis' write.
inline bool isCurrentVector(const QString& v) { return v.startsWith(QLatin1Char('@')) && !v.endsWith(QLatin1String("[p]")); }
inline bool isPowerVector(const QString& v) { return v.startsWith(QLatin1Char('@')) && v.endsWith(QLatin1String("[p]")); }

/// The diagram probes go into: the schematic's selected one, else the last
/// one probed into, else its first that draws curves; null when it has
/// none (a new one is made).
Diagram* frontDiagram(Schematic* sch);

struct Result {
    QString variable;      ///< the trace's variable, as a graph names it
    QString labelled;      ///< a net label put on an unnamed net
    QString saved;         ///< a vector the netlist is to save now
    bool hasData = false;  ///< whether the dataset has it already
    Diagram* diagram = nullptr;
    bool newDiagram = false;
    Graph* graph = nullptr;
    bool already = false;  ///< the diagram had the trace: nothing added
    QString error;
    /// What to tell: what was added where, and what the next run brings.
    QString text() const;
};

/// \a target probed: its trace in \a into (frontDiagram() when null; a
/// new Cartesian diagram below the circuit when there is none), the net
/// labelled first when it has no name (ngspice saves named nets), the
/// vector saved by the next run when the dataset lacks it. One step to
/// undo; the schematic is marked changed.
Result probe(Schematic* sch, const Target& target, Diagram* into = nullptr);

/// The net a trace shows the voltage of ("ngspice/tran.v(out)": out), or
/// the part whose current or power it shows (R1); empty when neither.
QString netOfVariable(const QString& variable);
QString partOfVariable(const QString& variable);

} // namespace qucs_s::probe

#endif
