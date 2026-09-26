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
#include "qucscontrol_p.h"

#include "components/component.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "extsimkernels/spicecompat.h"
#include "extsimkernels/simulationrun.h"
#include "geometry/multi_point.h"
#include "graphicsexport.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "node.h"
#include "paintings/painting.h"
#include "qucs.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "simulatorlog.h"
#include "dataset.h"
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
 "description": "Opens a file in a tab of Qucs-S - a schematic (.sch), a symbol (.sym), a data display (.dpl), a netlist or any text file, a PDF document (read in Qucs-S's viewer: a datasheet, a report) - or brings it to the front if it is open. A relative path is taken from the workspace folder.",
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
 "description": "Reads a schematic as it is in Qucs-S now, unsaved changes included. 'summary' (the default) lists its components - name, type, place, rotation, mirroring, whether active, properties, and each pin's place, whether anything is on it and its net (a label's name, gnd, or net1, net2, ...) - its nets with the pins on each (those with two pins or more, or a name), its wires, net labels, paintings, and its diagrams, numbered as the diagram tools take them, with their axes and traces - each trace's variable, look, and points or why it shows no data. 'text' is the text its .sch file would have. Coordinates are the schematic's units; the grid is usually 10.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "format": {"type": "string", "enum": ["summary", "text"]}}}},
{"name": "set_schematic",
 "description": "Replaces the elements of a schematic with those of 'text': a .sch file's text, or any of its <Components>, <Wires>, <Diagrams> and <Paintings> sections (sections left out stay as they are; <Properties> and <Symbol> are not taken). One step to undo; the diagrams read their data again. When the text does not read, the schematic stays as it was and the error is told. For diagrams and traces add_diagram, edit_diagram, add_trace and edit_trace are simpler and safer.",
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
 "description": "Changes a component: its properties (by name), its name, its place (x, y: where its centre goes), its rotation (0-3 quarter turns) and mirroring, whether it is active (an inactive one is left out of the simulation). What is not given stays. Turned or moved, the circuit stays as it was: its pins are wired again to the nets they were on, and wires of other nets that its pins would come down on are moved out of the way. A change that cannot keep every net as it was is not made, and the error says why. A pin with nothing on it that comes down on another part's pin joins that pin's net, and the result's 'note' says so.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "name": {"type": "string"}, "rename": {"type": "string"},
   "properties": {"type": "object", "additionalProperties": {"type": "string"}},
   "x": {"type": "integer"}, "y": {"type": "integer"}, "rotation": {"type": "integer", "minimum": 0, "maximum": 3},
   "mirror": {"type": "boolean"}, "active": {"type": "boolean"}}, "required": ["name"]}},
{"name": "delete",
 "description": "Deletes components (by name), net labels (by the net's name), wires (by their two ends, [x1, y1, x2, y2]), diagrams (by their numbers as get_schematic lists them) and traces ({\"diagram\": n, \"trace\": its number or variable}) from a schematic, as one step to undo.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "names": {"type": "array", "items": {"type": "string"}},
   "wires": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}, "minItems": 4, "maxItems": 4}},
   "diagrams": {"type": "array", "items": {"type": "integer"}},
   "traces": {"type": "array", "items": {"type": "object", "properties": {"diagram": {"type": "integer"}, "trace": {}}}}}}},
{"name": "connect",
 "description": "Draws a wire between two pins or places, with right angles, by a way that goes over no other pin or wire (a wire joins whatever it runs over): around the parts when it can, else over them. It joins the two nets and nothing else - or, when no way would, draws nothing and says why. A pin is \"R1.1\" (the component's name and the pin's number, from 1, or the pin's name); a place is [x, y].",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"},
   "from": {"description": "\"R1.2\" or [x, y]; a ground is GND.1 when there is one"}, "to": {"description": "\"C1.1\" or [x, y]"}}, "required": ["from", "to"]}},
{"name": "add_wire",
 "description": "Draws a wire through places, [[x, y], [x, y], ...], a segment from each to the next (a step that is not straight gets a right angle). What is at its places is joined; a wire that would run over another pin or wire between them is not drawn, and the error names what is in the way.",
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
 "description": "The components of the library: the type to give add_component, what it is, its category. 'search' keeps those whose type, name or category has it. describe_component_type tells a type's properties.",
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
 "description": "Simulates a schematic (the one in front unless path names another; it must have been saved once) with the simulator in the settings, as Simulation > Simulate, and waits for the end. It says whether it succeeded (the simulator ran to its end and reported no error), its errors and warnings - each with its message and, where the simulator names them, the netlist line (its number and text), the part of the schematic and the node - the dataset it wrote (name.dat.ngspice for ngspice, .dat.xyce, .dat.spopus; name.dat for Qucsator) and its variables, the traces of the diagrams that show no data and why, and the last lines of the output. 'timeout' in seconds, 120 unless given. 'keep_as' keeps a copy of the dataset under that name, to compare runs: get_dataset reads it by its file, and a trace shows it beside the current run as ngspice/<name>:tran.v(out).",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "timeout": {"type": "integer"},
   "keep_as": {"type": "string", "description": "A name of letters, digits, _ and -: the copy is <name>.dat.ngspice (or .xyce, ...) beside the schematic"}}}},
{"name": "get_netlist",
 "description": "The netlist of a schematic as text: as a simulation with the simulator in the settings would write it now, or with 'last' the one the last simulation ran (Simulation > Show Last Netlist; the one the line numbers of simulate's errors are of). 'numbered' puts each line's number before it.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "last": {"type": "boolean"}, "numbered": {"type": "boolean"}}}},
{"name": "get_dataset",
 "description": "Reads a simulation's results as numbers, from the dataset the simulator wrote. Without 'variables': the variables it holds - the independent ones (time, frequency, a swept parameter: their range and points) and the others (what they depend on, their points, whether complex, their range) with the name a trace takes. With 'variables': for each, over the range from 'from' to 'to' of its x, its statistics (min and max and where, mean and RMS weighted over x, initial and final value) and, as asked, samples ('points': at most so many, spread evenly over the samples in the range), values at given x ('at': interpolated), and measurements on the full data ('measure'): rise_time and fall_time (10%-90% of the swing, the first edge), overshoot (percent of the step), settling_time (within 'tolerance' of the step, 0.02 unless given, from 'from'), period, frequency and duty_cycle (at 'level', the middle of the swing unless given), crossings (of 'level'), bandwidth (-3 dB points of a magnitude). A variable swept over a parameter gives a curve for each of its values. Complex values (AC) come as magnitude and phase in degrees unless 'form' says otherwise; statistics and measurements are of the magnitude.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "A schematic or data display, open or not (the document in front when not given), or a dataset file (.dat, .dat.ngspice, ...)"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"], "description": "Whose dataset of a schematic: the simulator in the settings unless given (else the newest there is)"},
   "variables": {"type": "array", "items": {"type": "string"}, "description": "As get_dataset lists them (tran.v(out)), as a trace names them (ngspice/tran.v(out)), without the analysis (v(out): each analysis's), or a node's name (out: its voltage)"},
   "from": {"type": "number"}, "to": {"type": "number"},
   "points": {"type": "integer", "minimum": 0, "maximum": 5000, "description": "Samples of each curve: 100 unless 'at' or 'measure' is given, then none"},
   "at": {"type": "array", "items": {"type": "number"}},
   "measure": {"type": "array", "items": {"type": "string", "enum": ["rise_time", "fall_time", "overshoot", "settling_time", "period", "frequency", "duty_cycle", "crossings", "bandwidth"]}},
   "level": {"type": "number"}, "tolerance": {"type": "number"},
   "form": {"type": "string", "enum": ["magnitude_phase", "db_phase", "real_imaginary"]}}}},
{"name": "reload_data",
 "description": "Reads the datasets again and redraws the diagrams of a document (the document named by path, or every open one), as Simulation > Reload Simulation Data: after a simulation outside Qucs-S, or when a plot is blank. Says which traces still show no data, and why.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}}},
{"name": "add_diagram",
 "description": "Places a diagram on a schematic or a data display, its traces showing the dataset's data at once. 'type': rect (x-y, the default), polar, smith, admittance_smith, polar_smith, smith_polar, tab (a table), timing, truth, 3d, locus, histogram. x, y: its lower left corner; width and height, 240 x 160 unless given. 'traces': each a variable (as get_dataset names it - tran.v(out), or v(out) or out when that says which) or an object as add_trace takes it. The axes, grid and legend as edit_diagram takes them. Returns the diagram as get_schematic lists it: its number, and each trace's points or why it has none.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "type": {"type": "string"}, "x": {"type": "integer"}, "y": {"type": "integer"},
   "width": {"type": "integer"}, "height": {"type": "integer"},
   "traces": {"type": "array", "items": {}},
   "x_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}},
   "y_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}},
   "y2_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}},
   "grid": {"type": "boolean"}, "legend": {"type": "string", "enum": ["off", "top_left", "top_right", "bottom_left", "bottom_right"]}},
   "required": ["x", "y"]}},
{"name": "edit_diagram",
 "description": "Changes a diagram: its place (x, y: the lower left corner) and size, its axes (x_axis, y_axis, and y2_axis on the right: label, log, auto, from, to, step, units), grid, legend. What is not given stays. 'diagram' is its number as get_schematic lists them (it may be left out when there is one). One step to undo; the traces are read again from the dataset.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "diagram": {"type": "integer"},
   "x": {"type": "integer"}, "y": {"type": "integer"}, "width": {"type": "integer"}, "height": {"type": "integer"},
   "x_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}},
   "y_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}},
   "y2_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}},
   "grid": {"type": "boolean"}, "legend": {"type": "string", "enum": ["off", "top_left", "top_right", "bottom_left", "bottom_right"]}}}},
{"name": "add_trace",
 "description": "Adds a trace to a diagram: 'variable' as get_dataset names it (tran.v(out); v(out) or out when that says which; the simulator's prefix is added), its color (#rrggbb, a name, or auto: each swept curve a color of its own), thickness, style (solid, dash, dot, long_dash, stars, circles, arrows), the y axis it is drawn on (left or right), point markers (none, auto, circle, square, triangle, diamond, triangle_down, cross, plus), auto_color (each swept curve a color of its own); in a table, precision and numbers (real_imaginary, magnitude_degrees, magnitude_radians). Returns its points, or why it shows nothing.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "diagram": {"type": "integer"}, "variable": {"type": "string"},
   "color": {"type": "string"}, "thickness": {"type": "integer", "minimum": 0, "maximum": 99},
   "style": {"type": "string", "enum": ["solid", "dash", "dot", "long_dash", "stars", "circles", "arrows"]},
   "axis": {"type": "string", "enum": ["left", "right"]},
   "marker": {"type": "string", "enum": ["none", "auto", "circle", "square", "triangle", "diamond", "triangle_down", "cross", "plus"]},
   "auto_color": {"type": "boolean"}, "precision": {"type": "integer"},
   "numbers": {"type": "string", "enum": ["real_imaginary", "magnitude_degrees", "magnitude_radians"]}},
   "required": ["variable"]}},
{"name": "edit_trace",
 "description": "Changes a trace of a diagram - 'trace' is its number in the diagram or its variable - as add_trace takes it: another variable, its color, thickness, style, axis, marker, ... What is not given stays. One step to undo.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "diagram": {"type": "integer"}, "trace": {"description": "Its number (from 1) or its variable"},
   "variable": {"type": "string"}, "color": {"type": "string"}, "thickness": {"type": "integer", "minimum": 0, "maximum": 99},
   "style": {"type": "string", "enum": ["solid", "dash", "dot", "long_dash", "stars", "circles", "arrows"]},
   "axis": {"type": "string", "enum": ["left", "right"]},
   "marker": {"type": "string", "enum": ["none", "auto", "circle", "square", "triangle", "diamond", "triangle_down", "cross", "plus"]},
   "auto_color": {"type": "boolean"}, "precision": {"type": "integer"},
   "numbers": {"type": "string", "enum": ["real_imaginary", "magnitude_degrees", "magnitude_radians"]}}}},
{"name": "rename_net",
 "description": "Renames a net - its labels, or a net get_schematic calls net1, net2, ... gets a label - and whatever names its voltage: the traces of the schematic's diagrams and of its data displays (open ones as a change to undo; the .dpl file of a closed one is rewritten) and its equations: v(out) becomes v(out1), out.v out1.v. Refused when a net has the new name already (that would join the two). The dataset keeps the old name until the next simulation.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "from": {"type": "string"}, "to": {"type": "string"}}, "required": ["from", "to"]}},
{"name": "describe_component_type",
 "description": "A component type of the library described: what it is, its category, how its parts are named, its pins (their places relative to its centre, unturned), its properties in their order - name, default value, unit, what it means, whether shown on the schematic, and the simulators it counts for - the simulators it works with, the netlist line it makes with its defaults under the simulator in the settings, and notes on what is easy to get wrong with it (a Vpulse is one pulse: Vrect repeats).",
 "inputSchema": {"type": "object", "properties": {"type": {"type": "string", "description": "As list_component_types gives it: R, Vpulse, .TR, ..."}}, "required": ["type"]}}
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
    {"add_diagram", QT_TRANSLATE_NOOP("QucsControl", "add a diagram in Qucs-S")},
    {"edit_diagram", QT_TRANSLATE_NOOP("QucsControl", "change a diagram in Qucs-S")},
    {"add_trace", QT_TRANSLATE_NOOP("QucsControl", "add a trace to a diagram in Qucs-S")},
    {"edit_trace", QT_TRANSLATE_NOOP("QucsControl", "change a trace in Qucs-S")},
    {"rename_net", QT_TRANSLATE_NOOP("QucsControl", "rename a net in Qucs-S")},
};

