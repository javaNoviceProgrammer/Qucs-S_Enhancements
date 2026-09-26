/*
 * qucscontrol_p.h - what the two halves of QucsControl share: the forms
 *                   of a tool's result, and the diagrams' traces
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_QUCSCONTROL_P_H
#define QUCS_QUCSCONTROL_P_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

class Component;
class Diagram;
class Graph;
class Module;
class Schematic;

namespace qucs_s::control {

QString tr(const char* text);
QJsonObject textResult(const QString& text, bool error = false);
/// \a value as JSON text: indented, or on one line (\a compact) - numbers
/// by the hundred read better so, and cost less.
/// \a value as JSON text: compact (fewer tokens for Claude), or
/// \a indented.
QJsonObject jsonResult(const QJsonValue& value, bool indented = false);
QJsonObject errorResult(const QString& text);
bool sameFile(const QString& a, const QString& b);
/// A path as given, or taken from the workspace folder.
QString absolute(const QString& path);

/// The name of the simulator in the settings as a trace's prefix gives it
/// ("ngspice", "xyce", "spopus"); empty for Qucsator.
QString simulatorPrefix();
/// The dataset a simulation of \a schematic (a .sch file) writes with
/// \a simulator (spicecompat::Simulator): name.dat.ngspice, .dat.xyce,
/// .dat.spopus, or \a dataSet (the document's "DataSet") for Qucsator.
QString datasetFile(const QString& schematic, const QString& dataSet, int simulator);

/// The diagrams of \a sch, numbered from 1 as the tools take them: each
/// one's type, place, axes and traces - with whether each trace has data
/// and, when not, why.
QJsonArray diagramsJson(Schematic* sch);
/// The diagram \a which (its number) of \a sch; when not given, the only
/// one there is. nullptr and why in \a error.
Diagram* diagramOf(Schematic* sch, const QJsonValue& which, QString* error);
/// The trace \a which (its number in the diagram, or its variable) of \a d.
Graph* traceOf(Diagram* d, const QJsonValue& which, QString* error);
/// Why trace \a g of \a sch shows nothing (no dataset; no such variable,
/// and those there are like it); empty when it shows data.
QString whyNoData(Schematic* sch, Graph* g);

/// A new component of \a type (its model: R, Vpulse, Eqn, ...) - those of
/// the library's hash, and the equation blocks that are only in its
/// categories; nullptr when there is none. Its module in \a module.
Component* newComponent(const QString& type, Module** module = nullptr);
/// The type (model) of the components a module of the library makes;
/// empty for a diagram or a painting.
QString typeOf(Module* module);

/// \a text (a trace's variable, an equation) with the net \a from called
/// \a to wherever it names the net's voltage: v(out), vdb(out),
/// v(out,in), out.v, out.Vt.
QString renameNetIn(const QString& text, const QString& from, const QString& to);

} // namespace qucs_s::control

#endif // QUCS_QUCSCONTROL_P_H
