/*
 * qucscontrol.cpp - the Qucs-S window as tools for Claude
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"

#include "components/component.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "extsimkernels/spicecompat.h"
#include "extsimkernels/simulationrun.h"
#include "graphicsexport.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "node.h"
#include "paintings/painting.h"
#include "qucs.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "textdoc.h"
#include "wire.h"
#include "wirelabel.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

// The tools, as MCP's tools/list gives them.
const char* const kTools = R"JSON([
{"name": "get_state",
 "description": "The state of the Qucs-S window: the documents open (path, title, kind, unsaved changes, which is in front), the simulator in the settings, the workspace folder, a simulation under way, a dialog waiting for an answer. Start here.",
 "inputSchema": {"type": "object", "properties": {}}},
{"name": "open_document",
 "description": "Opens a file in a tab of Qucs-S - a schematic (.sch), a symbol (.sym), a data display (.dpl), a netlist or any text file - or brings it to the front if it is open. A relative path is taken from the workspace folder.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The file"}}, "required": ["path"]}},
{"name": "new_document",
 "description": "Opens a new, untitled document in front: a schematic, or a text document.",
 "inputSchema": {"type": "object", "properties": {"kind": {"type": "string", "enum": ["schematic", "text"]}}}},
{"name": "show_document",
 "description": "Brings an open document to the front.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "Its file, or its tab's title"}}, "required": ["path"]}},
{"name": "save_document",
 "description": "Saves a document (the one in front unless path names another); with 'as', under that file name from now on. An untitled document needs 'as'.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "as": {"type": "string", "description": "A new file name"}}}},
{"name": "close_document",
 "description": "Closes a document's tab (the one in front unless path names another). A document with unsaved changes is closed only when 'unsaved' says what to do with them.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "unsaved": {"type": "string", "enum": ["save", "discard"]}}}},
{"name": "get_schematic",
 "description": "Reads a schematic as it is in Qucs-S now, unsaved changes included. 'summary' (the default) lists its components - name, type, place, rotation, mirroring, whether active, properties, and each pin's place, whether it is connected and the net label on it - its wires, net labels, diagrams and paintings. 'text' is the text its .sch file would have. Coordinates are the schematic's units; the grid is usually 10.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "format": {"type": "string", "enum": ["summary", "text"]}}}},
{"name": "set_schematic",
 "description": "Replaces the elements of a schematic with those of 'text': a .sch file's text, or any of its <Components>, <Wires>, <Diagrams> and <Paintings> sections (sections left out stay as they are; <Properties> and <Symbol> are not taken). One step to undo; when the text does not read, the schematic stays as it was and the error is told.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "text": {"type": "string"}}, "required": ["text"]}},
{"name": "add_component",
 "description": "Places a component from the library at x, y (snapped to the grid): 'type' is its model - R, C, L, GND, Vdc, Vac, Idc, Iac, Diode, _BJT, _MOSFET, OpAmp, Sub, .DC, .AC, .TR, .SP, ... (list_component_types lists them all). Properties by name (as get_schematic shows them, e.g. {\"R\": \"4.7k\"}); rotation in quarter turns as Rotate makes them; mirror about the x axis. Returns the component with its pins' places.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "type": {"type": "string"}, "x": {"type": "integer"}, "y": {"type": "integer"},
   "name": {"type": "string", "description": "Its name; the next free one (R1, R2, ...) when not given"},
   "properties": {"type": "object", "additionalProperties": {"type": "string"}},
   "rotation": {"type": "integer", "minimum": 0, "maximum": 3}, "mirror": {"type": "boolean"}},
   "required": ["type", "x", "y"]}},
{"name": "edit_component",
 "description": "Changes a component: its properties (by name), its name, its place (x, y: where its centre goes), its rotation (0-3 quarter turns) and mirroring, whether it is active (an inactive one is left out of the simulation). What is not given stays.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "name": {"type": "string"}, "rename": {"type": "string"},
   "properties": {"type": "object", "additionalProperties": {"type": "string"}},
   "x": {"type": "integer"}, "y": {"type": "integer"}, "rotation": {"type": "integer", "minimum": 0, "maximum": 3},
   "mirror": {"type": "boolean"}, "active": {"type": "boolean"}}, "required": ["name"]}},
{"name": "delete",
 "description": "Deletes components (by name), net labels (by the net's name) and wires (by their two ends, [x1, y1, x2, y2]) from a schematic.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "names": {"type": "array", "items": {"type": "string"}},
   "wires": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}, "minItems": 4, "maxItems": 4}}}}},
{"name": "connect",
 "description": "Draws a wire between two pins or places, routed with right angles as the wire tool does. A pin is \"R1.1\" (the component's name and the pin's number, from 1, or the pin's name); a place is [x, y].",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"},
   "from": {"description": "\"R1.2\" or [x, y]; a ground is GND.1 when there is one"}, "to": {"description": "\"C1.1\" or [x, y]"}}, "required": ["from", "to"]}},
{"name": "add_wire",
 "description": "Draws a wire through places, [[x, y], [x, y], ...], a segment from each to the next (a step that is not straight gets a right angle).",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "points": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2}, "minItems": 2}},
   "required": ["points"]}},
{"name": "set_label",
 "description": "Names the net at a pin or at a place on a wire (a net label: nets with the same name are connected). An empty name takes the label away.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "at": {"description": "\"R1.1\" or [x, y]"}, "name": {"type": "string"}}, "required": ["at", "name"]}},
{"name": "select",
 "description": "Selects components by name in a schematic (the rest are deselected); no names deselects everything.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "names": {"type": "array", "items": {"type": "string"}}}}},
{"name": "zoom",
 "description": "Zooms a schematic: 'all' shows all of it, 'selection' the selection, 'in' and 'out' a step, 'none' the scale of 1.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "to": {"type": "string", "enum": ["all", "selection", "in", "out", "none"]}}, "required": ["to"]}},
{"name": "undo",
 "description": "Undoes the last change of a document (the one in front unless path names another), as Edit > Undo.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}}},
{"name": "redo",
 "description": "Redoes the last change undone, as Edit > Redo.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}}},
{"name": "screenshot",
 "description": "A picture of a schematic (PNG): all of it (the default), or 'visible' - what the window shows of it now, at its zoom. For a text document, what its window shows.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "area": {"type": "string", "enum": ["all", "visible"]}}}},
{"name": "list_component_types",
 "description": "The components of the library: the type to give add_component, what it is, its category. 'search' keeps those whose type, name or category has it.",
 "inputSchema": {"type": "object", "properties": {"search": {"type": "string"}}}},
{"name": "list_actions",
 "description": "The actions of Qucs-S's menus: the menu path to give trigger_action (\"Simulation > Simulate\"), whether it can be used now, whether it is checked, its shortcut. 'search' keeps those whose path has it.",
 "inputSchema": {"type": "object", "properties": {"search": {"type": "string"}}}},
{"name": "trigger_action",
 "description": "Uses a menu action as a click on it would: 'action' is its menu path (\"Edit > Rotate\") or its object name. When it opens a dialog, the dialog stays open: get_dialog reads it, set_dialog fills it in and closes it. Actions that open the system's file or print dialogs are refused: use open_document and save_document.",
 "inputSchema": {"type": "object", "properties": {"action": {"type": "string"}}, "required": ["action"]}},
{"name": "get_dialog",
 "description": "The dialog of Qucs-S that waits for an answer (or another window of it that is open): its title, its texts and its controls - fields, lists, check boxes, tabs, tables, buttons - each with its label, value and an id for set_dialog.",
 "inputSchema": {"type": "object", "properties": {}}},
{"name": "set_dialog",
 "description": "Fills in the open dialog and presses a button. 'set' changes controls, each by its id or label from get_dialog: a field takes text, a list an item, a check box true or false, a spin box a number, tabs a tab's title, a table [row, column, text]. 'press' names the button pressed after (OK, Cancel, Apply, ... or its id).",
 "inputSchema": {"type": "object", "properties": {
   "set": {"type": "array", "items": {"type": "object", "properties": {"control": {"type": "string"}, "value": {}}, "required": ["control", "value"]}},
   "press": {"type": "string"}}}},
{"name": "simulate",
 "description": "Simulates a schematic (the one in front unless path names another; it must have been saved once) with the simulator in the settings, as Simulation > Simulate, and waits for the end: whether it succeeded, the simulator's errors and warnings, the dataset written and the last lines of its output. 'timeout' in seconds, 120 unless given.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "timeout": {"type": "integer"}}}}
])JSON";

const struct {
    const char* tool;
    const char* action;
} kActions[] = {
    {"open_document", QT_TRANSLATE_NOOP("QucsControl", "open a document in Qucs-S")},
    {"new_document", QT_TRANSLATE_NOOP("QucsControl", "open a new document in Qucs-S")},
    {"save_document", QT_TRANSLATE_NOOP("QucsControl", "save a document in Qucs-S")},
    {"close_document", QT_TRANSLATE_NOOP("QucsControl", "close a document in Qucs-S")},
    {"set_schematic", QT_TRANSLATE_NOOP("QucsControl", "change the schematic in Qucs-S")},
    {"add_component", QT_TRANSLATE_NOOP("QucsControl", "add a component in Qucs-S")},
    {"edit_component", QT_TRANSLATE_NOOP("QucsControl", "change a component in Qucs-S")},
    {"delete", QT_TRANSLATE_NOOP("QucsControl", "delete from the schematic in Qucs-S")},
    {"connect", QT_TRANSLATE_NOOP("QucsControl", "draw a wire in Qucs-S")},
    {"add_wire", QT_TRANSLATE_NOOP("QucsControl", "draw a wire in Qucs-S")},
    {"set_label", QT_TRANSLATE_NOOP("QucsControl", "label a net in Qucs-S")},
    {"undo", QT_TRANSLATE_NOOP("QucsControl", "undo in Qucs-S")},
    {"redo", QT_TRANSLATE_NOOP("QucsControl", "redo in Qucs-S")},
    {"trigger_action", QT_TRANSLATE_NOOP("QucsControl", "use a menu action of Qucs-S")},
    {"set_dialog", QT_TRANSLATE_NOOP("QucsControl", "answer a dialog of Qucs-S")},
    {"simulate", QT_TRANSLATE_NOOP("QucsControl", "run a simulation in Qucs-S")},
};

// Tools that only look (or move the view): used without asking.
const char* const kReadOnly[] = {"get_state", "get_schematic", "screenshot", "list_component_types", "list_actions",
                                 "get_dialog", "show_document", "select", "zoom"};

QString tr(const char* text)
{
    return QCoreApplication::translate("QucsControl", text);
}

QJsonObject textResult(const QString& text, bool error = false)
{
    return {{QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                               {QStringLiteral("text"), text}}}},
            {QStringLiteral("isError"), error}};
}

QJsonObject jsonResult(const QJsonValue& value)
{
    const QByteArray json = value.isObject() ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented)
                                             : QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented);
    return textResult(QString::fromUtf8(json));
}

QJsonObject errorResult(const QString& text)
{
    return textResult(text, true);
}

// "File > Save &As..." and "file > save as": the same.
QString normalized(QString path)
{
    path.remove(QLatin1Char('&'));
    path.replace(QChar(0x2026), QStringLiteral("..."));
    path.remove(QStringLiteral("..."));
    path = path.simplified().toLower();
    path.replace(QRegularExpression(QStringLiteral("\\s*>\\s*")), QStringLiteral(">"));
    return path;
}

QString cleanText(QString text)
{
    text.remove(QLatin1Char('&'));
    return text.trimmed();
}

bool sameFile(const QString& a, const QString& b)
{
    const QFileInfo fa(a), fb(b);
    const QString ca = fa.exists() ? fa.canonicalFilePath() : QDir::cleanPath(fa.absoluteFilePath());
    const QString cb = fb.exists() ? fb.canonicalFilePath() : QDir::cleanPath(fb.absoluteFilePath());
    return ca == cb;
}

QString absolute(const QString& path)
{
    if (QFileInfo(path).isAbsolute()) return QDir::cleanPath(path);
    return QDir::cleanPath(QucsSettings.qucsWorkspaceDir.absoluteFilePath(path));
}

QString propertyValue(const QJsonValue& v)
{
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 12);
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

QJsonObject componentJson(Component* c)
{
    QJsonArray props;
    for (Property* p : c->Props)
        props.append(QJsonObject{{QStringLiteral("name"), p->Name}, {QStringLiteral("value"), p->Value},
                                 {QStringLiteral("shown"), p->display}});
    QJsonArray pins;
    for (int i = 0; i < c->Ports.size(); ++i) {
        const Port* pp = c->Ports.at(i);
        QJsonObject pin{{QStringLiteral("pin"), i + 1}, {QStringLiteral("x"), c->cx + pp->x}, {QStringLiteral("y"), c->cy + pp->y}};
        if (!pp->Name.isEmpty()) pin.insert(QStringLiteral("name"), pp->Name);
        if (pp->Connection != nullptr) {
            pin.insert(QStringLiteral("connected"), pp->Connection->conn_count() > 1);
            if (pp->Connection->hasLabel()) pin.insert(QStringLiteral("net"), pp->Connection->label()->Name);
        }
        pins.append(pin);
    }
    return {{QStringLiteral("name"), c->Name},
            {QStringLiteral("type"), c->Model},
            {QStringLiteral("description"), c->Description},
            {QStringLiteral("x"), c->cx},
            {QStringLiteral("y"), c->cy},
            {QStringLiteral("rotation"), c->rotated},
            {QStringLiteral("mirrored"), c->mirroredX},
            {QStringLiteral("active"), c->isActive == COMP_IS_ACTIVE},
            {QStringLiteral("properties"), props},
            {QStringLiteral("pins"), pins}};
}

QString propertyNames(Component* c)
{
    QStringList names;
    for (Property* p : c->Props) names << p->Name;
    return names.join(QStringLiteral(", "));
}

// Sets properties by name; false and which are unknown in \a error.
bool setProperties(Component* c, const QJsonObject& properties, QString* error)
{
    QStringList unknown;
    for (auto it = properties.begin(); it != properties.end(); ++it)
        if (c->getProperty(it.key()) == nullptr) unknown << it.key();
    if (!unknown.isEmpty()) {
        *error = tr("%1 has no property %2; its properties are: %3.")
                     .arg(c->Name.isEmpty() ? c->Model : c->Name, unknown.join(QStringLiteral(", ")), propertyNames(c));
        return false;
    }
    for (auto it = properties.begin(); it != properties.end(); ++it) c->getProperty(it.key())->Value = propertyValue(it.value());
    return true;
}

QString kindOf(QucsDoc* doc)
{
    if (qobject_cast<TextDoc*>(QucsApp::documentWidget(doc)) != nullptr) return QStringLiteral("text");
    const QString suffix = QFileInfo(doc->getDocName()).suffix().toLower();
    if (suffix == QLatin1String("dpl")) return QStringLiteral("data display");
    if (suffix == QLatin1String("sym")) return QStringLiteral("symbol");
    return QStringLiteral("schematic");
}

} // namespace

// ----------------------------------------------------------------------
QucsControl::QucsControl(QucsApp* app) : QObject(app), a_app(app)
{
    a_tools = QJsonDocument::fromJson(QByteArray(kTools)).array();
    for (const auto& a : kActions) a_actions.insert(QString::fromLatin1(a.tool), tr(a.action));
    for (const char* t : kReadOnly) a_readOnly << QString::fromLatin1(t);
}

QJsonArray QucsControl::tools() const
{
    return a_tools;
}

QStringList QucsControl::readOnlyTools() const
{
    return a_readOnly;
}

QString QucsControl::actionOf(const QString& tool) const
{
    return a_actions.value(tool, tr("use Qucs-S (%1)").arg(tool));
}

QString QucsControl::subjectOf(const QString& tool, const QJsonObject& a) const
{
    const auto s = [&a](const char* key) { return a.value(QLatin1String(key)).toString(); };
    const auto point = [](const QJsonValue& v) {
        if (v.isString()) return v.toString();
        const QJsonArray xy = v.toArray();
        return xy.size() >= 2 ? QStringLiteral("%1, %2").arg(xy.at(0).toInt()).arg(xy.at(1).toInt()) : QString();
    };
    QString subject;
    if (tool == QLatin1String("open_document") || tool == QLatin1String("show_document")) subject = s("path");
    else if (tool == QLatin1String("save_document")) subject = s("as").isEmpty() ? s("path") : tr("as %1").arg(s("as"));
    else if (tool == QLatin1String("close_document")) subject = s("path");
    else if (tool == QLatin1String("add_component"))
        subject = QStringLiteral("%1%2 at %3, %4").arg(s("type"), s("name").isEmpty() ? QString() : QLatin1Char(' ') + s("name"))
                      .arg(a.value(QLatin1String("x")).toInt()).arg(a.value(QLatin1String("y")).toInt());
    else if (tool == QLatin1String("edit_component")) {
        QStringList changes;
        const QJsonObject props = a.value(QLatin1String("properties")).toObject();
        for (auto it = props.begin(); it != props.end(); ++it) changes << it.key() + QLatin1Char('=') + propertyValue(it.value());
        if (!s("rename").isEmpty()) changes << tr("named %1").arg(s("rename"));
        if (a.contains(QLatin1String("x")) || a.contains(QLatin1String("y"))) changes << tr("moved");
        if (a.contains(QLatin1String("rotation"))) changes << tr("rotated");
        if (a.contains(QLatin1String("mirror"))) changes << tr("mirrored");
        if (a.contains(QLatin1String("active"))) changes << (a.value(QLatin1String("active")).toBool() ? tr("active") : tr("inactive"));
        subject = s("name") + QStringLiteral(": ") + changes.join(QStringLiteral(", "));
    } else if (tool == QLatin1String("delete")) {
        QStringList what;
        for (const QJsonValue& v : a.value(QLatin1String("names")).toArray()) what << v.toString();
        const int wires = int(a.value(QLatin1String("wires")).toArray().size());
        if (wires > 0) what << (wires == 1 ? tr("a wire") : tr("%1 wires").arg(wires));
        subject = what.join(QStringLiteral(", "));
    } else if (tool == QLatin1String("connect"))
        subject = point(a.value(QLatin1String("from"))) + QStringLiteral(" → ") + point(a.value(QLatin1String("to")));
    else if (tool == QLatin1String("add_wire")) {
        QStringList points;
        for (const QJsonValue& v : a.value(QLatin1String("points")).toArray()) points << QLatin1Char('(') + point(v) + QLatin1Char(')');
        subject = points.join(QStringLiteral(" → "));
    } else if (tool == QLatin1String("set_label"))
        subject = point(a.value(QLatin1String("at"))) + QStringLiteral(": ") + s("name");
    else if (tool == QLatin1String("set_schematic"))
        subject = tr("%1 lines").arg(s("text").count(QLatin1Char('\n')) + 1);
    else if (tool == QLatin1String("trigger_action")) subject = s("action");
    else if (tool == QLatin1String("set_dialog")) {
        QStringList parts;
        for (const QJsonValue& v : a.value(QLatin1String("set")).toArray())
            parts << v.toObject().value(QLatin1String("control")).toString() + QLatin1Char('=') + propertyValue(v.toObject().value(QLatin1String("value")));
        if (!s("press").isEmpty()) parts << tr("press %1").arg(s("press"));
        subject = parts.join(QStringLiteral(", "));
    } else if (tool == QLatin1String("zoom")) subject = s("to");
    else if (tool == QLatin1String("screenshot")) subject = s("area").isEmpty() ? s("path") : s("area");
    else if (tool == QLatin1String("get_schematic")) subject = s("path");
    else if (tool == QLatin1String("select")) {
        QStringList names;
        for (const QJsonValue& v : a.value(QLatin1String("names")).toArray()) names << v.toString();
        subject = names.join(QStringLiteral(", "));
    } else if (tool == QLatin1String("list_component_types") || tool == QLatin1String("list_actions")) subject = s("search");
    else if (tool == QLatin1String("simulate")) subject = s("path");
    if (subject.size() > 160) subject = subject.left(159) + QChar(0x2026);
    return subject;
}

QString QucsControl::instructions() const
{
    return QStringLiteral(
        "These tools drive the Qucs-S window the user is looking at. get_state says what is open. Documents: "
        "open_document, show_document, new_document, save_document, close_document. A schematic: get_schematic "
        "(its components with their pins' places, or its .sch text), then change it with add_component, "
        "edit_component, connect (pin to pin, e.g. \"R1.2\" to \"C1.1\"), add_wire, set_label, delete, or "
        "set_schematic (new .sch sections at once); select, zoom, undo, redo; screenshot to see it. "
        "list_component_types gives add_component's types. Menus: list_actions, trigger_action; a dialog it opens "
        "is read with get_dialog and answered with set_dialog. simulate runs the simulator and reports. Changes "
        "appear in the window at once, each one step to undo: prefer these tools to editing the file of a schematic "
        "that is open. Coordinates are the schematic's (grid 10 as a rule; place pins on it). The system's file and "
        "print dialogs cannot be filled: use open_document and save_document instead.");
}

// ----------------------------------------------------------------------
void QucsControl::callTool(const QString& tool, const QJsonObject& arguments, std::function<void(const QJsonObject&)> done)
{
    bool async = false;
    // Errors are told to Claude, not shown in message boxes.
    misc::ErrorCapture capture;
    QJsonObject result = call(tool, arguments, done, async);
    if (async) return;
    const QStringList errors = capture.errors();
    if (!errors.isEmpty()) {
        QJsonArray content = result.value(QStringLiteral("content")).toArray();
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), tr("Qucs-S reported: %1").arg(errors.join(QStringLiteral("\n")))}});
        result.insert(QStringLiteral("content"), content);
    }
    done(result);
}

QJsonObject QucsControl::callNow(const QString& tool, const QJsonObject& arguments, int timeoutMs)
{
    QJsonObject result;
    bool finished = false;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    callTool(tool, arguments, [&](const QJsonObject& r) {
        result = r;
        finished = true;
        loop.quit();
    });
    if (!finished) {
        timer.start(timeoutMs);
        loop.exec();
    }
    return finished ? result : errorResult(QStringLiteral("no answer in time"));
}

QString QucsControl::textOf(const QJsonObject& result)
{
    QStringList parts;
    for (const QJsonValue& v : result.value(QStringLiteral("content")).toArray())
        if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text"))
            parts << v.toObject().value(QStringLiteral("text")).toString();
    return parts.join(QLatin1Char('\n'));
}

QJsonObject QucsControl::call(const QString& tool, const QJsonObject& args, const Done& done, bool& async)
{
    if (tool == QLatin1String("get_state")) return getState();
    if (tool == QLatin1String("open_document")) return openDocument(args);
    if (tool == QLatin1String("new_document")) return newDocument(args);
    if (tool == QLatin1String("show_document")) return showDocument(args);
    if (tool == QLatin1String("save_document")) return saveDocument(args);
    if (tool == QLatin1String("close_document")) return closeDocument(args);
    if (tool == QLatin1String("get_schematic")) return getSchematic(args);
    if (tool == QLatin1String("set_schematic")) return setSchematic(args);
    if (tool == QLatin1String("add_component")) return addComponent(args);
    if (tool == QLatin1String("edit_component")) return editComponent(args);
    if (tool == QLatin1String("delete")) return remove(args);
    if (tool == QLatin1String("connect")) return connectPins(args);
    if (tool == QLatin1String("add_wire")) return addWire(args);
    if (tool == QLatin1String("set_label")) return setLabel(args);
    if (tool == QLatin1String("select")) return select(args);
    if (tool == QLatin1String("zoom")) return zoom(args);
    if (tool == QLatin1String("undo")) return undoRedo(args, false);
    if (tool == QLatin1String("redo")) return undoRedo(args, true);
    if (tool == QLatin1String("screenshot")) return screenshot(args);
    if (tool == QLatin1String("list_component_types")) return listComponentTypes(args);
    if (tool == QLatin1String("list_actions")) return listActions(args);
    if (tool == QLatin1String("get_dialog")) return getDialog();
    async = true;
    if (tool == QLatin1String("trigger_action")) triggerAction(args, done);
    else if (tool == QLatin1String("set_dialog")) setDialog(args, done);
    else if (tool == QLatin1String("simulate")) simulate(args, done);
    else {
        async = false;
        return errorResult(tr("There is no tool %1.").arg(tool));
    }
    return {};
}

// ----------------------------------------------------------------------
// Documents

QString QucsControl::titleOf(QucsDoc* doc) const
{
    QWidget* w = QucsApp::documentWidget(doc);
    const QTabWidget* pane = a_app->paneOf(w);
    return pane != nullptr ? cleanText(pane->tabText(pane->indexOf(w))) : QString();
}

QucsDoc* QucsControl::document(const QJsonObject& args, QString* error) const
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) {
        QucsDoc* doc = a_app->DocumentTab->count() > 0 ? a_app->getDoc() : nullptr;
        if (doc == nullptr) *error = tr("No document is open.");
        return doc;
    }
    const QString wanted = absolute(path);
    for (QucsDoc* doc : a_app->allDocuments())
        if ((!doc->getDocName().isEmpty() && sameFile(doc->getDocName(), wanted)) || titleOf(doc) == path) return doc;
    *error = tr("%1 is not open (open_document opens it).").arg(path);
    return nullptr;
}

Schematic* QucsControl::schematic(const QJsonObject& args, QString* error, bool forChange) const
{
    QucsDoc* doc = document(args, error);
    if (doc == nullptr) return nullptr;
    auto* sch = dynamic_cast<Schematic*>(doc);
    if (sch == nullptr) {
        *error = tr("%1 is not a schematic.").arg(titleOf(doc));
        return nullptr;
    }
    if (forChange && sch->getSymbolMode()) {
        *error = tr("%1 shows its symbol: these tools change the schematic (Edit Circuit Symbol switches back).").arg(titleOf(doc));
        return nullptr;
    }
    return sch;
}

void QucsControl::prepare(Schematic* sch)
{
    a_app->showDocument(sch);
    QMetaObject::invokeMethod(a_app, "slotHideEdit", Qt::DirectConnection);
}

void QucsControl::finish(Schematic* sch, const QList<QPoint>& where)
{
    sch->updateAllBoundingRect();
    sch->setChanged(true, true);
    // What changed in sight: the user watches it happen.
    const QRect inside = sch->viewportRect().adjusted(40, 40, -40, -40);
    for (const QPoint& p : where)
        if (!inside.contains(sch->modelToViewport(p))) {
            sch->centerOn(p);
            break;
        }
    sch->viewport()->update();
}

QJsonObject QucsControl::getState()
{
    QJsonArray docs;
    QucsDoc* front = a_app->DocumentTab->count() > 0 ? a_app->getDoc() : nullptr;
    for (QucsDoc* doc : a_app->allDocuments()) {
        QJsonObject d{{QStringLiteral("title"), titleOf(doc)},
                      {QStringLiteral("path"), doc->getDocName()},
                      {QStringLiteral("kind"), kindOf(doc)},
                      {QStringLiteral("unsaved changes"), doc->getDocChanged()},
                      {QStringLiteral("in front"), doc == front}};
        if (auto* sch = dynamic_cast<Schematic*>(doc)) {
            d.insert(QStringLiteral("components"), int(sch->a_DocComps.size()));
            d.insert(QStringLiteral("wires"), int(sch->a_DocWires.size()));
            d.insert(QStringLiteral("showing its symbol"), sch->getSymbolMode());
            QJsonArray selected;
            for (Component* c : sch->a_DocComps)
                if (c->isSelected) selected.append(c->Name);
            if (!selected.isEmpty()) d.insert(QStringLiteral("selected"), selected);
        }
        docs.append(d);
    }
    QString simulator = QStringLiteral("none");
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simNgspice: simulator = QStringLiteral("ngspice"); break;
    case spicecompat::simXyce: simulator = QStringLiteral("Xyce"); break;
    case spicecompat::simSpiceOpus: simulator = QStringLiteral("SpiceOpus"); break;
    case spicecompat::simQucsator: simulator = QStringLiteral("Qucsator"); break;
    default: break;
    }
    QJsonObject state{{QStringLiteral("documents"), docs},
                      {QStringLiteral("workspace"), QucsSettings.qucsWorkspaceDir.absolutePath()},
                      {QStringLiteral("simulator"), simulator},
                      {QStringLiteral("simulation running"),
                       a_app->simulationConsole() != nullptr && a_app->simulationConsole()->isRunning()}};
    if (QWidget* dialog = openDialog())
        state.insert(QStringLiteral("dialog open"), dialog->windowTitle().isEmpty() ? dialog->metaObject()->className() : dialog->windowTitle());
    return jsonResult(state);
}

QJsonObject QucsControl::openDocument(const QJsonObject& args)
{
    const QString path = absolute(args.value(QLatin1String("path")).toString().trimmed());
    if (args.value(QLatin1String("path")).toString().trimmed().isEmpty()) return errorResult(tr("Which file? ('path')"));
    if (!QFileInfo(path).isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(path)));
    if (!a_app->gotoPage(path, false, false)) return errorResult(tr("%1 could not be opened.").arg(QDir::toNativeSeparators(path)));
    QucsDoc* doc = a_app->getDoc();
    return textResult(tr("%1 is open, in front (%2).").arg(QDir::toNativeSeparators(path), doc != nullptr ? kindOf(doc) : QString()));
}

QJsonObject QucsControl::newDocument(const QJsonObject& args)
{
    if (args.value(QLatin1String("kind")).toString() == QLatin1String("text")) a_app->slotTextNew();
    else a_app->slotFileNew();
    QucsDoc* doc = a_app->getDoc();
    return textResult(tr("A new document is in front: %1.").arg(doc != nullptr ? titleOf(doc) : QString()));
}

QJsonObject QucsControl::showDocument(const QJsonObject& args)
{
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    a_app->showDocument(QucsApp::documentWidget(doc));
    return textResult(tr("%1 is in front.").arg(titleOf(doc)));
}

QJsonObject QucsControl::saveDocument(const QJsonObject& args)
{
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString as = args.value(QLatin1String("as")).toString().trimmed();
    if (!as.isEmpty()) {
        QString target = absolute(as);
        if (QFileInfo(target).suffix().isEmpty() && kindOf(doc) == QLatin1String("schematic")) target += QStringLiteral(".sch");
        for (QucsDoc* other : a_app->allDocuments())
            if (other != doc && !other->getDocName().isEmpty() && sameFile(other->getDocName(), target))
                return errorResult(tr("%1 is open in another tab.").arg(QDir::toNativeSeparators(target)));
        if (!QFileInfo(QFileInfo(target).absolutePath()).isDir())
            return errorResult(tr("There is no folder %1.").arg(QDir::toNativeSeparators(QFileInfo(target).absolutePath())));
        if (!a_app->saveDocumentAs(doc, target)) return errorResult(tr("%1 could not be saved.").arg(QDir::toNativeSeparators(target)));
        return textResult(tr("Saved as %1.").arg(QDir::toNativeSeparators(target)));
    }
    if (doc->getDocName().isEmpty()) return errorResult(tr("%1 has no file yet: give 'as'.").arg(titleOf(doc)));
    if (!a_app->saveFile(doc)) return errorResult(tr("%1 could not be saved.").arg(QDir::toNativeSeparators(doc->getDocName())));
    return textResult(tr("Saved %1.").arg(QDir::toNativeSeparators(doc->getDocName())));
}

QJsonObject QucsControl::closeDocument(const QJsonObject& args)
{
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString title = titleOf(doc);
    const QString unsaved = args.value(QLatin1String("unsaved")).toString();
    if (doc->getDocChanged()) {
        if (unsaved == QLatin1String("save")) {
            if (doc->getDocName().isEmpty()) return errorResult(tr("%1 has no file: save_document with 'as' first.").arg(title));
            if (!a_app->saveFile(doc)) return errorResult(tr("%1 could not be saved; it stays open.").arg(title));
        } else if (unsaved == QLatin1String("discard")) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            else if (auto* text = qobject_cast<TextDoc*>(QucsApp::documentWidget(doc))) text->document()->setModified(false);
            doc->setDocChanged(false);
        } else {
            return errorResult(tr("%1 has unsaved changes: say whether to save or discard them ('unsaved').").arg(title));
        }
    }
    QWidget* w = QucsApp::documentWidget(doc);
    a_app->showDocument(w);
    a_app->slotFileClose(a_app->DocumentTab->indexOf(w));
    return textResult(tr("%1 is closed.").arg(title));
}

// ----------------------------------------------------------------------
// A schematic

QJsonObject QucsControl::getSchematic(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    if (args.value(QLatin1String("format")).toString() == QLatin1String("text")) return textResult(sch->documentText());

    QJsonArray components, wires, labels, diagrams, paintings;
    for (Component* c : sch->a_DocComps) components.append(componentJson(c));
    for (Wire* w : sch->a_DocWires) {
        QJsonObject wire{{QStringLiteral("x1"), w->x1}, {QStringLiteral("y1"), w->y1}, {QStringLiteral("x2"), w->x2}, {QStringLiteral("y2"), w->y2}};
        if (w->hasLabel()) {
            wire.insert(QStringLiteral("net"), w->label()->Name);
            labels.append(QJsonObject{{QStringLiteral("net"), w->label()->Name},
                                      {QStringLiteral("x"), w->label()->root().x()}, {QStringLiteral("y"), w->label()->root().y()}});
        }
        wires.append(wire);
    }
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel())
            labels.append(QJsonObject{{QStringLiteral("net"), n->label()->Name}, {QStringLiteral("x"), n->cx}, {QStringLiteral("y"), n->cy}});
    for (Diagram* d : sch->a_DocDiags) {
        QJsonArray graphs;
        for (Graph* g : d->Graphs) graphs.append(g->Var);
        diagrams.append(QJsonObject{{QStringLiteral("type"), d->Name}, {QStringLiteral("x"), d->cx}, {QStringLiteral("y"), d->cy},
                                    {QStringLiteral("width"), d->x2}, {QStringLiteral("height"), d->y2}, {QStringLiteral("graphs"), graphs}});
    }
    for (Painting* p : sch->a_DocPaints) paintings.append(p->Name.trimmed());
    return jsonResult(QJsonObject{{QStringLiteral("document"), titleOf(sch)},
                                  {QStringLiteral("path"), sch->getDocName()},
                                  {QStringLiteral("grid"), QJsonArray{sch->getGridX(), sch->getGridY()}},
                                  {QStringLiteral("components"), components},
                                  {QStringLiteral("wires"), wires},
                                  {QStringLiteral("labels"), labels},
                                  {QStringLiteral("diagrams"), diagrams},
                                  {QStringLiteral("paintings"), paintings}});
}

QJsonObject QucsControl::setSchematic(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    prepare(sch);
    if (!sch->replaceContent(args.value(QLatin1String("text")).toString(), &error))
        return errorResult(tr("Not changed: %1").arg(error));
    return textResult(tr("%1 now has %2 components and %3 wires (one step to undo).")
                          .arg(titleOf(sch)).arg(sch->a_DocComps.size()).arg(sch->a_DocWires.size()));
}

QJsonObject QucsControl::addComponent(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString type = args.value(QLatin1String("type")).toString().trimmed();
    Component* c = Module::getComponent(type);
    if (c == nullptr) return errorResult(tr("There is no component type %1 (list_component_types lists them).").arg(type));
    c->setSchematic(sch);
    if (!setProperties(c, args.value(QLatin1String("properties")).toObject(), &error)) {
        delete c;
        return errorResult(error);
    }
    const QString wanted = args.value(QLatin1String("name")).toString().trimmed();
    if (!wanted.isEmpty() && sch->getComponentByName(wanted) != nullptr) {
        delete c;
        return errorResult(tr("There is a component named %1 already.").arg(wanted));
    }
    prepare(sch);
    c->recreate();   // the symbol, with the properties
    for (int i = 0; i < ((args.value(QLatin1String("rotation")).toInt() % 4) + 4) % 4; ++i) c->rotate();
    if (args.value(QLatin1String("mirror")).toBool()) c->mirrorX();
    int x = args.value(QLatin1String("x")).toInt(), y = args.value(QLatin1String("y")).toInt();
    const QPoint at = Schematic::withinModelLimit(QPoint(x, y));
    x = at.x();
    y = at.y();
    sch->setOnGrid(x, y);
    c->moveCenter(x - c->cx, y - c->cy);
    // As a click with the component does.
    int x1, y1, x2, y2;
    c->textSize(x1, y1);
    sch->insertComponent(c);
    c->textSize(x2, y2);
    if (c->tx < c->x1) c->tx -= x2 - x1;
    if (!wanted.isEmpty()) c->Name = wanted;
    sch->enlargeView(c);
    finish(sch, {QPoint(c->cx, c->cy)});
    return jsonResult(componentJson(c));
}

QJsonObject QucsControl::editComponent(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    Component* c = sch->getComponentByName(name);
    if (c == nullptr) return errorResult(tr("There is no component %1 in %2.").arg(name, titleOf(sch)));
    const QString rename = args.value(QLatin1String("rename")).toString().trimmed();
    if (!rename.isEmpty() && rename != name && sch->getComponentByName(rename) != nullptr)
        return errorResult(tr("There is a component named %1 already.").arg(rename));
    // Check the properties before anything changes.
    const QJsonObject props = args.value(QLatin1String("properties")).toObject();
    for (auto it = props.begin(); it != props.end(); ++it)
        if (c->getProperty(it.key()) == nullptr)
            return errorResult(tr("%1 has no property %2; its properties are: %3.").arg(name, it.key(), propertyNames(c)));
    prepare(sch);
    if (!props.isEmpty()) {
        setProperties(c, props, &error);
        sch->recreateComponent(c);
    }
    const bool move = args.contains(QLatin1String("x")) || args.contains(QLatin1String("y"));
    const bool turn = args.contains(QLatin1String("rotation")) || args.contains(QLatin1String("mirror"));
    if (move || turn) {
        // Off its nodes, changed, and back on (as a recreate does).
        sch->detachComp(c);
        if (args.contains(QLatin1String("mirror")) && args.value(QLatin1String("mirror")).toBool() != c->mirroredX) c->mirrorX();
        if (args.contains(QLatin1String("rotation"))) {
            const int want = ((args.value(QLatin1String("rotation")).toInt() % 4) + 4) % 4;
            for (int i = 0; i < 4 && c->rotated != want; ++i) c->rotate();
        }
        if (move) {
            int x = args.contains(QLatin1String("x")) ? args.value(QLatin1String("x")).toInt() : c->cx;
            int y = args.contains(QLatin1String("y")) ? args.value(QLatin1String("y")).toInt() : c->cy;
            const QPoint at = Schematic::withinModelLimit(QPoint(x, y));
            x = at.x();
            y = at.y();
            sch->setOnGrid(x, y);
            c->moveCenter(x - c->cx, y - c->cy);
        }
        sch->insertRawComponent(c, false);
    }
    if (args.contains(QLatin1String("active")))
        c->isActive = args.value(QLatin1String("active")).toBool() ? COMP_IS_ACTIVE : COMP_IS_OPEN;
    if (!rename.isEmpty()) c->Name = rename;
    sch->enlargeView(c);
    finish(sch, {QPoint(c->cx, c->cy)});
    return jsonResult(componentJson(c));
}

QJsonObject QucsControl::remove(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    QStringList missing, done;
    QList<Element*> doomed;
    QList<Conductor*> unlabelled;
    for (const QJsonValue& v : args.value(QLatin1String("names")).toArray()) {
        const QString name = v.toString().trimmed();
        if (Component* c = sch->getComponentByName(name)) {
            doomed << c;
            done << name;
            continue;
        }
        bool label = false;
        for (Wire* w : sch->a_DocWires)
            if (w->hasLabel() && w->label()->Name == name) {
                unlabelled << w;
                label = true;
            }
        for (Node* n : sch->a_DocNodes)
            if (n->hasLabel() && n->label()->Name == name) {
                unlabelled << n;
                label = true;
            }
        if (label) done << tr("the label %1").arg(name);
        else missing << name;
    }
    for (const QJsonValue& v : args.value(QLatin1String("wires")).toArray()) {
        const QJsonArray e = v.toArray();
        if (e.size() != 4) continue;
        const QPoint a(e.at(0).toInt(), e.at(1).toInt()), b(e.at(2).toInt(), e.at(3).toInt());
        Wire* found = nullptr;
        for (Wire* w : sch->a_DocWires)
            if ((QPoint(w->x1, w->y1) == a && QPoint(w->x2, w->y2) == b) || (QPoint(w->x1, w->y1) == b && QPoint(w->x2, w->y2) == a))
                found = w;
        if (found != nullptr) {
            doomed << found;
            done << tr("the wire %1, %2 - %3, %4").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
        } else {
            missing << QStringLiteral("[%1, %2, %3, %4]").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
        }
    }
    if (doomed.isEmpty() && unlabelled.isEmpty())
        return errorResult(missing.isEmpty() ? tr("Nothing to delete.") : tr("Not found: %1.").arg(missing.join(QStringLiteral(", "))));
    prepare(sch);
    for (Conductor* c : unlabelled) c->dropLabel();
    if (!doomed.isEmpty()) {
        sch->deselectElements(nullptr);
        for (Element* e : doomed) e->isSelected = true;
        sch->deleteElements();
    }
    finish(sch);
    QString text = tr("Deleted %1.").arg(done.join(QStringLiteral(", ")));
    if (!missing.isEmpty()) text += QLatin1Char(' ') + tr("Not found: %1.").arg(missing.join(QStringLiteral(", ")));
    return textResult(text);
}

bool QucsControl::pointOf(Schematic* sch, const QJsonValue& at, QPoint* point, QString* error) const
{
    if (at.isArray() || at.isObject()) {
        const QJsonArray xy = at.toArray();
        const QJsonObject o = at.toObject();
        if (at.isArray() && xy.size() >= 2) *point = QPoint(xy.at(0).toInt(), xy.at(1).toInt());
        else if (at.isObject() && o.contains(QLatin1String("x"))) *point = QPoint(o.value(QLatin1String("x")).toInt(), o.value(QLatin1String("y")).toInt());
        else {
            *error = tr("A place is [x, y].");
            return false;
        }
        *point = Schematic::withinModelLimit(*point);
        return true;
    }
    const QString pin = at.toString().trimmed();
    const qsizetype dot = pin.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        *error = tr("%1 is not a pin (\"R1.2\") or a place ([x, y]).").arg(pin);
        return false;
    }
    const QString name = pin.left(dot), which = pin.mid(dot + 1);
    Component* c = sch->getComponentByName(name);
    if (c == nullptr) {
        // A part without a name (a ground): by its type, when it is the
        // only one of it.
        QList<Component*> ofType;
        for (Component* pc : sch->a_DocComps)
            if (pc->Model == name && (pc->Name.isEmpty() || pc->Name == QLatin1String("*"))) ofType << pc;
        if (ofType.size() == 1) c = ofType.first();
        else if (ofType.size() > 1) {
            *error = tr("There are %1 of %2: give the pin's place, [x, y] (get_schematic has it).").arg(ofType.size()).arg(name);
            return false;
        }
    }
    if (c == nullptr) {
        *error = tr("There is no component %1.").arg(name);
        return false;
    }
    bool number = false;
    const int n = which.toInt(&number);
    const Port* port = nullptr;
    if (number && n >= 1 && n <= c->Ports.size()) port = c->Ports.at(n - 1);
    for (const Port* p : c->Ports)
        if (port == nullptr && !p->Name.isEmpty() && p->Name.compare(which, Qt::CaseInsensitive) == 0) port = p;
    if (port == nullptr) {
        *error = tr("%1 has no pin %2 (it has %3).").arg(name, which).arg(c->Ports.size());
        return false;
    }
    *point = QPoint(c->cx + port->x, c->cy + port->y);
    return true;
}

QJsonObject QucsControl::connectPins(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    QPoint a, b;
    if (!pointOf(sch, args.value(QLatin1String("from")), &a, &error) || !pointOf(sch, args.value(QLatin1String("to")), &b, &error))
        return errorResult(error);
    if (a == b) return errorResult(tr("The two ends are the same place."));
    prepare(sch);
    const std::size_t before = sch->a_DocWires.size();
    sch->connectWithWire(a, b);
    finish(sch, {a, b});
    return textResult(tr("Wired %1, %2 to %3, %4 (%5 wires now).")
                          .arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y())
                          .arg(sch->a_DocWires.size()) + (sch->a_DocWires.size() == before ? tr(" They were connected already.") : QString()));
}

QJsonObject QucsControl::addWire(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    QList<QPoint> points;
    for (const QJsonValue& v : args.value(QLatin1String("points")).toArray()) {
        QPoint p;
        if (!pointOf(sch, v, &p, &error)) return errorResult(error);
        points << p;
    }
    if (points.size() < 2) return errorResult(tr("A wire needs two places at least."));
    prepare(sch);
    for (int i = 1; i < points.size(); ++i)
        if (points.at(i) != points.at(i - 1)) sch->connectWithWire(points.at(i - 1), points.at(i));
    finish(sch, points);
    return textResult(tr("Wire drawn through %1 places (%2 wires now).").arg(points.size()).arg(sch->a_DocWires.size()));
}

QJsonObject QucsControl::setLabel(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    QPoint p;
    if (!pointOf(sch, args.value(QLatin1String("at")), &p, &error)) return errorResult(error);
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    Node* node = sch->findNode(p);
    Wire* wire = node == nullptr ? sch->selectedWire(p.x(), p.y()) : nullptr;
    if (node == nullptr && wire == nullptr) return errorResult(tr("There is no pin or wire at %1, %2.").arg(p.x()).arg(p.y()));
    Element* labelled = wire != nullptr ? sch->getWireLabel(wire->Port1) : sch->getWireLabel(node);
    if (labelled != nullptr && (labelled->Type & isComponent))
        return errorResult(tr("The net is ground: it cannot be labelled."));
    prepare(sch);
    if (labelled != nullptr) static_cast<Conductor*>(labelled)->dropLabel();
    if (!name.isEmpty()) {
        int xl = p.x() + 30, yl = p.y() - 30;
        sch->setOnGrid(xl, yl);
        if (wire != nullptr) wire->setName(name, QString(), p.x(), p.y(), xl, yl);
        else node->setName(name, QString(), xl, yl);
    }
    finish(sch, {p});
    return textResult(name.isEmpty() ? tr("The label at %1, %2 is gone.").arg(p.x()).arg(p.y())
                                     : tr("The net at %1, %2 is %3.").arg(p.x()).arg(p.y()).arg(name));
}

QJsonObject QucsControl::select(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    a_app->showDocument(sch);
    sch->deselectElements(nullptr);
    QStringList missing;
    int n = 0;
    for (const QJsonValue& v : args.value(QLatin1String("names")).toArray()) {
        if (Component* c = sch->getComponentByName(v.toString().trimmed())) {
            c->isSelected = true;
            ++n;
        } else {
            missing << v.toString();
        }
    }
    sch->viewport()->update();
    QString text = n == 0 ? tr("Nothing is selected.") : tr("%1 selected.").arg(n);
    if (!missing.isEmpty()) text += QLatin1Char(' ') + tr("Not found: %1.").arg(missing.join(QStringLiteral(", ")));
    return textResult(text);
}

QJsonObject QucsControl::zoom(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    a_app->showDocument(sch);
    const QString to = args.value(QLatin1String("to")).toString();
    if (to == QLatin1String("all")) sch->showAll();
    else if (to == QLatin1String("selection")) sch->zoomToSelection();
    else if (to == QLatin1String("in")) sch->zoomBy(1.5);
    else if (to == QLatin1String("out")) sch->zoomBy(1.0 / 1.5);
    else if (to == QLatin1String("none")) sch->showNoZoom();
    else return errorResult(tr("Zoom to all, selection, in, out or none."));
    sch->viewport()->update();
    return textResult(tr("The scale is %1.").arg(sch->getScale(), 0, 'g', 3));
}

QJsonObject QucsControl::undoRedo(const QJsonObject& args, bool redo)
{
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    a_app->showDocument(QucsApp::documentWidget(doc));
    QMetaObject::invokeMethod(a_app, "slotHideEdit", Qt::DirectConnection);
    bool ok = true;
    if (auto* sch = dynamic_cast<Schematic*>(doc)) {
        ok = redo ? sch->redo() : sch->undo();
        sch->viewport()->update();
    } else if (auto* text = qobject_cast<TextDoc*>(QucsApp::documentWidget(doc))) {
        if (redo) text->redo();
        else text->undo();
    }
    if (!ok) return errorResult(redo ? tr("There is nothing to redo.") : tr("There is nothing to undo."));
    return textResult(redo ? tr("Redone.") : tr("Undone."));
}

QJsonObject QucsControl::screenshot(const QJsonObject& args)
{
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    QImage image;
    auto* sch = dynamic_cast<Schematic*>(doc);
    if (sch != nullptr && args.value(QLatin1String("area")).toString() != QLatin1String("visible")) {
        const QRect area = qucs_s::graphicsexport::area(sch, false);
        if (area.isEmpty()) return errorResult(tr("%1 is empty.").arg(titleOf(doc)));
        // Big enough to read, not bigger than a picture should be.
        qucs_s::graphicsexport::Options options;
        options.scale = std::clamp(std::min(1600.0 / area.width(), 1600.0 / area.height()), 0.1, 2.0);
        image = qucs_s::graphicsexport::image(sch, options);
    } else {
        QWidget* w = QucsApp::documentWidget(doc);
        a_app->showDocument(w);
        image = (sch != nullptr ? sch->viewport() : w)->grab().toImage();
    }
    if (image.isNull()) return errorResult(tr("No picture could be made of %1.").arg(titleOf(doc)));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return {{QStringLiteral("content"),
             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                    {QStringLiteral("data"), QString::fromLatin1(png.toBase64())},
                                    {QStringLiteral("mimeType"), QStringLiteral("image/png")}},
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                    {QStringLiteral("text"), tr("%1, %2 x %3 pixels.").arg(titleOf(doc)).arg(image.width()).arg(image.height())}}}},
            {QStringLiteral("isError"), false}};
}

QJsonObject QucsControl::listComponentTypes(const QJsonObject& args)
{
    const QString search = args.value(QLatin1String("search")).toString().trimmed();
    QHash<Module*, QString> models;
    for (auto it = Module::Modules.cbegin(); it != Module::Modules.cend(); ++it) models.insert(it.value(), it.key());
    QJsonArray list;
    for (Category* category : Category::Categories) {
        for (Module* m : category->Content) {
            const QString type = models.value(m);
            if (type.isEmpty()) continue;
            QString name;
            if (m->info != nullptr) {
                char* file = nullptr;
                m->info(name, file, false);
            }
            const QString line = type + QLatin1Char(' ') + name + QLatin1Char(' ') + category->Name;
            if (!search.isEmpty() && !line.contains(search, Qt::CaseInsensitive)) continue;
            list.append(QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("name"), name},
                                    {QStringLiteral("category"), category->Name}});
        }
    }
    return jsonResult(list);
}

// ----------------------------------------------------------------------
// Menus and dialogs

QList<QAction*> QucsControl::menuActions(QStringList* paths) const
{
    QList<QAction*> actions;
    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& path) {
        for (QAction* a : menu->actions()) {
            if (a->isSeparator() || a->text().trimmed().isEmpty()) continue;
            const QString here = path + QStringLiteral(" > ") + cleanText(a->text());
            if (a->menu() != nullptr) {
                walk(a->menu(), here);
                continue;
            }
            actions << a;
            paths->append(here);
        }
    };
    for (QAction* top : a_app->menuBar()->actions())
        if (top->menu() != nullptr) walk(top->menu(), cleanText(top->text()));
    return actions;
}

QJsonObject QucsControl::listActions(const QJsonObject& args)
{
    const QString search = args.value(QLatin1String("search")).toString().trimmed();
    QStringList paths;
    const QList<QAction*> actions = menuActions(&paths);
    QJsonArray list;
    for (int i = 0; i < actions.size(); ++i) {
        if (!search.isEmpty() && !paths.at(i).contains(search, Qt::CaseInsensitive)) continue;
        QAction* a = actions.at(i);
        QJsonObject o{{QStringLiteral("action"), paths.at(i)}, {QStringLiteral("enabled"), a->isEnabled()}};
        if (a->isCheckable()) o.insert(QStringLiteral("checked"), a->isChecked());
        if (!a->shortcut().isEmpty()) o.insert(QStringLiteral("shortcut"), a->shortcut().toString(QKeySequence::NativeText));
        if (!a->objectName().isEmpty()) o.insert(QStringLiteral("object name"), a->objectName());
        list.append(o);
    }
    return jsonResult(list);
}

void QucsControl::triggerAction(const QJsonObject& args, const Done& done)
{
    const QString wanted = args.value(QLatin1String("action")).toString().trimmed();
    QStringList paths;
    const QList<QAction*> actions = menuActions(&paths);
    QAction* action = nullptr;
    for (int i = 0; i < actions.size() && action == nullptr; ++i)
        if (normalized(paths.at(i)) == normalized(wanted) || actions.at(i)->objectName() == wanted) action = actions.at(i);
    // Only the last part of the path, when it names one action alone.
    if (action == nullptr) {
        QList<QAction*> tails;
        for (int i = 0; i < actions.size(); ++i)
            if (normalized(paths.at(i)).section(QLatin1Char('>'), -1) == normalized(wanted)) tails << actions.at(i);
        if (tails.size() == 1) action = tails.first();
    }
    if (action == nullptr) {
        done(errorResult(tr("There is no action %1 (list_actions lists them).").arg(wanted)));
        return;
    }
    const QList<QAction*> refused = {a_app->fileQuit, a_app->fileOpen, a_app->fileSaveAs, a_app->filePrint, a_app->filePrintFit};
    if (refused.contains(action)) {
        done(errorResult(action == a_app->fileQuit
                             ? tr("Claude does not quit Qucs-S.")
                             : tr("%1 opens a dialog of the system, which cannot be filled here: open_document and "
                                  "save_document do this.").arg(cleanText(action->text()))));
        return;
    }
    if (!action->isEnabled()) {
        done(errorResult(tr("%1 cannot be used now.").arg(cleanText(action->text()))));
        return;
    }
    // Triggered from the event loop, the answer given a moment later: a
    // dialog it opens runs an event loop of its own until it is answered.
    QPointer<QAction> target(action);
    const QString name = cleanText(action->text());
    QTimer::singleShot(0, a_app, [target] {
        if (target) target->trigger();
    });
    QTimer::singleShot(500, this, [this, done, name] {
        QWidget* dialog = openDialog();
        done(textResult(dialog != nullptr
                            ? tr("%1: it opened “%2”, which waits for an answer (get_dialog reads it, set_dialog answers it).")
                                  .arg(name, dialog->windowTitle())
                            : tr("%1: done.").arg(name)));
    });
}

QWidget* QucsControl::openDialog() const
{
    if (QWidget* modal = QApplication::activeModalWidget()) return modal;
    // A window of Qucs-S that waits without being modal (Find, a tool).
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w != a_app && w->isVisible() && qobject_cast<QDialog*>(w) != nullptr) return w;
    return nullptr;
}

namespace {

// The tab page \a w is on (a page of a QTabWidget of the dialog), or null.
QWidget* pageOf(QWidget* w, QWidget* dialog, QTabWidget** tabs = nullptr)
{
    QWidget* child = w;
    for (QWidget* p = w->parentWidget(); p != nullptr && p != dialog; child = p, p = p->parentWidget())
        if (qobject_cast<QStackedWidget*>(p) != nullptr && qobject_cast<QTabWidget*>(p->parentWidget()) != nullptr) {
            if (tabs != nullptr) *tabs = static_cast<QTabWidget*>(p->parentWidget());
            return child;
        }
    return nullptr;
}

// Shown - or on a tab that is not in front, which a click would show.
bool shown(QWidget* w, QWidget* dialog)
{
    QTabWidget* tabs = nullptr;
    if (QWidget* page = pageOf(w, dialog, &tabs)) return w->isVisibleTo(page) && shown(tabs, dialog);
    return w->isVisibleTo(dialog);
}

// The title of the tab \a w is on, empty when it is on none.
QString tabOf(QWidget* w, QWidget* dialog)
{
    QTabWidget* tabs = nullptr;
    QWidget* page = pageOf(w, dialog, &tabs);
    return page != nullptr ? cleanText(tabs->tabText(tabs->indexOf(page))) : QString();
}

// Brings the tab \a w is on to the front (and those it is on in turn).
void reveal(QWidget* w, QWidget* dialog)
{
    QTabWidget* tabs = nullptr;
    if (QWidget* page = pageOf(w, dialog, &tabs)) {
        reveal(tabs, dialog);
        tabs->setCurrentWidget(page);
    }
}

} // namespace

QList<QWidget*> QucsControl::dialogControls(QWidget* dialog) const
{
    QList<QWidget*> controls;
    for (QWidget* w : dialog->findChildren<QWidget*>()) {
        if (!shown(w, dialog)) continue;
        // The line edit inside a combo box or a spin box is theirs.
        if (qobject_cast<QLineEdit*>(w) != nullptr
            && (qobject_cast<QComboBox*>(w->parentWidget()) != nullptr || qobject_cast<QAbstractSpinBox*>(w->parentWidget()) != nullptr))
            continue;
        const bool control = qobject_cast<QLineEdit*>(w) != nullptr || qobject_cast<QPlainTextEdit*>(w) != nullptr
                             || (qobject_cast<QTextEdit*>(w) != nullptr && !static_cast<QTextEdit*>(w)->isReadOnly())
                             || qobject_cast<QComboBox*>(w) != nullptr || qobject_cast<QAbstractSpinBox*>(w) != nullptr
                             || qobject_cast<QAbstractButton*>(w) != nullptr || qobject_cast<QTableWidget*>(w) != nullptr
                             || qobject_cast<QListWidget*>(w) != nullptr || qobject_cast<QTabWidget*>(w) != nullptr;
        if (!control) continue;
        if (auto* b = qobject_cast<QAbstractButton*>(w); b != nullptr && cleanText(b->text()).isEmpty() && b->toolTip().isEmpty())
            continue;   // (a tab bar's arrows, a combo box's button)
        controls << w;
    }
    return controls;
}

namespace {

QLayout* layoutHolding(QLayout* layout, QWidget* w)
{
    if (layout == nullptr) return nullptr;
    if (layout->indexOf(w) >= 0) return layout;
    for (int i = 0; i < layout->count(); ++i)
        if (QLayout* found = layoutHolding(layout->itemAt(i)->layout(), w)) return found;
    return nullptr;
}

// The label before \a w in its layout: in its row of a grid, or just
// before it in a row of widgets. (Pages of tabs not yet shown have no
// geometry to go by.)
QLabel* labelBeside(QWidget* w)
{
    QLayout* layout = w->parentWidget() != nullptr ? layoutHolding(w->parentWidget()->layout(), w) : nullptr;
    if (layout == nullptr) return nullptr;
    const int index = layout->indexOf(w);
    if (auto* grid = qobject_cast<QGridLayout*>(layout)) {
        int row = 0, column = 0, rows = 0, columns = 0;
        grid->getItemPosition(index, &row, &column, &rows, &columns);
        for (int c = column - 1; c >= 0; --c)
            if (QLayoutItem* item = grid->itemAtPosition(row, c))
                if (auto* l = qobject_cast<QLabel*>(item->widget())) return l;
    } else if (auto* box = qobject_cast<QBoxLayout*>(layout);
               box != nullptr && (box->direction() == QBoxLayout::LeftToRight || box->direction() == QBoxLayout::RightToLeft)) {
        for (int i = index - 1; i >= 0; --i) {
            QWidget* before = layout->itemAt(i)->widget();
            if (auto* l = qobject_cast<QLabel*>(before)) return l;
            if (before != nullptr) break;
        }
    }
    return nullptr;
}

// What a field is called: its label in a form, the label that names it as
// its buddy, the label before it in its layout or just to its left, else
// what it says of itself.
QString labelOf(QWidget* w, QWidget* dialog)
{
    for (QFormLayout* form : dialog->findChildren<QFormLayout*>())
        if (QWidget* label = form->labelForField(w))
            if (auto* l = qobject_cast<QLabel*>(label)) return cleanText(l->text()).remove(QLatin1Char(':'));
    for (QLabel* l : dialog->findChildren<QLabel*>())
        if (l->buddy() == w) return cleanText(l->text()).remove(QLatin1Char(':'));
    if (qobject_cast<QAbstractButton*>(w) == nullptr)
        if (QLabel* l = labelBeside(w); l != nullptr && !l->text().trimmed().isEmpty())
            return cleanText(l->text()).remove(QLatin1Char(':'));
    if (qobject_cast<QAbstractButton*>(w) == nullptr) {
        const QRect r(w->mapTo(dialog, QPoint(0, 0)), w->size());
        QLabel* best = nullptr;
        int bestGap = 400;
        for (QLabel* l : dialog->findChildren<QLabel*>()) {
            if (!l->isVisibleTo(dialog) || l->text().trimmed().isEmpty()) continue;
            const QRect lr(l->mapTo(dialog, QPoint(0, 0)), l->size());
            const bool sameRow = lr.center().y() >= r.top() - 4 && lr.center().y() <= r.bottom() + 4;
            const int gap = r.left() - lr.right();
            if (sameRow && gap >= -2 && gap < bestGap) {
                best = l;
                bestGap = gap;
            }
        }
        if (best != nullptr) return cleanText(best->text()).remove(QLatin1Char(':'));
    }
    if (auto* b = qobject_cast<QAbstractButton*>(w)) return cleanText(b->text()).isEmpty() ? b->toolTip() : cleanText(b->text());
    if (auto* e = qobject_cast<QLineEdit*>(w); e != nullptr && !e->placeholderText().isEmpty()) return e->placeholderText();
    if (!w->toolTip().isEmpty()) return w->toolTip();
    return w->objectName();
}

} // namespace

QJsonObject QucsControl::getDialog()
{
    QWidget* dialog = openDialog();
    if (dialog == nullptr) return textResult(tr("No dialog is open."));
    QJsonArray controls;
    const QList<QWidget*> list = dialogControls(dialog);
    for (int i = 0; i < list.size(); ++i) {
        QWidget* w = list.at(i);
        QJsonObject o{{QStringLiteral("id"), QStringLiteral("c%1").arg(i + 1)}, {QStringLiteral("label"), labelOf(w, dialog)}};
        if (const QString tab = tabOf(w, dialog); !tab.isEmpty()) o.insert(QStringLiteral("tab"), tab);
        if (!w->isEnabled()) o.insert(QStringLiteral("enabled"), false);
        if (auto* e = qobject_cast<QLineEdit*>(w)) {
            o.insert(QStringLiteral("kind"), QStringLiteral("field"));
            o.insert(QStringLiteral("value"), e->text());
        } else if (auto* p = qobject_cast<QPlainTextEdit*>(w)) {
            o.insert(QStringLiteral("kind"), QStringLiteral("text"));
            o.insert(QStringLiteral("value"), p->toPlainText().left(4000));
        } else if (auto* t = qobject_cast<QTextEdit*>(w)) {
            o.insert(QStringLiteral("kind"), QStringLiteral("text"));
            o.insert(QStringLiteral("value"), t->toPlainText().left(4000));
        } else if (auto* c = qobject_cast<QComboBox*>(w)) {
            o.insert(QStringLiteral("kind"), c->isEditable() ? QStringLiteral("editable list") : QStringLiteral("list"));
            o.insert(QStringLiteral("value"), c->currentText());
            QJsonArray items;
            for (int k = 0; k < c->count() && k < 200; ++k) items.append(c->itemText(k));
            o.insert(QStringLiteral("items"), items);
        } else if (auto* s = qobject_cast<QAbstractSpinBox*>(w)) {
            o.insert(QStringLiteral("kind"), QStringLiteral("number"));
            o.insert(QStringLiteral("value"), s->text());
        } else if (auto* b = qobject_cast<QAbstractButton*>(w)) {
            if (b->isCheckable() && (qobject_cast<QCheckBox*>(w) != nullptr || qobject_cast<QRadioButton*>(w) != nullptr)) {
                o.insert(QStringLiteral("kind"), qobject_cast<QRadioButton*>(w) != nullptr ? QStringLiteral("option") : QStringLiteral("check box"));
                o.insert(QStringLiteral("value"), b->isChecked());
            } else {
                o.insert(QStringLiteral("kind"), QStringLiteral("button"));
                if (b->isCheckable()) o.insert(QStringLiteral("value"), b->isChecked());
            }
        } else if (auto* tabs = qobject_cast<QTabWidget*>(w)) {
            o.insert(QStringLiteral("kind"), QStringLiteral("tabs"));
            o.insert(QStringLiteral("value"), cleanText(tabs->tabText(tabs->currentIndex())));
            QJsonArray items;
            for (int k = 0; k < tabs->count(); ++k) items.append(cleanText(tabs->tabText(k)));
            o.insert(QStringLiteral("items"), items);
        } else if (auto* table = qobject_cast<QTableWidget*>(w)) {
            o.insert(QStringLiteral("kind"), QStringLiteral("table"));
            QJsonArray columns;
            for (int k = 0; k < table->columnCount(); ++k)
                columns.append(table->horizontalHeaderItem(k) != nullptr ? table->horizontalHeaderItem(k)->text() : QString::number(k));
            QJsonArray rows;
            for (int r = 0; r < table->rowCount() && r < 200; ++r) {
                QJsonArray row;
                for (int k = 0; k < table->columnCount(); ++k) {
                    if (QWidget* cell = table->cellWidget(r, k)) {
                        if (auto* cc = qobject_cast<QComboBox*>(cell)) row.append(cc->currentText());
                        else if (auto* cb = qobject_cast<QAbstractButton*>(cell)) row.append(cb->isChecked());
                        else if (auto* ce = qobject_cast<QLineEdit*>(cell)) row.append(ce->text());
                        else row.append(QString());
                    } else {
                        QTableWidgetItem* item = table->item(r, k);
                        if (item != nullptr && (item->flags() & Qt::ItemIsUserCheckable)) row.append(item->checkState() == Qt::Checked);
                        else row.append(item != nullptr ? item->text() : QString());
                    }
                }
                rows.append(row);
            }
            o.insert(QStringLiteral("columns"), columns);
            o.insert(QStringLiteral("rows"), rows);
        } else if (auto* lw = qobject_cast<QListWidget*>(w)) {
            o.insert(QStringLiteral("kind"), QStringLiteral("list"));
            o.insert(QStringLiteral("value"), lw->currentItem() != nullptr ? lw->currentItem()->text() : QString());
            QJsonArray items;
            for (int k = 0; k < lw->count() && k < 200; ++k) items.append(lw->item(k)->text());
            o.insert(QStringLiteral("items"), items);
        }
        controls.append(o);
    }
    QJsonArray texts;
    if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
        texts.append(box->text());
        if (!box->informativeText().isEmpty()) texts.append(box->informativeText());
    } else {
        for (QLabel* l : dialog->findChildren<QLabel*>())
            if (l->isVisibleTo(dialog) && !l->text().trimmed().isEmpty() && l->buddy() == nullptr) texts.append(cleanText(l->text()));
    }
    return jsonResult(QJsonObject{{QStringLiteral("title"), dialog->windowTitle()},
                                  {QStringLiteral("modal"), dialog == QApplication::activeModalWidget()},
                                  {QStringLiteral("texts"), texts},
                                  {QStringLiteral("controls"), controls}});
}

void QucsControl::setDialog(const QJsonObject& args, const Done& done)
{
    QWidget* dialog = openDialog();
    if (dialog == nullptr) {
        done(errorResult(tr("No dialog is open.")));
        return;
    }
    const QList<QWidget*> list = dialogControls(dialog);
    const auto find = [&](const QString& key) -> QWidget* {
        const QString k = key.trimmed();
        static const QRegularExpression id(QStringLiteral("^c(\\d+)$"));
        if (const auto m = id.match(k); m.hasMatch()) {
            const int n = m.captured(1).toInt();
            return n >= 1 && n <= list.size() ? list.at(n - 1) : nullptr;
        }
        for (QWidget* w : list)
            if (labelOf(w, dialog).compare(k, Qt::CaseInsensitive) == 0) return w;
        for (QWidget* w : list)
            if (labelOf(w, dialog).startsWith(k, Qt::CaseInsensitive)) return w;
        return nullptr;
    };
    QStringList problems, changed;
    for (const QJsonValue& v : args.value(QLatin1String("set")).toArray()) {
        const QJsonObject change = v.toObject();
        const QString key = change.value(QLatin1String("control")).toString();
        const QJsonValue value = change.value(QLatin1String("value"));
        QWidget* w = find(key);
        if (w == nullptr) {
            problems << tr("no control %1").arg(key);
            continue;
        }
        const QString text = propertyValue(value);
        bool ok = true;
        reveal(w, dialog);   // as the user would, on its tab
        if (auto* e = qobject_cast<QLineEdit*>(w)) {
            e->setText(text);
            e->setModified(true);
        } else if (auto* p = qobject_cast<QPlainTextEdit*>(w)) {
            p->setPlainText(text);
        } else if (auto* t = qobject_cast<QTextEdit*>(w)) {
            t->setPlainText(text);
        } else if (auto* c = qobject_cast<QComboBox*>(w)) {
            int index = c->findText(text, Qt::MatchFixedString);
            if (index < 0) index = c->findText(text, Qt::MatchContains);
            if (index >= 0) c->setCurrentIndex(index);
            else if (c->isEditable()) c->setEditText(text);
            else ok = false;
        } else if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) {
            d->setValue(text.toDouble(&ok));
        } else if (auto* s = qobject_cast<QSpinBox*>(w)) {
            s->setValue(text.toInt(&ok));
        } else if (auto* tabs = qobject_cast<QTabWidget*>(w)) {
            ok = false;
            for (int k = 0; k < tabs->count(); ++k)
                if (cleanText(tabs->tabText(k)).compare(text, Qt::CaseInsensitive) == 0) {
                    tabs->setCurrentIndex(k);
                    ok = true;
                }
        } else if (auto* table = qobject_cast<QTableWidget*>(w)) {
            const QJsonArray cell = value.toArray();
            const int r = cell.at(0).toInt(-1), k = cell.at(1).toInt(-1);
            if (cell.size() < 3 || r < 0 || r >= table->rowCount() || k < 0 || k >= table->columnCount()) {
                ok = false;
            } else if (QWidget* cw = table->cellWidget(r, k)) {
                if (auto* cc = qobject_cast<QComboBox*>(cw)) cc->setCurrentIndex(std::max(0, cc->findText(propertyValue(cell.at(2)))));
                else if (auto* cb = qobject_cast<QAbstractButton*>(cw)) {
                    if (cb->isChecked() != cell.at(2).toBool()) cb->click();
                } else if (auto* ce = qobject_cast<QLineEdit*>(cw)) ce->setText(propertyValue(cell.at(2)));
            } else {
                QTableWidgetItem* item = table->item(r, k);
                if (item == nullptr) table->setItem(r, k, item = new QTableWidgetItem);
                if (item->flags() & Qt::ItemIsUserCheckable) item->setCheckState(cell.at(2).toBool() ? Qt::Checked : Qt::Unchecked);
                else item->setText(propertyValue(cell.at(2)));
                table->setCurrentCell(r, k);
            }
        } else if (auto* lw = qobject_cast<QListWidget*>(w)) {
            const QList<QListWidgetItem*> items = lw->findItems(text, Qt::MatchFixedString);
            if (items.isEmpty()) ok = false;
            else lw->setCurrentItem(items.first());
        } else if (auto* b = qobject_cast<QAbstractButton*>(w)) {
            // A check box as a click would set it (its signals go).
            const bool want = value.isBool() ? value.toBool() : text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
            if (b->isCheckable() && b->isChecked() != want) b->click();
            else if (!b->isCheckable()) ok = false;
        }
        if (ok) changed << labelOf(w, dialog);
        else problems << tr("%1 does not take %2").arg(labelOf(w, dialog), text);
    }
    const QString press = args.value(QLatin1String("press")).toString().trimmed();
    QAbstractButton* button = nullptr;
    if (!press.isEmpty()) {
        QWidget* w = find(press);
        button = qobject_cast<QAbstractButton*>(w);
        if (button == nullptr)
            for (QWidget* c : list)
                if (auto* b = qobject_cast<QAbstractButton*>(c); b != nullptr && cleanText(b->text()).compare(press, Qt::CaseInsensitive) == 0)
                    button = b;
        if (button == nullptr) problems << tr("no button %1").arg(press);
    }
    QString report = changed.isEmpty() ? QString() : tr("Set: %1.").arg(changed.join(QStringLiteral(", ")));
    if (!problems.isEmpty()) report += (report.isEmpty() ? QString() : QStringLiteral(" ")) + tr("Not done: %1.").arg(problems.join(QStringLiteral("; ")));
    if (button == nullptr) {
        done(textResult(report.isEmpty() ? tr("Nothing changed.") : report, !problems.isEmpty() && changed.isEmpty()));
        return;
    }
    // Pressed from the event loop: it may close the dialog, or open another.
    QPointer<QAbstractButton> target(button);
    QPointer<QWidget> was(dialog);
    const QString name = cleanText(button->text());
    QTimer::singleShot(0, a_app, [target] {
        if (target && target->isEnabled()) target->click();
    });
    QTimer::singleShot(500, this, [this, done, report, name, was] {
        QWidget* now = openDialog();
        QString after;
        if (now == nullptr) after = tr("%1 pressed; no dialog is open now.").arg(name);
        else if (now == was.data()) after = tr("%1 pressed; the dialog is still open.").arg(name);
        else after = tr("%1 pressed; “%2” is open now.").arg(name, now->windowTitle());
        done(textResult(report.isEmpty() ? after : report + QLatin1Char(' ') + after));
    });
}

// ----------------------------------------------------------------------
// Simulation

void QucsControl::simulate(const QJsonObject& args, const Done& done)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) {
        done(errorResult(error));
        return;
    }
    if (sch->getDocName().isEmpty()) {
        done(errorResult(tr("%1 has no file yet: save_document with 'as' first.").arg(titleOf(sch))));
        return;
    }
    SimulationConsole* console = a_app->simulationConsole();
    if (console == nullptr || console->isRunning()) {
        done(errorResult(tr("A simulation is running already.")));
        return;
    }
    a_app->showDocument(sch);
    if (QucsSettings.DefaultSimulator == spicecompat::simQucsator) {
        QTimer::singleShot(0, a_app, [app = a_app] { app->slotSimulate(); });
        QTimer::singleShot(200, this, [done] {
            done(textResult(tr("The simulation with Qucsator has started; its end is not waited for here.")));
        });
        return;
    }
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(120), 5, 3600) * 1000;
    QPointer<Schematic> doc(sch);
    // A dataset written since now: the simulator did produce something.
    const QDateTime started = QDateTime::currentDateTime().addSecs(-1);
    // Started from the event loop (the legacy window runs one of its
    // own); the run it made is watched after.
    QTimer::singleShot(0, a_app, [app = a_app] { app->slotSimulateWithSpice(); });
    QTimer::singleShot(0, this, [this, done, timeout, doc, console, started] {
        SimulationRun* run = console->currentRun();
        if (run == nullptr) {
            QStringList log;
            for (int i = 0; i < console->statusLog()->count(); ++i) log << console->statusLog()->item(i)->text();
            done(errorResult(tr("The simulation did not start. %1").arg(log.join(QLatin1Char('\n')))));
            return;
        }
        auto answered = std::make_shared<bool>(false);
        auto report = [done, answered, doc, console, started](SimulationRun* r, bool timedOut) {
            if (*answered) return;
            *answered = true;
            QStringList lines = console->console()->toPlainText().split(QLatin1Char('\n'));
            QStringList problems;
            for (const QString& line : std::as_const(lines))
                if (line.contains(QLatin1String("error"), Qt::CaseInsensitive) || line.contains(QLatin1String("warning"), Qt::CaseInsensitive))
                    if (problems.size() < 40) problems << line.trimmed();
            while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
            QJsonObject result{{QStringLiteral("finished"), !timedOut},
                               {QStringLiteral("errors or warnings"), QJsonArray::fromStringList(problems)},
                               {QStringLiteral("last lines"), lines.mid(std::max<qsizetype>(0, lines.size() - 40)).join(QLatin1Char('\n'))}};
            bool written = false;
            if (doc) {
                const QFileInfo info(doc->getDocName());
                const QFileInfo dataset(info.absoluteDir().filePath(doc->getDataSet()));
                written = dataset.isFile() && dataset.lastModified() >= started;
                result.insert(QStringLiteral("dataset"), dataset.absoluteFilePath());
                result.insert(QStringLiteral("dataset written"), written);
                result.insert(QStringLiteral("data display"), info.absoluteDir().filePath(doc->getDataDisplay()));
            }
            if (r != nullptr && !timedOut) {
                result.insert(QStringLiteral("succeeded"), r->wasSimulated() && !r->hasError() && written);
                result.insert(QStringLiteral("stopped"), r->wasStopped());
                result.insert(QStringLiteral("warnings"), r->warningCount());
            }
            if (timedOut) result.insert(QStringLiteral("note"), tr("Still running: its end was not waited for any longer."));
            done(jsonResult(result));
        };
        connect(run, &SimulationRun::simulated, this, [report](SimulationRun* r) { report(r, false); });
        QTimer::singleShot(timeout, this, [report] { report(nullptr, true); });
    });
}