// Tools that only look (or move the view): used without asking.
const char* const kReadOnly[] = {"get_state", "get_schematic", "screenshot", "list_component_types", "list_actions",
                                 "get_dialog", "show_document", "select", "zoom", "get_netlist", "get_dataset",
                                 "reload_data", "describe_component_type"};

} // namespace

namespace qucs_s::control {

QString tr(const char* text)
{
    return QCoreApplication::translate("QucsControl", text);
}

QJsonObject textResult(const QString& text, bool error)
{
    return {{QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                               {QStringLiteral("text"), text}}}},
            {QStringLiteral("isError"), error}};
}

QJsonObject jsonResult(const QJsonValue& value, bool compact)
{
    const QJsonDocument::JsonFormat format = compact ? QJsonDocument::Compact : QJsonDocument::Indented;
    const QByteArray json = value.isObject() ? QJsonDocument(value.toObject()).toJson(format)
                                             : QJsonDocument(value.toArray()).toJson(format);
    return textResult(QString::fromUtf8(json));
}

QJsonObject errorResult(const QString& text)
{
    return textResult(text, true);
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

} // namespace qucs_s::control

using namespace qucs_s::control;

namespace {

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
    if (QucsApp::isPdfDocument(QucsApp::documentWidget(doc))) return QStringLiteral("pdf");
    const QString suffix = QFileInfo(doc->getDocName()).suffix().toLower();
    if (suffix == QLatin1String("dpl")) return QStringLiteral("data display");
    if (suffix == QLatin1String("sym")) return QStringLiteral("symbol");
    return QStringLiteral("schematic");
}

// Which pins are on one net - through wires, net labels (one name, one
// net) and ground - by a key for each: "R1.2" (a part without a name by
// its type, "GND.1"); "@.2" for \a edited's (it changes places in the
// list, the others keep their order); the second and later of parts of
// one name "X#1.1"; and for a net label
// "label out", for ground "ground", as if they were pins; and for each of
// \a probes, a place, "at x,y": the net of what is there (a new one when
// nothing is).
struct Nets {
    QHash<QString, int> netOf;
    QHash<const Node*, int> nodeNet;   // every node's
    QSet<QString> open;       // \a edited's pins with nothing else on them
    QSet<QString> touching;   // \a edited's pins with something on them
};

// A part's name in netsOf's keys: its name, or its type when it has none
// ("GND"), "#1", "#2" ... after the second and later of one name.
QString keyBase(const Component* c, QHash<QString, int>& seen)
{
    const QString name = c->Name.isEmpty() ? c->Model : c->Name;
    const int k = seen[name]++;
    return k == 0 ? name : name + QLatin1Char('#') + QString::number(k);
}

