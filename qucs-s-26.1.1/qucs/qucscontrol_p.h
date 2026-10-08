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
#include <QList>
#include <QCoreApplication>
#include <QString>

#include <list>

class Component;
class Diagram;
class Graph;
class Marker;
class Module;
class Painting;
class Schematic;
class QWidget;
class QRegularExpression;

namespace qucs_s::control {

QString tr(const char* text);
/// The properties \a c has any number of (.NGOPT's Knob, Target and
/// Constraint, NgSweep's Record and Vs, a Monte Carlo's Record and Spec,
/// an optimization's Var and Goal): each one's fields and an example, as
/// describe_component_type gives them; empty for any other type.
QJsonArray repeatedProperties(const Component* c);
/// What a property of \a c saved with its name, not a description, holds
/// (.NGOPT's Method, Polish, ...); empty when there is nothing to say.
QString propertyNote(const Component* c, const QString& name);
QJsonObject textResult(const QString& text, bool error = false);
/// \a value as JSON text: indented, or on one line (\a compact) - numbers
/// by the hundred read better so, and cost less.
/// \a value as JSON text: compact (fewer tokens for Claude), or
/// \a indented.
QJsonObject jsonResult(const QJsonValue& value, bool indented = false);
QJsonObject errorResult(const QString& text);
bool sameFile(const QString& a, const QString& b);
/// Whether \a file is a document of Qucs-S (a schematic, symbol, data
/// display or dataset - its first line <Qucs ...>, or its suffix says so),
/// or Verilog-A or VHDL code: what a netlist or a picture must not replace.
bool isQucsDocument(const QString& file);
/// A path as given, or taken from the workspace folder.
QString absolute(const QString& path);
/// Whether \a w is the user's alone - a control of the Claude Code panel's
/// outside its dock (the status bar's Claude chip, which shows and hides
/// it): marked so (the property "qucsUsersOnly"), itself or a widget it is
/// in. Claude's tools neither list nor use it.
bool usersOnly(const QWidget* w);
/// What a console printed for \a input, typed into it, from what it showed
/// since (\a shown): after the line's echo - the terminal's, and again after
/// the prompt -, nothing before it (a banner), its prompt at the end left
/// out. \a promptBack: whether that prompt is back (the line is done).
QString consoleReply(const QString& shown, const QString& input, const QRegularExpression& prompt, bool* promptBack);
/// Why \a name (a file's or a folder's, without its folder) is no name of
/// a file, in words: a character a file system refuses (<>:"/\|?* and
/// control characters - Windows refuses them all), a space or a dot at
/// its end, a device name of Windows (CON, NUL, COM1 ...), or no letter or
/// digit at all ("'", "{}"); empty when it is one.
QString badFileName(const QString& name);
/// The parts of type \a model (any case: gnd is GND) that have no name
/// (grounds), in order.
QList<Component*> unnamedOf(const Schematic* sch, const QString& model);
/// A part by what refOf() tells it by. Null, and why in \a error.
Component* componentOf(const Schematic* sch, const QString& ref, QString* error);
/// What a part is told by: its name; one without a name (a ground) by its
/// type when it is the only one of it, else by its number among them -
/// GND#2, get_schematic's 'ref'. Every tool that names a part names it so.
QString refOf(const Schematic* sch, const Component* c);
/// Each pin of \a c by number, with its name when it has one and the side
/// of the symbol it is on: "1 (left, upper), 2 (left, lower), 3 (right)" -
/// for a part whose model gives its pins no names (which is the output?).
QString pinSides(const Component* c);
/// The side of \a c's symbol its pin \a i is on: "left, upper", "right".
QString pinSide(const Component* c, int i);

/// The name of the simulator in the settings as a trace's prefix gives it
/// ("ngspice", "xyce", "spopus"); empty for Qucsator.
QString simulatorPrefix();
/// Why nothing can be simulated or netlisted: no simulator chosen, as
/// when Qucs-S found none installed at its start.
QString noSimulatorText();
/// The dataset a simulation of \a schematic (a .sch file) writes with
/// \a simulator (spicecompat::Simulator): name.dat.ngspice, .dat.xyce,
/// .dat.spopus, or \a dataSet (the document's "DataSet") for Qucsator.
QString datasetFile(const QString& schematic, const QString& dataSet, int simulator);

/// The diagrams of \a sch, numbered from 1 as the tools take them: each
/// one's type, place, axes and traces - with whether each trace has data
/// and, when not, why.
QJsonArray diagramsJson(Schematic* sch);
/// The named nets' values at the marker they follow (cursorvalues.h):
/// which marker, its x, each net's value - or why there are none.
QJsonObject cursorValuesJson(Schematic* sch);
/// The diagram \a which (its number) of \a sch; when not given, the only
/// one there is. nullptr and why in \a error.
Diagram* diagramOf(Schematic* sch, const QJsonValue& which, QString* error);
/// The trace \a which (its number in the diagram, or its variable) of \a d.
Graph* traceOf(Diagram* d, const QJsonValue& which, QString* error);
/// The markers of a diagram, numbered from 1 as the tools take them: those
/// of its first trace, then of its second...
QList<Marker*> markersOf(const Diagram* d);
/// A marker as get_schematic lists it: its trace, the sample it shows and
/// the value there, its text, where its label is, how it is drawn.
QJsonObject markerJson(const Diagram* d, Marker* m, int index);
/// The marker \a which (its number) of \a d; the only one when not given.
Marker* markerOf(const Diagram* d, const QJsonValue& which, QString* error);
/// Why trace \a g of \a sch shows nothing (no dataset; no such variable,
/// and those there are like it); empty when it shows data.
QString whyNoData(Schematic* sch, Graph* g);
/// The dataset file a trace's variable \a var reads (ngspice/tran.v(out):
/// name.dat.ngspice), and the variable in it in \a variable.
QString datasetOfTrace(Schematic* sch, const QString& var, QString* variable);
/// The dataset \a name in \a folder when it is no simulator's - imported
/// (import_data, the Import tab; the file it came from in \a source), or a
/// name.dat with no name.dat.<simulator> of the settings beside it: its
/// traces are name:variable, without a prefix. Its file, else empty.
QString plainDataset(const QString& folder, const QString& name, QString* source = nullptr);
/// An imported dataset in \a folder that has \a variable: its name (the
/// file it came from in \a source), else empty.
QString importedWith(const QString& folder, const QString& variable, QString* source = nullptr);
/// \a file as a person reads it from \a folder: relative when inside it.
QString shownFrom(const QString& folder, const QString& file);

/// The operating point a DC bias run (Simulation > Calculate DC bias) of
/// \a sch left in its Scratch folder \a scratch: each node's value and
/// branch current, and - with ngspice - each device's operating quantities
/// under its component, with what they make plain (re = 1/gm, beta, ro,
/// ...); empty when there is none.
QJsonObject operatingPointOfRun(Schematic* sch, const QString& scratch);

/// A painting as get_schematic lists it: its number (\a index, from 1),
/// its type (text, arrow, rectangle, text_box, ...) and its fields by name.
QJsonObject paintingJson(Painting* p, int index);
/// The paintings of a list, numbered from 1 as the tools take them; at
/// most \a most (all when negative).
QJsonArray paintingsJson(const std::list<Painting*>& paintings, int most = -1);
/// The painting \a which (its number) of \a paintings; nullptr and why.
Painting* paintingOf(const std::list<Painting*>& paintings, const QJsonValue& which, QString* error);
/// A symbol's port or its name text: moved, not made or deleted by hand.
bool isFixedPainting(const Painting* p);
/// The fields add_painting and edit_painting take, type by type.
QString paintingFieldsText();
/// The parameters of \a sch's symbol (a subcircuit's), as get_schematic
/// and set_subcircuit_parameters give them: [{name, default, description,
/// type, shown}].
QJsonArray subcircuitParametersJson(const Schematic* sch);

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

/// \a text with the component \a from called \a to wherever it names one
/// of its quantities: i(v1), @q1[ic], v1#branch, V1.It, D1.Id.
QString renameComponentIn(const QString& text, const QString& from, const QString& to);

/// What changed from one state of a schematic to another (as snapshot()
/// and its undo stack have them), in words: parts added, deleted, renamed,
/// moved, turned, their properties changed (R2: R 47k → 67k), wires and
/// labels, diagrams and their traces, paintings. At most \a most lines.
QStringList describeChanges(const QString& before, const QString& after, int most = 20);

// While Claude clicks - a menu action, a dialog's button -, the file
// dialogs that click opens are Qt's own: get_dialog reads them and
// set_dialog fills them in ("File name" or "Directory", then Open, Save
// or Choose). The system's (macOS's panel) is drawn by the system, out
// of reach, and waited for the user. Those the user opens stay the
// system's. (A modal dialog's click returns when it is answered: the
// setting holds until then.)
class QtFileDialogs
{
public:
    QtFileDialogs() : a_was(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs))
    {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    }
    ~QtFileDialogs() { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, a_was); }
    QtFileDialogs(const QtFileDialogs&) = delete;
    QtFileDialogs& operator=(const QtFileDialogs&) = delete;

private:
    bool a_was;
};


} // namespace qucs_s::control

#endif // QUCS_QUCSCONTROL_P_H