Nets netsOf(Schematic* sch, const Component* edited, const QList<QPoint>& probes = {})
{
    std::vector<int> up;
    QHash<const Node*, int> nodes;
    QHash<QString, int> named;
    const auto make = [&up] {
        up.push_back(int(up.size()));
        return int(up.size()) - 1;
    };
    const auto nodeId = [&](const Node* n) {
        auto it = nodes.constFind(n);
        return it != nodes.constEnd() ? *it : *nodes.insert(n, make());
    };
    const auto nameId = [&](const QString& name) {
        auto it = named.constFind(name);
        return it != named.constEnd() ? *it : *named.insert(name, make());
    };
    const auto root = [&up](int i) {
        while (up[i] != i) i = up[i] = up[up[i]];
        return i;
    };
    const auto join = [&](int a, int b) { up[root(a)] = root(b); };
    const auto labelled = [&](const Conductor* c, int id) {
        if (!c->hasLabel()) return;
        const QString name = c->label()->Name;
        join(id, nameId(name.compare(QLatin1String("gnd"), Qt::CaseInsensitive) == 0 ? QStringLiteral("ground")
                                                                                      : QStringLiteral("label ") + name));
    };
    for (const Wire* w : sch->a_DocWires) {
        if (w->Port1 == nullptr || w->Port2 == nullptr) continue;
        join(nodeId(w->Port1), nodeId(w->Port2));
        labelled(w, nodeId(w->Port1));
    }
    for (const Node* n : sch->a_DocNodes) labelled(n, nodeId(n));

    Nets nets;
    QHash<QString, int> seen;
    QHash<QString, int> pins;
    for (const Component* c : sch->a_DocComps) {
        const QString base = c == edited ? QStringLiteral("@") : keyBase(c, seen);
        for (int i = 0; i < c->Ports.size(); ++i) {
            const Node* n = c->Ports.at(i)->Connection;
            if (n == nullptr) continue;
            const QString key = base + QLatin1Char('.') + QString::number(i + 1);
            const int id = nodeId(n);
            if (c->Model == QLatin1String("GND")) join(id, nameId(QStringLiteral("ground")));
            else if (c == edited) (n->conn_count() == 1 && !n->hasLabel() ? nets.open : nets.touching) << key;
            pins.insert(key, id);
        }
    }
    for (const QPoint& p : probes) {
        int id = -1;
        if (const Node* n = sch->findNode(p)) id = nodeId(n);
        for (const Wire* w : sch->a_DocWires)
            if (id < 0 && w->Port1 != nullptr && qucs_s::geom::is_between(p, w->P1(), w->P2())) id = nodeId(w->Port1);
        pins.insert(QStringLiteral("at %1,%2").arg(p.x()).arg(p.y()), id >= 0 ? id : make());
    }
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) nets.netOf.insert(it.key(), root(it.value()));
    for (auto it = named.constBegin(); it != named.constEnd(); ++it) nets.netOf.insert(it.key(), root(it.value()));
    for (auto it = nodes.constBegin(); it != nodes.constEnd(); ++it) nets.nodeNet.insert(it.key(), root(it.value()));
    return nets;
}

// A key of netsOf in words (\a name for the "@" of the part changed).
QString said(const QString& key, const QString& name)
{
    if (key == QLatin1String("ground")) return tr("ground");
    if (key.startsWith(QLatin1String("label "))) return tr("the net label %1").arg(key.mid(6));
    if (key.startsWith(QLatin1String("at "))) return tr("the place %1").arg(key.mid(3).replace(QLatin1Char(','), QStringLiteral(", ")));
    QString pin = key;
    if (pin.startsWith(QLatin1Char('@'))) pin = name + pin.mid(1);
    return pin.remove(QRegularExpression(QStringLiteral("#\\d+(?=\\.)")));
}

// What became of the nets from \a before to \a after, in words, for the
// part \a name (the "@" of netsOf): pins taken off a net they were on
// (unless \a merged only) and nets joined. Its pins that were open may
// come to be on a net - \a landed says which - but not on one with
// another of its pins.
QStringList netChanges(const Nets& before, const Nets& after, const QString& name, bool mergedOnly, QStringList* landed)
{
    const auto said = [&name](const QString& key) { return ::said(key, name); };
    QStringList changes;
    QStringList keys = before.netOf.keys();
    keys.sort();   // (the same words each time)
    if (!mergedOnly) {
        QHash<int, QString> firstOf;   // a net before -> one of its keys
        for (const QString& key : keys) {
            const int was = before.netOf.value(key);
            if (!after.netOf.contains(key)) {
                changes << tr("%1 would be gone").arg(said(key));
                continue;
            }
            auto it = firstOf.constFind(was);
            if (it == firstOf.constEnd()) firstOf.insert(was, key);
            else if (after.netOf.value(*it) != after.netOf.value(key))
                changes << tr("%1 would no longer be on the net of %2").arg(said(key), said(*it));
        }
    }
    QHash<int, QString> held;   // a net after -> a key of a part that was on something
    QHash<int, QString> own;    // a net after -> a pin of the part
    for (const QString& key : keys) {
        if (!after.netOf.contains(key)) continue;
        const int now = after.netOf.value(key);
        if (key.startsWith(QLatin1Char('@'))) {
            auto it = own.constFind(now);
            if (it == own.constEnd()) own.insert(now, key);
            else if (before.netOf.value(*it) != before.netOf.value(key))
                changes << tr("%1 and %2 would be on one net").arg(said(*it), said(key));
        }
        if (before.open.contains(key)) continue;
        auto it = held.constFind(now);
        if (it == held.constEnd()) held.insert(now, key);
        else if (before.netOf.value(*it) != before.netOf.value(key))
            changes << tr("%1 would be on the net of %2").arg(said(key), said(*it));
    }
    if (landed != nullptr)
        for (const QString& key : keys) {
            if (!before.open.contains(key) || !after.touching.contains(key)) continue;
            const auto it = held.constFind(after.netOf.value(key));
            *landed << (it != held.constEnd() ? tr("%1 is now on the net of %2").arg(said(key), said(*it))
                                               : tr("%1 is now on a wire").arg(said(key)));
        }
    changes.removeDuplicates();
    return changes;
}

// What became of the nets from \a before to \a after beyond making the
// nets of \a joined (keys) one, in words: nets split, nets joined.
QStringList netChangesBeyond(const Nets& before, const Nets& after, const QStringList& joined)
{
    QHash<int, int> as;   // a net before -> the one it is to be part of
    int one = -1;
    for (const QString& key : joined)
        if (before.netOf.contains(key)) {
            if (one < 0) one = before.netOf.value(key);
            as.insert(before.netOf.value(key), one);
        }
    const auto expected = [&](const QString& key) { return as.value(before.netOf.value(key), before.netOf.value(key)); };
    QStringList changes;
    QStringList keys = before.netOf.keys();
    keys.sort();
    QHash<int, QString> firstExpected, firstAfter;
    for (const QString& key : keys) {
        if (!after.netOf.contains(key)) {
            changes << tr("%1 would be gone").arg(said(key, {}));
            continue;
        }
        const int e = expected(key), a = after.netOf.value(key);
        auto it = firstExpected.constFind(e);
        if (it == firstExpected.constEnd()) firstExpected.insert(e, key);
        else if (after.netOf.value(*it) != a)
            changes << tr("%1 would no longer be on the net of %2").arg(said(key, {}), said(*it, {}));
        auto jt = firstAfter.constFind(a);
        if (jt == firstAfter.constEnd()) firstAfter.insert(a, key);
        else if (expected(*jt) != e) changes << tr("%1 would be on the net of %2").arg(said(key, {}), said(*jt, {}));
    }
    changes.removeDuplicates();
    return changes;
}

bool onSegment(const QPoint& p, const QPoint& a, const QPoint& b)
{
    return p == a || p == b || qucs_s::geom::is_between(p, a, b);
}

// Whether a wire along \a way (from \a way's first place to its last)
// would touch nothing of another net than \a ours (nets of \a now) on its
// way: no node (a pin, a wire's end) on it but at its two ends, none of
// its bends on a wire - and, \a strict, no part's symbol crossed and every
// piece straight across or up.
bool clearWay(Schematic* sch, const std::vector<QPoint>& way, bool strict, const Nets& now, const QSet<int>& ours)
{
    const QPoint a = way.front(), b = way.back();
    for (std::size_t k = 1; k < way.size(); ++k) {
        const QPoint p = way[k - 1], q = way[k];
        if (p == q) continue;
        if (strict && p.x() != q.x() && p.y() != q.y()) return false;
        for (const Node* n : sch->a_DocNodes) {
            const QPoint c = n->center();
            if (c != a && c != b && !ours.contains(now.nodeNet.value(n, -1)) && onSegment(c, p, q)) return false;
        }
        if (strict) {
            const QRect piece = QRect(p, q).normalized();
            for (const Component* c : sch->a_DocComps)
                if (!c->Ports.isEmpty() && piece.intersects(c->boundingRect().adjusted(1, 1, -1, -1))) return false;
        }
    }
    for (std::size_t k = 1; k + 1 < way.size(); ++k)
        for (const Wire* w : sch->a_DocWires)
            if (!ours.contains(now.nodeNet.value(w->Port1, -1)) && onSegment(way[k], w->P1(), w->P2())) return false;
    return true;
}

// The ways a wire from \a a to \a b may go, in the order they are tried:
// the wire planner's, then out to a line beside both ends - further and
// further out, on each side - along it and in; last straight.
std::vector<std::vector<QPoint>> waysBetween(Schematic* sch, const QPoint& a, const QPoint& b)
{
    using Plan = qucs_s::wire::Planner::PlanType;
    std::vector<std::vector<QPoint>> ways;
    for (Plan plan : {Plan::TwoStepXY, Plan::TwoStepYX, Plan::ThreeStepXY, Plan::ThreeStepYX})
        ways.push_back(qucs_s::wire::Planner::plan(plan, a, b));
    const int gx = std::max(sch->getGridX(), 1), gy = std::max(sch->getGridY(), 1);
    for (int k = 1; k <= 20; ++k) {
        for (int y : {std::min(a.y(), b.y()) - k * gy, std::max(a.y(), b.y()) + k * gy})
            ways.push_back({a, {a.x(), y}, {b.x(), y}, b});
        for (int x : {std::min(a.x(), b.x()) - k * gx, std::max(a.x(), b.x()) + k * gx})
            ways.push_back({a, {x, a.y()}, {x, b.y()}, b});
    }
    ways.push_back(qucs_s::wire::Planner::plan(Plan::Straight, a, b));
    return ways;
}

// Wires \a a to \a b the first way (waysBetween(): around the parts, then
// over them) that touches nothing else and that \a check - what is wrong
// with the nets, in words - finds nothing wrong with; a way it finds fault
// with is taken back (\a sch put back as it was). False, and why in \a
// why, when there is no such way. \a sch's elements may be new ones after.
bool wireUp(Schematic* sch, const QPoint& a, const QPoint& b, const std::function<QStringList()>& check, QString* why)
{
    const QString state = sch->snapshot();
    const std::vector<std::vector<QPoint>> ways = waysBetween(sch, a, b);
    // What is on the two nets already the wire may touch.
    Nets now;
    QSet<int> ours;
    const auto look = [&] {
        now = netsOf(sch, nullptr, {a, b});
        ours = {now.netOf.value(QStringLiteral("at %1,%2").arg(a.x()).arg(a.y())),
                now.netOf.value(QStringLiteral("at %1,%2").arg(b.x()).arg(b.y()))};
    };
    look();
    int tries = 0;
    QStringList faults;
    for (bool strict : {true, false})
        for (const std::vector<QPoint>& way : ways) {
            if (!clearWay(sch, way, strict, now, ours) || (!strict && clearWay(sch, way, true, now, ours))) continue;
            for (std::size_t k = 1; k < way.size(); ++k)
                if (way[k] != way[k - 1])
                    sch->connectWithWire(way[k - 1], way[k], true, qucs_s::wire::Planner::PlanType::Straight);
            faults = check();
            if (faults.isEmpty()) return true;
            sch->restore(state);
            look();   // (new nodes)
            if (++tries == 8) break;
        }
    *why = !faults.isEmpty() ? faults.join(QStringLiteral("; "))
                             : tr("every way from %1, %2 to %3, %4 goes over another pin or wire")
                                   .arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
    return false;
}

// Joins again the nets of \a before that are in pieces now, each piece
// to the closest of the others by a wire (wireUp()) that joins nothing
// else - \a check says what that would join. False, and why in \a why,
// when a piece cannot be reached. \a edited is the part "@" is, \a probes
// the places before has.
bool joinPieces(Schematic* sch, const Nets& before, const QString& edited, const QList<QPoint>& probes,
                const std::function<QStringList()>& check, QString* why)
{
    for (int round = 0; round < 64; ++round) {
        const Nets now = netsOf(sch, sch->getComponentByName(edited), probes);
        // A net of before in pieces: the pieces' nodes.
        QHash<int, QSet<int>> pieces;   // net before -> nets now
        for (auto it = before.netOf.constBegin(); it != before.netOf.constEnd(); ++it)
            if (now.netOf.contains(it.key())) pieces[it.value()].insert(now.netOf.value(it.key()));
        QList<int> split;
        for (auto it = pieces.constBegin(); it != pieces.constEnd(); ++it)
            if (it.value().size() > 1) split << it.key();
        if (split.isEmpty()) return true;
        std::sort(split.begin(), split.end());
        const QSet<int> parts = pieces.value(split.first());
        QHash<int, QList<QPoint>> places;   // a piece -> its nodes' places
        for (const Node* n : sch->a_DocNodes) {
            const int piece = now.nodeNet.value(n, -1);
            if (parts.contains(piece)) places[piece] << n->center();
        }
        for (const QPoint& p : probes) {   // (a place with nothing there now)
            const int piece = now.netOf.value(QStringLiteral("at %1,%2").arg(p.x()).arg(p.y()), -1);
            if (parts.contains(piece) && !places.value(piece).contains(p)) places[piece] << p;
        }
        // The two closest places of two pieces.
        QPoint from, to;
        qint64 best = -1;
        const QList<int> ids = places.keys();
        for (int i = 0; i < ids.size(); ++i)
            for (int j = i + 1; j < ids.size(); ++j)
                for (const QPoint& p : places.value(ids.at(i)))
                    for (const QPoint& q : places.value(ids.at(j))) {
                        const qint64 d = qAbs(qint64(p.x()) - q.x()) + qAbs(qint64(p.y()) - q.y());
                        if (best < 0 || d < best) {
                            best = d;
                            from = p;
                            to = q;
                        }
                    }
        if (best < 0) {
            *why = tr("a net is in pieces with nothing to wire");
            return false;
        }
        if (!wireUp(sch, from, to, check, why)) return false;
    }
    *why = tr("its nets could not be joined again");
    return false;
}

// Turns, mirrors and moves the part \a name of \a sch as \a args say,
// keeping every net as it was: taken off its nodes, changed; the wires of
// other nets under its pins' new places taken up (not labelled ones); put
// down; and every net that is in pieces then - its own, those taken up -
// joined again by wires that join nothing else (joinPieces()). A net label
// on a pin alone goes with it. Null, and why in \a why, when the nets
// cannot be kept: \a sch is then part way, for the caller to put back.
Component* turnAndMove(Schematic* sch, const QString& name, const QJsonObject& args, QString* why, QStringList* landed)
{
    Component* c = sch->getComponentByName(name);
    const Nets before = netsOf(sch, c);
    // The wires' open ends as they are: those the change leaves (where a
    // pin was, where a wire was taken up) are taken away after.
    QSet<std::pair<int, int>> openEnds;
    for (const Node* n : sch->a_DocNodes)
        if (n->conn_count() == 1) openEnds.insert({n->cx, n->cy});
    std::vector<QPoint> placeOf;
    std::vector<std::unique_ptr<WireLabel>> labels;
    for (Port* p : c->Ports) {
        Node* n = p->Connection;
        placeOf.push_back(n != nullptr ? n->center() : c->center() + QPoint(p->x, p->y));
        labels.push_back(n != nullptr && n->conn_count() == 1 ? n->releaseLabel() : nullptr);
    }
    sch->detachComp(c);
    for (Port* p : c->Ports) p->Connection = nullptr;   // (a turn or move moves a port's node)

    if (args.contains(QLatin1String("mirror")) && args.value(QLatin1String("mirror")).toBool() != c->mirroredX) c->mirrorX();
    if (args.contains(QLatin1String("rotation"))) {
        const int want = ((args.value(QLatin1String("rotation")).toInt() % 4) + 4) % 4;
        for (int i = 0; i < 4 && c->rotated != want; ++i) c->rotate();
    }
    if (args.contains(QLatin1String("x")) || args.contains(QLatin1String("y"))) {
        int x = args.contains(QLatin1String("x")) ? args.value(QLatin1String("x")).toInt() : c->cx;
        int y = args.contains(QLatin1String("y")) ? args.value(QLatin1String("y")).toInt() : c->cy;
        const QPoint at = Schematic::withinModelLimit(QPoint(x, y));
        x = at.x();
        y = at.y();
        sch->setOnGrid(x, y);
        c->moveCenter(x - c->cx, y - c->cy);
    }
    // What another net has under a pin's new place is moved out of the
    // way: its wire taken up here, its pieces joined again below.
    std::vector<Wire*> doomed;
    for (int i = 0; i < c->Ports.size(); ++i) {
        const QPoint p = c->center() + QPoint(c->Ports.at(i)->x, c->Ports.at(i)->y);
        const int mine = before.netOf.value(QStringLiteral("@.%1").arg(i + 1), -1);
        for (Wire* w : sch->a_DocWires)
            if (!w->hasLabel() && onSegment(p, w->P1(), w->P2()) && before.nodeNet.value(w->Port1, -2) != mine
                && std::find(doomed.begin(), doomed.end(), w) == doomed.end())
                doomed.push_back(w);
    }
    // A net with no pin or label on it - wires only - is kept by its
    // wires' ends: they are to be joined again, but where a pin comes.
    // (Those of others are kept by what is on them.)
    QSet<int> keyed;
    for (auto it = before.netOf.constBegin(); it != before.netOf.constEnd(); ++it) keyed.insert(it.value());
    QList<QPoint> pinsAt, ends;
    for (const Port* p : c->Ports) pinsAt << c->center() + QPoint(p->x, p->y);
    Nets was = before;
    for (const Wire* w : doomed)
        for (const Node* n : {w->Port1, w->Port2}) {
            if (keyed.contains(before.nodeNet.value(n))) continue;
            const QPoint e = n->center();
            if (pinsAt.contains(e) || ends.contains(e)) continue;
            ends << e;
            was.netOf.insert(QStringLiteral("at %1,%2").arg(e.x()).arg(e.y()), before.nodeNet.value(n));
        }
    sch->deleteWires(doomed);
    sch->insertRawComponent(c, false);
    for (int i = 0; i < c->Ports.size(); ++i) {
        Node* n = c->Ports.at(i)->Connection;
        if (labels[i] == nullptr || n->hasLabel()) continue;   // (one there already: the check tells)
        const QPoint d = n->center() - placeOf[i];
        labels[i]->moveRoot(d.x(), d.y());
        labels[i]->moveCenter(d.x(), d.y());
        n->acquireLabel(std::move(labels[i]));
    }
    const auto check = [&] { return netChanges(was, netsOf(sch, sch->getComponentByName(name), ends), name, true, nullptr); };
    QStringList changes = check();
    if (!changes.isEmpty()) {
        *why = changes.join(QStringLiteral("; "));
        return nullptr;
    }
    QString fault;
    if (!joinPieces(sch, was, name, ends, check, &fault)) {
        *why = tr("not every net could be wired together again (%1)").arg(fault);
        return nullptr;
    }
    // The ends left open: a wire to nothing, taken away (and the one it
    // came from, when that is left open in turn). A net is no different
    // for it.
    for (bool more = true; more;) {
        more = false;
        std::vector<Wire*> loose;
        for (Wire* w : sch->a_DocWires) {
            if (w->hasLabel()) continue;
            for (const Node* n : {w->Port1, w->Port2})
                if (n->conn_count() == 1 && !n->hasLabel() && !openEnds.contains({n->cx, n->cy}) && !ends.contains(n->center())) {
                    loose.push_back(w);
                    break;
                }
        }
        if (!loose.empty()) {
            sch->deleteWires(loose);
            more = true;
        }
    }
    c = sch->getComponentByName(name);
    changes = netChanges(was, netsOf(sch, c, ends), name, false, landed);
    if (!changes.isEmpty()) {
        *why = changes.join(QStringLiteral("; "));
        return nullptr;
    }
    return c;
}

} // namespace

// ----------------------------------------------------------------------
namespace {

// Whether a net of \a sch is called \a name (a label has it).
bool netNamed(Schematic* sch, const QString& name)
{
    for (Wire* w : sch->a_DocWires)
        if (w->hasLabel() && w->label()->Name == name) return true;
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel() && n->label()->Name == name) return true;
    return false;
}

// The traces of \a docs that show the voltage of the net \a name.
QStringList tracesNaming(const QList<Schematic*>& docs, const QString& name)
{
    QStringList list;
    const QString probe = QStringLiteral("\x01");
    for (Schematic* doc : docs) {
        int n = 0;
        for (Diagram* d : doc->a_DocDiags) {
            ++n;
            for (Graph* g : d->Graphs)
                if (renameNetIn(g->Var, name, probe) != g->Var)
                    list << QStringLiteral("%1 (%2, diagram %3)").arg(g->Var, QFileInfo(doc->getDocName()).fileName()).arg(n);
        }
    }
    return list;
}

} // namespace

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
        const int diagrams = int(a.value(QLatin1String("diagrams")).toArray().size());
        if (diagrams > 0) what << (diagrams == 1 ? tr("a diagram") : tr("%1 diagrams").arg(diagrams));
        const int traces = int(a.value(QLatin1String("traces")).toArray().size());
        if (traces > 0) what << (traces == 1 ? tr("a trace") : tr("%1 traces").arg(traces));
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
    else if (tool == QLatin1String("simulate") || tool == QLatin1String("get_netlist") || tool == QLatin1String("reload_data")) subject = s("path");
    else if (tool == QLatin1String("get_dataset")) {
        QStringList vars;
        for (const QJsonValue& v : a.value(QLatin1String("variables")).toArray()) vars << v.toString();
        subject = vars.isEmpty() ? s("path") : vars.join(QStringLiteral(", "));
    } else if (tool == QLatin1String("add_diagram")) {
        QStringList vars;
        for (const QJsonValue& v : a.value(QLatin1String("traces")).toArray())
            vars << (v.isString() ? v.toString() : v.toObject().value(QLatin1String("variable")).toString());
        subject = (s("type").isEmpty() ? QStringLiteral("rect") : s("type")) + (vars.isEmpty() ? QString() : QStringLiteral(": ") + vars.join(QStringLiteral(", ")));
    } else if (tool == QLatin1String("edit_diagram")) subject = tr("diagram %1").arg(a.value(QLatin1String("diagram")).toInt(1));
    else if (tool == QLatin1String("add_trace")) subject = s("variable");
    else if (tool == QLatin1String("edit_trace"))
        subject = a.value(QLatin1String("trace")).isString() ? s("trace") : tr("trace %1").arg(a.value(QLatin1String("trace")).toInt(1));
    else if (tool == QLatin1String("rename_net")) subject = s("from") + QStringLiteral(" → ") + s("to");
    else if (tool == QLatin1String("describe_component_type")) subject = s("type");
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
        "list_component_types gives add_component's types, describe_component_type a type's properties. "
        "rename_net renames a net with the traces that show it. Menus: list_actions, trigger_action; a dialog it opens "
        "is read with get_dialog and answered with set_dialog. simulate runs the simulator and reports its errors; "
        "get_netlist gives the netlist. get_dataset reads the results as numbers and measures them (rise time, "
        "overshoot, values at a time, ...): use it rather than a screenshot to tell what a simulation gave. "
        "Diagrams: add_diagram, edit_diagram, add_trace, edit_trace, delete; reload_data reads the data again. Changes "
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
    if (tool == QLatin1String("get_netlist")) return getNetlist(args);
    if (tool == QLatin1String("get_dataset")) return getDataset(args);
    if (tool == QLatin1String("reload_data")) return reloadData(args);
    if (tool == QLatin1String("add_diagram")) return addDiagram(args);
    if (tool == QLatin1String("edit_diagram")) return editDiagram(args);
    if (tool == QLatin1String("add_trace")) return addTrace(args);
    if (tool == QLatin1String("edit_trace")) return editTrace(args);
    if (tool == QLatin1String("rename_net")) return renameNet(args);
    if (tool == QLatin1String("describe_component_type")) return describeComponentType(args);
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

    QJsonArray components, wires, labels, diagrams, paintings, netList;
    // The nets: each pin's by name - its label's, gnd, or net1, net2, ...
    // in the order the parts come - and the pins on each.
    const Nets nets = netsOf(sch, nullptr);
    QHash<int, QString> netNames;
    QStringList keys = nets.netOf.keys();
    keys.sort();
    for (const QString& key : keys) {
        const int id = nets.netOf.value(key);
        if (key == QLatin1String("ground")) netNames.insert(id, QStringLiteral("gnd"));
        else if (key.startsWith(QLatin1String("label ")) && !netNames.contains(id)) netNames.insert(id, key.mid(6));
    }
    QHash<int, QStringList> pinsOn;
    QList<int> order;
    QHash<QString, int> seen;
    for (Component* c : sch->a_DocComps) {
        QJsonObject json = componentJson(c);
        QJsonArray pins = json.value(QStringLiteral("pins")).toArray();
        const QString base = keyBase(c, seen);
        for (int i = 0; i < pins.size(); ++i) {
            const auto it = nets.netOf.constFind(base + QLatin1Char('.') + QString::number(i + 1));
            if (it == nets.netOf.constEnd()) continue;
            if (!netNames.contains(*it)) netNames.insert(*it, QStringLiteral("net%1").arg(order.size() + 1));
            if (!order.contains(*it)) order << *it;
            QJsonObject pin = pins.at(i).toObject();
            pin.insert(QStringLiteral("net"), netNames.value(*it));
            pins[i] = pin;
            pinsOn[*it] << QStringLiteral("%1.%2").arg(c->Name.isEmpty() ? c->Model : c->Name).arg(i + 1);
        }
        json.insert(QStringLiteral("pins"), pins);
        components.append(json);
    }
    for (int id : order)
        if (pinsOn.value(id).size() > 1 || !netNames.value(id).startsWith(QLatin1String("net")))
            netList.append(QJsonObject{{QStringLiteral("net"), netNames.value(id)}, {QStringLiteral("pins"), QJsonArray::fromStringList(pinsOn.value(id))}});
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
    diagrams = diagramsJson(sch);
    for (Painting* p : sch->a_DocPaints) paintings.append(p->Name.trimmed());
    return jsonResult(QJsonObject{{QStringLiteral("document"), titleOf(sch)},
                                  {QStringLiteral("path"), sch->getDocName()},
                                  {QStringLiteral("grid"), QJsonArray{sch->getGridX(), sch->getGridY()}},
                                  {QStringLiteral("components"), components},
                                  {QStringLiteral("nets"), netList},
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
    Component* c = newComponent(type);
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
    int named = 0;
    for (Component* pc : sch->a_DocComps) named += pc->Name.compare(name, Qt::CaseInsensitive) == 0 ? 1 : 0;
    if (named > 1) return errorResult(tr("There are %1 components named %2 in %3.").arg(named).arg(name, titleOf(sch)));
    const QString rename = args.value(QLatin1String("rename")).toString().trimmed();
    if (!rename.isEmpty() && rename != name && sch->getComponentByName(rename) != nullptr)
        return errorResult(tr("There is a component named %1 already.").arg(rename));
    // Check the properties before anything changes.
    const QJsonObject props = args.value(QLatin1String("properties")).toObject();
    for (auto it = props.begin(); it != props.end(); ++it)
        if (c->getProperty(it.key()) == nullptr)
            return errorResult(tr("%1 has no property %2; its properties are: %3.").arg(name, it.key(), propertyNames(c)));
    prepare(sch);
    const QString before = sch->snapshot();
    if (!props.isEmpty()) {
        setProperties(c, props, &error);
        sch->recreateComponent(c);
    }
    QStringList landed;
    if (args.contains(QLatin1String("x")) || args.contains(QLatin1String("y")) || args.contains(QLatin1String("rotation"))
        || args.contains(QLatin1String("mirror"))) {
        QString why;
        c = turnAndMove(sch, name, args, &why, &landed);
        if (c == nullptr) {
            sch->restore(before);
            return errorResult(tr("%1 is not changed: turned or moved so, %2. Try another place or turn - or take "
                                  "its wires away (delete), change it and connect it again.")
                                   .arg(name, why));
        }
    }
    if (args.contains(QLatin1String("active")))
        c->isActive = args.value(QLatin1String("active")).toBool() ? COMP_IS_ACTIVE : COMP_IS_OPEN;
    if (!rename.isEmpty()) c->Name = rename;
    sch->enlargeView(c);
    finish(sch, {QPoint(c->cx, c->cy)});
    QJsonObject result = componentJson(c);
    if (!landed.isEmpty()) result.insert(QStringLiteral("note"), landed.join(QStringLiteral("; ")) + QLatin1Char('.'));
    return jsonResult(result);
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
    // Traces, then diagrams (by their numbers before any goes).
    QList<QPair<Diagram*, Graph*>> traces;
    for (const QJsonValue& v : args.value(QLatin1String("traces")).toArray()) {
        const QJsonObject t = v.toObject();
        Diagram* d = diagramOf(sch, t.value(QLatin1String("diagram")), &error);
        Graph* g = d != nullptr ? traceOf(d, t.value(QLatin1String("trace")), &error) : nullptr;
        if (g == nullptr) return errorResult(error);
        if (!traces.contains(qMakePair(d, g))) traces << qMakePair(d, g);
    }
    QList<Diagram*> diagrams;
    for (const QJsonValue& v : args.value(QLatin1String("diagrams")).toArray()) {
        Diagram* d = diagramOf(sch, v, &error);
        if (d == nullptr) return errorResult(error);
        if (!diagrams.contains(d)) diagrams << d;
    }
    if (doomed.isEmpty() && unlabelled.isEmpty() && traces.isEmpty() && diagrams.isEmpty())
        return errorResult(missing.isEmpty() ? tr("Nothing to delete.") : tr("Not found: %1.").arg(missing.join(QStringLiteral(", "))));
    prepare(sch);
    for (const auto& [d, g] : std::as_const(traces)) {
        if (diagrams.contains(d)) continue;
        done << tr("the trace %1").arg(g->Var);
        d->Graphs.removeOne(g);
        delete g;
        d->recalcGraphData();
    }
    for (Diagram* d : std::as_const(diagrams)) {
        done << tr("a %1 diagram").arg(d->Name);
        doomed << d;
    }
    for (Conductor* c : unlabelled) c->dropLabel();
    // Deleting records its step to undo itself: then not a second one.
    bool recorded = false;
    if (!doomed.isEmpty()) {
        sch->deselectElements(nullptr);
        for (Element* e : doomed) e->isSelected = true;
        recorded = sch->deleteElements();
    }
    if (recorded) sch->viewport()->update();
    else finish(sch);
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
    const QString from = args.value(QLatin1String("from")).isString() ? args.value(QLatin1String("from")).toString()
                                                                     : QStringLiteral("%1, %2").arg(a.x()).arg(a.y());
    const QString to = args.value(QLatin1String("to")).isString() ? args.value(QLatin1String("to")).toString()
                                                                 : QStringLiteral("%1, %2").arg(b.x()).arg(b.y());
    const Nets before = netsOf(sch, nullptr, {a, b});
    const QStringList ends{QStringLiteral("at %1,%2").arg(a.x()).arg(a.y()), QStringLiteral("at %1,%2").arg(b.x()).arg(b.y())};
    if (before.netOf.value(ends.at(0)) == before.netOf.value(ends.at(1)))
        return textResult(tr("%1 and %2 are on one net already: nothing drawn.").arg(from, to));
    prepare(sch);
    // A wire that joins these two nets and nothing else: one going over a
    // pin on its way would join that pin's net too.
    const auto check = [&] { return netChangesBeyond(before, netsOf(sch, nullptr, {a, b}), ends); };
    if (!wireUp(sch, a, b, check, &error))
        return errorResult(tr("Not wired: %1. Give the way with add_wire, or move a part out of it.").arg(error));
    finish(sch, {a, b});
    return textResult(tr("Wired %1 to %2 (%3, %4 to %5, %6), going over no other pin or wire.")
                          .arg(from, to).arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y()));
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
    // What is at its places is joined; what it goes over between them must
    // not be.
    const Nets before = netsOf(sch, nullptr, points);
    QStringList places;
    for (const QPoint& p : points) places << QStringLiteral("at %1,%2").arg(p.x()).arg(p.y());
    prepare(sch);
    const QString state = sch->snapshot();
    for (int i = 1; i < points.size(); ++i)
        if (points.at(i) != points.at(i - 1)) sch->connectWithWire(points.at(i - 1), points.at(i));
    const QStringList faults = netChangesBeyond(before, netsOf(sch, nullptr, points), places);
    if (!faults.isEmpty()) {
        sch->restore(state);
        return errorResult(tr("Not drawn: it goes over what is not at its places - %1. Give places that go around "
                              "them (or use connect, which finds a way).")
                               .arg(faults.join(QStringLiteral("; "))));
    }
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
    const QString before = labelled != nullptr && static_cast<Conductor*>(labelled)->hasLabel()
                               ? static_cast<Conductor*>(labelled)->label()->Name : QString();
    prepare(sch);
    if (labelled != nullptr) static_cast<Conductor*>(labelled)->dropLabel();
    if (!name.isEmpty()) {
        int xl = p.x() + 30, yl = p.y() - 30;
        sch->setOnGrid(xl, yl);
        if (wire != nullptr) wire->setName(name, QString(), p.x(), p.y(), xl, yl);
        else node->setName(name, QString(), xl, yl);
    }
    finish(sch, {p});
    QString text = name.isEmpty() ? tr("The label at %1, %2 is gone.").arg(p.x()).arg(p.y())
                                  : tr("The net at %1, %2 is %3.").arg(p.x()).arg(p.y()).arg(name);
    // Traces of the name the net had, when no net has it now.
    if (!before.isEmpty() && before != name && !netNamed(sch, before)) {
        const QStringList orphans = tracesNaming(showingDataOf(sch), before);
        if (!orphans.isEmpty())
            text += QLatin1Char(' ') + tr("No net is called %1 now, but %2 still show its voltage: they will show nothing after "
                                          "the next simulation (rename_net renames a net with its traces; edit_trace changes one).")
                                           .arg(before, orphans.join(QStringLiteral(", ")));
    }
    return textResult(text);
}

QJsonObject QucsControl::renameNet(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString from = args.value(QLatin1String("from")).toString().trimmed();
    const QString to = args.value(QLatin1String("to")).toString().trimmed();
    if (from.isEmpty() || to.isEmpty()) return errorResult(tr("Which net, and its new name? ('from', 'to')"));
    if (to.contains(QRegularExpression(QStringLiteral("[\\s,;()\\[\\]{}\"'=]"))))
        return errorResult(tr("%1 cannot name a net: no spaces, commas, brackets, quotes or '='.").arg(to));
    if (from == to) return textResult(tr("The net is called %1 already.").arg(to));
    if (netNamed(sch, to))
        return errorResult(tr("A net is called %1 already: renaming %2 so would join the two nets (set_label joins nets on purpose).").arg(to, from));

    QList<Conductor*> labels;
    for (Wire* w : sch->a_DocWires)
        if (w->hasLabel() && w->label()->Name == from) labels << w;
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel() && n->label()->Name == from) labels << n;
    // A net without a label, as get_schematic calls it: net1, net2, ...
    QPoint pinAt;
    bool unnamed = false;
    if (labels.isEmpty()) {
        static const QRegularExpression auto_(QStringLiteral("^net(\\d+)$"));
        const QRegularExpressionMatch m = auto_.match(from);
        if (m.hasMatch()) {
            const Nets nets = netsOf(sch, nullptr);
            QList<int> order;
            QHash<QString, int> seen;
            for (Component* c : sch->a_DocComps) {
                const QString base = keyBase(c, seen);
                for (int i = 0; i < c->Ports.size(); ++i) {
                    const auto it = nets.netOf.constFind(base + QLatin1Char('.') + QString::number(i + 1));
                    if (it == nets.netOf.constEnd() || order.contains(*it)) continue;
                    // A net with a name is not numbered.
                    bool named = false;
                    for (auto k = nets.netOf.constBegin(); k != nets.netOf.constEnd() && !named; ++k)
                        if (k.value() == *it && (k.key() == QLatin1String("ground") || k.key().startsWith(QLatin1String("label ")))) named = true;
                    order << *it;
                    if (!named && order.size() == m.captured(1).toInt() && !unnamed) {
                        pinAt = QPoint(c->cx + c->Ports.at(i)->x, c->cy + c->Ports.at(i)->y);
                        unnamed = true;
                    }
                }
            }
        }
        if (!unnamed) return errorResult(tr("No net is called %1 (get_schematic lists the nets).").arg(from));
    }

    prepare(sch);
    for (Conductor* c : std::as_const(labels)) c->label()->setName(to);
    if (unnamed) {
        Node* node = sch->findNode(pinAt);
        if (node == nullptr) return errorResult(tr("The net %1 has no place to label.").arg(from));
        int xl = pinAt.x() + 30, yl = pinAt.y() - 30;
        sch->setOnGrid(xl, yl);
        node->setName(to, QString(), xl, yl);
    }
    // What names its voltage: traces, equations, a closed data display.
    QStringList changed;
    const QList<Schematic*> showing = showingDataOf(sch);
    for (Schematic* doc : showing) {
        int n = 0;
        bool any = false;
        for (Diagram* d : doc->a_DocDiags) {
            ++n;
            for (Graph* g : d->Graphs) {
                const QString renamed = renameNetIn(g->Var, from, to);
                if (renamed == g->Var) continue;
                changed << tr("%1, diagram %2: %3 is %4").arg(titleOf(doc)).arg(n).arg(g->Var, renamed);
                g->Var = renamed;
                g->lastLoaded = QDateTime();
                any = true;
            }
        }
        if (any && doc != sch) {
            doc->setChanged(true, true);
            doc->reloadGraphs();
            doc->viewport()->update();
        }
    }
    for (Component* c : sch->a_DocComps) {
        if (!c->isEquation) continue;
        for (Property* p : c->Props) {
            const QString renamed = renameNetIn(p->Value, from, to);
            if (renamed == p->Value) continue;
            changed << tr("%1: %2 is %3").arg(c->Name, p->Name + QLatin1Char('=') + p->Value, renamed);
            p->Value = renamed;
        }
    }
    QString rewritten;
    if (!sch->getDocName().isEmpty() && !sch->getDataDisplay().isEmpty()) {
        const QString dpl = QFileInfo(sch->getDocName()).absoluteDir().filePath(sch->getDataDisplay());
        bool open = false;
        for (Schematic* doc : showing)
            if (sameFile(doc->getDocName(), dpl)) open = true;
        QFile file(dpl);
        if (!open && file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
            file.close();
            int count = 0;
            for (QString& line : lines) {
                // A trace: <"ngspice/tran.v(out)" #0000ff 2 3 0 0 0>
                const QString t = line.trimmed();
                if (!t.startsWith(QLatin1String("<\""))) continue;
                const QString var = t.section(QLatin1Char('"'), 1, 1);
                const QString renamed = renameNetIn(var, from, to);
                if (renamed == var) continue;
                line.replace(QLatin1Char('"') + var + QLatin1Char('"'), QLatin1Char('"') + renamed + QLatin1Char('"'));
                ++count;
            }
            if (count > 0 && file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                file.write(lines.join(QLatin1Char('\n')).toUtf8());
                rewritten = tr("%1 (not open) was rewritten: %2 traces renamed.").arg(QFileInfo(dpl).fileName()).arg(count);
            }
        }
    }
    finish(sch, unnamed ? QList<QPoint>{pinAt} : QList<QPoint>{});
    sch->reloadGraphs();

    QString text = unnamed ? tr("The net %1 is labelled %2.").arg(from, to)
                           : (labels.size() == 1 ? tr("The net %1 is %2 now (its label).").arg(from, to)
                                                 : tr("The net %1 is %2 now (its %3 labels).").arg(from, to).arg(labels.size()));
    if (!changed.isEmpty()) text += QLatin1Char(' ') + tr("Renamed too: %1.").arg(changed.join(QStringLiteral("; ")));
    if (!rewritten.isEmpty()) text += QLatin1Char(' ') + rewritten;
    if (!changed.isEmpty() || !rewritten.isEmpty())
        text += QLatin1Char(' ') + tr("The dataset still calls it %1: the traces show it again after the next simulation.").arg(from);
    return textResult(text);
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
    QJsonArray list;
    for (Category* category : Category::Categories) {
        for (Module* m : category->Content) {
            const QString type = typeOf(m);   // the equation blocks too
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
    const QString keepAs = args.value(QLatin1String("keep_as")).toString().trimmed();
    if (!keepAs.isEmpty() && !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,64}$")).match(keepAs).hasMatch()) {
        done(errorResult(tr("'keep_as' is a name of letters, digits, _ and -.")));
        return;
    }
    QPointer<Schematic> doc(sch);
    // A dataset written since now: the simulator did produce something.
    const QDateTime started = QDateTime::currentDateTime().addSecs(-1);
    const int simulator = QucsSettings.DefaultSimulator;
    // Started from the event loop (the legacy window runs one of its
    // own); the run it made is watched after.
    QTimer::singleShot(0, a_app, [app = a_app] { app->slotSimulateWithSpice(); });
    QTimer::singleShot(0, this, [this, done, timeout, doc, console, started, simulator, keepAs] {
        SimulationRun* run = console->currentRun();
        if (run == nullptr) {
            QStringList log;
            for (int i = 0; i < console->statusLog()->count(); ++i) log << console->statusLog()->item(i)->text();
            done(errorResult(tr("The simulation did not start. %1").arg(log.join(QLatin1Char('\n')))));
            return;
        }
        auto answered = std::make_shared<bool>(false);
        auto report = [this, done, answered, doc, console, started, simulator, keepAs](SimulationRun* r, bool timedOut) {
            if (*answered) return;
            *answered = true;
            const QString output = console->console()->toPlainText();
            QStringList lines = output.split(QLatin1Char('\n'));
            while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
            QJsonObject result{{QStringLiteral("finished"), !timedOut},
                               {QStringLiteral("simulator"), spicecompat::getDefaultSimulatorName(simulator)},
                               {QStringLiteral("last lines"), lines.mid(std::max<qsizetype>(0, lines.size() - 40)).join(QLatin1Char('\n'))}};
            // The errors and warnings, each with its netlist line and part.
            QStringList netlist;
            QHash<QString, QString> parts;
            if (doc) {
                QFile file(QDir(misc::scratchDirFor(doc->getDocName())).filePath(QStringLiteral("spice4qucs.cir")));
                if (simulator != spicecompat::simXyce && file.open(QIODevice::ReadOnly | QIODevice::Text))
                    netlist = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
                for (Component* c : doc->a_DocComps) {
                    if (c->Name.isEmpty()) continue;
                    QString name = c->Name, model = c->SpiceModel;
                    parts.insert(c->Name.toLower(), c->Name);
                    if (!model.isEmpty() && !model.startsWith(QLatin1Char('.'))) parts.insert(spicecompat::check_refdes(name, model).toLower(), c->Name);
                }
            }
            QJsonArray errors, warnings;
            for (const QJsonValue& p : qucs_s::simlog::toJson(qucs_s::simlog::problems(output, netlist, parts))) {
                QJsonObject o = p.toObject();
                const bool isError = o.take(QStringLiteral("severity")).toString() == QLatin1String("error");
                QJsonArray& list = isError ? errors : warnings;
                if (list.size() < 40) list.append(o);
            }
            result.insert(QStringLiteral("errors"), errors);
            result.insert(QStringLiteral("warnings"), warnings);
            bool written = false;
            if (doc) {
                // Where this simulator writes it: name.dat.ngspice, ...
                const QFileInfo info(doc->getDocName());
                const QFileInfo dataset(datasetFile(info.absoluteFilePath(), info.completeBaseName() + QStringLiteral(".dat"), simulator));
                written = dataset.isFile() && dataset.lastModified() >= started;
                result.insert(QStringLiteral("dataset"), QDir::toNativeSeparators(dataset.absoluteFilePath()));
                result.insert(QStringLiteral("dataset written"), written);
                if (written) {
                    // What it holds (nothing, when the simulator made no output).
                    qucs_s::dataset::Dataset data;
                    QJsonArray names;
                    if (data.read(dataset.absoluteFilePath()))
                        for (const auto& v : data.variables())
                            if (!v.independent && names.size() < 40) names.append(v.name);
                    result.insert(QStringLiteral("variables"), names);
                    // A copy to compare with later runs: name.dat.ngspice.
                    if (!keepAs.isEmpty()) {
                        const QString suffix = dataset.fileName().mid(dataset.fileName().indexOf(QLatin1String(".dat")));
                        const QString kept = info.absoluteDir().filePath(keepAs + suffix);
                        QFile::remove(kept);
                        if (QFile::copy(dataset.absoluteFilePath(), kept)) {
                            const QString prefix = simulatorPrefix();
                            result.insert(QStringLiteral("kept as"), QDir::toNativeSeparators(kept));
                            result.insert(QStringLiteral("its traces"), (prefix.isEmpty() ? QString() : prefix + QLatin1Char('/'))
                                                                            + keepAs + QStringLiteral(":<variable>"));
                        } else {
                            result.insert(QStringLiteral("kept as"), tr("not kept: %1 could not be written").arg(QDir::toNativeSeparators(kept)));
                        }
                    }
                }
                result.insert(QStringLiteral("data display"), QDir::toNativeSeparators(info.absoluteDir().filePath(doc->getDataDisplay())));
                // The traces that show nothing (the documents are reloaded
                // by now: QucsApp::slotAfterSpiceSimulation() ran first).
                QJsonArray blank;
                for (Schematic* shown : showingDataOf(doc)) {
                    shown->reloadGraphs();
                    int n = 0;
                    for (Diagram* d : shown->a_DocDiags) {
                        ++n;
                        for (Graph* g : d->Graphs)
                            if (g->isEmpty() && blank.size() < 40)
                                blank.append(QJsonObject{{QStringLiteral("document"), titleOf(shown)}, {QStringLiteral("diagram"), n},
                                                         {QStringLiteral("trace"), g->Var}, {QStringLiteral("why"), whyNoData(shown, g)}});
                    }
                }
                if (!blank.isEmpty()) result.insert(QStringLiteral("traces without data"), blank);
            }
            if (r != nullptr && !timedOut) {
                // The simulator's own word: it ran to its end, reported no
                // error and exited so (the dataset's name is its affair).
                const bool succeeded = r->wasSimulated() && !r->hasError() && r->exitCode() == 0;
                result.insert(QStringLiteral("succeeded"), succeeded);
                result.insert(QStringLiteral("stopped"), r->wasStopped());
                result.insert(QStringLiteral("exit code"), r->exitCode());
                if (r->exitCode() != 0 && errors.isEmpty())
                    errors.append(QJsonObject{{QStringLiteral("message"),
                                               r->exitCode() < 0 ? tr("The simulator crashed or did not start.")
                                                                 : tr("The simulator ended with exit code %1.").arg(r->exitCode())}});
                result.insert(QStringLiteral("errors"), errors);
                if (succeeded && !written)
                    result.insert(QStringLiteral("note"), tr("The simulator reported no error but wrote no new dataset: its analyses may "
                                                             "save nothing (an operating point alone), or a post-processing step failed."));
                if (!succeeded && errors.isEmpty() && !r->wasStopped())
                    result.insert(QStringLiteral("note"), tr("The simulator failed without an error message of its own: see the last lines."));
            }
            if (timedOut) result.insert(QStringLiteral("note"), tr("Still running: its end was not waited for any longer."));
            done(jsonResult(result));
        };
        connect(run, &SimulationRun::simulated, this, [report](SimulationRun* r) { report(r, false); });
        QTimer::singleShot(timeout, this, [report] { report(nullptr, true); });
    });
}
