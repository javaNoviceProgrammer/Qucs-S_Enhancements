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
#include "valuereading.h"
#include "qucscontrol_p.h"

#include "components/component.h"
#include "components/libcomp.h"
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
#include "dataimport.h"
#include "dataset.h"
#include "erc.h"
#include "ngoptimize.h"
#include "mcpserver.h"
#include "dialogs/simmessage.h"
#include "ngstatistics.h"
#include "ngsweep.h"
#include "textdoc.h"
#include "textplacement.h"
#include "vamodule.h"
#include "wire.h"
#include "wirelabel.h"
#include "workspace.h"

#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QHeaderView>
#include <QListView>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTableView>
#include <QTreeView>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QDockWidget>
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
#include <QImageReader>
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
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <memory>
#include <optional>
#include <tuple>

namespace {

// The tools, as MCP's tools/list gives them.
const char* const kTools = R"JSON([
{"name": "get_state",
 "description": "Returns the state of the Qucs-S window. Panes: each pane's number, row, column, rectangle and documents, with the active one marked (move_to_pane rearranges them). Documents: path, title, kind, whether it has unsaved changes, whether it is in front, its pane, its revision (a counter of every edit, undo and reload, by anyone), who made the last edit (you, the user, another conversation, or its file changed on disk and loaded again) and when its dataset was last written. Also the simulator chosen in the settings, the workspace folder, the open project ('project': its name and folder, null when none is open; relative paths are resolved against its folder - one is open at a time), the workspace's projects ('projects', the open one marked; each document's 'project' is the one its file is in, and 'project not open' says when that is not the open one), any simulation in progress, the simulations followed in the background (their ids) and any dialog waiting for an answer. Start here. Every tool result also reports what changed since your last call that you did not do yourself, such as the user's edits or a simulation the user ran.",
 "inputSchema": {"type": "object", "properties": {}}},
{"name": "move_to_pane",
 "description": "Moves a document (the one in front unless 'path' names another) to another pane, to show documents side by side. 'pane' is a pane number from get_state, or \"right\" or \"below\" for a new pane next to the document's current one (at most two panes per row, and two rows). Returns the state with the panes.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "pane": {"description": "A pane's number, or \"right\" or \"below\""}}, "required": ["pane"]}},
{"name": "open_document",
 "description": "Opens a file in a Qucs-S tab, or brings it to the front if it is already open: a schematic (.sch), a symbol (.sym), a data display (.dpl), a netlist or any text file, or a PDF (shown in Qucs-S's own viewer, for example a datasheet or a report). A relative path is resolved against the open project's folder, otherwise the workspace folder; a bare file name finds an open document with that name. If a component line in a schematic has more values than its type has properties, the result says so: values are read in order, so one extra value shifts every value after it. A schematic whose dataset and data display are named after another file (copied outside Qucs-S) is pointed out: its runs and its diagrams would use that file; 'own_data_names' names them after this one.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The file"}, "own_data_names": {"type": "boolean", "description": "A schematic whose dataset and data display are named after another file (a copy made outside Qucs-S, which the answer points out): name them after this one, as the window offers when it opens such a file"}}, "required": ["path"]}},
{"name": "new_document",
 "description": "Creates a new, untitled document and brings it to the front: a schematic or a text document. An untitled schematic nothing was done in (the one Qucs-S opens at start) is closed, so there is one \"untitled\" to name. With kind data_display it opens a schematic's data display instead (its .dpl file, created if it has none; 'path' is the schematic, the one in front if not given). A data display holds diagrams and paintings for a report and keeps the schematic itself clean; export_image can write a picture of it.",
 "inputSchema": {"type": "object", "properties": {"kind": {"type": "string", "enum": ["schematic", "text", "data_display"], "description": "schematic (the default), text, or data_display: a schematic's data display (.dpl)"}, "path": {"type": "string", "description": "With data_display: the schematic whose data display opens; the one in front when not given"}}}},
{"name": "show_document",
 "description": "Brings an open document to the front.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "Its file, or its tab's title"}}, "required": ["path"]}},
{"name": "save_document",
 "description": "Saves a document (the one in front unless 'path' names another). With 'as' it saves under that file name, which the document keeps from then on; an untitled document needs 'as'. Saving over an existing file with 'as' needs 'replace', otherwise the user is asked. When a subcircuit is saved, its instances in open schematics take the new symbol: the result lists which were refreshed, each pin that moved, and whether it still meets its wiring.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "as": {"type": "string", "description": "A new file name"},
   "replace": {"type": "boolean", "description": "With 'as': write over a file that is there (else the user is asked, or it is refused)"}}}},
{"name": "close_document",
 "description": "Closes a document's tab (the one in front unless 'path' names another). For a document with unsaved changes, 'unsaved' says whether to save or discard them; without it the user is asked to save, discard or keep the document open.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "unsaved": {"type": "string", "enum": ["save", "discard"], "description": "For a document with unsaved changes: save them, or discard them; not given, the user is asked"}}}},
{"name": "get_schematic",
 "description": "Reads a schematic as it is in Qucs-S now, including unsaved changes. Format 'summary' (the default) lists: the components (name, type, position, rotation, mirroring, whether active, properties, each pin's position with whether anything is connected and its net - a label's name, gnd, or net1, net2, ... (numbered in order, so a label or a new part may renumber them: read again before using one) - and the boxes of its texts); the nets with the pins on each (those with two or more pins, or a name); the wires, net labels and paintings (numbered as the painting tools expect, each with its type and fields by name); the settings (dataset, data display, frame); a subcircuit's parameters, with their defaults ('subcircuit parameters', as set_subcircuit_parameters takes them); and the diagrams, numbered as the diagram tools expect, with their axes, traces (each trace's variable, style, and points or the reason it shows no data) and markers. 'properties' chooses which properties are listed: non_default (those shown or not at the type's default; the default), shown, or all. 'texts' gives each part's name and shown properties with their box [x1, y1, x2, y2], to move one clear of a wire or label with edit_component's text_at. 'components' (names; a ground by its ref, GND#2) or 'region' ([x1, y1, x2, y2]) limits the list to those components with their nets and wires - the nets named as in a full read, net1 being the same net in both; its net labels are those on their pins and wires (or in the region) - a label elsewhere on one of their nets is left out, its name given in 'nets'; a list longer than 200 is cut short and says what was left out. Read whole, a schematic of more than 200 parts lists the first 50 of each kind (parts, nets, wires, labels): read it in parts with 'region' or 'components', or at a glance with format 'overview'. 'symbol' also lists the paintings of its symbol (its ports and name text among them), as does a document that is showing its symbol. Format 'overview' summarizes it at a glance - parts counted by type, analyses, named nets, extent and diagrams - in a few hundred bytes even for thousands of parts: start there with a large schematic. Format 'text' returns the text its .sch file would have, and 'json' the parts and wires in the form set_schematic accepts. Coordinates are in schematic units; the grid is usually 10.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "format": {"type": "string", "enum": ["summary", "overview", "text", "json"], "description": "json: its parts and wires as set_schematic's 'components' and 'wires' take them back"}, "symbol": {"type": "boolean", "description": "Also list its symbol's paintings (its ports and name text among them)"},
   "properties": {"type": "string", "enum": ["non_default", "shown", "all"], "description": "Which properties each part lists: non_default (shown, or not at the type's default; the default), shown, or all"},
   "components": {"type": "array", "items": {"type": "string"}, "description": "Only these parts, by name - a ground by its ref (GND when there is one, GND#2 the second of several, as get_schematic gives it) - with their nets and wires; the nets keep the names a full read gives them, and names that match nothing come back under 'not found'"},
   "region": {"type": "array", "items": {"type": "integer"}, "minItems": 4, "maxItems": 4, "description": "Only the parts whose centre is in [x1, y1, x2, y2], with their nets and the wires in it"},
   "selection": {"type": "boolean", "description": "What the user selected: those components (or that region)"}}}},
{"name": "check_schematic",
 "description": "Checks a schematic for anything a simulation would fail on or do differently than intended, like Simulation > Check Schematic, and reports each finding with its location and part. Errors: no ground, two parts with the same name, a part the simulator cannot handle, voltage sources in parallel or shorted by a wire, and so on. Warnings: pins and wire ends connected to nothing; a wire end or a pin lying on another net's wire mid-segment (not connected: a wire connects only at its ends); wires of two nets on top of each other; parts not connected to any ground (floating); nets that reach ground only through capacitors or current sources (no DC path, so no operating point); an inductor across a source; a negative capacitance; an AC analysis with no AC source, or only ones of AC magnitude 0 (a Vac of U = 0); a NutmegEq's v(node) of no net; an equation's variable named as a net, which under ngspice writes over the node's voltage; no simulation block. Notes, fine if intended: wires of two nets crossing without a junction, a net label on a single pin, two labels on one net, a 0 Ohm part, a capacitor across a pulse source, an input, base or gate with no DC bias but through its own part, an op-amp loaded under 1 kOhm. A topology and netlist check: nothing found does not mean the circuit works. Use it after building or rewiring a circuit and before simulate; get_schematic's summary also counts these findings.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"},
   "subcircuits": {"type": "boolean", "description": "Also the findings inside each subcircuit it uses, at any depth, each with its file (without it, a line of counts for each)"}}}},
{"name": "set_schematic",
 "description": "Replaces the elements of a schematic in one undo step. Give either the JSON form - 'components' and 'wires', as get_schematic's format json returns them, with properties by name and checked against each type - or 'text': the text of a .sch file, or any of its <Components>, <Wires>, <Diagrams> and <Paintings> sections (sections left out stay as they are; <Properties> and <Symbol> are ignored). Diagrams re-read their data. Returns what it read in each section it replaced: the components by name and type, the number of wires, the diagrams as get_schematic lists them (each trace's points or why it has none, each marker and the sample it shows) and the paintings. If the text cannot be read, the schematic stays unchanged and the error is reported. The same happens for a component line with more values than its type has properties, because values are positional and one extra value in the middle puts every later value in the wrong property. A line with fewer values is accepted, the rest at their defaults, and the result says so. A number with letters after it that are no scale and unit (1kk, 10uu) is refused, and the schematic stays unchanged. Other values that do not fit their property - a word where a number belongs, a word that is not one of the property's choices - are listed under 'values'. To hide or show a property or move a part's text, use edit_component instead of rewriting the line. describe_format explains each line's fields. For diagrams, traces and markers, add_diagram, edit_diagram, add_trace, edit_trace, add_marker and edit_marker are simpler and safer.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "text": {"type": "string", "description": "The .sch lines (or 'components' and 'wires' instead)"},
   "components": {"type": "array", "items": {"type": "object", "properties": {"type": {"type": "string"}, "name": {"type": "string"}, "x": {"type": "integer"}, "y": {"type": "integer"}, "rotation": {"type": "integer"}, "mirror": {"type": "boolean"}, "properties": {"type": "object"}, "shown": {"type": "object"}, "name_shown": {"type": "boolean"}, "text_at": {"type": "array"}, "active": {"type": "boolean"}, "equations": {"type": "array"}, "replace_equations": {"type": "boolean"}, "flags": {"type": "array"}, "records": {"type": "array"}, "specs": {"type": "array"}}}, "description": "The JSON form, in place of <Components>: each part {\"type\": \"R\", \"name\": \"R1\", \"x\": 100, \"y\": 100, \"rotation\": 0-3, \"mirror\": false, \"properties\": {\"R\": \"1k\"}, \"shown\": {...}, \"equations\": [...], \"active\": true, \"text_at\": [dx, dy]} - properties by name, checked against the type, so no value can shift into another's place; get_schematic's format json gives them so"},
   "wires": {"type": "array", "items": {"type": "object", "properties": {"from": {"type": "array"}, "to": {"type": "array"}, "at": {"type": "array"}, "label": {"type": "string"}, "label_at": {"type": "array"}, "initial": {"type": "string"}}}, "description": "The JSON form, in place of <Wires>: {\"from\": [x, y], \"to\": [x, y], \"label\": \"out\"}, a label on a pin alone {\"at\": [x, y], \"label\": \"in\"}; a label's 'initial' is its net's initial value (.IC)"}}}},
{"name": "describe_format",
 "description": "Explains the lines of a .sch file field by field - a component, a wire, a diagram (all of its roughly 30 fields), a trace, a marker, a painting - as set_schematic accepts them and get_schematic's format 'text' returns them. For paintings it also lists the fields add_painting and edit_painting accept, type by type. Without 'element', it explains all of them.",
 "inputSchema": {"type": "object", "properties": {"element": {"type": "string", "enum": ["component", "wire", "diagram", "trace", "marker", "painting"], "description": "The kind of line explained (all of them when not given)"}}}},
{"name": "add_component",
 "description": "Places a library component at x, y (snapped to the grid). 'type' is its model - R, C, L, GND, Vdc, Vac, Idc, Iac, Diode, _BJT, _MOSFET, OpAmp, Sub, .DC, .AC, .TR, .SP and many more (list_component_types lists them all). Properties go by name, as get_schematic shows them, for example {\"R\": \"4.7k\"}. 'rotation' is in quarter turns from the type's own orientation, as Rotate turns it - or, for a part of two pins, 'pin1' says where pin 1 goes (top, bottom, left, right) and the turn is found; 'mirror' mirrors it about the x axis; 'shown' chooses which properties are written on the schematic; 'name_shown' and 'text_at' control its name and where its text goes. An equation block (Eqn, NutmegEq, .PARAM, .OPTIONS, ...) takes 'equations' - they replace its placeholder y=1; an .OPTIONS block takes options without a value (noinit) as 'flags'; an ngspice Monte Carlo (.NGMONTECARLO) or corners block takes 'records' and 'specs'. Returns the component with its pin positions, its equations if it is an equation block, a note on a common mistake with its type, and a note when a pin landed on a wire or another pin (it is then connected to it) or on another net's wire without connecting. Its text goes where its type puts it when that is clear, else to the nearest free spot beside it ('text' says where, or that none was free); 'text_at' puts it yourself.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "type": {"type": "string", "description": "Its model: R, C, L, GND, Vdc, Vac, Diode, _BJT, OpAmp, Sub, .TR, .AC, ... (list_component_types)"}, "x": {"type": "integer", "description": "Where its centre goes, in schematic units, on the grid (usually 10)"}, "y": {"type": "integer", "description": "Where its centre goes, in schematic units, on the grid (usually 10)"}, "near": {"type": "object", "properties": {"part": {"type": "string"}, "side": {"type": "string", "enum": ["above", "below", "left", "right"]}, "gap": {"type": "integer", "minimum": 0}}, "description": "Instead of x, y: beside another part - {\"part\": \"U1\", \"side\": \"below\", \"gap\": 40}, the room between their symbols (40 unless given), centred on it across that side"},
   "name": {"type": "string", "description": "Its name; the next free one (R1, R2, ...) when not given"},
   "properties": {"type": "object", "additionalProperties": {"anyOf": [{"type": "string"}, {"type": "array", "items": {"type": "string"}}]}, "description": "Values by property name, as get_schematic and describe_component_type show them: {\"R\": \"4.7k\"}; the others stay at the type's defaults. A number with letters after it that are no scale and unit (1kk) is refused. A property a block has any number of - .NGOPT's Knob, Target and Constraint, NgSweep's Record and Vs, a Monte Carlo's Record and Spec, an optimization's Var and Goal - takes a list of its values: {\"Knob\": [\"dparam|Cp|20p|5p|100p\", \"dparam|Lp|100n|20n|500n\"]} (describe_component_type gives each one's fields)"},
   "rotation": {"type": "integer", "minimum": 0, "maximum": 3, "description": "Quarter turns from the type's own orientation, 0-3"}, "mirror": {"type": "boolean", "description": "Mirrored about the x axis"},
   "pin1": {"type": "string", "enum": ["top", "bottom", "left", "right"], "description": "Instead of 'rotation', for a part of two pins: the side of its centre its pin 1 goes to - a capacitor from a node above to ground below is pin1 top - turned so, with no table of rotations"},
   "shown": {"type": "object", "additionalProperties": {"type": "boolean"}, "description": "Which properties are shown on the schematic: {\"R\": true, \"Temp\": false}"},
   "name_shown": {"type": "boolean", "description": "Whether its name is written on the schematic"}, "text_at": {"anyOf": [{"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2}, {"type": "string", "enum": ["auto"]}], "description": "Where its text begins (the top left corner), [dx, dy] from its centre - or \"auto\": the nearest spot beside it where it is drawn over nothing (the right, the left, below, above), when where it is is not"}, "equations": {"type": ["array", "object"], "items": {"anyOf": [{"type": "string"}, {"type": "object"}]}, "description": "An equation block's equations (Eqn, NutmegEq, .PARAM, .OPTIONS, .FUNC, .IC, ...), as get_schematic gives them: a list of \"name=expression\" in their order, [\"gain_db=db(v(out))\", \"k=2\"]. edit_component changes those it names and keeps the rest ('replace_equations' for a whole new list); {\"k\": null} in the list takes k away. An .OPTIONS option with no value is a flag: 'flags'. The answer lists the block's equations as they are then."},
   "flags": {"type": "array", "items": {"type": "string"}, "description": "An ngspice .OPTIONS block's (SpiceOptions) options with no value, each written alone: [\"noinit\", \"keepopinfo\"] - as {\"noinit\": true} in 'equations'"},
   "replace_equations": {"type": "boolean", "description": "The equations become exactly the 'equations' and 'flags' given with it (else each given is set or added); alone it is refused"},
   "records": {"type": "array", "items": {}, "description": "An ngspice Monte Carlo's or corners' values recorded for each sample: [{\"name\": \"gain\", \"expression\": \"db(v(out))\"}] or \"gain|db(v(out))\" - the list it records"},
   "specs": {"type": "array", "items": {}, "description": "Their limits: [{\"expression\": \"gain\", \"min\": \"19\", \"max\": \"21\"}] (one limit may be left out) or \"gain|19|21\" - a sample passes within all"}},
   "required": ["type", "x", "y"]}},
{"name": "edit_component",
 "description": "Changes a component; whatever is not given stays as it is. You can change its properties (by name), its name, its position (x, y: where its center goes), its rotation (0-3 quarter turns from the type's own orientation, as get_schematic reports it), mirroring, whether it is active (an inactive part is left out of the simulation), and its text: 'shown' chooses which properties are written on the schematic ({\"Is\": false} hides one, with no need to rewrite its line with set_schematic), 'name_shown', and 'text_at' ([dx, dy] from its center, where its text begins, to move it off another part - or \"auto\": the nearest spot beside it where it is drawn over nothing; check_schematic notes each text drawn over something). Moved, turned or with its texts changed, a part whose text is then drawn over something says so ('text'). An equation block's 'equations' are changed by name: each one given is set or added, {\"k\": null} removes k, and 'replace_equations' makes the list exactly those given; an .OPTIONS option without a value is a flag ('flags'). A Monte Carlo or corners block's 'records' and 'specs' replace its lists. 'rename' renames the part, and the traces, equations and markers that refer to it follow (i(V1), V1.It, @R1[i], R1's parameters in an equation), as rename_net does for a net. When the part is turned or moved, the circuit stays the same: its pins are wired again to the nets they were on, and other nets' wires under its new pin positions are moved out of the way. A change that cannot keep every net as it was is not made, and the error says why. A pin with nothing connected that lands on another part's pin joins that pin's net, and the result's 'note' says so. The answer lists an equation block's equations as they are afterwards. A ground is named by its ref (GND#2): it can be turned and moved, and the answer gives its ref, which may change.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "name": {"type": "string", "description": "The part: its name, or a ground by its ref (GND when there is one, GND#2 the second of several, as get_schematic gives it)"}, "rename": {"type": "string", "description": "Its new name; the traces, equations and markers that name it follow"},
   "properties": {"type": "object", "additionalProperties": {"anyOf": [{"type": "string"}, {"type": "array", "items": {"type": "string"}}]}, "description": "Values to change, by property name: {\"R\": \"10k\"}; the rest stay. A number with letters after it that are no scale and unit (1kk) is refused. A property a block has any number of (.NGOPT's Knob, Target and Constraint, ...) takes a list: it replaces every one of that name"},
   "x": {"type": "integer", "description": "Where its centre goes, in schematic units, on the grid (usually 10); its pins are wired again to their nets"}, "y": {"type": "integer", "description": "Where its centre goes, in schematic units, on the grid (usually 10); its pins are wired again to their nets"}, "near": {"type": "object", "properties": {"part": {"type": "string"}, "side": {"type": "string", "enum": ["above", "below", "left", "right"]}, "gap": {"type": "integer", "minimum": 0}}, "description": "Instead of x, y: moved beside another part - {\"part\": \"U1\", \"side\": \"below\", \"gap\": 40}, the room between their symbols (40 unless given), centred on it across that side"}, "rotation": {"type": "integer", "minimum": 0, "maximum": 3, "description": "Quarter turns from the type's own orientation, 0-3, as get_schematic gives it"},
   "pin1": {"type": "string", "enum": ["top", "bottom", "left", "right"], "description": "Instead of 'rotation', for a part of two pins: turned so its pin 1 is on that side of its centre (its nets kept, as a rotation keeps them)"},
   "mirror": {"type": "boolean", "description": "Mirrored about the x axis"}, "active": {"type": "boolean", "description": "false leaves it out of the simulation (inactive); true puts it back"},
   "shown": {"type": "object", "additionalProperties": {"type": "boolean"}, "description": "Which properties are shown on the schematic: {\"Is\": false, \"Bf\": true}"},
   "name_shown": {"type": "boolean", "description": "Whether its name is written on the schematic"}, "text_at": {"anyOf": [{"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2}, {"type": "string", "enum": ["auto"]}], "description": "Where its text begins (the top left corner), [dx, dy] from its centre - or \"auto\": the nearest spot beside it where it is drawn over nothing (the right, the left, below, above), when where it is is not"}, "equations": {"type": ["array", "object"], "items": {"anyOf": [{"type": "string"}, {"type": "object"}]}, "description": "An equation block's equations (Eqn, NutmegEq, .PARAM, .OPTIONS, .FUNC, .IC, ...), as get_schematic gives them: a list of \"name=expression\" in their order, [\"gain_db=db(v(out))\", \"k=2\"]. edit_component changes those it names and keeps the rest ('replace_equations' for a whole new list); {\"k\": null} in the list takes k away. An .OPTIONS option with no value is a flag: 'flags'. The answer lists the block's equations as they are then."},
   "flags": {"type": "array", "items": {"type": "string"}, "description": "An ngspice .OPTIONS block's (SpiceOptions) options with no value, each written alone: [\"noinit\", \"keepopinfo\"] - as {\"noinit\": true} in 'equations'"},
   "replace_equations": {"type": "boolean", "description": "The equations become exactly the 'equations' and 'flags' given with it (else each given is set or added); alone it is refused"},
   "records": {"type": "array", "items": {}, "description": "An ngspice Monte Carlo's or corners' values recorded for each sample: [{\"name\": \"gain\", \"expression\": \"db(v(out))\"}] or \"gain|db(v(out))\" - the list it records"},
   "specs": {"type": "array", "items": {}, "description": "Their limits: [{\"expression\": \"gain\", \"min\": \"19\", \"max\": \"21\"}] (one limit may be left out) or \"gain|19|21\" - a sample passes within all"}}, "required": ["name"]}},
{"name": "diff",
 "description": "Lists what changed in a schematic, part by part (a component's properties, position and rotation; wires; labels; diagrams; paintings): against its saved file (its unsaved changes), a number of 'steps' back in its undo history, or 'against' another schematic (an open one or a file). Nothing is changed.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"},
   "steps": {"type": "integer", "minimum": 1, "description": "How many steps back in its undo history"},
   "against": {"type": "string", "description": "Another schematic: an open document's name or a .sch file"}}}},
{"name": "replace_component",
 "description": "Replaces a component with one of another type - for example a built-in OpAmp with a subcircuit (Sub, with its File given) or a diode model with a Verilog-A one - and connects the new pins to the old pins' nets. 'pins' maps old pins to new ones by number or name ({\"2\": \"inp\", \"1\": \"inn\", \"3\": \"out\"}); without it, pins are matched by name when all the old pins' names exist on the new part, otherwise by number - but when a wired old pin has a name the new part lacks, or the old pins have none (a built-in OpAmp's) and the new part's named pins are numbered otherwise than their roles and places say, it is refused, with both parts' pins, the side of the symbol each is on and a map by their roles and places to give back as 'pins' (or \"by number\"). The new part is turned, mirrored and placed so that its pins land where the old ones were; a pin that cannot is wired to its net along a path that touches nothing else. Of the placements that keep every net, the one with the fewest pins off their old positions and the least wire is chosen; 'rotation', 'mirror', 'x' and 'y' choose it yourself. The part keeps the old name unless 'rename' gives another, so traces and equations that refer to it stay correct. Properties, equations and flags are given as for add_component. It is refused if an old pin that has something connected has no new pin, or if no placement keeps the nets. One undo step. Returns the new part, which old pin became which new one, and how it was placed.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "name": {"type": "string", "description": "The component to replace"},
   "type": {"type": "string", "description": "The new part's type (list_component_types)"},
   "pins": {"description": "Old pin to new pin, each by number or name: {\"1\": \"inn\"} or [[1, \"inn\"], ...]; or \"by number\". Needed when the old part's wired pins have names the new part's lack, or have none while the new part's do and the numbers do not match their roles and places (it is refused without, listing both parts' pins, the side each is on, and a map by their roles and places to pass back here)"},
   "properties": {"type": "object", "description": "The new part's values by property name, as add_component takes them; a number mistyped (1kk) is refused"}, "equations": {"description": "As add_component takes them"},
   "flags": {"type": "array", "items": {"type": "string"}, "description": "An .OPTIONS block's options with no value, as add_component takes them"},
   "rotation": {"type": "integer", "minimum": 0, "maximum": 3, "description": "Quarter turns, 0-3: this placement instead of the one found"}, "mirror": {"type": "boolean", "description": "Mirrored about the x axis: with 'rotation', this placement instead of the one found"},
   "x": {"type": "integer", "description": "Where its centre goes (with 'y'), instead of where its pins land on the old ones"}, "y": {"type": "integer", "description": "Where its centre goes (with 'x'), instead of where its pins land on the old ones"},
   "rename": {"type": "string", "description": "The new part's name (the old one's unless given)"},
   "shown": {"type": "object", "description": "Which properties are shown on the schematic: {\"C\": true}"}, "name_shown": {"type": "boolean", "description": "Whether its name is written on the schematic"},
   "text_at": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Where its text begins (the top left corner), [dx, dy] from its centre"},
   "replace_equations": {"type": "boolean", "description": "Taken as add_component takes it: the new part's equations are those given in any case; with no 'equations' or 'flags' it is refused"},
   "records": {"type": "array", "items": {}, "description": "A Monte Carlo's or corners' values recorded for each sample, as add_component takes them"},
   "specs": {"type": "array", "items": {}, "description": "Their limits, as add_component takes them"}},
   "required": ["name", "type"]}},
{"name": "delete",
 "description": "Deletes elements from a schematic as one undo step: components (by name; a ground by its ref from get_schematic, GND#2), net labels (by the net's name), wires (by their two ends, [x1, y1, x2, y2]), diagrams (by their numbers from get_schematic), traces ({\"diagram\": n, \"trace\": its number or variable}) and paintings (by their numbers; the symbol's paintings with 'symbol').",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "names": {"type": "array", "items": {"type": "string"}, "description": "Components by name (a ground by its ref (GND when there is one, GND#2 the second of several, as get_schematic gives it)), or a net's name: its labels"},
   "wires": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}, "minItems": 4, "maxItems": 4}, "description": "Wires by their two ends: [[x1, y1, x2, y2], ...], either end first"},
   "diagrams": {"type": "array", "items": {"type": "integer"}, "description": "Diagrams by their numbers from get_schematic"},
   "traces": {"type": "array", "items": {"type": "object", "properties": {"diagram": {"type": "integer"}, "trace": {}}}, "description": "Traces: [{\"diagram\": 1, \"trace\": 2}] - the trace by its number or its variable"},
   "paintings": {"type": "array", "items": {"type": "integer"}, "description": "Paintings by their numbers from get_schematic (the symbol's, with 'symbol')"}, "symbol": {"type": "boolean", "description": "The paintings are the symbol's"},
   "selection": {"type": "boolean", "description": "What the user selected: its parts, wires, diagrams, paintings"}}}},
{"name": "add_analysis",
 "description": "Adds an analysis with the usual settings and, with 'plot', a diagram of its results below the circuit - one call instead of several coordinated edits. 'kind' is one of: ac ('from' and 'to' in Hz, 1 Hz to 100 MHz by default; 'points', 101; 'scale' log unless lin; plotted in dB over a logarithmic frequency axis); tran ('stop' time, 1 ms by default; 'points', 201, so the print step is stop/(points-1); plotted over time); op (the DC operating point, a .DC block); sweep (a parameter sweep of 'analysis' - its name, such as TR1, AC1 or DC1 - over 'parameter', either a component name whose value is swept (R2) or a parameter defined by an equation; 'from', 'to', 'points' (11) and 'scale'; the analysis's curves become one per value). 'plot' lists what to show: nodes (out), v(out), i(v1) - or expressions such as db(v(out)) or v(out)/v(in), which a NutmegEq placed beside the analysis computes (ngspice; the answer lists its equations), drawn on an AC plot's right axis, since its left axis already shows dB. 'x' and 'y' place the block (next to the other analyses by default) and 'name' names it. Values are numbers or text with units (\"10 kHz\"). Returns the block and the diagram, each one undo step.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "kind": {"type": "string", "enum": ["ac", "tran", "op", "sweep"], "description": "ac, tran, op (the DC operating point) or sweep (of another analysis over a parameter)"},
   "from": {"description": "ac: the start frequency (1 Hz by default); tran: the start time (0); sweep: the first value - a number or text with units (10 kHz)"}, "to": {"description": "ac: the stop frequency (100 MHz by default); sweep: the last value; tran: as 'stop'"}, "stop": {"description": "tran: the stop time, 1 ms by default (1m, 10 us, 0.002)"}, "points": {"description": "How many points: ac 101, tran 201 (the print step is stop/(points-1)), sweep 11 by default"}, "scale": {"type": "string", "enum": ["lin", "log"], "description": "ac and sweep: log or lin (ac log, sweep lin by default)"},
   "analysis": {"type": "string", "description": "sweep: the analysis swept, by name (TR1, AC1, DC1)"}, "parameter": {"type": "string", "description": "sweep: what is swept - a component's name (its value: R2) or a parameter an equation defines"},
   "properties": {"type": "object", "description": "Its properties by name, as add_component takes them ({\"Start\": \"10 Hz\", \"Points\": \"201\"}, a .TR's \"MaxStep\"): over what kind's defaults set; the same thing in 'from', 'to', 'stop', 'points' or 'scale' as well is refused"},
   "plot": {"type": "array", "items": {"type": "string"}, "description": "What a diagram of its results shows: nodes (out), v(out), i(V1) - or expressions, db(v(out)), v(out)/v(in), which a NutmegEq added beside the analysis computes (ngspice; on an AC plot on the right axis, as the left one shows dB already). No diagram when not given"}, "x": {"type": "integer", "description": "Where the block goes (with 'y'); beside the other analyses by default"}, "y": {"type": "integer", "description": "Where the block goes (with 'x'); beside the other analyses by default"}, "name": {"type": "string", "description": "Its name: AC1, TR1, ... the next free one by default"}, "simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"], "description": "The simulator its 'plot' expressions are made for - under ngspice and spiceopus a NutmegEq computes them; the one in the settings by default (simulate's 'simulator' runs another once)"}},
   "required": ["kind"]}},
{"name": "create_subcircuit",
 "description": "Turns several components into a subcircuit. They and the wiring between them move into a new schematic 'save_as' (a .sch next to this one; 'replace' overwrites an existing file), with a port for each net that also reaches the rest of the circuit (named as the net's label, else as the one named pin of theirs on it - a 741's INN -, else file_n1) and a ground for each pin on ground. In their place comes one subcircuit component ('name', SUB1 by default) whose pins are connected to those nets by net labels. Analyses stay outside. Returns the file, the instance and its ports (port number and net). One undo step here (the file stays written; with 'preview' no file is left, and one it would replace is as it was); make_symbol then draws its symbol.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "names": {"type": "array", "items": {"type": "string"}, "description": "The components that go into it, by name (a ground by its ref (GND when there is one, GND#2 the second of several, as get_schematic gives it))"}, "save_as": {"type": "string", "description": "The subcircuit's file: a .sch beside this schematic (a name, or a path)"},
   "name": {"type": "string", "description": "The subcircuit component's name, SUB1 (the next free one) by default"}, "replace": {"type": "boolean", "description": "Write over the file 'save_as' names when it is there (else refused)"},
   "selection": {"type": "boolean", "description": "The parts the user selected, instead of names"}}, "required": ["save_as"]}},
{"name": "move",
 "description": "Moves several components together (by name; a ground by its ref, GND#2) by dx, dy (in schematic units, on its grid: 100 is ten steps of a grid of 10), keeping every net as it was. The wiring between them moves with them, and the wires to the rest of the circuit are extended to their pins' new positions, as the cursor keys move a selection. 'diagrams' and 'paintings' (their numbers) can move along; a diagram's title (add_diagram's 'title') is part of the diagram. A move that would join or split a net is not made. Use it to make room for a stage or to tidy a circuit. One undo step.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "names": {"type": "array", "items": {"type": "string"}, "description": "The components to move, by name (a ground by its ref (GND when there is one, GND#2 the second of several, as get_schematic gives it))"},
   "dx": {"type": "integer", "description": "How far right (negative: left), in schematic units, on the grid (usually 10)"}, "dy": {"type": "integer", "description": "How far down (negative: up), in schematic units, on the grid (usually 10)"},
   "diagrams": {"type": "array", "items": {"type": "integer"}, "description": "Diagrams to move along, by their numbers"}, "paintings": {"type": "array", "items": {"type": "integer"}, "description": "Paintings to move along, by their numbers"},
   "selection": {"type": "boolean", "description": "What the user selected - its parts, diagrams and paintings - instead of names"}}}},
{"name": "arrange",
 "description": "Lays out a whole schematic again so a person can read it. The parts go in columns by signal flow: sources on the left, then each part one column to the right of the part that drives it, with room between them; a DC supply gets a column of its own on the left. Two-pin parts are turned the way schematics usually show them: in series lying down with the driving side on the left, to ground or a supply standing up with ground below. Every wire is redrawn by the same router connect uses (around the parts, never over another pin). Each piece of circuit that had a ground symbol gets one back, and net labels go back on the nets that had them. Blocks without pins (analyses, equations) go in a row below; diagrams and paintings the circuit would cover move to its right. The circuit is kept: every net is compared before and after, and if any would differ nothing changes and the answer says why. It suits a schematic built from scratch or imported; a carefully drawn one may read better as it was, and one undo step brings it back. 'wire_labels' draws wires where only labels join a net's pieces - an imported netlist's nets are labels on every pin - keeping one label for its name. 'keep_places' leaves every part where it is and draws only the wiring again; 'straighten' nudges parts a few grid steps so the wires between them run straight; 'feedback' puts an op-amp's feedback parts below (or above) it; 'supplies': labels joins the supplies by labels and each ground pin by a ground symbol instead of wires. 'labels' moves only texts: each that is drawn over something, to a free spot beside its part (check_schematic notes them). 'preview' reports the result without keeping it.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"},
   "spacing": {"type": "integer", "minimum": 30, "maximum": 400, "description": "The room between parts, in the schematic's units: 60 unless given (more room is tried when the wires do not fit)"},
   "wire_labels": {"type": "boolean", "description": "Wires where only net labels join the pieces of a net (an imported netlist's labels on every pin), one label kept for its name; ground symbols stay"},
   "keep_places": {"type": "boolean", "description": "Every part stays where it is: only the wires, ground symbols and labels are drawn again (a tidy)"},
   "straighten": {"type": "boolean", "description": "Nudge parts up to 4 grid steps, clear of the others, so the two pins of each wire between two parts line up and it runs straight; with keep_places, a tidy that finishes a drawing"},
   "feedback": {"type": "string", "enum": ["inline", "below", "above"], "description": "Where a feedback part goes - a two-pin part between two nets of one part of three pins or more (Rf from an op-amp's output to its inverting input): in the columns (inline, the default), or below or above that part, lying as its pins run"},
   "supplies": {"type": "string", "enum": ["column", "labels"], "description": "column (the default): the supplies in a column at the left, wired; labels: a label of the supply's net on each of its pins (VCC above ground, VEE below, unless it has a name) and a ground symbol on each pin on ground - no wires for them"},
   "labels": {"type": "boolean", "description": "Move only texts, never a part or a wire: each part's text and each net label's that is drawn over something (a wire, a symbol, another text or label, a diagram) to the nearest free spot beside it - the right, the left, below, above; the others stay. Alone, with no other option"}}}},
{"name": "connect",
 "description": "Draws a wire with right angles between two pins or points, along a path that runs over no other pin or wire (a wire connects to whatever it runs over): around the parts when possible, otherwise across them; 'side' makes it go round one side first, 'via' through points of yours. It joins the two nets and nothing else; if no path would, it draws nothing and explains why. A crossing of another net's wire along the way (no connection) is reported. A pin is \"R1.1\" (the component's name and the pin number, from 1, or the pin's name); a point is [x, y]. With \"ground\" (or \"gnd\") at either end, the pin gets a ground symbol of its own, placed on the pin when it fits there and otherwise a little away and wired to it.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"},
   "from": {"description": "\"R1.2\" or [x, y]; a ground is GND.1 when there is one, GND#2.1 the second of several (get_schematic's 'ref')"}, "to": {"description": "\"C1.1\", [x, y] - or \"ground\": a ground symbol of the pin's own, on it or beside it, wired"},
   "side": {"type": "string", "enum": ["above", "below", "left", "right"], "description": "The side of the two ends the wire goes round first (a feedback path below an op-amp): along a line past both ends, a grid step further out each try; the shortest other way when none there is clear"},
   "via": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2}, "description": "Points the wire goes through, in order, [[x, y], ...]: a right angle, across first, where a step is not straight; refused when it would touch another net"}}, "required": ["from", "to"]}},
{"name": "add_wire",
 "description": "Draws a wire through points, [[x, y], [x, y], ...], one segment from each point to the next (a step that is not straight gets a right angle). Whatever is at its points is connected - a wire connects only where it ends or bends. A wire that would run along another pin or wire between its points is not drawn, and the error names what is in the way. A wire that crosses another net's wire is drawn, but a crossing is not a junction and makes no connection; the result says so (\"Look: ...\"). To join the other wire, end this one on it.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "points": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2}, "minItems": 2, "description": "Its points in order, [[x1, y1], [x2, y2], ...]: a segment from each to the next, a bend where a step is not straight"}},
   "required": ["points"]}},
{"name": "set_label",
 "description": "Names the net at a pin or at a point on a wire (a net label: nets with the same name are connected). An empty name removes the label. 'text_at' puts the label's text where you want it (the same name again moves only its text).",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "at": {"description": "\"R1.1\" or [x, y]"}, "name": {"type": "string", "description": "The net's name (\"\" takes the label away); nets of one name are one. As the label dialog takes one: a letter, then letters, digits and single _ - not gnd, 0 or net1, net2 ..."},
   "text_at": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Where the label's text goes, [x, y] on the schematic (above right of the place unless given)"}}, "required": ["at", "name"]}},
{"name": "select",
 "description": "Selects components by name (a ground by its ref, GND#2), and diagrams and paintings by their numbers from get_schematic, in a schematic; everything else is deselected. Nothing given deselects everything.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "names": {"type": "array", "items": {"type": "string"}, "description": "Components by name (a ground by its ref (GND when there is one, GND#2 the second of several, as get_schematic gives it))"},
   "diagrams": {"type": "array", "items": {"type": "integer"}, "description": "Diagrams by their numbers from get_schematic"}, "paintings": {"type": "array", "items": {"type": "integer"}, "description": "Paintings by their numbers from get_schematic"}}}},
{"name": "zoom",
 "description": "Zooms a schematic: 'all' shows all of it, 'selection' the selection, 'in' and 'out' one step, 'none' a scale of 1.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "to": {"type": "string", "enum": ["all", "selection", "in", "out", "none"], "description": "all (the whole schematic), selection, in or out (one step), none (a scale of 1)"}}, "required": ["to"]}},
{"name": "undo",
 "description": "Undoes the last change of a document (the one in front unless 'path' names another), like Edit > Undo; with 'files', the files the last calls wrote instead. 'steps' undoes that many changes (a batch that stopped halfway reports how many changes it made); 'to' goes to a step of a schematic as undo_history numbers them, backward or forward again. Reports what it changed back, part by part.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "steps": {"type": "integer", "minimum": 1, "maximum": 1000, "description": "How many changes to undo, 1 by default"},
   "to": {"type": "integer", "minimum": 0, "description": "A step as undo_history lists them: the schematic as it was after it (0: as it was loaded)"},
   "files": {"description": "Instead of a document's changes: the files the last calls wrote put back as they were (true or 1: the last call's; a number: that many calls') - save_document's, create_subcircuit's, copy_document's, import_netlist's, import_data's, the exports', rename_net's data display; a file rename_file moved or trash_file put in the trash is moved back. A file made by the call is removed; one changed since is left, and said. undo_history lists them"}}}},
{"name": "undo_history",
 "description": "Lists a schematic's undo steps in words - \"step 7: R2 R 47k → 67k; step 8: diagram 2: trace 2's look changed\" - the last 'steps' (10 by default) up to the current position, plus those that can be redone after it, so undo can go to a known step ('to') instead of counting.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "steps": {"type": "integer", "minimum": 1, "maximum": 200, "description": "How many steps before the current one are listed, 10 by default"}}}},
{"name": "redo",
 "description": "Redoes the last undone change, like Edit > Redo; 'steps' redoes that many, 'to' goes forward to a step of a schematic as undo_history numbers them (a step before the current one is refused: undo's 'to' goes back).",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "steps": {"type": "integer", "minimum": 1, "maximum": 1000, "description": "How many changes to redo, 1 by default"},
   "to": {"type": "integer", "minimum": 0, "description": "A later step as undo_history lists them: the schematic as it was after it"}}}},
{"name": "screenshot",
 "description": "Takes a picture (PNG). 'diagram' (its number) or 'region' ([x1, y1, x2, y2] in schematic units) captures only that, as printed, to check a plot without the whole page. 'area' chooses what is captured: paper (or all, the default) is the whole schematic as it is printed and exported, on white paper in its print colors whatever the theme; screen (or visible) is the canvas exactly as the user sees it now, with the theme's colors (a dark canvas in the dark theme), its zoom and what is scrolled into view - use it to see what the user sees, especially for a problem with how something looks on screen; window is the whole Qucs-S window (panes, tabs, docks, status bar), plus each dialog open over it as a separate picture. For a text document or a PDF, paper and screen show what its tab shows.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "area": {"type": "string", "enum": ["paper", "screen", "window", "all", "visible"], "description": "paper (or all): the whole schematic as printed, the default; screen (or visible): the canvas as the user sees it; window: the whole Qucs-S window and its dialogs"},
   "diagram": {"type": "integer", "description": "Only this diagram, by its number, as printed"}, "region": {"type": "array", "items": {"type": "integer"}, "minItems": 4, "maxItems": 4, "description": "Only [x1, y1, x2, y2] of the schematic, as printed"}}}},
{"name": "list_component_types",
 "description": "Lists the library's components: the type to give add_component, what it is, and its category. 'search' keeps only those whose type, name or category contains it. describe_component_type describes a type's properties.",
 "inputSchema": {"type": "object", "properties": {"search": {"type": "string", "description": "Only types whose type, name or category contains it (resistor, BJT, source)"}}}},
{"name": "list_actions",
 "description": "Lists the actions of Qucs-S's menus: the menu path to give trigger_action (\"Simulation > Simulate\"), whether it can be used now, whether it is checked, and its shortcut. 'search' keeps only those whose path contains it.",
 "inputSchema": {"type": "object", "properties": {"search": {"type": "string", "description": "Only actions whose menu path contains it (Simulate, Rotate)"}}}},
{"name": "trigger_action",
 "description": "Runs a menu action as a click would: 'action' is its menu path (\"Edit > Rotate\") or its object name. If it opens a dialog, the dialog stays open: get_dialog reads it, and set_dialog fills it in and closes it - a file dialog too, which is Qt's when Claude opens it: its \"File name\" (or \"Directory\") field takes a path, then Open, Save or Choose. File > Open and Save As are refused (open_document and save_document do them), and printing.",
 "inputSchema": {"type": "object", "properties": {"action": {"type": "string", "description": "Its menu path (\"Edit > Rotate\") as list_actions gives it, or its object name"},
   "path": {"type": "string", "description": "The document to use it on, brought to the front first; the one in front when not given"}},
  "required": ["action"]}},
{"name": "get_dialog",
 "description": "Reads the Qucs-S dialog waiting for an answer (or another open window of Qucs-S): its title, texts and controls - fields, lists (and whether each item is ticked, when they have check boxes: 'checked'), check boxes, tabs, tables, trees (a search's results: rows, and whether each is checked), buttons - each with its label, value, an id for set_dialog and, while the dialog is shown, 'at' [x, y, width, height] in its picture (send_input's target dialog). A field in a table's cell is named by its row and column: \"R (Value)\".",
 "inputSchema": {"type": "object", "properties": {}}},
{"name": "set_dialog",
 "description": "Fills in the open dialog and presses a button. 'set' changes controls, each by its id or label from get_dialog: a field takes text, a list an item (or [item or row, true or false] to tick it or not), a check box true or false, a spin box a number, tabs a tab's title, a table [row, column, text] (a cell with a check box: true or false), and a tree [row, column, true or false] to check a row or not (or [row, column, text]); a field that edits a table's cell (Edit Component Properties' values) gives the cell its value, as Return would. With 'action' (select or activate - a double click) a table's, list's or tree's row is chosen by its text or number as a click would: Diagram Properties' variables take a trace activated. 'press' names the button to press afterwards (OK, Cancel, Apply, ... or its id). The answer names each control set as get_dialog does (its label, else its id; a cell with its row and column).",
 "inputSchema": {"type": "object", "properties": {
   "set": {"type": "array", "items": {"type": "object", "properties": {"control": {"type": "string"}, "value": {}, "action": {"type": "string", "enum": ["select", "activate"]}}, "required": ["control", "value"]}, "description": "Controls to change: [{\"control\": id or label from get_dialog, \"value\": text, an item, true or false, a number, a tab's title, [row, column, text] for a table, [row, column, true or false] for a tree's check box, or [item or row, true or false] for a list's}]; with \"action\" select or activate, \"value\" is a row of a table, list or tree - its text or number - clicked or double-clicked"},
   "press": {"type": "string", "description": "The button pressed after: OK, Cancel, Apply, ... or its id"}}}},
{"name": "get_settings",
 "description": "Reads the settings of Qucs-S, typed: 'scope' app (Application Settings), simulators (Simulators Settings), document (the settings of the document 'path' names, the one in front unless given) or cdl (CDL Settings). Each setting by its key - \"Tab/Label\" as the dialog shows it (\"Settings/Maximum undo operations\", \"Simulators/Ngspice executable location\") - with its type (text, bool, option, choice, number, integer, table, folders - a list of folders, such as \"Locations/Library search paths\"), its value, and its choices or range. 'keys' and 'search' give only some (app alone has about 70): 'keys' by key, * for any part (\"Locations/*\"), or by label alone (\"Maximum undo operations\"); 'search' those with each word in the key, value or choices (\"ngspice\"). Without 'scope', 'keys' or 'search' look through app, simulators and cdl, each setting saying its scope. Nothing is shown or changed.",
 "inputSchema": {"type": "object", "properties": {"scope": {"type": "string", "enum": ["app", "simulators", "document", "cdl"], "description": "Application Settings, Simulators Settings, a document's own, or CDL Settings; not given, 'keys' or 'search' looks through app, simulators and cdl"}, "path": {"type": "string", "description": "For scope document: the document; the one in front when not given"},
   "keys": {"type": "array", "items": {"type": "string"}, "description": "Only these: keys (\"Simulators/Ngspice executable location\"), with * for any part (\"Locations/*\", \"*/Ngspice*\"), or labels alone (\"Maximum undo operations\"), case aside"},
   "search": {"type": "string", "description": "Only the settings with each of these words in the key, value or choices (\"ngspice\", \"undo\")"}}}},
{"name": "set_settings",
 "description": "Changes settings of Qucs-S by their keys (get_settings lists them): 'values' {\"Tab/Label\": new value} - or a label alone when it is one setting's. Done through the settings' own dialog, opened as its menu action opens it and applied with its own OK, so what Qucs-S does after it is done too, and its checks hold. Returns each change with what it 'was' (set_settings with it puts it back) and what it is 'now', read again; what was not done and why; what the dialog said. Claude Code's own settings are refused.",
 "inputSchema": {"type": "object", "properties": {"scope": {"type": "string", "enum": ["app", "simulators", "document", "cdl"], "description": "Application Settings, Simulators Settings, a document's own, or CDL Settings"}, "values": {"type": "object", "description": "{\"Tab/Label\": value}: text, true or false, a number, one of its choices, or for folders the whole list of full paths ([] for none)"}, "path": {"type": "string", "description": "For scope document: the document; the one in front when not given"}}, "required": ["scope", "values"]}},
{"name": "console",
 "description": "Types a line into a console dock of Qucs-S - octave (the Octave dock), python (the Python Shell) or terminal (the Terminal dock, a shell) - as the user would there, and returns what it printed once its prompt is back, or what came by 'wait' seconds (10 unless given; the run goes on, and 'interrupt': true stops it with Ctrl-C). What is typed runs with the user's rights, outside Claude Code's own rules for commands: each use is asked about, every time, with the line shown. Without 'input': the last 'lines' of what the console shows, nothing typed. The user sees it all in the dock.",
 "inputSchema": {"type": "object", "properties": {"kind": {"type": "string", "enum": ["octave", "python", "terminal"], "description": "The console"}, "input": {"type": "string", "description": "One line, typed and entered"}, "wait": {"type": "integer", "description": "Seconds to wait for its prompt back (1 to 600; 10 unless given)"}, "interrupt": {"type": "boolean", "description": "Ctrl-C to what runs (python, terminal)"}, "lines": {"type": "integer", "description": "How many of the last lines to give (40 unless given)"}}, "required": ["kind"]}},
{"name": "wait_for",
 "description": "Waits for something to happen in Qucs-S and returns when it has, instead of asking again and again: 'event' simulation_finished (the run 'id' names, followed since simulate's 'background' or its timeout - its outcome then, as simulate gives it; without 'id', the run going now or the next one, the user's too), dialog (a dialog comes up; one open already counts), document_changed (the document 'path' names, the one in front unless given, is edited, undone or reloaded - by the user, another conversation, its file - since 'revision' from an answer before, else since now; or closed) or file_written (the file 'path' names is written, made or removed, then left alone for a moment). 'timeout' is in seconds, 60 unless given (1 to 3600): then it returns with happened false, and wait_for again waits on. Other calls go on meanwhile; what the user edits while it waits is the user's. Not in a batch or a script.",
 "inputSchema": {"type": "object", "properties": {"event": {"type": "string", "enum": ["simulation_finished", "dialog", "document_changed", "file_written"], "description": "What is waited for"}, "id": {"type": "integer", "description": "simulation_finished: the run followed (from simulate's answer, simulation_status)"}, "path": {"type": "string", "description": "document_changed: the document, the one in front unless given; file_written: the file"}, "revision": {"type": "integer", "description": "document_changed: changed since this revision (get_state's, an answer's), not since now"}, "timeout": {"type": "integer", "description": "Seconds to wait at most, 60 unless given (1 to 3600)"}}, "required": ["event"]}},
{"name": "simulation_status",
 "description": "Tells how a simulation followed by its id goes - one simulate began with 'background', or one still running at its timeout: running (for how long, the last lines it printed), or ended and then its outcome as simulate gives it (succeeded, errors, the dataset written ...). Without 'id': the runs followed, the last first.",
 "inputSchema": {"type": "object", "properties": {"id": {"type": "integer", "description": "The run's id, from simulate's answer"}}}},
{"name": "stop_simulation",
 "description": "Stops a simulation - the one 'id' names (followed since simulate's 'background' or its timeout), or without it the one running now, the user's too (as Simulation > Stop Simulation, or Qucsator's Abort) - and returns its outcome once it has ended.",
 "inputSchema": {"type": "object", "properties": {"id": {"type": "integer", "description": "The run's id; the one running when not given"}}}},
{"name": "send_input",
 "description": "Raw mouse and keyboard input - the last resort, for what no other tool does, or to do something exactly as the user did: a 'click' (left unless 'button' says right or middle; 'double'; 'drag_to' drags from it, in steps; 'modifiers' held), then 'keys' (as list_actions writes shortcuts: \"Ctrl+Z\", \"Delete\", \"Escape, Return\" - at most four; Ctrl is Command on a Mac; a key that is an action's shortcut sets off that action, as the keyboard does), then 'text' typed into what has the focus there. On a schematic's canvas ('target' canvas, the default; points in the schematic's coordinates as get_schematic gives them, brought into view - or with 'pixels' the canvas picture's pixels) or on a part of the window as get_ui names it (dock:Content, toolbar:Simulate, statusbar; points in its picture's pixels), or on the dialog that waits for an answer ('target' dialog: points in its picture's pixels - get_dialog's 'at' -, keys and text into its field with the focus, which a click gives; the window's shortcuts are not set off; the only input that reaches a dialog besides set_dialog). Returns what it opened (a dialog: get_dialog reads it; a menu: read and closed - context_menu chooses from one), the status bar's message, and a picture of the target as it is after, with how its pixels map. Not the consoles (console types there), not the Claude Code panel, not keys that would quit Qucs-S or set off what trigger_action refuses: then nothing is sent. Asked about each time.",
 "inputSchema": {"type": "object", "properties": {
   "target": {"type": "string", "description": "canvas (the default), dock:<title>, a panel's name, toolbar:<title>, statusbar, or dialog (the one open)"},
   "path": {"type": "string", "description": "For the canvas: the schematic, the one in front unless given"},
   "click": {"type": "array", "items": {"type": "number"}, "description": "[x, y]: on the canvas the schematic's coordinates (pixels with 'pixels'); on a part of the window its picture's pixels"},
   "button": {"type": "string", "enum": ["left", "right", "middle"], "description": "The mouse button, left unless given"},
   "double": {"type": "boolean", "description": "A double click"},
   "drag_to": {"type": "array", "items": {"type": "number"}, "description": "[x, y] where a drag from 'click' ends, the button held"},
   "modifiers": {"type": "array", "items": {"type": "string", "enum": ["shift", "ctrl", "alt", "meta"]}, "description": "Keys held during the click or drag (ctrl is Command on a Mac)"},
   "keys": {"type": "string", "description": "Keys pressed after: \"Ctrl+Z\", \"Delete\", \"Escape, Return\" (at most four)"},
   "text": {"type": "string", "description": "Characters typed after, into what has the focus there"},
   "pixels": {"type": "boolean", "description": "On the canvas: 'click' and 'drag_to' in the picture's pixels, not the schematic's coordinates"}}}},
{"name": "read_help",
 "description": "Finds what this build of Qucs-S says on a topic, to answer from the help this version ships: each menu action's own help (its What's This, status tip and shortcut), the component types (describe_component_type explains one), the example schematics (open_document opens one), and any papers or tutorials it has (read_pdf reads them) - and the Qucs-S manual: 'manual' fetches its text once into the cache (from qucs-s-help.readthedocs.io) and searches it by section, offline after, and 'page' gives a page whole; the Getting Started tutorial is online only. 'topic' is words a match has all of (tuner, s-parameter, monte carlo); without it, what help there is.",
 "inputSchema": {"type": "object", "properties": {"topic": {"type": "string", "description": "Words to find: tuner, s-parameter, monte carlo; none: what help there is"},
   "manual": {"type": "boolean", "description": "Search the Qucs-S manual (qucs-s-help.readthedocs.io): its text is fetched the first time (with curl, a few hundred KB) into Qucs-S's cache and read from there after - each section that has all the topic's words, with its page and an excerpt; once fetched, read_help searches it with every topic"},
   "page": {"type": "string", "description": "One page of the manual whole, by its name (overview/simulation-types/index, as the sections found name them)"},
   "refresh": {"type": "boolean", "description": "Fetch the manual again"}}}},
{"name": "get_ui",
 "description": "Reads a part of the Qucs-S window as get_dialog reads a dialog: a dock or a panel of one (dock:Simulation, dock:Content, dock:Problems, dock:Tuner, dock:Main Dock/Projects), a toolbar (toolbar:Simulate), the status bar (statusbar), or the documents' tabs (tabs). It gives the controls - fields, lists, buttons, check boxes, sliders - each with an id for set_ui, the views of files, projects and parts with their rows (a tree's with depth and whether open), the logs (their end), and the texts. Without 'area': the parts there are. Secret fields show as hidden. The Claude Code panel is the user's and is not among them.",
 "inputSchema": {"type": "object", "properties": {"area": {"type": "string", "description": "dock:<title> or a panel's name (dock:Content), toolbar:<title>, statusbar or tabs; none: the list"}}}},
{"name": "set_ui",
 "description": "Uses a part of the window as set_dialog answers a dialog - its dock is shown first. 'set' changes controls by their id or label from get_ui: a field takes text (a filter's: the panel filters), a list an item, a check box true or false, a slider a number, tabs a tab's title; a view's row (its text, its number in get_ui's rows, or a tree's path \"Schematics > amp.sch\") is selected, or with 'action' activated (a double click: a file opens), expanded or collapsed. 'press' presses a button. Area tabs: 'value' a tab's title brings that document to the front. Refused: the Claude Code panel, a secret field, and the consoles (Terminal, Python Shell, Octave), whose typing runs commands - the console tool types there.",
 "inputSchema": {"type": "object", "properties": {"area": {"type": "string", "description": "As get_ui's"},
   "set": {"type": "array", "items": {"type": "object", "properties": {"control": {"type": "string"}, "value": {}, "action": {"type": "string", "enum": ["select", "activate", "expand", "collapse"]}}, "required": ["control", "value"]}, "description": "[{\"control\": id or label, \"value\": ..., \"action\": for a view's row: select (the default), activate, expand or collapse}]"},
   "press": {"type": "string", "description": "A button pressed after, by its label or id"}}, "required": ["area"]}},
{"name": "context_menu",
 "description": "Opens the right-click menu the user would get on something, lists its entries (submenus as 'A > B', with whether each can be chosen and is checked), and with 'choose' chooses one - as the keyboard would. A dialog it opens goes on through get_dialog and set_dialog. 'on': {\"part\": \"R1\"}, {\"diagram\": 2} or {\"canvas\": [x, y]} on the schematic ('path'; the one in front), {\"project_item\": \"amp.sch\"} in the Content panel, {\"project\": \"amp_prj\"} in the Projects panel, {\"tab\": \"amp.sch\"} on a document's tab, or {\"file\": path} in the File Browser.",
 "inputSchema": {"type": "object", "properties": {"on": {"type": "object", "properties": {"part": {"type": "string"}, "diagram": {"type": ["integer", "string"]}, "canvas": {"type": "array", "items": {"type": "number"}}, "project_item": {"type": "string"}, "project": {"type": "string"}, "tab": {"type": "string"}, "file": {"type": "string"}}, "description": "What is right-clicked: one of part, diagram, canvas, project_item, project, tab, file"},
   "choose": {"type": "string", "description": "The entry chosen, by its path in the menu (\"Edit Properties\", \"Toggle hierarchy search view > Flat\") or its name alone; none: the menu is only read"},
   "path": {"type": "string", "description": "The schematic, for part, diagram and canvas: its file or tab's title; the one in front when not given"}}, "required": ["on"]}},
{"name": "simulate",
 "description": "Simulates a schematic (the one in front unless 'path' names another - a file not open is opened first, as open_document opens it, and the answer says so; an untitled one is saved in the scratch folder first, and the answer says where) with the simulator from the settings - or 'simulator' for this run only, leaving the setting unchanged - like Simulation > Simulate, and waits for it to finish (Qucsator too). Check Schematic runs first and its errors and warnings are reported ('before the run' - and, when the run fails, first in its 'errors': a pin connected to nothing before the simulator's complaint it led to). The result says whether it succeeded (the simulator ran to the end and reported no error); lists its errors and warnings, each with its message and, where the simulator names them, the netlist line (number and text), the schematic part and the node; names the dataset it wrote (name.dat.ngspice for ngspice, .dat.xyce, .dat.spopus; name.dat for Qucsator) and its variables; lists diagram traces that show no data and why; says whether the schematic was changed while it ran (by the user or another conversation - the results are then of the schematic as it was when the run began); and gives the last lines of the output. 'operating_point' runs only the DC operating point instead (like Simulation > Calculate DC bias, also for a transient-only schematic) and returns it structured: node voltages, branch currents and, with ngspice, each transistor's gm, ic, vbe, gpi and so on under its component, with re = 1/gm, rpi, beta and ro computed - the numbers that explain a gain; the datasets are left untouched. 'timeout' is in seconds, 120 by default. 'keep_as' keeps a copy of the dataset under that name for comparing runs: get_dataset reads it by its file name, and a trace can show it next to the current run as ngspice/<name>:tran.v(out). With the Simulator Settings' check of commands on, a schematic that runs commands besides the simulator - a System command part, ngspice's shell in its text, an Octave script after the run - is refused unless 'allow_commands' says so (check_schematic lists them). An ngspice optimize block's result comes back as 'optimum': each knob's value found, and with 'apply_optimum' the parameter or part it was written into. 'background' answers at once with the run's id - for a long run (Monte Carlo, a long transient): simulation_status gives how it goes and its outcome once it has ended, wait_for waits for its end, stop_simulation stops it; meanwhile other calls go on. A run still going at its timeout goes on the same way, the answer giving its id. A run going - the user's too - is stopped by stop_simulation (as Simulation > Stop Simulation).",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given; an untitled one is saved in the scratch folder first"}, "timeout": {"type": "integer", "description": "Seconds to wait for it, 120 by default (5 to 3600); a run still going then goes on, followed by the id the answer gives. With background: the most it may run, stopped then (none unless given)"},
   "background": {"type": "boolean", "description": "Answer at once with the run's id; simulation_status, wait_for and stop_simulation follow it"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"], "description": "For this run alone (an installed one); set_simulator changes the setting"},
   "keep_as": {"type": "string", "description": "A name of letters, digits, _ and -: the copy is <name>.dat.ngspice (or .xyce, ...) beside the schematic"},
   "compare": {"type": "object", "properties": {"with": {"type": "string"}, "measure": {"type": "array", "items": {"type": "object", "properties": {"variable": {"type": "string"}, "what": {"type": "string"}, "field": {"type": "string"}, "from": {"type": "number"}, "to": {"type": "number"}, "level": {"type": "number"}, "tolerance": {"type": "number"}, "fundamental": {"type": "number"}, "harmonics": {"type": "integer"}, "periods": {"type": "number"}, "decibels": {"type": "boolean"}, "form": {"type": "string"}}}}}, "description": "Before and after in one call: {\"with\": \"before\", \"measure\": [{\"variable\": \"ac.v(out)\", \"what\": \"bandwidth\"}, ...]} - each measured on this run and on the one kept as 'with' (keep_as), in a table of before, after and the change (and in %); 'what' is min, max, mean, rms, final, peak_to_peak or a get_dataset measurement, as tune's 'measure' takes it"},
   "apply_optimum": {"type": "boolean", "description": "With an ngspice optimize block (.NGOPT): write the values it finds into the parameters and parts its knobs name (.PARAM Cp, R1's value), one undo step - the answer's 'optimum' says what went where, and its 'status' why the search stopped (converged, completed, maxiter, interrupted, unchanged; nosolve and infeasible are not written in); without it they are only reported"},
   "allow_commands": {"type": "boolean", "description": "Run the commands the schematic carries besides the simulator (a System command part, ngspice's shell, an Octave script): with the user's rights - only when the user has seen them (check_schematic lists them)"},
   "brief": {"type": "boolean", "description": "What came of the run only: no log lines, the errors, warnings, variables and traces without data cut to a few with how many more"},
   "operating_point": {"type": "boolean", "description": "Run the DC operating point alone, whatever analyses the schematic has, and return it: each node's voltage and branch current, and (ngspice) each device's quantities - gm, ic, vbe, gpi, gds, ... - with re = 1/gm, beta, ro"}}}},
{"name": "get_netlist",
 "description": "Returns a schematic's netlist as text: as a simulation with the simulator from the settings would write it now, or with 'last' the one the last simulation ran (Simulation > Show Last Netlist; the one simulate's error line numbers refer to). 'numbered' puts each line's number in front of it. 'format' is spice (the default) or cdl (Simulation > Save CDL netlist, with the CDL settings). 'map' ties each netlist line to the part that wrote it and each node to the pins on it. If the netlister gives up (a part with no model, a subcircuit or library it cannot read), the error says why. export_netlist writes the netlist to a file.",
 "inputSchema": {"type": "object", "properties": {
   "map": {"type": "boolean", "description": "The netlist as lines, which part wrote each line, and which pins each node joins - to find an error's line, device or node on the schematic"},
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "last": {"type": "boolean", "description": "The netlist the last simulation ran (simulate's error line numbers are of it), not one written now"}, "numbered": {"type": "boolean", "description": "Each line's number in front of it"},
   "format": {"type": "string", "enum": ["spice", "cdl"], "description": "spice (the default) or cdl"}}}},
{"name": "export_netlist",
 "description": "Writes a schematic's netlist to a file, like Simulation > Save netlist and Save CDL netlist, whose file dialogs cannot be answered. 'save_as' is a path, or a name in the project's folder (otherwise the workspace's); an existing file is overwritten, but not a document open in Qucs-S, and a schematic, symbol, data display or code only with 'replace'. 'format' is spice (the default) or cdl, and 'last' writes the one the last simulation ran. Returns the file and its lines.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "save_as": {"type": "string", "description": "The file: a path, or a name in the project's folder (else the workspace's); written over when it is there, unless it is a document of Qucs-S"}, "last": {"type": "boolean", "description": "The netlist the last simulation ran"},
   "replace": {"type": "boolean", "description": "Write over a schematic, symbol, data display or code file that is not open (else refused)"},
   "format": {"type": "string", "enum": ["spice", "cdl"], "description": "spice (the default) or cdl"}}, "required": ["save_as"]}},
{"name": "get_dataset",
 "description": "Reads a simulation's results as numbers, from the dataset the simulator wrote (its 'written' time tells which run). Without 'variables' it lists the variables: the independent ones (time, frequency, a swept parameter, with their range and number of points) and the others (what they depend on, number of points, whether complex, range, units when known - dB, V, A, degrees - and the definition an equation gives them), each with the name a trace uses; plus the operating point of a DC simulation (op): each node's value and, with ngspice, each device's quantities (id, gm, vgs, ...) under its component. 'operating_point' returns only that, with every device in full. With 'variables', for each one over the x range from 'from' to 'to': its statistics (min and max and where they occur, mean and RMS weighted over x, initial and final value) and, as requested, 'points' (at most that many samples, spread evenly over the range), 'at' (values at given x, interpolated) and 'measure' (measurements on the full data). Measurements: rise_time and fall_time (10%-90% of the swing, first edge); overshoot (percent of the step); settling_time (to within 'tolerance' of the step, 0.02 by default, counted from 'from'); period, frequency and duty_cycle (at 'level', the middle of the swing by default); crossings (of 'level'); bandwidth (the -3 dB points: 1/sqrt(2) of the peak of a magnitude, or 3 dB below the peak of a curve in dB - db(...), vdb(...) or an equation that makes one, with 'decibels' saying so when it cannot be told; refused on a curve that goes below 0 and is not in dB); thd (of a transient: total harmonic distortion in percent and dB and each harmonic's amplitude, over the last 'periods' whole periods (1 by default) of 'fundamental' (Hz; the curve's own frequency by default) before 'to', harmonics 2 to 'harmonics' (9 by default), like ngspice's .four); gain (of an AC curve: at the lowest frequency and at its peak, as a ratio and in dB, and the unity-gain frequency where it falls through 1, 0 dB); phase_margin and gain_margin (of a loop gain given as a complex AC variable: 180 degrees plus its phase where its magnitude falls through 1, and minus its gain in dB where its phase falls through -180 degrees, each with its frequency); distribution (the values as samples, such as one per Monte Carlo run: mean, standard deviation, median, 5th and 95th percentiles and a histogram; with 'level', the share at or above it); fft (of a transient: its spectrum, resampled evenly with a Hann window - the strongest lines with amplitude and dBc, DC, the noise floor and the resolution); eye (of a transient folded at 'bit_period' - when not given, the Tbit of the V(PRBS) source the signal comes from, else told from the crossings - from 'offset': eye height at the bit centers, eye width (and at BER 1e-12), crossing jitter peak-to-peak and RMS, Q, the levels and the threshold - 'level', halfway between the levels by default; with 'levels' 4, PAM4's three eyes, the lowest one's height and width first). A variable swept over a parameter gives one curve per value and, when measured, a table with one row per value (bandwidth against R, overshoot per sample). A table - a .csv, .tsv or .xlsx file, a script's results or a Monte Carlo workbook - is read as a dataset: each column of numbers is a variable, over the first column when it rises (time), otherwise over the row number. Complex values (AC) come as magnitude and phase in degrees unless 'form' says otherwise; statistics and measurements use the magnitude. A variable written as complex numbers with no imaginary part (a Nutmeg equation's db(...)) is read as real, keeping its sign, and the result says so. A variable can be an expression of others - v(out)/v(in), db(ac.v(out)/ac.v(in)) - evaluated sample by sample and measured like any other. 'compare' (a name from simulate's keep_as, or a dataset file) puts another run next to each variable: its statistics and measurements over the same range, and the difference (the largest difference and where, its mean and RMS). When the dataset may not be of the circuit as it is now, 'stale' says why and 'stale certain' whether that is known: true when the netlist a run would be given now differs from the one its run was given (a value set, a wire moved, a value tune tried and put back) or a later run failed; false when only the time of an edit after it tells - which may have changed nothing that is simulated (a text, a diagram, a move); no 'stale': of the circuit as it is.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "A schematic or data display, open or not (the document in front when not given), or a dataset file (.dat, .dat.ngspice, ...)"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"], "description": "Whose dataset of a schematic: the simulator in the settings unless given (else the newest there is)"},
   "variables": {"type": "array", "items": {"type": "string"}, "description": "As get_dataset lists them (tran.v(out)), as a trace names them (ngspice/tran.v(out)), without the analysis (v(out): each analysis's), a node's name (out: its voltage) - or an expression of them: v(out)/v(in), v(out)-v(in), db(ac.v(out)/ac.v(in)), with + - * / ^ and db, abs, mag, phase, real, imag, sqrt, log10, ln, exp, conj, in complex numbers for AC"},
   "compare": {"type": "string", "description": "Another run to compare with: a name simulate's keep_as gave (run1), or a dataset file - each variable gets that run's statistics and measurements, and the difference"},
   "from": {"type": "number", "description": "The start of the x range (time, frequency, the swept value); the first sample when not given"}, "to": {"type": "number", "description": "The end of the x range; the last sample when not given"},
   "points": {"type": "integer", "minimum": 0, "maximum": 5000, "description": "Samples of each curve: 100 unless 'at' or 'measure' is given, then none"},
   "at": {"type": "array", "items": {"type": "number"}, "description": "x values at which each variable is given, interpolated: [1e-3, 2e-3]"},
   "measure": {"type": "array", "items": {"type": "string", "enum": ["rise_time", "fall_time", "overshoot", "settling_time", "period", "frequency", "duty_cycle", "crossings", "bandwidth", "thd", "gain", "phase_margin", "gain_margin", "distribution", "fft", "eye"]}, "description": "Measurements of each variable on its data over the range (the description says what each gives)"},
   "bit_period": {"type": "number", "description": "eye: a bit's length (seconds); when not given, the Tbit of the V(PRBS) source the signal comes from (the nearest to its node) as the run the data is of gave it, else told from the crossings"}, "offset": {"type": "number", "description": "eye: where the eye begins, after the range's start (the settling before it left out)"},
   "levels": {"type": "integer", "enum": [2, 4], "description": "eye: 2 levels (NRZ) or 4 (PAM4: its three eyes); not given, as the V(PRBS) source the signal comes from is coded (PAM4: 4), else 2"},
   "level": {"type": "number", "description": "crossings, period, frequency, duty_cycle: the level crossed (the middle of the swing by default); distribution: the share at or above it"}, "tolerance": {"type": "number", "description": "settling_time: how near the final value counts as settled, a fraction of the step, 0.02 by default"},
   "fundamental": {"type": "number", "description": "thd: the fundamental's frequency in Hz; the curve's own frequency unless given"},
   "harmonics": {"type": "integer", "minimum": 2, "maximum": 100, "description": "thd: the highest harmonic counted, 9 unless given"},
   "periods": {"type": "integer", "minimum": 1, "maximum": 10000, "description": "thd: how many whole periods of the fundamental, ending at 'to' (the end), 1 unless given"},
   "decibels": {"type": "boolean", "description": "Whether the variables' values are in dB (bandwidth: 3 dB below the peak); told from their names and equations when not given"},
   "operating_point": {"type": "boolean", "description": "Only the operating point: every node's value and every device's quantities"},
   "form": {"type": "string", "enum": ["magnitude_phase", "db_phase", "real_imaginary"], "description": "How complex values come: magnitude_phase (the default, degrees), db_phase or real_imaginary"}}}},
{"name": "reload_data",
 "description": "Re-reads the datasets and redraws the diagrams of a document (the one 'path' names, or every open one), like Simulation > Reload Simulation Data - after a simulation run outside Qucs-S, or when a plot is blank. Reports which traces still show no data, and why.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document whose data are read again; every open one when not given"}}}},
{"name": "add_diagram",
 "description": "Places a diagram on a schematic or a data display (a .dpl in 'path', opened if it is not - or 'document': \"data_display\" for the schematic's, made when it has none); its traces show the dataset's data right away. 'type' is rect (x-y, the default), polar, smith, admittance_smith, polar_smith, smith_polar, tab (a table), timing, truth, 3d, locus, histogram or eye (an eye diagram: a transient folded at its unit interval; its own settings in 'eye'). 'x' and 'y' are its lower left corner; if left out, it goes below everything on the schematic, in a free spot (the result says where, and warns when a diagram lies over another one or over parts). 'width' and 'height' default to 240 x 160. 'title' is drawn above it and moves with it as part of the diagram. 'traces' lists variables (as get_dataset names them - tran.v(out), or v(out) or out when that is unambiguous) or objects as add_trace accepts them. The axes, grid, legend and theme are set as edit_diagram accepts them; it starts in the user's default theme for new diagrams. Returns the diagram as get_schematic lists it: its number, and each trace's points or why it has none.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given (a schematic or a data display - a .dpl not open is opened)"}, "document": {"type": "string", "enum": ["schematic", "data_display"], "description": "data_display: on the data display of the schematic 'path' names (its .dpl: opened, made when it has none) - the plots of a report apart from the circuit; schematic (the default): on the document 'path' names"}, "type": {"type": "string", "description": "rect (x-y, the default), polar, smith, admittance_smith, polar_smith, smith_polar, tab (a table), timing, truth, 3d, locus, histogram or eye (an eye diagram: a transient folded at its unit interval; its own settings in 'eye')"}, "x": {"type": "integer", "description": "Its lower left corner (with 'y'); below everything, in a free spot, when not given"}, "y": {"type": "integer", "description": "Its lower left corner (with 'x')"},
   "width": {"type": "integer", "description": "240 by default"}, "height": {"type": "integer", "description": "160 by default"}, "title": {"type": "string", "description": "Drawn above it, and moved, selected and exported with it"},
   "traces": {"type": "array", "items": {"anyOf": [{"type": "string"}, {"type": "object", "properties": {"variable": {"type": "string"}, "axis": {"type": "string"}, "color": {"type": "string"}, "auto_color": {"type": "boolean"}, "thickness": {"type": "integer"}, "style": {"type": "string"}, "marker": {"type": "string"}, "part": {"type": "string"}, "numbers": {"type": "string"}, "precision": {"type": "integer"}}}]}, "description": "Variables as get_dataset names them (tran.v(out); v(out) or out when that is one), or objects as add_trace takes them: {\"variable\": \"ac.v(out)\", \"axis\": \"right\", \"color\": \"#ff0000\", \"part\": \"db\"}"},
   "x_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}, "description": "The x axis: Its label, log (a logarithmic scale), auto (the range from the data) or from, to and step, and units (dB, dBuV, dBm): with log, its numbers are the values in dB (20 log10); a linear axis shows the values as they are - a trace's 'part' db plots them in dB"},
   "y_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}, "description": "The left y axis: Its label, log (a logarithmic scale), auto (the range from the data) or from, to and step, and units (dB, dBuV, dBm): with log, its numbers are the values in dB (20 log10); a linear axis shows the values as they are - a trace's 'part' db plots them in dB"},
   "y2_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}, "description": "The right y axis (for traces with axis right): Its label, log (a logarithmic scale), auto (the range from the data) or from, to and step, and units (dB, dBuV, dBm): with log, its numbers are the values in dB (20 log10); a linear axis shows the values as they are - a trace's 'part' db plots them in dB"},
   "eye": {"type": "object", "properties": {"unit_interval": {"type": ["number", "string", "null"], "description": "A bit's (a PAM4 symbol's) length in seconds - 1e-10 or \"100p\"; null or absent: the Tbit of the V(PRBS) source each trace comes from (the nearest to its node, never through ground; as the run the data is of gave it) - traces of sources of different Tbits each at its own, the time across it then in UI - else told from the first trace's crossings"}, "span": {"type": "integer", "minimum": 1, "maximum": 8, "description": "UIs across it (2 by default: an eye in the middle, half one either side)"}, "from": {"type": ["number", "string", "null"], "description": "The eye from this time on, the settling before it left out (seconds); null: from the start"}, "levels": {"type": ["integer", "null"], "enum": [2, 4, null], "description": "2 (NRZ) or 4 (PAM4: three eyes); null: as each trace's V(PRBS) source is coded (PAM4: 4), else 2 - the default"}, "threshold": {"type": ["number", "string", "null"], "description": "NRZ's decision threshold; null: halfway between the levels"}, "drawn": {"type": "string", "enum": ["density", "traces"], "description": "density (how many traces pass each point, in colour) or the traces"}, "measurements": {"type": "boolean", "description": "Height, width, jitter, levels, Q and the mask's hits beside it, the height and width marked in it"}, "mask": {"type": ["object", "null"], "properties": {"width": {"type": "number", "description": "In UI, above 0 and at most 1"}, "height": {"type": "number", "description": "In the signal's unit"}}, "description": "A hexagon at each eye's centre no trace should enter: how many UIs go through it is measured; null: none"}}, "description": "An eye diagram's own (type eye); what is not given stays. get_schematic lists them, and what was measured on each trace: unit interval, levels, each eye's height, width, jitter, Q and the mask's hits"},
   "grid": {"type": "boolean", "description": "Grid lines drawn (true by default)"}, "legend": {"type": "string", "enum": ["off", "top_left", "top_right", "bottom_left", "bottom_right"], "description": "Where the legend goes, or off - the default, as for a diagram made in the window or written as .sch text without it; with several traces the answer says so"},
   "notation": {"type": "string", "enum": ["automatic", "decimal", "scientific", "power_of_ten", "engineering", "engineering_exponent"], "description": "Of the numbers on its axes and in markers without their own: automatic (2.5e-05), decimal, scientific (1.5e3), power_of_ten, engineering (SI prefixes: 1.5k, the default), engineering_exponent (250e-3)"},
   "decimals": {"type": "integer", "minimum": -1, "maximum": 15, "description": "Their places after the point; -1 as many as each needs"},
   "theme": {"type": "object", "properties": {"preset": {"type": "string", "enum": ["automatic", "light", "dark", "no_background", "default"]}, "background": {"type": "string"}, "plot_area": {"type": "string"}, "frame": {"type": "string"}, "grid": {"type": "string"}, "x_axis": {"type": "string"}, "y_axis": {"type": "string"}, "y2_axis": {"type": "string"}, "title": {"type": "string"}, "text": {"type": "string"}, "legend_background": {"type": "string"}, "legend_border": {"type": "string"}, "legend_text": {"type": "string"}}, "description": "The colours of its parts, each #rrggbb, #aarrggbb (see-through), a name (white, darkgray, ...) or auto (as Qucs-S draws it: no background on light paper, a white one on dark paper, the rest in colours that show on the background). background is under all of it, plot_area inside its frame; an axis' colour is that of its ticks, numbers and label; text is a table's. preset first sets them all - automatic, light, dark, no_background (the canvas shows through), default (the user's for new diagrams) - and the parts given then change that. A part the diagram does not have (a table's legend) is left out"}}}},
{"name": "edit_diagram",
 "description": "Changes a diagram; whatever is not given stays. You can change its position (x, y: the lower left corner) and size, its title (drawn above it, moving with it), its axes (x_axis, y_axis, and y2_axis on the right, each with label, log, auto, from, to, step and units), the notation of its numbers and their decimals, grid, legend and theme (the colours of its background, plot area, frame, grid, axes, title and legend: a preset - light, dark, no_background - and each part's). 'diagram' is its number from get_schematic (it can be left out when there is only one). One undo step; the traces are re-read from the dataset.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "diagram": {"type": "integer", "description": "Its number from get_schematic; may be left out when there is one"},
   "x": {"type": "integer", "description": "Its lower left corner's x"}, "y": {"type": "integer", "description": "Its lower left corner's y"}, "width": {"type": "integer", "description": "Its width"}, "height": {"type": "integer", "description": "Its height"}, "title": {"type": "string", "description": "\"\" takes it away"},
   "x_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}, "description": "The x axis: Its label, log (a logarithmic scale), auto (the range from the data) or from, to and step, and units (dB, dBuV, dBm): with log, its numbers are the values in dB (20 log10); a linear axis shows the values as they are - a trace's 'part' db plots them in dB; what is not given stays"},
   "y_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}, "description": "The left y axis: Its label, log (a logarithmic scale), auto (the range from the data) or from, to and step, and units (dB, dBuV, dBm): with log, its numbers are the values in dB (20 log10); a linear axis shows the values as they are - a trace's 'part' db plots them in dB; what is not given stays"},
   "y2_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}, "description": "The right y axis: Its label, log (a logarithmic scale), auto (the range from the data) or from, to and step, and units (dB, dBuV, dBm): with log, its numbers are the values in dB (20 log10); a linear axis shows the values as they are - a trace's 'part' db plots them in dB; what is not given stays"},
   "eye": {"type": "object", "properties": {"unit_interval": {"type": ["number", "string", "null"], "description": "A bit's (a PAM4 symbol's) length in seconds - 1e-10 or \"100p\"; null or absent: the Tbit of the V(PRBS) source each trace comes from (the nearest to its node, never through ground; as the run the data is of gave it) - traces of sources of different Tbits each at its own, the time across it then in UI - else told from the first trace's crossings"}, "span": {"type": "integer", "minimum": 1, "maximum": 8, "description": "UIs across it (2 by default: an eye in the middle, half one either side)"}, "from": {"type": ["number", "string", "null"], "description": "The eye from this time on, the settling before it left out (seconds); null: from the start"}, "levels": {"type": ["integer", "null"], "enum": [2, 4, null], "description": "2 (NRZ) or 4 (PAM4: three eyes); null: as each trace's V(PRBS) source is coded (PAM4: 4), else 2 - the default"}, "threshold": {"type": ["number", "string", "null"], "description": "NRZ's decision threshold; null: halfway between the levels"}, "drawn": {"type": "string", "enum": ["density", "traces"], "description": "density (how many traces pass each point, in colour) or the traces"}, "measurements": {"type": "boolean", "description": "Height, width, jitter, levels, Q and the mask's hits beside it, the height and width marked in it"}, "mask": {"type": ["object", "null"], "properties": {"width": {"type": "number", "description": "In UI, above 0 and at most 1"}, "height": {"type": "number", "description": "In the signal's unit"}}, "description": "A hexagon at each eye's centre no trace should enter: how many UIs go through it is measured; null: none"}}, "description": "An eye diagram's own (type eye); what is not given stays. get_schematic lists them, and what was measured on each trace: unit interval, levels, each eye's height, width, jitter, Q and the mask's hits"},
   "grid": {"type": "boolean", "description": "Grid lines drawn or not"}, "legend": {"type": "string", "enum": ["off", "top_left", "top_right", "bottom_left", "bottom_right"], "description": "Where the legend goes, or off"},
   "notation": {"type": "string", "enum": ["automatic", "decimal", "scientific", "power_of_ten", "engineering", "engineering_exponent"], "description": "Of the numbers on its axes and in markers without their own: automatic (2.5e-05), decimal, scientific (1.5e3), power_of_ten, engineering (SI prefixes: 1.5k, the default), engineering_exponent (250e-3)"},
   "decimals": {"type": "integer", "minimum": -1, "maximum": 15, "description": "Their places after the point; -1 as many as each needs"},
   "theme": {"type": "object", "properties": {"preset": {"type": "string", "enum": ["automatic", "light", "dark", "no_background", "default"]}, "background": {"type": "string"}, "plot_area": {"type": "string"}, "frame": {"type": "string"}, "grid": {"type": "string"}, "x_axis": {"type": "string"}, "y_axis": {"type": "string"}, "y2_axis": {"type": "string"}, "title": {"type": "string"}, "text": {"type": "string"}, "legend_background": {"type": "string"}, "legend_border": {"type": "string"}, "legend_text": {"type": "string"}}, "description": "The colours of its parts (what is not given stays), each #rrggbb, #aarrggbb (see-through), a name (white, darkgray, ...) or auto (as Qucs-S draws it: no background on light paper, a white one on dark paper, the rest in colours that show on the background). background is under all of it, plot_area inside its frame; an axis' colour is that of its ticks, numbers and label; text is a table's. preset first sets them all - automatic, light, dark, no_background (the canvas shows through), default (the user's for new diagrams) - and the parts given then change that. A part the diagram does not have (a table's legend) is left out"}}}},
{"name": "add_trace",
 "description": "Adds a trace to a diagram. 'variable' is named as get_dataset names it (tran.v(out); v(out) or out when unambiguous; the simulator's prefix is added). You can set its color (#rrggbb, a name, or auto), thickness, style (solid, dash, dot, long_dash, stars, circles, arrows), the y axis it is drawn on (left or right), point markers (none, auto, circle, square, triangle, diamond, triangle_down, cross, plus) and auto_color (each swept curve its own color); 'part' plots a part of each value - db, phase (degrees), magnitude, real or imaginary - so a complex S-parameter or an imported one shows in dB or as its phase with no equation (a Cartesian diagram or a table); in a table, precision and numbers (real_imaginary, magnitude_degrees, magnitude_radians). Returns its points, or why it shows nothing.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "diagram": {"type": "integer", "description": "The diagram's number from get_schematic; may be left out when there is one"}, "variable": {"type": "string", "description": "As get_dataset names it: tran.v(out) (v(out) or out when that is one); the simulator's prefix is added - not to name:variable of an imported dataset (import_data), which has none"},
   "color": {"type": "string", "description": "#rrggbb, a colour's name, or auto"}, "thickness": {"type": "integer", "minimum": 0, "maximum": 99, "description": "Line width in pixels, 0-99"},
   "style": {"type": "string", "enum": ["solid", "dash", "dot", "long_dash", "stars", "circles", "arrows"], "description": "How the curve is drawn"},
   "axis": {"type": "string", "enum": ["left", "right"], "description": "The y axis it is drawn against: left (the default) or right"},
   "marker": {"type": "string", "enum": ["none", "auto", "circle", "square", "triangle", "diamond", "triangle_down", "cross", "plus"], "description": "A mark at each point, or none"},
   "part": {"type": "string", "enum": ["auto", "magnitude", "db", "phase", "real", "imaginary"], "description": "What of each value it plots: auto (a complex value's magnitude, a real one as it is - the default), magnitude, db (20 log10 of the magnitude), phase (degrees), real or imaginary - on a Cartesian diagram or a table; the legend and axis name it dB(...), phase(...)"},
   "auto_color": {"type": "boolean", "description": "Each curve of a sweep its own colour"}, "precision": {"type": "integer", "minimum": 0, "maximum": 16, "description": "A table's digits, 0-16"},
   "numbers": {"type": "string", "enum": ["real_imaginary", "magnitude_degrees", "magnitude_radians"], "description": "How a table shows complex values"}},
   "required": ["variable"]}},
{"name": "edit_trace",
 "description": "Changes a trace of a diagram - 'trace' is its number in the diagram or its variable - with the same fields add_trace accepts: another variable, color, thickness, style, axis, markers and so on. Whatever is not given stays. One undo step.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "diagram": {"type": "integer", "description": "The diagram's number from get_schematic; may be left out when there is one"}, "trace": {"description": "Its number (from 1) or its variable"},
   "variable": {"type": "string", "description": "Another variable for it, as get_dataset names it"}, "color": {"type": "string", "description": "#rrggbb, a colour's name, or auto"}, "thickness": {"type": "integer", "minimum": 0, "maximum": 99, "description": "Line width in pixels, 0-99"},
   "style": {"type": "string", "enum": ["solid", "dash", "dot", "long_dash", "stars", "circles", "arrows"], "description": "How the curve is drawn"},
   "axis": {"type": "string", "enum": ["left", "right"], "description": "The y axis it is drawn against: left or right"},
   "marker": {"type": "string", "enum": ["none", "auto", "circle", "square", "triangle", "diamond", "triangle_down", "cross", "plus"], "description": "A mark at each point, or none"},
   "part": {"type": "string", "enum": ["auto", "magnitude", "db", "phase", "real", "imaginary"], "description": "What of each value it plots: auto (a complex value's magnitude, a real one as it is - the default), magnitude, db (20 log10 of the magnitude), phase (degrees), real or imaginary - on a Cartesian diagram or a table; the legend and axis name it dB(...), phase(...)"},
   "auto_color": {"type": "boolean", "description": "Each curve of a sweep its own colour"}, "precision": {"type": "integer", "minimum": 0, "maximum": 16, "description": "A table's digits, 0-16"},
   "numbers": {"type": "string", "enum": ["real_imaginary", "magnitude_degrees", "magnitude_radians"], "description": "How a table shows complex values"}}}},
{"name": "add_marker",
 "description": "Places a marker on a trace of a diagram; it shows the sample nearest to where it is placed, and the value there. 'at' is an x value, or a point on the trace: peak (or max), min, -3dB (3 dB below the peak - for a curve in dB, the peak minus 3; for a magnitude, the peak divided by sqrt(2) - after the peak first, otherwise before it; 'reference' dc or a level, such as 0 dB for a filter specification, measures from that instead), or crossing:<y> (the first crossing of y). Returns the marker as get_schematic lists it and what 'at' found (the exact crossing, and the sample the marker is on). 'label' places its box's top left corner at [x, y] on the schematic; 'label_offset' [dx, dy] places it that far from the marked point (y pointing down). Also 'precision' (digits), 'format' for complex values, a 'transparent' background, an 'indicator' at the point, and text and background colors (#rrggbb, #aarrggbb, a name, or auto). One undo step.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "diagram": {"type": "integer", "description": "The diagram's number from get_schematic; may be left out when there is one"}, "trace": {"description": "Its number (from 1) or its variable"},
   "at": {"description": "An x value (a number), or \"peak\", \"max\", \"min\", \"-3dB\", \"crossing:<y>\""},
   "reference": {"description": "What -3dB is 3 dB below: \"peak\" (the default), \"dc\" (the value at the curve's start, the lowest frequency), or a level - 0 for a filter's spec in dB (on a magnitude, 1 is a gain of one)"},
   "label": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Where its box's top left corner goes, [x, y] on the schematic"},
   "label_offset": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Its box [dx, dy] from the marked point (y pointing down), instead of 'label'"},
   "precision": {"type": "integer", "minimum": 0, "maximum": 12, "description": "Significant digits (automatic notation) or places after the point, 0-12"},
   "format": {"type": "string", "enum": ["real_imaginary", "magnitude_degrees", "magnitude_radians"], "description": "How a complex value is shown"},
   "notation": {"type": "string", "enum": ["diagram", "automatic", "decimal", "scientific", "power_of_ten", "engineering", "engineering_exponent"], "description": "How its numbers are written: as its diagram's axes (the default), or a notation of its own (edit_diagram's)"},
   "transparent": {"type": "boolean", "description": "No background behind its text"}, "indicator": {"type": "string", "enum": ["off", "square", "triangle"], "description": "A mark at the point: square, triangle or off"},
   "text_color": {"type": "string", "description": "#rrggbb, #aarrggbb, a colour's name, or auto"}, "fill_color": {"type": "string", "description": "Its background: #rrggbb, #aarrggbb, a colour's name, or auto"}},
  "required": ["at"]}},
{"name": "edit_marker",
 "description": "Changes a marker of a diagram - 'marker' is its number from get_schematic - with the same fields add_marker accepts: 'at' moves it (its label moves along), and its label, precision, notation, format, transparency, indicator and colors. Whatever is not given stays. One undo step.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "diagram": {"type": "integer", "description": "The diagram's number from get_schematic; may be left out when there is one"}, "marker": {"type": "integer", "description": "The marker's number from get_schematic"},
   "at": {"description": "An x value (a number), or \"peak\", \"max\", \"min\", \"-3dB\", \"crossing:<y>\""},
   "reference": {"description": "What -3dB is 3 dB below: \"peak\" (the default), \"dc\" (the value at the curve's start, the lowest frequency), or a level - 0 for a filter's spec in dB (on a magnitude, 1 is a gain of one)"},
   "label": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Where its box's top left corner goes, [x, y] on the schematic"},
   "label_offset": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Its box [dx, dy] from the marked point (y pointing down)"},
   "precision": {"type": "integer", "minimum": 0, "maximum": 12, "description": "Significant digits (automatic notation) or places after the point, 0-12"},
   "format": {"type": "string", "enum": ["real_imaginary", "magnitude_degrees", "magnitude_radians"], "description": "How a complex value is shown"},
   "notation": {"type": "string", "enum": ["diagram", "automatic", "decimal", "scientific", "power_of_ten", "engineering", "engineering_exponent"], "description": "How its numbers are written: as its diagram's axes (the default), or a notation of its own (edit_diagram's)"},
   "transparent": {"type": "boolean", "description": "No background behind its text"}, "indicator": {"type": "string", "enum": ["off", "square", "triangle"], "description": "A mark at the point: square, triangle or off"},
   "text_color": {"type": "string", "description": "#rrggbb, #aarrggbb, a colour's name, or auto"}, "fill_color": {"type": "string", "description": "Its background: #rrggbb, #aarrggbb, a colour's name, or auto"}}}},
{"name": "delete_marker",
 "description": "Deletes a marker from a diagram ('marker' is its number from get_schematic). One undo step.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "diagram": {"type": "integer", "description": "The diagram's number from get_schematic; may be left out when there is one"}, "marker": {"type": "integer", "description": "The marker's number from get_schematic"}}}},
{"name": "rename_net",
 "description": "Renames a net - its labels change, and a net get_schematic calls net1, net2, ... gets a label - together with everything that names its voltage: the traces of the schematic's diagrams and of its data displays (open ones as an undoable change; the .dpl file of a closed one is rewritten) and its equations, so v(out) becomes v(out1) and out.v becomes out1.v. It is refused when another net already has the new name, since that would join the two. The dataset keeps the old name until the next simulation.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "from": {"type": "string", "description": "The net's name now: a label's, or net1, net2, ... as get_schematic calls it"}, "to": {"type": "string", "description": "Its new name, as the label dialog takes one (a letter, then letters, digits and single _): refused when another net has it, and for gnd, 0 and net1, net2 ..."}}, "required": ["from", "to"]}},
{"name": "describe_component_type",
 "description": "Describes a library component type: what it is, its category, how its parts are named, its pins (their positions relative to its center at rotation 0, how a turn and a mirror move them, or - with 'rotation' and 'mirror' - where they are on a part placed so), its properties in order, and those it may have any number of (.NGOPT's Knob, Target and Constraint: their fields and an example) - name, default value, unit, meaning, whether shown on the schematic and which simulators it applies to. It also gives the simulators the type works with, the netlist line it produces with its defaults for the simulator from the settings, and notes on common mistakes (a Vpulse is a single pulse; Vrect repeats), including properties hidden on the schematic that still go into the netlist and where (an OpAmp's Umax clips its output). With type \"Verilog-A\" it instead returns a new Verilog-A module to start from - a template OpenVAF compiles as it is - and how to write one (attributes before the declaration, desc, units, type=\"instance\", contributions, a DC path for every node).",
 "inputSchema": {"type": "object", "properties": {"type": {"type": "string", "description": "As list_component_types gives it: R, Vpulse, .TR, ...; a Verilog-A module's name or file; \"Verilog-A\" for a template of a new one"},
   "rotation": {"type": "integer", "minimum": 0, "maximum": 3, "description": "The pins as a part turned so is placed (add_component's rotation: a quarter turn each)"},
   "mirror": {"type": "boolean", "description": "The pins as a mirrored part's (mirrored first, then turned)"}}, "required": ["type"]}},
{"name": "batch",
 "description": "Runs several of these tools in one call, in order - much faster than one call each: place and wire a circuit, set many properties, add a diagram and its traces at once. Each change is one step of Edit > Undo, as when called alone. It stops at the first call that fails unless 'keep_going' is set, and then reports how many changes the earlier calls made (undo with 'steps' takes them back); with 'atomic' those changes are undone at once, so it is all or nothing (files written, simulations run and documents opened stay). With 'preview' it holds only calls that change schematics or only look. Returns each call's result in order.",
 "inputSchema": {"type": "object", "properties": {
   "calls": {"type": "array", "minItems": 1, "items": {"type": "object", "properties": {
     "tool": {"type": "string", "description": "A tool's name: add_component, connect, ..."},
     "arguments": {"type": "object"}}, "required": ["tool"]}, "description": "The calls in order: [{\"tool\": \"add_component\", \"arguments\": {...}}, ...]; not another batch"},
   "keep_going": {"type": "boolean", "description": "Go on after one that fails"}, "brief": {"type": "boolean", "description": "Each call that succeeds said in a line (what it made: a part's name, type and place, a note) instead of its whole answer; those that fail in full"},
   "atomic": {"type": "boolean", "description": "All or nothing: when one fails, the changes of those before it are undone"}}, "required": ["calls"]}},
{"name": "add_painting",
 "description": "Draws a painting - a text, arrow, line, box, text box, table, dimension or formula - on a schematic, or on its symbol with 'symbol' (the document switches to show its symbol, like Edit Circuit Symbol; a .sym file is all symbol). Use it to annotate a result, label part of the circuit or draw a subcircuit's symbol. 'type' is text, line, arrow, rectangle, ellipse, arc, polyline, image (from 'file'), rounded_rectangle, polygon, brace, waveform, text_box (kind block, note or callout, with a 'tip' it points at), table, dimension or formula (TeX). Its fields go by name: a text's x, y, text (_x or _{xy} for a subscript, ^ for a superscript, as in TeX), size, color and angle; a line's or arrow's from and to ([x, y]) and an arrow's head (open or filled); a box's x, y (top left corner), width and height; color, thickness, style, fill_color, fill_style, filled and so on. describe_format with element painting lists every type's fields. Returns it as get_schematic lists it, with its number. One undo step.",
 "inputSchema": {"type": "object", "additionalProperties": true, "properties": {
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "symbol": {"type": "boolean", "description": "On the schematic's symbol"},
   "type": {"type": "string", "enum": ["text", "line", "arrow", "rectangle", "ellipse", "arc", "polyline", "image", "rounded_rectangle", "polygon", "brace", "waveform", "text_box", "table", "dimension", "formula"], "description": "What is drawn (describe_format with element painting lists each type's fields)"},
   "x": {"type": "integer", "description": "A text's or formula's place, a box's top left corner, in schematic units, on the grid (usually 10)"}, "y": {"type": "integer", "description": "A text's or formula's place, a box's top left corner, in schematic units, on the grid (usually 10)"}, "width": {"type": "integer", "description": "A box's, ellipse's, image's, table's or text box's width"}, "height": {"type": "integer", "description": "Its height"},
   "from": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "A line's, arrow's or dimension's first end, [x, y]"}, "to": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Its other end, [x, y]"},
   "points": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}}, "description": "A polyline's or polygon's points, [[x, y], ...]"},
   "text": {"type": "string", "description": "A text's or text box's text (_x, ^x in a text)"}, "tex": {"type": "string", "description": "A formula's TeX math, without dollars: \\frac{V_{out}}{V_{in}}"}, "display": {"type": "boolean", "description": "A formula in display style (larger fractions, limits above and below)"}, "size": {"type": "integer", "description": "A text's font size, 1-400"}, "color": {"type": "string", "description": "The line's or text's colour: #rrggbb or a colour's name"}, "thickness": {"type": "integer", "description": "The line's width, 0-100"},
   "style": {"type": "string", "description": "The line: none, solid, dash, dot, dash_dot or dash_dot_dot"}, "fill_color": {"type": "string", "description": "The fill's colour: #rrggbb or a colour's name"}, "fill_style": {"type": "string", "description": "The fill: none, solid, dense1-dense7, horizontal, vertical, cross, backward_diagonal, forward_diagonal or diagonal_cross"}, "filled": {"type": "boolean", "description": "Whether a box, ellipse or polygon is filled"},
   "angle": {"type": "integer", "description": "A text's angle in degrees, -360 to 360"}, "head": {"type": "string", "enum": ["open", "filled"], "description": "An arrow's head: open or filled"}, "file": {"type": "string", "description": "An image's file (PNG, JPEG, SVG ...)"},
   "around": {"type": "string", "enum": ["selection"], "description": "Placed by what the user selected: a box about it, a text above it, an arrow to it, a brace beside it, a dimension under it (the fields given stay)"}},
   "required": ["type"]}},
{"name": "edit_painting",
 "description": "Changes a painting by the fields add_painting accepts for its type - 'painting' is its number from get_schematic ('symbol' for a painting of the symbol). Whatever is not given stays, and it keeps its type. A symbol port can be moved (x, y) and given a 'label': what its instances show next to the pin instead of the port's name (the net's name, which the netlist keeps) - \"+\" for inp, \"\" for nothing, null for the name again. The symbol's name text can be moved and given its 'prefix' (SUB makes the instances SUB1, SUB2, ...) and 'parameters' - a subcircuit's parameters, [{\"name\": \"R\", \"default\": \"1k\", \"description\": ..., \"type\": ..., \"shown\": true}], which each instance then takes (save the file so they see it). Returns it as get_schematic lists it. One undo step.",
 "inputSchema": {"type": "object", "additionalProperties": true, "properties": {
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "symbol": {"type": "boolean", "description": "The painting is the symbol's"}, "painting": {"type": "integer", "description": "Its number from get_schematic (its 'symbol paintings' with 'symbol')"},
   "x": {"type": "integer", "description": "A text's or formula's place, a box's top left corner, a symbol port's place"}, "y": {"type": "integer", "description": "A text's or formula's place, a box's top left corner, a symbol port's place"}, "width": {"type": "integer", "description": "A box's, ellipse's, image's, table's or text box's width"}, "height": {"type": "integer", "description": "Its height"},
   "from": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "A line's, arrow's or dimension's first end, [x, y]"}, "to": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Its other end, [x, y]"},
   "points": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}}, "description": "A polyline's or polygon's points, [[x, y], ...]"},
   "text": {"type": "string", "description": "A text's or text box's text"}, "tex": {"type": "string", "description": "A formula's TeX math, without dollars"}, "display": {"type": "boolean", "description": "A formula in display style (larger fractions, limits above and below)"}, "size": {"type": "integer", "description": "A text's font size, 1-400"}, "color": {"type": "string", "description": "The line's or text's colour: #rrggbb or a colour's name"}, "thickness": {"type": "integer", "description": "The line's width, 0-100"},
   "style": {"type": "string", "description": "The line: none, solid, dash, dot, dash_dot or dash_dot_dot"}, "fill_color": {"type": "string", "description": "The fill's colour: #rrggbb or a colour's name"}, "fill_style": {"type": "string", "description": "The fill: none, solid, dense1-dense7, horizontal, vertical, cross, backward_diagonal, forward_diagonal or diagonal_cross"}, "filled": {"type": "boolean", "description": "Whether a box, ellipse or polygon is filled"},
   "angle": {"type": "integer", "description": "A text's angle in degrees, -360 to 360"}, "head": {"type": "string", "enum": ["open", "filled"], "description": "An arrow's head: open or filled"}, "file": {"type": "string", "description": "An image's file"}},
   "required": ["painting"]}},
{"name": "list_documents",
 "description": "Lists the files of the workspace, a project or a folder - schematics, symbols, data displays, datasets, netlists, texts, PDFs, spreadsheets, pictures - newest first, each with its path (relative to the folder listed), kind, size, modification time and whether it is open. A dataset also says which simulator wrote it, which schematic it belongs to, and which traces of open diagrams read it but find nothing there; an open schematic lists the traces whose dataset does not exist at all (ngspice/... reads name.dat.ngspice - the usual Qucsator-versus-ngspice mix-up, or an imported dataset's trace with a prefix: import_data's name.dat is read as name:variable). An imported dataset says which file it came from. Without 'folder' it lists the open project's files, or the workspace's when no project is open - the workspace's projects named either way. 'folder' is a project's name (amp or amp_prj) or a folder (relative to the open project's folder, else the workspace; or absolute - the workspace's path lists the workspace). 'kind' keeps one kind, 'search' the files whose name contains it, and 'sort' is newest (the default) or name. Subfolders are included up to 4 levels deep; at most 300 files, with what was left out.",
 "inputSchema": {"type": "object", "properties": {
   "folder": {"type": "string", "description": "A project's name (amp or amp_prj) or a folder, relative to the open project's folder (else the workspace) or absolute; the open project (else the workspace) when not given"},
   "kind": {"type": "string", "enum": ["schematic", "symbol", "data display", "dataset", "netlist", "text", "pdf", "spreadsheet", "markdown", "picture", "verilog-a"], "description": "Only files of this kind"},
   "search": {"type": "string", "description": "Only files whose name contains it"}, "sort": {"type": "string", "enum": ["newest", "name"], "description": "newest first (the default) or by name"}}}},
{"name": "export_image",
 "description": "Writes a picture of a schematic, symbol or data display (the one in front unless 'path' names another) to a file, like File > Export as image without its dialog. 'save_as' is a path, or a name in the project's folder (otherwise the workspace's); an existing file is overwritten, and its suffix gives the format when 'format' is not given. 'format' is png, jpeg, bmp, tiff, webp, svg, pdf, eps or pdf_tex (a PDF with its text in a LaTeX file next to it). 'scale' is a raster image's pixels per schematic unit (1 is 96 dpi; 2 by default). 'colours' is colour (the default), grayscale or monochrome. 'transparent' leaves out the paper where the format allows it. 'diagram' (its number from get_schematic) exports that diagram alone, for example a frequency response for a report, and 'selection' only what is selected. Returns the file, its format and its size in pixels or units.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "save_as": {"type": "string", "description": "The file: a path, or a name in the project's folder (else the workspace's); written over when it is there. Its suffix gives the format when 'format' is not given"},
   "format": {"type": "string", "enum": ["png", "jpeg", "bmp", "tiff", "webp", "svg", "pdf", "eps", "pdf_tex"], "description": "The picture's format; pdf_tex is a PDF with its text in a LaTeX file beside it"},
   "scale": {"type": "number", "minimum": 0.1, "maximum": 20, "description": "A raster picture's pixels per schematic unit: 2 by default (1 is 96 dpi)"}, "colours": {"type": "string", "enum": ["colour", "grayscale", "monochrome"], "description": "colour (the default), grayscale or monochrome"},
   "transparent": {"type": "boolean", "description": "No paper behind it, where the format allows"}, "diagram": {"type": "integer", "description": "Only this diagram, by its number"}, "selection": {"type": "boolean", "description": "Only what is selected"}},
   "required": ["save_as"]}},
{"name": "build_verilog_a",
 "description": "Compiles a Verilog-A source with OpenVAF (the one set under Application Settings > Locations), like Build Verilog-A, and waits for it, so a syntax error shows up now rather than at the next simulation. 'file' is the .va file (relative to the project, otherwise the workspace; the .va document in front if not given); an open file with unsaved changes needs 'unsaved': save or as_saved. Returns whether it compiled, its errors and warnings - each with message, line, column, the source line and OpenVAF's marks - the .osdi file written and its modules: beside the source, or in Qucs-S's cache when the source's folder cannot be written (a read-only library folder) or the .osdi beside it is another platform's (kept; 'into the cache' says which) - where a simulation loads it from either way. describe_component_type with a module's name then lists its parameters.",
 "inputSchema": {"type": "object", "properties": {"file": {"type": "string", "description": "The .va file, relative to the project (else the workspace); the .va document in front when not given"}, "unsaved": {"type": "string", "enum": ["save", "as_saved"], "description": "For an open file with unsaved changes: save them first, or build the file as saved"},
   "timeout": {"type": "integer", "description": "Seconds, 120 unless given"}}}},
{"name": "tune",
 "description": "Finds the value that makes a measurement come out right: it sets a component's property, simulates, measures, and repeats - searching 'range' for the value that brings the measurement to 'target' (false position, on a logarithmic scale across decades, so a few runs), within 'tolerance' (0.5% of the target by default) and at most 'max_runs' (12) simulations. With 'values' it simulates and measures each value and returns a table (with a target, the closest is chosen). 'measure' is {\"variable\": \"tran.v(out)\", \"what\": \"final\"}, where 'what' is min, max, mean, rms, initial, final, peak_to_peak or one of get_dataset's measurements (bandwidth, overshoot, rise_time, settling_time, frequency, gain, thd, phase_margin, ...: its value, or 'field'); 'at' takes an x value instead, 'from' and 'to' the range, plus the measurement's options as get_dataset accepts them. Or {\"operating_point\": \"e\"} measures a node's DC voltage (or a device quantity, Q1.ic), running only the operating point each time. The value found is set as one undo step ('apply' false leaves the part as it was) and simulated, so the diagrams show it. Returns each run's value and measurement, the value found and what it gives. An untitled schematic is saved in the scratch folder first. With 'knobs' (2 to 4 parts, each a range) and as many 'targets' it tunes them together (Broyden's method: a run for each knob to begin with, then a few), setting the values found as one undo step - a gain and an input resistance from Rf and Rg. 'hold' keeps other measurements within bounds (a gain of 11 while the bandwidth stays above 50 kHz): each run measures them, and only a value that keeps them all is set - one that reaches the target but breaks a hold is told, not taken. 'compare' (on with 'hold') runs the values as they are first and gives 'before and after', every measurement's value then and with the value found. Examples: sweep RE until the emitter sits at 5 V; sweep C until the peaking is 1 dB.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given; an untitled one is saved in the scratch folder first"}, "component": {"type": "string", "description": "The part whose property is tuned, by name"}, "property": {"type": "string", "description": "Its first property unless given (R of a resistor); of an equation block, a variable it defines"},
   "target": {"type": "number", "description": "The number the measurement is to come to"}, "range": {"type": "array", "items": {}, "minItems": 2, "maxItems": 2, "description": "[low, high]: numbers, or text with units (1k)"},
   "values": {"type": "array", "items": {}, "description": "Instead of a search: each value simulated and measured, [\"1k\", \"2.2k\", 4700] (40 at most)"},
   "measure": {"type": "object", "properties": {"variable": {"type": "string"}, "what": {"type": "string"}, "field": {"type": "string"}, "at": {"type": "number"}, "from": {"type": "number"}, "to": {"type": "number"}, "operating_point": {"type": "string"}, "level": {"type": "number"}, "tolerance": {"type": "number"}, "fundamental": {"type": "number"}, "harmonics": {"type": "integer"}, "periods": {"type": "number"}, "decibels": {"type": "boolean"}, "form": {"type": "string"}, "simulator": {"type": "string"}}, "description": "What is measured after each run: {\"variable\": \"tran.v(out)\", \"what\": \"final\"} - 'what' min, max, mean, rms, initial, final, peak_to_peak or a get_dataset measurement (bandwidth, overshoot, gain, ...; 'field' picks one of its numbers); 'at' an x value instead; 'from', 'to' and the measurement's options as get_dataset takes them. Or {\"operating_point\": \"e\"}: a node's DC voltage or a device's quantity (Q1.ic)"}, "tolerance": {"type": "number", "description": "How near the target is near enough: 0.5% of the target by default"}, "max_runs": {"type": "integer", "minimum": 2, "maximum": 60, "description": "Simulations at most, 12 by default (24 with knobs)"},
   "knobs": {"type": "array", "items": {"type": "object", "properties": {"component": {"type": "string"}, "property": {"type": "string"}, "range": {"type": "array", "items": {}, "minItems": 2, "maxItems": 2}}}, "description": "Instead of 'component': 2 to 4 parts tuned together, [{\"component\": \"RF\", \"range\": [\"1k\", \"100k\"]}, {\"component\": \"RG\", \"range\": [\"100\", \"10k\"]}], for as many 'targets'"},
   "targets": {"type": "array", "items": {"type": "object", "properties": {"measure": {"type": "object", "properties": {"variable": {"type": "string"}, "what": {"type": "string"}, "field": {"type": "string"}, "at": {"type": "number"}, "from": {"type": "number"}, "to": {"type": "number"}, "operating_point": {"type": "string"}, "level": {"type": "number"}, "tolerance": {"type": "number"}, "fundamental": {"type": "number"}, "harmonics": {"type": "integer"}, "periods": {"type": "number"}, "decibels": {"type": "boolean"}, "form": {"type": "string"}, "simulator": {"type": "string"}}}, "target": {"type": "number"}, "tolerance": {"type": "number"}}}, "description": "With 'knobs': one target for each knob, [{\"measure\": {...as 'measure'}, \"target\": 20, \"tolerance\": 0.1}, ...]; all of the operating point or all of the analyses"},
   "hold": {"type": "array", "items": {"type": "object", "properties": {"measure": {"type": "object", "properties": {"variable": {"type": "string"}, "what": {"type": "string"}, "field": {"type": "string"}, "at": {"type": "number"}, "from": {"type": "number"}, "to": {"type": "number"}, "operating_point": {"type": "string"}, "level": {"type": "number"}, "tolerance": {"type": "number"}, "fundamental": {"type": "number"}, "harmonics": {"type": "integer"}, "periods": {"type": "number"}, "decibels": {"type": "boolean"}, "form": {"type": "string"}, "simulator": {"type": "string"}}, "description": "As 'measure'"}, "min": {"type": "number"}, "max": {"type": "number"}}}, "description": "Measurements to keep within bounds, [{\"measure\": {\"variable\": \"ac.v(out)\", \"what\": \"bandwidth\"}, \"min\": 50e3}]: of the operating point, or of the analyses, as the target is"},
   "allow_commands": {"type": "boolean", "description": "As simulate takes it: run the commands the schematic carries besides the simulator, at every run"},
   "compare": {"type": "boolean", "description": "Run the values as they are first, and give every measurement before and after (on with 'hold')"},
   "apply": {"type": "boolean", "description": "Set the value found (the default), or leave the part as it was"}, "simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"], "description": "For these runs alone; the one in the settings by default"},
   "timeout": {"type": "integer", "description": "Seconds for each run, 120 unless given"}},
   "required": ["component", "measure"]}},
{"name": "get_text",
 "description": "Reads a text document open in a tab (.cir, .va, .m, .py, .txt, ...) as it is in the window, its unsaved edits included: each line with its number ('12| text'), 'lines' (how many), 'revision' (give it to edit_text), 'unsaved', the cursor and the selection, and the errors and warnings marked in it. 'from_line' and 'to_line' read a part. A file not open: open_document opens it.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "from_line": {"type": "integer", "description": "The first line read (from 1)"}, "to_line": {"type": "integer", "description": "The last line read"}}}},
{"name": "edit_text",
 "description": "Edits a text document open in a tab as one step of its undo (Edit > Undo takes all of it back), keeping the user's unsaved edits. 'edits' are made in order, each on the text the ones before it left: {\"find\": exact text, \"replace\": its new text} - refused when it is not there, or there more than once unless 'all': true -, or {\"lines\": [first, last], \"text\": the lines in their place} (\"\" takes them away; [n, n - 1] puts the text before line n). All are made or none. 'revision' (get_text's) refuses the edits when the text changed since - the user typed. It is not saved: save_document saves it.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"},
   "edits": {"type": "array", "items": {"type": "object", "properties": {"find": {"type": "string"}, "replace": {"type": "string"}, "all": {"type": "boolean"}, "lines": {"type": "array", "items": {"type": "integer"}}, "text": {"type": "string"}}}, "description": "[{\"find\": ..., \"replace\": ..., \"all\": false}] or [{\"lines\": [first, last], \"text\": ...}], in order"},
   "revision": {"type": "number", "description": "get_text's 'revision': refused when the text changed since"}}, "required": ["edits"]}},
{"name": "goto_line",
 "description": "Brings a text document to the front with a line in the middle of its view and the cursor there - to show the user an error or a place in the text. The keyboard stays where it is.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"}, "line": {"type": "integer", "description": "The line, from 1"}, "column": {"type": "integer", "description": "The column, from 1 (1 unless given)"}}, "required": ["line"]}},
{"name": "read_pdf",
 "description": "Reads the text of a PDF - a datasheet, an application note, a report - page by page, for example to take a model's parameters or a table's values from it. 'path' is relative to the project, otherwise the workspace (the PDF in front if not given); 'pages' is [3, 4], \"2-5\" or 7 (the first 3 by default); 'search' finds a word or value on every page (or those given) and returns the lines around each hit. A scanned page has no text: a screenshot of its tab shows it.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The PDF, relative to the project (else the workspace); the PDF in front when not given"}, "pages": {"description": "Which pages: [3, 4], \"2-5\" or 7; the first 3 by default (with 'search', all)"}, "search": {"type": "string", "description": "A word or value to find: the lines around each hit"}}}},
{"name": "describe_part",
 "description": "What a library part is, in one call: its pins in order, each with its name, the side of the symbol it is on and its role (input, output, supply); its supply pins; what its model is - one component placed as that component, a macromodel of controlled sources, a transistor-level subcircuit - with the count of its elements; how the test of every library part under ngspice found it; its description; and 'place' for add_component. find_library_component finds the part.",
 "inputSchema": {"type": "object", "properties": {"library": {"type": "string", "description": "The library, as find_library_component gives it: OpAmps"}, "part": {"type": "string", "description": "The part in it: uA741"}}, "required": ["library", "part"]}},
{"name": "find_library_component",
 "description": "Searches the component libraries - Qucs-S's own, the project's, the user's (user_lib) and those of the library search paths (get_settings app, Locations/Library search paths) - and the SPICE model files (.model cards in .lib, .mod, .inc and .cir files) of the project and the workspace for a part by what it is and by its values. 'search' matches words in its name or description (2N3904, NPN 40V); 'type' is npn, pnp, nmos, pmos, njf, pjf, diode or a Qucs model (_BJT, _MOSFET, Diode, ...); 'near' gives parameter values ({\"Bf\": 200}, nearest first on a logarithmic scale); 'library' limits it to one library. Returns each part with its library, description, the values asked about and how to place it: a Qucs library part is add_component with type Lib and its Lib and Comp ('placed as' names the component it becomes when its model is one component with the library's values - a Diode, a _BJT); a SPICE model comes with its .model card. A plain resistor, capacitor or inductor is add_component R, C or L with its value. A library of your own: create_library; one from elsewhere: import_library; every library: list_libraries. Each library part says how it fared under ngspice ('ngspice'): tested - it netlists and its operating point converges, each pin to ground through 1 MOhm (a smoke test, not of what it does) - or failing, and why; 'tested' lists only those that pass.",
 "inputSchema": {"type": "object", "properties": {"search": {"type": "string", "description": "Words in its name or description: 2N3904, NPN 40V"}, "type": {"type": "string", "description": "npn, pnp, nmos, pmos, njf, pjf, diode, or a Qucs model (_BJT, _MOSFET, Diode, ...)"}, "near": {"type": "object", "description": "Parameter values, nearest first on a logarithmic scale: {\"Bf\": 200}"},
   "library": {"type": "string", "description": "Only this library, by name"}, "limit": {"type": "integer", "minimum": 1, "maximum": 100, "description": "Parts at most, 15 by default"},
   "tested": {"type": "boolean", "description": "Only the library parts the test of every part under ngspice found working (each result's 'ngspice' says how it fared)"}}}},
{"name": "list_libraries",
 "description": "Lists the component libraries as the Libraries panel shows them, by section: installed (Qucs-S's own), user (the workspace's user_lib), each folder of the library search paths, and the open project's - each library with its file, its kind (qucs: a Qucs-S library; spice: a SPICE library of subcircuits) and how many parts it has; one that cannot be read, and one hidden with this simulator, say so. 'library' (a name or a file) gives one library's parts instead, each with its description and 'place': the add_component that places it (a Qucs-S library's part as type Lib with Lib and Comp, a SPICE library's as SpLib with File and Device). create_library makes a library, import_library brings one in, and set_settings (scope app, \"Locations/Library search paths\") adds a folder of them.",
 "inputSchema": {"type": "object", "properties": {"library": {"type": "string", "description": "One library, by its name (OpAmps) or its file: its parts"}}}},
{"name": "create_library",
 "description": "Makes a component library of the open project's subcircuits, as Project > Create Library does: NAME.lib, with each subcircuit's Qucs and SPICE models and its symbol, and beside it a folder NAME/ of the files its models need (SPICE libraries, and the Verilog-A sources of the modules its .model cards name unless 'embed_verilog_a' is false). 'destination' is user_lib (the default: the user libraries), project, or a folder of the library search paths. It is in the Libraries panel at once; the answer gives each part's add_component ('place') and the messages of making it. A library of that name there is refused unless 'replace', which moves the old one to the trash first. Another library of the name elsewhere (installed, the project's, user_lib's, a search path's) is said in 'also_named' and 'warning': a part placed by the name is taken from the first of them that has it, so a name of its own is better. Without 'subcircuits', every subcircuit of the project (a schematic with ports; create_subcircuit makes one).",
 "inputSchema": {"type": "object", "properties": {
   "name": {"type": "string", "description": "The library's name: letters, digits and _ (MyAmps); its file is NAME.lib"},
   "subcircuits": {"type": "array", "items": {"type": "string"}, "description": "The project's subcircuits to put in it, by name (amp or amp.sch); all of them when not given"},
   "destination": {"type": "string", "description": "user_lib (the default), project, or a folder of the library search paths"},
   "descriptions": {"type": "object", "additionalProperties": {"type": "string"}, "description": "Each part's description, by subcircuit name: {\"amp\": \"A x10 amplifier\"}"},
   "digital_models": {"type": "boolean", "description": "Verilog and VHDL models too, for a digital simulation (off: analog only, the default)"},
   "embed_verilog_a": {"type": "boolean", "description": "Copy the Verilog-A sources its models use into its folder (the setting's choice when not given, on unless changed)"},
   "ground_pin": {"type": "boolean", "description": "Give each subcircuit's SPICE model a first pin, gnd, its parts tie to the circuit's ground - for Qucs-S 26.1.5 and earlier, which always tie one (the setting's choice when not given, off unless changed)"},
   "replace": {"type": "boolean", "description": "Write over a library of that name there, the old one moved to the trash"}},
  "required": ["name"]}},
{"name": "import_library",
 "description": "Brings a library file into Qucs-S: a Qucs-S library (made by Create Library or create_library - another computer's, a colleague's) or a SPICE library of subcircuits (a .lib with .subckt), copied with its folder of models (NAME/ beside NAME.lib) into user_lib (the default), the project, or a folder of the library search paths. It is in the Libraries panel at once; the answer gives its kind, the files written and each part's add_component ('place'). One there already is refused unless 'replace', which moves it (and its folder) to the trash first. A Qucs-S library whose name another library has elsewhere is said in 'also_named' and 'warning' (a part placed by the name is taken from the first of them that has it). A folder of libraries is used where it is instead: set_settings (scope app, \"Locations/Library search paths\") adds it.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The library file (a .lib): a path, or a name in the open project's folder (else the workspace's)"},
   "destination": {"type": "string", "description": "user_lib (the default), project, or a folder of the library search paths"},
   "replace": {"type": "boolean", "description": "Replace a library of that name there, the old one (and its folder) moved to the trash"}},
  "required": ["path"]}},
{"name": "new_project",
 "description": "Creates a project in the workspace (a NAME_prj folder with its Scratch folder, like Project > New Project; a plain folder when any folder is a project) and opens it unless 'open' is false. Opening closes the documents, so it is not opened while one has unsaved changes. Relative paths are then resolved against the open project.",
 "inputSchema": {"type": "object", "properties": {"name": {"type": "string", "description": "The project's name: a folder NAME_prj in the workspace"}, "open": {"type": "boolean", "description": "Open it after (the default); refused while a document has unsaved changes"}}, "required": ["name"]}},
{"name": "open_project",
 "description": "Opens a project of the workspace ('name': amp or amp_prj, or a project's folder), like Project > Open Project. The documents are closed first, so it is refused while one has unsaved changes. Relative paths are resolved against it afterwards. The workspace folder itself and the home folder are not projects.",
 "inputSchema": {"type": "object", "properties": {"name": {"type": "string", "description": "amp or amp_prj, or a project's folder"}}, "required": ["name"]}},
{"name": "copy_document",
 "description": "Copies a schematic ('path', open or not; an open one is copied as it is, including unsaved changes) to 'to': a name next to it (amp2), a path, or a folder or project to copy it into. Unless 'results' is false, its datasets (for each simulator) and its data display are copied too, renamed along with it and pointing at each other. 'replace' overwrites an existing copy. Returns the files written.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic copied, open or not (an open one as it is now); the one in front when not given"}, "to": {"type": "string", "description": "A name beside it (amp2), a path, or a folder or project to copy it into"}, "results": {"type": "boolean", "description": "Copy its datasets and data display too (the default)"},
   "replace": {"type": "boolean", "description": "Write over a copy that is there (else refused)"}}, "required": ["to"]}},
{"name": "clean_scratch",
 "description": "Moves a schematic's scratch files - its subfolder of the project's Scratch folder, with the netlists, simulator output and logs its runs left - to the system's trash; with 'datasets', its datasets too (name.dat, .dat.ngspice, ...). The next run creates them again.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "datasets": {"type": "boolean", "description": "Its datasets go to the trash too"}}}},
{"name": "rename_file",
 "description": "Renames a file or folder, or moves it, as the File Browser does: 'to' is its new name in the same folder (amp2.sch), or a path - into a folder that is there, or as the name it ends in. The documents open from it follow, their tabs renamed, unsaved changes kept; undo with 'files' renames it back. Refused when something is there already, and for the workspace, the home folder and the project open now (Project > Close Project first). A schematic's datasets and data display keep their names: copy_document copies a schematic with them under a new name. Use it rather than mv, which leaves an open document's tab on a file that is not there.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The file or folder: a path, relative to the open project's folder (else the workspace)"}, "to": {"type": "string", "description": "Its new name in the same folder (amp2.sch), or a path to move it to: into a folder that is there, or as the name it ends in"}}, "required": ["path", "to"]}},
{"name": "trash_file",
 "description": "Moves a file or folder to the system's trash, from which undo with 'files' takes it back (as Finder's Put Back does; the documents it closed are not opened again). The documents open from it close; refused while one of them has unsaved changes, while a simulation runs, and for the workspace, the home folder and the project open now. Nothing is deleted when the trash cannot take it. Use it rather than rm, which deletes for good.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The file or folder: a path, relative to the open project's folder (else the workspace)"}}, "required": ["path"]}},
{"name": "make_symbol",
 "description": "Draws a subcircuit's symbol from scratch: a box with each port on one side. 'sides' assigns ports by name or number ({\"in\": \"left\", \"out\": \"right\", \"vdd\": \"top\", \"gnd\": \"bottom\"}); a port not listed goes by its name (a supply - vdd, vcc, v+ - on top, a ground or negative supply - gnd, vss, v- - at the bottom), its type or name (in, inp, in+ on the left; out on the right), and the rest alternate left and right. Its name text goes below, with the parameters and prefix it had; 'parameters' gives the parameters its instances take instead, as set_subcircuit_parameters takes them. Afterwards the document shows its symbol, like Edit Circuit Symbol. One undo step. Use it to finish what create_subcircuit started.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The schematic: its file or its tab's title; the one in front when not given"}, "sides": {"type": "object", "additionalProperties": {"type": "string", "enum": ["left", "right", "top", "bottom"]}, "description": "Ports by name or number to a side: {\"in\": \"left\", \"out\": \"right\", \"vdd\": \"top\", \"gnd\": \"bottom\"}; the rest by their names and types"},
   "parameters": {"type": "array", "items": {"anyOf": [{"type": "string"}, {"type": "object", "properties": {"name": {"type": "string"}, "default": {"type": ["string", "number"]}, "description": {"type": "string"}, "type": {"type": "string"}, "shown": {"type": "boolean"}}, "required": ["name"]}]}, "description": "The parameters its instances take, all of them: [{\"name\": \"Rs\", \"default\": \"1k\", \"description\": \"series resistance\", \"shown\": true}] or [\"Rs=1k\"]; those it had when not given"},
   "prefix": {"type": "string", "description": "What its instances' names begin with: SUB (SUB1, SUB2, ...) unless given"}}}},
{"name": "set_subcircuit_parameters",
 "description": "Sets the parameters a subcircuit's instances take - the name=default pairs of its .SUBCKT line, used inside it in values as {Rs}. 'parameters' lists them as [{\"name\": \"Rs\", \"default\": \"1k\", \"description\": \"series resistance\", \"type\": \"\", \"shown\": true}] or [\"Rs=1k\"]: each is set or added by name (a new one needs a default), and fields not given stay; 'remove' takes some away; 'replace' makes the list exactly those given. 'prefix' is what the instances' names begin with (SUB gives SUB1, SUB2). They live on the symbol's name text; a schematic whose symbol is not drawn yet gets one, as make_symbol draws it. One undo step. Save it for its instances to take them: in open schematics each instance keeps the values set on it by name, and a new parameter starts at its default. Returns the list, as get_schematic gives it under 'subcircuit parameters', and the .SUBCKT line's pairs.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The subcircuit's schematic: its file or its tab's title; the one in front when not given"},
   "parameters": {"type": "array", "items": {"anyOf": [{"type": "string"}, {"type": "object", "properties": {"name": {"type": "string"}, "default": {"type": ["string", "number"]}, "description": {"type": "string"}, "type": {"type": "string"}, "shown": {"type": "boolean"}}, "required": ["name"]}]}, "description": "Each set or added by name: [{\"name\": \"Rs\", \"default\": \"1k\", \"description\": \"series resistance\", \"shown\": true}] or [\"Rs=1k\"]; a default is one value with no space (1k, {2*Rs})"},
   "remove": {"type": "array", "items": {"type": "string"}, "description": "Parameters taken away, by name"},
   "replace": {"type": "boolean", "description": "The list becomes those given, in their order (those not given are taken away)"},
   "prefix": {"type": "string", "description": "What its instances' names begin with: SUB gives SUB1, SUB2, ..."}}}},
{"name": "import_netlist",
 "description": "Creates a schematic from a SPICE netlist ('text', or 'file': .cir, .sp, .net) in a new document. Each element becomes a SPICE part of its kind carrying its netlist text as written (R_SPICE, C_SPICE, S4Q_V for a source with SIN, PULSE and the rest, NPN_SPICE with its model, NMOS_SPICE, DIODE_SPICE, VCVS for a linear E, SPICE_dev for an X instance, K_SPICE, ...), placed in rows, with each pin's net as a net label on it and node 0 as a ground. Its .model cards become SpiceModel blocks; .param, .options, .include and .lib become blocks; .tran, .ac and .op become analyses; its .subckt definitions go into a library file next to it, which is included. The layout is rough but simulates as the netlist did; arrange (or move and connect) tidies it. The first line is the title unless it reads as an element ('title_line'). 'save_as' saves it. An untitled schematic nothing was done in (the one Qucs-S opens at start) is closed. Returns the parts, the nets and anything that was not taken.",
 "inputSchema": {"type": "object", "properties": {"text": {"type": "string", "description": "The netlist's text (or 'file'); its first line is the title unless it reads as an element"}, "file": {"type": "string", "description": "A netlist file instead of 'text' (.cir, .sp, .net), relative to the project or the workspace"}, "save_as": {"type": "string", "description": "Save the new schematic as this file (a .sch); one there already is refused unless 'replace'"}, "replace": {"type": "boolean", "description": "With save_as: write over a file of that name"},
   "title_line": {"type": "boolean", "description": "Whether the first line is a title (skipped): told from the line when not given"},
   "title": {"type": "string", "description": "The schematic's title, in place of the netlist's: a text above the circuit, and the name of its subcircuits' library"}, "spacing": {"type": "integer", "minimum": 120, "maximum": 600, "description": "Room between the parts placed, 200 by default"}}}},
{"name": "import_data",
 "description": "Imports a data file - a measurement, a script's results - as a dataset beside a schematic, to plot in its diagrams with (or without) a simulation's, as the Import tab of a diagram's dialog does: CSV or TSV, an Excel workbook (.xlsx, a sheet of it), text of numbers in columns (.txt, .prn, ...), NumPy .npy or .npz, Touchstone (.s1p ... .sNp) or a Qucs-S dataset from elsewhere. It is written as name.dat in the schematic's folder, with the file it came from. Its traces are name:variable, with no simulator's prefix: add_diagram with traces [\"name:v1\"] plots it, no simulation needed. In a table one column is x: the first that rises or falls steadily, else the row, unless 'x' says (a name 'columns' lists, or \"#row\"). Returns the dataset's name and file, its variables (x first, each with its points and range), the traces to use, the columns and a workbook's sheets, and anything left out. 'reload' reads an imported dataset again from its file after the file changed; 'remove' deletes one (to the trash; the file it came from stays). The diagrams showing it are read again. The schematic must have a file: the dataset goes beside it. undo with 'files' puts the dataset back as it was.",
 "inputSchema": {"type": "object", "properties": {"file": {"type": "string", "description": "The data file: beside the schematic, in the project or the workspace, or absolute"},
   "path": {"type": "string", "description": "The schematic (or data display) whose folder the dataset goes in; the one in front when not given"},
   "name": {"type": "string", "description": "The dataset's name (letters, digits, _): by default the file's, with a number after it when a dataset or a schematic there has it. With 'reload' or 'remove': the imported dataset's"},
   "x": {"type": "string", "description": "The column that is x, by its name as 'columns' lists it, or \"#row\" for the row; by default the first column when it rises or falls steadily, else the row"},
   "sheet": {"type": "string", "description": "A workbook's sheet, by its name; the first by default"},
   "reload": {"type": "boolean", "description": "Read the imported dataset 'name' (or the one of 'file') again from its file, with its x and sheet unless given"},
   "remove": {"type": "boolean", "description": "Delete the imported dataset 'name' (or the one of 'file'): its .dat, to the trash; the file it came from stays"}}}},
{"name": "export_data",
 "description": "Writes curves to a file for another program - a spreadsheet, Python, MATLAB - as the Export tab of a diagram's dialog does: a diagram's traces (all, or 'traces' of it by their numbers or variables), or 'variables' of a dataset (names, trace names such as ngspice/tran.v(out), m:gain or ngspice/run1:v(out), or expressions such as db(ac.v(out)/ac.v(in))). Formats: csv, tsv, xlsx (an Excel workbook), text (columns lined up, the names in a comment), npz (NumPy: an array for each variable, shaped by what it is over) or dataset (a Qucs-S .dat, to plot as name:variable). The variables go in tables by what they are over: the independent variables first (time, frequency, a swept parameter, the fastest first), then the variables on them, a row for each point. CSV, TSV and text hold one table - variables of one sweep - a workbook a sheet for each; NumPy and a dataset any. A complex variable takes two columns: real and imaginary parts (by default), magnitude and phase in degrees, or dB and phase ('complex'). One dataset's variables in a file: the traces of a run and of a measurement in one diagram are written in two calls ('traces'). 'save_as' is a path, or a name in the project's folder (else the workspace's); a suffix of a format gives the format (out.xlsx is a workbook), else 'format''s is added (csv by default). A file there is written over - never the dataset read, nor a file a dataset was imported from. Returns the file, its format, the dataset read, the tables' rows and columns (or the arrays, the variables) and what was left out. undo with 'files' puts back a file written over.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "The schematic or data display whose diagram or dataset it is; the one in front when not given"},
   "diagram": {"type": "integer", "description": "The diagram whose traces are written, by its number (get_schematic numbers them)"},
   "traces": {"type": "array", "items": {}, "description": "With 'diagram': only these of its traces, by their numbers (from 1) or variables; all by default"},
   "variables": {"type": "array", "items": {"type": "string"}, "description": "Instead of a diagram: the variables to write - names, trace names (ngspice/tran.v(out), m:gain) or expressions; the independent variables they are over come with them"},
   "dataset": {"type": "string", "description": "With 'variables': the dataset - a file (run1.dat.ngspice, m.dat), or a name keep_as or import_data gave (run1, m); the schematic's own by default, or the one name:variable names"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"], "description": "With 'variables' and no 'dataset': the simulator whose dataset of the schematic is read (the one in the settings, else the newest there)"},
   "save_as": {"type": "string", "description": "The file: a path, or a name in the project's folder (else the workspace's); written over when it is there. A format's suffix gives the format; another name gets 'format''s added"},
   "format": {"type": "string", "enum": ["csv", "tsv", "xlsx", "text", "npz", "dataset"], "description": "csv (the default), tsv, xlsx (an Excel workbook), text (columns lined up), npz (NumPy) or dataset (a Qucs-S .dat); a suffix of one in 'save_as' gives it"},
   "complex": {"type": "string", "enum": ["real_imaginary", "magnitude_phase", "db_phase"], "description": "A complex variable's two columns in a table: real and imaginary parts (the default), magnitude and phase (degrees), or dB and phase"}},
   "required": ["save_as"]}},
{"name": "set_simulator",
 "description": "Chooses the simulator that simulate runs and get_netlist writes for, like the toolbar's simulator list (a setting kept for next time): ngspice, xyce, spiceopus or qucsator - one that is installed. To run another one once, simulate takes 'simulator'. To compare two engines, simulate, then simulate again with 'simulator'; get_dataset with 'simulator' reads each result. Returns the simulator in use and those installed.",
 "inputSchema": {"type": "object", "properties": {"simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"], "description": "One that is installed (the answer lists them)"}}, "required": ["simulator"]}},
{"name": "synthesize_filter",
 "description": "Designs a filter as Tools > Filter synthesis (an LC ladder, or lines) or Active filter synthesis does - their own calculation, run for you - and places it: in a new schematic (saved as 'save_as' when given), or in the schematic 'path' names at x, y (its top left corner; below what is there when not given), as their 'put into clipboard' and a paste would. It comes with its ports, an analysis and equations of its response (dBS21, dBS11 for an LC filter, for the simulator in the settings or 'simulator'), ready to simulate. 'kind' lc (the default) or active. 'response' bessel, butterworth, chebyshev (LC's default), cauer (elliptic); active also inverse_chebyshev and legendre (butterworth its default). 'type' lowpass (the default), highpass, bandpass, bandstop. 'fc' is the corner (a Chebyshev's -3 dB point) or the band's start, 'f2' the band's end (band-pass, band-stop). The order: 'order', or - an LC Butterworth or Chebyshev low- or high-pass - 'atten' (dB) at 'fs' finds the lowest that has it; a Cauer filter's always comes from 'atten' at 'fs'. An active Butterworth's or Chebyshev's: 'order', or 'atten' at 'fs' with 'ap' (dB at fc) - at 'transition' for a band; an active inverse Chebyshev's or Cauer's only from those; a Bessel's or Legendre's is given. 'topology': LC pi (the default), tee, c_coupled_lines, microstrip_end_coupled, coupled_lines, microstrip_coupled, stepped_impedance, microstrip_stepped_impedance, quarter_wave, microstrip_quarter_wave, equation (lines: band-pass; stepped: low-pass); active sallen_key (the default), mfb, cauer. Returns its parts with their values, the order and where it is; simulate then measures it.",
 "inputSchema": {"type": "object", "properties": {
   "kind": {"type": "string", "enum": ["lc", "active"], "description": "lc (the default): inductors and capacitors, or lines; active: op-amps, resistors and capacitors"},
   "response": {"type": "string", "enum": ["bessel", "butterworth", "chebyshev", "cauer", "elliptic", "inverse_chebyshev", "legendre"], "description": "The response: chebyshev for lc, butterworth for active when not given"},
   "type": {"type": "string", "enum": ["lowpass", "highpass", "bandpass", "bandstop"], "description": "lowpass when not given"},
   "order": {"type": "integer", "minimum": 1, "maximum": 40, "description": "Its order; else found from 'atten' at 'fs'"},
   "fc": {"description": "Hz: the corner (low-pass, high-pass), the band's start (band-pass, band-stop) - a number or \"1 GHz\""},
   "f2": {"description": "Hz: the band's end (band-pass, band-stop)"},
   "fs": {"description": "Hz: where 'atten' is reached in the stop band"},
   "ripple": {"type": "number", "description": "dB: the pass band's ripple (Chebyshev, Cauer)"},
   "atten": {"type": "number", "description": "dB: the stop band's attenuation at fs"},
   "impedance": {"description": "Ohms: an LC filter's terminations, 50 unless given"},
   "gain": {"type": "number", "description": "dB: an active filter's pass band gain, 0 unless given"},
   "ap": {"type": "number", "description": "dB: an active low- or high-pass's attenuation at fc, 3 unless given"},
   "transition": {"description": "Hz: an active band filter's width from each band edge to where atten is reached"},
   "topology": {"type": "string", "description": "LC: pi (the default), tee, c_coupled_lines, microstrip_end_coupled, coupled_lines, microstrip_coupled, stepped_impedance, microstrip_stepped_impedance, quarter_wave, microstrip_quarter_wave, equation; active: sallen_key (the default), mfb, cauer"},
   "substrate": {"type": "object", "properties": {"er": {"type": "number", "description": "Relative permittivity"}, "h": {"description": "Height, metres or \"1.6 mm\""}, "t": {"description": "Metal thickness"}, "min_width": {"description": "The narrowest line"}, "max_width": {"description": "The widest line"}}, "description": "A microstrip realization's substrate"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "qucsator"], "description": "Whose equations of its response it gets (and, for a combiner, what it can be made of); the settings' when not given"},
   "path": {"type": "string", "description": "A schematic open to place it in, at x, y (else a new schematic)"}, "x": {"type": "integer", "description": "With 'path': where its top left corner goes (below what is there when x, y are not given)"}, "y": {"type": "integer", "description": "With 'path': where its top left corner goes"}, "save_as": {"type": "string", "description": "The new schematic's file, saved there"}}}},
{"name": "synthesize_attenuator",
 "description": "Designs a resistive attenuator as Tools > Attenuator synthesis does (its own calculation) and places it with its ports and an S-parameter analysis: in a new schematic ('save_as'), or in the one 'path' names at x, y. 'topology' pi (the default), tee, bridged_tee, reflection, quarter_wave_series, quarter_wave_shunt (at 'f'; 'lumped' for its CLC equivalent), l_pad_series_first, l_pad_shunt_first, series, shunt. 'attenuation' in dB; 'z_in' and 'z_out' in ohms (50; z_out = z_in unless given) - a pi or tee between unequal ones has a least attenuation, said when it is more. Returns each resistor's ohms and, for 'p_in' watts in, the watts each dissipates.",
 "inputSchema": {"type": "object", "properties": {
   "topology": {"type": "string", "enum": ["pi", "tee", "bridged_tee", "reflection", "quarter_wave_series", "quarter_wave_shunt", "l_pad_series_first", "l_pad_shunt_first", "series", "shunt"], "description": "pi when not given"},
   "attenuation": {"type": "number", "description": "dB"}, "z_in": {"description": "Ohms, 50 unless given"}, "z_out": {"description": "Ohms, z_in unless given"},
   "f": {"description": "Hz: a quarter-wave attenuator's frequency"}, "lumped": {"type": "boolean", "description": "A quarter-wave attenuator's line as its CLC equivalent"},
   "r_below_z0": {"type": "boolean", "description": "A reflection attenuator's resistors below the reference (true unless false), else above it"},
   "p_in": {"type": "number", "description": "Watts in, for the dissipation (1 mW unless given)"}, "s_parameters": {"type": "boolean", "description": "With ports and an S-parameter analysis (true unless false)"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "qucsator"], "description": "Whose equations of its response it gets (and, for a combiner, what it can be made of); the settings' when not given"},
   "path": {"type": "string", "description": "A schematic open to place it in, at x, y (else a new schematic)"}, "x": {"type": "integer", "description": "With 'path': where its top left corner goes (below what is there when x, y are not given)"}, "y": {"type": "integer", "description": "With 'path': where its top left corner goes"}, "save_as": {"type": "string", "description": "The new schematic's file, saved there"}}, "required": ["attenuation"]}},
{"name": "synthesize_matching",
 "description": "Designs a matching circuit as Tools > Matching Circuit does (its own calculation, in Qucs-S) and places it with its ports and an S-parameter analysis: in a new schematic ('save_as'), or in the one 'path' names at x, y. A load: 'z_load' (\"10-j20\", [10, -20] or {\"re\", \"im\"}) matched to 'z_source' (a real reference, 50 unless given) at 'f'. A two-port (an amplifier's input and output at once): 's' {\"s11\", \"s12\", \"s21\", \"s22\"} - refused, with K and |delta|, when it is not unconditionally stable. 'topology' l_section (the default), single_stub, double_stub ('stubs' open or short, 'balanced_stubs'), quarter_wave ('sections', 'weighting' binomial or chebyshev with 'max_ripple'), cascaded_l_sections ('sections'), lambda8_lambda4; 'microstrip' with a 'substrate' makes its lines microstrip. An element of no reactance is left out (10-j20 to 50 needs no series one). Returns the parts and what the calculation said (a reactive load a quarter-wave transformer matches only the real part of).",
 "inputSchema": {"type": "object", "properties": {
   "z_load": {"description": "The load: \"10-j20\", [10, -20], {\"re\": 10, \"im\": -20}"}, "z_source": {"description": "Ohms, real: the reference (50)"}, "z_out": {"description": "A two-port's output reference (z_source unless given)"}, "f": {"description": "Hz: the frequency it matches at"},
   "s": {"type": "object", "description": "A two-port's S-parameters at f: {\"s11\": [re, im] or {\"mag\", \"deg\"}, \"s12\", \"s21\", \"s22\"}"},
   "topology": {"type": "string", "enum": ["l_section", "single_stub", "double_stub", "quarter_wave", "cascaded_l_sections", "lambda8_lambda4"], "description": "l_section when not given"},
   "stubs": {"type": "string", "enum": ["open", "short"], "description": "A stub match's stubs: open (the default) or short"}, "balanced_stubs": {"type": "boolean", "description": "Each stub as two, either side of the line"},
   "sections": {"type": "integer", "minimum": 1, "maximum": 8, "description": "A multistage quarter-wave or cascaded L match's sections (3)"}, "weighting": {"type": "string", "enum": ["binomial", "chebyshev"], "description": "A multistage quarter-wave match's: binomial (the default) or chebyshev"}, "max_ripple": {"type": "number", "description": "A Chebyshev weighting's largest reflection (0.05)"},
   "microstrip": {"type": "boolean", "description": "Its lines as microstrip on 'substrate'"}, "substrate": {"type": "object", "description": "{\"er\", \"h\", \"t\", \"tand\", \"resistivity\", \"roughness\", \"min_width\", \"max_width\"}: metres, or text with units"},
   "s_parameters": {"type": "boolean", "description": "With ports and an S-parameter analysis (true unless false)"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "qucsator"], "description": "Whose equations of its response it gets (and, for a combiner, what it can be made of); the settings' when not given"},
   "path": {"type": "string", "description": "A schematic open to place it in, at x, y (else a new schematic)"}, "x": {"type": "integer", "description": "With 'path': where its top left corner goes (below what is there when x, y are not given)"}, "y": {"type": "integer", "description": "With 'path': where its top left corner goes"}, "save_as": {"type": "string", "description": "The new schematic's file, saved there"}}, "required": ["f"]}},
{"name": "synthesize_power_combiner",
 "description": "Designs a power divider or combiner as Tools > Power combining does (its own calculation) and places it with its ports and an S-parameter analysis: in a new schematic ('save_as'), or in the one 'path' names at x, y. 'type' wilkinson (the default), multistage_wilkinson ('stages' 2-7), tee, branchline, double_box_branchline, bagley, gysel, travelling_wave, tree ('ways' for the last three). 'f' its frequency, 'z0' (50), 'ratio_db' the split's ratio in dB (wilkinson, tee, branchline). 'implementation': for a SPICE simulator lumped elements only, and only the Wilkinsons (lumped is then its default); with simulator qucsator also ideal (lines, its default) and microstrip ('substrate', 'alpha' dB/m).",
 "inputSchema": {"type": "object", "properties": {
   "type": {"type": "string", "enum": ["wilkinson", "multistage_wilkinson", "tee", "branchline", "double_box_branchline", "bagley", "gysel", "travelling_wave", "tree"], "description": "wilkinson when not given"},
   "f": {"description": "Hz: its frequency"}, "z0": {"description": "Ohms, 50 unless given"}, "ways": {"type": "integer", "description": "Outputs: a Bagley's, a travelling wave's, a tree's"}, "stages": {"type": "integer", "description": "A multistage Wilkinson's, 2 to 7"},
   "ratio_db": {"type": "number", "description": "An unequal split's power ratio in dB (wilkinson, tee, branchline)"},
   "implementation": {"type": "string", "enum": ["lumped", "ideal", "microstrip"], "description": "lumped (a SPICE simulator's, the Wilkinsons only), ideal lines or microstrip (qucsator)"},
   "substrate": {"type": "object", "description": "A microstrip implementation's {\"er\", \"h\", \"t\", \"tand\", \"resistivity\", \"roughness\", \"min_width\", \"max_width\"}"}, "alpha": {"type": "number", "description": "A line's loss in dB/m"},
   "s_parameters": {"type": "boolean", "description": "With ports and an S-parameter analysis (true unless false)"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "qucsator"], "description": "Whose equations of its response it gets (and, for a combiner, what it can be made of); the settings' when not given"},
   "path": {"type": "string", "description": "A schematic open to place it in, at x, y (else a new schematic)"}, "x": {"type": "integer", "description": "With 'path': where its top left corner goes (below what is there when x, y are not given)"}, "y": {"type": "integer", "description": "With 'path': where its top left corner goes"}, "save_as": {"type": "string", "description": "The new schematic's file, saved there"}}, "required": ["f"]}},
{"name": "line_calc",
 "description": "Tools > Line calculation's own calculation, for numbers: a transmission line's geometry from its impedance and electrical length (do synthesize - the default when z0 or z0e is given) or its impedance, effective permittivity and losses from its geometry (do analyze). 'type' microstrip (the default), coplanar, grounded_coplanar, rectangular (waveguide), coaxial, coupled_microstrip, stripline. Values in metres, hertz, ohms and degrees (numbers, or text with units: \"1.6 mm\", \"2.4 GHz\"): f; substrate er, mur, h, h_t, t, cond, sigma, tand, tanm, rough (or in 'substrate'); geometry w, l, s, a, b, din, dout; electrical z0, z0e, z0o, angle - each line type takes those its window has. 'solve_for' picks which physical value synthesis finds where the window offers a choice (coaxial din or dout). Returns every value afterwards and the results (ErEff, conductor and dielectric losses, skin depth) - the width of a 50 ohm microstrip on FR4 is W.",
 "inputSchema": {"type": "object", "properties": {
   "type": {"type": "string", "enum": ["microstrip", "coplanar", "grounded_coplanar", "rectangular", "coaxial", "coupled_microstrip", "stripline"], "description": "microstrip when not given"},
   "do": {"type": "string", "enum": ["analyze", "synthesize"], "description": "analyze: the geometry gives Z0; synthesize: Z0 gives the geometry"},
   "f": {"description": "Hz"}, "z0": {"description": "Ohms: its impedance"}, "z0e": {"description": "Ohms: a coupled line's even mode impedance"}, "z0o": {"description": "Ohms: its odd mode impedance"}, "angle": {"description": "Degrees: its electrical length"},
   "er": {"description": "The substrate's relative permittivity"}, "mur": {"description": "Its relative permeability"}, "h": {"description": "The substrate's height (a stripline's: between its grounds)"}, "h_t": {"description": "The height of a cover above it"}, "t": {"description": "The metal's thickness"},
   "cond": {"description": "The metal's conductivity, S/m"}, "sigma": {"description": "A coax's or stripline's conductivity, S/m"}, "tand": {"description": "The dielectric's loss tangent"}, "tanm": {"description": "A waveguide's magnetic loss tangent"}, "rough": {"description": "The metal's roughness"},
   "w": {"description": "The line's width"}, "l": {"description": "Its length"}, "s": {"description": "A gap: coplanar's, coupled lines'"}, "a": {"description": "A waveguide's broad side"}, "b": {"description": "Its narrow side"}, "din": {"description": "A coax's inner diameter"}, "dout": {"description": "Its outer diameter"},
   "substrate": {"type": "object", "description": "The substrate's values, by the same names, in one object"}, "solve_for": {"type": "string", "description": "The physical value synthesis finds, where there is a choice: din or dout of a coax, a or b of a waveguide"}}}},
{"name": "receiver_budget",
 "description": "Tools > Receiver calculator's own calculation (RxCalc): a receiver's cascade, stage by stage. 'stages' [{\"name\", \"gain\" dB, \"nf\" dB (a passive stage's loss when not given), \"iip3\" or \"oip3\" dBm, \"ip1db\" or \"op1db\" dBm, \"enabled\"}], the input's first; an IP3 or P1dB not given is +100 dBm (no limit, said). 'input_power' dBm (-60), 'bandwidth' Hz (the noise bandwidth, 1000), 'snr_min' dB (10), 'temperature' C (25), 'peak_to_average' dB. Returns each stage's cascade to there (gain, noise figure, IIP3, OIP3, P1dB, powers, its part of the noise figure and IP3, backoff) and the system's: gain, noise figure, IP3, P1dB, noise floor, noise temperature, MDS, sensitivity, SNR, IMD, SFDR, blocking dynamic range.",
 "inputSchema": {"type": "object", "properties": {
   "stages": {"type": "array", "items": {"type": "object", "properties": {"name": {"type": "string", "description": "Its name"}, "gain": {"type": "number", "description": "dB"}, "nf": {"type": "number", "description": "Its noise figure, dB"}, "iip3": {"type": "number", "description": "dBm, at its input"}, "oip3": {"type": "number", "description": "dBm, at its output"}, "ip1db": {"type": "number", "description": "Its 1 dB compression at the input, dBm"}, "op1db": {"type": "number", "description": "... at the output, dBm"}, "enabled": {"type": "boolean", "description": "false leaves it out"}}}, "description": "The stages, the input's first"},
   "input_power": {"type": "number", "description": "dBm at the input (-60)"}, "bandwidth": {"description": "Hz: the noise bandwidth (1000)"}, "snr_min": {"type": "number", "description": "dB: the least signal to noise, for the sensitivity (10)"}, "temperature": {"type": "number", "description": "C (25)"}, "peak_to_average": {"type": "number", "description": "dB: the signal's peak over its average"}}, "required": ["stages"]}},
{"name": "ngspice_commands",
 "description": "Tells which commands ngspice has and how to write them: the analyses (op, dc, ac, tran, noise, pz, sens, tf, disto; sp, pss, hb, stb, loadpull and the rest of the RF set), measurements (meas, fft, fourier, eye, track), vectors and expressions (let, print, set, option), plots and data files (wrdata, write, pyplot, wrsnp), the circuit (alter, altermod, show, save), compiled models (pre_osdi, snp), statistics and optimization (montecarlo, corners, sweep, optimize), breakpoints and the .control language (if, foreach, dowhile) - each summed up in a line, by category. It asks the ngspice of the settings for its own list (help all), so the answer says which of them it has, which it lacks and any others it has; * marks those stock ngspice lacks (the enhanced build's). Without arguments: every command in a line, by category, and how Qucs-S uses them - a Nutmeg script block (.CUSTOMSIM) holds any commands, a NutmegEq computes let expressions, the simulation blocks write theirs. 'command' names one or several (tran, .tran, pre_osdi, meas) for the syntax, what it does, this ngspice's own help line, how Qucs-S writes it and an example; 'search' finds commands by what they do (stability, touchstone, monte carlo, eye); 'category' lists a category's commands with their syntax. Use it before writing a Nutmeg script or NutmegEq, or to answer whether ngspice can do something.",
 "inputSchema": {"type": "object", "properties": {"command": {"description": "A command's name, or a list of names: tran, .tran (a dot-card's command), pre_osdi, [\"meas\", \"fft\"]"},
   "search": {"type": "string", "description": "Words for what a command does: stability, touchstone, noise figure, monte carlo"},
   "category": {"type": "string", "enum": ["analysis", "rf", "measure", "vectors", "output", "circuit", "models", "statistics", "reliability", "debug", "script", "digital", "utility"], "description": "One category's commands, each with its syntax"}}}}
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
    {"replace_component", QT_TRANSLATE_NOOP("QucsControl", "put a component of another type in one's place in Qucs-S")},
    {"run_script", QT_TRANSLATE_NOOP("QucsControl", "run a script of Qucs-S's tools")},
    {"delete", QT_TRANSLATE_NOOP("QucsControl", "delete from the schematic in Qucs-S")},
    {"connect", QT_TRANSLATE_NOOP("QucsControl", "draw a wire in Qucs-S")},
    {"add_wire", QT_TRANSLATE_NOOP("QucsControl", "draw a wire in Qucs-S")},
    {"set_label", QT_TRANSLATE_NOOP("QucsControl", "label a net in Qucs-S")},
    {"undo", QT_TRANSLATE_NOOP("QucsControl", "undo in Qucs-S")},
    {"redo", QT_TRANSLATE_NOOP("QucsControl", "redo in Qucs-S")},
    {"trigger_action", QT_TRANSLATE_NOOP("QucsControl", "use a menu action of Qucs-S")},
    {"set_dialog", QT_TRANSLATE_NOOP("QucsControl", "answer a dialog of Qucs-S")},
    {"set_ui", QT_TRANSLATE_NOOP("QucsControl", "use a panel, toolbar or tab of Qucs-S")},
    {"console", QT_TRANSLATE_NOOP("QucsControl", "type into a console of Qucs-S - what is typed runs")},
    {"set_settings", QT_TRANSLATE_NOOP("QucsControl", "change the settings of Qucs-S")},
    {"context_menu", QT_TRANSLATE_NOOP("QucsControl", "use a right-click menu of Qucs-S")},
    {"simulate", QT_TRANSLATE_NOOP("QucsControl", "run a simulation in Qucs-S")},
    {"stop_simulation", QT_TRANSLATE_NOOP("QucsControl", "stop a simulation in Qucs-S")},
    {"send_input", QT_TRANSLATE_NOOP("QucsControl", "click or type in Qucs-S as the mouse and keyboard would")},
    {"add_diagram", QT_TRANSLATE_NOOP("QucsControl", "add a diagram in Qucs-S")},
    {"edit_diagram", QT_TRANSLATE_NOOP("QucsControl", "change a diagram in Qucs-S")},
    {"add_trace", QT_TRANSLATE_NOOP("QucsControl", "add a trace to a diagram in Qucs-S")},
    {"edit_trace", QT_TRANSLATE_NOOP("QucsControl", "change a trace in Qucs-S")},
    {"describe_format", QT_TRANSLATE_NOOP("QucsControl", "read the format of a schematic file")},
    {"export_netlist", QT_TRANSLATE_NOOP("QucsControl", "write a netlist of Qucs-S to a file")},
    {"move_to_pane", QT_TRANSLATE_NOOP("QucsControl", "move a document to another pane in Qucs-S")},
    {"add_marker", QT_TRANSLATE_NOOP("QucsControl", "place a marker on a diagram in Qucs-S")},
    {"edit_marker", QT_TRANSLATE_NOOP("QucsControl", "change a marker in Qucs-S")},
    {"delete_marker", QT_TRANSLATE_NOOP("QucsControl", "delete a marker in Qucs-S")},
    {"rename_net", QT_TRANSLATE_NOOP("QucsControl", "rename a net in Qucs-S")},
    {"batch", QT_TRANSLATE_NOOP("QucsControl", "use several Qucs-S tools at once")},
    {"add_painting", QT_TRANSLATE_NOOP("QucsControl", "draw on the schematic in Qucs-S")},
    {"move", QT_TRANSLATE_NOOP("QucsControl", "move components in Qucs-S")},
    {"arrange", QT_TRANSLATE_NOOP("QucsControl", "lay out a schematic again in Qucs-S")},
    {"create_subcircuit", QT_TRANSLATE_NOOP("QucsControl", "make a subcircuit in Qucs-S")},
    {"add_analysis", QT_TRANSLATE_NOOP("QucsControl", "add an analysis in Qucs-S")},
    {"edit_painting", QT_TRANSLATE_NOOP("QucsControl", "change a painting in Qucs-S")},
    {"export_image", QT_TRANSLATE_NOOP("QucsControl", "write a picture of Qucs-S to a file")},
    {"set_simulator", QT_TRANSLATE_NOOP("QucsControl", "choose the simulator of Qucs-S")},
    {"build_verilog_a", QT_TRANSLATE_NOOP("QucsControl", "compile Verilog-A in Qucs-S")},
    {"tune", QT_TRANSLATE_NOOP("QucsControl", "tune a part by simulating it again and again in Qucs-S")},
    {"new_project", QT_TRANSLATE_NOOP("QucsControl", "make a project in Qucs-S")},
    {"open_project", QT_TRANSLATE_NOOP("QucsControl", "open a project in Qucs-S")},
    {"copy_document", QT_TRANSLATE_NOOP("QucsControl", "copy a schematic in Qucs-S")},
    {"clean_scratch", QT_TRANSLATE_NOOP("QucsControl", "clear a schematic's scratch files in Qucs-S")},
    {"edit_text", QT_TRANSLATE_NOOP("QucsControl", "edit a text document in Qucs-S")},
    {"rename_file", QT_TRANSLATE_NOOP("QucsControl", "rename or move a file in Qucs-S")},
    {"trash_file", QT_TRANSLATE_NOOP("QucsControl", "move a file to the trash from Qucs-S")},
    {"make_symbol", QT_TRANSLATE_NOOP("QucsControl", "draw a subcircuit's symbol in Qucs-S")},
    {"set_subcircuit_parameters", QT_TRANSLATE_NOOP("QucsControl", "set a subcircuit's parameters in Qucs-S")},
    {"import_netlist", QT_TRANSLATE_NOOP("QucsControl", "make a schematic of a netlist in Qucs-S")},
    {"import_data", QT_TRANSLATE_NOOP("QucsControl", "import a data file as a dataset in Qucs-S")},
    {"export_data", QT_TRANSLATE_NOOP("QucsControl", "write curves of Qucs-S to a file")},
    {"create_library", QT_TRANSLATE_NOOP("QucsControl", "make a component library in Qucs-S")},
    {"import_library", QT_TRANSLATE_NOOP("QucsControl", "bring a component library into Qucs-S")},
    {"synthesize_filter", QT_TRANSLATE_NOOP("QucsControl", "design a filter into a schematic in Qucs-S")},
    {"synthesize_attenuator", QT_TRANSLATE_NOOP("QucsControl", "design an attenuator into a schematic in Qucs-S")},
    {"synthesize_matching", QT_TRANSLATE_NOOP("QucsControl", "design a matching circuit into a schematic in Qucs-S")},
    {"synthesize_power_combiner", QT_TRANSLATE_NOOP("QucsControl", "design a power combiner into a schematic in Qucs-S")},
};

// Tools that only look (or move the view): used without asking.
const char* const kReadOnly[] = {"get_state", "get_schematic", "screenshot", "list_component_types", "list_actions",
                                 "get_dialog", "show_document", "select", "zoom", "get_netlist", "get_dataset",
                                 "reload_data", "describe_component_type", "describe_format", "list_documents", "check_schematic",
                                 "read_pdf", "find_library_component", "describe_part", "undo_history", "describe_tool", "diff",
                                 "get_text", "goto_line", "get_ui", "get_settings", "wait_for", "simulation_status", "read_help",
                                 "ngspice_commands", "list_libraries", "line_calc", "receiver_budget"};

// Tools that only add (MCP's destructiveHint false): nothing there is
// changed or taken away - a simulation writes its dataset anew, which it
// can do again.
const char* const kAdditive[] = {"add_component", "add_wire", "connect", "set_label", "add_diagram", "add_trace", "add_marker",
                                 "add_painting", "add_analysis", "new_document", "open_document", "new_project", "open_project",
                                 "simulate", "synthesize_filter", "synthesize_attenuator", "synthesize_matching", "synthesize_power_combiner"};

// Each tool's description in the tool list: a summary (the list is in
// every turn); describe_tool gives the whole of it.
const struct {
    const char* tool;
    const char* summary;
} kSummaries[] = {
    {"get_state", QT_TRANSLATE_NOOP("QucsControl", "Returns the state of the Qucs-S window: panes, open documents (each with its revision and who edited it last), the simulator, the open project, a running simulation and any open dialog. Start here.")},
    {"move_to_pane", QT_TRANSLATE_NOOP("QucsControl", "Moves a document to another pane (a number from get_state, or \"right\" or \"below\") to show documents side by side.")},
    {"open_document", QT_TRANSLATE_NOOP("QucsControl", "Opens a file in a tab (schematic, symbol, data display, text, netlist or PDF), or brings it to the front if it is already open. Warns when a component line has more values than its type.")},
    {"new_document", QT_TRANSLATE_NOOP("QucsControl", "Creates an untitled schematic or text document, or opens a schematic's data display (.dpl) for plots.")},
    {"show_document", QT_TRANSLATE_NOOP("QucsControl", "Brings an open document to the front.")},
    {"save_document", QT_TRANSLATE_NOOP("QucsControl", "Saves a document, or saves it under a new name with 'as'. Reports which open schematics picked up a changed subcircuit symbol.")},
    {"close_document", QT_TRANSLATE_NOOP("QucsControl", "Closes a document. 'unsaved' says whether to save or discard its changes; otherwise the user is asked.")},
    {"get_schematic", QT_TRANSLATE_NOOP("QucsControl", "Reads a schematic: its components with pin positions, nets, wires, texts, paintings and diagrams. Use format 'overview' for a large schematic, 'text' for the .sch source, or 'json' for the form set_schematic accepts.")},
    {"check_schematic", QT_TRANSLATE_NOOP("QucsControl", "Checks a schematic for problems a simulation would fail on or misread (no ground, floating parts, no DC path, unconnected pins), each with its location.")},
    {"set_schematic", QT_TRANSLATE_NOOP("QucsControl", "Replaces whole sections of a schematic in one undo step: 'components' and 'wires' as JSON (properties by name, checked against each type) or 'text' as .sch lines (values checked too).")},
    {"describe_format", QT_TRANSLATE_NOOP("QucsControl", "Explains the fields of each kind of .sch line (component, wire, diagram, trace, marker, painting).")},
    {"add_component", QT_TRANSLATE_NOOP("QucsControl", "Places a component of a library type at x, y, with properties by name, rotation and mirroring (plus equations or flags for an equation block). Returns the part with its pins and notes on common mistakes.")},
    {"edit_component", QT_TRANSLATE_NOOP("QucsControl", "Changes a component's properties, name, position, rotation, mirroring, active state or texts, keeping all its connections.")},
    {"diff", QT_TRANSLATE_NOOP("QucsControl", "Lists what changed in a schematic, part by part: against its saved file, a number of undo steps back, or another schematic.")},
    {"replace_component", QT_TRANSLATE_NOOP("QucsControl", "Replaces a component with one of another type, mapping its pins onto the old nets, in one undo step.")},
    {"delete", QT_TRANSLATE_NOOP("QucsControl", "Deletes components, net labels, wires, diagrams, traces or paintings.")},
    {"add_analysis", QT_TRANSLATE_NOOP("QucsControl", "Adds a simulation (AC, transient, DC, parameter sweep and others), optionally with a diagram of its results.")},
    {"create_subcircuit", QT_TRANSLATE_NOOP("QucsControl", "Turns a group of components into a subcircuit: they move to a new schematic with a port for each outside net, and an instance takes their place.")},
    {"move", QT_TRANSLATE_NOOP("QucsControl", "Moves components together by dx, dy, keeping all connections.")},
    {"arrange", QT_TRANSLATE_NOOP("QucsControl", "Lays out the whole schematic again: parts in columns by signal flow, every wire redrawn, every net kept. One undo step.")},
    {"connect", QT_TRANSLATE_NOOP("QucsControl", "Draws a wire between two pins or points (\"R1.2\" to \"C1.1\") along a path that touches nothing else, or to \"ground\" (a new ground symbol). Draws nothing and explains why if there is no such path.")},
    {"add_wire", QT_TRANSLATE_NOOP("QucsControl", "Draws a wire through the given points.")},
    {"set_label", QT_TRANSLATE_NOOP("QucsControl", "Names the net at a pin or wire; nets with the same name are connected. \"\" removes the label.")},
    {"select", QT_TRANSLATE_NOOP("QucsControl", "Selects components, diagrams and paintings.")},
    {"zoom", QT_TRANSLATE_NOOP("QucsControl", "Zooms a schematic: all, selection, in, out or 1:1.")},
    {"undo", QT_TRANSLATE_NOOP("QucsControl", "Undoes the last change, a number of steps, or back to a step from undo_history.")},
    {"undo_history", QT_TRANSLATE_NOOP("QucsControl", "Lists a schematic's undo steps in words.")},
    {"redo", QT_TRANSLATE_NOOP("QucsControl", "Redoes what was undone.")},
    {"screenshot", QT_TRANSLATE_NOOP("QucsControl", "Takes a picture of a document, the visible canvas or the whole window.")},
    {"list_component_types", QT_TRANSLATE_NOOP("QucsControl", "Lists the component types add_component accepts, with a description and category.")},
    {"list_actions", QT_TRANSLATE_NOOP("QucsControl", "Lists the menu actions trigger_action accepts and whether each is available now.")},
    {"trigger_action", QT_TRANSLATE_NOOP("QucsControl", "Runs a menu action (\"Edit > Rotate\") as a click would; read any dialog it opens with get_dialog.")},
    {"get_dialog", QT_TRANSLATE_NOOP("QucsControl", "Reads the open dialog: its texts and controls, each with an id for set_dialog.")},
    {"set_dialog", QT_TRANSLATE_NOOP("QucsControl", "Fills in the open dialog and presses a button.")},
    {"get_ui", QT_TRANSLATE_NOOP("QucsControl", "Reads a dock, panel, toolbar, the status bar or the tabs, as get_dialog a dialog.")},
    {"console", QT_TRANSLATE_NOOP("QucsControl", "Types a line into the Octave, Python Shell or Terminal dock and returns what it printed.")},
    {"wait_for", QT_TRANSLATE_NOOP("QucsControl", "Waits until a simulation ends, a dialog comes up, a document changes or a file is written, and returns then.")},
    {"simulation_status", QT_TRANSLATE_NOOP("QucsControl", "Tells how a simulation run in the background goes, and its outcome once it has ended.")},
    {"stop_simulation", QT_TRANSLATE_NOOP("QucsControl", "Stops a simulation (one followed by its id, or the one running) and returns its outcome.")},
    {"send_input", QT_TRANSLATE_NOOP("QucsControl", "Raw mouse and keyboard input on the canvas or a part of the window - the last resort - with a picture after.")},
    {"read_help", QT_TRANSLATE_NOOP("QucsControl", "Finds what this build's help says on a topic: menu actions' help, component types, examples, papers.")},
    {"get_settings", QT_TRANSLATE_NOOP("QucsControl", "Reads the settings (application, simulators, a document's, CDL) by typed keys, or those 'keys' or 'search' find.")},
    {"synthesize_filter", QT_TRANSLATE_NOOP("QucsControl", "Designs an LC or active filter (Tools > Filter synthesis) and places it, with its analysis, in a new schematic or one open.")},
    {"synthesize_attenuator", QT_TRANSLATE_NOOP("QucsControl", "Designs a resistive attenuator (Tools > Attenuator synthesis) and places it with its ports and analysis.")},
    {"synthesize_matching", QT_TRANSLATE_NOOP("QucsControl", "Designs a matching circuit for a load or a two-port (Tools > Matching Circuit) and places it.")},
    {"synthesize_power_combiner", QT_TRANSLATE_NOOP("QucsControl", "Designs a power divider or combiner (Tools > Power combining) and places it.")},
    {"line_calc", QT_TRANSLATE_NOOP("QucsControl", "Calculates a transmission line's geometry from its impedance, or its impedance and losses from its geometry (Tools > Line calculation).")},
    {"receiver_budget", QT_TRANSLATE_NOOP("QucsControl", "A receiver's cascade: gain, noise figure, IP3, P1dB, sensitivity, stage by stage (Tools > Receiver calculator).")},
    {"set_settings", QT_TRANSLATE_NOOP("QucsControl", "Changes settings by their keys through their own dialog, each with its old value.")},
    {"set_ui", QT_TRANSLATE_NOOP("QucsControl", "Uses a dock, panel, toolbar or the tabs, as set_dialog a dialog.")},
    {"context_menu", QT_TRANSLATE_NOOP("QucsControl", "Opens a right-click menu, lists it, and chooses an entry.")},
    {"simulate", QT_TRANSLATE_NOOP("QucsControl", "Runs a simulation and waits for it: whether it succeeded, errors with their netlist line and part, and the dataset's variables. 'operating_point' runs the DC bias only.")},
    {"get_netlist", QT_TRANSLATE_NOOP("QucsControl", "Returns the netlist a simulation would use now ('last' for the one it ran); 'map' ties each line to its part and each node to its pins.")},
    {"export_netlist", QT_TRANSLATE_NOOP("QucsControl", "Writes a schematic's SPICE or CDL netlist to a file.")},
    {"get_dataset", QT_TRANSLATE_NOOP("QucsControl", "Reads simulation results as numbers: variables, statistics, samples and measurements (bandwidth, rise time, overshoot, THD, phase margin and more), spectra, eye diagrams and Monte Carlo distributions.")},
    {"reload_data", QT_TRANSLATE_NOOP("QucsControl", "Re-reads the datasets and redraws the diagrams.")},
    {"add_diagram", QT_TRANSLATE_NOOP("QucsControl", "Places a diagram (rectangular, polar, Smith, table and others) with traces from the dataset.")},
    {"edit_diagram", QT_TRANSLATE_NOOP("QucsControl", "Changes a diagram's position, size, title, axes, grid, legend or colours (its theme).")},
    {"add_trace", QT_TRANSLATE_NOOP("QucsControl", "Adds a trace to a diagram: variable, color, thickness, style and axis.")},
    {"edit_trace", QT_TRANSLATE_NOOP("QucsControl", "Changes a trace of a diagram.")},
    {"add_marker", QT_TRANSLATE_NOOP("QucsControl", "Places a marker on a trace (at an x value, the peak, the -3 dB point or a crossing) that shows the value there.")},
    {"edit_marker", QT_TRANSLATE_NOOP("QucsControl", "Changes a marker's position, label, precision or look.")},
    {"delete_marker", QT_TRANSLATE_NOOP("QucsControl", "Deletes a marker from a diagram.")},
    {"rename_net", QT_TRANSLATE_NOOP("QucsControl", "Renames a net together with everything that refers to it: traces, equations and data displays.")},
    {"describe_component_type", QT_TRANSLATE_NOOP("QucsControl", "Describes a component type: pins, properties in order with defaults and units, netlist line, common mistakes and hidden properties that still reach the netlist. \"Verilog-A\" returns a module template.")},
    {"batch", QT_TRANSLATE_NOOP("QucsControl", "Runs several of these tools in one call, in order ('atomic': all or nothing).")},
    {"add_painting", QT_TRANSLATE_NOOP("QucsControl", "Draws a text, arrow, line, box, text box, table, dimension or formula on a schematic, or on its symbol with 'symbol'.")},
    {"edit_painting", QT_TRANSLATE_NOOP("QucsControl", "Changes a painting's fields; can also move a symbol port and set its label.")},
    {"list_documents", QT_TRANSLATE_NOOP("QucsControl", "Lists the files of the workspace, a project or a folder: kinds, sizes, dates, and which dataset belongs to which schematic.")},
    {"export_image", QT_TRANSLATE_NOOP("QucsControl", "Writes a picture of a schematic, symbol, data display or single diagram to a file (PNG, SVG, PDF and others).")},
    {"build_verilog_a", QT_TRANSLATE_NOOP("QucsControl", "Compiles a Verilog-A file with OpenVAF, reporting each error with its line and column.")},
    {"tune", QT_TRANSLATE_NOOP("QucsControl", "Adjusts a component value, simulating and measuring until a measurement reaches its target (or measures a table of values).")},
    {"read_pdf", QT_TRANSLATE_NOOP("QucsControl", "Reads the text of a PDF, such as a datasheet, page by page.")},
    {"get_text", QT_TRANSLATE_NOOP("QucsControl", "Reads a text tab (.cir, .va, ...) with its unsaved edits, line by line.")},
    {"edit_text", QT_TRANSLATE_NOOP("QucsControl", "Edits a text tab as one undo step, keeping the user's unsaved edits.")},
    {"goto_line", QT_TRANSLATE_NOOP("QucsControl", "Shows a line of a text tab, the cursor there.")},
    {"find_library_component", QT_TRANSLATE_NOOP("QucsControl", "Searches the libraries and the project's SPICE models for a part by name and values.")},
    {"describe_part", QT_TRANSLATE_NOOP("QucsControl", "A library part's pins (names, sides, roles), model kind, supplies and tested status in one call.")},
    {"new_project", QT_TRANSLATE_NOOP("QucsControl", "Creates a project in the workspace.")},
    {"open_project", QT_TRANSLATE_NOOP("QucsControl", "Opens a project of the workspace.")},
    {"copy_document", QT_TRANSLATE_NOOP("QucsControl", "Copies a schematic together with its datasets and data display.")},
    {"clean_scratch", QT_TRANSLATE_NOOP("QucsControl", "Moves a schematic's temporary files (netlists, logs, and datasets if asked) to the trash.")},
    {"rename_file", QT_TRANSLATE_NOOP("QucsControl", "Renames or moves a file or folder; the documents open from it follow.")},
    {"trash_file", QT_TRANSLATE_NOOP("QucsControl", "Moves a file or folder to the trash; the documents open from it close.")},
    {"make_symbol", QT_TRANSLATE_NOOP("QucsControl", "Draws a subcircuit symbol with ports on four sides.")},
    {"set_subcircuit_parameters", QT_TRANSLATE_NOOP("QucsControl", "Sets the parameters a subcircuit's instances take, with their defaults.")},
    {"import_netlist", QT_TRANSLATE_NOOP("QucsControl", "Creates a schematic from a SPICE netlist.")},
    {"import_data", QT_TRANSLATE_NOOP("QucsControl", "Imports a CSV, workbook, NumPy or Touchstone file as a dataset beside a schematic, to plot in its diagrams (traces name:variable).")},
    {"export_data", QT_TRANSLATE_NOOP("QucsControl", "Writes a diagram's traces, or a dataset's variables, to a file for another program: CSV, TSV, an Excel workbook, text in columns, NumPy or a Qucs-S dataset - as the Export tab does.")},
    {"set_simulator", QT_TRANSLATE_NOOP("QucsControl", "Chooses the simulator that simulate runs and get_netlist writes for.")},
    {"ngspice_commands", QT_TRANSLATE_NOOP("QucsControl", "Lists ngspice's commands by category, each in a line, and which the installed ngspice has; a command's syntax, example and how Qucs-S writes it; a search by what they do.")},
    {"run_script", QT_TRANSLATE_NOOP("QucsControl", "Runs a short JavaScript program that calls these tools (qucs.call) with loops and conditions, in one turn; 'atomic' restores every schematic if it throws.")},
    {"describe_tool", QT_TRANSLATE_NOOP("QucsControl", "Returns a tool's full description: what it does and returns, its fields and common mistakes. Without 'name', lists every tool's summary.")},
};

// The tools that change schematics alone: they take 'preview'.
const char* const kPreviewable[] = {"set_schematic", "add_component", "edit_component", "replace_component", "delete", "move", "arrange",
                                    "connect", "add_wire", "set_label", "add_analysis", "create_subcircuit", "rename_net",
                                    "add_painting", "edit_painting", "add_diagram", "edit_diagram", "add_trace", "edit_trace",
                                    "add_marker", "edit_marker", "delete_marker", "make_symbol", "set_subcircuit_parameters", "batch"};

// The tools most sessions use, loaded into every turn (Claude Code's
// "anthropic/alwaysLoad"); the others are found by its tool search when a
// task needs them, each with words to find it by.
const char* const kCore[] = {"get_state", "get_schematic", "set_schematic", "add_component", "edit_component", "delete",
                             "connect", "add_wire", "set_label", "move", "batch", "simulate", "get_dataset", "check_schematic",
                             "get_netlist", "undo", "screenshot", "open_document", "save_document", "describe_component_type",
                             "list_component_types", "describe_tool"};
const struct {
    const char* tool;
    const char* hint;
} kSearchHints[] = {
    {"new_document", "new schematic text document data display dpl report"},
    {"show_document", "bring document to front tab"},
    {"close_document", "close tab discard unsaved"},
    {"describe_format", "sch file line fields format component wire diagram painting"},
    {"replace_component", "swap substitute part type opamp subcircuit keep wiring pins"},
    {"add_analysis", "analysis ac transient dc sweep simulation block plot"},
    {"create_subcircuit", "group parts into subcircuit hierarchy block ports pin names"},
    {"select", "select highlight parts"},
    {"zoom", "zoom view fit region"},
    {"redo", "redo undone step"},
    {"undo_history", "undo steps history list"},
    {"list_actions", "menu actions commands"},
    {"trigger_action", "menu action run command"},
    {"get_dialog", "dialog read open window fields"},
    {"set_dialog", "dialog answer fill fields press button"},
    {"get_settings", "settings preferences options configuration read application simulators document cdl grid language path"},
    {"set_settings", "settings preferences options configuration change set application simulators document cdl grid language path"},
    {"console", "octave python shell terminal console command type run repl interpreter"},
    {"wait_for", "wait event notify until simulation finished dialog document changed file written watch poll"},
    {"simulation_status", "simulation status background progress running outcome result id long monte carlo"},
    {"stop_simulation", "stop abort cancel kill simulation run background"},
    {"send_input", "click mouse keyboard keys type drag double click raw input shortcut press reproduce"},
    {"read_help", "help documentation manual docs whats this tutorial examples paper"},
    {"synthesize_filter", "filter synthesis design lowpass highpass bandpass bandstop chebyshev butterworth bessel elliptic cauer lc ladder active sallen key"},
    {"synthesize_attenuator", "attenuator pad pi tee resistive synthesis design dB"},
    {"synthesize_matching", "matching network circuit impedance l-section stub quarter wave smith conjugate amplifier"},
    {"synthesize_power_combiner", "power combiner divider splitter wilkinson branchline hybrid coupler"},
    {"line_calc", "transmission line calculator microstrip coplanar coax stripline width impedance z0 effective permittivity"},
    {"receiver_budget", "receiver cascade budget noise figure friis gain ip3 p1db sensitivity mds sfdr rxcalc"},
    {"get_ui", "dock panel toolbar status bar tabs widget read log tuner problems operating point content projects components filter"},
    {"set_ui", "dock panel toolbar status bar tabs widget click button filter row select open expand slider tuner"},
    {"context_menu", "right-click context menu popup entries choose canvas part diagram tab file project"},
    {"add_diagram", "diagram plot graph rectangular polar smith table notation number format scientific engineering"},
    {"edit_diagram", "diagram axes limits log scale grid legend title theme colour color background dark notation number format scientific engineering decimals"},
    {"add_trace", "trace curve plot variable diagram"},
    {"edit_trace", "trace colour thickness style axis precision table digits"},
    {"reload_data", "reload dataset simulation results diagrams"},
    {"add_marker", "marker cursor value on curve peak 3dB notation number format scientific engineering precision"},
    {"edit_marker", "marker label precision format notation number format scientific engineering decimal"},
    {"delete_marker", "remove marker"},
    {"rename_net", "rename net label node"},
    {"arrange", "layout tidy arrange autoplace auto route reroute neat schematic feedback straighten supplies rails columns mirror"},
    {"add_painting", "drawing text arrow line box callout table formula dimension symbol"},
    {"edit_painting", "change drawing text arrow symbol port label"},
    {"export_netlist", "write netlist file spice cdl"},
    {"export_image", "picture png svg pdf export diagram"},
    {"list_documents", "files workspace project datasets list"},
    {"set_simulator", "simulator ngspice xyce qucsator choose"},
    {"move_to_pane", "split pane window layout"},
    {"build_verilog_a", "verilog-a openvaf compile osdi"},
    {"tune", "tune optimize sweep value until target hold constraint keep within bounds compare knobs targets"},
    {"new_project", "new project folder"},
    {"open_project", "open project"},
    {"copy_document", "copy schematic duplicate with results"},
    {"clean_scratch", "clear scratch files netlists"},
    {"rename_file", "rename move file folder document mv"},
    {"trash_file", "delete remove trash file folder document rm"},
    {"make_symbol", "subcircuit symbol draw pins sides inputs outputs supplies"},
    {"set_subcircuit_parameters", "subcircuit parameters params subckt default value symbol id prefix instance"},
    {"import_netlist", "spice netlist to schematic import"},
    {"import_data", "import csv tsv excel xlsx spreadsheet workbook measurement measured data file table columns npy npz numpy touchstone s2p plot dataset"},
    {"export_data", "export save write csv tsv excel xlsx spreadsheet workbook text columns numpy npz matlab python curves traces diagram plot graph data dataset variables file"},
    {"find_library_component", "library part search by values model"},
    {"list_libraries", "libraries list library panel sections installed user_lib search path project parts lib"},
    {"create_library", "create make export library lib subcircuits share user_lib"},
    {"import_library", "import add install bring library lib file vendor colleague spice subckt copy user_lib"},
    {"describe_part", "library part pins order names supply model macromodel transistor tested bench roles input output"},
    {"read_pdf", "datasheet pdf text read"},
    {"get_text", "text document tab read cir va verilog-a netlist script lines unsaved"},
    {"edit_text", "text document tab edit change replace lines cir va verilog-a netlist script"},
    {"goto_line", "text document line cursor show error"},
    {"diff", "compare revisions files changes"},
    {"run_script", "script loop javascript many calls"},
    {"ngspice_commands", "ngspice commands supported control nutmeg script syntax analysis meas let help spice"},
};

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

QJsonObject jsonResult(const QJsonValue& value, bool indented)
{
    // Compact: what Claude reads is tokens, and indentation is a third of
    // them and more.
    const QJsonDocument::JsonFormat format = indented ? QJsonDocument::Indented : QJsonDocument::Compact;
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
    // (And the file itself: TN.sch is tn.sch on a case-insensitive disk.)
    return ca == cb || (fa.exists() && fb.exists() && misc::isSameFile(ca, cb));
}

bool isQucsDocument(const QString& file)
{
    static const QStringList kinds{QStringLiteral("sch"), QStringLiteral("dpl"), QStringLiteral("sym"), QStringLiteral("va"),
                                   QStringLiteral("vhd"), QStringLiteral("vhdl"), QStringLiteral("v"), QStringLiteral("dat")};
    if (kinds.contains(QFileInfo(file).suffix().toLower())) return true;
    QFile f(file);
    return f.open(QIODevice::ReadOnly) && f.read(6) == "<Qucs ";
}

QString badFileName(const QString& name)
{
    if (name.isEmpty()) return tr("a file needs a name");
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return tr("%1 is a folder, not a file's name (\".\" as a file's name wrote a file named after the folder, beside it)").arg(name);
    static const QRegularExpression refused(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    if (const QRegularExpressionMatch m = refused.match(name); m.hasMatch())
        return tr("%1 is no file's name: a file's name has no %2 (no <>:\"/\\|?* or control characters, which Windows refuses)")
            .arg(name, m.captured(0).at(0).isPrint() ? m.captured(0) : tr("control character"));
    if (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.')) || name.startsWith(QLatin1Char(' ')))
        return tr("\"%1\" is no file's name: it begins with a space or ends with a space or a dot").arg(name);
    static const QRegularExpression device(QStringLiteral("^(con|prn|aux|nul|com\\d|lpt\\d)(\\..*)?$"), QRegularExpression::CaseInsensitiveOption);
    if (device.match(name).hasMatch()) return tr("%1 is no file's name: Windows keeps it for a device").arg(name);
    // (Before its suffix: "'.sch" is "'" and a suffix.)
    const QString stem = QFileInfo(name).completeBaseName().isEmpty() ? name : QFileInfo(name).completeBaseName();
    if (!stem.contains(QRegularExpression(QStringLiteral("[\\p{L}\\p{N}]")))) return tr("%1 is no file's name: it has no letter or digit").arg(name);
    return {};
}

bool usersOnly(const QWidget* w)
{
    for (; w != nullptr; w = w->parentWidget())
        if (w->property("qucsUsersOnly").toBool()) return true;
    return false;
}

QString absolute(const QString& path)
{
    if (QFileInfo(path).isAbsolute()) return QDir::cleanPath(path);
    // The open project's folder first (a file there, or a new one while a
    // project is open), then the workspace.
    const QString project = QucsSettings.QucsWorkDir.absolutePath();
    const QString workspace = QucsSettings.qucsWorkspaceDir.absolutePath();
    const QString inProject = QDir::cleanPath(QDir(project).absoluteFilePath(path));
    const QString inWorkspace = QDir::cleanPath(QDir(workspace).absoluteFilePath(path));
    if (QDir::cleanPath(project) == QDir::cleanPath(workspace) || QFileInfo::exists(inProject)) return inProject;
    if (QFileInfo::exists(inWorkspace)) return inWorkspace;
    return inProject;
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

// A region [x1, y1, x2, y2] as a rectangle, its corners kept on the canvas:
// [-2^31, -2^31, 2^31-1, 2^31-1] overflowed its width, and found nothing.
QRect regionOf(const QJsonArray& r)
{
    const auto at = [&r](int i) { return misc::clampCoordinate(int(std::clamp(std::isfinite(r.at(i).toDouble()) ? r.at(i).toDouble() : 0.0, -1e9, 1e9))); };
    return QRect(QPoint(at(0), at(1)), QPoint(at(2), at(3))).normalized();
}

// Why \a name cannot name a part (empty when it can): as the component
// dialog takes one - a letter, then letters, digits and _. "R 9" gave an
// extra node, "R;9" and "R$9" began a SPICE comment, "R9.2" made pins
// R9.2.1 that connect could not tell, "9R" was netlisted as R9R.
QString badPartName(const QString& name)
{
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
    if (ok.match(name).hasMatch()) return {};
    return tr("%1 cannot name a part: a letter first, then letters, digits and _ (as the component dialog takes a name)").arg(name);
}

// Why \a name cannot name a net (empty when it can): as the label dialog
// takes one - a letter, then letters, digits and single _ - and not
// ground's names (0, gnd: a net so called was shorted to ground, silently)
// or those get_schematic gives nets without a label (net2: two nets of one
// name then).
QString badNetName(const QString& name)
{
    if (name == QLatin1String("0") || name.compare(QLatin1String("gnd"), Qt::CaseInsensitive) == 0)
        return tr("%1 is ground's name: a net so called is ground in the netlist, shorted to it (connect with 'ground' grounds a "
                  "pin, if that is meant)").arg(name);
    static const QRegularExpression automatic(QStringLiteral("^net\\d+$"), QRegularExpression::CaseInsensitiveOption);
    if (automatic.match(name).hasMatch())
        return tr("%1 is a name get_schematic gives a net without a label: another net may be called so now or later - choose "
                  "another").arg(name);
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z](?:[A-Za-z0-9]|_(?!_))*!?$"));
    if (!ok.match(name).hasMatch())
        return tr("%1 cannot name a net: a letter first, then letters, digits and single _ (as the label dialog takes a name)").arg(name);
    return {};
}

QString propertyValue(const QJsonValue& v)
{
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 12);
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

// The quarter turns a type has as the library makes it (a DC source is
// made turned once): the tools' rotation counts from there.
int defaultRotation(const QString& model)
{
    static QHash<QString, int> cache;
    if (const auto it = cache.constFind(model); it != cache.cend()) return *it;
    int turns = 0;
    if (std::unique_ptr<Component> fresh{newComponent(model)}) turns = fresh->rotated;
    cache.insert(model, turns);
    return turns;
}

// \a c, new and as its type draws it, turned and mirrored as get_schematic
// tells them: \a turns quarter turns from its type's own orientation, and
// mirrored - in the order a file is read (Component::load: mirrored, then
// turned to its count). Turned first, then mirrored, a part both mirrored
// and turned read back turned 180 degrees: a source's polarity reversed.
void orient(Component* c, int turns, bool mirror)
{
    const int want = ((turns + defaultRotation(c->Model)) % 4 + 4) % 4;
    if (mirror != c->mirroredX) c->mirrorX();
    for (int i = 0; i < 4 && c->rotated != want; ++i) c->rotate();
}

// The rotation (as get_schematic gives it) that puts pin 1 of a part of
// two pins on \a side of its centre - top, bottom, left or right -
// mirrored as \a mirror: tried on \a c (new, not placed), left turned so.
// -1, and why, when it has not two pins or no turn does.
int rotationForPin1(Component* c, const QString& side, bool mirror, QString* why)
{
    static const QStringList sides{QStringLiteral("top"), QStringLiteral("bottom"), QStringLiteral("left"), QStringLiteral("right")};
    if (!sides.contains(side)) {
        *why = tr("'pin1' is top, bottom, left or right: the side of its centre pin 1 goes to.");
        return -1;
    }
    if (c->Ports.size() != 2) {
        *why = tr("'pin1' places a part of two pins; %1 has %2: give 'rotation' (describe_component_type gives its pins as each "
                  "rotation places them).").arg(c->Model).arg(c->Ports.size());
        return -1;
    }
    for (int turns = 0; turns < 4; ++turns) {
        orient(c, turns, mirror);
        const QPoint p(c->Ports.at(0)->x, c->Ports.at(0)->y);
        const QString at = std::abs(p.y()) > std::abs(p.x()) ? (p.y() < 0 ? sides.at(0) : sides.at(1)) : (p.x() < 0 ? sides.at(2) : sides.at(3));
        if (at == side && p != QPoint()) return turns;
    }
    *why = tr("no turn of %1 puts pin 1 at the %2: its pins are not either side of its centre - give 'rotation'.").arg(c->Model, side);
    return -1;
}

// Where each of its texts is on the schematic, [x1, y1, x2, y2]: its name,
// then each property shown for the simulator in the settings, one under
// the other from its text's corner - as they are drawn.
QJsonArray textBoxes(const Component* c)
{
    QJsonArray texts;
    const QList<QRect> boxes = qucs_s::textplace::textBoxes(c);
    QStringList shown;
    if (c->showName && !c->Name.isEmpty()) shown << c->Name;
    for (const Property* p : c->Props)
        if (p->display && (p->simulators & QucsSettings.DefaultSimulator) == QucsSettings.DefaultSimulator) shown << p->displayText();
    for (int i = 0; i < boxes.size() && i < shown.size(); ++i) {
        const QRect& box = boxes.at(i);
        texts.append(QJsonObject{{QStringLiteral("text"), shown.at(i)},
                                 {QStringLiteral("box"), QJsonArray{box.left(), box.top(), box.right(), box.bottom()}}});
    }
    return texts;
}

QJsonObject componentJson(Component* c)
{
    QJsonArray props;
    for (Property* p : c->Props) {
        QJsonObject prop{{QStringLiteral("name"), p->Name}, {QStringLiteral("value"), p->Value}, {QStringLiteral("shown"), p->display}};
        // (An .OPTIONS option with no value is written alone: a flag.)
        if (c->Model == QLatin1String("SpiceOptions") && p->Name != QLatin1String("XyceOptionPackage") && p->Value.trimmed().isEmpty())
            prop.insert(QStringLiteral("flag"), true);
        props.append(prop);
    }
    QJsonArray pins;
    for (int i = 0; i < c->Ports.size(); ++i) {
        const Port* pp = c->Ports.at(i);
        QJsonObject pin{{QStringLiteral("pin"), i + 1}, {QStringLiteral("x"), c->cx + pp->x}, {QStringLiteral("y"), c->cy + pp->y}};
        if (!pp->Name.isEmpty()) pin.insert(QStringLiteral("name"), pp->Name);
        if (pp->LabelSet) pin.insert(QStringLiteral("label"), pp->Label);   // (drawn instead of the name)
        if (pp->Connection != nullptr) {
            pin.insert(QStringLiteral("connected"), pp->Connection->conn_count() > 1);
            if (pp->Connection->hasLabel()) pin.insert(QStringLiteral("net"), pp->Connection->label()->Name);
        }
        pins.append(pin);
    }
    const int base = defaultRotation(c->Model);
    QJsonObject json{{QStringLiteral("name"), c->Name},
                     {QStringLiteral("type"), c->Model},
                     {QStringLiteral("description"), c->Description},
                     {QStringLiteral("x"), c->cx},
                     {QStringLiteral("y"), c->cy},
                     {QStringLiteral("rotation"), ((c->rotated - base) % 4 + 4) % 4},
                     {QStringLiteral("mirrored"), c->mirroredX},
                     {QStringLiteral("active"), c->isActive == COMP_IS_ACTIVE},
                     {QStringLiteral("text at"), QJsonArray{c->tx, c->ty}},
                     {QStringLiteral("properties"), props},
                     {QStringLiteral("pins"), pins}};
    // (The file's rotation is the symbol's own: a DC source is made turned.)
    if (base != 0) json.insert(QStringLiteral("rotation in the file"), c->rotated);
    if (!c->showName) json.insert(QStringLiteral("name shown"), false);
    // Where its texts are, to place them clear of a wire or a label.
    if (const QJsonArray texts = textBoxes(c); !texts.isEmpty()) json.insert(QStringLiteral("texts"), texts);
    return json;
}

QString propertyNames(Component* c)
{
    QStringList names;
    for (Property* p : c->Props) names << p->Name;
    return names.join(QStringLiteral(", "));
}

bool isEquationKind(const Component* c);
bool isStatisticsKind(const Component* c);

// "R1 has no property X; its properties are: ..." - and where an equation
// block's new equations, or a Monte Carlo's records, go instead.
QString noSuchProperty(const Component* c, const QString& names)
{
    QString text = tr("%1 has no property %2; its properties are: %3.")
                       .arg(c->Name.isEmpty() ? c->Model : c->Name, names, propertyNames(const_cast<Component*>(c)));
    if (isEquationKind(c))
        text += tr(" It is an equation block: 'equations' sets its equations, new ones too ({\"gain_db\": \"db(v(out))\"}).");
    else if (isStatisticsKind(c))
        text += tr(" 'records' and 'specs' set what it records and the limits it judges by.");
    return text;
}

// Whether \a name is a property \a c may have any number of: NgSweep's
// Record and Vs, a Monte Carlo's or corners' Record and Spec, NgOpt's Knob,
// Target and Constraint, an optimization's Var and Goal.
bool isRepeated(const Component* c, const QString& name)
{
    if (qucs_s::ngsweep::isSweep(c)) return name == QLatin1String("Record") || name == QLatin1String("Vs");
    if (qucs_s::ngstats::isStatistics(c)) return name == QLatin1String("Record") || name == QLatin1String("Spec");
    if (c->Model == QLatin1String(".NGOPT"))
        return name == QLatin1String("Knob") || name == QLatin1String("Target") || name == QLatin1String("Constraint");
    if (c->Model == QLatin1String(".Opt")) return name == QLatin1String("Var") || name == QLatin1String("Goal");
    // (Not any name that comes twice: a parameter sweep of a list calls two
    // properties in their places Symbol.)
    return false;
}

} // namespace

QString qucs_s::control::propertyNote(const Component* c, const QString& name)
{
    if (c->Model == QLatin1String(".NGOPT")) return qucs_s::ngopt::propertyNote(name);
    return {};
}

QJsonArray qucs_s::control::repeatedProperties(const Component* c)
{
    struct Doc {
        const char *name, *fields, *example;
    };
    QList<Doc> docs;
    if (c->Model == QLatin1String(".NGOPT"))
        docs = {{"Knob", QT_TRANSLATE_NOOP("QucsControl", "kind|name|initial|low|high - kind dparam (a .PARAM's or an equation's parameter), param (a device's: R1, @m1[w]) or mparam (a .model's: @dmod[is]); the search starts at the initial value"),
                 "dparam|Cp|20p|5p|100p"},
                {"Target", QT_TRANSLATE_NOOP("QucsControl", "analysis|expression|value|weight - the analysis (a simulation block's name, or an ngspice command), an expression of its results (an index picks a point: db(S_2_1[20])), the value it should have, and its weight (1 when left out)"),
                 "SP1|db(S_2_1[20])|-0.0771|1"},
                {"Constraint", QT_TRANSLATE_NOOP("QucsControl", "analysis|expression|min|max - an expression of an analysis' results held at or above min and at or below max (one may be left empty) while Minimize is minimized or the Targets fitted, by any Method; the analysis empty: Minimize's (the first Target's). simulate's 'optimum' gives each constraint at the end - its value, active or its slack, and the multiplier (what a unit of the bound costs) - and status infeasible when one cannot be met; CTol is how near counts as met"),
                 "OP1|v(out)|0.9|"}};
    else if (qucs_s::ngsweep::isSweep(c))
        docs = {{"Record", QT_TRANSLATE_NOOP("QucsControl", "name|expression - a value recorded at each point of the sweep"), "gain|db(v(out))"},
                {"Vs", QT_TRANSLATE_NOOP("QucsControl", "name|type|start|stop|points|list - an outer knob: lin or log from start to stop in points, or list (values apart by ;)"),
                 "C1|list|||| 1n; 2n"}};
    else if (qucs_s::ngstats::isStatistics(c))
        docs = {{"Record", QT_TRANSLATE_NOOP("QucsControl", "name|expression - a value recorded for every sample or corner"), "gain|db(v(out))"},
                {"Spec", QT_TRANSLATE_NOOP("QucsControl", "expression|min|max - a metric and its limits (one may be left empty); a sample passes within all"),
                 "v(out)|0.49|0.51"}};
    else if (c->Model == QLatin1String(".Opt"))
        docs = {{"Var", QT_TRANSLATE_NOOP("QucsControl", "name|active|initial|min|max|type - active yes or no; type LIN_DOUBLE, LOG_DOUBLE, LIN_INT, LOG_INT or an E series (E12, E24, ...)"),
                 "Rload|yes|1000|10|1e4|LOG_DOUBLE"},
                {"Goal", QT_TRANSLATE_NOOP("QucsControl", "name|type|value - type MIN, MAX, LE, GE, EQ or MON"), "gain|GE|20"}};
    QJsonArray list;
    for (const Doc& d : std::as_const(docs))
        list.append(QJsonObject{{QStringLiteral("name"), QString::fromLatin1(d.name)},
                                {QStringLiteral("fields"), QucsControl::tr(d.fields)},
                                {QStringLiteral("example"), QString::fromLatin1(d.example)},
                                {QStringLiteral("give"), QStringLiteral("{\"%1\": [\"%2\", ...]}").arg(QLatin1String(d.name), QLatin1String(d.example))}});
    return list;
}

namespace {


// A property that comes more than once - NgSweep's Record and Vs, a Monte
// Carlo's Record and Spec - is given as a list of its values: they replace
// every one of that name, after the others. (As one value, the JSON form
// kept only the last.)
void setRepeated(Component* c, const QString& name, const QJsonArray& values)
{
    bool display = true, seen = false;
    for (qsizetype i = c->Props.size() - 1; i >= 0; --i)
        if (c->Props.at(i)->Name == name) {
            display = c->Props.at(i)->display;
            seen = true;
            delete c->Props.takeAt(i);
        }
    if (!seen && !values.isEmpty()) display = true;
    for (const QJsonValue& v : values) c->Props.append(new Property(name, propertyValue(v), display, QString()));
}

// Sets \a properties by name; false and which are unknown in \a error.
// A new part (\a fresh) also takes those its type
// makes only once made again - an equation-defined device's branches (I2,
// Q2 with Branches 2), a subcircuit's parameters (with its File) - and, when
// a subcircuit's file is not found, its parameters as they are given.
bool setProperties(Component* c, const QJsonObject& properties, QString* error, bool fresh = false)
{
    const auto unknownOf = [&] {
        QStringList unknown;
        for (auto it = properties.begin(); it != properties.end(); ++it)
            if (c->getProperty(it.key()) == nullptr && !isRepeated(c, it.key())) unknown << it.key();
        return unknown;
    };
    const auto set = [&] {
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (isRepeated(c, it.key()))
                setRepeated(c, it.key(), it.value().isArray() ? it.value().toArray() : QJsonArray{it.value()});
            else if (it.value().isArray()) setRepeated(c, it.key(), it.value().toArray());
            else if (Property* p = c->getProperty(it.key())) p->Value = propertyValue(it.value());
        }
    };
    QStringList unknown = unknownOf();
    if (!unknown.isEmpty() && fresh) {
        // Those it has, then made again: those it lacked, if it has them now
        // (what was set, renamed by recreate() - a list sweep's Start and
        // Stop are Symbol then - is set already).
        set();
        c->recreate();
        QStringList still;
        for (const QString& n : std::as_const(unknown))
            if (c->getProperty(n) == nullptr && !isRepeated(c, n)) still << n;
        const bool sub = c->Model == QLatin1String("Sub") || c->Model == QLatin1String("Lib");
        const int own = c->Model == QLatin1String("Lib") ? 2 : 1;   // (File; Lib and Comp)
        if (!still.isEmpty() && sub && c->Props.size() <= own) {
            for (const QString& n : std::as_const(still))
                c->Props.append(new Property(n, propertyValue(properties.value(n)), true, QStringLiteral(" ")));
            still.clear();
        }
        // A type whose last property has no description takes more, each
        // by its name (a .MODEL's Line_6), as the file loader does - in
        // their names' order, Line_10 after Line_9.
        const bool extendable = !c->Props.isEmpty() && c->Model != QLatin1String("EDD") && c->Model != QLatin1String("RFEDD")
                                && c->Model != QLatin1String("RFEDD2P")
                                && (c->Props.last()->Description.isEmpty() || c->Props.last()->Description == QLatin1String("Expression"));
        if (!still.isEmpty() && extendable) {
            QCollator natural;
            natural.setNumericMode(true);
            std::sort(still.begin(), still.end(), natural);
            for (const QString& n : std::as_const(still))
                c->Props.append(new Property(n, propertyValue(properties.value(n)), false, QString()));
            still.clear();
        }
        if (!still.isEmpty()) {
            *error = noSuchProperty(c, still.join(QStringLiteral(", ")));
            return false;
        }
        for (const QString& n : std::as_const(unknown)) {
            const QJsonValue v = properties.value(n);
            if (isRepeated(c, n)) setRepeated(c, n, v.isArray() ? v.toArray() : QJsonArray{v});
            else if (Property* p = c->getProperty(n)) p->Value = propertyValue(v);
        }
        return true;
    }
    if (!unknown.isEmpty()) {
        *error = noSuchProperty(c, unknown.join(QStringLiteral(", ")));
        return false;
    }
    set();
    return true;
}

// A value that does not read as what its property takes - a number where
// the type's default is one ("1,5k", "1.2.3", nothing): told, not refused.
// A name (a parameter's), an expression or a value in braces is taken as
// meant.
// The names \a sch's equation blocks (Eqn, .PARAM, ...) define: what a
// property's value may name instead of a number.
QSet<QString> definedNames(const Schematic* sch)
{
    QSet<QString> names;
    if (sch == nullptr) return names;
    for (const Component* c : sch->a_DocComps)
        if (isEquationKind(c))
            for (const Property* p : c->Props) names << p->Name;
    return names;
}

// Digits, with letters after them that are no scale letter and unit - 1kk,
// 10uu: a typo, refused (SPICE would read 1k, the letters after ignored).
// A number reads as digits; a scale letter (f p n u m k M G T, meg, mil); a
// unit (Ohm, Hz, s, V, A, F, H, S, W, dB, dBm, m, deg ...), spelt out or
// plural too (Ohms, Volts, Amps, Hertz), a dB of any reference (dBV, dBc),
// degC, and a rate (V/us) - what every value of the examples and every
// type's default has. Empty when \a value is none, or \a byDefault (its
// type's) is no number.
QString numberTypo(const QString& who, const QString& property, const QString& value, const QString& byDefault)
{
    static const QRegularExpression loose(QStringLiteral(
        "^\\s*[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?\\s*([A-Za-z\u00b5\u03a9\u00b0%/]*)\\s*$"));
    static const QString scale = QStringLiteral("(?:meg|mil|[fpnu\u00b5mkgtea])");
    static const QString unit = QStringLiteral(
        "(?:ohms?|\u03a9|hz|hertz|s|secs?|seconds?|v|volts?|a|amps?|amperes?|f|farads?|h|henr(?:y|ys|ies)|w|watts?|j|joules?"
        "|db(?:m|uv|w|v|c|i|mv|fs)?|m|met(?:er|re)s?|deg(?:c|f|rees?)?|\u00b0[cf]?|c|rad|%|ppm|mhos?|siemens|bits?|baud|dec|oct)");
    static const QRegularExpression strict(
        QStringLiteral("^\\s*[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?\\s*%1?\\s*(?:%2(?:\\s*/\\s*%1?%2)?)?\\s*$")
            .arg(scale, unit),
        QRegularExpression::CaseInsensitiveOption);
    if (!loose.match(byDefault).hasMatch() || strict.match(value).hasMatch()) return {};
    const QRegularExpressionMatch m = loose.match(value);
    if (!m.hasMatch()) return {};
    return tr("%1: %2 = \"%3\" is no number: after the digits, \"%4\" is no scale letter (f p n u m k M G T, meg, mil) "
              "and unit (Ohm, Hz, s, V, A, F, H, S, W, dB, dBV, m, degC; Ohms, Volts ...; V/us)")
        .arg(who, property, value.trimmed(), m.captured(1));
}

// Those of the properties \a props of a part of type \a model, named \a who.
QStringList numberTypos(const QString& who, const QString& model, const QJsonObject& props)
{
    QStringList typos;
    std::unique_ptr<Component> fresh(newComponent(model));
    if (!fresh) return typos;
    for (auto it = props.begin(); it != props.end(); ++it)
        if (const Property* d = fresh->getProperty(it.key()))
            if (const QString t = numberTypo(who, it.key(), propertyValue(it.value()), d->Value); !t.isEmpty()) typos << t;
    return typos;
}

QStringList valueNotes(Component* c, const QStringList& names, const Schematic* sch)
{
    const QSet<QString> defined = definedNames(sch);
    static const QRegularExpression number(QStringLiteral(
        "^\\s*[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?\\s*[A-Za-z\u00b5\u03a9\u00b0%/]*\\s*$"));
    static const QRegularExpression name(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.]*$"));
    static const QRegularExpression expression(QStringLiteral("[{}()*/^+]|[A-Za-z_]\\w*\\s*[-+*/]"));
    // A property of a few words to choose from says them in its description,
    // "schematic symbol [european, US]".
    static const QRegularExpression options(QStringLiteral("\\[([^\\[\\]]+)\\]\\s*$"));
    std::unique_ptr<Component> fresh(newComponent(c->Model));
    QStringList notes;
    for (const QString& n : names) {
        const Property* p = c->getProperty(n);
        const Property* d = fresh ? fresh->getProperty(n) : nullptr;
        if (p == nullptr || d == nullptr) continue;
        const QString v = p->Value.trimmed();
        const QString who = c->Name.isEmpty() ? c->Model : c->Name;
        if (const QRegularExpressionMatch m = options.match(d->Description); m.hasMatch() && !v.isEmpty()) {
            QStringList words;
            for (const QString& w : m.captured(1).split(QLatin1Char(','))) words << w.trimmed();
            // (Words only: "[0, 1]" or "[1 ... 4]" is a range of numbers.)
            bool allWords = words.size() >= 2;   // (one word in brackets may be a unit)
            for (const QString& w : std::as_const(words)) allWords = allWords && !w.isEmpty() && !w.at(0).isDigit() && !w.contains(QLatin1String("..."));
            if (allWords && std::none_of(words.cbegin(), words.cend(), [&v](const QString& w) { return w.compare(v, Qt::CaseInsensitive) == 0; })) {
                notes << tr("%1: %2 = \"%3\" is not one of its choices (%4)").arg(who, n, v, words.join(QStringLiteral(", ")));
                continue;
            }
        }
        if (!number.match(d->Value).hasMatch()) continue;
        if (number.match(v).hasMatch() || expression.match(v).hasMatch()) continue;
        // A word: a parameter's name - one an equation block here defines;
        // else said, as it may be a value in the wrong place ("european").
        if (name.match(v).hasMatch()) {
            if (sch == nullptr || defined.contains(v)) continue;
            notes << tr("%1: %2 = \"%3\" is no number, and no equation block here defines %3 (its default is %4; fine if "
                        "it is a subcircuit's parameter or from an included file)")
                         .arg(who, n, v, d->Value);
            continue;
        }
        notes << tr("%1: %2 = \"%3\" does not read as a number (its default is %4; a scale letter - f p n u m k M G T - "
                    "and a unit may follow, a decimal point, not a comma)")
                     .arg(who, n, v, d->Value);
    }
    return notes;
}

// What is easy to get wrong with a type, in a sentence: said when one is
// placed (describe_component_type tells more).
QString trapOf(const QString& type)
{
    if (type == QLatin1String("Vpulse") || type == QLatin1String("Ipulse"))
        return tr("A %1 is one pulse, T1 to T2 (under SPICE a PULSE with a period of 1e9 s); a repeating square wave is %2.")
            .arg(type, type.at(0) == QLatin1Char('V') ? QStringLiteral("Vrect") : QStringLiteral("Irect"));
    if (type == QLatin1String("Vdc") || type == QLatin1String("Idc"))
        return tr("A %1 is DC only: nothing in an AC analysis (Vac and Iac drive one).").arg(type);
    if (type == QLatin1String("Vac") || type == QLatin1String("Iac"))
        return tr("Its %1 is also its AC magnitude unless ACmag is set: %1 = 0 silences an AC analysis too.")
            .arg(type == QLatin1String("Vac") ? QStringLiteral("U") : QStringLiteral("I"));
    if (type == QLatin1String("Eqn") || type == QLatin1String("NutmegEq"))
        return tr("Name its variables unlike the nets: under ngspice a variable and a node of one name clash, without an error.");
    if (type == QLatin1String("OpAmp"))
        return tr("Its output is clipped at Umax (15 V by default), which is not shown on the schematic.");
    if (type == QLatin1String("SpiceOptions"))
        return tr("An option with no value is a flag: 'flags' or {\"noinit\": true}.");
    return {};
}

// ---- equations: the components whose properties are "name = value" lines

// As the component dialog has them: their properties are equations, as
// many as wanted, around fixed fields (a Nutmeg equation's Simulation, the
// .OPTIONS' Xyce package, a Qucsator equation's Export).
bool isEquationKind(const Component* c)
{
    static const QSet<QString> models{QStringLiteral("Eqn"), QStringLiteral("NutmegEq"), QStringLiteral("SpiceIC"),
                                      QStringLiteral("SpicePar"), QStringLiteral("SpiceOptions"), QStringLiteral("SpiceFunc"),
                                      QStringLiteral("SpiceCSPar"), QStringLiteral("SpGlobPar"), QStringLiteral("SpiceNodeset")};
    return models.contains(c->Model);
}

bool isFixedField(const Property* p)
{
    return p->Name == QLatin1String("Simulation") || p->Name == QLatin1String("XyceOptionPackage")
           || p->Name == QLatin1String("Export");
}

bool isStatisticsKind(const Component* c)
{
    return qucs_s::ngstats::isStatistics(c);
}

// 'equations' in order: an object ({"gain": "db(v(out))"}; its keys come
// sorted), or a list of "name=value", [name, value] or {"name", "value"}.
// A value null, false or "" takes that one away; true - or, in a list, a
// name alone - is a flag: there with no value (.OPTIONS noinit).
struct Equation {
    QString name;
    QString value;
    bool flag = false;
};
using Equations = QList<Equation>;
bool equationsOf(const QJsonValue& v, Equations* out, QString* error)
{
    const auto named = [&](const QString& name) {
        const QString n = name.trimmed();
        if (n.isEmpty() || n.contains(QLatin1Char('=')) || n.contains(QLatin1Char('"')) || n.contains(QLatin1Char('\n'))
            || n.contains(QLatin1Char(' '))) {
            *error = tr("An equation's name is its variable - letters, digits, _ - with no = or quotes (%1).").arg(name);
            return QString();
        }
        return n;
    };
    const auto take = [&](const QString& name, const QJsonValue& value) {
        const QString n = named(name);
        if (n.isEmpty()) return false;
        if (value.isBool()) {
            out->append({n, QString(), value.toBool()});   // (false: taken away)
            return true;
        }
        if (!value.isNull() && !value.isString() && !value.isDouble()) {
            *error = tr("%1's value is its expression, as text (true: a flag, with no value).").arg(n);
            return false;
        }
        const QString text = value.isNull() ? QString() : propertyValue(value).trimmed();
        if (text.contains(QLatin1Char('"'))) {
            *error = tr("%1: an expression has no double quotes (the file keeps them as two single ones).").arg(n);
            return false;
        }
        out->append({n, text});
        return true;
    };
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (!take(it.key(), it.value())) return false;
        return true;
    }
    if (!v.isArray()) {
        *error = tr("'equations' is {\"name\": \"expression\", ...} or a list of \"name=expression\" (in their order).");
        return false;
    }
    for (const QJsonValue& e : v.toArray()) {
        if (e.isString()) {
            const QString s = e.toString();
            const int eq = int(s.indexOf(QLatin1Char('=')));
            if (eq < 0) {   // a name alone: a flag
                const QString n = named(s);
                if (n.isEmpty()) return false;
                out->append({n, QString(), true});
                continue;
            }
            if (eq == 0) {
                *error = tr("\"%1\" is not name=expression.").arg(s);
                return false;
            }
            if (!take(s.left(eq), s.mid(eq + 1))) return false;
        } else if (e.isArray() && e.toArray().size() == 2 && e.toArray().at(0).isString()) {
            if (!take(e.toArray().at(0).toString(), e.toArray().at(1))) return false;
        } else if (e.isObject() && e.toObject().value(QLatin1String("name")).isString()) {
            if (!take(e.toObject().value(QLatin1String("name")).toString(), e.toObject().value(QLatin1String("value")))) return false;
        } else if (e.isObject() && e.toObject().size() == 1) {   // {"k": null} in the list: k taken away
            if (!take(e.toObject().begin().key(), e.toObject().begin().value())) return false;
        } else {
            *error = tr("Each of 'equations' is \"name=expression\", [name, expression] or {\"name\": ..., \"value\": ...}.");
            return false;
        }
    }
    return true;
}

// Whether a block writes an equation with no value as a flag on its own:
// .OPTIONS (noinit, noopiter, keepopinfo, ...). The others would write
// "name=" - not an equation.
bool takesFlags(const Component* c)
{
    return c->Model == QLatin1String("SpiceOptions");
}

// Sets an equation block's equations: each one given takes its value (a
// new one goes after the others), an empty one goes, a flag is there with
// no value; with \a replace they are the equations given alone. The fixed
// fields stay where they are.
void setEquations(Component* c, const Equations& equations, bool replace)
{
    if (replace)
        for (int i = int(c->Props.size()) - 1; i >= 0; --i)
            if (!isFixedField(c->Props.at(i))) delete c->Props.takeAt(i);
    for (const auto& [name, value, flag] : equations) {
        int at = -1;
        for (int i = 0; i < c->Props.size(); ++i)
            if (!isFixedField(c->Props.at(i)) && c->Props.at(i)->Name == name) at = i;
        if (value.isEmpty() && !flag) {
            if (at >= 0) delete c->Props.takeAt(at);
            continue;
        }
        if (at >= 0) {
            c->Props.at(at)->Value = value;
            continue;
        }
        // Before a trailing fixed field (Export).
        int where = int(c->Props.size());
        while (where > 0 && isFixedField(c->Props.at(where - 1)) && c->Props.at(where - 1)->Name == QLatin1String("Export")) --where;
        c->Props.insert(where, new Property(name, value, true));
    }
}

// A Monte Carlo's or corners' 'records' ([{"name", "expression"}] or
// "name|expression") and 'specs' ([{"expression", "min", "max"}] or
// "expression|min|max"); false and why when one does not read.
bool recordsOf(const QJsonValue& v, QList<qucs_s::ngstats::Record>* out, QString* error)
{
    if (!v.isArray()) {
        *error = tr("'records' is a list: [{\"name\": \"gain\", \"expression\": \"db(v(out))\"}, ...].");
        return false;
    }
    for (const QJsonValue& e : v.toArray()) {
        qucs_s::ngstats::Record r;
        if (e.isString()) {
            if (!qucs_s::ngstats::Record::parse(e.toString(), &r)) {
                *error = tr("A record is \"name|expression\" or {\"name\": ..., \"expression\": ...} (%1).").arg(e.toString());
                return false;
            }
        } else {
            r.name = e.toObject().value(QLatin1String("name")).toString().trimmed();
            r.expression = e.toObject().value(QLatin1String("expression")).toString().trimmed();
        }
        if (r.name.isEmpty() || r.expression.isEmpty() || r.name.contains(QLatin1Char('|')) || r.expression.contains(QLatin1Char('"'))) {
            *error = tr("A record has a name and an expression: {\"name\": \"gain\", \"expression\": \"db(v(out))\"}.");
            return false;
        }
        out->append(r);
    }
    return true;
}

bool specsOf(const QJsonValue& v, QList<qucs_s::ngstats::Spec>* out, QString* error)
{
    if (!v.isArray()) {
        *error = tr("'specs' is a list: [{\"expression\": \"gain\", \"min\": \"19\", \"max\": \"21\"}, ...].");
        return false;
    }
    for (const QJsonValue& e : v.toArray()) {
        qucs_s::ngstats::Spec s;
        if (e.isString()) {
            if (!qucs_s::ngstats::Spec::parse(e.toString(), &s)) {
                *error = tr("A spec is \"expression|min|max\" or {\"expression\": ..., \"min\": ..., \"max\": ...} (%1).").arg(e.toString());
                return false;
            }
        } else {
            const QJsonObject o = e.toObject();
            s.expression = o.value(QLatin1String("expression")).toString().trimmed();
            s.min = propertyValue(o.value(QLatin1String("min"))).trimmed();
            s.max = propertyValue(o.value(QLatin1String("max"))).trimmed();
            if (o.value(QLatin1String("min")).isUndefined() || o.value(QLatin1String("min")).isNull()) s.min.clear();
            if (o.value(QLatin1String("max")).isUndefined() || o.value(QLatin1String("max")).isNull()) s.max.clear();
        }
        if (s.expression.isEmpty() || (s.min.isEmpty() && s.max.isEmpty()) || s.expression.contains(QLatin1Char('|'))) {
            *error = tr("A spec has an expression and a limit at least: {\"expression\": \"gain\", \"min\": \"19\"}.");
            return false;
        }
        out->append(s);
    }
    return true;
}

// Whether a call gives a component's lists - 'equations', 'flags',
// 'records', 'specs' -, which setListsOf() applies: one place says which,
// so that a caller's test of whether to apply them and setListsOf() agree
// ('flags' alone was once checked and never applied).
bool givesLists(const QJsonObject& args)
{
    for (const char* key : {"equations", "flags", "records", "specs"})
        if (args.contains(QLatin1String(key))) return true;
    return false;
}

// 'equations' and 'flags' (with 'replace_equations'), 'records' and
// 'specs' on a component (a \a fresh one: being placed): checked, and
// applied unless \a check. False and why when they do not read or the
// component takes none.
bool setListsOf(Component* c, const QJsonObject& args, QString* error, bool check, bool fresh = false)
{
    const bool flags = args.contains(QLatin1String("flags"));
    const bool eq = args.contains(QLatin1String("equations")) || flags, rec = args.contains(QLatin1String("records")),
               spec = args.contains(QLatin1String("specs"));
    // What it replaces the list with is given beside it, or it does nothing.
    if (args.contains(QLatin1String("replace_equations")) && !eq) {
        *error = tr("'replace_equations' goes with 'equations' or 'flags': it makes the block's list exactly those given.");
        return false;
    }
    if (!givesLists(args)) return true;
    if (flags && !isEquationKind(c)) {
        *error = tr("%1 (%2) has no flags: 'flags' are the options with no value of an ngspice .OPTIONS block (SpiceOptions).")
                     .arg(c->Name, c->Model);
        return false;
    }
    if (eq && !isEquationKind(c)) {
        *error = tr("%1 (%2) has no equations: 'equations' is for an equation block - Eqn, NutmegEq, .PARAM "
                    "(SpicePar), .OPTIONS (SpiceOptions), .FUNC, .IC, .NODESET, ... Its properties are: %3.")
                     .arg(c->Name, c->Model, propertyNames(c));
        return false;
    }
    if ((rec || spec) && !isStatisticsKind(c)) {
        *error = tr("%1 (%2) has no records or specs: they are an ngspice Monte Carlo's or corners' (NgMonteCarlo, NgCorners).")
                     .arg(c->Name, c->Model);
        return false;
    }
    Equations equations;
    QList<qucs_s::ngstats::Record> records;
    QList<qucs_s::ngstats::Spec> specs;
    if (args.contains(QLatin1String("equations")) && !equationsOf(args.value(QLatin1String("equations")), &equations, error))
        return false;
    if (flags) {
        if (!args.value(QLatin1String("flags")).isArray()) {
            *error = tr("'flags' is a list of the options with no value: [\"noinit\", \"keepopinfo\"].");
            return false;
        }
        for (const QJsonValue& f : args.value(QLatin1String("flags")).toArray()) {
            const QString n = f.toString().trimmed();
            if (!f.isString() || n.isEmpty() || n.contains(QRegularExpression(QStringLiteral("[\\s=\"]")))) {
                *error = tr("Each of 'flags' is an option's name, alone: \"noinit\".");
                return false;
            }
            equations.append({n, QString(), true});
        }
    }
    // A flag - an equation with no value - only where the block writes one
    // alone.
    if (!takesFlags(c))
        for (const Equation& e : std::as_const(equations))
            if (e.flag) {
                *error = tr("%1 has no value: only an ngspice .OPTIONS block (SpiceOptions) takes a flag, an option with no "
                            "value (noinit); %2's equations each have one - name=expression.").arg(e.name, c->Name);
                return false;
            }
    if (rec && !recordsOf(args.value(QLatin1String("records")), &records, error)) return false;
    if (spec && !specsOf(args.value(QLatin1String("specs")), &specs, error)) return false;
    if (check) return true;
    // A new block's first equation is only a placeholder (y=1): replaced.
    if (eq) setEquations(c, equations, fresh || args.value(QLatin1String("replace_equations")).toBool());
    if (rec || spec) {
        if (c->Model == QLatin1String(qucs_s::ngstats::kMonteCarloModel)) {
            qucs_s::ngstats::MonteCarlo m = qucs_s::ngstats::MonteCarlo::read(c);
            if (rec) m.records = records;
            if (spec) m.specs = specs;
            m.write(c);
        } else {
            qucs_s::ngstats::Corners m = qucs_s::ngstats::Corners::read(c);
            if (rec) m.records = records;
            if (spec) m.specs = specs;
            m.write(c);
        }
    }
    return true;
}

// What of a part's text is shown, and where it is: 'shown' ({"R": true,
// "Temp": false}), 'name_shown', 'text_at' ([dx, dy] from its centre -
// the top left corner of its text). False and why in \a error, nothing
// changed, when one does not do; only checked unless \a apply.
bool setTextOf(Component* c, const QJsonObject& args, QString* error, bool apply = true)
{
    const QJsonObject shown = args.value(QLatin1String("shown")).toObject();
    if (args.contains(QLatin1String("shown")) && !args.value(QLatin1String("shown")).isObject()) {
        *error = tr("'shown' is {\"property\": true or false, ...}.");
        return false;
    }
    QStringList unknown;
    for (auto it = shown.begin(); it != shown.end(); ++it) {
        if (c->getProperty(it.key()) == nullptr) unknown << it.key();
        else if (!it.value().isBool()) {
            *error = tr("'shown' takes true or false for each property (%1).").arg(it.key());
            return false;
        }
    }
    if (!unknown.isEmpty()) {
        *error = tr("%1 has no property %2; its properties are: %3.")
                     .arg(c->Name.isEmpty() ? c->Model : c->Name, unknown.join(QStringLiteral(", ")), propertyNames(c));
        return false;
    }
    if (args.contains(QLatin1String("name_shown")) && !args.value(QLatin1String("name_shown")).isBool()) {
        *error = tr("'name_shown' is true or false.");
        return false;
    }
    QPoint at(c->tx, c->ty);
    // ("auto": the caller finds it a free spot, the schematic around it.)
    if (args.contains(QLatin1String("text_at")) && args.value(QLatin1String("text_at")).toString() != QLatin1String("auto")) {
        const QJsonArray a = args.value(QLatin1String("text_at")).toArray();
        if (a.size() != 2 || !a.at(0).isDouble() || !a.at(1).isDouble() || std::abs(a.at(0).toDouble()) > 100000
            || std::abs(a.at(1).toDouble()) > 100000) {
            *error = tr("'text_at' is [dx, dy]: where its text begins (the top left corner) from its centre - or \"auto\": a free spot "
                        "beside it.");
            return false;
        }
        at = QPoint(a.at(0).toInt(), a.at(1).toInt());
    }
    if (!apply) return true;
    for (auto it = shown.begin(); it != shown.end(); ++it) c->getProperty(it.key())->display = it.value().toBool();
    if (args.contains(QLatin1String("name_shown"))) c->showName = args.value(QLatin1String("name_shown")).toBool();
    c->tx = at.x();
    c->ty = at.y();
    return true;
}

// \a c's text put clear of what is around it - the wires, the symbols, the
// other texts and labels, the diagrams - at the nearest free spot beside
// it (the right, the left, below, above), unless it is clear where it is
// and \a keepIfClear: what was done, for the answer ("" when it was clear).
QString placeTextClear(Schematic* sch, Component* c, bool keepIfClear)
{
    namespace tp = qucs_s::textplace;
    if (tp::textBoxes(c).isEmpty()) return keepIfClear ? QString() : tr("it shows no text to place");
    const QList<tp::Thing> all = tp::things(sch);
    const QStringList over = tp::overlapped(c, all);
    if (over.isEmpty() && keepIfClear) return {};
    QString side;
    const std::optional<QPoint> spot = tp::freeSpot(c, all, &side);
    if (!spot) {
        return over.isEmpty() ? tr("its text is left where it is: no other spot beside it is free")
                              : tr("its text overlaps %1, and no spot beside it is free: move a part, or give text_at")
                                    .arg(over.mid(0, 3).join(QStringLiteral(", ")));
    }
    if (QPoint(c->tx, c->ty) == *spot) return over.isEmpty() ? tr("its text is clear where it is") : QString();
    c->tx = spot->x();
    c->ty = spot->y();
    return over.isEmpty() ? tr("its text put %1 of it, [%2, %3]").arg(side).arg(spot->x()).arg(spot->y())
                          : tr("its text put %1 of it, [%2, %3], clear of %4").arg(side).arg(spot->x()).arg(spot->y())
                                .arg(over.mid(0, 3).join(QStringLiteral(", ")));
}

QString kindOf(QucsDoc* doc)
{
    if (qobject_cast<TextDoc*>(QucsApp::documentWidget(doc)) != nullptr) return QStringLiteral("text");
    if (QucsApp::isPdfDocument(QucsApp::documentWidget(doc))) return QStringLiteral("pdf");
    if (QucsApp::isSheetDocument(QucsApp::documentWidget(doc))) return QStringLiteral("spreadsheet");
    if (QucsApp::isArchiveDocument(QucsApp::documentWidget(doc))) return QStringLiteral("archive");
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

} // namespace

namespace qucs_s::control {

QList<Component*> unnamedOf(const Schematic* sch, const QString& model)
{
    QList<Component*> found;
    for (Component* c : sch->a_DocComps)
        if (c->Model.compare(model, Qt::CaseInsensitive) == 0 && (c->Name.isEmpty() || c->Name == QLatin1String("*"))) found << c;
    return found;
}

QString refOf(const Schematic* sch, const Component* c)
{
    if (!c->Name.isEmpty() && c->Name != QLatin1String("*")) {
        // A name given twice (a hand-edited file): the second is R1#2, so
        // that each can be told - a selection of both moved the first alone.
        int k = 0;
        for (const Component* o : sch->a_DocComps) {
            if (o->Name == c->Name) ++k;
            if (o == c) break;
        }
        return k > 1 ? QStringLiteral("%1#%2").arg(c->Name).arg(k) : c->Name;
    }
    const QList<Component*> same = unnamedOf(sch, c->Model);
    return same.size() > 1 ? QStringLiteral("%1#%2").arg(c->Model).arg(same.indexOf(c) + 1) : c->Model;
}

} // namespace qucs_s::control

namespace {

// A part by what refOf() tells it by. Null, and why in \a error.
Component* componentOf(const Schematic* sch, const QString& ref, QString* error)
{
    const QString name = ref.trimmed();
    if (!name.isEmpty())
        if (Component* c = sch->getComponentByName(name)) return c;
    QString model = name;
    int nth = 0;
    if (const qsizetype hash = name.indexOf(QLatin1Char('#')); hash > 0) {
        // (Of a name given twice: R1#2 the second, R1 or R1#1 the first.)
        QList<Component*> named;
        for (Component* c : sch->a_DocComps)
            if (c->Name == name.left(hash)) named << c;
        if (named.size() > 1) {
            bool number = false;
            const int k = name.mid(hash + 1).toInt(&number);
            if (number && k >= 1 && k <= named.size()) return named.at(k - 1);
            *error = tr("There are %1 parts named %2, not %3: %2 (or %2#1) to %2#%1.").arg(named.size()).arg(name.left(hash), name);
            return nullptr;
        }
        model = name.left(hash);
        nth = std::max(name.mid(hash + 1).toInt(), -1);
        if (nth == 0) nth = -1;
    }
    const QList<Component*> same = model.isEmpty() ? QList<Component*>() : unnamedOf(sch, model);
    if (!same.isEmpty()) model = same.first()->Model;   // (as it is written: gnd#2 is GND#2)
    if (nth > 0 && nth <= same.size()) return same.at(nth - 1);
    if (nth == 0 && same.size() == 1) return same.first();
    if (nth != 0 && same.size() == 1)
        *error = tr("There is one %1, told by %1 alone: not %2.").arg(model, name);
    else if (nth != 0 && !same.isEmpty())
        *error = tr("There are %1 of %2, not %3: %2#1 to %2#%1.").arg(same.size()).arg(model, name);
    else if (same.size() > 1)
        *error = tr("There are %1 of %2: say which, %2#1 to %2#%1 (get_schematic gives each its 'ref').").arg(same.size()).arg(model);
    else
        *error = tr("There is no component %1.").arg(name);
    return nullptr;
}

// Whether what \a ref names before its # is that of several parts - an
// unnamed type's (GND#9 of 3) or a name given twice (R1#3) - so that why it
// was not found says more than "no such part".
bool toldByNumber(const Schematic* sch, const QString& ref)
{
    const QString base = ref.trimmed().section(QLatin1Char('#'), 0, 0);
    if (!unnamedOf(sch, base).isEmpty()) return true;
    int named = 0;
    for (const Component* c : sch->a_DocComps)
        if (c->Name == base && ++named > 1) return true;
    return false;
}

// The parts of a schematic by what refOf() tells them by, all at once: a
// selection of 15,000 parts each looked for among all of them (and each
// checked against those found) took seconds, a list squared.
class PartIndex
{
public:
    explicit PartIndex(const Schematic* sch) : m_sch(sch)
    {
        for (Component* c : sch->a_DocComps) {
            if (!c->Name.isEmpty() && c->Name != QLatin1String("*")) {
                if (!m_byName.contains(c->Name)) m_byName.insert(c->Name, c);   // (the first, as getComponentByName)
                m_named[c->Name] << c;
            } else {
                m_unnamed[c->Model.toLower()] << c;
            }
        }
    }
    /// The part \a ref tells: its name, or an unnamed one's type ("GND",
    /// "GND#17"); null, and why in \a error (componentOf's words).
    Component* find(const QString& ref, QString* error) const
    {
        const QString name = ref.trimmed();
        if (Component* c = m_byName.value(name)) return c;
        QString model = name;
        int nth = 0;
        if (const qsizetype hash = name.indexOf(QLatin1Char('#')); hash > 0) {
            model = name.left(hash);
            nth = name.mid(hash + 1).toInt();
            // (Of a name given twice: R1#2.)
            const QList<Component*> named = m_named.value(model);
            if (named.size() > 1 && nth >= 1 && nth <= named.size()) return named.at(nth - 1);
        }
        const QList<Component*> same = m_unnamed.value(model.toLower());
        if (nth > 0 && nth <= same.size()) return same.at(nth - 1);
        if (nth == 0 && !name.contains(QLatin1Char('#')) && same.size() == 1) return same.first();
        return componentOf(m_sch, ref, error);
    }
    /// Each part's ref, as refOf() gives it, all in one pass (as Check
    /// Schematic's findings name them).
    static QHash<const Component*, QString> refs(const Schematic* sch) { return qucs_s::erc::refs(sch); }

private:
    const Schematic* m_sch;
    QHash<QString, Component*> m_byName;
    QHash<QString, QList<Component*>> m_named;
    QHash<QString, QList<Component*>> m_unnamed;
};

// Messages said once each, with how often ("There is no component R9. (×252)"),
// at most \a most of them and how many more: an answer of a 15,000-part
// selection ran to 100 kB of one sentence.
QStringList onceEach(const QStringList& messages, int most = 20)
{
    QStringList order;
    QHash<QString, int> count;
    for (const QString& m : messages)
        if (count[m]++ == 0) order << m;
    QStringList out;
    for (const QString& m : order) {
        if (out.size() == most) {
            out << QCoreApplication::translate("QucsControl", "and %1 more").arg(order.size() - most);
            break;
        }
        const int n = count.value(m);
        out << (n > 1 ? QStringLiteral("%1 (\u00D7%2)").arg(m).arg(n) : m);
    }
    return out;
}

// "Not found: ..." of what was not found, each once - one period at its
// end, though the last tells why in a sentence ("... GND#1 to GND#3." gave
// two).
QString notFound(const QStringList& missing)
{
    QString list = onceEach(missing).join(QStringLiteral(", "));
    if (list.endsWith(QLatin1Char('.'))) list.chop(1);
    return QCoreApplication::translate("QucsControl", "Not found: %1.").arg(list);
}

// At most \a most of a list, and how many more.
QStringList atMost(const QStringList& list, int most = 20)
{
    if (list.size() <= most) return list;
    QStringList out = list.mid(0, most);
    out << QCoreApplication::translate("QucsControl", "and %1 more").arg(list.size() - most);
    return out;
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
            // (A ground left out of the simulation grounds nothing: its net
            // was "gnd" here while the netlist had _net1.)
            if (c->Model == QLatin1String("GND")) {
                if (c->isActive == COMP_IS_ACTIVE) join(id, nameId(QStringLiteral("ground")));
            } else if (c == edited) (n->conn_count() == 1 && !n->hasLabel() ? nets.open : nets.touching) << key;
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

// Where \a c's centre goes to be on a side of another part, as \a near
// says - {"part": "U1", "side": "below", "gap": 40}: that far from its
// symbol (the symbols' boxes, not their texts), centred on it across that
// side, on the grid - or slid along that side, a grid step at a time,
// to the nearest place where its symbol meets no other part's and none
// of its pins comes down on a wire or another pin. As it is turned now.
// \a how says where it went. False, and why, when it does not read.
bool placeNear(Schematic* sch, const Component* c, const QJsonValue& near, QPoint* at, QString* error, QString* how = nullptr)
{
    const QJsonObject o = near.toObject();
    const QString ref = o.value(QLatin1String("part")).toString().trimmed();
    const QString side = o.value(QLatin1String("side")).toString(QStringLiteral("right")).trimmed().toLower();
    static const QStringList sides{QStringLiteral("above"), QStringLiteral("below"), QStringLiteral("left"), QStringLiteral("right")};
    if (!near.isObject() || ref.isEmpty() || !sides.contains(side)) {
        *error = QObject::tr("'near' is {\"part\": \"U1\", \"side\": \"below\", \"gap\": 40}: a part, the side of it (above, below, "
                             "left or right) and the room between their symbols (40 unless given).");
        return false;
    }
    const Component* other = componentOf(sch, ref, error);
    if (other == nullptr) return false;
    if (other == c) {
        *error = QObject::tr("'near' names the part itself.");
        return false;
    }
    const int gap = std::clamp(o.value(QLatin1String("gap")).toInt(40), 0, 2000);
    const QRect them = other->boundingRect();
    const QRect mine = c->boundingRect().translated(-c->center());
    int x = them.center().x() - mine.center().x(), y = them.center().y() - mine.center().y();
    if (side == QLatin1String("below")) y = them.bottom() + gap - mine.top();
    else if (side == QLatin1String("above")) y = them.top() - gap - mine.bottom();
    else if (side == QLatin1String("left")) x = them.left() - gap - mine.right();
    else x = them.right() + gap - mine.left();
    const QPoint within = Schematic::withinModelLimit(QPoint(x, y));
    x = within.x();
    y = within.y();
    sch->setOnGrid(x, y);
    // Clear of the others there? (Its own wires, when it is moved, go with it.)
    QSet<const Node*> own;
    for (const Port* p : c->Ports)
        if (p->Connection != nullptr) own.insert(p->Connection);
    const auto clearAt = [&](const QPoint& centre) {
        const QRect box = mine.translated(centre);
        for (const Component* o : sch->a_DocComps)
            if (o != c && !o->Ports.isEmpty() && box.intersects(o->boundingRect())) return false;
        for (const Port* p : c->Ports) {
            const QPoint pin = centre + QPoint(p->x, p->y);
            for (const Node* n : sch->a_DocNodes)
                if (!own.contains(n) && n->center() == pin) return false;
            for (const Wire* w : sch->a_DocWires)
                if (!own.contains(w->Port1) && !own.contains(w->Port2) && onSegment(pin, w->P1(), w->P2())) return false;
        }
        return true;
    };
    const bool across = side == QLatin1String("below") || side == QLatin1String("above");
    const int step = across ? std::max(sch->getGridX(), 1) : std::max(sch->getGridY(), 1);
    int slid = 0;
    if (!clearAt(QPoint(x, y)))
        for (int k = 1; k <= 30 && slid == 0; ++k)
            for (int d : {k, -k})
                if (slid == 0 && clearAt(QPoint(x + (across ? d * step : 0), y + (across ? 0 : d * step)))) slid = d * step;
    if (slid != 0) {
        if (across) x += slid;
        else y += slid;
    }
    *at = QPoint(x, y);
    if (how != nullptr) {
        *how = QObject::tr("%1 %2, %3 from its symbol").arg(side, ref).arg(gap);
        *how += slid == 0 ? QObject::tr(", centred on it")
                          : QObject::tr(", slid %1 %2 of its centre: there its symbol met another part's, or a pin came down on a wire or pin")
                                .arg(std::abs(slid))
                                .arg(across ? (slid > 0 ? QObject::tr("right") : QObject::tr("left")) : (slid > 0 ? QObject::tr("down") : QObject::tr("up")));
    }
    return true;
}

// The way out of a part's symbol from \a p inside its box \a box: toward
// the nearest side, as the pin's stub goes (an op-amp's VEE: down).
QPoint outwardOf(const QRect& box, const QPoint& p)
{
    const int left = p.x() - box.left(), right = box.right() - p.x(), up = p.y() - box.top(), down = box.bottom() - p.y();
    const int least = std::min({left, right, up, down});
    return least == left ? QPoint(-1, 0) : least == right ? QPoint(1, 0) : least == up ? QPoint(0, -1) : QPoint(0, 1);
}

} // namespace

namespace qucs_s::control {

QString pinSide(const Component* c, int i)
{
    const QRect box = c->boundingRect().translated(-c->center());
    const QPoint at(c->Ports.at(i)->x, c->Ports.at(i)->y);
    const QPoint d = outwardOf(box, at);
    QString side = d.x() < 0 ? tr("left") : d.x() > 0 ? tr("right") : d.y() < 0 ? tr("top") : tr("bottom");
    if (d.x() != 0 && at.y() != 0) side += at.y() < 0 ? tr(", upper") : tr(", lower");
    if (d.y() != 0 && at.x() != 0) side += at.x() < 0 ? tr(", left") : tr(", right");
    return side;
}

QString pinSides(const Component* c)
{
    QStringList said;
    for (int i = 0; i < c->Ports.size(); ++i)
        said << (c->Ports.at(i)->Name.isEmpty() ? QStringLiteral("%1 (%2)").arg(i + 1).arg(pinSide(c, i))
                                                : QStringLiteral("%1 %2 (%3)").arg(i + 1).arg(c->Ports.at(i)->Name, pinSide(c, i)));
    return said.join(QStringLiteral(", "));
}

} // namespace qucs_s::control

namespace {

// Whether the piece \a p to \a q of a way from \a a to \a b may cross the
// part's box \a box: it leaves \a a (or comes to \b), a pin of that part,
// straight out of the box. A pin sits on its symbol's box or in it - a
// library part's box is four points wider, an op-amp's supply pins are in
// its box - so no way from it was clear of every symbol, and the wire went
// over the parts (Rf's up through the op-amp).
bool leavesItsBox(const QRect& box, const QPoint& a, const QPoint& b, const QPoint& p, const QPoint& q)
{
    const auto sign = [](int v) { return (v > 0) - (v < 0); };
    for (const auto& [end, other] : {std::pair(a, p == a ? q : QPoint()), std::pair(b, q == b ? p : QPoint())}) {
        if ((end == a ? p != a : q != b) || !box.contains(end) || other == end) continue;
        if (QPoint(sign(other.x() - end.x()), sign(other.y() - end.y())) == outwardOf(box, end)) return true;
    }
    return false;
}

// Whether a wire along \a way (from \a way's first place to its last)
// would touch nothing of another net than \a ours (nets of \a now) on its
// way: no node (a pin, a wire's end) on it but at its two ends, none of
// its bends on a wire - and, \a strict, no part's symbol crossed (but a
// pin's own, straight out of it) and every piece straight across or up.
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
            for (const Component* c : sch->a_DocComps) {
                const QRect box = c->boundingRect().adjusted(1, 1, -1, -1);
                if (!c->Ports.isEmpty() && piece.intersects(box) && !leavesItsBox(box, a, b, p, q)) return false;
            }
        }
    }
    for (std::size_t k = 1; k + 1 < way.size(); ++k)
        for (const Wire* w : sch->a_DocWires)
            if (!ours.contains(now.nodeNet.value(w->Port1, -1)) && onSegment(way[k], w->P1(), w->P2())) return false;
    return true;
}

// clearWay() over an index of the schematic by place: what a way passes is
// looked for in the cells it crosses, not among every node, part and wire
// (each wire of a large arrange scanned them all). What joinPieces() draws
// meanwhile goes in too, as of no net - which a way may not touch.
class WayIndex
{
public:
    WayIndex(Schematic* sch, const Nets& now)
    {
        for (const Node* n : sch->a_DocNodes) addNode(n->center(), now.nodeNet.value(n, -1));
        for (const Component* c : sch->a_DocComps)
            if (!c->Ports.isEmpty()) {
                addTo(m_parts, m_wideParts, c->boundingRect().adjusted(1, 1, -1, -1), c->boundingRect().adjusted(1, 1, -1, -1));
                const QRect texts = c->boundingRectIncludingProperties();
                addTo(m_texts, m_wideTexts, texts, texts);
            }
        for (const Wire* w : sch->a_DocWires) addWire(w->P1(), w->P2(), now.nodeNet.value(w->Port1, -1));
    }
    void addNode(const QPoint& p, int net) { m_nodes[cellOf(p)].append({p, net}); }
    void addWire(const QPoint& a, const QPoint& b, int net)
    {
        // (A little around it: a slanting wire's points are near it, not on it.)
        addTo(m_wires, m_wideWires, QRect(a, b).normalized().adjusted(-2, -2, 2, 2), Segment{a, b, net});
    }
    // (\a ownSymbols: a way's ends may leave their own symbols straight out
    // - not a step of mazeWay(), whose ends are no pins. \a texts: nor over
    // the texts of a part, but its own at the way's ends.)
    bool clear(const std::vector<QPoint>& way, bool strict, const QSet<int>& ours, bool ownSymbols = true, bool texts = false) const
    {
        const QPoint a = way.front(), b = way.back();
        for (std::size_t k = 1; k < way.size(); ++k) {
            const QPoint p = way[k - 1], q = way[k];
            if (p == q) continue;
            if (strict && p.x() != q.x() && p.y() != q.y()) return false;
            const QRect piece = QRect(p, q).normalized();
            const bool slanting = p.x() != q.x() && p.y() != q.y();
            const auto nodeInTheWay = [&](const std::pair<QPoint, int>& n) {
                return n.first != a && n.first != b && !ours.contains(n.second) && onSegment(n.first, p, q);
            };
            if (slanting) {   // (a slanting way: its tolerance - every node)
                for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it)
                    if (std::any_of(it.value().cbegin(), it.value().cend(), nodeInTheWay)) return false;
            } else {
                for (const QPoint& cell : cellsOf(piece))
                    if (const auto it = m_nodes.constFind(cell); it != m_nodes.cend())
                        if (std::any_of(it.value().cbegin(), it.value().cend(), nodeInTheWay)) return false;
            }
            if (texts) {
                // (A piece from an end: over its own part's texts, as it leaves the pin.)
                const auto over = [&](const QRect& r) { return piece.intersects(r) && !(p == a && r.contains(a)) && !(q == b && r.contains(b)); };
                if (std::any_of(m_wideTexts.cbegin(), m_wideTexts.cend(), over)) return false;
                for (const QPoint& cell : cellsOf(piece))
                    if (const auto it = m_texts.constFind(cell); it != m_texts.cend())
                        if (std::any_of(it.value().cbegin(), it.value().cend(), over)) return false;
            }
            if (strict) {
                const auto crosses = [&](const QRect& r) { return piece.intersects(r) && !(ownSymbols && leavesItsBox(r, a, b, p, q)); };
                if (std::any_of(m_wideParts.cbegin(), m_wideParts.cend(), crosses)) return false;
                for (const QPoint& cell : cellsOf(piece))
                    if (const auto it = m_parts.constFind(cell); it != m_parts.cend())
                        if (std::any_of(it.value().cbegin(), it.value().cend(), crosses)) return false;
            }
        }
        for (std::size_t k = 1; k + 1 < way.size(); ++k) {
            const QPoint bend = way[k];
            const auto under = [&](const Segment& s) { return !ours.contains(s.net) && onSegment(bend, s.a, s.b); };
            if (std::any_of(m_wideWires.cbegin(), m_wideWires.cend(), under)) return false;
            if (const auto it = m_wires.constFind(cellOf(bend)); it != m_wires.cend())
                if (std::any_of(it.value().cbegin(), it.value().cend(), under)) return false;
        }
        return true;
    }

    // What a way one grid step at a time (mazeWay()) asks: a node of
    // another net at \a p; another net's wire through \a p (a bend there
    // would join it); another net's wire along \a p to \a q (over it); the
    // parts' boxes \a p is in.
    bool nodeOfOthersAt(const QPoint& p, const QSet<int>& ours) const
    {
        const auto it = m_nodes.constFind(cellOf(p));
        return it != m_nodes.cend()
               && std::any_of(it.value().cbegin(), it.value().cend(), [&](const std::pair<QPoint, int>& n) { return n.first == p && !ours.contains(n.second); });
    }
    bool wireOfOthersAt(const QPoint& p, const QSet<int>& ours) const
    {
        const auto on = [&](const Segment& s) { return !ours.contains(s.net) && onSegment(p, s.a, s.b); };
        if (std::any_of(m_wideWires.cbegin(), m_wideWires.cend(), on)) return true;
        const auto it = m_wires.constFind(cellOf(p));
        return it != m_wires.cend() && std::any_of(it.value().cbegin(), it.value().cend(), on);
    }
    bool alongWireOfOthers(const QPoint& p, const QPoint& q, const QSet<int>& ours) const
    {
        const auto along = [&](const Segment& s) { return !ours.contains(s.net) && onSegment(p, s.a, s.b) && onSegment(q, s.a, s.b); };
        if (std::any_of(m_wideWires.cbegin(), m_wideWires.cend(), along)) return true;
        const auto it = m_wires.constFind(cellOf(p));
        return it != m_wires.cend() && std::any_of(it.value().cbegin(), it.value().cend(), along);
    }
    // Whether \a p to \a q goes over a part's texts (its name, its values):
    // a way round them is better (mazeWay() weighs it).
    bool overTexts(const QPoint& p, const QPoint& q) const
    {
        const QRect piece = QRect(p, q).normalized();
        const auto over = [&](const QRect& r) { return piece.intersects(r); };
        if (std::any_of(m_wideTexts.cbegin(), m_wideTexts.cend(), over)) return true;
        for (const QPoint& cell : {cellOf(p), cellOf(q)})
            if (const auto it = m_texts.constFind(cell); it != m_texts.cend() && std::any_of(it.value().cbegin(), it.value().cend(), over))
                return true;
        return false;
    }
    QList<QRect> boxesAt(const QPoint& p) const
    {
        QList<QRect> found;
        for (const QRect& r : m_wideParts)
            if (r.contains(p)) found << r;
        if (const auto it = m_parts.constFind(cellOf(p)); it != m_parts.cend())
            for (const QRect& r : it.value())
                if (r.contains(p) && !found.contains(r)) found << r;
        return found;
    }

private:
    struct Segment {
        QPoint a, b;
        int net;
    };
    static constexpr int kShift = 6;        // cells of 64 x 64
    static constexpr qint64 kMostCells = 64;   // wider: checked every time
    static QPoint cellOf(const QPoint& p) { return {p.x() >> kShift, p.y() >> kShift}; }
    static QList<QPoint> cellsOf(const QRect& r)
    {
        QList<QPoint> cells;
        const QPoint from = cellOf(r.topLeft()), to = cellOf(r.bottomRight());
        for (int x = from.x(); x <= to.x(); ++x)
            for (int y = from.y(); y <= to.y(); ++y) cells << QPoint(x, y);
        return cells;
    }
    template <typename T>
    static void addTo(QHash<QPoint, QList<T>>& cells, QList<T>& wide, const QRect& r, const T& item)
    {
        const QPoint from = cellOf(r.topLeft()), to = cellOf(r.bottomRight());
        if ((qint64(to.x()) - from.x() + 1) * (qint64(to.y()) - from.y() + 1) > kMostCells) {
            wide << item;
            return;
        }
        for (int x = from.x(); x <= to.x(); ++x)
            for (int y = from.y(); y <= to.y(); ++y) cells[QPoint(x, y)] << item;
    }
    QHash<QPoint, QList<std::pair<QPoint, int>>> m_nodes;
    QHash<QPoint, QList<QRect>> m_parts, m_texts;
    QList<QRect> m_wideParts, m_wideTexts;
    QHash<QPoint, QList<Segment>> m_wires;
    QList<Segment> m_wideWires;
};

// A way from \a a to \a b a grid step at a time (A*), when none of the few
// shapes waysBetween() tries is clear of every symbol: out of each end's
// own symbol straight, then round every part's box, through no pin or wire
// end of another net, along none of its wires and turning on none - the
// fewest steps, a bend weighing three and a crossing two. Empty when there
// is none within \a reach steps of the two ends' box (or too many tried).
// A step over a part's texts weighs two more: round them where there is room.
std::vector<QPoint> mazeWay(const WayIndex& index, const QPoint& a, const QPoint& b, const QSet<int>& ours, int gx, int gy, int reach = 30)
{
    // Out of each end's own symbol first, straight, as its stub goes.
    const auto escape = [&](const QPoint& end) {
        QPoint at = end;
        const QList<QRect> boxes = index.boxesAt(end);
        if (boxes.isEmpty()) return at;
        const QPoint d = outwardOf(boxes.first(), end);
        for (int k = 0; k < 20 && !index.boxesAt(at).isEmpty(); ++k) at += QPoint(d.x() * gx, d.y() * gy);
        return at;
    };
    const QPoint from = escape(a), to = escape(b);
    const auto outOk = [&](const QPoint& end, const QPoint& out) {
        return end == out || (index.clear({end, out}, true, ours) && (out == a || out == b || !index.nodeOfOthersAt(out, ours)));
    };
    if (!outOk(a, from) || !outOk(b, to)) return {};
    if (from == to) return {a, from, b};
    const QRect area = QRect(from, to).normalized().adjusted(-reach * gx, -reach * gy, reach * gx, reach * gy);
    const QPoint steps[4] = {QPoint(gx, 0), QPoint(-gx, 0), QPoint(0, gy), QPoint(0, -gy)};
    struct State {
        int cost, guess, dir;
        QPoint at;
        bool operator>(const State& o) const { return cost + guess > o.cost + o.guess; }
    };
    const auto key = [](const QPoint& p, int dir) { return std::tuple(p.x(), p.y(), dir); };
    std::priority_queue<State, std::vector<State>, std::greater<State>> open;
    std::map<std::tuple<int, int, int>, int> best;
    std::map<std::tuple<int, int, int>, std::pair<QPoint, int>> cameFrom;
    const auto guessOf = [&](const QPoint& p) { return std::abs(p.x() - to.x()) / gx + std::abs(p.y() - to.y()) / gy; };
    open.push({0, guessOf(from), -1, from});
    best[key(from, -1)] = 0;
    int tried = 0, endDir = -2;
    while (!open.empty() && ++tried < 60000) {
        const State s = open.top();
        open.pop();
        if (s.cost > best[key(s.at, s.dir)]) continue;
        if (s.at == to) {
            endDir = s.dir;
            break;
        }
        // (On another net's wire - a crossing - it goes straight on.)
        const bool onWire = s.at != from && index.wireOfOthersAt(s.at, ours);
        for (int d = 0; d < 4; ++d) {
            if (s.dir >= 0 && (d ^ 1) == s.dir && d / 2 == s.dir / 2) continue;   // (not back)
            if (onWire && s.dir >= 0 && d != s.dir) continue;
            const QPoint q = s.at + steps[d];
            if (!area.contains(q)) continue;
            if (q != to && q != a && q != b && index.nodeOfOthersAt(q, ours)) continue;
            if (index.alongWireOfOthers(s.at, q, ours) || !index.clear({s.at, q}, true, ours, false)) continue;
            const bool crosses = q != to && index.wireOfOthersAt(q, ours);
            const int cost = s.cost + 1 + (s.dir >= 0 && d != s.dir ? 3 : 0) + (crosses ? 2 : 0) + (index.overTexts(s.at, q) ? 2 : 0);
            const auto k = key(q, d);
            if (const auto it = best.find(k); it != best.end() && it->second <= cost) continue;
            best[k] = cost;
            cameFrom[k] = {s.at, s.dir};
            open.push({cost, guessOf(q), d, q});
        }
    }
    if (endDir == -2) return {};
    // Back from the end, the bends kept.
    std::vector<QPoint> path{to};
    for (auto k = key(to, endDir); cameFrom.count(k);) {
        const auto [p, dir] = cameFrom.at(k);
        path.push_back(p);
        k = key(p, dir);
    }
    std::reverse(path.begin(), path.end());
    std::vector<QPoint> way{a};
    for (const QPoint& p : path) {
        if (way.size() >= 2) {
            const QPoint u = way[way.size() - 2], v = way.back();
            if ((u.x() == v.x() && v.x() == p.x()) || (u.y() == v.y() && v.y() == p.y())) way.back() = p;   // (straight on)
            else way.push_back(p);
        } else if (p != way.back()) {
            way.push_back(p);
        }
    }
    if (way.back() != b) {
        const QPoint u = way.size() >= 2 ? way[way.size() - 2] : way.back(), v = way.back();
        if (way.size() >= 2 && ((u.x() == v.x() && v.x() == b.x()) || (u.y() == v.y() && v.y() == b.y()))) way.back() = b;
        else way.push_back(b);
    }
    return index.clear(way, true, ours) ? way : std::vector<QPoint>{};
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
// As wireUp(), along the first of \a ways that touches nothing else and
// that \a check finds nothing wrong with; \a used says which (its index).
// \a maze: a way round the parts a step at a time when none of \a ways is
// clear of them (not when the ways are the caller's own: 'via'); and
// then, before it, a way of \a ways clear of the parts' texts too (not
// when their order says something: 'side').
bool wireAlong(Schematic* sch, const QPoint& a, const QPoint& b, const std::vector<std::vector<QPoint>>& ways,
               const std::function<QStringList()>& check, QString* why, int* used = nullptr, bool maze = true, bool tidy = false)
{
    const QString state = sch->snapshot();
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
    // Clear of every symbol: one of the ways given, else a way round them a
    // step at a time (mazeWay(), after them: \a used counts only the ways
    // given); over them only when neither will do.
    std::vector<std::vector<QPoint>> all(ways);
    std::vector<int> index(ways.size());   // (each way's place in \a ways; the maze's after them)
    std::iota(index.begin(), index.end(), 0);
    if (maze || tidy) {
        const WayIndex place(sch, now);
        const auto tidyOne = std::find_if(all.cbegin(), all.cend(), [&](const std::vector<QPoint>& w) { return place.clear(w, true, ours, true, true); });
        if (tidy && tidyOne != all.cend()) {
            // (Tried first: the others after it as they were.)
            const int at = int(tidyOne - all.cbegin());
            std::rotate(all.begin(), all.begin() + at, all.begin() + at + 1);
            std::rotate(index.begin(), index.begin() + at, index.begin() + at + 1);
        } else if (maze && (tidy || std::none_of(all.cbegin(), all.cend(), [&](const std::vector<QPoint>& w) { return place.clear(w, true, ours); }))) {
            if (std::vector<QPoint> m = mazeWay(place, a, b, ours, std::max(sch->getGridX(), 1), std::max(sch->getGridY(), 1)); !m.empty()) {
                // (Before the ways that cross texts when tidy; after all of them else.)
                if (tidy) {
                    all.insert(all.begin(), std::move(m));
                    index.insert(index.begin(), int(ways.size()));
                } else {
                    all.push_back(std::move(m));
                    index.push_back(int(ways.size()));
                }
            }
        }
    }
    for (bool strict : {true, false})
        for (std::size_t w = 0; w < all.size(); ++w) {
            const std::vector<QPoint>& way = all[w];
            if (!clearWay(sch, way, strict, now, ours) || (!strict && clearWay(sch, way, true, now, ours))) continue;
            for (std::size_t k = 1; k < way.size(); ++k)
                if (way[k] != way[k - 1])
                    sch->connectWithWire(way[k - 1], way[k], true, qucs_s::wire::Planner::PlanType::Straight);
            faults = check();
            if (faults.isEmpty()) {
                if (used != nullptr) *used = index[w];
                return true;
            }
            sch->restore(state);
            look();   // (new nodes)
            if (++tries == 8) break;
        }
    *why = !faults.isEmpty() ? faults.join(QStringLiteral("; "))
                             : tr("every way from %1, %2 to %3, %4 goes over another pin or wire")
                                   .arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
    return false;
}

bool wireUp(Schematic* sch, const QPoint& a, const QPoint& b, const std::function<QStringList()>& check, QString* why)
{
    return wireAlong(sch, a, b, waysBetween(sch, a, b), check, why, nullptr, true, true);
}

// The ways from \a a to \a b around a side first - "below": along a line
// under both ends, each grid step further down, then the others.
// \a preferred says how many of them go round that side.
std::vector<std::vector<QPoint>> waysAround(Schematic* sch, const QPoint& a, const QPoint& b, const QString& side, int* preferred)
{
    std::vector<std::vector<QPoint>> ways;
    const int gx = std::max(sch->getGridX(), 1), gy = std::max(sch->getGridY(), 1);
    for (int k = 1; k <= 40; ++k) {
        if (side == QLatin1String("above")) {
            const int y = std::min(a.y(), b.y()) - k * gy;
            ways.push_back({a, {a.x(), y}, {b.x(), y}, b});
        } else if (side == QLatin1String("below")) {
            const int y = std::max(a.y(), b.y()) + k * gy;
            ways.push_back({a, {a.x(), y}, {b.x(), y}, b});
        } else if (side == QLatin1String("left")) {
            const int x = std::min(a.x(), b.x()) - k * gx;
            ways.push_back({a, {x, a.y()}, {x, b.y()}, b});
        } else {
            const int x = std::max(a.x(), b.x()) + k * gx;
            ways.push_back({a, {x, a.y()}, {x, b.y()}, b});
        }
    }
    *preferred = int(ways.size());
    for (std::vector<QPoint>& w : waysBetween(sch, a, b)) ways.push_back(std::move(w));
    return ways;
}

// A way from \a a through \a via to \a b: a right angle, across first,
// where a step is not straight.
std::vector<QPoint> wayThrough(const QPoint& a, const QList<QPoint>& via, const QPoint& b)
{
    std::vector<QPoint> way{a};
    QList<QPoint> points = via;
    points << b;
    for (const QPoint& q : std::as_const(points)) {
        const QPoint p = way.back();
        if (p.x() != q.x() && p.y() != q.y()) way.push_back(QPoint(q.x(), p.y()));
        if (q != way.back()) way.push_back(q);
    }
    return way;
}

// Joins again the nets of \a before that are in pieces now, each piece
// to the closest of the others by a wire (wireUp()) that joins nothing
// else - \a check says what that would join. False, and why in \a why,
// when a piece cannot be reached. \a edited is the part "@" is, \a probes
// the places before has.
bool joinPieces(Schematic* sch, const Nets& before, const QString& edited, const QList<QPoint>& probes,
                const std::function<QStringList()>& check, QString* why, int rounds = 64)
{
    for (int round = 0; round < rounds; ++round) {
        // (No part edited: none - an empty name would find a ground.)
        const Nets now = netsOf(sch, edited.isEmpty() ? nullptr : sch->getComponentByName(edited), probes);
        // A net of before in pieces: the pieces' nodes.
        QHash<int, QSet<int>> pieces;   // net before -> nets now
        for (auto it = before.netOf.constBegin(); it != before.netOf.constEnd(); ++it)
            if (now.netOf.contains(it.key())) pieces[it.value()].insert(now.netOf.value(it.key()));
        QList<int> split;
        for (auto it = pieces.constBegin(); it != pieces.constEnd(); ++it)
            if (it.value().size() > 1) split << it.key();
        if (split.isEmpty()) return true;
        std::sort(split.begin(), split.end());
        // Many nets in pieces (arrange: all of them): each one's two closest
        // pieces wired at once, and the nets checked once - a netsOf() and
        // a check for every wire made arrange quadratic, a minute at 3,000
        // parts. Should the check find fault, one at a time after all.
        if (split.size() > 1) {
            const QString state = sch->snapshot();
            QHash<std::pair<int, int>, int> netAt;   // a node's place -> its net now
            for (const Node* n : sch->a_DocNodes) netAt.insert({n->cx, n->cy}, now.nodeNet.value(n, -1));
            for (const QPoint& p : probes)
                if (const int net = now.netOf.value(QStringLiteral("at %1,%2").arg(p.x()).arg(p.y()), -1); net >= 0) netAt.insert({p.x(), p.y()}, net);
            QHash<int, QList<QPoint>> placesOf;   // a piece -> its nodes' places
            for (auto it = netAt.cbegin(); it != netAt.cend(); ++it) placesOf[it.value()] << QPoint(it.key().first, it.key().second);
            // (In order: the closest of equals the same one every time - the
            // hashes' order, other in every run, laid a schematic out anew
            // each time.)
            for (QList<QPoint>& places : placesOf)
                std::sort(places.begin(), places.end(), [](const QPoint& a, const QPoint& b) { return std::pair(a.x(), a.y()) < std::pair(b.x(), b.y()); });
            bool all = true;
            WayIndex index(sch, now);
            // (Nodes and wires found by place as each wire goes in: every one
            // made the schematic look through all of them.)
            std::optional<Schematic::IndexedInsertion> indexed(std::in_place, sch);
            for (int net : std::as_const(split)) {
                QList<int> ids = QList<int>(pieces.value(net).cbegin(), pieces.value(net).cend());
                std::sort(ids.begin(), ids.end());
                QPoint from, to;
                qint64 best = -1;
                for (int i = 0; i < ids.size(); ++i)
                    for (int j = i + 1; j < ids.size(); ++j)
                        for (const QPoint& p : placesOf.value(ids.at(i)))
                            for (const QPoint& q : placesOf.value(ids.at(j))) {
                                const qint64 d = qAbs(qint64(p.x()) - q.x()) + qAbs(qint64(p.y()) - q.y());
                                if (best < 0 || d < best) {
                                    best = d;
                                    from = p;
                                    to = q;
                                }
                            }
                if (best < 0) {
                    all = false;
                    break;
                }
                const QSet<int> ours{netAt.value({from.x(), from.y()}, -1), netAt.value({to.x(), to.y()}, -1)};
                bool wired = false;
                const auto draw = [&](const std::vector<QPoint>& way) {
                    // (Merged once, after the lot: optimizeWires() scans every wire.)
                    for (std::size_t k = 1; k < way.size(); ++k)
                        if (way[k] != way[k - 1]) {
                            sch->connectWithWire(way[k - 1], way[k], false, qucs_s::wire::Planner::PlanType::Straight);
                            index.addWire(way[k - 1], way[k], -1);
                        }
                    for (std::size_t k = 1; k + 1 < way.size(); ++k) index.addNode(way[k], -1);   // (its bends)
                    wired = true;
                };
                // Clear of every symbol: one of the few shapes, else a way
                // round them a step at a time; over them only when neither.
                // (Clear of the parts' texts too, first: a wire through a
                // source's name read as joined to it.)
                const std::vector<std::vector<QPoint>> ways = waysBetween(sch, from, to);
                for (const std::vector<QPoint>& way : ways)
                    if (index.clear(way, true, ours, true, true)) {
                        draw(way);
                        break;
                    }
                if (!wired)
                    if (const std::vector<QPoint> way = mazeWay(index, from, to, ours, std::max(sch->getGridX(), 1), std::max(sch->getGridY(), 1));
                        !way.empty())
                        draw(way);
                if (!wired)
                    for (const std::vector<QPoint>& way : ways)
                        if (index.clear(way, true, ours)) {
                            draw(way);
                            break;
                        }
                if (!wired)
                    for (const std::vector<QPoint>& way : ways)
                        if (index.clear(way, false, ours)) {
                            draw(way);
                            break;
                        }
                if (!wired) {
                    all = false;
                    break;
                }
            }
            indexed.reset();   // (before merging: that deletes nodes and wires)
            sch->optimizeWires();
            if (all && check().isEmpty()) continue;
            sch->restore(state);
        }
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
        QList<int> ids = places.keys();
        std::sort(ids.begin(), ids.end());   // (a hash's order: the closest of equals another each run)
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
        // From the type's own orientation, as add_component turns it.
        const int want = ((args.value(QLatin1String("rotation")).toInt() + defaultRotation(c->Model)) % 4 + 4) % 4;
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


// The wires drawn across \a part: through its symbol's bounds, a wire's
// last stretch along a pin's stub - to a pin of it - not counted.
int wiresAcross(Schematic* sch, const Component* part)
{
    constexpr int stub = 12;
    const QRect bounds = QRect(QPoint(part->x1, part->y1), QPoint(part->x2, part->y2)).normalized()
                             .translated(part->center()).adjusted(1, 1, -1, -1);
    QSet<std::pair<int, int>> pinsAt;
    for (const Port* p : part->Ports) pinsAt.insert({part->cx + p->x, part->cy + p->y});
    int n = 0;
    for (const Wire* w : sch->a_DocWires) {
        QPoint a = w->P1(), b = w->P2();
        const QPoint along = QPoint((b.x() > a.x()) - (b.x() < a.x()), (b.y() > a.y()) - (b.y() < a.y()));
        const int length = std::abs(b.x() - a.x()) + std::abs(b.y() - a.y());
        int from = 0, to = length;
        if (pinsAt.contains({a.x(), a.y()})) from = std::min(stub, length);
        if (pinsAt.contains({b.x(), b.y()})) to = std::max(length - stub, 0);
        if (from >= to) continue;
        const QPoint p = a + along * from, q = a + along * to;
        if (QRect(p, q).normalized().intersects(bounds)) ++n;
    }
    return n;
}

// One way of putting a part of another type in \a name's place: \a fresh
// (made, turned and mirrored already) centred at \a centre and named
// \a newName, each old pin's net going to the new pin \a pins maps it to
// (old index -> new index) - taken off, the wires of other nets under the
// new pins taken up, put down, and every net in pieces joined again, as
// turnAndMove() does. The new part, or null and why (\a sch part way, for
// the caller to put back: \a fresh is the schematic's by then).
Component* replaceWith(Schematic* sch, const QString& name, Component* fresh, const QPoint& centre, const QString& newName,
                       const QHash<int, int>& pins, QString* why, QStringList* landed)
{
    Component* old = sch->getComponentByName(name);
    const Nets before = netsOf(sch, old);
    // The nets as they are to be: each old pin's key given to the new pin
    // that takes its net ("@.2" of the old part is "@.3" of the new one).
    Nets was = before;
    for (auto it = before.netOf.constBegin(); it != before.netOf.constEnd(); ++it)
        if (it.key().startsWith(QLatin1String("@."))) was.netOf.remove(it.key());
    was.open.clear();
    was.touching.clear();
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) {
        const QString from = QStringLiteral("@.%1").arg(it.key() + 1), to = QStringLiteral("@.%1").arg(it.value() + 1);
        if (!before.netOf.contains(from)) continue;
        was.netOf.insert(to, before.netOf.value(from));
        if (before.open.contains(from)) was.open << to;
        if (before.touching.contains(from)) was.touching << to;
    }
    QSet<std::pair<int, int>> openEnds;
    for (const Node* n : sch->a_DocNodes)
        if (n->conn_count() == 1) openEnds.insert({n->cx, n->cy});
    // A label on an old pin alone goes to its new pin.
    std::map<int, std::unique_ptr<WireLabel>> labels;
    QHash<int, QPoint> labelFrom;
    for (int i = 0; i < old->Ports.size(); ++i) {
        Node* n = old->Ports.at(i)->Connection;
        if (n == nullptr || n->conn_count() != 1 || !n->hasLabel() || !pins.contains(i)) continue;
        labelFrom.insert(pins.value(i), n->center());
        labels[pins.value(i)] = n->releaseLabel();
    }
    sch->detachComp(old);
    delete old;

    Component* c = fresh;
    c->moveCenter(centre.x() - c->cx, centre.y() - c->cy);
    c->Name = newName;
    std::vector<Wire*> doomed;
    for (int i = 0; i < c->Ports.size(); ++i) {
        const QPoint p = c->center() + QPoint(c->Ports.at(i)->x, c->Ports.at(i)->y);
        const int mine = was.netOf.value(QStringLiteral("@.%1").arg(i + 1), -1);
        for (Wire* w : sch->a_DocWires)
            if (!w->hasLabel() && onSegment(p, w->P1(), w->P2()) && before.nodeNet.value(w->Port1, -2) != mine
                && std::find(doomed.begin(), doomed.end(), w) == doomed.end())
                doomed.push_back(w);
    }
    QSet<int> keyed;
    for (auto it = was.netOf.constBegin(); it != was.netOf.constEnd(); ++it) keyed.insert(it.value());
    QList<QPoint> pinsAt, ends;
    for (const Port* p : c->Ports) pinsAt << c->center() + QPoint(p->x, p->y);
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
    for (auto& [pin, label] : labels) {
        Node* n = c->Ports.at(pin)->Connection;
        if (n == nullptr || n->hasLabel() || label == nullptr) continue;
        const QPoint d = n->center() - labelFrom.value(pin);
        label->moveRoot(d.x(), d.y());
        label->moveCenter(d.x(), d.y());
        n->acquireLabel(std::move(label));
    }
    // The ends left open - the old pins' wires, where no new pin came -
    // taken back to where they meet something (a pin, a junction, a
    // label): each new pin is then wired to its net anew, around the parts,
    // rather than to a stub that may run under the new symbol. (A net is
    // no different for it.)
    const auto trimLoose = [&] {
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
    };
    trimLoose();
    const auto check = [&] { return netChanges(was, netsOf(sch, sch->getComponentByName(newName), ends), newName, true, nullptr); };
    QStringList changes = check();
    if (!changes.isEmpty()) {
        *why = changes.join(QStringLiteral("; "));
        return nullptr;
    }
    QString fault;
    if (!joinPieces(sch, was, newName, ends, check, &fault)) {
        *why = tr("not every net could be wired to its new pin (%1)").arg(fault);
        return nullptr;
    }
    trimLoose();
    c = sch->getComponentByName(newName);
    changes = netChanges(was, netsOf(sch, c, ends), newName, false, landed);
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

// The equations of \a sch (Eqn, NutmegEq) whose expressions name the net
// \a name, as "k=v(out) in NutmegEq1".
QStringList equationsNaming(Schematic* sch, const QString& name)
{
    QStringList list;
    const QRegularExpression word(QStringLiteral("(?<![\\w.#])%1(?![\\w#(])").arg(QRegularExpression::escape(name)),
                                  QRegularExpression::CaseInsensitiveOption);
    for (Component* c : sch->a_DocComps) {
        if (c->Model != QLatin1String("Eqn") && c->Model != QLatin1String("NutmegEq")) continue;
        for (Property* p : c->Props)
            if (p->Name != QLatin1String("Export") && p->Name != QLatin1String("Simulation") && word.match(p->Value).hasMatch())
                list << QStringLiteral("%1=%2 in %3").arg(p->Name, p->Value, c->Name);
    }
    return list;
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

namespace {

// \a a with \a b's fields too.
QJsonObject mergedJson(QJsonObject a, const QJsonObject& b)
{
    for (auto it = b.begin(); it != b.end(); ++it) a.insert(it.key(), it.value());
    return a;
}

// A box as the tools give one: [x1, y1, x2, y2].
QJsonArray rectArray(const QRect& r)
{
    return QJsonArray{r.left(), r.top(), r.right(), r.bottom()};
}

QJsonObject issueJson(const qucs_s::erc::Issue& i)
{
    QJsonObject o{{QStringLiteral("message"), i.message}, {QStringLiteral("at"), QJsonArray{i.where.x(), i.where.y()}}};
    if (!i.component.isEmpty()) o.insert(QStringLiteral("component"), i.component);
    if (!i.ref.isEmpty()) o.insert(QStringLiteral("ref"), i.ref);   // (what select, move and delete take: GND#2)
    return o;
}

// What the wiring shows and does not do now that it did not before (a
// wire drawn across another net's, a pin put on one): the messages.
QStringList newWiringIssues(Schematic* sch, const QList<qucs_s::erc::Issue>& before)
{
    QSet<QString> old;
    for (const auto& i : before) old.insert(i.message);
    QStringList added;
    for (const auto& i : qucs_s::erc::wiring(sch))
        if (!old.contains(i.message)) added << i.message;
    return added;
}

} // namespace

QucsControl::QucsControl(QucsApp* app) : QObject(app), a_app(app)
{
    qApp->installEventFilter(this);
    a_tools = QJsonDocument::fromJson(QByteArray(kTools)).array();
    a_tools.append(QJsonDocument::fromJson(R"JSON({"name": "describe_tool",
 "description": "Returns a tool's full description: what it does and returns, its fields and common mistakes. The tools loaded in every turn have short descriptions; without 'name', it lists every tool's summary.",
 "inputSchema": {"type": "object", "properties": {"name": {"type": "string", "description": "The tool (add_component, get_dataset, ...); without it, every tool's summary"}}}})JSON").object());
    // run_script, where the build has Qt's JavaScript engine.
    if (scriptingBuilt())
        a_tools.append(QJsonDocument::fromJson(R"JSON({"name": "run_script",
 "description": "Runs a short JavaScript program against these tools in one turn. qucs.call(tool, args) calls any tool and returns its answer as an object (its JSON, or {text} when the answer is text), and throws when the tool fails - so loops, conditions and arithmetic can go between calls: place and wire a ladder, set a value then simulate and measure repeatedly, check and then simulate. qucs.log(value) adds a line to the answer. The answer's 'result' is the value of the script's last expression, or what a return at the top level gives. There is no file or network access, only the tools. With 'atomic', every schematic is restored if the script throws. 'timeout' is in seconds, 60 by default (600 at most); a tool call running when it runs out - a simulation - finishes first, and the script stops after it.",
 "inputSchema": {"type": "object", "properties": {
   "script": {"type": "string", "description": "JavaScript. Its last expression's value is the result (or return it at the top level). qucs.call gives a tool's answer as the tool gives it: get_schematic with format 'overview' has 'components' as a count, the full read (no format) as a list. Example: for (let i = 1; i <= 5; i++) qucs.call('add_component', {type: 'R', name: 'R' + i, x: 100 * i, y: 100}); qucs.call('get_schematic', {}).components.length"},
   "atomic": {"type": "boolean", "description": "Every schematic put back as it was if the script throws (else what it did before stays)"},
   "timeout": {"type": "integer", "minimum": 1, "maximum": 600, "description": "Seconds it may run, 60 by default; it is stopped after"}}, "required": ["script"]}})JSON").object());
    for (const auto& a : kActions) a_actions.insert(QString::fromLatin1(a.tool), tr(a.action));
    for (const char* t : kReadOnly) a_readOnly << QString::fromLatin1(t);
    // What each tool is to the client (MCP's annotations: those that only
    // look, only add, or may change or take away), and whether it is in
    // every turn or found when needed.
    QSet<QString> additive, core;
    for (const char* t : kAdditive) additive << QString::fromLatin1(t);
    for (const char* t : kCore) core << QString::fromLatin1(t);
    QHash<QString, QString> hints;
    for (const auto& h : kSearchHints) hints.insert(QString::fromLatin1(h.tool), QString::fromLatin1(h.hint));
    QSet<QString> previewable;
    for (const char* t : kPreviewable) previewable << QString::fromLatin1(t);
    QHash<QString, QString>& summaries = a_summaries;
    for (const auto& s : kSummaries) summaries.insert(QString::fromLatin1(s.tool), tr(s.summary));
    for (int i = 0; i < a_tools.size(); ++i) {
        QJsonObject tool = a_tools.at(i).toObject();
        const QString name = tool.value(QLatin1String("name")).toString();
        const bool looks = a_readOnly.contains(name);
        if (previewable.contains(name)) {
            QJsonObject schema = tool.value(QLatin1String("inputSchema")).toObject();
            QJsonObject properties = schema.value(QLatin1String("properties")).toObject();
            properties.insert(QStringLiteral("preview"),
                              QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                                          {QStringLiteral("description"), QStringLiteral("What it would change, part by part, and its answer - "
                                                                                         "every schematic, and any file it writes, put back after: "
                                                                                         "nothing is changed")}});
            schema.insert(QStringLiteral("properties"), properties);
            tool.insert(QStringLiteral("inputSchema"), schema);
        }
        tool.insert(QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), looks},
                                                               {QStringLiteral("destructiveHint"), !looks && !additive.contains(name)},
                                                               {QStringLiteral("idempotentHint"), looks},
                                                               {QStringLiteral("openWorldHint"), false}});
        // In every turn, a summary (the whole kept for describe_tool); a tool
        // found by the tool search when a task needs it is loaded then, and
        // only then - with its whole description, not to need describe_tool
        // before its first use.
        a_details.insert(name, tool);
        if (summaries.contains(name) && core.contains(name)) tool.insert(QStringLiteral("description"), summaries.value(name));
        QJsonObject meta;
        if (core.contains(name)) meta.insert(QStringLiteral("anthropic/alwaysLoad"), true);
        else meta.insert(QStringLiteral("anthropic/searchHint"), hints.value(name, QStringLiteral("qucs-s schematic %1").arg(name)));
        tool.insert(QStringLiteral("_meta"), meta);
        a_tools[i] = tool;
    }
}

QJsonArray QucsControl::tools() const
{
    return a_tools;
}

const QSet<QString>& QucsControl::previewTools()
{
    static const QSet<QString> tools = [] {
        QSet<QString> s;
        for (const char* t : kPreviewable) s << QString::fromLatin1(t);
        return s;
    }();
    return tools;
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
        const int paintings = int(a.value(QLatin1String("paintings")).toArray().size());
        if (paintings > 0) what << (paintings == 1 ? tr("a painting") : tr("%1 paintings").arg(paintings));
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
    else if (tool == QLatin1String("trigger_action"))
        subject = s("path").isEmpty() ? s("action") : tr("%1 on %2").arg(s("action"), QFileInfo(s("path")).fileName());
    else if (tool == QLatin1String("batch")) {
        // Its tools in order, a run of one counted: "add_component ×3, connect ×2".
        QStringList parts;
        QString last;
        int run = 0;
        const auto flush = [&] {
            if (run > 0) parts << (run == 1 ? last : QStringLiteral("%1 ×%2").arg(last).arg(run));
        };
        for (const QJsonValue& v : a.value(QLatin1String("calls")).toArray()) {
            const QString t = v.toObject().value(QLatin1String("tool")).toString();
            if (t == last) {
                ++run;
                continue;
            }
            flush();
            last = t;
            run = 1;
        }
        flush();
        subject = parts.join(QStringLiteral(", "));
    }
    else if (tool == QLatin1String("set_dialog") || tool == QLatin1String("set_ui")) {
        QStringList parts;
        if (tool == QLatin1String("set_ui")) parts << s("area");
        for (const QJsonValue& v : a.value(QLatin1String("set")).toArray())
            parts << v.toObject().value(QLatin1String("control")).toString() + QLatin1Char('=') + propertyValue(v.toObject().value(QLatin1String("value")));
        if (!s("press").isEmpty()) parts << tr("press %1").arg(s("press"));
        subject = parts.join(QStringLiteral(", "));
    } else if (tool == QLatin1String("set_settings") || tool == QLatin1String("get_settings")) {
        QStringList parts{s("scope").isEmpty() ? QStringLiteral("app, simulators, cdl") : s("scope")};
        if (!s("search").isEmpty()) parts << tr("search %1").arg(s("search"));
        for (const QJsonValue& k : a.value(QLatin1String("keys")).toArray()) parts << k.toString();
        // (Iterated as one object kept here: an iterator of a temporary's
        // pointed into an object already gone - and took the window down.)
        const QJsonObject values = a.value(QLatin1String("values")).toObject();
        for (auto it = values.constBegin(); it != values.constEnd(); ++it)
            parts << it.key() + QLatin1Char('=') + propertyValue(it.value());
        subject = parts.join(QStringLiteral(", "));
    } else if (tool == QLatin1String("console")) {
        subject = s("kind") + QStringLiteral(": ") + (a.value(QLatin1String("interrupt")).toBool() ? tr("interrupt (Ctrl-C)")
                                                                                                 : s("input").isEmpty() ? tr("read") : s("input"));
    } else if (tool == QLatin1String("send_input")) {
        // What is sent, all of it: the user is asked about each.
        QStringList given;
        if (a.contains(QLatin1String("click")))
            given << tr("%1%2 click at %3%4")
                         .arg(a.value(QLatin1String("double")).toBool() ? tr("double ") : QString(), s("button").isEmpty() ? tr("left") : s("button"),
                              point(a.value(QLatin1String("click"))),
                              a.contains(QLatin1String("drag_to")) ? tr(", dragged to %1").arg(point(a.value(QLatin1String("drag_to")))) : QString());
        if (!s("keys").isEmpty()) given << tr("keys %1").arg(s("keys"));
        if (!s("text").isEmpty()) given << tr("typed \"%1\"").arg(s("text"));
        subject = tr("%1 on %2").arg(given.join(QStringLiteral(", ")), s("target").isEmpty() ? tr("the canvas") : s("target"));
    } else if (tool == QLatin1String("stop_simulation")) {
        subject = a.contains(QLatin1String("id")) ? tr("simulation %1").arg(a.value(QLatin1String("id")).toInt()) : tr("the simulation running");
    } else if (tool == QLatin1String("context_menu")) {
        const QJsonObject on = a.value(QLatin1String("on")).toObject();
        const QString where = on.isEmpty() ? QString() : on.constBegin().key() + QLatin1Char(' ') + propertyValue(on.constBegin().value());
        subject = s("choose").isEmpty() ? tr("the menu on %1").arg(where) : tr("%1 on %2").arg(s("choose"), where);
    } else if (tool == QLatin1String("zoom")) subject = s("to");
    else if (tool == QLatin1String("screenshot")) subject = s("area").isEmpty() ? s("path") : s("area");
    else if (tool == QLatin1String("get_schematic")) subject = s("path");
    else if (tool == QLatin1String("select")) {
        QStringList names;
        for (const QJsonValue& v : a.value(QLatin1String("names")).toArray()) names << v.toString();
        subject = names.join(QStringLiteral(", "));
    } else if (tool == QLatin1String("list_component_types") || tool == QLatin1String("list_actions")) subject = s("search");
    else if (tool == QLatin1String("simulate") || tool == QLatin1String("get_netlist") || tool == QLatin1String("reload_data")) subject = s("path");
    else if (tool == QLatin1String("export_netlist")) subject = s("save_as");
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
    else if (tool == QLatin1String("move_to_pane"))
        subject = (s("path").isEmpty() ? tr("the document in front") : s("path")) + QStringLiteral(" → ")
                  + (a.value(QLatin1String("pane")).isDouble() ? tr("pane %1").arg(a.value(QLatin1String("pane")).toInt()) : s("pane"));
    else if (tool == QLatin1String("add_marker"))
        subject = tr("diagram %1: %2").arg(a.value(QLatin1String("diagram")).toInt(1))
                      .arg(a.value(QLatin1String("at")).isDouble() ? QString::number(a.value(QLatin1String("at")).toDouble()) : s("at"));
    else if (tool == QLatin1String("edit_marker") || tool == QLatin1String("delete_marker"))
        subject = tr("diagram %1, marker %2").arg(a.value(QLatin1String("diagram")).toInt(1)).arg(a.value(QLatin1String("marker")).toInt(1));
    else if (tool == QLatin1String("edit_trace"))
        subject = a.value(QLatin1String("trace")).isString() ? s("trace") : tr("trace %1").arg(a.value(QLatin1String("trace")).toInt(1));
    else if (tool == QLatin1String("rename_net")) subject = s("from") + QStringLiteral(" → ") + s("to");
    else if (tool == QLatin1String("describe_component_type")) subject = s("type");
    else if (tool == QLatin1String("add_painting"))
        subject = s("type") + (s("text").isEmpty() ? QString() : QStringLiteral(" \u201C%1\u201D").arg(s("text")))
                  + (a.value(QLatin1String("symbol")).toBool() ? tr(" on the symbol") : QString());
    else if (tool == QLatin1String("edit_painting"))
        subject = tr("painting %1").arg(a.value(QLatin1String("painting")).toInt())
                  + (a.value(QLatin1String("symbol")).toBool() ? tr(" of the symbol") : QString());
    else if (tool == QLatin1String("set_subcircuit_parameters")) {
        QStringList names;
        for (const QJsonValue& v : a.value(QLatin1String("parameters")).toArray())
            names << (v.isString() ? v.toString() : v.toObject().value(QLatin1String("name")).toString());
        for (const QJsonValue& v : a.value(QLatin1String("remove")).toArray()) names << QStringLiteral("-") + v.toString();
        subject = (s("path").isEmpty() ? tr("the schematic in front") : s("path")) + (names.isEmpty() ? QString() : QStringLiteral(": ") + names.join(QStringLiteral(", ")));
    } else if (tool == QLatin1String("export_image"))
        subject = s("save_as") + (a.contains(QLatin1String("diagram")) ? tr(" (diagram %1)").arg(a.value(QLatin1String("diagram")).toInt()) : QString());
    else if (tool == QLatin1String("set_simulator")) subject = s("simulator");
    else if (tool == QLatin1String("build_verilog_a")) subject = s("file").isEmpty() ? tr("the .va in front") : s("file");
    else if (tool == QLatin1String("new_project") || tool == QLatin1String("open_project")) subject = s("name");
    else if (tool == QLatin1String("import_data"))
        subject = (a.value(QLatin1String("remove")).toBool() ? tr("remove ") : a.value(QLatin1String("reload")).toBool() ? tr("read again ") : QString())
                  + (s("file").isEmpty() ? s("name") : s("file"));
    else if (tool == QLatin1String("export_data")) {
        QStringList what;
        for (const QJsonValue& v : a.value(QLatin1String("variables")).toArray()) what << v.toString();
        subject = s("save_as") + QStringLiteral(" \u2190 ")
                  + (a.contains(QLatin1String("diagram")) ? tr("diagram %1").arg(a.value(QLatin1String("diagram")).toInt()) : what.join(QStringLiteral(", ")));
    } else if (tool == QLatin1String("import_netlist")) subject = s("file").isEmpty() ? tr("%1 lines").arg(s("text").count(QLatin1Char('\n')) + 1) : s("file");
    else if (tool == QLatin1String("copy_document")) subject = (s("path").isEmpty() ? tr("the schematic in front") : s("path")) + QStringLiteral(" → ") + s("to");
    else if (tool == QLatin1String("rename_file")) subject = s("path") + QStringLiteral(" → ") + s("to");
    else if (tool == QLatin1String("trash_file")) subject = s("path");
    else if (tool == QLatin1String("clean_scratch")) subject = (s("path").isEmpty() ? tr("the schematic in front") : s("path"))
                                                               + (a.value(QLatin1String("datasets")).toBool() ? tr(", datasets too") : QString());
    else if (tool == QLatin1String("edit_text") || tool == QLatin1String("get_text") || tool == QLatin1String("goto_line")) {
        subject = s("path").isEmpty() ? tr("the text in front") : s("path");
        if (tool == QLatin1String("edit_text"))
            subject += tr(": %n edit(s)", "", int(a.value(QLatin1String("edits")).toArray().size()));
        else if (tool == QLatin1String("goto_line")) subject += tr(", line %1").arg(a.value(QLatin1String("line")).toInt());
    } else if (tool == QLatin1String("read_pdf")) subject = s("search").isEmpty() ? s("path") : tr("%1 in %2").arg(s("search"), s("path"));
    else if (tool == QLatin1String("find_library_component")) subject = (s("type") + QLatin1Char(' ') + s("search")).trimmed();
    else if (tool == QLatin1String("describe_part")) subject = s("library") + QLatin1Char('/') + s("part");
    else if (tool == QLatin1String("list_libraries")) subject = s("library");
    else if (tool == QLatin1String("create_library") || tool == QLatin1String("import_library"))
        subject = (tool == QLatin1String("create_library") ? s("name") : s("path")) + QStringLiteral(" → ")
                  + (s("destination").isEmpty() ? QStringLiteral("user_lib") : s("destination"))
                  + (a.value(QLatin1String("replace")).toBool() ? tr(", replacing") : QString());
    else if (tool == QLatin1String("tune"))
        subject = s("component") + (s("property").isEmpty() ? QString() : QLatin1Char('.') + s("property"))
                  + (a.value(QLatin1String("target")).isDouble() ? tr(" to %1").arg(a.value(QLatin1String("target")).toDouble()) : QString());
    else if (tool == QLatin1String("list_documents")) subject = s("folder");
    else if (tool == QLatin1String("check_schematic")) subject = s("path");
    else if (tool == QLatin1String("create_subcircuit")) subject = s("save_as");
    else if (tool == QLatin1String("add_analysis"))
        subject = s("kind") + (s("parameter").isEmpty() ? QString() : QLatin1Char(' ') + s("parameter"))
                  + (a.value(QLatin1String("plot")).toArray().isEmpty() ? QString() : tr(" with a diagram"));
    else if (tool == QLatin1String("move")) {
        QStringList names;
        for (const QJsonValue& v : a.value(QLatin1String("names")).toArray()) names << v.toString();
        subject = tr("%1 by %2, %3").arg(names.join(QStringLiteral(", "))).arg(a.value(QLatin1String("dx")).toInt()).arg(a.value(QLatin1String("dy")).toInt());
    }
    else if ((tool == QLatin1String("undo") || tool == QLatin1String("redo")) && a.value(QLatin1String("steps")).toInt() > 1)
        subject = tr("%1 steps").arg(a.value(QLatin1String("steps")).toInt());
    if (subject.size() > 160) subject = subject.left(159) + QChar(0x2026);
    return subject;
}

QString QucsControl::instructions() const
{
    return QStringLiteral(
        "These tools drive the Qucs-S window the user is looking at (or, when run as qucs-s --mcp-server, a Qucs-S "
        "without a window). The tools loaded in every turn have short descriptions; describe_tool returns the full "
        "description of any tool (what it returns, its fields, common mistakes). The other tools are found by the tool "
        "search when a task needs them and come with their full description. If these instructions end in "
        "\"[truncated]\", the client cut them: the resource qucs://instructions holds them whole (Claude Code keeps "
        "2,048 characters unless CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH says more).\n\n"
        "Start with get_state to see what is open. Documents: open_document, show_document, new_document, save_document, "
        "close_document. Read a schematic with get_schematic (components with pin positions, nets, or the .sch text), "
        "then change it with add_component, edit_component, connect (pin to pin, such as \"R1.2\" to \"C1.1\", or a pin "
        "to \"ground\"), add_wire, set_label, delete, or set_schematic (whole sections at once, preferably as JSON). "
        "arrange lays a schematic out again by signal flow, keeping every net. Also select, zoom, undo, redo, and "
        "screenshot to see the result. list_component_types lists add_component's types and describe_component_type a "
        "type's properties. rename_net renames a net together with the traces that show it. batch runs several tools in "
        "one call: use it for several changes at once (placing and wiring parts, setting properties, adding a diagram and "
        "its traces) instead of one call each.\n\n"
        "Menus: list_actions and trigger_action; read a dialog it opens with get_dialog and answer it with set_dialog. get_ui and "
        "set_ui read and use the rest of the window as those do a dialog - a dock, a panel (Content, Projects, Problems, "
        "Tuner), a toolbar, the status bar, the tabs - and context_menu a right-click menu. console types a line into the Octave, Python Shell or Terminal dock and "
        "returns what it printed - it runs, and the user is asked each time. get_settings and set_settings read and "
        "change the settings (application, simulators, a document's, CDL) by typed keys, each change with its old "
        "value. send_input is the last resort: a raw click, drag or keys on the canvas or a part of the window, with a "
        "picture after (asked each time). read_help finds what this build's help says (menu actions, component types, "
        "examples). "
        "simulate runs the simulator and reports errors; with 'background' it answers at once with an id, which "
        "simulation_status, wait_for and stop_simulation follow (a long run: Monte Carlo, a long transient), and a run "
        "past its timeout goes on the same way. wait_for waits until a run ends, a dialog comes up, a document changes "
        "or a file is written - rather than asking again and again. get_netlist returns the netlist. get_dataset reads results as "
        "numbers and measures them (rise time, overshoot, value at a time, ...): use it rather than a screenshot to judge "
        "a simulation. Diagrams: add_diagram (on the schematic, or with document: \"data_display\" on its data display, "
        "a .dpl for a report apart from the circuit), edit_diagram, add_trace, edit_trace, delete; markers: add_marker (at an x "
        "value, the peak, -3 dB or a crossing), edit_marker, delete_marker; reload_data re-reads the data. Paintings "
        "(texts, arrows, boxes, text boxes, tables, dimensions, formulas) annotate a result or draw a symbol: "
        "add_painting, edit_painting, delete. list_documents lists the files of the workspace or a project; export_image "
        "writes a picture of a schematic or a single diagram; set_simulator chooses the simulator. tune adjusts a part's "
        "value, simulating and measuring until a measurement reaches its target - one call instead of three per "
        "iteration. build_verilog_a compiles a .va file now and reports errors with their lines; describe_component_type "
        "lists a Verilog-A module's parameters. find_library_component finds a part by its values (an NPN with Bf near "
        "200), describe_part a library part's pins, model and tested status; list_libraries lists the libraries as the Libraries "
        "panel does, create_library makes one of the project's subcircuits and import_library brings a library file in; "
        "the Verilog-A of a library device a schematic of the project uses is linked into the project, in Libraries/<its "
        "library>/ (read-only - change it in the library -, compiled there; no tool writes a file onto it, which would "
        "write the library's own file), and taken away with its model when no "
        "schematic uses the device; a part named by its library's name is taken from the first library of that name that "
        "has the part (of two that have it, the one the project linked its Verilog-A from: check_schematic warns which); "
        "read_pdf reads a datasheet's text; "
        "get_text and edit_text read and edit a text tab (a netlist, a .va) with the user's unsaved edits, goto_line "
        "shows a line of it; "
        "import_netlist builds a schematic from a SPICE netlist; import_data brings a data file (CSV, a workbook, NumPy, "
        "Touchstone) in as a dataset beside the schematic, whose traces are name:variable - measured data plotted with a "
        "simulation's, or alone; export_data writes a diagram's curves (or a dataset's variables) out for another program - "
        "CSV, an Excel workbook, text, NumPy; make_symbol "
        "draws a subcircuit's symbol and set_subcircuit_parameters gives it parameters (a value inside uses {Rs}; each "
        "instance sets its own). ngspice_commands tells which commands ngspice has (analyses, measurements, output, "
        "statistics, the .control language), their syntax and which the installed ngspice has - for a Nutmeg script or a "
        "NutmegEq. new_project, open_project, copy_document, clean_scratch, rename_file and trash_file manage files (rather "
        "than mv and rm: open documents follow, and the trash keeps what goes; undo with 'files' renames back and takes "
        "back from the trash). get_state names the open project. "
        "undo_history lists the undo steps in words. \"selection\": true acts on what the user selected (move, delete, "
        "create_subcircuit, get_schematic).\n\n"
        "Each tool result reports, part by part, what the user changed since your last call. Changes appear in the "
        "window immediately, each as one undo step. Prefer these tools to editing the file of an open schematic: a file "
        "you change on disk is reloaded only if its document has no unsaved changes in Qucs-S, and the next tool result "
        "tells you when it was not. A ground has no name: get_schematic gives each its 'ref' (GND#2), which the tools "
        "that take a part's name accept. Coordinates are schematic units (the grid is usually 10; keep pins on it). A file "
        "dialog an action opens is Qt's: set_dialog answers it with its \"File name\" (or \"Directory\") field, a path, and "
        "Open, Save or Choose. A document is named "
        "by its path (relative to the open project's folder, otherwise the workspace) or, when it is open, by its file "
        "name alone (amp.sch). A conversation the user has pinned to a schematic says so in its prompts: the tools then "
        "act on that schematic when no path is given, whichever document is in front, and trigger_action brings it to "
        "the front first. Every tool also takes 'max_chars', the longest answer you want (200 or more): a longer one "
        "comes back with its longest lists and texts cut (\"… 40 more\") and a 'trimmed' field that says what was cut - JSON "
        "still, however short; a batch's answers are cut together, and each of its calls takes one of its own.\n\n"
        "How to work, as it has worked best:\n"
        "1. A library part: describe_part first - its pins by name and role, its supplies, and whether its model passed "
        "its bench (find_library_component with 'tested' finds only those). Wire it by pin name (connect \"U1.INP\" to "
        "\"Vin.1\") in one batch with the rest, and give every supply pin a supply.\n"
        "2. arrange once it is built. 'feedback': below puts an op-amp's feedback parts (Rf, and Rg to ground) under it; "
        "an op-amp whose - input is its upper pin reads best mirrored ('mirror': true) - describe_part's 'side' says which "
        "(upper in ua741(TI), tl081(TI), OP07(TI); lower in uA741 and AD825: leave those) - and 'supplies': labels spares "
        "the supply wires. 'straighten' lines up what is left; 'preview' shows it without keeping it.\n"
        "3. check_schematic before simulate, with 'subcircuits': true when there are any. Its errors are what the "
        "simulator fails on, its warnings what runs and gives nonsense, its notes fine if meant. Nothing found does not "
        "mean the circuit works.\n"
        "4. simulate ('brief': what came of it only), then get_dataset: 'operating_point' for DC, 'measure' for a gain, "
        "a bandwidth, a rise time. Judge by numbers, not by a picture. simulate's 'keep_as' keeps a run; get_dataset's "
        "'compare' sets the next against it.\n"
        "5. A value to meet a target: tune, not a loop of edits and runs; 'hold' keeps other measurements within bounds, "
        "'compare' gives each before and after.\n"
        "6. Long answers: batch's 'brief', each tool's own filters, else 'max_chars'. To go back: undo (steps as "
        "undo_history lists them) and undo with 'files' (the files the last calls wrote).");
}

QJsonObject QucsControl::forDocument(const QString& tool, const QJsonObject& arguments, const QString& document) const
{
    if (document.isEmpty()) return arguments;
    if (tool == QLatin1String("batch")) {
        // Each of its calls, as it would be alone.
        QJsonArray calls;
        for (const QJsonValue& v : arguments.value(QLatin1String("calls")).toArray()) {
            QJsonObject call = v.toObject();
            call.insert(QStringLiteral("arguments"),
                        forDocument(call.value(QLatin1String("tool")).toString(), call.value(QLatin1String("arguments")).toObject(), document));
            calls.append(call);
        }
        QJsonObject a = arguments;
        a.insert(QStringLiteral("calls"), calls);
        return a;
    }
    if (!arguments.value(QLatin1String("path")).toString().trimmed().isEmpty()) return arguments;
    // The tools whose 'path' is the document they act on, the one in
    // front when not given; and get_state, which names it.
    bool onDocument = tool == QLatin1String("get_state");
    // (A new document is of no schematic - but a data display is of one.)
    const bool newOfNone = tool == QLatin1String("new_document") && arguments.value(QLatin1String("kind")).toString() != QLatin1String("data_display");
    if (tool != QLatin1String("open_document") && tool != QLatin1String("show_document")
        && tool != QLatin1String("reload_data") && !newOfNone)
        for (const QJsonValue& t : a_tools)
            if (t.toObject().value(QLatin1String("name")).toString() == tool)
                onDocument = onDocument || t.toObject().value(QLatin1String("inputSchema")).toObject()
                                               .value(QLatin1String("properties")).toObject().contains(QLatin1String("path"));
    if (!onDocument) return arguments;
    QJsonObject a = arguments;
    a.insert(QStringLiteral("path"), document);
    return a;
}

bool QucsControl::eventFilter(QObject* watched, QEvent* event)
{
    // (Shown while a call runs: a box exec()'d within it. One the user's
    // own action opens between calls - or while a call waits in an event
    // loop of its own - is theirs; with no window, there is no user.)
    if (event->type() == QEvent::Show && misc::ErrorCapture::active() && (a_headless || a_spinning == 0))
        if (auto* box = qobject_cast<QMessageBox*>(watched); box != nullptr && box->windowModality() != Qt::NonModal) {
            QPointer<QMessageBox> held(box);
            QTimer::singleShot(0, box, [held] {
                if (!held || !held->isVisible()) return;
                QString said = held->text();
                if (!held->informativeText().isEmpty()) said += QLatin1Char(' ') + held->informativeText();
                if (Qt::mightBeRichText(said)) said = QTextDocumentFragment::fromHtml(said).toPlainText();
                said = said.simplified();
                held->close();   // (its escape button: No, Cancel, OK)
                if (held && held->isVisible()) held->reject();
                const QString answer = held && held->clickedButton() != nullptr ? cleanText(held->clickedButton()->text()) : QString();
                misc::reportError(answer.isEmpty() ? tr("%1 (a message box, closed)").arg(said)
                                                   : tr("%1 (a message box, answered %2)").arg(said, answer));
            });
        }
    return QObject::eventFilter(watched, event);
}

// ----------------------------------------------------------------------
void QucsControl::callTool(const QString& tool, const QJsonObject& arguments, std::function<void(const QJsonObject&)> done)
{
    bool async = false;
    // Errors are told to Claude, not shown in message boxes.
    misc::ErrorCapture capture;
    // What a tool did beside what was asked, after its result.
    const auto withNotes = [this](QJsonObject result) {
        if (a_callNotes.isEmpty()) return result;
        QJsonArray content = result.value(QStringLiteral("content")).toArray();
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), std::exchange(a_callNotes, {}).join(QLatin1Char(' '))}});
        result.insert(QStringLiteral("content"), content);
        return result;
    };
    QJsonObject result = call(tool, arguments, [done, withNotes](const QJsonObject& r) { done(withNotes(r)); }, async);
    if (async) return;
    result = withNotes(result);
    const QStringList errors = capture.errors();
    if (!errors.isEmpty()) {
        QJsonArray content = result.value(QStringLiteral("content")).toArray();
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), tr("Qucs-S reported: %1").arg(errors.join(QStringLiteral("\n")))}});
        result.insert(QStringLiteral("content"), content);
    }
    done(result);
}

bool QucsControl::runsAlone(const QString& tool, const QJsonObject& arguments)
{
    return tool == QLatin1String("batch") || tool == QLatin1String("run_script") || tool == QLatin1String("tune")
           || arguments.value(QLatin1String("preview")).toBool();
}

void QucsControl::runWaiting()
{
    while (a_alone == 0 && !a_waiting.isEmpty()) {
        Waiting w = a_waiting.takeFirst();
        callToolFor(w.caller, w.tool, w.arguments, w.done, true);
    }
}

void QucsControl::callToolFor(quint64 caller, const QString& tool, const QJsonObject& arguments,
                              std::function<void(const QJsonObject&)> done)
{
    callToolFor(caller, tool, arguments, std::move(done), false);
}

void QucsControl::callToolFor(quint64 caller, const QString& tool, const QJsonObject& arguments,
                              std::function<void(const QJsonObject&)> done, bool waited)
{
    // Behind one that runs alone (and those waiting already, in order) -
    // but a dialog's own tools, which may be what it waits for.
    const bool dialogs = tool == QLatin1String("get_dialog") || tool == QLatin1String("set_dialog");
    if (!dialogs && (a_alone > 0 || (!waited && !a_waiting.isEmpty()))) {
        a_waiting.append(Waiting{caller, tool, arguments, std::move(done)});
        return;
    }
    const bool alone = runsAlone(tool, arguments);
    if (alone) ++a_alone;
    const QStringList changes = changesSince(caller) + notesFor(caller);
    a_callers.append(caller);
    QucsDoc::setEditor(caller);
    auto over = std::make_shared<bool>(false), released = std::make_shared<bool>(false);
    callTool(tool, arguments, [this, caller, changes, done, alone, over, released](const QJsonObject& r) {
        *over = true;
        if (alone && --a_alone == 0 && !a_waiting.isEmpty())
            QTimer::singleShot(0, this, [this] { runWaiting(); });
        if (!*released) a_callers.removeOne(caller);
        QucsDoc::setEditor(a_callers.isEmpty() ? 0 : a_callers.last());
        noteSeen(caller);
        QJsonObject result = r;
        if (!changes.isEmpty()) {
            QJsonArray content = result.value(QStringLiteral("content")).toArray();
            content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                       {QStringLiteral("text"), tr("Since your last call: %1").arg(changes.join(QLatin1Char(' ')))}});
            result.insert(QStringLiteral("content"), content);
        }
        done(result);
    });
    // A call that only waits (wait_for; a console's line running; a run
    // simulated) edits nothing while it waits: what is edited meanwhile is
    // the user's - or another call's -, not its caller's. (A run's own
    // edit at its end - apply_optimum's - is said to be its caller's.)
    if (!*over && (tool == QLatin1String("wait_for") || tool == QLatin1String("console") || tool == QLatin1String("simulate"))) {
        *released = true;
        a_callers.removeOne(caller);
        QucsDoc::setEditor(a_callers.isEmpty() ? 0 : a_callers.last());
    }
}

void QucsControl::noteForConversations(const QString& text)
{
    // (Once: the dock and the file watcher may both tell of one edit.)
    const QDateTime now = QDateTime::currentDateTime();
    for (const Broadcast& b : std::as_const(a_broadcasts))
        if (b.text == text && b.at.secsTo(now) < 60) return;
    a_broadcasts.append({++a_broadcastSerial, now, text});
    while (a_broadcasts.size() > 50) a_broadcasts.removeFirst();
}

QStringList QucsControl::notesFor(quint64 caller)
{
    // (Half an hour at most: a conversation that comes later is told what
    // may still concern it, not the whole day.)
    const QDateTime since = QDateTime::currentDateTime().addSecs(-30 * 60);
    const qint64 seen = a_broadcastSeen.value(caller, 0);
    QStringList notes;
    for (const Broadcast& b : std::as_const(a_broadcasts))
        if (b.serial > seen && b.at >= since) notes << b.text;
    a_broadcastSeen.insert(caller, a_broadcastSerial);
    return notes;
}

QString QucsControl::seenKey(QucsDoc* doc)
{
    // (One not saved yet has no file: its place in memory, while it lives.)
    return doc->getDocName().isEmpty() ? QStringLiteral("#%1").arg(quintptr(doc), 0, 16) : doc->getDocName();
}

QDateTime QucsControl::datasetWritten(QucsDoc* doc)
{
    if (dynamic_cast<Schematic*>(doc) == nullptr || doc->getDocName().isEmpty()) return {};
    const QFileInfo info(datasetFile(doc->getDocName(), doc->getDataSet(), QucsSettings.DefaultSimulator));
    return info.exists() ? info.lastModified() : QDateTime();
}

QString QucsControl::whoMade(quint64 by, quint64 caller) const
{
    if (by == 0) return tr("the user");
    if (by == QucsDoc::kOnDisk) return tr("its file, changed on disk (by a command such as your Bash, another program or the user)");
    if (by == QucsDoc::kSimulation) return tr("the simulation (an optimizer's result written into its knobs)");
    return by == caller ? tr("you") : tr("another conversation");
}

QStringList QucsControl::changesSince(quint64 caller) const
{
    const auto seenIt = a_seen.constFind(caller);
    if (seenIt == a_seen.cend()) return {};   // (its first call: nothing to compare with)
    const QHash<QString, Seen>& seen = *seenIt;
    QStringList lines;
    QSet<QString> open;
    for (QucsDoc* doc : a_app->allDocuments()) {
        const QString key = seenKey(doc);
        open.insert(key);
        const auto it = seen.constFind(key);
        if (it == seen.cend()) {
            lines << tr("%1 has been opened (revision %2).").arg(titleOf(doc)).arg(doc->revision());
            continue;
        }
        int byUser = 0, byOthers = 0, onDisk = 0, bySimulation = 0;
        QDateTime last;
        for (const QucsDoc::Edit& e : doc->recentEdits()) {
            if (e.revision <= it->revision || e.by == caller) continue;
            (e.by == 0 ? byUser : e.by == QucsDoc::kOnDisk ? onDisk : e.by == QucsDoc::kSimulation ? bySimulation : byOthers)++;
            last = e.at;
        }
        if (byUser + byOthers + onDisk + bySimulation > 0) {
            // (A file changed on disk: by whom is not known - a command of
            // yours, another program, the user.)
            QStringList whom;
            if (byUser > 0) whom << tr("the user");
            if (byOthers > 0) whom << tr("another conversation");
            if (onDisk > 0) whom << tr("its file, changed on disk (by a command such as your Bash, another program or the user) and loaded again");
            if (bySimulation > 0) whom << tr("the simulation (an optimizer's result written into its knobs)");
            const QString who = whom.join(tr(" and "));
            byOthers += onDisk + bySimulation;
            // What changed, part by part (a schematic's), from how it was.
            QStringList what;
            auto* sch = dynamic_cast<Schematic*>(doc);
            if (sch != nullptr && !it->state.isEmpty()) what = describeChanges(it->state, sch->snapshot(), 20);
            const bool complete = !what.isEmpty() && !what.last().startsWith(tr("and "));
            lines << tr("%1 was changed by %2 - %3 edit(s), the last at %4; now revision %5, it was %6.")
                         .arg(titleOf(doc), who).arg(byUser + byOthers).arg(last.toString(QStringLiteral("HH:mm:ss")))
                         .arg(doc->revision()).arg(it->revision)
                  + (what.isEmpty() ? tr(" What you read of it before may no longer hold: read it again.")
                                    : tr(" What changed: %1.").arg(what.join(QStringLiteral("; ")))
                                          + (complete ? QString() : tr(" (More than this: read it again.)")));
        }
        const QDateTime written = datasetWritten(doc);
        if (written.isValid() && (!it->dataset.isValid() || written > it->dataset))
            lines << tr("%1 was simulated again (its dataset written at %2): results you read before are not these.")
                         .arg(titleOf(doc), written.toString(QStringLiteral("HH:mm:ss")));
    }
    for (auto it = seen.cbegin(); it != seen.cend(); ++it)
        if (!open.contains(it.key()) && !it.key().startsWith(QLatin1Char('#')))
            lines << tr("%1 has been closed.").arg(QFileInfo(it.key()).fileName());
    return lines;
}

void QucsControl::noteSeen(quint64 caller)
{
    const QHash<QString, Seen> before = a_seen.value(caller);
    QHash<QString, Seen> seen;
    for (QucsDoc* doc : a_app->allDocuments()) {
        Seen s{doc->revision(), datasetWritten(doc), {}};
        // A schematic's state, to tell what others change part by part (not
        // of a huge one: taken after every call).
        if (auto* sch = dynamic_cast<Schematic*>(doc); sch != nullptr && sch->a_DocComps.size() <= 5000) {
            const auto was = before.constFind(seenKey(doc));
            s.state = was != before.cend() && was->revision == s.revision && !was->state.isEmpty() ? was->state : sch->snapshot();
        }
        seen.insert(seenKey(doc), s);
    }
    a_seen.insert(caller, seen);
}

QJsonObject QucsControl::callNow(const QString& tool, const QJsonObject& arguments, int timeoutMs, quint64 caller)
{
    QJsonObject result;
    bool finished = false;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto take = [&](const QJsonObject& r) {
        result = r;
        finished = true;
        loop.quit();
    };
    if (caller != 0) callToolFor(caller, tool, arguments, take);
    else callTool(tool, arguments, take);
    if (!finished) {
        timer.start(timeoutMs);
        ++a_spinning;
        loop.exec();
        --a_spinning;
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

namespace {

// How many letters \a a and \a b are apart: one left out, put in, changed
// or two swapped each count one (lable is one from label).
int lettersApart(const QString& a, const QString& b)
{
    std::vector<std::vector<int>> d(a.size() + 1, std::vector<int>(b.size() + 1));
    for (qsizetype i = 0; i <= a.size(); ++i) d[i][0] = int(i);
    for (qsizetype j = 0; j <= b.size(); ++j) d[0][j] = int(j);
    for (qsizetype i = 1; i <= a.size(); ++i)
        for (qsizetype j = 1; j <= b.size(); ++j) {
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a.at(i - 1) == b.at(j - 1) ? 0 : 1)});
            if (i > 1 && j > 1 && a.at(i - 1) == b.at(j - 2) && a.at(i - 2) == b.at(j - 1)) d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    return d[a.size()][b.size()];
}

// The names in \a takes near \a unknown: a letter or two off, the same
// beginning, or one in the other.
QStringList nearNamesOf(const QStringList& unknown, const QStringList& takes)
{
    QStringList near;
    for (const QString& u : unknown)
        for (const QString& t : takes) {
            const QString a = u.toLower(), b = t.toLower();
            int same = 0;
            while (same < std::min(a.size(), b.size()) && a.at(same) == b.at(same)) ++same;
            const bool typo = std::min(a.size(), b.size()) >= 3 && lettersApart(a, b) <= (std::max(a.size(), b.size()) > 5 ? 2 : 1);
            if ((typo || a.contains(b) || b.contains(a) || same >= std::min<qsizetype>(4, std::min(a.size(), b.size()))) && !near.contains(t)) near << t;
        }
    return near;
}

// The fields of \a schema an object \a value has not, inside it too - a
// set_schematic part's "rotaton" - as "where: its keys, and what it
// takes"; empty when all are known. \a schema's 'properties' name them,
// an 'items' schema each of an array's, an object alternative of 'anyOf'
// an object's; 'additionalProperties' (true, or a schema: a map by name)
// takes any.
QString unknownIn(const QJsonValue& value, const QJsonObject& schema, const QString& where)
{
    if (value.isArray()) {
        const QJsonObject items = schema.value(QLatin1String("items")).toObject();
        if (items.isEmpty()) return {};
        const QJsonArray a = value.toArray();
        for (int i = 0; i < a.size(); ++i) {
            // (A part by its name, else by its place in the list.)
            const QString name = a.at(i).toObject().value(QLatin1String("name")).toString();
            if (QString u = unknownIn(a.at(i), items, QStringLiteral("%1[%2]%3").arg(where).arg(i).arg(name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(name)));
                !u.isEmpty())
                return u;
        }
        return {};
    }
    if (!value.isObject()) return {};
    QJsonObject object = schema;
    if (!schema.contains(QLatin1String("properties")))
        for (const QJsonValue& alt : schema.value(QLatin1String("anyOf")).toArray())
            if (alt.toObject().contains(QLatin1String("properties"))) object = alt.toObject();
    if (!object.contains(QLatin1String("properties")) || object.contains(QLatin1String("additionalProperties"))) return {};
    const QJsonObject fields = object.value(QLatin1String("properties")).toObject();
    const QJsonObject o = value.toObject();
    QStringList unknown;
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!fields.contains(it.key())) unknown << it.key();
        else if (QString u = unknownIn(it.value(), fields.value(it.key()).toObject(), where + QLatin1Char('.') + it.key()); !u.isEmpty())
            return u;
    if (unknown.isEmpty()) return {};
    const QStringList near = nearNamesOf(unknown, fields.keys());
    return tr("%1 has no %2; it takes %3.%4").arg(where, unknown.join(QStringLiteral(", ")), fields.keys().join(QStringLiteral(", ")),
                                                            near.isEmpty() ? QString() : tr(" Meant %1?").arg(near.join(tr(" or "))));
}

// What a JSON value is, said: "the number 1", "the text \"yes\"".
QString whatItIs(const QJsonValue& v)
{
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isDouble()) return tr("the number %1").arg(v.toDouble(), 0, 'g', 12);
    if (v.isString()) return tr("the text \"%1\"").arg(v.toString().size() > 40 ? v.toString().left(40) + QStringLiteral("...") : v.toString());
    if (v.isArray()) return tr("a list");
    if (v.isObject()) return tr("an object");
    return tr("null");
}

// Whether \a v is of the JSON schema's \a type (one this does not know takes all).
bool isOfType(const QJsonValue& v, const QString& type)
{
    if (type == QLatin1String("string")) return v.isString();
    if (type == QLatin1String("number")) return v.isDouble();
    if (type == QLatin1String("integer")) return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() == std::floor(v.toDouble());
    if (type == QLatin1String("boolean")) return v.isBool();
    if (type == QLatin1String("array")) return v.isArray();
    if (type == QLatin1String("object")) return v.isObject();
    if (type == QLatin1String("null")) return v.isNull();
    return true;
}

// A schema's types: "string", ["string", "number"]; none when it names none.
QStringList typesOf(const QJsonObject& schema)
{
    const QJsonValue t = schema.value(QLatin1String("type"));
    if (t.isString()) return {t.toString()};
    QStringList types;
    for (const QJsonValue& v : t.toArray()) types << v.toString();
    return types;
}

QString typeWords(const QStringList& types)
{
    QStringList words;
    for (const QString& t : types)
        words << (t == QLatin1String("string")    ? tr("a text")
                  : t == QLatin1String("number")  ? tr("a number")
                  : t == QLatin1String("integer") ? tr("a whole number")
                  : t == QLatin1String("boolean") ? tr("true or false")
                  : t == QLatin1String("array")   ? tr("a list")
                  : t == QLatin1String("object")  ? tr("an object")
                                                  : t);
    words.removeDuplicates();
    return words.join(tr(" or "));
}

// The first value in \a value of another JSON type than its schema says,
// inside it too (a list's items, an object's fields, a map's values), as
// "where is a list, not an object"; empty when all are. \a schema's
// 'type' names the types, else each alternative of its 'anyOf' does.
QString wrongTypeIn(const QJsonValue& value, const QJsonObject& schema, const QString& where)
{
    if (value.isNull() || value.isUndefined()) return {};
    QJsonObject object = schema;
    QStringList types = typesOf(schema);
    if (types.isEmpty() && schema.contains(QLatin1String("anyOf"))) {
        QStringList all;
        bool one = false;
        for (const QJsonValue& alt : schema.value(QLatin1String("anyOf")).toArray()) {
            const QStringList altTypes = typesOf(alt.toObject());
            if (altTypes.isEmpty()) return {};   // (an alternative of any type)
            all << altTypes;
            if (std::any_of(altTypes.cbegin(), altTypes.cend(), [&](const QString& t) { return isOfType(value, t); })) {
                object = alt.toObject();
                one = true;
                break;
            }
        }
        if (!one) return tr("%1 is %2, not %3").arg(where, typeWords(all), whatItIs(value));
    } else if (!types.isEmpty() && std::none_of(types.cbegin(), types.cend(), [&](const QString& t) { return isOfType(value, t); })) {
        return tr("%1 is %2, not %3").arg(where, typeWords(types), whatItIs(value));
    }
    if (value.isArray()) {
        const QJsonObject items = object.value(QLatin1String("items")).toObject();
        if (items.isEmpty()) return {};
        const QJsonArray a = value.toArray();
        for (int i = 0; i < a.size(); ++i)
            if (QString u = wrongTypeIn(a.at(i), items, QStringLiteral("%1[%2]").arg(where).arg(i)); !u.isEmpty()) return u;
    } else if (value.isObject()) {
        const QJsonObject fields = object.value(QLatin1String("properties")).toObject();
        const QJsonValue more = object.value(QLatin1String("additionalProperties"));
        const QJsonObject o = value.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            const QJsonObject field = fields.contains(it.key()) ? fields.value(it.key()).toObject() : more.toObject();
            if (field.isEmpty()) continue;
            if (QString u = wrongTypeIn(it.value(), field, where + QLatin1Char('.') + it.key()); !u.isEmpty()) return u;
        }
    }
    return {};
}

} // namespace

QString QucsControl::wrongTypes(const QString& tool, const QJsonObject& args) const
{
    const auto known = a_details.constFind(tool);
    if (known == a_details.constEnd()) return {};
    const QJsonObject fields = known->value(QLatin1String("inputSchema")).toObject().value(QLatin1String("properties")).toObject();
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (!fields.contains(it.key())) continue;
        if (const QString wrong = wrongTypeIn(it.value(), fields.value(it.key()).toObject(), it.key()); !wrong.isEmpty())
            return tr("%1: %2. Nothing was done (it would have been read as its default, not refused).").arg(tool, wrong);
    }
    return {};
}

QString QucsControl::unknownArguments(const QString& tool, const QJsonObject& args) const
{
    const auto known = a_details.constFind(tool);
    if (known == a_details.constEnd()) return {};
    // (A painting's fields are its type's: add_painting and edit_painting
    // take them all, and refuse those its type has not, themselves.)
    if (known->value(QLatin1String("inputSchema")).toObject().value(QLatin1String("additionalProperties")).toBool()) return {};
    const QJsonObject fields = known->value(QLatin1String("inputSchema")).toObject().value(QLatin1String("properties")).toObject();
    QStringList unknown;
    for (auto it = args.begin(); it != args.end(); ++it)
        // (A conversation pinned to a schematic names it to get_state too.)
        if (!fields.contains(it.key()) && !(it.key() == QLatin1String("path") && tool == QLatin1String("get_state"))) unknown << it.key();
    if (unknown.isEmpty()) {
        // Inside an argument: a part of set_schematic's JSON form, a wire,
        // a trace of add_diagram, an axis - left out as silently.
        for (auto it = args.begin(); it != args.end(); ++it)
            if (const QString inside = unknownIn(it.value(), fields.value(it.key()).toObject(), it.key()); !inside.isEmpty())
                return tr("%1: %2 Nothing was done (it would be left out, not refused).").arg(tool, inside);
        return {};
    }
    QStringList takes = fields.keys();
    // (The one meant, when a letter or two is off, or one names the other.)
    const QStringList near = nearNamesOf(unknown, takes);
    return tr("%1 takes no %2: nothing was done (an argument it does not know would be left out, not refused). It takes %3.%4")
        .arg(tool, unknown.join(QStringLiteral(", ")), takes.isEmpty() ? tr("no arguments") : takes.join(QStringLiteral(", ")),
             near.isEmpty() ? QString() : tr(" Meant %1?").arg(near.join(tr(" or "))));
}

QJsonObject QucsControl::call(const QString& tool, const QJsonObject& args, const Done& done, bool& async)
{
    // The files this call writes, one step for undo's 'files' (a call
    // inside it is part of it).
    struct Depth {
        QucsControl* control;
        Depth(QucsControl* c, const QString& tool) : control(c)
        {
            if (control->a_callDepth++ == 0) control->openFileStep(tool);
        }
        ~Depth()
        {
            if (--control->a_callDepth == 0) control->closeFileStep();
        }
    } depth(this, tool);
    // An argument the tool does not take: refused, with those it does - it
    // was left out, and the call done without it (a text box's tip, a
    // misspelt width: the box came out as if neither was given).
    if (const QString unknown = unknownArguments(tool, args); !unknown.isEmpty()) return errorResult(unknown);
    // An argument of another JSON type than the tool takes ('straighten':
    // 1, 'hold' one object for a list): refused - it was read as its
    // default, and the call done as if it was not given.
    if (const QString wrong = wrongTypes(tool, args); !wrong.isEmpty()) return errorResult(wrong);
    // A dialog waiting for an answer - the user's, editing a part or a
    // diagram, or one Claude opened - holds on to what it edits: a change
    // under it (an undo rebuilds the whole schematic) freed what it held.
    // Only what looks, and what answers the dialog, runs until it closes.
    static const QSet<QString> whileADialogWaits{
        QStringLiteral("get_state"), QStringLiteral("get_schematic"), QStringLiteral("screenshot"),
        QStringLiteral("list_component_types"), QStringLiteral("list_actions"), QStringLiteral("get_dialog"),
        QStringLiteral("set_dialog"), QStringLiteral("get_netlist"), QStringLiteral("get_dataset"), QStringLiteral("get_ui"),
        QStringLiteral("describe_component_type"), QStringLiteral("describe_format"), QStringLiteral("batch"),
        QStringLiteral("list_documents"), QStringLiteral("check_schematic"), QStringLiteral("read_pdf"), QStringLiteral("get_text"),
        QStringLiteral("find_library_component"), QStringLiteral("describe_part"), QStringLiteral("undo_history"),
        QStringLiteral("ngspice_commands"), QStringLiteral("wait_for"), QStringLiteral("simulation_status"), QStringLiteral("read_help"),
        QStringLiteral("list_libraries")};
    // (send_input with 'target' dialog: the dialog's own clicks and keys.)
    const bool intoTheDialog = tool == QLatin1String("send_input")
                               && args.value(QLatin1String("target")).toString().trimmed().compare(QLatin1String("dialog"), Qt::CaseInsensitive) == 0;
    if (QWidget* dialog = QApplication::activeModalWidget(); dialog != nullptr && !whileADialogWaits.contains(tool) && !intoTheDialog)
        return errorResult(tr("“%1” is open in Qucs-S and waits for an answer: %2 waits until it is closed (get_dialog "
                              "reads it, set_dialog answers it - or ask the user to).")
                               .arg(dialog->windowTitle().isEmpty() ? QString::fromLatin1(dialog->metaObject()->className())
                                                                    : dialog->windowTitle(),
                                    tool));
    // "selection": true - what the user selected, instead of names.
    static const QSet<QString> takeSelection{QStringLiteral("move"), QStringLiteral("delete"), QStringLiteral("create_subcircuit"),
                                             QStringLiteral("get_schematic"), QStringLiteral("add_painting")};
    if (takeSelection.contains(tool) && (args.value(QLatin1String("selection")).toBool()
                                         || args.value(QLatin1String("around")).toString() == QLatin1String("selection"))) {
        QString error;
        const QJsonObject resolved = withSelection(tool, args, &error);
        if (!error.isEmpty()) return errorResult(error);
        return call(tool, resolved, done, async);
    }
    // A data display: 'document': "data_display" on add_diagram (the
    // schematic's), or a .dpl in 'path' that is not open - opened, or made
    // for its schematic, as new_document's data_display does.
    static const QSet<QString> onDataDisplays{QStringLiteral("add_diagram"), QStringLiteral("edit_diagram"), QStringLiteral("add_trace"),
                                              QStringLiteral("edit_trace"), QStringLiteral("add_marker"), QStringLiteral("edit_marker"),
                                              QStringLiteral("delete_marker"), QStringLiteral("add_painting"), QStringLiteral("edit_painting")};
    if (onDataDisplays.contains(tool) && !args.value(QLatin1String("preview")).toBool()
        && (args.contains(QLatin1String("document")) || args.value(QLatin1String("path")).toString().trimmed().endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive))) {
        QString error;
        QJsonObject resolved = args;
        if (!toDataDisplay(&resolved, &error)) return errorResult(error);
        if (resolved != args) return call(tool, resolved, done, async);
    }
    // 'preview': what a change would do - every open schematic put back
    // after, nothing kept.
    if (args.value(QLatin1String("preview")).toBool()) {
        if (!previewTools().contains(tool))
            return errorResult(tr("%1 has no preview: 'preview' is for the tools that change schematics (not files, "
                                  "simulations or what only looks).").arg(tool));
        return preview(tool, args, done, async);
    }
    if (tool == QLatin1String("diff")) return diffTool(args);
    if (tool == QLatin1String("run_script")) return runScript(args);
    if (tool == QLatin1String("describe_tool")) {
        const QString name = args.value(QLatin1String("name")).toString().trimmed();
        if (name.isEmpty()) {
            QJsonArray list;
            for (const QJsonValue& v : std::as_const(a_tools))
                // (Each tool's summary: the tools not in every turn are listed
                // with their whole description, which was all this gave.)
                list.append(QJsonObject{{QStringLiteral("name"), v.toObject().value(QLatin1String("name"))},
                                        {QStringLiteral("summary"), a_summaries.value(v.toObject().value(QLatin1String("name")).toString(),
                                                                                      v.toObject().value(QLatin1String("description")).toString())}});
            return jsonResult(QJsonObject{{QStringLiteral("tools"), list}});
        }
        if (!a_details.contains(name)) return errorResult(tr("There is no tool %1 (describe_tool without 'name' lists them).").arg(name));
        QJsonObject whole = a_details.value(name);
        for (const QJsonValue& v : std::as_const(a_tools))
            if (v.toObject().value(QLatin1String("name")).toString() == name) {
                whole.insert(QStringLiteral("inputSchema"), v.toObject().value(QLatin1String("inputSchema")));
                whole.insert(QStringLiteral("annotations"), v.toObject().value(QLatin1String("annotations")));
                whole.insert(QStringLiteral("summary"), a_summaries.value(name, v.toObject().value(QLatin1String("description")).toString()));
            }
        return jsonResult(whole);
    }
    if (tool == QLatin1String("get_state")) return getState(args);
    if (tool == QLatin1String("open_document")) return openDocument(args);
    if (tool == QLatin1String("new_document")) return newDocument(args);
    if (tool == QLatin1String("show_document")) return showDocument(args);
    if (tool == QLatin1String("save_document")) return saveDocument(args);
    if (tool == QLatin1String("close_document")) return closeDocument(args);
    if (tool == QLatin1String("get_schematic")) return getSchematic(args);
    if (tool == QLatin1String("set_schematic")) return setSchematic(args);
    if (tool == QLatin1String("add_component")) return addComponent(args);
    if (tool == QLatin1String("edit_component")) return editComponent(args);
    if (tool == QLatin1String("replace_component")) return replaceComponent(args);
    if (tool == QLatin1String("delete")) return remove(args);
    if (tool == QLatin1String("move")) return moveGroup(args);
    if (tool == QLatin1String("arrange")) return arrange(args);
    if (tool == QLatin1String("add_analysis")) return addAnalysis(args);
    if (tool == QLatin1String("create_subcircuit")) return createSubcircuit(args);
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
    if (tool == QLatin1String("get_ui")) return getUi(args);
    if (tool == QLatin1String("get_netlist")) {
        QJsonObject looking = args;
        looking.remove(QStringLiteral("save_as"));   // (export_netlist writes)
        return getNetlist(looking);
    }
    if (tool == QLatin1String("export_netlist")) {
        if (args.value(QLatin1String("save_as")).toString().trimmed().isEmpty())
            return errorResult(tr("'save_as' names the file to write."));
        return getNetlist(args);
    }
    if (tool == QLatin1String("get_dataset")) return getDataset(args);
    if (tool == QLatin1String("reload_data")) return reloadData(args);
    if (tool == QLatin1String("add_diagram")) return addDiagram(args);
    if (tool == QLatin1String("edit_diagram")) return editDiagram(args);
    if (tool == QLatin1String("add_trace")) return addTrace(args);
    if (tool == QLatin1String("edit_trace")) return editTrace(args);
    if (tool == QLatin1String("add_marker")) return addMarker(args);
    if (tool == QLatin1String("describe_format")) return describeFormat(args);
    if (tool == QLatin1String("ngspice_commands")) return ngspiceCommands(args);
    if (tool == QLatin1String("move_to_pane")) return moveToPane(args);
    if (tool == QLatin1String("edit_marker")) return editMarker(args);
    if (tool == QLatin1String("delete_marker")) return deleteMarker(args);
    if (tool == QLatin1String("rename_net")) return renameNet(args);
    if (tool == QLatin1String("describe_component_type")) return describeComponentType(args);
    if (tool == QLatin1String("add_painting")) return addPainting(args);
    if (tool == QLatin1String("edit_painting")) return editPainting(args);
    if (tool == QLatin1String("list_documents")) return listDocuments(args);
    if (tool == QLatin1String("check_schematic")) return checkSchematic(args);
    if (tool == QLatin1String("export_image")) return exportImage(args);
    if (tool == QLatin1String("set_simulator")) return setSimulator(args);
    if (tool == QLatin1String("read_pdf")) return readPdf(args);
    if (tool == QLatin1String("get_text")) return getText(args);
    if (tool == QLatin1String("edit_text")) return editText(args);
    if (tool == QLatin1String("goto_line")) return gotoLine(args);
    if (tool == QLatin1String("undo_history")) return undoHistory(args);
    if (tool == QLatin1String("new_project")) return newProject(args);
    if (tool == QLatin1String("open_project")) return openProject(args);
    if (tool == QLatin1String("copy_document")) return copyDocument(args);
    if (tool == QLatin1String("clean_scratch")) return cleanScratch(args);
    if (tool == QLatin1String("rename_file")) return renameFile(args);
    if (tool == QLatin1String("trash_file")) return trashFile(args);
    if (tool == QLatin1String("make_symbol")) return makeSymbol(args);
    if (tool == QLatin1String("set_subcircuit_parameters")) return setSubcircuitParameters(args);
    if (tool == QLatin1String("import_netlist")) return importNetlist(args);
    if (tool == QLatin1String("import_data")) return importData(args);
    if (tool == QLatin1String("export_data")) return exportData(args);
    if (tool == QLatin1String("find_library_component")) return findLibraryComponent(args);
    if (tool == QLatin1String("describe_part")) return describePart(args);
    if (tool == QLatin1String("list_libraries")) return listLibraries(args);
    if (tool == QLatin1String("create_library")) return createLibrary(args);
    if (tool == QLatin1String("import_library")) return importLibrary(args);
    if (tool == QLatin1String("simulation_status")) return simulationStatus(args);
    if (tool == QLatin1String("read_help")) return readHelp(args);
    if (tool == QLatin1String("synthesize_filter")) return synthesizeFilter(args);
    if (tool == QLatin1String("synthesize_attenuator")) return synthesizeAttenuator(args);
    if (tool == QLatin1String("synthesize_matching")) return synthesizeMatching(args);
    if (tool == QLatin1String("synthesize_power_combiner")) return synthesizePowerCombiner(args);
    if (tool == QLatin1String("line_calc")) return lineCalc(args);
    if (tool == QLatin1String("receiver_budget")) return receiverBudget(args);
    async = true;
    if (tool == QLatin1String("batch")) runBatch(args, done);
    else if (tool == QLatin1String("trigger_action")) triggerAction(args, done);
    else if (tool == QLatin1String("set_dialog")) setDialog(args, done);
    else if (tool == QLatin1String("set_ui")) setUi(args, done);
    else if (tool == QLatin1String("console")) console(args, done);
    else if (tool == QLatin1String("get_settings")) getSettings(args, done);
    else if (tool == QLatin1String("set_settings")) setSettings(args, done);
    else if (tool == QLatin1String("context_menu")) contextMenu(args, done);
    else if (tool == QLatin1String("simulate")) simulate(args, done);
    else if (tool == QLatin1String("stop_simulation")) stopSimulation(args, done);
    else if (tool == QLatin1String("wait_for")) waitFor(args, done);
    else if (tool == QLatin1String("send_input")) sendInput(args, done);
    else if (tool == QLatin1String("build_verilog_a")) buildVerilogA(args, done);
    else if (tool == QLatin1String("tune")) tune(args, done);
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
    if (pane == nullptr) return QString();
    // A file's whole name, whatever its tab shows of it (a long one cut).
    if (!doc->getDocName().isEmpty()) return QFileInfo(doc->getDocName()).fileName();
    const QString title = cleanText(pane->tabText(pane->indexOf(w)));
    // Two without a file of one title (an untitled schematic and an
    // untitled text): the second "untitled (2)", each named apart.
    int before = 0;
    for (QucsDoc* other : a_app->allDocuments()) {
        if (other == doc) break;
        QWidget* ow = QucsApp::documentWidget(other);
        const QTabWidget* op = a_app->paneOf(ow);
        if (other->getDocName().isEmpty() && op != nullptr && cleanText(op->tabText(op->indexOf(ow))) == title) ++before;
    }
    return before == 0 ? title : QStringLiteral("%1 (%2)").arg(title).arg(before + 1);
}

QString QucsControl::shownTitleOf(QucsDoc* doc) const
{
    QWidget* w = QucsApp::documentWidget(doc);
    const QTabWidget* pane = a_app->paneOf(w);
    // (An & of a file's name is written && on its tab.)
    return pane == nullptr ? QString() : pane->tabText(pane->indexOf(w)).replace(QStringLiteral("&&"), QStringLiteral("&"));
}

QJsonObject QucsControl::withSelection(const QString& tool, const QJsonObject& args, QString* error) const
{
    Schematic* sch = schematic(args, error, false);
    if (sch == nullptr) return {};
    error->clear();
    QJsonArray names, diagrams, paintings, wires;
    QRect bounds;
    // Each part by its ref - a ground's name is empty: GND#17, which the
    // tools take ("There is no component ." for every ground selected).
    QHash<const Component*, QString> refs;
    for (Component* c : sch->a_DocComps)
        if (c->isSelected) {
            if (refs.isEmpty()) refs = PartIndex::refs(sch);
            names.append(refs.value(c));
            bounds |= c->boundingRect();
        }
    for (Wire* w : sch->a_DocWires)
        if (w->isSelected) {
            wires.append(QJsonArray{w->x1, w->y1, w->x2, w->y2});
            bounds |= w->boundingRect();
        }
    int n = 0;
    for (Diagram* d : sch->a_DocDiags) {
        ++n;
        if (!d->isSelected) continue;
        diagrams.append(n);
        int x1, y1, x2, y2;
        d->Bounding(x1, y1, x2, y2);
        bounds |= QRect(QPoint(x1, y1), QPoint(x2, y2));
    }
    n = 0;
    for (Painting* p : sch->a_DocPaints) {
        ++n;
        if (!p->isSelected) continue;
        paintings.append(n);
        bounds |= p->boundingRect();
    }
    if (names.isEmpty() && diagrams.isEmpty() && paintings.isEmpty() && wires.isEmpty()) {
        *error = tr("Nothing is selected in %1 (the user selects with the mouse; select selects by name).").arg(titleOf(sch));
        return {};
    }
    QJsonObject a = args;   // ('path' as given: the same document)
    a.remove(QStringLiteral("selection"));
    if (tool == QLatin1String("add_painting")) {
        // Around the selection: a box about it, a text above it, an arrow
        // to it, a brace beside it, a dimension under it - what is not given.
        a.remove(QStringLiteral("around"));
        const QString type = args.value(QLatin1String("type")).toString();
        const QRect r = bounds.adjusted(-10, -10, 10, 10);
        const auto give = [&a](const char* key, const QJsonValue& v) {
            if (!a.contains(QLatin1String(key))) a.insert(QLatin1String(key), v);
        };
        if (type == QLatin1String("rectangle") || type == QLatin1String("rounded_rectangle") || type == QLatin1String("ellipse")) {
            give("x", r.left());
            give("y", r.top());
            give("width", r.width());
            give("height", r.height());
        } else if (type == QLatin1String("arrow") || type == QLatin1String("line")) {
            give("to", QJsonArray{r.center().x(), r.top()});
            give("from", QJsonArray{r.center().x() + 60, r.top() - 60});
        } else if (type == QLatin1String("brace")) {
            give("x", r.right() + 10);
            give("y", r.top());
            give("width", 20);
            give("height", r.height());
        } else if (type == QLatin1String("dimension")) {
            give("from", QJsonArray{r.left(), r.bottom() + 30});
            give("to", QJsonArray{r.right(), r.bottom() + 30});
        } else {
            give("x", r.left());
            give("y", r.top() - 30);
        }
        return a;
    }
    if (tool == QLatin1String("get_schematic")) {
        if (!names.isEmpty()) a.insert(QStringLiteral("components"), names);
        else a.insert(QStringLiteral("region"), QJsonArray{bounds.left(), bounds.top(), bounds.right(), bounds.bottom()});
        return a;
    }
    a.insert(QStringLiteral("names"), names);
    if (tool == QLatin1String("move") || tool == QLatin1String("delete")) {
        if (!diagrams.isEmpty()) a.insert(QStringLiteral("diagrams"), diagrams);
        if (!paintings.isEmpty()) a.insert(QStringLiteral("paintings"), paintings);
    }
    if (tool == QLatin1String("delete") && !wires.isEmpty()) a.insert(QStringLiteral("wires"), wires);
    return a;
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
    // As its tab shows it, a long name cut ("a_very_long_na….sch").
    for (QucsDoc* doc : a_app->allDocuments())
        if (!doc->getDocName().isEmpty() && shownTitleOf(doc) == path) return doc;
    // A file's name alone (amp.sch), when one open document has it.
    if (!path.contains(QLatin1Char('/')) && !path.contains(QLatin1Char('\\'))) {
        QList<QucsDoc*> named;
        for (QucsDoc* doc : a_app->allDocuments())
            if (!doc->getDocName().isEmpty() && QFileInfo(doc->getDocName()).fileName().compare(path, Qt::CaseInsensitive) == 0) named << doc;
        if (named.size() == 1) return named.first();
        if (named.size() > 1) {
            QStringList paths;
            for (QucsDoc* doc : std::as_const(named)) paths << QDir::toNativeSeparators(doc->getDocName());
            *error = tr("%1 names %2 open documents: give its path (%3).").arg(path).arg(named.size()).arg(paths.join(QStringLiteral(", ")));
            return nullptr;
        }
    }
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
        if (sch->getIsSymbolOnly()) {
            *error = tr("%1 is a symbol file: it has only a symbol, which add_painting and edit_painting change.").arg(titleOf(doc));
            return nullptr;
        }
        // The change is to the schematic: shown again, as File > Edit
        // Schematic (F9) shows it, and said.
        a_app->showDocument(sch);
        QMetaObject::invokeMethod(a_app, "slotSymbolEdit", Qt::DirectConnection);
        if (sch->getSymbolMode()) {
            *error = tr("%1 shows its symbol and could not be switched back to its schematic (File > Edit Schematic, F9).")
                         .arg(titleOf(doc));
            return nullptr;
        }
        a_callNotes << tr("%1 showed its symbol: it shows its schematic again, where this change is made (File > Edit "
                          "Schematic, F9, switches between the two; the painting tools take 'symbol': true for the symbol).")
                           .arg(titleOf(doc));
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

QJsonObject QucsControl::moveToPane(const QJsonObject& args)
{
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    QWidget* w = QucsApp::documentWidget(doc);
    ContextMenuTabWidget* from = a_app->paneOf(w);
    if (from == nullptr) return errorResult(tr("%1 is in no pane.").arg(titleOf(doc)));
    const QJsonValue where = args.value(QLatin1String("pane"));
    ContextMenuTabWidget* to = nullptr;
    if (where.isDouble()) {
        const QList<ContextMenuTabWidget*> all = a_app->panes();
        // (1.5 was read as 0: "There is no pane 0".)
        if (where.toDouble() != std::floor(where.toDouble()) || std::abs(where.toDouble()) > 1e6)
            return errorResult(tr("There is no pane %1: they are numbered 1 to %2 (get_state lists them).").arg(where.toDouble()).arg(all.size()));
        const int n = int(where.toDouble());
        if (n < 1 || n > all.size()) return errorResult(tr("There is no pane %1: they are numbered 1 to %2 (get_state lists them).").arg(n).arg(all.size()));
        to = all.at(n - 1);
        if (to == from) return textResult(tr("%1 is in pane %2 already.").arg(titleOf(doc)).arg(n));
    } else {
        const QString side = where.toString();
        if (side != QLatin1String("right") && side != QLatin1String("below"))
            return errorResult(tr("'pane' is a pane's number, or right or below: a new pane beside the document's."));
        // (Moved out of a pane it is alone in, the pane would go again.)
        if (from->count() < 2)
            return errorResult(tr("%1 is the only document in its pane: moved to a new pane, it would leave that one empty, "
                                  "and it would close. Open another document beside it first, or move another one.").arg(titleOf(doc)));
        a_app->setActivePane(from);
        if (side == QLatin1String("right")) {
            if (!a_app->canSplitRight()) return errorResult(tr("There is a pane to the right already (at most two a row): give its number."));
            a_app->slotSplitPaneRight();
        } else {
            if (!a_app->canSplitDown()) return errorResult(tr("There is a row below already (at most two rows): give a pane's number."));
            a_app->slotSplitPaneDown();
        }
        to = a_app->activePane();   // the new one, with a placeholder the document replaces
    }
    a_app->moveDocument(w, to);
    a_app->showDocument(w);
    return getState(QJsonObject());
}

QJsonObject QucsControl::getState(const QJsonObject& args)
{
    QJsonArray docs;
    QucsDoc* front = a_app->DocumentTab->count() > 0 ? a_app->getDoc() : nullptr;
    // The document of a conversation pinned to one (forDocument()).
    const QString pinned = args.value(QLatin1String("path")).toString().trimmed();
    bool pinnedOpen = false;
    const QList<ContextMenuTabWidget*> panes = a_app->panes();
    // The workspace's projects, by their folders as they really are (a
    // linked one where it points): each document's is the one it is in.
    struct ProjectFolder {
        QString name, folder, real;
        bool linked = false;
    };
    QList<ProjectFolder> projectFolders;
    const QString workspace = QucsSettings.qucsWorkspaceDir.absolutePath();
    for (const QFileInfo& fi : QDir(workspace).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (qucs_s::workspace::isProjectFolder(fi.filePath()))
            projectFolders.append({qucs_s::workspace::projectName(fi.fileName()), fi.absoluteFilePath(), fi.canonicalFilePath(), fi.isSymLink()});
    const QString openFolder = a_app->ProjName.isEmpty() ? QString() : QFileInfo(QucsSettings.QucsWorkDir.absolutePath()).canonicalFilePath();
    const auto projectOf = [&projectFolders](const QString& file) -> const ProjectFolder* {
        const QString real = QFileInfo(file).canonicalFilePath();
        if (real.isEmpty()) return nullptr;
        for (const ProjectFolder& p : projectFolders)
            if (!p.real.isEmpty() && real.startsWith(p.real + QLatin1Char('/'))) return &p;
        return nullptr;
    };
    for (QucsDoc* doc : a_app->allDocuments()) {
        QJsonObject d{{QStringLiteral("title"), titleOf(doc)},
                      {QStringLiteral("path"), doc->getDocName()},
                      {QStringLiteral("kind"), kindOf(doc)},
                      {QStringLiteral("unsaved changes"), doc->getDocChanged()},
                      {QStringLiteral("in front"), doc == front},
                      {QStringLiteral("revision"), qint64(doc->revision())}};
        if (panes.size() > 1)
            d.insert(QStringLiteral("pane"), int(panes.indexOf(a_app->paneOf(QucsApp::documentWidget(doc)))) + 1);
        // Who changed it last: "you" only for this conversation's calls.
        if (!doc->recentEdits().isEmpty()) {
            const QucsDoc::Edit& e = doc->recentEdits().last();
            d.insert(QStringLiteral("last edit"), QJsonObject{{QStringLiteral("by"), whoMade(e.by, a_callers.isEmpty() ? 0 : a_callers.last())},
                                                               {QStringLiteral("at"), e.at.toString(Qt::ISODate)}});
        }
        if (const QDateTime written = datasetWritten(doc); written.isValid())
            d.insert(QStringLiteral("dataset written"), written.toString(Qt::ISODate));
        // The project its file is in - another than the open one said so:
        // relative paths, Scratch and Verilog-A are the open project's.
        if (const ProjectFolder* p = doc->getDocName().isEmpty() ? nullptr : projectOf(doc->getDocName())) {
            d.insert(QStringLiteral("project"), p->name);
            if (p->real != openFolder)
                d.insert(QStringLiteral("project not open"), openFolder.isEmpty() ? tr("no project is open: open_project opens %1").arg(p->name)
                                                                                  : tr("%1 is open, not %2: relative paths, Scratch and Verilog-A are "
                                                                                       "%1's").arg(a_app->ProjName, p->name));
        }
        if (!pinned.isEmpty() && !doc->getDocName().isEmpty() && sameFile(doc->getDocName(), absolute(pinned))) {
            d.insert(QStringLiteral("this conversation's document"), true);
            pinnedOpen = true;
        }
        if (auto* sch = dynamic_cast<Schematic*>(doc)) {
            d.insert(QStringLiteral("components"), int(sch->a_DocComps.size()));
            d.insert(QStringLiteral("wires"), int(sch->a_DocWires.size()));
            d.insert(QStringLiteral("showing its symbol"), sch->getSymbolMode());
            // (Each by its ref, as select takes it: a ground's name is "".)
            QJsonArray selected;
            QHash<const Component*, QString> refs;
            for (Component* c : sch->a_DocComps)
                if (c->isSelected) {
                    if (refs.isEmpty()) refs = PartIndex::refs(sch);
                    selected.append(refs.value(c));
                }
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
    // The panes: where each is, what it holds, which is active.
    QJsonArray paneList;
    for (int i = 0; i < panes.size(); ++i) {
        ContextMenuTabWidget* pane = panes.at(i);
        const QPoint cell = a_app->paneCell(pane);
        const QRect rect = a_app->paneRect(pane);
        QJsonArray held;
        for (int t = 0; t < pane->count(); ++t)
            if (QucsDoc* doc = QucsApp::docIn(pane->widget(t))) held.append(titleOf(doc));
        QJsonObject p{{QStringLiteral("pane"), i + 1},
                      {QStringLiteral("row"), cell.y() + 1},
                      {QStringLiteral("column"), cell.x() + 1},
                      {QStringLiteral("rectangle"), QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()}},
                      {QStringLiteral("documents"), held},
                      {QStringLiteral("active"), pane == a_app->activePane()}};
        if (QucsDoc* current = QucsApp::docIn(pane->currentWidget())) p.insert(QStringLiteral("in front"), titleOf(current));
        paneList.append(p);
    }
    QJsonObject state{{QStringLiteral("documents"), docs},
                      {QStringLiteral("panes"), paneList},
                      {QStringLiteral("workspace"), QucsSettings.qucsWorkspaceDir.absolutePath()},
                      {QStringLiteral("simulator"), simulator},
                      {QStringLiteral("simulation running"),
                       a_app->simulationConsole() != nullptr && a_app->simulationConsole()->isRunning()}};
    if (QWidget* dialog = openDialog())
        state.insert(QStringLiteral("dialog open"), dialog->windowTitle().isEmpty() ? dialog->metaObject()->className() : dialog->windowTitle());
    if (!pinned.isEmpty())
        state.insert(QStringLiteral("this conversation works on"),
                     pinnedOpen ? pinned : tr("%1 (not open: open_document opens it)").arg(pinned));
    // The open project, its name and folder - null when none is (told from
    // an older build, which has no such field).
    state.insert(QStringLiteral("project"), a_app->ProjName.isEmpty()
                                                ? QJsonValue(QJsonValue::Null)
                                                : QJsonValue(QJsonObject{{QStringLiteral("name"), a_app->ProjName},
                                                                         {QStringLiteral("folder"), QDir::toNativeSeparators(QucsSettings.QucsWorkDir.absolutePath())}}));
    // Every project of the workspace: one is open at a time.
    QJsonArray projects;
    for (const ProjectFolder& p : std::as_const(projectFolders)) {
        if (projects.size() >= 100) break;
        QJsonObject o{{QStringLiteral("name"), p.name}, {QStringLiteral("folder"), QDir::toNativeSeparators(p.folder)}};
        if (p.real == openFolder && !openFolder.isEmpty()) o.insert(QStringLiteral("open"), true);
        if (p.linked) o.insert(QStringLiteral("linked"), true);
        projects.append(o);
    }
    state.insert(QStringLiteral("projects"), projects);
    // Simulations followed in the background, while they run.
    QJsonArray following;
    for (const SimRun& r : std::as_const(a_simRuns))
        if (!r.ended.isValid()) following.append(QJsonObject{{QStringLiteral("id"), r.id}, {QStringLiteral("schematic"), QFileInfo(r.schematic).fileName()}});
    if (!following.isEmpty()) state.insert(QStringLiteral("simulations followed"), following);
    return jsonResult(state);
}

QJsonObject QucsControl::openDocument(const QJsonObject& args)
{
    const QString path = absolute(args.value(QLatin1String("path")).toString().trimmed());
    if (args.value(QLatin1String("path")).toString().trimmed().isEmpty()) return errorResult(tr("Which file? ('path')"));
    if (!QFileInfo(path).isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(path)));
    bool wasOpen = false;
    for (QucsDoc* d : a_app->allDocuments()) wasOpen = wasOpen || (!d->getDocName().isEmpty() && sameFile(d->getDocName(), path));
    // Why it could not be opened, in the answer ("could not be opened." alone
    // said nothing): what the loader reported - a part it does not know,
    // and of a Verilog-A module beside it not built yet, how to build it.
    QStringList said;
    bool opened = false;
    {
        misc::ErrorCapture loading;
        opened = a_app->gotoPage(path, false, false);
        said = loading.errors();
    }
    if (!opened) {
        QStringList hints;
        const QString unknown = QObject::tr("Unknown component: %1").arg(QString());
        for (const QString& e : std::as_const(said)) {
            if (!e.startsWith(unknown)) continue;
            const QString part = e.mid(unknown.size()).trimmed();
            QString va = QFileInfo(path).dir().filePath(part + QStringLiteral(".va"));
            if (!QFileInfo::exists(va) && !a_app->ProjName.isEmpty()) va = QucsSettings.QucsWorkDir.filePath(part + QStringLiteral(".va"));
            hints << (QFileInfo::exists(va)
                          ? tr("%1 is the Verilog-A module of %2, not built yet: build it (build_verilog_a with that file), then "
                               "open this again").arg(part, QDir::toNativeSeparators(va))
                          : tr("%1 is no part this Qucs-S knows (a newer version's, or of a library or module not here)").arg(part));
        }
        QString text = tr("%1 could not be opened").arg(QDir::toNativeSeparators(path));
        if (!said.isEmpty()) text += QStringLiteral(": ") + onceEach(said).join(QStringLiteral("; "));
        if (!text.endsWith(QLatin1Char('.'))) text += QLatin1Char('.');
        if (!hints.isEmpty()) text += QLatin1Char(' ') + hints.join(QStringLiteral(". ")) + QLatin1Char('.');
        return errorResult(text);
    }
    for (const QString& e : std::as_const(said)) misc::reportError(e);   // (after the answer, as before)
    QucsDoc* doc = a_app->getDoc();
    closeUntouched(doc);
    QString text = tr("%1 is open, in front (%2).").arg(QDir::toNativeSeparators(path), doc != nullptr ? kindOf(doc) : QString());
    // (What the window says in a box - which would wait for no one here.)
    if (!QFileInfo(path).isWritable() && dynamic_cast<Schematic*>(doc) != nullptr)
        text += QLatin1Char(' ') + tr("It is read-only: it cannot be saved there, and its simulations cannot write their data "
                                      "beside it - copy_document makes a copy that can be.");
    else if (!QFileInfo(QFileInfo(path).absolutePath()).isWritable() && dynamic_cast<Schematic*>(doc) != nullptr)
        text += QLatin1Char(' ') + tr("Its folder is read-only: its simulations cannot write their data beside it, nor a data "
                                      "display be made there - copy_document makes a copy elsewhere.");
    // A component line with more values than its type has properties: read
    // positionally, so one too many in the middle shifted the rest - said
    // (a file of an older Qucs may simply carry values its type dropped).
    if (auto* sch = dynamic_cast<Schematic*>(doc); sch != nullptr && !wasOpen)
        if (const QStringList notes = sch->takeLoadNotes(); !notes.isEmpty())
            text += QLatin1Char(' ') + tr("Look: %1. If the file was written by hand, check those lines' values against "
                                          "describe_component_type's properties, in order; a file of an older Qucs may carry "
                                          "values its type has since dropped.").arg(notes.join(QStringLiteral("; ")));
    // A dataset and data display named after another file - a copy made
    // outside Qucs-S (the window asks on opening; nobody here to ask): said,
    // and named after this one when asked.
    if (auto* sch = dynamic_cast<Schematic*>(doc); sch != nullptr && !sch->getIsSymbolOnly()
                                                   && QFileInfo(path).suffix().compare(QLatin1String("sch"), Qt::CaseInsensitive) == 0) {
        const QString base = QFileInfo(path).completeBaseName();
        const QString ownSet = base + QStringLiteral(".dat"), ownDisplay = base + QStringLiteral(".dpl");
        const QString dataSet = sch->getDataSet(), dataDisplay = sch->getDataDisplay();
        // (Foo.sch's foo.dat is its own where the file system does not tell
        // case - macOS's, Windows's: found so when Foo.sch is also fOO.SCH.)
        QString swapped = QFileInfo(path).fileName();
        for (QChar& ch : swapped) ch = ch.isUpper() ? ch.toLower() : ch.toUpper();
        const Qt::CaseSensitivity cs = swapped != QFileInfo(path).fileName() && QFileInfo::exists(QFileInfo(path).absoluteDir().filePath(swapped))
                                           ? Qt::CaseInsensitive : Qt::CaseSensitive;
        if (dataSet.compare(ownSet, cs) != 0 || dataDisplay.compare(ownDisplay, cs) != 0) {
            if (args.value(QLatin1String("own_data_names")).toBool()) {
                sch->setDataSet(ownSet);
                sch->setDataDisplay(ownDisplay);
                sch->setChanged(true, true);
                text += QLatin1Char(' ') + tr("Its dataset and data display were %1 and %2: named after it now, %3 and %4 (unsaved) - "
                                              "its runs write its own results, and its diagrams read them.")
                                               .arg(dataSet, dataDisplay, ownSet, ownDisplay);
            } else {
                text += QLatin1Char(' ') + tr("Its dataset and data display are %1 and %2, not named after it (a copy made outside "
                                              "Qucs-S, or set so): its simulations write %3, and its diagrams, the data display and "
                                              "get_dataset read that - another schematic's results, when that one has some there. "
                                              "open_document with 'own_data_names' names them %4 and %5, as the window offers.")
                                               .arg(dataSet, dataDisplay,
                                                    QFileInfo(datasetFile(path, dataSet, QucsSettings.DefaultSimulator)).fileName(),
                                                    ownSet, ownDisplay);
            }
        }
    }
    return textResult(text);
}

// An untitled schematic nothing was ever done in - the one Qucs-S opens at
// start - is closed when a tool opens or makes a document beside it: not
// left behind as an empty tab, nor a second "untitled" to tell apart.
void QucsControl::closeUntouched(QucsDoc* keep)
{
    if (keep == nullptr) return;
    for (QucsDoc* doc : a_app->allDocuments()) {
        const auto* sch = dynamic_cast<Schematic*>(doc);
        if (sch == nullptr || doc == keep || !doc->getDocName().isEmpty() || doc->revision() > 0 || doc->getDocChanged()
            || !sch->a_DocComps.empty() || !sch->a_DocWires.empty() || !sch->a_DocPaints.empty() || !sch->a_DocDiags.empty())
            continue;
        QWidget* w = QucsApp::documentWidget(doc);
        a_app->showDocument(w);
        a_app->slotFileClose(a_app->DocumentTab->indexOf(w));
    }
    a_app->showDocument(QucsApp::documentWidget(keep));
}

bool QucsControl::toDataDisplay(QJsonObject* args, QString* error)
{
    const QString where = args->take(QStringLiteral("document")).toString().trimmed();
    if (!where.isEmpty() && where != QLatin1String("schematic") && where != QLatin1String("data_display")) {
        *error = tr("'document' is schematic or data_display (the schematic's .dpl).");
        return false;
    }
    QString path = args->value(QLatin1String("path")).toString().trimmed();
    QString schematicName;   // the schematic whose data display it is
    if (where == QLatin1String("data_display")) {
        Schematic* sch = schematic(*args, error, false);
        if (sch == nullptr) return false;
        if (sch->getDocName().endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive)) return true;   // (one already)
        if (sch->getDocName().isEmpty()) {
            *error = tr("%1 has no file yet, so no data display: save_document with 'as' first.").arg(titleOf(sch));
            return false;
        }
        schematicName = sch->getDocName();
        // (A Data Display that is no .dpl - a text, a schematic - is not
        // made: an empty file of it was left behind, and the call refused.)
        if (!sch->getDataDisplay().isEmpty() && !sch->getDataDisplay().endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive)) {
            *error = tr("%1's Data Display is %2, which is no data display (.dpl): File > Document Settings names it.")
                         .arg(titleOf(sch), sch->getDataDisplay());
            return false;
        }
        path = QFileInfo(schematicName).absoluteDir().filePath(sch->getDataDisplay().isEmpty()
                                                               ? QFileInfo(schematicName).completeBaseName() + QStringLiteral(".dpl")
                                                               : sch->getDataDisplay());
    } else if (!path.endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive)) {
        return true;   // (on the schematic, as always)
    }
    QString unused;
    const QJsonObject atPath{{QStringLiteral("path"), path}};
    if (QucsDoc* open = document(atPath, &unused)) {
        args->insert(QStringLiteral("path"), open->getDocName().isEmpty() ? path : open->getDocName());
        return true;
    }
    const QString file = absolute(path);
    if (QFileInfo(file).isFile()) {
        const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), file}});
        if (opened.value(QLatin1String("isError")).toBool()) {
            *error = tr("%1 could not be opened.").arg(QDir::toNativeSeparators(file));
            return false;
        }
    } else {
        // None yet: made for its schematic - the one named so beside it,
        // open (its data display is where its settings say).
        if (schematicName.isEmpty()) {
            const QString beside = QFileInfo(file).absoluteDir().filePath(QFileInfo(file).completeBaseName() + QStringLiteral(".sch"));
            QString why;
            if (Schematic* sch = schematic(QJsonObject{{QStringLiteral("path"), beside}}, &why, false)) schematicName = sch->getDocName();
            else {
                *error = tr("There is no data display %1 yet, and no open schematic %2 to make it for: open that schematic (or give "
                            "'document': \"data_display\" with the schematic as 'path').")
                             .arg(QDir::toNativeSeparators(file), QFileInfo(beside).fileName());
                return false;
            }
        }
        // (In a preview the file made for it goes again after.)
        written(file, std::nullopt);
        const QJsonObject made = newDocument(QJsonObject{{QStringLiteral("kind"), QStringLiteral("data_display")}, {QStringLiteral("path"), schematicName}});
        if (made.value(QLatin1String("isError")).toBool()) {
            *error = made.value(QLatin1String("content")).toArray().at(0).toObject().value(QLatin1String("text")).toString();
            return false;
        }
    }
    QucsDoc* now = document(atPath, &unused);
    if (now == nullptr) now = a_app->getDoc();
    if (now == nullptr || !now->getDocName().endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive)) {
        *error = tr("The data display %1 could not be opened.").arg(QDir::toNativeSeparators(file));
        return false;
    }
    args->insert(QStringLiteral("path"), now->getDocName());
    a_callNotes << tr("%1, the data display, is open for it (saved when it is).").arg(titleOf(now));
    return true;
}

QJsonObject QucsControl::newDocument(const QJsonObject& args)
{
    if (args.value(QLatin1String("kind")).toString() == QLatin1String("data_display")) {
        // A schematic's data display (its .dpl): opened, or made when it
        // has none - for the plots of a report, the schematic kept clean.
        QString error;
        Schematic* sch = schematic(args, &error, false);
        if (sch == nullptr) return errorResult(error);
        if (sch->getDocName().isEmpty()) return errorResult(tr("%1 has no file yet: save_document with 'as' first.").arg(titleOf(sch)));
        if (sch->getDocName().endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive)) return errorResult(tr("%1 is a data display.").arg(titleOf(sch)));
        const QString dpl = sch->getDataDisplay().isEmpty() ? QFileInfo(sch->getDocName()).completeBaseName() + QStringLiteral(".dpl")
                                                            : sch->getDataDisplay();
        if (!dpl.endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive))
            return errorResult(tr("%1's Data Display is %2, which is no data display (.dpl): File > Document Settings names it.")
                                   .arg(titleOf(sch), dpl));
        const bool existed = QFileInfo::exists(QFileInfo(sch->getDocName()).absoluteDir().filePath(dpl));
        QMetaObject::invokeMethod(a_app, "slotChangePage", Qt::DirectConnection, Q_ARG(QString, sch->getDocName()), Q_ARG(QString, dpl));
        QucsDoc* doc = a_app->getDoc();
        if (doc == nullptr || !doc->getDocName().endsWith(dpl)) return errorResult(tr("%1 could not be opened.").arg(dpl));
        return textResult(existed ? tr("%1, the data display of %2, is in front: add_diagram, add_painting and export_image "
                                       "work on it (path: %1); it shows %2's dataset.").arg(dpl, titleOf(sch))
                                  : tr("%1, a new data display of %2, is in front (saved when it is): add_diagram, add_painting "
                                       "and export_image work on it (path: %1); it shows %2's dataset.").arg(dpl, titleOf(sch)));
    }
    if (args.value(QLatin1String("kind")).toString() == QLatin1String("text")) a_app->slotTextNew();
    else a_app->slotFileNew();
    QucsDoc* doc = a_app->getDoc();
    closeUntouched(doc);
    QString text = tr("A new document is in front: %1.").arg(doc != nullptr ? titleOf(doc) : QString());
    if (dynamic_cast<Schematic*>(doc) != nullptr)
        text += QLatin1Char(' ') + tr("It has no file until save_document with 'as' gives it one; simulate and tune save it in the "
                                      "scratch folder first if it has none.");
    return textResult(text);
}

QJsonObject QucsControl::showDocument(const QJsonObject& args)
{
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    a_app->showDocument(QucsApp::documentWidget(doc));
    return textResult(tr("%1 is in front.").arg(titleOf(doc)));
}

namespace {

// A subcircuit's instances in the open schematics, as they are - to tell,
// once its file is saved (and Qucs-S has made them again from its new
// symbol), which were refreshed and whose pins no longer meet their wires.
struct Instance {
    Schematic* sch = nullptr;
    QString document;   // its title
    Component* part = nullptr;
    QString name;
    struct Pin {
        QPoint at;
        QString name;
        bool wired = false;
        QString net;       // its label's, if any
        QString initial;   // (and the label's initial value)
    };
    QList<Pin> pins;
    std::shared_ptr<const Nets> nets;   // its schematic's, as they were
};

QList<Instance::Pin> pinsOf(const Component* c)
{
    QList<Instance::Pin> pins;
    for (const Port* p : c->Ports) {
        Instance::Pin pin{QPoint(c->cx + p->x, c->cy + p->y), p->Name};
        if (p->Connection != nullptr) {
            pin.wired = p->Connection->conn_count() > 1 || p->Connection->hasLabel();
            if (p->Connection->hasLabel()) {
                pin.net = p->Connection->label()->Name;
                pin.initial = p->Connection->label()->initValue;
            }
        }
        pins << pin;
    }
    return pins;
}

// Those of \a file (a schematic's .sch) in the other open schematics.
QList<Instance> instancesOf(QucsApp* app, const QucsDoc* doc, const QString& file,
                            const std::function<QString(QucsDoc*)>& titleOf)
{
    QList<Instance> found;
    if (file.isEmpty() || !file.endsWith(QLatin1String(".sch"), Qt::CaseInsensitive)) return found;
    const QString fileName = QFileInfo(file).fileName();
    for (QucsDoc* other : app->allDocuments()) {
        auto* sch = dynamic_cast<Schematic*>(other);
        if (sch == nullptr || other == doc) continue;
        std::shared_ptr<const Nets> nets;
        for (Component* c : sch->a_DocComps) {
            if (c->Model != QLatin1String("Sub") || c->Props.isEmpty()) continue;
            const QString used = c->Props.front()->Value;
            if (used != file && used != fileName) continue;
            if (!nets) nets = std::make_shared<const Nets>(netsOf(sch, nullptr));
            found << Instance{sch, titleOf(sch), c, c->Name, pinsOf(c), nets};
        }
    }
    return found;
}

// Which of \a now each of \a was is: by name when the pins had names
// and each is there once, before and now (a port added or numbered anew
// shifts the numbers, not the names) - else by number. -1: gone.
QList<int> matchedPins(const QList<Instance::Pin>& was, const QList<Instance::Pin>& now)
{
    QHash<QString, int> named;
    bool byName = !was.isEmpty();
    for (int j = 0; j < now.size() && byName; ++j) {
        if (now.at(j).name.isEmpty() || named.contains(now.at(j).name)) byName = false;
        named.insert(now.at(j).name, j);
    }
    QSet<QString> seen;
    for (const Instance::Pin& p : was) {
        if (p.name.isEmpty() || seen.contains(p.name) || !named.contains(p.name)) byName = false;
        seen.insert(p.name);
    }
    QList<int> to;
    for (int i = 0; i < was.size(); ++i) to << (byName ? named.value(was.at(i).name) : i < now.size() ? i : -1);
    return to;
}

// A pin's key in netsOf.
QString pinKey(const Instance& in, int index)
{
    return QStringLiteral("%1.%2").arg(in.name).arg(index + 1);
}

// Each pin that moved off its wiring is joined to it again, one step to
// undo in its schematic: a net label on it alone - as create_subcircuit
// joins each pin to its net - goes along to where it is now (the label
// went with the node the pin left: a netlist of _net0), and a wire that
// ended on it is drawn on to it (as connect draws one: over no other pin
// or wire, joining those two nets alone). What was done, in words; \a
// stranded: the pins that could not be.
QStringList followPins(const QList<Instance>& before, QStringList* stranded)
{
    QStringList moved;
    QSet<Schematic*> changed;
    for (const Instance& was : before) {
        const auto& parts = was.sch->a_DocComps;
        if (std::find(parts.begin(), parts.end(), was.part) == parts.end()) continue;
        const QList<Instance::Pin> now = pinsOf(was.part);
        const QList<int> to = matchedPins(was.pins, now);
        for (int i = 0; i < was.pins.size(); ++i) {
            if (to.at(i) < 0) continue;
            const Instance::Pin& a = was.pins.at(i);
            const Instance::Pin& b = now.at(to.at(i));
            if (a.at == b.at || !a.wired || b.wired) continue;
            Node* left = was.sch->findNode(a.at);
            Node* pin = was.part->Ports.at(to.at(i))->Connection;
            if (pin == nullptr || pin->conn_count() != 1 || pin->hasLabel()) continue;
            const QString which = QStringLiteral("%1.%2%3").arg(was.name).arg(to.at(i) + 1)
                                      .arg(b.name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(b.name));
            // What is where it was: wires, other parts - or itself, another
            // of its pins there now (a port numbered anew: its label is the
            // moved pin's, and goes with it). A wire drawn on would join
            // what else is there: left, for the check of the nets to tell.
            const bool wires = left != nullptr && left != pin && !left->wires().empty();
            bool itself = false, others = false;
            if (left != nullptr && left != pin)
                for (const Component* c : left->components()) (c == was.part ? itself : others) = true;
            if (others || (wires && itself)) continue;
            if (wires) {
                // A wire ends where it was: on to where it is.
                const QStringList ends{QStringLiteral("at %1,%2").arg(a.at.x()).arg(a.at.y()),
                                       QStringLiteral("at %1,%2").arg(b.at.x()).arg(b.at.y())};
                const Nets nets = netsOf(was.sch, nullptr, {a.at, b.at});
                const auto check = [&] { return netChangesBeyond(nets, netsOf(was.sch, nullptr, {a.at, b.at}), ends); };
                QString why;
                if (!wireUp(was.sch, a.at, b.at, check, &why)) {
                    *stranded << QObject::tr("%1 in %2 could not be wired from %3, %4, where it was, to %5, %6: %7")
                                     .arg(which, was.document).arg(a.at.x()).arg(a.at.y()).arg(b.at.x()).arg(b.at.y()).arg(why);
                    continue;
                }
                changed.insert(was.sch);
                moved << QObject::tr("%1 wired on from %2, %3, where it was, to %4, %5 in %6").arg(which).arg(a.at.x()).arg(a.at.y())
                             .arg(b.at.x()).arg(b.at.y()).arg(was.document);
                continue;
            }
            // A label alone where it was (or nothing: the node went).
            if (a.net.isEmpty()) continue;
            if (left != nullptr && left != pin && left->hasLabel()) left->dropLabel();
            int xl = b.at.x() + 10, yl = b.at.y() - 20;
            was.sch->setOnGrid(xl, yl);
            pin->setName(a.net, a.initial, xl, yl);
            changed.insert(was.sch);
            moved << QObject::tr("%1's label %2 went with it to %3, %4 in %5").arg(which, a.net).arg(b.at.x()).arg(b.at.y()).arg(was.document);
        }
    }
    for (Schematic* sch : std::as_const(changed)) {
        sch->updateAllBoundingRect();
        sch->setChanged(true, true);
        sch->viewport()->update();
    }
    return moved;
}

// What became of them after the save: refreshed where, the pins that
// moved, what joined them to their nets again - and whether every net is
// as it was (a pin now on another net's wire, one no longer on its own).
QString refreshedInstances(const QList<Instance>& before)
{
    if (before.isEmpty()) return {};
    QStringList look;
    const QStringList followed = followPins(before, &look);
    QMap<QString, QStringList> where;   // a document: its instances
    QStringList moves;
    // Each schematic's nets as they were, its instances' pins by what they
    // are now (a pin numbered anew is the same pin).
    QHash<Schematic*, Nets> was;
    QHash<Schematic*, QStringList> changed;   // what became of its nets
    for (const Instance& in : before) {
        const auto& parts = in.sch->a_DocComps;
        if (std::find(parts.begin(), parts.end(), in.part) == parts.end()) continue;   // (gone meanwhile)
        const QList<Instance::Pin> now = pinsOf(in.part);
        const QList<int> to = matchedPins(in.pins, now);
        where[in.document] << in.name;
        if (!was.contains(in.sch)) was.insert(in.sch, *in.nets);
        Nets& nets = was[in.sch];
        QHash<QString, int> renamed;
        for (int i = 0; i < in.pins.size(); ++i) {
            const QString key = pinKey(in, i);
            if (!in.nets->netOf.contains(key)) continue;
            nets.netOf.remove(key);
            if (to.at(i) >= 0) renamed.insert(pinKey(in, to.at(i)), in.nets->netOf.value(key));
        }
        nets.netOf.insert(renamed);
        if (now.size() != in.pins.size())
            look << QObject::tr("%1 in %2 has %3 pins now, %4 before").arg(in.name, in.document).arg(now.size()).arg(in.pins.size());
        for (int i = 0; i < in.pins.size(); ++i) {
            if (to.at(i) < 0) continue;
            const Instance::Pin& a = in.pins.at(i);
            const Instance::Pin& b = now.at(to.at(i));
            const QString pin = QStringLiteral("%1.%2%3").arg(in.name).arg(to.at(i) + 1).arg(b.name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(b.name));
            if (to.at(i) != i) moves << QObject::tr("%1 in %2 was pin %3").arg(pin, in.document).arg(i + 1);
            if (a.at == b.at) continue;
            moves << QObject::tr("%1 in %2 from %3, %4 to %5, %6").arg(pin, in.document).arg(a.at.x()).arg(a.at.y()).arg(b.at.x()).arg(b.at.y());
        }
    }
    if (where.isEmpty()) return {};
    for (auto it = was.cbegin(); it != was.cend(); ++it) {
        Schematic* sch = it.key();
        QString title;
        for (const Instance& in : before)
            if (in.sch == sch) title = in.document;
        QStringList differ = netChangesBeyond(it.value(), netsOf(sch, nullptr), {});
        for (QString& d : differ)
            d.replace(QObject::tr(" would no longer be "), QObject::tr(" is no longer ")).replace(QObject::tr(" would be "), QObject::tr(" is "));
        // (Undo there does not put them back: the symbol's change is no
        // step of that schematic - it takes away the wires drawn on to the
        // pins, and leaves them on nothing.)
        if (!differ.isEmpty())
            look << QObject::tr("the nets of %1 are not as they were - %2. The new symbol is no undo step there (an undo there "
                                "takes away only the wires drawn on to the pins): delete the wires at those pins and connect each "
                                "pin by its name (connect takes SUB1.in), or put the ports back as they were in the subcircuit and "
                                "save it again").arg(title, differ.join(QStringLiteral(", ")));
    }
    QStringList refreshed;
    for (auto it = where.cbegin(); it != where.cend(); ++it)
        refreshed << QStringLiteral("%1 (%2)").arg(it.key(), it.value().join(QStringLiteral(", ")));
    QString text = QObject::tr("Its instances took the new symbol: %1.").arg(refreshed.join(QStringLiteral("; ")));
    text += moves.isEmpty() ? QObject::tr(" Their pins are where they were.")
                            : QLatin1Char(' ') + QObject::tr("Their pins moved: %1.").arg(moves.join(QStringLiteral("; ")));
    if (!followed.isEmpty())
        text += QLatin1Char(' ') + QObject::tr("Joined to their nets again (one step to undo there): %1.").arg(followed.join(QStringLiteral("; ")));
    if (!look.isEmpty()) text += QLatin1Char(' ') + QObject::tr("Look: %1.").arg(look.join(QStringLiteral("; ")));
    else if (!moves.isEmpty()) text += QLatin1Char(' ') + QObject::tr("Every net is as it was.");
    return text;
}

} // namespace

QJsonObject QucsControl::saveDocument(const QJsonObject& args)
{
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString as = args.value(QLatin1String("as")).toString().trimmed();
    if (!as.isEmpty()) {
        // (The name as given, and as it is to be written.)
        if (const QString bad = badFileName(QFileInfo(as).fileName()); !bad.isEmpty()) return errorResult(tr("'as': %1.").arg(bad));
        QString target = absolute(as);
        if (QFileInfo(target).suffix().isEmpty() && kindOf(doc) == QLatin1String("schematic")) target += QStringLiteral(".sch");
        if (const QString bad = badFileName(QFileInfo(target).fileName()); !bad.isEmpty()) return errorResult(tr("'as': %1.").arg(bad));
        // A schematic as x.txt was written as one, then opened as text: the
        // suffix says what a file is to Qucs-S.
        static const QHash<QString, QString> suffixOf{{QStringLiteral("schematic"), QStringLiteral("sch")},
                                                      {QStringLiteral("data display"), QStringLiteral("dpl")},
                                                      {QStringLiteral("symbol"), QStringLiteral("sym")}};
        if (const QString wanted = suffixOf.value(kindOf(doc)); !wanted.isEmpty()
            && QFileInfo(target).suffix().compare(wanted, Qt::CaseInsensitive) != 0)
            return errorResult(tr("%1 is a %2: its file ends in .%3 (%4 would open as another kind).")
                                   .arg(titleOf(doc), kindOf(doc), wanted, QFileInfo(target).fileName()));
        for (QucsDoc* other : a_app->allDocuments())
            if (other != doc && !other->getDocName().isEmpty() && sameFile(other->getDocName(), target))
                return errorResult(tr("%1 is open in another tab.").arg(QDir::toNativeSeparators(target)));
        // Not the name of a dataset imported there: a schematic simulates
        // into its own name's (its Data Set), beside the import or over it.
        if (qucs_s::dataimport::Origin origin; kindOf(doc) == QLatin1String("schematic")
            && !(doc->getDocName() == target || (!doc->getDocName().isEmpty() && sameFile(doc->getDocName(), target)))
            && qucs_s::dataimport::originOf(QFileInfo(target).absoluteDir().filePath(QFileInfo(target).completeBaseName() + QStringLiteral(".dat")), &origin))
            return errorResult(tr("%1.dat there is a dataset imported from %2, and a schematic %1 simulates into %1.dat: choose another name.")
                                   .arg(QFileInfo(target).completeBaseName(), QDir::toNativeSeparators(origin.source)));
        if (!QFileInfo(QFileInfo(target).absolutePath()).isDir())
            return errorResult(tr("There is no folder %1.").arg(QDir::toNativeSeparators(QFileInfo(target).absolutePath())));
        // Another file there already: written over when 'replace' says so,
        // or the user says yes, asked.
        if (QFileInfo::exists(target) && !(doc->getDocName() == target || (!doc->getDocName().isEmpty() && sameFile(doc->getDocName(), target)))
            && !args.value(QLatin1String("replace")).toBool()
            && !confirmed(tr("%1 exists. Write %2 over it?").arg(QDir::toNativeSeparators(target), titleOf(doc))))
            return errorResult(tr("%1 exists: 'replace' writes over it (the user was not asked, or said no).").arg(QDir::toNativeSeparators(target)));
        const QList<Instance> instances = instancesOf(a_app, doc, target, [this](QucsDoc* d) { return titleOf(d); });
        if (const QString no = aboutToWrite(target); !no.isEmpty()) return errorResult(no);
        if (!a_app->saveDocumentAs(doc, target)) return errorResult(tr("%1 could not be saved.").arg(QDir::toNativeSeparators(target)));
        const QString refreshed = refreshedInstances(instances);
        return textResult(tr("Saved as %1.").arg(QDir::toNativeSeparators(target)) + (refreshed.isEmpty() ? QString() : QLatin1Char(' ') + refreshed)
                          + librariesLinked(doc));
    }
    if (doc->getDocName().isEmpty()) return errorResult(tr("%1 has no file yet: give 'as'.").arg(titleOf(doc)));
    const QList<Instance> instances = instancesOf(a_app, doc, doc->getDocName(), [this](QucsDoc* d) { return titleOf(d); });
    if (const QString no = aboutToWrite(doc->getDocName()); !no.isEmpty()) return errorResult(no);
    if (!a_app->saveFile(doc)) return errorResult(tr("%1 could not be saved.").arg(QDir::toNativeSeparators(doc->getDocName())));
    const QString refreshed = refreshedInstances(instances);
    return textResult(tr("Saved %1.").arg(QDir::toNativeSeparators(doc->getDocName())) + (refreshed.isEmpty() ? QString() : QLatin1Char(' ') + refreshed)
                      + librariesLinked(doc));
}

QString QucsControl::librariesLinked(QucsDoc* saved) const
{
    // A schematic's save brought the project's library folders up to date.
    if (kindOf(saved) != QLatin1String("schematic")) return {};
    const qucs_s::projectlibraries::Report& sync = a_app->lastLibrarySync();
    QStringList said;
    if (!sync.made.isEmpty())
        said << tr("linked into the project from its library (read-only; compiled there): %1").arg(sync.made.join(QStringLiteral(", ")));
    if (!sync.removed.isEmpty())
        said << tr("taken away, with its model - no schematic of the project uses it now: %1").arg(sync.removed.join(QStringLiteral(", ")));
    if (!sync.conflicts.isEmpty())
        said << tr("not linked, a file of the project's in the way: %1").arg(sync.conflicts.join(QStringLiteral(", ")));
    return said.isEmpty() ? QString() : tr(" Library Verilog-A %1.").arg(said.join(QStringLiteral("; ")));
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
            // Not said: the user is asked, when they can be.
            const QString save = tr("Save"), discard = tr("Discard"), keep = tr("Keep it open");
            const QString chosen = choice(tr("%1 has unsaved changes.").arg(title), {save, discard, keep});
            if (chosen == save && !doc->getDocName().isEmpty() && a_app->saveFile(doc)) {
            } else if (chosen == discard) {
                if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
                else if (auto* text = qobject_cast<TextDoc*>(QucsApp::documentWidget(doc))) text->document()->setModified(false);
                doc->setDocChanged(false);
            } else {
                return errorResult(chosen == keep ? tr("%1 stays open: the user kept it.").arg(title)
                                                  : tr("%1 has unsaved changes: say whether to save or discard them ('unsaved').").arg(title));
            }
        }
    }
    QWidget* w = QucsApp::documentWidget(doc);
    a_app->showDocument(w);
    a_app->slotFileClose(a_app->DocumentTab->indexOf(w));
    return textResult(tr("%1 is closed.").arg(title));
}

// ----------------------------------------------------------------------
// A schematic

namespace {
QJsonObject componentModel(Component* c);   // (below: the JSON form)
}

QJsonObject QucsControl::getSchematic(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    if (args.value(QLatin1String("format")).toString() == QLatin1String("text")) return textResult(sch->documentText());
    // The JSON form set_schematic takes back: its parts by named
    // properties, its wires and labels.
    if (args.value(QLatin1String("format")).toString() == QLatin1String("json")) {
        QJsonArray components, wires;
        for (Component* c : sch->a_DocComps) components.append(componentModel(c));
        for (Wire* w : sch->a_DocWires) {
            QJsonObject o{{QStringLiteral("from"), QJsonArray{w->x1, w->y1}}, {QStringLiteral("to"), QJsonArray{w->x2, w->y2}}};
            if (w->hasLabel()) {
                o.insert(QStringLiteral("label"), w->label()->Name);
                o.insert(QStringLiteral("label_at"), QJsonArray{w->label()->x1, w->label()->y1});
                // (Its net's initial value, a .IC: lost on the way back.)
                if (!w->label()->initValue.isEmpty()) o.insert(QStringLiteral("initial"), w->label()->initValue);
            }
            wires.append(o);
        }
        for (Node* n : sch->a_DocNodes)
            if (n->hasLabel()) {
                QJsonObject o{{QStringLiteral("at"), QJsonArray{n->cx, n->cy}}, {QStringLiteral("label"), n->label()->Name},
                              {QStringLiteral("label_at"), QJsonArray{n->label()->x1, n->label()->y1}}};
                if (!n->label()->initValue.isEmpty()) o.insert(QStringLiteral("initial"), n->label()->initValue);
                wires.append(o);
            }
        return jsonResult(QJsonObject{{QStringLiteral("document"), titleOf(sch)},
                                      {QStringLiteral("components"), components},
                                      {QStringLiteral("wires"), wires},
                                      {QStringLiteral("how"), tr("set_schematic takes 'components' and 'wires' so; diagrams and "
                                                                 "paintings have their own tools (or 'text')")}});
    }
    // At a glance: its parts counted by type, its analyses in full, its
    // named nets, where it all is - thousands of parts in a few hundred
    // bytes, before 'components' or 'region' lists some of them.
    if (args.value(QLatin1String("format")).toString() == QLatin1String("overview")) {
        QHash<QString, int> byType;
        QJsonArray analyses;
        QRect extent;
        for (Component* c : sch->a_DocComps) {
            byType[c->Model]++;
            extent = extent.isNull() ? QRect(c->cx, c->cy, 1, 1) : extent.united(QRect(c->cx, c->cy, 1, 1));
            if (!c->isSimulation || analyses.size() >= 40) continue;
            QJsonObject shown;
            for (Property* p : c->Props)
                if (p->display) shown.insert(p->Name, p->Value);
            analyses.append(QJsonObject{{QStringLiteral("name"), c->Name}, {QStringLiteral("type"), c->Model},
                                        {QStringLiteral("active"), c->isActive == COMP_IS_ACTIVE}, {QStringLiteral("properties"), shown}});
        }
        for (Wire* w : sch->a_DocWires)
            extent = extent.isNull() ? QRect(w->P1(), w->P2()).normalized() : extent.united(QRect(w->P1(), w->P2()).normalized());
        QStringList types = byType.keys();
        std::sort(types.begin(), types.end(), [&byType](const QString& a, const QString& b) {
            return byType.value(a) != byType.value(b) ? byType.value(a) > byType.value(b) : a < b;
        });
        QJsonArray counted;
        for (const QString& t : std::as_const(types)) counted.append(QJsonObject{{QStringLiteral("type"), t}, {QStringLiteral("count"), byType.value(t)}});
        QSet<QString> seenNames;
        QStringList named;
        const auto name = [&](const QString& n) {
            if (!seenNames.contains(n)) {
                seenNames.insert(n);
                named << n;
            }
        };
        for (Wire* w : sch->a_DocWires)
            if (w->hasLabel()) name(w->label()->Name);
        for (Node* n : sch->a_DocNodes)
            if (n->hasLabel()) name(n->label()->Name);
        named.sort();
        const Nets nets = netsOf(sch, nullptr);
        QSet<int> netIds;
        for (int id : nets.netOf) netIds.insert(id);
        QJsonArray diagrams;
        int n = 0;
        for (Diagram* d : sch->a_DocDiags) {
            QJsonArray traces;
            for (Graph* g : d->Graphs) traces.append(g->Var);
            diagrams.append(QJsonObject{{QStringLiteral("diagram"), ++n}, {QStringLiteral("type"), d->Name}, {QStringLiteral("traces"), traces}});
        }
        QJsonObject overview{{QStringLiteral("document"), titleOf(sch)},
                             {QStringLiteral("path"), sch->getDocName()},
                             {QStringLiteral("components"), int(sch->a_DocComps.size())},
                             {QStringLiteral("by type"), counted},
                             {QStringLiteral("analyses"), analyses},
                             {QStringLiteral("nets"), int(netIds.size())},
                             {QStringLiteral("named nets"), QJsonArray::fromStringList(named.mid(0, 100))},
                             {QStringLiteral("wires"), int(sch->a_DocWires.size())},
                             {QStringLiteral("diagrams"), diagrams},
                             {QStringLiteral("paintings"), int(sch->a_DocPaints.size())},
                             {QStringLiteral("dataset"), sch->getDataSet()},
                             {QStringLiteral("next"), tr("'components' (names) or 'region' ([x1, y1, x2, y2]) lists parts in full, with their pins and nets.")}};
        if (!extent.isNull())
            overview.insert(QStringLiteral("extent"), QJsonArray{extent.left(), extent.top(), extent.right(), extent.bottom()});
        if (named.size() > 100) overview.insert(QStringLiteral("more named nets"), int(named.size() - 100));
        return jsonResult(overview);
    }

    // Which properties: those shown or not at the type's default (a JFET
    // has 27, most of them at theirs), those shown, or all.
    const QString propertyMode = args.value(QLatin1String("properties")).toString(QStringLiteral("non_default"));
    if (propertyMode != QLatin1String("non_default") && propertyMode != QLatin1String("shown") && propertyMode != QLatin1String("all"))
        return errorResult(tr("'properties' is non_default, shown or all."));
    // Which components: by name, or in a region.
    QSet<QString> wantedNames;
    for (const QJsonValue& v : args.value(QLatin1String("components")).toArray()) wantedNames.insert(v.toString().trimmed());
    QRect region;
    if (args.contains(QLatin1String("region"))) {
        const QJsonArray r = args.value(QLatin1String("region")).toArray();
        if (r.size() != 4) return errorResult(tr("'region' is [x1, y1, x2, y2]."));
        region = regionOf(r);
    }
    const bool filtered = !wantedNames.isEmpty() || region.isValid();
    QSet<QString> namesFound;
    // What most can take in: a schematic of thousands of parts is listed
    // in part, and says how to see the rest - of each kind, 200 of those
    // asked for, or the first 50 of a large schematic's read whole (its 200
    // parts with their pins were 45,000 tokens, its 2,500 labels as many).
    constexpr int kMost = 200;
    const int most = filtered || int(sch->a_DocComps.size()) <= kMost ? kMost : 50;
    QHash<QString, QHash<QString, QString>> defaults;   // by type
    const auto defaultsOf = [&defaults](Component* c) -> const QHash<QString, QString>& {
        auto it = defaults.find(c->Model);
        if (it != defaults.end()) return *it;
        QHash<QString, QString> values;
        if (std::unique_ptr<Component> fresh{newComponent(c->Model)})
            for (const Property* p : fresh->Props) values.insert(p->Name, p->Value);
        return *defaults.insert(c->Model, values);
    };
    int omittedProperties = 0, omittedComponents = 0;
    QSet<QPoint> pinPlaces;   // of the components listed: the wires to them

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
    QSet<int> netsListed;
    // Parts without a name, of a type there are several of (grounds): each
    // its 'ref', GND#1, GND#2, ... - what connect and add_wire take.
    QHash<const Component*, QString> refs;
    {
        QHash<QString, QList<const Component*>> unnamed;
        for (const Component* c : sch->a_DocComps)
            if (c->Name.isEmpty() || c->Name == QLatin1String("*")) unnamed[c->Model] << c;
        for (auto it = unnamed.cbegin(); it != unnamed.cend(); ++it)
            if (it.value().size() > 1)
                for (int k = 0; k < it.value().size(); ++k) refs.insert(it.value().at(k), QStringLiteral("%1#%2").arg(it.key()).arg(k + 1));
        // (And a name given twice: R1, then R1#2.)
        QHash<QString, QList<const Component*>> named;
        for (const Component* c : sch->a_DocComps)
            if (!c->Name.isEmpty() && c->Name != QLatin1String("*")) named[c->Name] << c;
        for (auto it = named.cbegin(); it != named.cend(); ++it)
            if (it.value().size() > 1)
                for (int k = 0; k < it.value().size(); ++k)
                    refs.insert(it.value().at(k), k == 0 ? it.key() : QStringLiteral("%1#%2").arg(it.key()).arg(k + 1));
    }
    const auto refFor = [&refs](const Component* c) { return refs.value(c, c->Name.isEmpty() ? c->Model : c->Name); };
    const auto wanted = [&](Component* c) {
        if (!wantedNames.isEmpty()) {
            const QString ref = refFor(c);
            bool named = false;
            for (const QString& n : std::as_const(wantedNames))
                if (n.compare(ref, Qt::CaseInsensitive) == 0 || (!c->Name.isEmpty() && n.compare(c->Name, Qt::CaseInsensitive) == 0)
                    // (The one ground there is: GND#1 too, as move and select take it.)
                    || ((c->Name.isEmpty() || c->Name == QLatin1String("*")) && !refs.contains(c)
                        && n.compare(c->Model + QStringLiteral("#1"), Qt::CaseInsensitive) == 0)) {
                    namesFound.insert(n);
                    named = true;
                }
            if (!named) return false;
        }
        return !region.isValid() || region.contains(c->cx, c->cy);
    };
    // The pins on each net: of every component (a net's pins are all of
    // them, whichever are listed), and the nets' names - net1, net2 ... in
    // the order of every part, listed or not, so that a name from a read of
    // some parts is that of a read of all.
    {
        QHash<QString, int> again;
        int unnamedNets = 0;
        for (Component* c : sch->a_DocComps) {
            const QString base = keyBase(c, again);
            for (int i = 0; i < c->Ports.size(); ++i) {
                const auto it = nets.netOf.constFind(base + QLatin1Char('.') + QString::number(i + 1));
                if (it == nets.netOf.constEnd()) continue;
                if (!netNames.contains(*it)) netNames.insert(*it, QStringLiteral("net%1").arg(++unnamedNets));
                if (!order.contains(*it)) order << *it;
                pinsOn[*it] << QStringLiteral("%1.%2").arg(refFor(c)).arg(i + 1);
            }
        }
    }
    for (Component* c : sch->a_DocComps) {
        const QString base = keyBase(c, seen);
        if (!wanted(c)) continue;
        if (components.size() >= most) {
            ++omittedComponents;
            continue;
        }
        QJsonObject json = componentJson(c);
        if (propertyMode != QLatin1String("all")) {
            const QHash<QString, QString>& byDefault = defaultsOf(c);
            QJsonArray kept;
            for (const QJsonValue& v : json.value(QStringLiteral("properties")).toArray()) {
                const QJsonObject p = v.toObject();
                const bool shown = p.value(QStringLiteral("shown")).toBool();
                const QString name = p.value(QStringLiteral("name")).toString();
                const bool changed = !byDefault.contains(name) || byDefault.value(name) != p.value(QStringLiteral("value")).toString();
                if (shown || (propertyMode == QLatin1String("non_default") && changed)) kept.append(p);
                else ++omittedProperties;
            }
            json.insert(QStringLiteral("properties"), kept);
        }
        for (const Port* pp : c->Ports) pinPlaces.insert(QPoint(c->cx + pp->x, c->cy + pp->y));
        QJsonArray pins = json.value(QStringLiteral("pins")).toArray();
        for (int i = 0; i < pins.size(); ++i) {
            const auto it = nets.netOf.constFind(base + QLatin1Char('.') + QString::number(i + 1));
            if (it == nets.netOf.constEnd()) continue;
            QJsonObject pin = pins.at(i).toObject();
            pin.insert(QStringLiteral("net"), netNames.value(*it));
            pins[i] = pin;
            netsListed.insert(*it);
        }
        json.insert(QStringLiteral("pins"), pins);
        if (refs.contains(c)) json.insert(QStringLiteral("ref"), refs.value(c));
        components.append(json);
    }
    int omittedNets = 0, omittedWires = 0, omittedLabels = 0;
    for (int id : order) {
        if (filtered && !netsListed.contains(id)) continue;
        if (pinsOn.value(id).size() > 1 || !netNames.value(id).startsWith(QLatin1String("net"))) {
            if (netList.size() >= most) {
                ++omittedNets;
                continue;
            }
            // (Ground may join thousands of pins.)
            const QStringList& pins = pinsOn[id];
            QJsonObject net{{QStringLiteral("net"), netNames.value(id)}, {QStringLiteral("pins"), QJsonArray::fromStringList(pins.mid(0, 60))}};
            if (pins.size() > 60) net.insert(QStringLiteral("more pins"), int(pins.size() - 60));
            netList.append(net);
        }
    }
    for (Wire* w : sch->a_DocWires) {
        if (filtered) {
            const bool inRegion = region.isValid() && (region.contains(w->P1()) || region.contains(w->P2()));
            const bool toPin = wantedNames.isEmpty() ? false : (pinPlaces.contains(w->P1()) || pinPlaces.contains(w->P2()));
            if (!inRegion && !toPin) continue;
        }
        if (wires.size() >= most) {
            ++omittedWires;
            continue;
        }
        QJsonObject wire{{QStringLiteral("x1"), w->x1}, {QStringLiteral("y1"), w->y1}, {QStringLiteral("x2"), w->x2}, {QStringLiteral("y2"), w->y2}};
        if (w->hasLabel()) {
            wire.insert(QStringLiteral("net"), w->label()->Name);
            if (labels.size() < most)
                labels.append(QJsonObject{{QStringLiteral("net"), w->label()->Name},
                                          {QStringLiteral("x"), w->label()->root().x()}, {QStringLiteral("y"), w->label()->root().y()}});
            else ++omittedLabels;
        }
        wires.append(wire);
    }
    // (A read of some parts: the labels on their pins, or in its region.)
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel() && (!filtered || (region.isValid() && region.contains(n->center())) || pinPlaces.contains(n->center()))) {
            if (labels.size() >= most) {
                ++omittedLabels;
                continue;
            }
            labels.append(QJsonObject{{QStringLiteral("net"), n->label()->Name}, {QStringLiteral("x"), n->cx}, {QStringLiteral("y"), n->cy}});
        }
    diagrams = diagramsJson(sch);
    // Each painting by its fields (its type, place, text, look), numbered
    // as the painting tools take them.
    paintings = paintingsJson(sch->a_DocPaints, most);
    const int omittedPaintings = int(sch->a_DocPaints.size()) - int(paintings.size());
    // The document's own settings: those of its <Properties>.
    QJsonObject settings{{QStringLiteral("dataset"), sch->getDataSet()},
                         {QStringLiteral("data display"), sch->getDataDisplay()},
                         {QStringLiteral("open data display after simulation"), sch->getSimOpenDpl()},
                         {QStringLiteral("grid shown"), sch->getGridOn()}};
    if (!sch->getScript().isEmpty()) {
        settings.insert(QStringLiteral("script"), sch->getScript());
        settings.insert(QStringLiteral("run script after simulation"), sch->getSimRunScript());
    }
    if (sch->getShowFrame() != FrameSize::None)
        settings.insert(QStringLiteral("frame"), QJsonObject{{QStringLiteral("size"), int(sch->getShowFrame())},
                                                             {QStringLiteral("texts"), QJsonArray{sch->getFrame_Text0(), sch->getFrame_Text1(),
                                                                                                  sch->getFrame_Text2(), sch->getFrame_Text3()}}});
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)},
                       {QStringLiteral("path"), sch->getDocName()},
                       {QStringLiteral("grid"), QJsonArray{sch->getGridX(), sch->getGridY()}},
                       {QStringLiteral("settings"), settings},
                       {QStringLiteral("components"), components},
                       {QStringLiteral("nets"), netList},
                       {QStringLiteral("wires"), wires},
                       {QStringLiteral("labels"), labels},
                       {QStringLiteral("diagrams"), diagrams},
                       {QStringLiteral("paintings"), paintings}};
    QStringList notFound;
    for (const QString& n : std::as_const(wantedNames))
        if (!namesFound.contains(n)) notFound << n;
    if (!notFound.isEmpty()) {
        notFound.sort();
        result.insert(QStringLiteral("not found"), QJsonArray::fromStringList(notFound));
    }
    // What check_schematic would say, counted, the first few told.
    {
        const QList<qucs_s::erc::Issue> issues = qucs_s::erc::check(sch);
        const int notes = int(qucs_s::erc::notes(sch).size());
        if (!issues.isEmpty() || notes > 0) {
            QJsonArray first;
            for (const auto& i : issues)
                if (first.size() < 10) first.append(i.message);
            QJsonObject problems{{QStringLiteral("errors"), qucs_s::erc::errorCount(issues)},
                                 {QStringLiteral("warnings"), int(issues.size()) - qucs_s::erc::errorCount(issues)},
                                 {QStringLiteral("notes"), notes},
                                 {QStringLiteral("check_schematic"), tr("tells each with its place")}};
            if (!first.isEmpty()) problems.insert(QStringLiteral("first"), first);
            result.insert(QStringLiteral("problems"), problems);
        }
    }
    // The parameters its instances take, when it is a subcircuit with any.
    if (const QJsonArray parameters = subcircuitParametersJson(sch); !parameters.isEmpty())
        result.insert(QStringLiteral("subcircuit parameters"), parameters);
    // The symbol's (a subcircuit's): when asked, or when it is shown.
    if (args.value(QLatin1String("symbol")).toBool() || sch->getSymbolMode()) {
        result.insert(QStringLiteral("symbol paintings"), paintingsJson(sch->a_SymbolPaints));
        result.insert(QStringLiteral("showing its symbol"), sch->getSymbolMode());
    }
    QStringList left;
    if (omittedProperties > 0 && propertyMode == QLatin1String("non_default"))
        left << tr("%1 properties at their types' defaults and not shown (properties: all lists them)").arg(omittedProperties);
    else if (omittedProperties > 0)
        left << tr("%1 properties not shown (properties: all lists them)").arg(omittedProperties);
    if (omittedComponents > 0) left << tr("%1 more components").arg(omittedComponents);
    if (omittedNets > 0) left << tr("%1 more nets").arg(omittedNets);
    if (omittedWires > 0) left << tr("%1 more wires").arg(omittedWires);
    if (omittedLabels > 0) left << tr("%1 more net labels").arg(omittedLabels);
    if (omittedPaintings > 0) left << tr("%1 more paintings").arg(omittedPaintings);
    if (!left.isEmpty()) {
        QString note = tr("Left out: %1.").arg(left.join(QStringLiteral("; ")));
        if (omittedComponents + omittedNets + omittedWires + omittedLabels > 0)
            note += tr(" %1 components, %2 wires in all: 'components' (names) or 'region' ([x1, y1, x2, y2]) lists a part; "
                       "format: overview counts them by type.")
                        .arg(sch->a_DocComps.size()).arg(sch->a_DocWires.size());
        result.insert(QStringLiteral("left out"), note);
    }
    return jsonResult(result);
}

QJsonObject QucsControl::checkSchematic(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    QJsonArray errors, warnings, notes;
    for (const auto& i : qucs_s::erc::check(sch))
        (i.severity == qucs_s::erc::Severity::Error ? errors : warnings).append(issueJson(i));
    for (const auto& i : qucs_s::erc::notes(sch)) notes.append(issueJson(i));
    // A text drawn over something - a wire, a symbol, another text, a
    // label, a diagram: a note, with both boxes (edit_component's text_at
    // "auto", or arrange's 'labels', moves it clear).
    const QList<qucs_s::textplace::Overlap> overlapping = qucs_s::textplace::overlaps(sch);
    for (const auto& o : overlapping) {
        if (notes.size() >= 200) {
            notes.append(QJsonObject{{QStringLiteral("message"), tr("... and %1 more texts drawn over something").arg(overlapping.size() - int(&o - overlapping.constData()))}});
            break;
        }
        QJsonObject n{{QStringLiteral("message"), o.message},
                      {QStringLiteral("at"), QJsonArray{o.where.x(), o.where.y()}},
                      {QStringLiteral("box"), rectArray(o.box)},
                      {QStringLiteral("over"), rectArray(o.other)}};
        if (!o.part.isEmpty()) n.insert(QStringLiteral("component"), o.part);
        notes.append(n);
    }
    if (!overlapping.isEmpty())
        notes.append(QJsonObject{{QStringLiteral("message"), tr("%n text(s) drawn over something: edit_component with 'text_at': \"auto\" moves "
                                                                "a part's clear, arrange with 'labels' all of them (never a part or a wire)",
                                                                nullptr, int(overlapping.size()))}});
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)},
                       {QStringLiteral("errors"), errors},
                       {QStringLiteral("warnings"), warnings},
                       {QStringLiteral("notes"), notes},
                       {QStringLiteral("found"), tr("%1 errors, %2 warnings, %3 notes").arg(errors.size()).arg(warnings.size()).arg(notes.size())}};
    // Its subcircuits, at any depth: a line of counts each, or all they
    // hold. An open one as it is, the others from disk.
    const bool full = args.value(QStringLiteral("subcircuits")).toBool();
    const auto open = [this](const QString& file) { return dynamic_cast<Schematic*>(a_app->findDoc(file)); };
    QJsonArray subcircuits;
    int subErrors = 0, subWarnings = 0;
    for (const auto& sub : qucs_s::erc::checkSubcircuits(sch, open)) {
        const int e = qucs_s::erc::errorCount(sub.issues);
        subErrors += e;
        subWarnings += int(sub.issues.size()) - e;
        QJsonObject o{{QStringLiteral("file"), QDir::toNativeSeparators(sub.file)},
                      {QStringLiteral("errors"), e},
                      {QStringLiteral("warnings"), int(sub.issues.size()) - e}};
        if (full) {
            QJsonArray subErrorList, subWarningList;
            for (const auto& i : sub.issues)
                (i.severity == qucs_s::erc::Severity::Error ? subErrorList : subWarningList).append(issueJson(i));
            o.insert(QStringLiteral("error list"), subErrorList);
            o.insert(QStringLiteral("warning list"), subWarningList);
        }
        subcircuits.append(o);
    }
    if (!subcircuits.isEmpty()) {
        result.insert(QStringLiteral("subcircuits"), subcircuits);
        result.insert(QStringLiteral("found"), result.value(QStringLiteral("found")).toString()
                                                   + tr("; in its subcircuits %1 errors, %2 warnings").arg(subErrors).arg(subWarnings));
    }
    if (errors.isEmpty() && warnings.isEmpty()) {
        QString verdict = notes.isEmpty() ? tr("Nothing found.") : tr("Nothing wrong found; the notes are fine if meant.");
        if (subErrors + subWarnings > 0)
            verdict += full ? tr(" Its subcircuits have findings of their own, listed.")
                            : tr(" Its subcircuits have findings of their own: 'subcircuits': true lists them.");
        result.insert(QStringLiteral("verdict"), verdict);
    }
    return jsonResult(result);
}

// ---- the schematic as JSON: set_schematic's 'components' and 'wires',
// get_schematic's format "json" - each part by its type and its properties
// by name, checked against the type (a line's values are positional: one
// too many shifts every one after it), then written as its .sch line.

namespace {

// A part as the JSON form has it: what set_schematic takes back.
QJsonObject componentModel(Component* c)
{
    QJsonObject o{{QStringLiteral("type"), c->Model},
                  {QStringLiteral("x"), c->cx},
                  {QStringLiteral("y"), c->cy},
                  {QStringLiteral("rotation"), ((c->rotated - defaultRotation(c->Model)) % 4 + 4) % 4},
                  {QStringLiteral("text_at"), QJsonArray{c->tx, c->ty}}};
    if (!c->Name.isEmpty()) o.insert(QStringLiteral("name"), c->Name);
    if (c->mirroredX) o.insert(QStringLiteral("mirror"), true);
    if (c->isActive != COMP_IS_ACTIVE) o.insert(QStringLiteral("active"), false);
    if (!c->showName) o.insert(QStringLiteral("name_shown"), false);
    std::unique_ptr<Component> fresh(newComponent(c->Model));
    QJsonObject properties, shown;
    QJsonArray equations;
    const bool eq = isEquationKind(c);
    for (const Property* p : c->Props) {
        if (eq && !isFixedField(p)) {
            equations.append(p->Value.isEmpty() && takesFlags(c) ? p->Name : p->Name + QLatin1Char('=') + p->Value);
        } else if (isRepeated(c, p->Name)) {
            // (All of them, as a list: as one value, the last alone was kept.)
            QJsonArray all = properties.value(p->Name).toArray();
            all.append(p->Value);
            properties.insert(p->Name, all);
        } else {
            properties.insert(p->Name, p->Value);
        }
        const Property* d = fresh ? fresh->getProperty(p->Name) : nullptr;
        if (d == nullptr || d->display != p->display) shown.insert(p->Name, p->display);
    }
    if (!properties.isEmpty()) o.insert(QStringLiteral("properties"), properties);
    if (eq) o.insert(QStringLiteral("equations"), equations);
    if (!shown.isEmpty()) o.insert(QStringLiteral("shown"), shown);
    return o;
}

// A library part (a Lib) whose model is one component line - a varactor's
// Diode with the library's values - made that component, \a c deleted: the
// library panel places it so, and a Lib's netlist would read the diode's
// values as its library and part. "Library/part" then; empty for any other.
QString asItsModel(Component*& c, Schematic* sch)
{
    auto* lib = dynamic_cast<LibComp*>(c);
    if (lib == nullptr || lib->Props.size() < 2) return {};
    QString line = lib->componentModel();
    if (line.isEmpty()) return {};
    const QString named = QStringLiteral("%1/%2").arg(lib->Props.at(0)->Value, lib->Props.at(1)->Value);
    const misc::ErrorCapture quiet;
    Component* model = getComponentFromName(line, sch);
    if (model == nullptr) return {};
    delete c;
    c = model;
    return named;
}

// A part of the JSON form as its .sch line; empty, and why, when it does
// not do (a type there is not, a property it has not).
QString componentLineOf(Schematic* sch, const QJsonObject& o, QStringList* taken, QStringList* notes, QString* error)
{
    const QString type = o.value(QLatin1String("type")).toString().trimmed();
    std::unique_ptr<Component> c(newComponent(type));
    if (!c) {
        *error = tr("there is no component type %1 (list_component_types lists them)").arg(type);
        return {};
    }
    c->setSchematic(sch);
    if (!setListsOf(c.get(), o, error, false, true) || !setProperties(c.get(), o.value(QLatin1String("properties")).toObject(), error, true))
        return {};
    // (A library part that is one component: that component.)
    Component* part = c.release();
    asItsModel(part, sch);
    c.reset(part);
    c->recreate();
    const QJsonValue turn = o.value(QLatin1String("rotation"));
    if (!turn.isUndefined() && (!turn.isDouble() || turn.toInt() < 0 || turn.toInt() > 3 || turn.toDouble() != turn.toInt())) {
        *error = tr("'rotation' is 0 to 3 quarter turns");
        return {};
    }
    orient(c.get(), turn.toInt(), o.value(QLatin1String("mirror")).toBool());
    if (!o.value(QLatin1String("x")).isDouble() || !o.value(QLatin1String("y")).isDouble()) {
        *error = tr("a part needs its place: 'x' and 'y'");
        return {};
    }
    const QPoint at = Schematic::withinModelLimit(QPoint(o.value(QLatin1String("x")).toInt(), o.value(QLatin1String("y")).toInt()));
    c->moveCenter(at.x() - c->cx, at.y() - c->cy);
    // Its name: given, or its type's prefix and the first number free.
    // (\a taken: a device's name as it is; a simulation block's after a
    // "." - Tr1, a transformer, and TR1, a transient analysis, are two
    // names to a file and to the netlist, one to SPICE's devices alone.)
    const bool block = c->Model.startsWith(QLatin1Char('.'));
    const auto clash = [&](const QString& n) {
        for (const QString& t : std::as_const(*taken)) {
            const bool tBlock = t.startsWith(QLatin1Char('.'));
            const QString tName = tBlock ? t.mid(1) : t;
            if (tName == n || (!block && !tBlock && tName.compare(n, Qt::CaseInsensitive) == 0)) return true;
        }
        return false;
    };
    QString name = o.value(QLatin1String("name")).toString().trimmed();
    // (What a file can hold, as a file holds it: 2N2222 too - but not what
    // breaks its line.)
    if (name.contains(QRegularExpression(QStringLiteral("[\\s\"<>]")))) {
        *error = tr("%1 cannot name a part: no spaces, quotes, < or >").arg(name);
        return {};
    }
    if (name.isEmpty() && !c->Name.isEmpty() && c->Name != QLatin1String("*")) {
        const QString prefix = c->Name;
        for (int k = 1; name.isEmpty() || clash(name); ++k) name = prefix + QString::number(k);
    }
    if (!name.isEmpty() && clash(name)) {
        *error = tr("two parts are named %1").arg(name);
        return {};
    }
    if (!name.isEmpty()) *taken << (block ? QLatin1Char('.') + name : name);
    c->Name = name;
    if (o.contains(QLatin1String("active"))) c->isActive = o.value(QLatin1String("active")).toBool() ? COMP_IS_ACTIVE : COMP_IS_OPEN;
    if (!setTextOf(c.get(), o, error)) return {};
    if (const QStringList typos = numberTypos(name.isEmpty() ? c->Model : name, c->Model, o.value(QLatin1String("properties")).toObject());
        !typos.isEmpty()) {
        *error = typos.join(QStringLiteral("; "));
        return {};
    }
    *notes << valueNotes(c.get(), o.value(QLatin1String("properties")).toObject().keys(), sch);
    return QStringLiteral("  ") + c->save();
}

// A wire of the JSON form - {"from": [x, y], "to": [x, y], "label": "out"}
// - or a label on a pin alone ({"at": [x, y], "label": "out"}): its line.
QString wireLineOf(Schematic* sch, const QJsonObject& o, QString* error)
{
    const auto point = [&](const char* key, QPoint* p) {
        const QJsonArray a = o.value(QLatin1String(key)).toArray();
        if (a.size() != 2 || !a.at(0).isDouble() || !a.at(1).isDouble()) return false;
        *p = Schematic::withinModelLimit(QPoint(a.at(0).toInt(), a.at(1).toInt()));
        return true;
    };
    QPoint a, b;
    const bool alone = o.contains(QLatin1String("at"));
    if (alone ? !point("at", &a) : (!point("from", &a) || !point("to", &b))) {
        *error = tr("a wire is {\"from\": [x, y], \"to\": [x, y]}, a label on a pin alone {\"at\": [x, y], \"label\": ...}");
        return {};
    }
    if (alone) b = a;
    else if (a.x() != b.x() && a.y() != b.y()) {
        *error = tr("a wire runs straight across or up: %1, %2 to %3, %4 is neither").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
        return {};
    }
    const QString label = o.value(QLatin1String("label")).toString().trimmed();
    if (label.contains(QLatin1Char('"')) || label.contains(QLatin1Char(' '))) {
        *error = tr("a net's name has no spaces or quotes (%1)").arg(label);
        return {};
    }
    if (alone && label.isEmpty()) {
        *error = tr("{\"at\": ...} is a label: it needs its 'label'");
        return {};
    }
    if (label.isEmpty()) return QStringLiteral("  <%1 %2 %3 %4 \"\" 0 0 0 \"\">").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
    // The label fixed halfway along, its text up and to the right.
    const QPoint root = alone ? a : (a + b) / 2;
    int tx = root.x() + 30, ty = root.y() - 30;
    sch->setOnGrid(tx, ty);
    const QJsonArray textAt = o.value(QLatin1String("label_at")).toArray();
    if (textAt.size() == 2) {
        tx = textAt.at(0).toInt();
        ty = textAt.at(1).toInt();
    }
    const int along = std::abs(root.x() - a.x()) + std::abs(root.y() - a.y());
    // Its net's initial value (a .IC), as a label's dialog gives it.
    const QString initial = o.value(QLatin1String("initial")).toString().trimmed();
    if (initial.contains(QLatin1Char('"')) || initial.contains(QLatin1Char('>'))) {
        *error = tr("a label's 'initial' value has no quotes or > (%1)").arg(initial);
        return {};
    }
    return QStringLiteral("  <%1 %2 %3 %4 \"%5\" %6 %7 %8 \"%9\">").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y()).arg(label).arg(tx).arg(ty).arg(along)
        .arg(initial);
}

} // namespace

QJsonObject QucsControl::setSchematic(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    // The JSON form: its sections as text, each part checked on the way.
    const bool structured = args.contains(QLatin1String("components")) || args.contains(QLatin1String("wires"));
    QStringList valueNotesOf;
    QString composed;
    if (structured) {
        if (args.contains(QLatin1String("text")))
            return errorResult(tr("Give 'text' (the .sch lines) or 'components' and 'wires' (the JSON form), not both."));
        if (args.contains(QLatin1String("components"))) {
            if (!args.value(QLatin1String("components")).isArray())
                return errorResult(tr("'components' is a list of parts: [{\"type\": \"R\", \"name\": \"R1\", \"x\": 100, \"y\": 100, "
                                      "\"properties\": {\"R\": \"1k\"}}, ...] (get_schematic's format json gives them so)."));
            QStringList taken, lines;
            int i = 0;
            for (const QJsonValue& v : args.value(QLatin1String("components")).toArray()) {
                const QString line = componentLineOf(sch, v.toObject(), &taken, &valueNotesOf, &error);
                if (line.isEmpty()) return errorResult(tr("Not changed: components[%1]: %2.").arg(i).arg(error));
                lines << line;
                ++i;
            }
            composed += QStringLiteral("<Components>\n") + lines.join(QLatin1Char('\n')) + (lines.isEmpty() ? QString() : QStringLiteral("\n"))
                        + QStringLiteral("</Components>\n");
        }
        if (args.contains(QLatin1String("wires"))) {
            if (!args.value(QLatin1String("wires")).isArray())
                return errorResult(tr("'wires' is a list: [{\"from\": [x, y], \"to\": [x, y], \"label\": \"out\"}, {\"at\": [x, y], \"label\": \"in\"}, ...]."));
            QStringList lines;
            int i = 0;
            for (const QJsonValue& v : args.value(QLatin1String("wires")).toArray()) {
                const QString line = wireLineOf(sch, v.toObject(), &error);
                if (line.isEmpty()) return errorResult(tr("Not changed: wires[%1]: %2.").arg(i).arg(error));
                lines << line;
                ++i;
            }
            composed += QStringLiteral("<Wires>\n") + lines.join(QLatin1Char('\n')) + (lines.isEmpty() ? QString() : QStringLiteral("\n"))
                        + QStringLiteral("</Wires>\n");
        }
    }
    prepare(sch);
    const QString text = structured ? composed : args.value(QLatin1String("text")).toString();
    QStringList short_;
    // (The .sch lines' values are checked once read: a number mistyped
    // puts all back - the schematic, its undo steps, whether changed.)
    const QPair<QString, QString> stateBefore = structured ? QPair<QString, QString>() : sch->snapshotAll();
    const Schematic::UndoStacks marksBefore = sch->undoStacks();
    const bool changedBefore = sch->getDocChanged();
    // The values each part had (a value it keeps is not the text's to
    // answer for: a file of old wrote "1 kOhms").
    QHash<QString, QHash<QString, QString>> had;
    for (Component* c : sch->a_DocComps)
        if (!c->Name.isEmpty())
            for (const Property* p : c->Props) had[c->Name].insert(p->Name, p->Value);
    if (!sch->replaceContent(text, &error, &short_))
        return errorResult(tr("Not changed: %1").arg(error));
    // Only the parts of the text: one of <Paintings> alone checked nothing
    // it did not change.
    const bool partsGiven = (!text.contains(QLatin1String("<Wires>")) && !text.contains(QLatin1String("<Diagrams>"))
                             && !text.contains(QLatin1String("<Paintings>")) && !text.contains(QLatin1String("<Properties>"))
                             && !text.contains(QLatin1String("<Symbol>")))
                            || text.contains(QLatin1String("<Components>"));
    if (!structured && partsGiven) {
        QStringList typos;
        for (Component* c : sch->a_DocComps) {
            std::unique_ptr<Component> fresh(newComponent(c->Model));
            if (!fresh) continue;
            for (const Property* p : c->Props)
                if (const Property* d = fresh->getProperty(p->Name))
                    if (!(had.contains(c->Name) && had.value(c->Name).value(p->Name) == p->Value && had.value(c->Name).contains(p->Name)))
                        if (const QString t = numberTypo(c->Name.isEmpty() ? c->Model : c->Name, p->Name, p->Value, d->Value); !t.isEmpty())
                            typos << t;
        }
        if (!typos.isEmpty()) {
            sch->restoreAll(stateBefore, false);
            sch->setUndoStacks(marksBefore);
            sch->setChanged(changedBefore, false);
            if (typos.size() > 20) typos = typos.mid(0, 20) << tr("and %1 more").arg(typos.size() - 20);
            return errorResult(tr("Not changed: %1. A component line's values are its properties in order (describe_component_type "
                                  "gives them).").arg(typos.join(QStringLiteral("; "))));
        }
    }
    // What it read, section by section: of the diagrams all get_schematic
    // tells - each trace's points or why it has none, each marker and the
    // sample it shows.
    const bool whole = !text.contains(QLatin1String("<Components>")) && !text.contains(QLatin1String("<Wires>"))
                    && !text.contains(QLatin1String("<Diagrams>")) && !text.contains(QLatin1String("<Paintings>"));
    const auto replaced = [&](const char* section) { return whole || text.contains(QLatin1String(section)); };
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)}, {QStringLiteral("one step to undo"), true}};
    QJsonArray sections;
    if (replaced("<Components>")) {
        sections.append(QStringLiteral("components"));
        QJsonArray list;
        for (Component* c : sch->a_DocComps) {
            if (list.size() >= 200) break;
            list.append(QJsonObject{{QStringLiteral("name"), c->Name}, {QStringLiteral("type"), c->Model}});
        }
        result.insert(QStringLiteral("components"), list);
        result.insert(QStringLiteral("component count"), int(sch->a_DocComps.size()));
    }
    if (replaced("<Wires>")) {
        sections.append(QStringLiteral("wires"));
        result.insert(QStringLiteral("wire count"), int(sch->a_DocWires.size()));
    }
    if (replaced("<Diagrams>")) {
        sections.append(QStringLiteral("diagrams"));
        result.insert(QStringLiteral("diagrams"), diagramsJson(sch));
    }
    if (replaced("<Paintings>")) {
        sections.append(QStringLiteral("paintings"));
        result.insert(QStringLiteral("paintings"), paintingsJson(sch->a_DocPaints, 200));
    }
    result.insert(QStringLiteral("replaced"), sections);
    if (!short_.isEmpty()) result.insert(QStringLiteral("note"), short_.join(QStringLiteral("; ")) + QLatin1Char('.'));
    // The .sch lines' values are read in order: a value in the wrong place -
    // one left out, two swapped - reads as another property's. What does
    // not fit its property is told, as the JSON form's is.
    if (!structured && replaced("<Components>")) {
        for (Component* c : sch->a_DocComps) {
            if (valueNotesOf.size() >= 40) break;
            QStringList names;
            for (const Property* p : c->Props) names << p->Name;
            valueNotesOf << valueNotes(c, names, sch);
        }
        if (!valueNotesOf.isEmpty())
            valueNotesOf << tr("A component line's values are its properties in order (describe_component_type gives it): check "
                               "those lines, or give the parts as 'components', by name");
    }
    if (!valueNotesOf.isEmpty()) result.insert(QStringLiteral("values"), QJsonArray::fromStringList(valueNotesOf));
    return jsonResult(result);
}

QJsonObject QucsControl::describeFormat(const QJsonObject& args)
{
    // The lines of a .sch file, field by field: what set_schematic takes.
    static const QHash<QString, QString> formats{
        {QStringLiteral("component"), QStringLiteral(
             "<Model Name flags x y tx ty mirrored rotation \"value\" shown \"value\" shown ...>\n"
             "Model: the type (R, C, Vdc, J_SPICE, ...; list_component_types). Name: its name, * for none (a ground). "
             "flags: 1 active, 0 off (left out of the netlist), 2 shorted; plus 4 when its name is hidden. x y: its centre. "
             "tx ty: where its texts begin, from the centre. mirrored: 1 mirrored about the x axis. rotation: 0-3, "
             "quarter turns counterclockwise. Then each property in order (describe_component_type gives the order): its "
             "value in quotes, then 1 when it is shown on the schematic, 0 when not; an equation's properties are "
             "\"name=expression\". A quote in a value is written '' and a new line \\n.")},
        {QStringLiteral("wire"), QStringLiteral(
             "<x1 y1 x2 y2 \"label\" labelX labelY distance \"initial\">\n"
             "Its two ends; then its net label: the name (\"\" for none), where the label's text is, how far along the wire "
             "from (x1, y1) the label is fixed, and the node's initial value (\"\" for none). Without a label: "
             "<x1 y1 x2 y2 \"\" 0 0 0 \"\">.")},
        {QStringLiteral("diagram"), QStringLiteral(
             "<Type x y width height flags gridColor gridStyle logs xAuto xMin xStep xMax yAuto yMin yStep yMax "
             "zAuto zMin zStep zMax rotX rotY rotZ notation yUnits zUnits legend decimals [extra] \"xLabel\" \"yLabel\" \"zLabel\" [\"title\" [\"theme\"]]>\n"
             "  <\"variable\" ...> a trace, one line each (see trace), each followed by its markers (see marker)\n"
             "</Type>\n"
             "Type: Rect, Polar, Smith, ySmith, PS, SP, Tab, Time, Truth, Rect3D, Curve (locus), Histogram, Eye. x y: its lower left corner; "
             "width height. flags: 1 grid on, plus 2 to hide the lines (a table). gridColor: #rrggbb; gridStyle: a Qt pen style "
             "(1 solid, 2 dash, 3 dot, ...). logs: two digits written together - the x axis logarithmic (1) or not (0), then "
             "the y and right y axes (1 y, 2 right y, 3 both): \"10\" is a log x axis. Each axis (x, y, right y = z): "
             "automatic (1) or not (0), and its minimum, step and maximum (used when not automatic). rotX rotY rotZ: the "
             "view of a 3D diagram. notation of the numbers: 0 automatic, 1 engineering, 2 scientific, 3 engineering "
             "exponent, 4 decimal, 5 power of ten. yUnits zUnits: of a log y axis, 0 none, 1 dB, 2 dBuV, 3 dBm. legend: 0 off, 1 top left, 2 top "
             "right, 3 bottom left, 4 bottom right. decimals: -1 automatic. extra, a histogram's only: bins, height (0 counts, "
             "1 percent, 2 density), flags (1 normal fit, 2 statistics), lower and upper limit; an eye diagram's: unit "
             "interval, UIs across, from, levels (2 or 4), threshold, drawn (0 density, 1 traces), measurements (1 or 0), "
             "mask width (UI) and height (- for a value not set: automatic, or no mask). The labels last, in quotes "
             "(x, y, right y), then the title in quotes when it has one, then its theme in quotes when a colour is chosen for "
             "a part: \"background=#1e1f22 frame=#8c8f94 x_axis=#c8cacd\" (the parts: background, plot_area, frame, "
             "x_axis, y_axis, y2_axis, title, text, legend_background, legend_border, legend_text; the grid's colour is "
             "gridColor, #c0c0c0 automatic; #aarrggbb see-through; a part not named is automatic). add_diagram and "
             "edit_diagram set all of this by name.")},
        {QStringLiteral("trace"), QStringLiteral(
             "<\"variable\" #rrggbb thickness precision numbers style axis [autoColor [pointMarker]]>\n"
             "variable: as a trace names it (ngspice/tran.v(out)). precision: a table's digits. numbers of complex values in "
             "a table: 0 real/imaginary, 1 magnitude/degrees, 2 magnitude/radians. style: 0 solid, 1 dash, 2 dot, 3 long "
             "dash, 4 stars, 5 circles, 6 arrows. axis: 0 left, 1 right. autoColor: 1 each swept curve a colour of its own. "
             "pointMarker: 0 none, 1 auto, 2 circle, 3 square, 4 triangle, 5 diamond, 6 triangle down, 7 cross, 8 plus. "
             "add_trace and edit_trace set all of this by name.")},
        {QStringLiteral("marker"), QStringLiteral(
             "<Mkr x[/y...] labelX labelY precision numbers transparent [indicator [textColor fillColor [notation]]]>\n"
             "On the line after its trace's. x: the value of the independent variable it marks (and of a swept one after "
             "a /); it shows the sample nearest it. labelX labelY: the top left corner of its box, from the diagram's "
             "lower left corner (y up is negative). precision: significant digits (automatic notation) or places after the point. numbers: 0 real/imaginary, 1 magnitude/degrees, "
             "2 magnitude/radians. transparent: 1 a clear background. indicator: 0 off, 1 square, 2 triangle (the default). "
             "textColor fillColor: #rrggbb or #aarrggbb, - for automatic. notation: its own, as the diagram's codes (left out: "
             "the diagram's). add_marker and edit_marker set all of this, and "
             "place a marker at a peak, 3 dB below it or a crossing.")},
        {QStringLiteral("painting"), QStringLiteral(
             "<Text x y size #rrggbb angle \"text\"> a text (size in points, angle in degrees; \\n a new line); "
             "<Line x y dx dy #rrggbb width style>, <Arrow x y dx dy headLength headWidth #rrggbb width style kind>, "
             "<Rectangle x y width height #rrggbb width style #fillColour fillStyle filled>, "
             "<Ellipse ...> as a rectangle, <EArc x y width height startAngle spanAngle #rrggbb width style> (angles in "
             "sixteenths of a degree), <Image ...>. Shapes in a box share \"x y width height #rrggbb width style "
             "#fillColour fillStyle filled angle mirrored\" (angle 0/90/180/270, counter-clockwise), then their own fields: "
             "<RoundRect ... radius>, <RegPolygon ... kind sides star innerPercent firstCornerDegrees> (kind 0 triangle, "
             "1 polygon, 2 star; a triangle points right), <Brace ... kind> (0 curly, 1 square, 2 round; opens to the "
             "right), <Waveform ... shape cycles dutyPercent baseline> (shape 0 sine, 1 square, 2 triangle, 3 sawtooth, "
             "4 pulse, 5 damped sine), <TextBox ... kind radius padding #textColour size bold align valign pointer tipX "
             "tipY ~text> (kind 0 block, 1 note, 2 callout; align 0 left 1 centre 2 right; valign 0 top 1 middle 2 bottom; "
             "pointer 1: a wedge to the tip), <Table ... rows columns header #headerColour #textColour size align ~cell ...> "
             "(the cells row by row). <Dimension x1 y1 x2 y2 offset #rrggbb width style ends size scale decimals ~unit "
             "~text> (ends 0 arrows, 1 ticks, 2 dots; an empty text: the length times scale, with the unit), <Formula x y "
             "size #rrggbb angle display ~tex> (TeX math, no dollars). What follows a ~ is percent-encoded: %20 a space, "
             "%0A a new line, %22 a quote, %3C < and %3E >. get_schematic lists each painting by its fields.")},
    };
    const QString element = args.value(QLatin1String("element")).toString().trimmed().toLower();
    if (element.isEmpty()) {
        QString all;
        for (const QString& k : {QStringLiteral("component"), QStringLiteral("wire"), QStringLiteral("diagram"), QStringLiteral("trace"),
                                 QStringLiteral("marker"), QStringLiteral("painting")})
            all += QStringLiteral("%1:\n%2\n\n").arg(k, formats.value(k));
        return textResult(all + paintingFieldsText());
    }
    if (!formats.contains(element))
        return errorResult(tr("'element' is component, wire, diagram, trace, marker or painting."));
    if (element == QLatin1String("painting")) return textResult(formats.value(element) + QStringLiteral("\n\n") + paintingFieldsText());
    return textResult(formats.value(element));
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
    // Equations first: a property given may be one of them.
    if (!setListsOf(c, args, &error, false, true) || !setProperties(c, args.value(QLatin1String("properties")).toObject(), &error, true)
        || !setTextOf(c, args, &error, false)) {
        delete c;
        return errorResult(error);
    }
    const QString wanted = args.value(QLatin1String("name")).toString().trimmed();
    if (!wanted.isEmpty() && sch->getComponentByName(wanted) != nullptr) {
        delete c;
        return errorResult(tr("There is a component named %1 already.").arg(wanted));
    }
    if (const QString bad = wanted.isEmpty() ? QString() : badPartName(wanted); !bad.isEmpty()) {
        delete c;
        return errorResult(bad + QLatin1Char('.'));
    }
    if (const QStringList typos = numberTypos(wanted.isEmpty() ? type : wanted, type, args.value(QLatin1String("properties")).toObject());
        !typos.isEmpty()) {
        delete c;
        return errorResult(tr("Not added: %1.").arg(typos.join(QStringLiteral("; "))));
    }
    // A library part that is one component (a varactor: a Diode with the
    // library's values) is that component, as the library panel places it.
    const QString modelOf = asItsModel(c, sch);
    c->recreate();   // the symbol, with the properties
    // A library part not found (its library, or the part in it): no symbol,
    // no pins - placed so, nothing was said.
    if (c->Model == QLatin1String("Lib") && c->Ports.isEmpty()) {
        const QString lib = c->Props.value(0) != nullptr ? c->Props.at(0)->Value : QString();
        const QString comp = c->Props.value(1) != nullptr ? c->Props.at(1)->Value : QString();
        delete c;
        return errorResult(tr("There is no part %1 in a library %2 here (Lib and Comp name them): find_library_component finds "
                              "the parts there are.").arg(comp, lib));
    }
    // 'pin1': the turn that puts pin 1 of a part of two pins on that side.
    int turns = args.value(QLatin1String("rotation")).toInt();
    if (args.contains(QLatin1String("pin1"))) {
        if (args.contains(QLatin1String("rotation"))) {
            delete c;
            return errorResult(tr("Give 'rotation' or 'pin1', not both."));
        }
        turns = rotationForPin1(c, args.value(QLatin1String("pin1")).toString().trimmed().toLower(), args.value(QLatin1String("mirror")).toBool(), &error);
        if (turns < 0) {
            delete c;
            return errorResult(error);
        }
    }
    prepare(sch);
    orient(c, turns, args.value(QLatin1String("mirror")).toBool());
    int x = args.value(QLatin1String("x")).toInt(), y = args.value(QLatin1String("y")).toInt();
    // Or beside another part: "Rf below U1".
    QString nearHow;
    if (args.contains(QLatin1String("near"))) {
        QPoint there;
        if (args.contains(QLatin1String("x")) || args.contains(QLatin1String("y"))
            || !placeNear(sch, c, args.value(QLatin1String("near")), &there, &error, &nearHow)) {
            delete c;
            return errorResult(error.isEmpty() ? tr("Give x, y or 'near', not both.") : error);
        }
        x = there.x();
        y = there.y();
    }
    const QPoint at = Schematic::withinModelLimit(QPoint(x, y));
    x = at.x();
    y = at.y();
    sch->setOnGrid(x, y);
    c->moveCenter(x - c->cx, y - c->cy);
    // What its pins come down on - a wire, another pin, a wire's end - is
    // joined to them: said, as it joins nets without a wire drawn.
    QList<int> landing;
    for (int i = 0; i < c->Ports.size(); ++i) {
        const QPoint p(c->cx + c->Ports.at(i)->x, c->cy + c->Ports.at(i)->y);
        bool on = false;
        for (const Node* n : sch->a_DocNodes) on = on || n->center() == p;
        for (const Wire* w : sch->a_DocWires) on = on || onSegment(p, w->P1(), w->P2());
        if (on) landing << i;
    }
    const QList<qucs_s::erc::Issue> wiringBefore = qucs_s::erc::wiring(sch);
    // As a click with the component does.
    int x1, y1, x2, y2;
    c->textSize(x1, y1);
    sch->insertComponent(c);
    c->textSize(x2, y2);
    if (c->tx < c->x1) c->tx -= x2 - x1;
    if (!wanted.isEmpty()) c->Name = wanted;
    setTextOf(c, args, &error);
    // Its text where it is clear: its type's place, or the nearest free
    // spot beside it - unless it was given.
    const bool textGiven = args.contains(QLatin1String("text_at")) && args.value(QLatin1String("text_at")).toString() != QLatin1String("auto");
    const QString textPlaced = textGiven ? QString() : placeTextClear(sch, c, true);
    sch->enlargeView(c);
    finish(sch, {QPoint(c->cx, c->cy)});
    QJsonObject result = componentJson(c);
    if (!textPlaced.isEmpty()) result.insert(QStringLiteral("text"), textPlaced);
    // An equation block's equations as they are now, as get_schematic gives
    // and 'equations' takes them: what came of what was given, at a look.
    if (isEquationKind(c)) result.insert(QStringLiteral("equations"), componentModel(c).value(QLatin1String("equations")));
    if (const QString trap = trapOf(c->Model); !trap.isEmpty()) result.insert(QStringLiteral("watch"), trap);
    if (const QStringList values = valueNotes(c, args.value(QLatin1String("properties")).toObject().keys(), sch); !values.isEmpty())
        result.insert(QStringLiteral("values"), QJsonArray::fromStringList(values));
    QStringList notes;
    if (!landing.isEmpty()) {
        const Nets nets = netsOf(sch, nullptr);
        QHash<QString, int> seen;
        QString base;
        for (Component* pc : sch->a_DocComps) {
            const QString k = keyBase(pc, seen);
            if (pc == c) base = k;
        }
        for (int i : std::as_const(landing)) {
            const int net = nets.netOf.value(QStringLiteral("%1.%2").arg(base).arg(i + 1), -1);
            QStringList on;
            for (auto it = nets.netOf.cbegin(); it != nets.netOf.cend(); ++it)
                if (it.value() == net && !it.key().startsWith(base + QLatin1Char('.')) && on.size() < 4) on << said(it.key(), {});
            notes << (on.isEmpty() ? tr("pin %1 came down on a wire and is joined to it").arg(i + 1)
                                   : tr("pin %1 came down on what was there and is joined to %2").arg(i + 1).arg(on.join(QStringLiteral(", "))));
        }
    }
    notes << newWiringIssues(sch, wiringBefore);
    if (!modelOf.isEmpty())
        notes.prepend(tr("%1 is a %2 with the library's values: placed as one, as the library panel places it (its "
                         "values are its properties)").arg(modelOf, c->Model));
    if (!nearHow.isEmpty()) notes.prepend(tr("Placed %1").arg(nearHow));
    if (!notes.isEmpty()) result.insert(QStringLiteral("note"), notes.join(QStringLiteral("; ")) + QLatin1Char('.'));
    return jsonResult(result);
}

QJsonObject QucsControl::editComponent(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    Component* c = componentOf(sch, name, &error);
    if (c == nullptr && !toldByNumber(sch, name))
        return errorResult(tr("There is no component %1 in %2.").arg(name, titleOf(sch)));
    if (c == nullptr) return errorResult(error);
    // Beside another part ("Rf below U1"): moved there, as x, y move it.
    QJsonObject placed = args;
    QString nearHow;
    if (args.contains(QLatin1String("near"))) {
        if (args.contains(QLatin1String("x")) || args.contains(QLatin1String("y"))) return errorResult(tr("Give x, y or 'near', not both."));
        QPoint there;
        if (!placeNear(sch, c, args.value(QLatin1String("near")), &there, &error, &nearHow)) return errorResult(error);
        placed.insert(QStringLiteral("x"), there.x());
        placed.insert(QStringLiteral("y"), there.y());
    }
    // A part without a name (a ground): by its ref, GND#2.
    const bool unnamed = c->Name.isEmpty();
    int named = 0;
    for (Component* pc : sch->a_DocComps) named += !unnamed && pc->Name.compare(name, Qt::CaseInsensitive) == 0 ? 1 : 0;
    if (named > 1) return errorResult(tr("There are %1 components named %2 in %3.").arg(named).arg(name, titleOf(sch)));
    const QString rename = args.value(QLatin1String("rename")).toString().trimmed();
    if (unnamed && !rename.isEmpty()) return errorResult(tr("A %1 has no name to change.").arg(c->Model));
    if (!rename.isEmpty() && rename != name && sch->getComponentByName(rename) != nullptr)
        return errorResult(tr("There is a component named %1 already.").arg(rename));
    if (const QString bad = rename.isEmpty() || rename == name ? QString() : badPartName(rename); !bad.isEmpty())
        return errorResult(bad + QLatin1Char('.'));
    // Check the properties before anything changes (one a block may have
    // any number of, none yet too: its first constraint).
    const QJsonObject props = args.value(QLatin1String("properties")).toObject();
    for (auto it = props.begin(); it != props.end(); ++it)
        if (c->getProperty(it.key()) == nullptr && !isRepeated(c, it.key())) return errorResult(noSuchProperty(c, it.key()));
    if (const QStringList typos = numberTypos(name, c->Model, props); !typos.isEmpty())
        return errorResult(tr("Not changed: %1.").arg(typos.join(QStringLiteral("; "))));
    // A library part made one that is one component (a varactor's Diode):
    // a Lib cannot be that - replace_component places the component.
    if (c->Model == QLatin1String("Lib") && c->Props.size() >= 2 && (props.contains(QLatin1String("Lib")) || props.contains(QLatin1String("Comp")))) {
        LibComp probe;
        probe.setSchematic(sch);
        probe.Props.at(0)->Value = props.value(QLatin1String("Lib")).toString(c->Props.at(0)->Value);
        probe.Props.at(1)->Value = props.value(QLatin1String("Comp")).toString(c->Props.at(1)->Value);
        if (!probe.componentModel().isEmpty())
            return errorResult(tr("%1/%2 is one component with the library's values, not a part a Lib can be: replace_component "
                                  "%3 with type Lib and those properties places it as that component.")
                                   .arg(probe.Props.at(0)->Value, probe.Props.at(1)->Value, name));
    }
    if (!setTextOf(c, args, &error, false) || !setListsOf(c, args, &error, true)) return errorResult(error);
    // 'pin1': the turn that puts its pin 1 on that side, tried on a part of
    // its type and values.
    if (args.contains(QLatin1String("pin1"))) {
        if (args.contains(QLatin1String("rotation"))) return errorResult(tr("Give 'rotation' or 'pin1', not both."));
        std::unique_ptr<Component> probe(newComponent(c->Model));
        if (!probe) return errorResult(tr("%1 cannot be turned by 'pin1': give 'rotation'.").arg(name));
        probe->setSchematic(sch);
        for (const Property* p : c->Props)
            if (Property* q = probe->getProperty(p->Name)) q->Value = p->Value;
        probe->recreate();
        const int turns = rotationForPin1(probe.get(), args.value(QLatin1String("pin1")).toString().trimmed().toLower(),
                                          args.contains(QLatin1String("mirror")) ? args.value(QLatin1String("mirror")).toBool() : c->mirroredX, &error);
        if (turns < 0) return errorResult(error);
        placed.insert(QStringLiteral("rotation"), turns);
    }
    prepare(sch);
    const QString before = sch->snapshot();
    const QList<qucs_s::erc::Issue> wiringBefore = qucs_s::erc::wiring(sch);
    if (!props.isEmpty() || givesLists(args)) {
        setProperties(c, props, &error);
        setListsOf(c, args, &error, false);
        sch->recreateComponent(c);
    }
    QStringList landed;
    if (placed.contains(QLatin1String("x")) || placed.contains(QLatin1String("y")) || placed.contains(QLatin1String("rotation"))
        || args.contains(QLatin1String("mirror"))) {
        QString why;
        // turnAndMove() finds the part by its name: one without has one
        // while it is turned and moved.
        const QString key = unnamed ? QStringLiteral("%1 being moved").arg(c->Model) : c->Name;
        c->Name = key;
        c = turnAndMove(sch, key, placed, &why, &landed);
        if (c != nullptr && unnamed) c->Name.clear();
        if (c == nullptr) {
            sch->restore(before);
            return errorResult(tr("%1 is not changed: turned or moved so, %2. Try another place or turn - or take "
                                  "its wires away (delete), change it and connect it again.")
                                   .arg(name, why));
        }
    }
    if (args.contains(QLatin1String("active")))
        c->isActive = args.value(QLatin1String("active")).toBool() ? COMP_IS_ACTIVE : COMP_IS_OPEN;
    // Renamed: what names it follows - traces, equations, a closed data display.
    QStringList renamedToo;
    QString rewritten;
    if (!rename.isEmpty() && rename != c->Name) {
        const QString was = c->Name;
        c->Name = rename;
        renameEverywhere(sch, [&](const QString& text) { return renameComponentIn(text, was, rename); }, &renamedToo, &rewritten);
        if (!renamedToo.isEmpty() || !rewritten.isEmpty()) sch->reloadGraphs();
    }
    setTextOf(c, args, &error);
    // "auto": the nearest free spot beside it, when where it is is not.
    // Moved, turned or with other texts shown, and its text now drawn over
    // something: said.
    QString textPlaced;
    if (args.value(QLatin1String("text_at")).toString() == QLatin1String("auto")) {
        textPlaced = placeTextClear(sch, c, true);
        if (textPlaced.isEmpty()) textPlaced = tr("its text is clear where it is: left there");
    } else if (!args.contains(QLatin1String("text_at"))
               && (placed.contains(QLatin1String("x")) || placed.contains(QLatin1String("y")) || placed.contains(QLatin1String("rotation"))
                   || args.contains(QLatin1String("mirror")) || args.contains(QLatin1String("shown")) || args.contains(QLatin1String("name_shown"))
                   || !props.isEmpty() || !rename.isEmpty())) {
        if (const QStringList over = qucs_s::textplace::overlapped(c, qucs_s::textplace::things(sch)); !over.isEmpty())
            textPlaced = tr("its text overlaps %1 - text_at \"auto\" moves it clear").arg(over.mid(0, 3).join(QStringLiteral(", ")));
    }
    sch->enlargeView(c);
    finish(sch, {QPoint(c->cx, c->cy)});
    QJsonObject result = componentJson(c);
    if (!textPlaced.isEmpty()) result.insert(QStringLiteral("text"), textPlaced);
    if (unnamed) result.insert(QStringLiteral("ref"), refOf(sch, c));   // (another number, when it was taken up and put down)
    if (isEquationKind(c)) result.insert(QStringLiteral("equations"), componentModel(c).value(QLatin1String("equations")));
    if (const QStringList values = valueNotes(c, props.keys(), sch); !values.isEmpty())
        result.insert(QStringLiteral("values"), QJsonArray::fromStringList(values));
    if (!renamedToo.isEmpty()) result.insert(QStringLiteral("renamed too"), QJsonArray::fromStringList(renamedToo));
    if (!rewritten.isEmpty()) result.insert(QStringLiteral("rewritten"), rewritten);
    if (!renamedToo.isEmpty() || !rewritten.isEmpty())
        result.insert(QStringLiteral("dataset"), tr("The dataset still names it %1: the traces show it again after the next simulation.")
                                                     .arg(name));
    landed << newWiringIssues(sch, wiringBefore);
    if (!nearHow.isEmpty()) landed.prepend(tr("Moved %1").arg(nearHow));
    if (!landed.isEmpty()) result.insert(QStringLiteral("note"), landed.join(QStringLiteral("; ")) + QLatin1Char('.'));
    return jsonResult(result);
}

namespace {

// What a pin is by its name - an op-amp's inputs, output and supplies, as
// libraries name them - or, for a built-in part whose pins have none, by
// its place in the part (the OpAmp: 1 the - input, 2 the +, 3 the output).
QString pinRole(const Component* c, int i)
{
    if (c->Model == QLatin1String("OpAmp") && c->Ports.size() == 3) return QStringList{"inn", "inp", "out"}.at(i);
    // (IN_N1, VCC2: a unit's number after it.)
    static const QRegularExpression unit(QStringLiteral("(?<=[a-z+-])\\d+$"));
    const QString n = c->Ports.at(i)->Name.trimmed().toLower().remove(QLatin1Char('_')).remove(unit);
    if (n.isEmpty()) return {};
    static const QHash<QString, QString> roles{
        {"inn", "inn"}, {"in-", "inn"}, {"-in", "inn"}, {"inm", "inn"}, {"vin-", "inn"}, {"vinn", "inn"}, {"inneg", "inn"}, {"-", "inn"},
        {"inp", "inp"}, {"in+", "inp"}, {"+in", "inp"}, {"vin+", "inp"}, {"vinp", "inp"}, {"inpos", "inp"}, {"+", "inp"},
        {"out", "out"}, {"vout", "out"}, {"output", "out"},
        {"vcc", "pos"}, {"vdd", "pos"}, {"v+", "pos"}, {"vs+", "pos"}, {"vpos", "pos"},
        {"vee", "neg"}, {"vss", "neg"}, {"v-", "neg"}, {"vs-", "neg"}, {"vneg", "neg"},
        {"gnd", "gnd"}, {"ground", "gnd"}, {"agnd", "gnd"}};
    return roles.value(n);
}

// The old part as its type draws it, not turned or mirrored: the sides of
// its pins as they are on its own symbol.
std::unique_ptr<Component> unturned(const Component* old, Schematic* sch)
{
    std::unique_ptr<Component> c(const_cast<Component*>(old)->newOne());
    if (!c) return {};
    c->setSchematic(sch);
    for (int i = 0; i < c->Props.size() && i < old->Props.size(); ++i) c->Props.at(i)->Value = old->Props.at(i)->Value;
    c->recreate();
    if (c->Ports.size() != old->Ports.size()) return {};
    return c;
}

// Old pin to new pin, as best told: the same role (by name, or the
// OpAmp's by number), else the same place on the symbol (left, upper),
// else the same side in the same order along it. The old pins told.
QHash<int, int> guessedPins(const Component* old, const Component* fresh, Schematic* sch)
{
    QHash<int, int> map;
    std::unique_ptr<Component> plain = unturned(old, sch);
    const Component* was = plain ? plain.get() : old;
    QSet<int> taken;
    const auto take = [&](int a, int b) {
        map.insert(a, b);
        taken.insert(b);
    };
    // The one pin of \a fresh that \a key gives, not taken, or -1.
    const auto only = [&](const std::function<QString(const Component*, int)>& key, int a) {
        const QString k = key(was, a);
        if (k.isEmpty()) return -1;
        int found = -1;
        for (int b = 0; b < fresh->Ports.size(); ++b) {
            if (taken.contains(b) || key(fresh, b) != k) continue;
            if (found >= 0) return -1;
            found = b;
        }
        return found;
    };
    for (const auto& key : {std::function<QString(const Component*, int)>(pinRole),
                            std::function<QString(const Component*, int)>(pinSide)})
        for (int a = 0; a < was->Ports.size(); ++a)
            if (!map.contains(a))
                if (const int b = only(key, a); b >= 0) take(a, b);
    // Then side by side, in order along it (top to bottom, left to right).
    const auto sideOf = [](const Component* c, int i) { return pinSide(c, i).section(QLatin1Char(','), 0, 0); };
    const auto along = [](const Component* c, int i) { return c->Ports.at(i)->y * 10000 + c->Ports.at(i)->x; };
    QHash<QString, QList<int>> olds, news;
    for (int a = 0; a < was->Ports.size(); ++a)
        if (!map.contains(a)) olds[sideOf(was, a)] << a;
    for (int b = 0; b < fresh->Ports.size(); ++b)
        if (!taken.contains(b)) news[sideOf(fresh, b)] << b;
    for (auto it = olds.begin(); it != olds.end(); ++it) {
        QList<int> a = it.value(), b = news.value(it.key());
        if (a.size() != b.size()) continue;
        std::sort(a.begin(), a.end(), [&](int x, int y) { return along(was, x) < along(was, y); });
        std::sort(b.begin(), b.end(), [&](int x, int y) { return along(fresh, x) < along(fresh, y); });
        for (int k = 0; k < a.size(); ++k) take(a.at(k), b.at(k));
    }
    return map;
}

// A pin map as 'pins' takes it back: {"2": "INP", ...}, each new pin by
// its name when it has one.
QString pinsArgument(const QHash<int, int>& map, const Component* fresh)
{
    QList<int> olds = map.keys();
    std::sort(olds.begin(), olds.end());
    QStringList pairs;
    for (int a : olds) {
        const int b = map.value(a);
        const QString to = fresh->Ports.at(b)->Name.isEmpty() ? QString::number(b + 1) : QStringLiteral("\"%1\"").arg(fresh->Ports.at(b)->Name);
        pairs << QStringLiteral("\"%1\": %2").arg(a + 1).arg(to);
    }
    return QLatin1Char('{') + pairs.join(QStringLiteral(", ")) + QLatin1Char('}');
}

// How an old pin is said: its number, and its role or name.
QString pinSaid(const Component* c, int i)
{
    static const QHash<QString, QString> words{{"inn", QObject::tr("the - input")}, {"inp", QObject::tr("the + input")},
                                               {"out", QObject::tr("the output")}};
    if (!c->Ports.at(i)->Name.isEmpty()) return QStringLiteral("%1 (%2)").arg(i + 1).arg(c->Ports.at(i)->Name);
    const QString role = pinRole(c, i);
    return words.contains(role) ? QStringLiteral("%1 (%2)").arg(i + 1).arg(words.value(role)) : QString::number(i + 1);
}

} // namespace

// A part put in another's place, of another type (a built-in OpAmp for a
// subcircuit, say), its pins taking the old pins' nets: 'pins' maps them
// (by number or name; else by name when the old pins' names are all the
// new part's, else by number), and the new part is turned, mirrored and
// placed so that the pins land where the old ones were - of the ways that
// keep every net, the one that needs the least wire.
QJsonObject QucsControl::replaceComponent(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    Component* old = sch->getComponentByName(name);
    if (old == nullptr || name.isEmpty()) return errorResult(tr("There is no component %1 in %2.").arg(name, titleOf(sch)));
    int named = 0;
    for (Component* pc : sch->a_DocComps) named += pc->Name.compare(name, Qt::CaseInsensitive) == 0 ? 1 : 0;
    if (named > 1) return errorResult(tr("There are %1 components named %2 in %3.").arg(named).arg(name, titleOf(sch)));
    const QString type = args.value(QLatin1String("type")).toString().trimmed();
    const QString newName = args.value(QLatin1String("rename")).toString().trimmed().isEmpty()
                                ? name : args.value(QLatin1String("rename")).toString().trimmed();
    if (newName != name && sch->getComponentByName(newName) != nullptr)
        return errorResult(tr("There is a component named %1 already.").arg(newName));
    if (const QString bad = newName == name ? QString() : badPartName(newName); !bad.isEmpty()) return errorResult(bad + QLatin1Char('.'));
    const QJsonObject props = args.value(QLatin1String("properties")).toObject();
    // A new part as asked, turned \a turns and mirrored or not.
    const auto make = [&](int turns, bool mirror, QString* why) -> Component* {
        Component* c = newComponent(type);
        if (c == nullptr) {
            *why = tr("There is no component type %1 (list_component_types lists them).").arg(type);
            return nullptr;
        }
        c->setSchematic(sch);
        if (!setListsOf(c, args, why, false, true) || !setProperties(c, props, why, true)) {
            delete c;
            return nullptr;
        }
        asItsModel(c, sch);   // (a library part that is one component: that component)
        c->recreate();
        orient(c, turns, mirror);
        return c;
    };
    std::unique_ptr<Component> sample(make(0, false, &error));
    if (!sample) return errorResult(error);
    if (const QStringList typos = numberTypos(newName, type, props); !typos.isEmpty())
        return errorResult(tr("Not replaced: %1.").arg(typos.join(QStringLiteral("; "))));
    // A block for a block - an analysis, an equation or a statistics block
    // for another (a Monte Carlo's records to corners): neither has a pin,
    // nor a net to keep. In its place, or at x, y.
    if (sample->Ports.isEmpty() && old->Ports.isEmpty()) {
        const QPoint centre = args.value(QLatin1String("x")).isDouble() && args.value(QLatin1String("y")).isDouble()
                                  ? Schematic::withinModelLimit(QPoint(args.value(QLatin1String("x")).toInt(), args.value(QLatin1String("y")).toInt()))
                                  : QPoint(old->cx, old->cy);
        prepare(sch);
        QStringList landed;
        Component* c = replaceWith(sch, name, sample.release(), centre, newName, {}, &error, &landed);
        if (c == nullptr) return errorResult(tr("%1 is not replaced: %2").arg(name, error));
        setTextOf(c, args, &error);
        finish(sch, {centre});
        QJsonObject result = componentJson(c);
        result.insert(QStringLiteral("placed"), tr("where %1 was: neither has pins").arg(name));
        return jsonResult(result);
    }
    if (sample->Ports.isEmpty()) return errorResult(tr("%1 has no pins: nothing would take %2's nets.").arg(type, name));

    // Which old pin goes to which new one.
    const auto pinIndex = [](const Component* c, const QJsonValue& v) {
        const QString s = v.isDouble() ? QString::number(v.toInt()) : v.toString().trimmed();
        bool number = false;
        const int n = s.toInt(&number);
        if (number) return n >= 1 && n <= c->Ports.size() ? n - 1 : -1;
        for (int i = 0; i < c->Ports.size(); ++i)
            if (!c->Ports.at(i)->Name.isEmpty() && c->Ports.at(i)->Name.compare(s, Qt::CaseInsensitive) == 0) return i;
        return -1;
    };
    const auto pinList = [](const Component* c) {
        QStringList names;
        for (int i = 0; i < c->Ports.size(); ++i)
            names << (c->Ports.at(i)->Name.isEmpty() ? QString::number(i + 1)
                                                     : QStringLiteral("%1 (%2)").arg(i + 1).arg(c->Ports.at(i)->Name));
        return names.join(QStringLiteral(", "));
    };
    // (Each pin with the side of the symbol it is on: pinSides().)
    QHash<int, int> pins;   // old index -> new index
    QString mappedBy;
    if (args.value(QLatin1String("pins")).toString().trimmed().compare(QLatin1String("by number"), Qt::CaseInsensitive) == 0) {
        for (int i = 0; i < std::min<qsizetype>(old->Ports.size(), sample->Ports.size()); ++i) pins.insert(i, i);
        mappedBy = tr("by number, as 'pins' says");
    } else if (args.contains(QLatin1String("pins"))) {
        const QJsonValue v = args.value(QLatin1String("pins"));
        QList<QPair<QJsonValue, QJsonValue>> pairs;
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            for (auto it = o.begin(); it != o.end(); ++it) pairs << qMakePair(QJsonValue(it.key()), it.value());
        }
        else if (v.isArray())
            for (const QJsonValue& e : v.toArray())
                if (e.isArray() && e.toArray().size() == 2) pairs << qMakePair(e.toArray().at(0), e.toArray().at(1));
        if (pairs.isEmpty())
            return errorResult(tr("'pins' maps the old part's pins to the new one's, by number or name: {\"1\": \"inn\", \"2\": \"inp\", \"3\": \"out\"} "
                                  "- or \"by number\"."));
        for (const auto& [from, to] : std::as_const(pairs)) {
            const int a = pinIndex(old, from), b = pinIndex(sample.get(), to);
            if (a < 0) return errorResult(tr("%1 has no pin %2 (its pins: %3).").arg(name, from.toVariant().toString(), pinList(old)));
            if (b < 0) return errorResult(tr("%1 has no pin %2 (its pins: %3).").arg(type, to.toVariant().toString(), pinList(sample.get())));
            if (pins.contains(a) || std::find(pins.cbegin(), pins.cend(), b) != pins.cend())
                return errorResult(tr("'pins' maps a pin twice (%1 to %2).").arg(from.toVariant().toString(), to.toVariant().toString()));
            pins.insert(a, b);
        }
        mappedBy = tr("as 'pins' says");
    } else {
        // By name, when the old pins' names are all the new part's.
        bool byName = true;
        int namedPins = 0;
        for (int i = 0; i < old->Ports.size() && byName; ++i) {
            const QString n = old->Ports.at(i)->Name;
            if (n.isEmpty()) continue;
            ++namedPins;
            const int b = pinIndex(sample.get(), n);
            if (b < 0) byName = false;
            else pins.insert(i, b);
        }
        // Named pins with something on them that the new part has no such
        // names for: by number, pin 2 (INP) went to the transistor-level
        // uA741's pin 2, its output, and the amplifier sat at a rail with
        // nothing said. Which is which, and 'pins' to say it.
        QStringList namedWired;
        for (int i = 0; i < old->Ports.size(); ++i) {
            const Port* p = old->Ports.at(i);
            if (!p->Name.isEmpty() && p->Connection != nullptr && (p->Connection->conn_count() > 1 || p->Connection->hasLabel())
                && pinIndex(sample.get(), p->Name) < 0)
                namedWired << p->Name;
        }
        // The best guess, by role and place, to pass back as 'pins' - for
        // the old pins with something on them.
        QHash<int, int> guess = guessedPins(old, sample.get(), sch);
        for (int i = 0; i < old->Ports.size(); ++i) {
            const Port* p = old->Ports.at(i);
            if (!(p->Connection != nullptr && (p->Connection->conn_count() > 1 || p->Connection->hasLabel()))) guess.remove(i);
        }
        const auto guessText = [&] {
            return guess.isEmpty() ? QString() : tr(" By their roles and places on the symbols: 'pins': %1.").arg(pinsArgument(guess, sample.get()));
        };
        if (!byName && !namedWired.isEmpty())
            return errorResult(tr("%1's pins have names the new part's do not (%2): by number they could go to other pins than meant. "
                                  "Its pins: %3. The new part's: %4.%5 Say which is which in 'pins' ({\"%6\": 2, ...}), or \"pins\": "
                                  "\"by number\" when the numbers match.")
                                   .arg(name, namedWired.join(QStringLiteral(", ")), pinSides(old), pinSides(sample.get()), guessText(),
                                        namedWired.first()));
        if (!byName || namedPins == 0) {
            pins.clear();
            for (int i = 0; i < std::min<qsizetype>(old->Ports.size(), sample->Ports.size()); ++i) pins.insert(i, i);
            mappedBy = tr("by number");
            // Unnamed pins onto named ones (the built-in OpAmp onto a
            // library 741: its + input, pin 2, went to the 741's pin 2, the
            // output): by number only when that is what their roles and
            // places say too.
            bool newNamed = false;
            for (const Port* p : sample->Ports) newNamed = newNamed || !p->Name.isEmpty();
            QStringList differ;
            QList<int> told = guess.keys();
            std::sort(told.begin(), told.end());
            for (int a : std::as_const(told)) {
                const int byNumber = pins.value(a, -1), meant = guess.value(a);
                if (byNumber == meant) continue;
                differ << (byNumber < 0 ? tr("%1 would go to no pin, not %2").arg(pinSaid(old, a), pinSaid(sample.get(), meant))
                                        : tr("%1 would go to %2, not %3").arg(pinSaid(old, a), pinSaid(sample.get(), byNumber),
                                                                             pinSaid(sample.get(), meant)));
            }
            if (namedPins == 0 && newNamed && !differ.isEmpty())
                return errorResult(tr("%1's pins have no names and the new part's have: by number %2. Its pins: %3. The new part's: %4.%5 "
                                      "Give that (or your own) as 'pins', or \"pins\": \"by number\" if the numbers are meant.")
                                       .arg(name, differ.join(QStringLiteral("; ")), pinSides(old), pinSides(sample.get()), guessText()));
        } else {
            mappedBy = tr("by name");
        }
    }
    // The old pins' places, and which have something on them.
    const QPoint oldCentre = old->center();
    QList<QPoint> oldAt;
    QList<bool> oldWired;
    for (const Port* p : old->Ports) {
        oldAt << oldCentre + QPoint(p->x, p->y);
        oldWired << (p->Connection != nullptr && (p->Connection->conn_count() > 1 || p->Connection->hasLabel()));
    }
    QStringList unmapped;   // old pins with something on them that no new pin takes
    for (int i = 0; i < old->Ports.size(); ++i)
        if (oldWired.at(i) && !pins.contains(i))
            unmapped << QStringLiteral("%1.%2%3").arg(name).arg(i + 1)
                            .arg(old->Ports.at(i)->Name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(old->Ports.at(i)->Name));
    if (!unmapped.isEmpty())
        return errorResult(tr("%1 has something on %2, which no pin of the new part takes: map it in 'pins', or take its wiring "
                              "away first (delete). The new part's pins: %3.")
                               .arg(name, unmapped.join(QStringLiteral(", ")), pinList(sample.get())));

    // The ways to put it: each turn and mirroring (those given alone), at
    // the old centre or so that one of the pins lands on its old place, and
    // each of those a few grid steps aside (at 'x', 'y' alone when given) -
    // tried in this order: the fewest parts under it, the fewest pins off
    // their places, the least distance, the fewest old places to wire to
    // under it, the old part's own turn.
    const int oldTurns = ((old->rotated - defaultRotation(old->Model)) % 4 + 4) % 4;
    const bool oldMirror = old->mirroredX;
    struct Way {
        int turns;
        bool mirror;
        QPoint centre;
        int covered;    // old places to wire to under the new symbol: a wire would go across it
        int overlaps;   // other parts under it
        int off;
        qint64 distance;
        int unlike;
    };
    QList<Way> ways;
    const bool turnGiven = args.contains(QLatin1String("rotation")), mirrorGiven = args.contains(QLatin1String("mirror"));
    const bool placeGiven = args.contains(QLatin1String("x")) || args.contains(QLatin1String("y"));
    for (int turns = 0; turns < 4; ++turns)
        for (bool mirror : {false, true}) {
            if (turnGiven && turns != ((args.value(QLatin1String("rotation")).toInt() % 4) + 4) % 4) continue;
            if (mirrorGiven && mirror != args.value(QLatin1String("mirror")).toBool()) continue;
            std::unique_ptr<Component> probe(make(turns, mirror, &error));
            if (!probe) return errorResult(error);
            QList<QPoint> centres;
            if (placeGiven) {
                int x = args.contains(QLatin1String("x")) ? args.value(QLatin1String("x")).toInt() : oldCentre.x();
                int y = args.contains(QLatin1String("y")) ? args.value(QLatin1String("y")).toInt() : oldCentre.y();
                sch->setOnGrid(x, y);
                centres << QPoint(x, y);
            } else {
                QList<QPoint> base{oldCentre};
                for (auto it = pins.constBegin(); it != pins.constEnd(); ++it)
                    if (oldWired.at(it.key())) {
                        const Port* p = probe->Ports.at(it.value());
                        const QPoint c = oldAt.at(it.key()) - QPoint(p->x, p->y);
                        if (!base.contains(c)) base << c;
                    }
                // ...and each a few grid steps aside: a part larger than
                // the old one clear of the old pins' places.
                const int gx = 2 * std::max(sch->getGridX(), 1), gy = 2 * std::max(sch->getGridY(), 1);
                for (const QPoint& b : std::as_const(base))
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx)
                            if (const QPoint c = b + QPoint(dx * gx, dy * gy); !centres.contains(c)) centres << c;
            }
            const QRect bounds = QRect(QPoint(probe->x1, probe->y1), QPoint(probe->x2, probe->y2)).normalized();
            for (const QPoint& c : std::as_const(centres)) {
                Way w{turns, mirror, c, 0, 0, 0, 0, (turns != oldTurns ? 1 : 0) + (mirror != oldMirror ? 1 : 0)};
                const QRect body = bounds.translated(c).adjusted(3, 3, -3, -3);
                for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) {
                    if (!oldWired.at(it.key())) continue;
                    const Port* p = probe->Ports.at(it.value());
                    const QPoint d = c + QPoint(p->x, p->y) - oldAt.at(it.key());
                    if (!d.isNull()) {
                        ++w.off;
                        if (body.contains(oldAt.at(it.key()))) ++w.covered;
                    }
                    w.distance += std::abs(d.x()) + std::abs(d.y());
                }
                for (const Component* other : sch->a_DocComps)
                    if (other != old && other->boundingRect().intersects(body)) ++w.overlaps;
                w.distance += (std::abs(c.x() - oldCentre.x()) + std::abs(c.y() - oldCentre.y())) / 4;
                ways << w;
            }
        }
    std::stable_sort(ways.begin(), ways.end(), [](const Way& a, const Way& b) {
        return std::tie(a.overlaps, a.off, a.distance, a.covered, a.unlike)
             < std::tie(b.overlaps, b.off, b.distance, b.covered, b.unlike);
    });
    if (ways.size() > 40) ways = ways.mid(0, 40);

    prepare(sch);
    const QString state = sch->snapshot();
    const QList<qucs_s::erc::Issue> wiringBefore = qucs_s::erc::wiring(sch);
    // How a way came out: the wires across the new symbol (drawn over it,
    // hard to read), and all the wire there is.
    const auto across = [sch](const Component* part) { return wiresAcross(sch, part); };
    const auto wireLength = [sch] {
        qint64 length = 0;
        for (const Wire* w : sch->a_DocWires) length += std::abs(w->x2 - w->x1) + std::abs(w->y2 - w->y1);
        return length;
    };
    QStringList landed, faults;
    Component* c = nullptr;
    Way used{};
    // The first way that keeps the nets with no wire across the symbol and
    // nothing new for Check Schematic (wires of two nets crossing); else, of
    // the first few that keep them, the one with the fewest of each, then
    // the least wire - made again.
    struct Outcome {
        Way way;
        int across;
        int issues;
        qint64 length;
    };
    std::optional<Outcome> best;
    int kept = 0;
    const auto attempt = [&](const Way& w, QString* why) -> Component* {
        Component* fresh = make(w.turns, w.mirror, why);
        if (fresh == nullptr) return nullptr;
        landed.clear();
        return replaceWith(sch, name, fresh, w.centre, newName, pins, why, &landed);
    };
    for (const Way& w : std::as_const(ways)) {
        QString why;
        c = attempt(w, &why);
        if (c == nullptr) {
            faults << why;
            sch->restore(state);
            continue;
        }
        const Outcome outcome{w, across(c), int(newWiringIssues(sch, wiringBefore).size()), wireLength()};
        if (outcome.across == 0 && outcome.issues == 0) {
            used = w;
            break;
        }
        if (!best || std::tie(outcome.across, outcome.issues, outcome.length) < std::tie(best->across, best->issues, best->length))
            best = outcome;
        c = nullptr;
        sch->restore(state);
        // (Each way tried puts the whole schematic back: few on a large one.)
        if (++kept >= (sch->a_DocComps.size() > 2000 ? 2 : 10)) break;
    }
    if (c == nullptr && best) {
        QString why;
        c = attempt(best->way, &why);
        used = best->way;
        if (c == nullptr) {
            faults << why;
            sch->restore(state);
        }
    }
    if (c == nullptr) {
        faults.removeDuplicates();
        return errorResult(tr("%1 is not replaced: no way of putting %2 there keeps every net (%3). Give 'x', 'y', 'rotation' "
                              "or 'mirror', or move a part out of the way.")
                               .arg(name, type, faults.mid(0, 3).join(QStringLiteral("; "))));
    }
    setTextOf(c, args, &error);
    sch->enlargeView(c);
    finish(sch, {QPoint(c->cx, c->cy)});
    QJsonObject result = componentJson(c);
    QJsonArray mapped;
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it)
        mapped.append(QStringLiteral("%1.%2 -> %3.%4%5").arg(name).arg(it.key() + 1).arg(newName).arg(it.value() + 1)
                          .arg(c->Ports.at(it.value())->Name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(c->Ports.at(it.value())->Name)));
    result.insert(QStringLiteral("pins taken"), mapped);
    result.insert(QStringLiteral("mapped"), mappedBy);
    QString placed = tr("turned %1, %2, centred at %3, %4").arg(used.turns).arg(used.mirror ? tr("mirrored") : tr("not mirrored"))
                         .arg(used.centre.x()).arg(used.centre.y());
    placed += used.off == 0 ? tr(": every pin on its old place") : tr(": %1 pin(s) wired to where the old ones were").arg(used.off);
    result.insert(QStringLiteral("placed"), placed);
    QStringList notes = landed;
    for (int j = 0; j < c->Ports.size(); ++j)
        if (std::find(pins.cbegin(), pins.cend(), j) == pins.cend())
            notes << tr("%1.%2 takes no old pin's net: it is open").arg(newName).arg(j + 1);
    notes << newWiringIssues(sch, wiringBefore);
    if (!notes.isEmpty()) result.insert(QStringLiteral("note"), notes.join(QStringLiteral("; ")));
    return jsonResult(result);
}

QJsonObject QucsControl::moveGroup(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    QList<Component*> group;
    QSet<Component*> grouped;
    QStringList missing;
    const PartIndex parts(sch);
    for (const QJsonValue& v : args.value(QLatin1String("names")).toArray()) {
        Component* c = parts.find(v.toString(), &error);
        if (c == nullptr) missing << error;
        else if (!grouped.contains(c)) {
            grouped.insert(c);
            group << c;
        }
    }
    if (!missing.isEmpty()) return errorResult(onceEach(missing).join(QLatin1Char(' ')));
    QList<Diagram*> diagrams;
    for (const QJsonValue& v : args.value(QLatin1String("diagrams")).toArray()) {
        Diagram* d = diagramOf(sch, v, &error);
        if (d == nullptr) return errorResult(error);
        if (!diagrams.contains(d)) diagrams << d;
    }
    QList<Painting*> paintings;
    for (const QJsonValue& v : args.value(QLatin1String("paintings")).toArray()) {
        Painting* p = paintingOf(sch->a_DocPaints, v, &error);
        if (p == nullptr) return errorResult(error);
        if (!paintings.contains(p)) paintings << p;
    }
    if (group.isEmpty() && diagrams.isEmpty() && paintings.isEmpty())
        return errorResult(tr("'names' are the components to move (and 'diagrams', 'paintings' their numbers)."));
    const QJsonValue dxv = args.value(QLatin1String("dx")), dyv = args.value(QLatin1String("dy"));
    if ((!dxv.isUndefined() && !dxv.isDouble()) || (!dyv.isUndefined() && !dyv.isDouble()) || std::abs(dxv.toDouble()) > 1e6
        || std::abs(dyv.toDouble()) > 1e6)
        return errorResult(tr("'dx' and 'dy' are how far to move, in the schematic's units."));
    // On the grid: a step of the grid, the nearest.
    const int gx = std::max(1, sch->getGridX()), gy = std::max(1, sch->getGridY());
    const int dx = int(std::lround(dxv.toDouble() / gx)) * gx, dy = int(std::lround(dyv.toDouble() / gy)) * gy;

    // The wiring among the parts moves with them: each set of wires joined
    // to one another that touches only parts of the group. Wires that go
    // out to the rest stay, and are drawn on to the pins' new places.
    QSet<const Component*> in(group.begin(), group.end());
    QSet<Wire*> moving;
    {
        QSet<Wire*> seen;
        for (Wire* start : sch->a_DocWires) {
            if (seen.contains(start)) continue;
            QList<Wire*> cluster{start};
            seen.insert(start);
            bool inside = true, touches = false;
            for (int k = 0; k < cluster.size(); ++k)
                for (Node* n : {cluster.at(k)->Port1, cluster.at(k)->Port2}) {
                    for (const Component* c : n->components()) {
                        if (in.contains(c)) touches = true;
                        else inside = false;
                    }
                    for (Wire* w : n->wires())
                        if (!seen.contains(w)) {
                            seen.insert(w);
                            cluster << w;
                        }
                }
            if (inside && touches)
                for (Wire* w : std::as_const(cluster)) moving.insert(w);
        }
    }
    // Moved by nothing: what would move, and nothing done (it was refused,
    // and so could not tell a group's wiring).
    if (dx == 0 && dy == 0) {
        QStringList names;
        int out = 0;
        for (Component* c : std::as_const(group)) {
            names << refOf(sch, c);
            for (const Port* p : c->Ports)
                if (p->Connection != nullptr)
                    for (const Wire* w : p->Connection->wires()) out += moving.contains(const_cast<Wire*>(w)) ? 0 : 1;
        }
        return jsonResult(QJsonObject{{QStringLiteral("moved"), QJsonArray()},
                                      {QStringLiteral("would move"), QJsonArray::fromStringList(names)},
                                      {QStringLiteral("wires that would move with them"), int(moving.size())},
                                      {QStringLiteral("wires to the rest, drawn on to them"), out},
                                      {QStringLiteral("note"), tr("dx and dy are 0 (or less than a step of the grid, %1): nothing moved.").arg(gx)}});
    }
    prepare(sch);
    const QString state = sch->snapshot();
    const Nets before = netsOf(sch, nullptr);
    const QList<qucs_s::erc::Issue> wiringBefore = qucs_s::erc::wiring(sch);
    // Moved as the cursor keys move a selection, and the wires healed.
    sch->deselectElements(nullptr);
    for (Component* c : std::as_const(group)) c->isSelected = true;
    for (Wire* w : std::as_const(moving)) {
        w->isSelected = true;
        if (w->hasLabel()) w->label()->isSelected = true;
    }
    for (Diagram* d : std::as_const(diagrams)) d->isSelected = true;
    for (Painting* p : std::as_const(paintings)) p->isSelected = true;
    const Schematic::Selection selection = sch->currentSelection();
    for (Node* n : selection.nodes)
        if (n->hasLabel()) n->label()->isSelected = true;
    const Schematic::Selection moved = sch->currentSelection();
    const auto mover = [dx, dy](Element* e) { e->moveCenter(dx, dy); };
    std::ranges::for_each(moved.paintings, mover);
    std::ranges::for_each(moved.diagrams, mover);
    std::ranges::for_each(moved.labels, mover);
    std::ranges::for_each(moved.components, mover);
    std::ranges::for_each(moved.wires, mover);
    std::ranges::for_each(moved.nodes, mover);
    sch->healAfterKeyboardMutation();
    sch->deselectElements(nullptr);
    // The circuit as it was: a move that would join or split a net is not
    // made (a pin come down on a wire of another net, say).
    if (const QStringList changes = netChangesBeyond(before, netsOf(sch, nullptr), {}); !changes.isEmpty()) {
        sch->restore(state);
        return errorResult(tr("Not moved: moved so, %1. Try another dx, dy.").arg(atMost(changes).join(QStringLiteral("; "))));
    }
    QList<QPoint> where;
    for (Component* c : std::as_const(group)) where << QPoint(c->cx, c->cy);
    finish(sch, where);
    // (By their refs, made in one pass; at most 20 named, and how many.)
    const QHash<const Component*, QString> refs = PartIndex::refs(sch);
    QStringList names;
    for (Component* c : std::as_const(group)) names << refs.value(c, c->Name);
    QJsonObject result{{QStringLiteral("moved"), QJsonArray::fromStringList(atMost(names))},
                       {QStringLiteral("by"), QJsonArray{dx, dy}},
                       {QStringLiteral("wires moved with them"), int(moving.size())},
                       {QStringLiteral("one step to undo"), true}};
    if (!diagrams.isEmpty()) result.insert(QStringLiteral("diagrams moved"), int(diagrams.size()));
    if (!paintings.isEmpty()) result.insert(QStringLiteral("paintings moved"), int(paintings.size()));
    if (const QStringList look = newWiringIssues(sch, wiringBefore); !look.isEmpty())
        result.insert(QStringLiteral("look"), look.join(QStringLiteral("; ")));
    return jsonResult(result);
}

QJsonObject QucsControl::addAnalysis(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString kind = args.value(QLatin1String("kind")).toString().trimmed().toLower();
    const auto given = [&args](const char* key, const QString& otherwise) {
        const QJsonValue v = args.value(QLatin1String(key));
        return v.isUndefined() || v.isNull() ? otherwise : propertyValue(v);
    };
    const QString scale = args.value(QLatin1String("scale")).toString().trimmed().toLower();
    if (!scale.isEmpty() && scale != QLatin1String("lin") && scale != QLatin1String("log"))
        return errorResult(tr("'scale' is lin or log."));
    QString type;
    QJsonObject properties;
    QString analysisPrefix;   // of the dataset's names: ac.v(out), tran.v(out)
    if (kind == QLatin1String("ac")) {
        type = QStringLiteral(".AC");
        properties = {{QStringLiteral("Type"), scale.isEmpty() ? QStringLiteral("log") : scale},
                      {QStringLiteral("Start"), given("from", QStringLiteral("1 Hz"))},
                      {QStringLiteral("Stop"), given("to", QStringLiteral("100 MHz"))},
                      {QStringLiteral("Points"), given("points", QStringLiteral("101"))}};
        analysisPrefix = QStringLiteral("ac");
    } else if (kind == QLatin1String("tran")) {
        type = QStringLiteral(".TR");
        properties = {{QStringLiteral("Type"), QStringLiteral("lin")},
                      {QStringLiteral("Start"), given("from", QStringLiteral("0"))},
                      {QStringLiteral("Stop"), given("stop", given("to", QStringLiteral("1 ms")))},
                      {QStringLiteral("Points"), given("points", QStringLiteral("201"))}};
        analysisPrefix = QStringLiteral("tran");
    } else if (kind == QLatin1String("op")) {
        type = QStringLiteral(".DC");
    } else if (kind == QLatin1String("sweep")) {
        type = QStringLiteral(".SW");
        const QString of = args.value(QLatin1String("analysis")).toString().trimmed();
        Component* sim = sch->getComponentByName(of);
        if (sim == nullptr || !sim->isSimulation)
            return errorResult(tr("'analysis' names the analysis swept (TR1, AC1, DC1, ...)%1.")
                                   .arg([&] {
                                       QStringList sims;
                                       for (Component* c : sch->a_DocComps)
                                           if (c->isSimulation && c->Model != QLatin1String(".SW")) sims << c->Name;
                                       return sims.isEmpty() ? tr(": the schematic has none - add one first") : tr(": it has %1").arg(sims.join(QStringLiteral(", ")));
                                   }()));
        const QString parameter = args.value(QLatin1String("parameter")).toString().trimmed();
        if (parameter.isEmpty())
            return errorResult(tr("'parameter' is what is swept: a component's name (its value is swept: R2) or a parameter an equation defines."));
        bool defined = sch->getComponentByName(parameter) != nullptr;
        for (Component* c : sch->a_DocComps)
            if (!defined && c->isEquation)
                for (const Property* p : c->Props) defined = defined || p->Value.section(QLatin1Char('='), 0, 0).trimmed() == parameter;
        if (!defined)
            return errorResult(tr("There is no component or equation parameter %1 to sweep.").arg(parameter));
        properties = {{QStringLiteral("Sim"), of},
                      {QStringLiteral("Type"), scale.isEmpty() ? QStringLiteral("lin") : scale},
                      {QStringLiteral("Param"), parameter},
                      {QStringLiteral("Start"), given("from", QString())},
                      {QStringLiteral("Stop"), given("to", QString())},
                      {QStringLiteral("Points"), given("points", QStringLiteral("11"))}};
        if (properties.value(QStringLiteral("Start")).toString().isEmpty() || properties.value(QStringLiteral("Stop")).toString().isEmpty())
            return errorResult(tr("A sweep needs 'from' and 'to': the values it goes over (1k, 10k, or numbers)."));
        analysisPrefix = sim->Model == QLatin1String(".AC") ? QStringLiteral("ac") : sim->Model == QLatin1String(".TR") ? QStringLiteral("tran") : QStringLiteral("dc");
    } else {
        return errorResult(tr("'kind' is ac, tran, op or sweep."));
    }
    // Its properties as add_component takes them, over what the others set
    // (one thing said twice is refused: which was meant?).
    const QJsonObject own = args.value(QLatin1String("properties")).toObject();
    static const QList<std::pair<const char*, const char*>> saidBy{{"from", "Start"}, {"to", "Stop"}, {"stop", "Stop"}, {"points", "Points"},
                                                                   {"scale", "Type"}, {"parameter", "Param"}, {"analysis", "Sim"}};
    for (const auto& [arg, property] : saidBy)
        if (args.contains(QLatin1String(arg)) && own.contains(QLatin1String(property)))
            return errorResult(tr("'%1' and 'properties' %2 say the same: give one of them. Nothing was added.").arg(QLatin1String(arg), QLatin1String(property)));
    for (auto it = own.constBegin(); it != own.constEnd(); ++it) properties.insert(it.key(), it.value());
    const QJsonArray plot = args.value(QLatin1String("plot")).toArray();
    if (!plot.isEmpty() && kind == QLatin1String("op"))
        return errorResult(tr("An operating point has nothing to plot: simulate with operating_point, or get_dataset reads it."));
    // What 'plot' names: a node (out), a voltage or current (v(out), i(V1)),
    // a name of the dataset (ac.v(out)) - else an expression, db(v(out)),
    // which the dataset has only when an equation computes it: a NutmegEq
    // beside the analysis, under a simulator that runs one.
    static const QRegularExpression plain(QStringLiteral("^(?:[A-Za-z_][A-Za-z0-9_]*\\.)?(?:[A-Za-z0-9_]+|[VvIi]\\([^(),]*\\))$"));
    QStringList expressions;
    for (const QJsonValue& v : plot)
        if (const QString item = v.toString().trimmed(); !item.isEmpty() && !plain.match(item).hasMatch()) expressions << item;
    // (For the simulator of this call, else of the settings: a run of
    // another is simulate's 'simulator', and its expressions were refused.)
    int simulator = QucsSettings.DefaultSimulator;
    if (const QString named = args.value(QLatin1String("simulator")).toString().trimmed().toLower(); !named.isEmpty()) {
        simulator = named == QLatin1String("ngspice")     ? int(spicecompat::simNgspice)
                    : named == QLatin1String("xyce")      ? int(spicecompat::simXyce)
                    : named == QLatin1String("spiceopus") ? int(spicecompat::simSpiceOpus)
                    : named == QLatin1String("qucsator")  ? int(spicecompat::simQucsator)
                                                          : -1;
        if (simulator < 0) return errorResult(tr("'simulator' is ngspice, xyce, spiceopus or qucsator."));
    }
    const bool nutmeg = simulator == spicecompat::simNgspice || simulator == spicecompat::simSpiceOpus;
    if (!expressions.isEmpty() && !nutmeg)
        return errorResult(tr("'plot' takes nodes (out), v(out) and i(V1) under %1: %2 is an expression, which only an equation "
                              "block computes - add an Eqn with it (add_component) and plot its name, or give 'simulator' "
                              "ngspice for a run of ngspice. Nothing was added.")
                               .arg(spicecompat::getDefaultSimulatorName(simulator), expressions.first()));
    // Read before anything is added: x=1 or v(out) + in the NutmegEq failed
    // only at the next simulation.
    for (const QString& e : std::as_const(expressions))
        if (QString why; !qucs_s::dataset::checkExpression(e, &why))
            return errorResult(tr("'plot': %1 does not read as an expression - %2. Nothing was added.").arg(e, why));
    // Where: beside the analyses there are, else below the circuit.
    int x = args.value(QLatin1String("x")).toInt(), y = args.value(QLatin1String("y")).toInt();
    if (!args.contains(QLatin1String("x")) || !args.contains(QLatin1String("y"))) {
        const Component* last = nullptr;
        for (const Component* c : sch->a_DocComps)
            if (c->isSimulation && (last == nullptr || c->boundingRect().right() > last->boundingRect().right())) last = c;
        const QRect used = sch->allBoundingRect();
        const bool empty = sch->a_DocComps.empty() && sch->a_DocWires.empty();
        if (!args.contains(QLatin1String("x"))) x = last != nullptr ? last->boundingRect().right() + 60 : (empty ? 0 : used.left());
        if (!args.contains(QLatin1String("y"))) y = last != nullptr ? last->cy : (empty ? 0 : used.bottom() + 80);
    }
    QJsonObject component{{QStringLiteral("type"), type}, {QStringLiteral("x"), x}, {QStringLiteral("y"), y},
                          {QStringLiteral("properties"), properties}};
    if (args.contains(QLatin1String("path"))) component.insert(QStringLiteral("path"), args.value(QLatin1String("path")));
    if (args.contains(QLatin1String("name"))) component.insert(QStringLiteral("name"), args.value(QLatin1String("name")));
    const QJsonObject added = addComponent(component);
    if (added.value(QLatin1String("isError")).toBool()) return added;
    QJsonObject result{{QStringLiteral("analysis"), QJsonDocument::fromJson(textOf(added).toUtf8()).object()}};
    int steps = 1;
    // The expressions: each a variable of a NutmegEq beside the analysis,
    // named after it (db(v(out)): db_v_out) unlike any net or name defined.
    QHash<QString, QString> variableOf;
    if (!expressions.isEmpty()) {
        const QString analysisName = result.value(QStringLiteral("analysis")).toObject().value(QStringLiteral("name")).toString();
        const QString simulated = kind == QLatin1String("sweep") ? properties.value(QStringLiteral("Sim")).toString() : analysisName;
        QJsonObject json;
        QString why;
        if (!nutmegVariables(sch, analysisName, simulated, expressions, args.value(QLatin1String("path")), &variableOf, &json, &why)) {
            result.insert(QStringLiteral("equations"), tr("not added: %1 - the expressions' traces have no data").arg(why));
        } else if (!json.isEmpty()) {
            ++steps;
            result.insert(QStringLiteral("equations"),
                          QJsonObject{{QStringLiteral("block"), json.value(QStringLiteral("name"))},
                                      {QStringLiteral("equations"), json.value(QStringLiteral("equations"))},
                                      {QStringLiteral("why"), tr("'plot' named expressions: the dataset has them when this NutmegEq computes "
                                                                 "them, each run of %1.").arg(simulated)}});
        }
    }
    if (!plot.isEmpty()) {
        // What the dataset will call them: ac.v(out) for out, v(out) or ac.v(out).
        const bool qucsator = QucsSettings.DefaultSimulator == spicecompat::simQucsator;
        QJsonArray traces;
        bool right = false;
        for (const QJsonValue& v : plot) {
            QString var = v.toString().trimmed();
            if (var.isEmpty()) continue;
            if (variableOf.contains(var)) {
                // An expression's: on the right, in its own units, beside an
                // AC plot's dB on the left (db(v(out)) is v(out) there).
                const QString name = analysisPrefix.isEmpty() ? variableOf.value(var) : analysisPrefix + QLatin1Char('.') + variableOf.value(var);
                const bool ac = analysisPrefix == QLatin1String("ac");
                right = right || ac;
                traces.append(ac ? QJsonValue(QJsonObject{{QStringLiteral("variable"), name}, {QStringLiteral("axis"), QStringLiteral("right")}})
                                 : QJsonValue(name));
                continue;
            }
            const bool bare = !var.contains(QLatin1Char('(')) && !var.contains(QLatin1Char('.'));
            if (qucsator) {
                if (bare) var += kind == QLatin1String("tran") || analysisPrefix == QLatin1String("tran") ? QStringLiteral(".Vt") : QStringLiteral(".v");
            } else {
                if (bare) var = QStringLiteral("v(%1)").arg(var);
                if (!var.contains(QLatin1Char('.')) && !analysisPrefix.isEmpty()) var = analysisPrefix + QLatin1Char('.') + var;
            }
            traces.append(var);
        }
        QJsonObject diagram{{QStringLiteral("traces"), traces}};
        if (args.contains(QLatin1String("path"))) diagram.insert(QStringLiteral("path"), args.value(QLatin1String("path")));
        if (analysisPrefix == QLatin1String("ac")) {
            diagram.insert(QStringLiteral("x_axis"), QJsonObject{{QStringLiteral("log"), true}, {QStringLiteral("label"), tr("frequency (Hz)")}});
            diagram.insert(QStringLiteral("y_axis"), QJsonObject{{QStringLiteral("log"), true}, {QStringLiteral("units"), QStringLiteral("dB")},
                                                                  {QStringLiteral("label"), tr("magnitude (dB)")}});
            if (right)
                diagram.insert(QStringLiteral("y2_axis"), QJsonObject{{QStringLiteral("log"), false},
                                                                       {QStringLiteral("label"), expressions.join(QStringLiteral(", ")).left(60)}});
        } else if (analysisPrefix == QLatin1String("tran")) {
            diagram.insert(QStringLiteral("x_axis"), QJsonObject{{QStringLiteral("label"), tr("time (s)")}});
        }
        diagram.insert(QStringLiteral("grid"), true);
        const QJsonObject d = addDiagram(diagram);
        if (d.value(QLatin1String("isError")).toBool()) {
            result.insert(QStringLiteral("diagram"), tr("not added: %1").arg(textOf(d)));
        } else {
            result.insert(QStringLiteral("diagram"), QJsonDocument::fromJson(textOf(d).toUtf8()).object());
            ++steps;
        }
    }
    result.insert(QStringLiteral("steps to undo"), steps);
    if (kind == QLatin1String("sweep"))
        result.insert(QStringLiteral("note"), tr("Each curve of %1 is now one for each value of %2: get_dataset gives them with the value.")
                                                  .arg(properties.value(QStringLiteral("Sim")).toString(), properties.value(QStringLiteral("Param")).toString()));
    return jsonResult(result);
}

bool QucsControl::nutmegVariables(Schematic* sch, const QString& beside, const QString& simulated, const QStringList& expressions,
                                  const QJsonValue& path, QHash<QString, QString>* variableOf, QJsonObject* made, QString* error)
{
    // One there is already: an equation of the expression, in a NutmegEq
    // run after that analysis - the first such block takes new ones too.
    Component* there = nullptr;
    for (Component* c : sch->a_DocComps) {
        if (c->Model != QLatin1String("NutmegEq") || c->isActive != COMP_IS_ACTIVE) continue;
        const Property* after = c->getProperty(QStringLiteral("Simulation"));
        if (after == nullptr || after->Value.trimmed().compare(simulated, Qt::CaseInsensitive) != 0) continue;
        if (there == nullptr) there = c;
        for (const Property* p : c->Props)
            if (!isFixedField(p) && expressions.contains(p->Value.trimmed()) && !variableOf->contains(p->Value.trimmed()))
                variableOf->insert(p->Value.trimmed(), p->Name);
    }
    // The others: each a variable of a new NutmegEq beside the analysis,
    // named after it (db(v(out)): db_v_out), unlike any net or name defined.
    QSet<QString> taken = definedNames(sch);
    for (Component* c : sch->a_DocComps) taken << c->Name.toLower();
    for (const Wire* w : sch->a_DocWires)
        if (w->hasLabel()) taken << w->label()->Name.toLower();
    for (const Node* n : sch->a_DocNodes)
        if (n->hasLabel()) taken << n->label()->Name.toLower();
    QJsonArray equations;
    for (const QString& e : expressions) {
        if (variableOf->contains(e)) continue;
        QString base = e.toLower().replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
        base = base.mid(0, 32);
        while (base.startsWith(QLatin1Char('_'))) base.remove(0, 1);
        while (base.endsWith(QLatin1Char('_'))) base.chop(1);
        if (base.isEmpty() || base.at(0).isDigit()) base.prepend(QStringLiteral("expr_"));
        QString name = base;
        for (int n = 2; taken.contains(name) || taken.contains(name.toLower()); ++n) name = QStringLiteral("%1_%2").arg(base).arg(n);
        taken << name.toLower();
        variableOf->insert(e, name);
        equations.append(QStringLiteral("%1=%2").arg(name, e));
    }
    if (equations.isEmpty()) return true;
    // (Added to the block there is: one NutmegEq an analysis, not one an
    // expression.)
    if (there != nullptr && !there->Name.isEmpty()) {
        const QString thereName = there->Name;
        QJsonObject change{{QStringLiteral("name"), thereName}, {QStringLiteral("equations"), equations}};
        if (!path.isUndefined() && !path.isNull()) change.insert(QStringLiteral("path"), path);
        const QJsonObject edited = editComponent(change);
        if (!edited.value(QLatin1String("isError")).toBool()) {
            *made = QJsonDocument::fromJson(textOf(edited).toUtf8()).object();
            made->insert(QStringLiteral("added to"), thereName);
            return true;
        }
    }
    Component* analysis = sch->getComponentByName(beside);
    const QRect at = analysis != nullptr ? analysis->boundingRectIncludingProperties() : sch->allBoundingRect();
    QJsonObject block{{QStringLiteral("type"), QStringLiteral("NutmegEq")},
                      {QStringLiteral("x"), at.right() + 80},
                      {QStringLiteral("y"), analysis != nullptr ? analysis->cy : at.bottom() + 80},
                      {QStringLiteral("properties"), QJsonObject{{QStringLiteral("Simulation"), simulated}}},
                      {QStringLiteral("equations"), equations}};
    if (!path.isUndefined() && !path.isNull()) block.insert(QStringLiteral("path"), path);
    const QJsonObject added = addComponent(block);
    if (added.value(QLatin1String("isError")).toBool()) {
        for (const QJsonValue& v : std::as_const(equations)) variableOf->remove(v.toString().section(QLatin1Char('='), 1));
        *error = textOf(added);
        return false;
    }
    *made = QJsonDocument::fromJson(textOf(added).toUtf8()).object();
    return true;
}

QString QucsControl::expressionTrace(Schematic* sch, const QString& wanted, const QJsonValue& path, bool dryRun, QString* note,
                                     QString* error)
{
    QString sim;
    QString bare = qucs_s::dataset::withoutSimulator(wanted.trimmed(), &sim);
    QString analysis;
    static const QRegularExpression prefixed(QStringLiteral("^(ac|tran|dc)\\.(.+)$"), QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = prefixed.match(bare); m.hasMatch()) {
        analysis = m.captured(1).toLower();
        bare = m.captured(2);
    }
    if (bare.contains(QLatin1Char(':')) || !qucs_s::dataset::isExpression(bare)) return {};
    if (QString why; !qucs_s::dataset::checkExpression(bare, &why)) {
        *error = tr("%1 does not read as an expression - %2.").arg(wanted.trimmed(), why);
        return {};
    }
    const bool nutmeg = QucsSettings.DefaultSimulator == spicecompat::simNgspice || QucsSettings.DefaultSimulator == spicecompat::simSpiceOpus;
    if (!nutmeg) {
        *error = tr("%1 is an expression: a trace shows a variable of the dataset, and under %2 only an equation block computes one - "
                    "add an Eqn with it (add_component with 'equations') and trace its name.")
                     .arg(wanted.trimmed(), spicecompat::getDefaultSimulatorName(QucsSettings.DefaultSimulator));
        return {};
    }
    // After which analysis: the one named (ac.db(v(out))), else the only one.
    QList<Component*> analyses;
    for (Component* c : sch->a_DocComps) {
        if (!c->isSimulation || c->isActive != COMP_IS_ACTIVE) continue;
        const bool ac = c->Model == QLatin1String(".AC"), tran = c->Model == QLatin1String(".TR");
        if ((analysis.isEmpty() && (ac || tran)) || (analysis == QLatin1String("ac") && ac) || (analysis == QLatin1String("tran") && tran))
            analyses << c;
    }
    if (analyses.isEmpty()) {
        *error = tr("%1 is an expression, which a NutmegEq computes after an analysis: %2 has no %3 analysis.")
                     .arg(wanted.trimmed(), titleOf(sch), analysis.isEmpty() ? tr("AC or transient") : analysis);
        return {};
    }
    if (analysis.isEmpty() && analyses.size() > 1) {
        *error = tr("%1 is an expression: after which analysis - ac.%2 or tran.%2?").arg(wanted.trimmed(), bare);
        return {};
    }
    Component* a = analyses.first();
    const QString of = a->Model == QLatin1String(".AC") ? QStringLiteral("ac") : QStringLiteral("tran");
    const QString prefix = simulatorPrefix();
    if (dryRun) return prefix + QLatin1Char('/') + of + QLatin1Char('.') + bare;
    QHash<QString, QString> variableOf;
    QJsonObject made;
    if (!nutmegVariables(sch, a->Name, a->Name, {bare}, path, &variableOf, &made, error)) return {};
    const QString var = variableOf.value(bare);
    *note = made.isEmpty() ? tr("%1 is %2, which a NutmegEq there computes after %3.").arg(bare, var, a->Name)
            : made.contains(QStringLiteral("added to"))
                ? tr("%1 is computed as %2, an equation added to %3 after %4 (one step to undo): the trace shows it after the next "
                     "simulation.").arg(bare, var, made.value(QStringLiteral("added to")).toString(), a->Name)
                : tr("%1 is computed as %2 by %3, a NutmegEq added after %4 (one step to undo): the trace shows it after the "
                     "next simulation.").arg(bare, var, made.value(QStringLiteral("name")).toString(), a->Name);
    return prefix + QLatin1Char('/') + of + QLatin1Char('.') + var;
}

QJsonObject QucsControl::createSubcircuit(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    if (sch->getDocName().isEmpty()) return errorResult(tr("%1 has no file yet: save it first (the subcircuit's file goes beside it).").arg(titleOf(sch)));
    QList<Component*> group;
    QSet<Component*> grouped;
    QStringList missing;
    const PartIndex parts(sch);
    for (const QJsonValue& v : args.value(QLatin1String("names")).toArray()) {
        Component* c = parts.find(v.toString(), &error);
        if (c == nullptr) missing << error;
        else if (c->isSimulation) return errorResult(tr("%1 is an analysis: a subcircuit holds the circuit, its analyses stay outside.").arg(c->Name));
        else if (!grouped.contains(c)) {
            grouped.insert(c);
            group << c;
        }
    }
    if (!missing.isEmpty()) return errorResult(onceEach(missing).join(QLatin1Char(' ')));
    if (group.isEmpty()) return errorResult(tr("'names' are the components that go into the subcircuit."));
    // Grounds stay where they are: ground is one node everywhere, and the
    // parts inside on it get grounds of their own. Taken in, the circuit
    // outside lost its ground, silently.
    int groundsLeft = 0;
    for (qsizetype i = group.size() - 1; i >= 0; --i)
        if (group.at(i)->Model == QLatin1String("GND")) {
            group.removeAt(i);
            ++groundsLeft;
        }
    if (group.isEmpty())
        return errorResult(tr("A subcircuit of grounds alone holds nothing: grounds stay where they are (the parts in a subcircuit "
                              "that are on ground get grounds of their own)."));
    if (groundsLeft > 0)
        a_callNotes << tr("%1 ground(s) named stay where they are: ground is one node everywhere, and the parts inside on it get "
                          "grounds of their own.").arg(groundsLeft);
    QString file = args.value(QLatin1String("save_as")).toString().trimmed();
    if (file.isEmpty()) return errorResult(tr("'save_as' names the subcircuit's file (a .sch beside this schematic)."));
    const QDir here = QFileInfo(sch->getDocName()).absoluteDir();
    if (!file.endsWith(QLatin1String(".sch"), Qt::CaseInsensitive)) file += QStringLiteral(".sch");
    file = QFileInfo(file).isAbsolute() ? QDir::cleanPath(file) : QDir::cleanPath(here.filePath(file));
    if (const QString bad = badFileName(QFileInfo(file).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
    if (sameFile(file, sch->getDocName())) return errorResult(tr("'save_as' is this schematic itself."));
    for (QucsDoc* doc : a_app->allDocuments())
        if (!doc->getDocName().isEmpty() && sameFile(doc->getDocName(), file))
            return errorResult(tr("%1 is open: close it, or choose another name.").arg(QFileInfo(file).fileName()));
    if (QFileInfo::exists(file) && !args.value(QLatin1String("replace")).toBool())
        return errorResult(tr("%1 is there already ('replace': true writes over it).").arg(QFileInfo(file).fileName()));

    // The nets of the group's pins: those also on anything outside it are
    // its ports; ground stays ground (a ground inside); the rest go inside.
    const Nets nets = netsOf(sch, nullptr);
    QHash<const Component*, QString> keyOf;
    {
        QHash<QString, int> seen;
        for (Component* c : sch->a_DocComps) keyOf.insert(c, keyBase(c, seen));
    }
    QSet<const Component*> in(group.begin(), group.end());
    QSet<QString> groupPins;
    for (Component* c : std::as_const(group))
        for (int i = 0; i < c->Ports.size(); ++i) groupPins.insert(QStringLiteral("%1.%2").arg(keyOf.value(c)).arg(i + 1));
    struct Boundary {
        int net;
        QString label;   // the name the port is joined by
        QPoint at;       // a pin of the group on it
        QString pin;
        QList<QPoint> all;   // every pin of the group on it
        bool named;      // the label is the net's own (on it outside already)
    };
    QList<Boundary> boundaries;
    QList<QPoint> grounded;   // the group's pins on ground
    QSet<int> handled;
    QSet<QString> labelsInUse;
    for (auto it = nets.netOf.cbegin(); it != nets.netOf.cend(); ++it)
        if (it.key().startsWith(QLatin1String("label "))) labelsInUse.insert(it.key().mid(6));
    for (Component* c : std::as_const(group))
        for (int i = 0; i < c->Ports.size(); ++i) {
            const QString key = QStringLiteral("%1.%2").arg(keyOf.value(c)).arg(i + 1);
            const int net = nets.netOf.value(key, -1);
            const QPoint at(c->cx + c->Ports.at(i)->x, c->cy + c->Ports.at(i)->y);
            if (net < 0 || handled.contains(net)) continue;
            bool outside = false, ground = false;
            QString label;
            for (auto k = nets.netOf.cbegin(); k != nets.netOf.cend(); ++k) {
                if (k.value() != net) continue;
                if (k.key() == QLatin1String("ground")) ground = true;
                else if (k.key().startsWith(QLatin1String("label "))) label = k.key().mid(6);
                else if (!k.key().startsWith(QLatin1String("at ")) && !groupPins.contains(k.key())) outside = true;
            }
            if (ground) {
                grounded << at;   // (each pin on ground gets a ground inside)
                continue;
            }
            handled.insert(net);
            if (!outside) continue;
            const bool named = !label.isEmpty();
            // A net without a name: the name of the group's pin on it, when
            // one pin has one (a 741's INN, VCC), set_label would take it
            // (not GND: ground's name) and no net has it yet - the
            // subcircuit's pins then say what they are (make_symbol sides
            // them, arrange finds the output). Else file_n1.
            if (label.isEmpty()) {
                static const QRegularExpression word(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]+$"));
                QStringList names;
                for (Component* d : std::as_const(group))
                    for (int j = 0; j < d->Ports.size(); ++j)
                        if (const QString& n = d->Ports.at(j)->Name;
                            !n.isEmpty() && nets.netOf.value(QStringLiteral("%1.%2").arg(keyOf.value(d)).arg(j + 1), -1) == net
                            && !names.contains(n, Qt::CaseInsensitive))
                            names << n;
                if (names.size() == 1 && word.match(names.first()).hasMatch() && badNetName(names.first()).isEmpty()
                    && std::none_of(labelsInUse.cbegin(), labelsInUse.cend(),
                                    [&names](const QString& l) { return l.compare(names.first(), Qt::CaseInsensitive) == 0; })) {
                    label = names.first();
                    labelsInUse.insert(label);
                }
            }
            if (label.isEmpty()) {
                int n = 1;
                const QString base = QFileInfo(file).completeBaseName();
                while (labelsInUse.contains(QStringLiteral("%1_n%2").arg(base).arg(n))) ++n;
                label = QStringLiteral("%1_n%2").arg(base).arg(n);
                labelsInUse.insert(label);
            }
            // Every pin of the group on it takes the label inside: the
            // wire between two of them stays outside when it goes on to
            // a part outside (R1 and R2 of a divider, C1 on their node),
            // and R2 was then on nothing in the subcircuit.
            QList<QPoint> all;
            for (Component* d : std::as_const(group))
                for (int j = 0; j < d->Ports.size(); ++j) {
                    const QPoint p(d->cx + d->Ports.at(j)->x, d->cy + d->Ports.at(j)->y);
                    if (nets.netOf.value(QStringLiteral("%1.%2").arg(keyOf.value(d)).arg(j + 1), -1) == net && !all.contains(p)) all << p;
                }
            boundaries.append({net, label, at, said(key, {}), all, named});
        }
    // Grounds of pins on ground: one each (a pin may be on ground more
    // than once only by its own wires).
    for (Component* c : std::as_const(group))
        for (int i = 0; i < c->Ports.size(); ++i) {
            const QString key = QStringLiteral("%1.%2").arg(keyOf.value(c)).arg(i + 1);
            const QPoint at(c->cx + c->Ports.at(i)->x, c->cy + c->Ports.at(i)->y);
            if (nets.netOf.value(key, -1) == nets.netOf.value(QStringLiteral("ground"), -2) && !grounded.contains(at)) grounded << at;
        }

    // The wiring among the group: sets of joined wires touching only it.
    QSet<Wire*> inside;
    QSet<Node*> insideNodes;
    {
        QSet<Wire*> seen;
        for (Wire* start : sch->a_DocWires) {
            if (seen.contains(start)) continue;
            QList<Wire*> cluster{start};
            seen.insert(start);
            bool only = true, touches = false;
            for (int k = 0; k < cluster.size(); ++k)
                for (Node* n : {cluster.at(k)->Port1, cluster.at(k)->Port2}) {
                    for (const Component* c : n->components()) {
                        if (in.contains(c)) touches = true;
                        else only = false;
                    }
                    for (Wire* w : n->wires())
                        if (!seen.contains(w)) {
                            seen.insert(w);
                            cluster << w;
                        }
                }
            if (!(only && touches)) continue;
            for (Wire* w : std::as_const(cluster)) {
                inside.insert(w);
                insideNodes.insert(w->Port1);
                insideNodes.insert(w->Port2);
            }
        }
    }
    // The subcircuit's file: the group, its wiring, a port for each net
    // that goes out (joined to it by a label of that net's name), a
    // ground for each pin on ground.
    QRect box;
    for (Component* c : std::as_const(group)) box = box.isNull() ? c->boundingRect() : box.united(c->boundingRect());
    QStringList components, wires;
    for (Component* c : std::as_const(group)) components << QStringLiteral("  ") + c->save();
    const auto labelLine = [](const QPoint& p, const QString& name) {
        return QStringLiteral("  <%1 %2 %1 %2 \"%3\" %4 %5 0 \"\">").arg(p.x()).arg(p.y()).arg(name).arg(p.x() + 10).arg(p.y() - 20);
    };
    const int gx = std::max(1, sch->getGridX()), gy = std::max(1, sch->getGridY());
    // (One label to a place: a node inside that has its own keeps it.)
    QSet<QPoint> labelledInside;
    for (Node* n : std::as_const(insideNodes))
        if (n->hasLabel()) labelledInside.insert(n->center());
    for (int k = 0; k < boundaries.size(); ++k) {
        std::unique_ptr<Component> port{newComponent(QStringLiteral("Port"))};
        if (!port) return errorResult(tr("The library has no Port component."));
        port->Name = QStringLiteral("P%1").arg(k + 1);
        if (Property* num = port->getProperty(QStringLiteral("Num"))) num->Value = QString::number(k + 1);
        int x = box.left() - 100, y = box.top() + 60 * k;
        x = x / gx * gx;
        y = y / gy * gy;
        port->moveCenter(x - port->cx, y - port->cy);
        const QPoint pin(port->cx + port->Ports.first()->x, port->cy + port->Ports.first()->y);
        components << QStringLiteral("  ") + port->save();
        wires << labelLine(pin, boundaries.at(k).label);
        for (const QPoint& p : boundaries.at(k).all)
            if (!labelledInside.contains(p)) {
                labelledInside.insert(p);
                wires << labelLine(p, boundaries.at(k).label);
            }
    }
    for (const QPoint& at : std::as_const(grounded)) {
        std::unique_ptr<Component> ground{newComponent(QStringLiteral("GND"))};
        if (!ground) return errorResult(tr("The library has no ground component."));
        ground->moveCenter(at.x() - ground->Ports.first()->x - ground->cx, at.y() - ground->Ports.first()->y - ground->cy);
        components << QStringLiteral("  ") + ground->save();
    }
    for (Wire* w : std::as_const(inside)) wires << QStringLiteral("  ") + w->save();
    for (Node* n : std::as_const(insideNodes))
        if (n->hasLabel()) wires << QStringLiteral("  ") + n->label()->save();
    const QString text = QStringLiteral("<Qucs Schematic %1>\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n%2\n</Components>\n"
                                        "<Wires>\n%3</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
                             .arg(QStringLiteral(PACKAGE_VERSION), components.join(QLatin1Char('\n')),
                                  wires.isEmpty() ? QString() : wires.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    // What the file held (a file replaced), to put back when this fails
    // after all, or is a preview.
    std::optional<QByteArray> held;
    if (QFile old(file); old.exists()) {
        // (One that cannot be read was taken for none: put back as none,
        // it was deleted - "as it was", the answer said.)
        if (!old.open(QIODevice::ReadOnly))
            return errorResult(tr("%1 is there and cannot be read (%2): it would be written over with no way back. Choose another name.")
                                   .arg(QDir::toNativeSeparators(file), old.errorString()));
        held = old.readAll();
    }
    const auto putBack = [&file, &held] {
        if (!held) {
            QFile::remove(file);
            return;
        }
        QFile out(file);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(*held);
    };
    {
        if (const QString no = aboutToWrite(file); !no.isEmpty()) return errorResult(no);
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return errorResult(tr("%1 could not be written.").arg(QDir::toNativeSeparators(file)));
        out.write(text.toUtf8());
    }

    // In this schematic: the group gives way to one subcircuit, its pins
    // joined to the nets by labels of the same names - one step to undo.
    std::unique_ptr<Component> sub{newComponent(QStringLiteral("Sub"))};
    if (!sub) {
        putBack();
        return errorResult(tr("The library has no subcircuit component."));
    }
    sub->setSchematic(sch);
    sub->Props.first()->Value = here.relativeFilePath(file);
    QString subName = args.value(QLatin1String("name")).toString().trimmed();
    if (subName.isEmpty()) {
        int n = 1;
        while (sch->getComponentByName(QStringLiteral("SUB%1").arg(n)) != nullptr) ++n;
        subName = QStringLiteral("SUB%1").arg(n);
    } else if (sch->getComponentByName(subName) != nullptr && !in.contains(sch->getComponentByName(subName))) {
        putBack();
        return errorResult(tr("There is a component named %1 already.").arg(subName));
    } else if (const QString bad = badPartName(subName); !bad.isEmpty()) {
        putBack();
        return errorResult(bad + QLatin1Char('.'));
    }
    sub->Name = subName;
    sub->recreate();
    if (sub->Ports.size() != boundaries.size()) {
        putBack();
        return errorResult(tr("The subcircuit written has %1 pins where %2 were meant: nothing changed, and %3 is as it was.")
                               .arg(sub->Ports.size()).arg(boundaries.size()).arg(QFileInfo(file).fileName()));
    }
    // What the group leaves behind with nothing to do: a ground that was
    // on its pins alone, a wire to where one of its pins was (and on from
    // there, as far as it goes to nothing) - not the ends the labels keep.
    // The wire between two of its pins goes too, when their net goes on to
    // a part outside: left, the instance put where the group was landed
    // both its pins on it (a divider's R1 and R2 in a row, C1 on their
    // node: XSUB1 mid mid). The ports are joined by labels, on what is left
    // of their nets.
    QSet<QPoint> gone;
    for (Component* c : std::as_const(group))
        for (const Port* p : c->Ports) gone.insert(QPoint(c->cx + p->x, c->cy + p->y));
    QSet<QPoint> labelled;
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel() && !insideNodes.contains(n)) labelled.insert(n->center());
    QList<Component*> kept;
    QSet<QPoint> pinsKept;
    for (Component* c : sch->a_DocComps) {
        if (in.contains(c)) continue;
        bool orphan = c->Model == QLatin1String("GND") && !c->Ports.isEmpty();
        for (const Port* p : c->Ports) {
            const QPoint at(c->cx + p->x, c->cy + p->y);
            const Node* n = p->Connection;
            // (Its node had the group's pins and nothing else: no wire.)
            bool onlyGroup = gone.contains(at) && n != nullptr && n->wires().empty();
            if (n != nullptr)
                for (const Component* o : n->components()) onlyGroup = onlyGroup && (o == c || in.contains(o));
            orphan = orphan && onlyGroup;
        }
        if (orphan) continue;
        kept << c;
        for (const Port* p : c->Ports) pinsKept.insert(QPoint(c->cx + p->x, c->cy + p->y));
    }
    QList<Wire*> wiresKept;
    for (Wire* w : sch->a_DocWires)
        if (!inside.contains(w)) wiresKept << w;
    for (bool again = true; again;) {
        again = false;
        for (bool more = true; more;) {
            more = false;
            QHash<QPoint, int> ends;
            for (const Wire* w : std::as_const(wiresKept)) {
                ends[w->P1()]++;
                ends[w->P2()]++;
            }
            for (int k = 0; k < wiresKept.size(); ++k) {
                const Wire* w = wiresKept.at(k);
                if (w->hasLabel()) continue;
                for (const QPoint& e : {w->P1(), w->P2()})
                    if (gone.contains(e) && ends.value(e) == 1 && !pinsKept.contains(e) && !labelled.contains(e)) {
                        gone.insert(e == w->P1() ? w->P2() : w->P1());   // (its other end may be left to nothing now)
                        wiresKept.removeAt(k--);
                        more = true;
                        break;
                    }
            }
        }
        // A ground the wires just taken left on nothing - it met the group
        // through one - goes too (it stayed, connected to nothing); and then
        // the wires again, which it no longer holds.
        QHash<QPoint, int> ends, pins;
        for (const Wire* w : std::as_const(wiresKept)) {
            ends[w->P1()]++;
            ends[w->P2()]++;
        }
        for (const Component* c : std::as_const(kept))
            for (const Port* p : c->Ports) pins[QPoint(c->cx + p->x, c->cy + p->y)]++;
        for (int k = 0; k < kept.size(); ++k) {
            const Component* c = kept.at(k);
            if (c->Model != QLatin1String("GND") || c->Ports.isEmpty()) continue;
            bool alone = true;
            for (const Port* p : c->Ports) {
                const QPoint at(c->cx + p->x, c->cy + p->y);
                alone = alone && gone.contains(at) && ends.value(at) == 0 && pins.value(at) == 1 && !labelled.contains(at);
            }
            if (!alone) continue;
            for (const Port* p : c->Ports) pinsKept.remove(QPoint(c->cx + p->x, c->cy + p->y));
            kept.removeAt(k--);
            again = true;
        }
    }
    // Each port's net in this schematic: by its own label, which stays; or
    // a label of the port's name on what is left of it - a wire's end, else
    // a pin outside - the one nearest where the group's pin was.
    QList<QPoint> anchors;
    for (const Boundary& b : std::as_const(boundaries)) {
        QPoint anchor(INT_MIN, INT_MIN);
        if (!b.named) {
            qint64 best = std::numeric_limits<qint64>::max();
            const auto consider = [&](const QPoint& p) {
                const qint64 d = qint64(std::abs(p.x() - b.at.x())) + std::abs(p.y() - b.at.y());
                if (d < best) {
                    best = d;
                    anchor = p;
                }
            };
            for (const Wire* w : std::as_const(wiresKept))
                if (w->Port1 != nullptr && nets.nodeNet.value(w->Port1, -1) == b.net) {
                    consider(w->P1());
                    consider(w->P2());
                }
            if (best == std::numeric_limits<qint64>::max())
                for (const Component* c : std::as_const(kept))
                    for (int i = 0; i < c->Ports.size(); ++i)
                        if (nets.netOf.value(QStringLiteral("%1.%2").arg(keyOf.value(c)).arg(i + 1), -1) == b.net)
                            consider(QPoint(c->cx + c->Ports.at(i)->x, c->cy + c->Ports.at(i)->y));
        }
        anchors << anchor;
    }
    // The instance where the group was, or the nearest place from there
    // where none of its pins lands on a wire, a pin or a label of another
    // net than its port's - clear of the parts too, when there is such a
    // place near. (On its own net's wire end, where the group's pin was, it
    // is joined as it was.)
    {
        QHash<QPoint, int> taken;   // a place: the net there
        const int groundNet = nets.netOf.value(QStringLiteral("ground"), -2);
        for (const Component* c : std::as_const(kept))
            for (int i = 0; i < c->Ports.size(); ++i)
                taken.insert(QPoint(c->cx + c->Ports.at(i)->x, c->cy + c->Ports.at(i)->y),
                             c->Model == QLatin1String("GND") ? groundNet
                                                              : nets.netOf.value(QStringLiteral("%1.%2").arg(keyOf.value(c)).arg(i + 1), -3));
        for (Node* n : sch->a_DocNodes)
            if (n->hasLabel() && !insideNodes.contains(n)) taken.insert(n->center(), nets.nodeNet.value(n, -3));
        for (int k = 0; k < anchors.size(); ++k)
            if (anchors.at(k).x() != INT_MIN) taken.insert(anchors.at(k), boundaries.at(k).net);
        QSet<int> portNets;
        for (const Boundary& b : std::as_const(boundaries)) portNets.insert(b.net);
        const auto pinsClear = [&] {
            for (int k = 0; k < sub->Ports.size(); ++k) {
                const QPoint at(sub->cx + sub->Ports.at(k)->x, sub->cy + sub->Ports.at(k)->y);
                const int own = k < boundaries.size() ? boundaries.at(k).net : -4;
                if (auto it = taken.constFind(at); it != taken.constEnd() && *it != own) return false;
                for (const Wire* w : std::as_const(wiresKept))
                    if (onSegment(at, w->P1(), w->P2()) && nets.nodeNet.value(w->Port1, -3) != own) return false;
                // (Nor two of its pins on one place.)
                for (int j = 0; j < k; ++j)
                    if (QPoint(sub->cx + sub->Ports.at(j)->x, sub->cy + sub->Ports.at(j)->y) == at) return false;
            }
            return true;
        };
        const auto boxClear = [&] {
            const QRect mine = sub->boundingRect().adjusted(-10, -10, 10, 10);
            for (const Component* c : std::as_const(kept))
                if (mine.intersects(c->boundingRect())) return false;
            for (const Wire* w : std::as_const(wiresKept))
                if (!portNets.contains(nets.nodeNet.value(w->Port1, -3))
                    && mine.intersects(QRect(w->P1(), w->P2()).normalized().adjusted(0, 0, 1, 1)))
                    return false;
            return true;
        };
        int cx = box.center().x(), cy = box.center().y();
        sch->setOnGrid(cx, cy);
        const auto placeAt = [&](int x, int y) { sub->moveCenter(x - sub->cx, y - sub->cy); };
        placeAt(cx, cy);
        bool placed = pinsClear();   // (where the group was: its box was there)
        for (int pass = 0; pass < 2 && !placed; ++pass)
            for (int ring = 1; ring <= 30 && !placed; ++ring)
                for (int dy = -ring; dy <= ring && !placed; ++dy)
                    for (int dx = -ring; dx <= ring && !placed; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
                        placeAt(cx + dx * gx, cy + dy * gy);
                        placed = pinsClear() && (pass == 1 || boxClear());
                    }
        if (!placed) {
            putBack();
            return errorResult(tr("There is no place near the group for the instance whose pins would land on nothing of this "
                                  "schematic: nothing changed, and %1 is as it was.").arg(QFileInfo(file).fileName()));
        }
    }
    QStringList mine, theirWires;
    for (Component* c : std::as_const(kept)) mine << QStringLiteral("  ") + c->save();
    mine << QStringLiteral("  ") + sub->save();
    for (Wire* w : std::as_const(wiresKept)) theirWires << QStringLiteral("  ") + w->save();
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel() && !insideNodes.contains(n)) theirWires << QStringLiteral("  ") + n->label()->save();
    QJsonArray ports;
    for (int k = 0; k < boundaries.size(); ++k) {
        const QPoint pin(sub->cx + sub->Ports.at(k)->x, sub->cy + sub->Ports.at(k)->y);
        if (anchors.at(k).x() != INT_MIN) theirWires << labelLine(anchors.at(k), boundaries.at(k).label);
        theirWires << labelLine(pin, boundaries.at(k).label);
        ports.append(QJsonObject{{QStringLiteral("port"), k + 1}, {QStringLiteral("net"), boundaries.at(k).label},
                                 {QStringLiteral("was at"), boundaries.at(k).pin}});
    }
    // (Their names now: the schematic is made anew below, and they with it.)
    QStringList names;
    for (Component* c : std::as_const(group)) names << refOf(sch, c);
    // The net names that go inside with it: a trace or an equation that
    // names one shows nothing after the next run (told, not guessed at).
    QStringList goneInside;
    {
        QSet<QString> outside;
        for (Node* n : sch->a_DocNodes)
            if (n->hasLabel() && !insideNodes.contains(n)) outside.insert(n->label()->Name);
        for (Wire* w : sch->a_DocWires)
            if (w->hasLabel() && !inside.contains(w)) outside.insert(w->label()->Name);
        for (const Boundary& b : std::as_const(boundaries)) outside.insert(b.label);
        for (Node* n : std::as_const(insideNodes))
            if (n->hasLabel() && !outside.contains(n->label()->Name) && !goneInside.contains(n->label()->Name)) goneInside << n->label()->Name;
        for (Wire* w : std::as_const(inside))
            if (w->hasLabel() && !outside.contains(w->label()->Name) && !goneInside.contains(w->label()->Name)) goneInside << w->label()->Name;
    }
    // The nets as they are to be: those of before without the group, the
    // instance's pins on the ports' nets.
    Nets expected;
    {
        QSet<QString> keptParts, labelsLeft;
        bool groundLeft = false;
        for (const Component* c : std::as_const(kept)) {
            if (c->Model == QLatin1String("GND")) groundLeft = groundLeft || c->isActive == COMP_IS_ACTIVE;
            else if (!c->Name.isEmpty()) keptParts.insert(keyOf.value(c));
        }
        for (Node* n : sch->a_DocNodes)
            if (n->hasLabel() && !insideNodes.contains(n)) labelsLeft.insert(n->label()->Name);
        for (const Wire* w : std::as_const(wiresKept))
            if (w->hasLabel()) labelsLeft.insert(w->label()->Name);
        for (auto it = nets.netOf.cbegin(); it != nets.netOf.cend(); ++it) {
            const QString& key = it.key();
            const bool keep = key == QLatin1String("ground") ? groundLeft
                              : key.startsWith(QLatin1String("label ")) ? labelsLeft.contains(key.mid(6))
                              : !key.startsWith(QLatin1String("at ")) && keptParts.contains(key.section(QLatin1Char('.'), 0, -2));
            if (keep) expected.netOf.insert(key, it.value());
        }
        for (int k = 0; k < boundaries.size(); ++k) expected.netOf.insert(QStringLiteral("%1.%2").arg(subName).arg(k + 1), boundaries.at(k).net);
    }
    prepare(sch);
    const QString sections = QStringLiteral("<Components>\n%1\n</Components>\n<Wires>\n%2</Wires>\n")
                                 .arg(mine.join(QLatin1Char('\n')), theirWires.isEmpty() ? QString() : theirWires.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    // (To put back if its nets are not as they are to be.)
    const QPair<QString, QString> stateBefore = sch->snapshotAll();
    const Schematic::UndoStacks marksBefore = sch->undoStacks();
    const bool changedBefore = sch->getDocChanged();
    const quint64 revisionBefore = sch->revision();
    const QList<QucsDoc::Edit> editsBefore = sch->recentEdits();
    QString why;
    if (!sch->replaceContent(sections, &why)) {
        putBack();
        return errorResult(tr("The schematic could not take the subcircuit (%1 is as it was): %2").arg(QFileInfo(file).fileName(), why));
    }
    // Every net as it was, the group's pins now the instance's: checked on
    // the schematic made, and undone when not - a subcircuit that answered
    // well joined two nets, and the next simulation read 0 V.
    {
        const Nets now = netsOf(sch, nullptr);
        Nets after;
        for (auto it = now.netOf.cbegin(); it != now.netOf.cend(); ++it)
            if (expected.netOf.contains(it.key())) after.netOf.insert(it.key(), it.value());
        QStringList differ = netChangesBeyond(expected, after, {});
        if (!differ.isEmpty()) {
            sch->restoreAll(stateBefore, false);
            sch->setUndoStacks(marksBefore);
            sch->setChanged(changedBefore, false);
            sch->rewind(revisionBefore, editsBefore);
            putBack();
            for (QString& d : differ) d.replace(tr(" would no longer be "), tr(" was no longer ")).replace(tr(" would be "), tr(" was "));
            return errorResult(tr("The instance would not keep this schematic's nets - %1: nothing changed, and %2 is as it was. "
                                  "Move the parts apart (or their wires) and try again.")
                                   .arg(differ.join(QStringLiteral(", ")), QFileInfo(file).fileName()));
        }
    }
    written(file, held);
    QJsonObject result{{QStringLiteral("subcircuit"), QDir::toNativeSeparators(file)},
                       {QStringLiteral("instance"), subName},
                       {QStringLiteral("moved in"), QJsonArray::fromStringList(names)},
                       {QStringLiteral("ports"), ports},
                       {QStringLiteral("grounds inside"), int(grounded.size())},
                       {QStringLiteral("one step to undo"), true},
                       {QStringLiteral("note"), tr("Its pins are joined to their nets by net labels. The file stays written when this is "
                                                   "undone. open_document opens it; its symbol is a box with a pin for each port until one "
                                                   "is drawn (add_painting with symbol).")}};
    QStringList naming;
    for (const QString& net : std::as_const(goneInside)) {
        QStringList by = tracesNaming(showingDataOf(sch), net);
        by << equationsNaming(sch, net);
        if (!by.isEmpty()) naming << tr("%1 (named by %2)").arg(net, by.join(QStringLiteral(", ")));
    }
    if (!naming.isEmpty())
        result.insert(QStringLiteral("inside now"), tr("These nets went inside %1, and nothing outside is on them: %2. What names them "
                                                       "shows nothing after the next simulation - label a net outside instead, or bring "
                                                       "the net out through a port.")
                                                        .arg(subName, naming.join(QStringLiteral("; "))));
    return jsonResult(result);
}

QJsonObject QucsControl::remove(const QJsonObject& args)
{
    QString error, note;
    Schematic* sch = nullptr;
    QStringList missing, done;
    QList<Element*> doomed;
    // Paintings, by their numbers: of the schematic, or of its symbol
    // (which has nothing else to delete).
    const QJsonArray paintingNumbers = args.value(QLatin1String("paintings")).toArray();
    if (!paintingNumbers.isEmpty()) {
        std::list<Painting*>* list = nullptr;
        sch = paintingsOf(args, &list, &note, &error);
        if (sch == nullptr) return errorResult(error);
        if (sch->getSymbolMode())
            for (const char* other : {"names", "wires", "diagrams", "traces"})
                if (!args.value(QLatin1String(other)).toArray().isEmpty())
                    return errorResult(tr("A symbol has only paintings: delete %1 of the schematic in a call of their own.").arg(QLatin1String(other)));
        for (const QJsonValue& v : paintingNumbers) {
            Painting* p = paintingOf(*list, v, &error);
            if (p == nullptr) return errorResult(error);
            if (isFixedPainting(p))
                return errorResult(tr("Painting %1 is the symbol's %2: its ports follow the schematic's Port components, and its name "
                                      "text stays (edit_painting moves either).")
                                       .arg(v.toInt()).arg(paintingJson(p, v.toInt()).value(QStringLiteral("type")).toString()));
            if (doomed.contains(p)) continue;
            doomed << p;
            done << tr("painting %1 (a %2)").arg(v.toInt()).arg(paintingJson(p, v.toInt()).value(QStringLiteral("type")).toString());
        }
    } else {
        sch = schematic(args, &error, true);
        if (sch == nullptr) return errorResult(error);
    }
    QList<Conductor*> unlabelled;
    // (Each looked up in tables made once: by name among all, the parts
    // of a 15,000-part selection took seconds.)
    const QJsonArray names = args.value(QLatin1String("names")).toArray();
    QSet<Element*> doomedSet(doomed.begin(), doomed.end());
    QHash<QString, QList<Conductor*>> labelled;
    QHash<const Component*, QString> refs;
    std::optional<PartIndex> parts;
    if (!names.isEmpty()) {
        for (Wire* w : sch->a_DocWires)
            if (w->hasLabel()) labelled[w->label()->Name] << w;
        for (Node* n : sch->a_DocNodes)
            if (n->hasLabel()) labelled[n->label()->Name] << n;
        refs = PartIndex::refs(sch);
        parts.emplace(sch);
    }
    for (const QJsonValue& v : names) {
        const QString name = v.toString().trimmed();
        const auto doom = [&](Component* c) {
            if (doomedSet.contains(c)) return;
            doomedSet.insert(c);
            doomed << c;
            done << refs.value(c, c->Name);
        };
        // A part by its name first.
        if (!name.isEmpty() && !name.contains(QLatin1Char('#'))) {
            QString unused;
            Component* named = parts->find(name, &unused);
            if (named != nullptr && named->Name == name) {
                doom(named);
                continue;
            }
        }
        const QList<Conductor*> withLabel = labelled.value(name);
        unlabelled << withLabel;
        const bool label = !withLabel.isEmpty();
        // A ground by its ref (GND#2), when no label is named so.
        QString unknown;
        if (label) done << tr("the label %1").arg(name);
        else if (Component* c = parts->find(name, &unknown)) doom(c);
        else if (toldByNumber(sch, name)) return errorResult(unknown);
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
        return errorResult(missing.isEmpty() ? tr("Nothing to delete.") : notFound(missing));
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
    QString text = tr("Deleted %1.").arg(atMost(done).join(QStringLiteral(", ")));
    if (!missing.isEmpty()) text += QLatin1Char(' ') + notFound(missing);
    if (!note.isEmpty()) text += QLatin1Char(' ') + note;
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
    if (pin.isEmpty()) {   // (null, or nothing: " is not a pin ...")
        *error = tr("Which place? A pin (\"R1.2\") or a place ([x, y]).");
        return false;
    }
    if (dot <= 0) {
        *error = tr("%1 is not a pin (\"R1.2\") or a place ([x, y]).").arg(pin);
        return false;
    }
    const QString name = pin.left(dot), which = pin.mid(dot + 1);
    // A part without a name (a ground) by its type, or its number among
    // them: GND#2.1.
    Component* c = componentOf(sch, name, error);
    if (c == nullptr) {
        if (const qsizetype many = unnamedOf(sch, name).size(); many > 1)
            *error = tr("There are %1 of %2: say which, %2#1.1 to %2#%1.1 (get_schematic gives each its 'ref'), or give the pin's place, [x, y].")
                         .arg(many).arg(name);
        return false;
    }
    bool number = false;
    const int n = which.toInt(&number);
    const Port* port = nullptr;
    if (number && n >= 1 && n <= c->Ports.size()) port = c->Ports.at(n - 1);
    for (const Port* p : c->Ports)
        if (port == nullptr && !p->Name.isEmpty() && p->Name.compare(which, Qt::CaseInsensitive) == 0) port = p;
    if (port == nullptr) {
        QStringList names;
        for (int i = 0; i < c->Ports.size(); ++i)
            if (!c->Ports.at(i)->Name.isEmpty()) names << QStringLiteral("%1 %2").arg(i + 1).arg(c->Ports.at(i)->Name);
        *error = names.isEmpty() ? tr("%1 has no pin %2 (it has %3, by number).").arg(name, which).arg(c->Ports.size())
                                 : tr("%1 has no pin %2 (it has %3: %4).").arg(name, which).arg(c->Ports.size()).arg(names.join(QStringLiteral(", ")));
        return false;
    }
    *point = QPoint(c->cx + port->x, c->cy + port->y);
    return true;
}

namespace {

// Rectangles by place, in cells of 64 x 64: whether one meets a given one
// is looked up in the cells it covers, not among all of them.
class RectCells
{
public:
    void add(const QRect& r)
    {
        const QPoint from = cellOf(r.topLeft()), to = cellOf(r.bottomRight());
        if ((qint64(to.x()) - from.x() + 1) * (qint64(to.y()) - from.y() + 1) > 64) {
            m_wide << r;
            return;
        }
        for (int x = from.x(); x <= to.x(); ++x)
            for (int y = from.y(); y <= to.y(); ++y) m_cells[QPoint(x, y)] << r;
    }
    bool meets(const QRect& r) const
    {
        if (std::any_of(m_wide.cbegin(), m_wide.cend(), [&r](const QRect& o) { return o.intersects(r); })) return true;
        const QPoint from = cellOf(r.topLeft()), to = cellOf(r.bottomRight());
        for (int x = from.x(); x <= to.x(); ++x)
            for (int y = from.y(); y <= to.y(); ++y)
                if (const auto it = m_cells.constFind(QPoint(x, y)); it != m_cells.cend())
                    if (std::any_of(it->cbegin(), it->cend(), [&r](const QRect& o) { return o.intersects(r); })) return true;
        return false;
    }

private:
    static QPoint cellOf(const QPoint& p) { return {p.x() >> 6, p.y() >> 6}; }
    QHash<QPoint, QList<QRect>> m_cells;
    QList<QRect> m_wide;
};

// Each net label on a pin (arrange puts them there) moved on to the longest
// stretch of its piece's wiring - a straight one lying down first - where
// its text keeps clear of the parts, their texts, the wires and the other
// labels: at a pin's end its text sat over the drawing (out on Rf's wire).
// A label where there is no such stretch stays; \a keep's (the supplies'
// on each of their pins) stay.
void relabelOnWires(Schematic* sch, const QSet<QString>& keep)
{
    RectCells room;
    for (const Component* c : sch->a_DocComps) room.add(c->boundingRectIncludingProperties().adjusted(-2, -2, 2, 2));
    for (const Wire* w : sch->a_DocWires) room.add(QRect(w->P1(), w->P2()).normalized().adjusted(-3, -3, 3, 3));
    // (A label's text from its top left corner, as it is drawn.)
    const auto textBox = [](const QString& name, const QPoint& at) { return QRect(at, QSize(8 * int(name.size()) + 10, 16)); };
    std::vector<Node*> labelled;
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel()) {
            labelled.push_back(n);
            room.add(textBox(n->label()->Name, QPoint(n->label()->x1, n->label()->y1)));
        }
    // (In order of place: the first takes the room.)
    std::sort(labelled.begin(), labelled.end(), [](const Node* a, const Node* b) { return std::pair(a->cx, a->cy) < std::pair(b->cx, b->cy); });
    const int gx = std::max(sch->getGridX(), 1), gy = std::max(sch->getGridY(), 1);
    for (Node* n : labelled) {
        const QString name = n->label()->Name;
        if (keep.contains(name)) continue;
        // Its piece's wires, joined through their nodes.
        QList<Wire*> wires;
        QSet<const Node*> seen{n};
        std::vector<const Node*> todo{n};
        while (!todo.empty()) {
            const Node* at = todo.back();
            todo.pop_back();
            for (Wire* w : at->wires()) {
                if (wires.contains(w)) continue;
                wires << w;
                for (const Node* o : {w->Port1, w->Port2})
                    if (o != nullptr && !seen.contains(o)) {
                        seen.insert(o);
                        todo.push_back(o);
                    }
            }
        }
        std::sort(wires.begin(), wires.end(), [](const Wire* a, const Wire* b) {
            const bool la = a->P1().y() == a->P2().y(), lb = b->P1().y() == b->P2().y();
            const int da = (a->P1() - a->P2()).manhattanLength(), db = (b->P1() - b->P2()).manhattanLength();
            return std::tuple(!la, -da, a->P1().x(), a->P1().y()) < std::tuple(!lb, -db, b->P1().x(), b->P1().y());
        });
        for (Wire* w : std::as_const(wires)) {
            const QPoint p = w->P1(), q = w->P2();
            const bool across = p.y() == q.y();
            if ((!across && p.x() != q.x()) || (p - q).manhattanLength() < 4 * std::max(gx, gy) || w->hasLabel()) continue;
            int x = (p.x() + q.x()) / 2, y = (p.y() + q.y()) / 2;
            sch->setOnGrid(x, y);
            if (across) y = p.y();
            else x = p.x();
            const QPoint anchor(x, y);
            if (anchor == p || anchor == q) continue;
            const QRect box0 = textBox(name, QPoint());
            // (Above it on the right, below it, above it on the left; beside
            // a standing one, right or left.)
            const QList<QPoint> offsets = across ? QList<QPoint>{QPoint(10, -10 - box0.height()), QPoint(10, 10), QPoint(-10 - box0.width(), -10 - box0.height())}
                                                 : QList<QPoint>{QPoint(10, -box0.height() / 2), QPoint(-10 - box0.width(), -box0.height() / 2)};
            bool moved = false;
            for (const QPoint& d : offsets) {
                const QPoint text = anchor + d;
                if (room.meets(textBox(name, text))) continue;
                n->dropLabel();
                w->setName(name, QString(), anchor.x(), anchor.y(), text.x(), text.y());
                room.add(textBox(name, text));
                moved = true;
                break;
            }
            if (moved) break;
        }
    }
}

// Where ground symbols may go, for many of them (arrange): the parts'
// symbols and texts, the nodes and the wires by place - each part's texts
// measured once, not every part's for every ground (a ladder's 500 grounds
// measured 2000 parts' texts each: 24 s). What placeGround() adds goes in.
class GroundRoom
{
public:
    explicit GroundRoom(Schematic* sch)
    {
        for (const Component* c : sch->a_DocComps) addPart(c);
        for (const Node* n : sch->a_DocNodes) addNode(n->center());
        for (const Wire* w : sch->a_DocWires) addWire(w->P1(), w->P2());
    }
    void addNode(const QPoint& p)
    {
        if (m_nodes.contains(p)) return;
        m_nodes.insert(p);
        m_nodeCells[QPoint(p.x() >> 6, p.y() >> 6)] << p;
    }
    void addPart(const Component* c)
    {
        m_symbols.add(c->boundingRect().adjusted(1, 1, -1, -1));
        m_texts.add(c->boundingRectIncludingProperties().adjusted(1, 1, -1, -1));
        if (!c->Ports.isEmpty()) m_pinned.add(c->boundingRect().adjusted(1, 1, -1, -1));
        for (const Port* p : c->Ports) addNode(QPoint(c->cx + p->x, c->cy + p->y));
    }
    void addWire(const QPoint& a, const QPoint& b)
    {
        addNode(a);
        addNode(b);
        const QRect r = QRect(a, b).normalized();
        m_wireBoxes.add(r.adjusted(-1, -1, 1, 1));
        const QPoint from{r.left() >> 6, r.top() >> 6}, to{r.right() >> 6, r.bottom() >> 6};
        for (int x = from.x(); x <= to.x(); ++x)
            for (int y = from.y(); y <= to.y(); ++y) m_wires[QPoint(x, y)].append({a, b});
    }
    // placeGround()'s test of a ground's symbol \a box, its pin at \a gp,
    // for the pin at \a p: clear of the parts (and \a texts, their texts),
    // of every node but \a p's, of every wire but those at \a p (going down
    // from \a p, where it hangs, when it is on the pin) - and, off the pin,
    // its own pin on nothing.
    bool clear(const QRect& box, const QPoint& p, const QPoint& gp, bool texts) const
    {
        if (m_symbols.meets(box) || (texts && m_texts.meets(box))) return false;
        for (int x = box.left() >> 6; x <= box.right() >> 6; ++x)
            for (int y = box.top() >> 6; y <= box.bottom() >> 6; ++y)
                if (const auto it = m_nodeCells.constFind(QPoint(x, y)); it != m_nodeCells.cend())
                    for (const QPoint& n : *it)
                        if (n != p && box.contains(n)) return false;
        bool clear = true;
        forWiresNear(box.adjusted(-1, -1, 1, 1), [&](const QPoint& a, const QPoint& b) {
            if (a == p || b == p) {
                const QPoint o = a == p ? b : a;
                if (gp == p && o.x() == p.x() && o.y() > p.y()) clear = false;
                if (gp == p) return;
            }
            if (QRect(a, b).normalized().adjusted(-1, -1, 1, 1).intersects(box)) clear = false;
        });
        if (!clear) return false;
        if (gp != p) return !m_nodes.contains(gp) && !onAWire(gp);
        return true;
    }
    // A way from \a p to a ground's pin \a gp (its symbol \a box) that
    // touches nothing: straight pieces, no node on it but at its ends, no
    // bend on a wire, no part crossed, nor the ground itself.
    bool clearWay(const std::vector<QPoint>& way, const QRect& box) const
    {
        const QPoint a = way.front(), b = way.back();
        for (std::size_t k = 1; k < way.size(); ++k) {
            const QPoint p = way[k - 1], q = way[k];
            if (p == q) continue;
            if (p.x() != q.x() && p.y() != q.y()) return false;
            const QRect piece = QRect(p, q).normalized();
            if (m_pinned.meets(piece) || piece.adjusted(1, 1, -1, -1).intersects(box)) return false;
            const int dx = (q.x() > p.x()) - (q.x() < p.x()), dy = (q.y() > p.y()) - (q.y() < p.y());
            // (Every node is on the grid it was put on: the points of the
            // piece looked up in the set, not the set searched.)
            for (QPoint c = p;; c += QPoint(dx, dy)) {
                if (c != a && c != b && m_nodes.contains(c)) return false;
                if (c == q) break;
            }
        }
        for (std::size_t k = 1; k + 1 < way.size(); ++k)
            if (onAWire(way[k])) return false;
        return true;
    }

private:
    template <typename F>
    void forWiresNear(const QRect& r, F f) const
    {
        if (!m_wireBoxes.meets(r)) return;
        QSet<QPair<QPoint, QPoint>> seen;
        for (int x = r.left() >> 6; x <= r.right() >> 6; ++x)
            for (int y = r.top() >> 6; y <= r.bottom() >> 6; ++y)
                if (const auto it = m_wires.constFind(QPoint(x, y)); it != m_wires.cend())
                    for (const auto& [a, b] : *it)
                        if (!seen.contains({a, b})) {
                            seen.insert({a, b});
                            f(a, b);
                        }
    }
    bool onAWire(const QPoint& p) const
    {
        bool on = false;
        forWiresNear(QRect(p, p), [&](const QPoint& a, const QPoint& b) { on = on || onSegment(p, a, b); });
        return on;
    }
    RectCells m_symbols, m_texts, m_pinned, m_wireBoxes;
    QSet<QPoint> m_nodes;
    QHash<QPoint, QList<QPoint>> m_nodeCells;
    QHash<QPoint, QList<std::pair<QPoint, QPoint>>> m_wires;
};

// A ground symbol for the pin at \a p: on the pin when it fits there
// (under a part's lower pin), else a little away, wired to it by a way
// that joins nothing else. Where its pin is in \a where. False, and \a sch
// as it was, when there is no room. (Elements may be new ones after:
// a way that would not do is taken back.) With \a room - many grounds -
// what is in the way is looked up there, and what it adds put in: a
// place is taken only when nothing is in its way, so nothing is taken back.
bool placeGround(Schematic* sch, const QPoint& p, QPoint* where, GroundRoom* room = nullptr)
{
    if (room != nullptr) {
        const int gx = std::max(sch->getGridX(), 1), gy = std::max(sch->getGridY(), 1);
        const QList<QPoint> places{p, p + QPoint(0, 2 * gy), p + QPoint(0, 3 * gy), p + QPoint(-3 * gx, 2 * gy),
                                   p + QPoint(3 * gx, 2 * gy), p + QPoint(0, 5 * gy), p + QPoint(-5 * gx, 3 * gy),
                                   p + QPoint(5 * gx, 3 * gy)};
        for (const bool clearOfTexts : {true, false})
            for (const QPoint& q : places) {
                std::unique_ptr<Component> g(newComponent(QStringLiteral("GND")));
                if (!g) return false;
                g->setSchematic(sch);
                g->recreate();
                int x = q.x(), y = q.y();
                sch->setOnGrid(x, y);
                g->moveCenter(x - g->cx, y - g->cy);
                const QPoint gp(g->cx + g->Ports.first()->x, g->cy + g->Ports.first()->y);
                const QRect box = g->boundingRect().adjusted(1, 1, -1, -1);
                if (!room->clear(box, p, gp, clearOfTexts)) continue;
                std::vector<QPoint> way;
                if (gp != p) {
                    for (const std::vector<QPoint>& w : waysBetween(sch, p, gp))
                        if (room->clearWay(w, box)) {
                            way = w;
                            break;
                        }
                    if (way.empty()) continue;
                }
                Component* ground = g.release();
                sch->insertComponent(ground);
                room->addPart(ground);
                for (std::size_t k = 1; k < way.size(); ++k)
                    if (way[k] != way[k - 1]) {
                        sch->connectWithWire(way[k - 1], way[k], true, qucs_s::wire::Planner::PlanType::Straight);
                        room->addWire(way[k - 1], way[k]);
                    }
                *where = gp;
                return true;
            }
        return false;
    }
    const QString at = QStringLiteral("at %1,%2").arg(p.x()).arg(p.y());
    const Nets before = netsOf(sch, nullptr, {p});
    const int gx = std::max(sch->getGridX(), 1), gy = std::max(sch->getGridY(), 1);
    // On the pin; below it; beside and below; further.
    const QList<QPoint> places{p, p + QPoint(0, 2 * gy), p + QPoint(0, 3 * gy), p + QPoint(-3 * gx, 2 * gy),
                               p + QPoint(3 * gx, 2 * gy), p + QPoint(0, 5 * gy), p + QPoint(-5 * gx, 3 * gy),
                               p + QPoint(5 * gx, 3 * gy)};
    const QString state = sch->snapshot();
    // Clear of the parts' texts too, when it can be; else of their symbols.
    for (const bool clearOfTexts : {true, false})
    for (const QPoint& q : places) {
        std::unique_ptr<Component> g(newComponent(QStringLiteral("GND")));
        if (!g) return false;
        g->setSchematic(sch);
        g->recreate();
        int x = q.x(), y = q.y();
        sch->setOnGrid(x, y);
        g->moveCenter(x - g->cx, y - g->cy);
        const QPoint gp(g->cx + g->Ports.first()->x, g->cy + g->Ports.first()->y);
        // Its symbol clear of every part and its texts, and of every pin and
        // wire but the one grounded.
        const QRect box = g->boundingRect().adjusted(1, 1, -1, -1);
        bool clear = true;
        for (const Component* c : sch->a_DocComps)
            clear = clear && !box.intersects(c->boundingRect().adjusted(1, 1, -1, -1))
                    && !(clearOfTexts && box.intersects(c->boundingRectIncludingProperties().adjusted(1, 1, -1, -1)));
        for (const Node* n : sch->a_DocNodes) clear = clear && (n->center() == p || !box.contains(n->center()));
        for (const Wire* w : sch->a_DocWires) {
            if (w->P1() == p || w->P2() == p) {   // on the pin: in the way only going down, where the symbol hangs
                const QPoint o = w->P1() == p ? w->P2() : w->P1();
                clear = clear && !(gp == p && o.x() == p.x() && o.y() > p.y());
                if (gp == p) continue;
            }
            clear = clear && !QRect(w->P1(), w->P2()).normalized().adjusted(-1, -1, 1, 1).intersects(box);
        }
        if (!clear) continue;
        sch->insertComponent(g.release());
        const auto check = [&] { return netChangesBeyond(before, netsOf(sch, nullptr, {p}), {at, QStringLiteral("ground")}); };
        bool done = gp == p ? check().isEmpty() : false;
        if (gp != p) {
            QString why;
            done = wireUp(sch, p, gp, check, &why);
        }
        const Nets now = netsOf(sch, nullptr, {p});
        if (done && now.netOf.value(at) == now.netOf.value(QStringLiteral("ground"), -2)) {
            *where = gp;
            return true;
        }
        sch->restore(state);
    }
    return false;
}

} // namespace

// A pin to ground: a ground symbol of its own (placeGround()) - not a
// ground elsewhere found by its number among them (GND#2.1).
QJsonObject QucsControl::groundPin(Schematic* sch, const QJsonValue& pin)
{
    QString error;
    QPoint p;
    if (!pointOf(sch, pin, &p, &error)) return errorResult(error);
    const QString named = pin.isString() ? pin.toString() : QStringLiteral("%1, %2").arg(p.x()).arg(p.y());
    const QString at = QStringLiteral("at %1,%2").arg(p.x()).arg(p.y());
    const Nets before = netsOf(sch, nullptr, {p});
    if (before.netOf.contains(QStringLiteral("ground")) && before.netOf.value(at) == before.netOf.value(QStringLiteral("ground")))
        return textResult(tr("%1 is on ground already: nothing drawn.").arg(named));
    prepare(sch);
    QPoint gp;
    if (!placeGround(sch, p, &gp))
        return errorResult(tr("%1 is not grounded: there is no room for a ground symbol near it that a wire reaches without "
                              "touching anything else. Place one with add_component (type GND) and connect to it.").arg(named));
    finish(sch, {p, gp});
    return textResult(gp == p ? tr("%1 is grounded: a ground symbol on it.").arg(named)
                              : tr("%1 is grounded: a ground symbol at %2, %3, wired to it.").arg(named).arg(gp.x()).arg(gp.y()));
}

// ----------------------------------------------------------------------
// The schematic laid out again for a person to read: its parts in columns
// by signal flow, every wire drawn again, every net kept.

namespace {

// Whether a part is a source a signal starts from: in a sources category
// of the library, with two pins (a controlled source has four).
bool isSignalSource(const Component* c)
{
    static const QSet<QString> sources = [] {
        QSet<QString> s;
        for (Category* category : Category::Categories)
            if (category->Name.contains(QLatin1String("source"), Qt::CaseInsensitive))
                for (Module* m : category->Content)
                    if (const QString t = typeOf(m); !t.isEmpty()) s << t;
        return s;
    }();
    static const QRegularExpression named(QStringLiteral("^[VI](dc|ac|pulse|rect|noise|exp|file|am|pm)$"));
    return c->Ports.size() == 2 && (sources.contains(c->Model) || named.match(c->Model).hasMatch() || c->Model == QLatin1String("Pac"));
}

// netsOf's keys of \a sch's ground symbols' pins, by the symbols themselves
// (a port may be named GND; an old file names its grounds *1, *2).
QSet<QString> groundSymbolKeys(const Schematic* sch)
{
    QSet<QString> keys;
    QHash<QString, int> seen;
    for (const Component* c : sch->a_DocComps) {
        const QString base = keyBase(c, seen);
        if (c->Model == QLatin1String("GND"))
            for (int k = 0; k < c->Ports.size(); ++k) keys << base + QLatin1Char('.') + QString::number(k + 1);
    }
    return keys;
}

// netsOf's keys of \a sch without those of its ground symbols' pins (they
// are put back anew, numbered otherwise) and without \a dropped labels.
Nets withoutGroundSymbols(const Schematic* sch, Nets nets, const QStringList& dropped)
{
    for (const QString& key : groundSymbolKeys(sch)) nets.netOf.remove(key);
    for (const QString& name : dropped) nets.netOf.remove(QStringLiteral("label ") + name);
    return nets;
}

} // namespace

namespace {

// Each net label of \a sch.
QList<WireLabel*> netLabels(Schematic* sch)
{
    QList<WireLabel*> labels;
    for (Wire* w : sch->a_DocWires)
        if (w->label() != nullptr) labels << w->label();
    for (Node* n : sch->a_DocNodes)
        if (n->label() != nullptr) labels << n->label();
    return labels;
}

} // namespace

// arrange's 'labels': only texts move - each part's text and each net
// label's that is drawn over something, to the nearest free spot beside
// it; the others stay, and nothing else changes.
QJsonObject QucsControl::arrangeTexts(Schematic* sch)
{
    namespace tp = qucs_s::textplace;
    const int before = int(tp::overlaps(sch).size());
    QList<tp::Thing> all = tp::things(sch);
    // Those drawn over something are taken up first, out of the others' way.
    QList<Component*> parts;
    for (Component* c : sch->a_DocComps)
        if (!tp::textBoxes(c).isEmpty() && tp::textOverlaps(c, all)) parts << c;
    QList<WireLabel*> labels;
    for (WireLabel* l : netLabels(sch))
        if (tp::labelOverlaps(l, all)) labels << l;
    if (parts.isEmpty() && labels.isEmpty())
        return jsonResult(QJsonObject{{QStringLiteral("document"), titleOf(sch)},
                                      {QStringLiteral("moved"), QJsonArray()},
                                      {QStringLiteral("arranged"), tr("Nothing moved: no text is drawn over anything.")}});
    for (const Component* c : std::as_const(parts)) all = tp::without(all, c, true);
    for (const WireLabel* l : std::as_const(labels)) all = tp::without(all, l, true);
    // From the top left, as a page is read.
    std::stable_sort(parts.begin(), parts.end(), [](const Component* a, const Component* b) {
        return std::tie(a->cy, a->cx) < std::tie(b->cy, b->cx);
    });
    prepare(sch);
    const QHash<const Component*, QString> refs = qucs_s::erc::refs(sch);
    QJsonArray moved, left;
    QList<QPoint> where;
    for (Component* c : std::as_const(parts)) {
        const QString name = refs.value(c, c->Name.isEmpty() ? c->Model : c->Name);
        QString side;
        if (const std::optional<QPoint> spot = tp::freeSpot(c, all, &side)) {
            c->tx = spot->x();
            c->ty = spot->y();
            moved.append(tr("%1's text: %2 of it, [%3, %4]").arg(name, side).arg(spot->x()).arg(spot->y()));
            where << QPoint(c->cx, c->cy);
        } else {
            left.append(tr("%1's text: no spot beside it is free").arg(name));
        }
        // (Where it is now is taken.)
        for (const tp::Thing& t : tp::things(sch))
            if (t.owner == c && t.kind == tp::Thing::Text) all << t;
    }
    for (WireLabel* l : std::as_const(labels)) {
        if (const std::optional<QPoint> spot = tp::freeLabelSpot(l, all)) {
            l->moveCenter(spot->x() - l->x1, spot->y() - l->y1);
            moved.append(tr("the label %1: at [%2, %3]").arg(l->Name).arg(spot->x()).arg(spot->y()));
            where << QPoint(l->cx, l->cy);
        } else {
            left.append(tr("the label %1: no spot near it is free").arg(l->Name));
        }
        tp::Thing t;
        t.kind = tp::Thing::Label;
        t.box = tp::labelBox(l);
        t.owner = l;
        t.name = tr("the label %1").arg(l->Name);
        all << t;
    }
    if (!moved.isEmpty()) finish(sch, where);
    const QList<tp::Overlap> after = tp::overlaps(sch);
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)},
                       {QStringLiteral("moved"), moved},
                       {QStringLiteral("arranged"), tr("%1 texts moved clear, no part or wire: %2 texts were drawn over something, %3 are now")
                                                        .arg(moved.size()).arg(before).arg(after.size())}};
    if (!left.isEmpty()) result.insert(QStringLiteral("left where they were"), left);
    if (!after.isEmpty()) {
        QJsonArray still;
        for (const tp::Overlap& o : after)
            if (still.size() < 20) still.append(o.message);
        result.insert(QStringLiteral("still"), still);
    }
    if (!moved.isEmpty()) result.insert(QStringLiteral("one step to undo"), true);
    return jsonResult(result);
}

QJsonObject QucsControl::arrange(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    if (args.value(QLatin1String("labels")).toBool()) {
        for (const char* other : {"spacing", "wire_labels", "keep_places", "straighten", "feedback", "supplies"})
            if (args.contains(QLatin1String(other)))
                return errorResult(tr("'labels' moves only texts, never a part or a wire: it takes no '%1' (arrange without 'labels' "
                                      "lays the parts out).").arg(QLatin1String(other)));
        return arrangeTexts(sch);
    }
    const int asked = std::clamp(args.value(QLatin1String("spacing")).toInt(60), 30, 400);
    const bool wireLabels = args.value(QLatin1String("wire_labels")).toBool();
    // The parts where they are, the wiring drawn again ("tidy"); a feedback
    // part (Rf round an op-amp) below or above the part it goes round; the
    // supplies as labels on every pin of theirs and a ground symbol on each
    // pin on ground (up and down), not wires from a column of them.
    const bool keepPlaces = args.value(QLatin1String("keep_places")).toBool();
    const bool straighten = args.value(QLatin1String("straighten")).toBool();
    const QString feedback = args.value(QLatin1String("feedback")).toString(QStringLiteral("inline")).trimmed().toLower();
    const QString supplies = args.value(QLatin1String("supplies")).toString(QStringLiteral("column")).trimmed().toLower();
    if (feedback != QLatin1String("inline") && feedback != QLatin1String("below") && feedback != QLatin1String("above"))
        return errorResult(tr("'feedback' is inline (the default: in the columns), below or above (the part it goes round)."));
    if (supplies != QLatin1String("column") && supplies != QLatin1String("labels"))
        return errorResult(tr("'supplies' is column (the default: a column of them at the left, wired) or labels (a label on each "
                              "pin of a supply, a ground symbol on each pin on ground)."));
    if (keepPlaces && (args.contains(QLatin1String("feedback")) || args.contains(QLatin1String("spacing"))))
        return errorResult(tr("'keep_places' keeps every part where it is: 'feedback' and 'spacing' place parts."));
    const bool railLabels = supplies == QLatin1String("labels");
    prepare(sch);
    const QString state = sch->snapshot();
    QStringList faults;
    // Tried with more room when the wires cannot all be drawn - and then
    // with the parts turned as they are (turned, they may leave no way).
    // (Where they are: once, as they are turned.)
    std::vector<std::pair<int, bool>> tries{{asked, true}, {asked * 3 / 2, true}, {asked, false}, {asked * 3 / 2, false}, {asked * 2, false}};
    if (keepPlaces) tries = {{asked, false}};
    for (const auto& [spacing, turn] : tries) {
        const int gy = std::max(sch->getGridY(), 1);
        // The parts with pins are laid out; the ground symbols put back anew;
        // blocks without pins (analyses, equations) go in a row below.
        std::vector<Component*> parts, grounds, blocks;
        for (Component* c : sch->a_DocComps) {
            if (c->Model == QLatin1String("GND")) grounds.push_back(c);
            else if (c->Ports.isEmpty()) blocks.push_back(c);
            else parts.push_back(c);
        }
        if (parts.empty()) return errorResult(tr("%1 has no parts with pins to arrange.").arg(titleOf(sch)));
        std::vector<QPoint> home(parts.size());   // (where each is: kept, with keep_places)
        for (std::size_t i = 0; i < parts.size(); ++i) home[i] = parts[i]->center();
        QHash<const Component*, QString> keyOf;   // netsOf's name of each
        {
            QHash<QString, int> seen;
            for (Component* c : sch->a_DocComps) keyOf.insert(c, keyBase(c, seen));
        }
        const Nets before = netsOf(sch, nullptr);

        // Pieces of circuit drawn together (wires, pins on one node), each
        // with its ground symbol and net labels - put back so.
        QHash<const Node*, const Node*> up;
        const auto root = [&up](const Node* n) {
            const Node* r = n;
            while (up.value(r, r) != r) r = up.value(r);
            while (up.value(n, n) != n) {
                const Node* next = up.value(n);
                up.insert(n, r);
                n = next;
            }
            return r;
        };
        for (const Wire* w : sch->a_DocWires)
            if (w->Port1 != nullptr && w->Port2 != nullptr) {
                const Node *a = root(w->Port1), *b = root(w->Port2);
                if (a != b) up.insert(a, b);
            }
        struct Piece {
            QList<QPair<int, int>> pins;   // part, pin
            QStringList labels;
            bool grounded = false;
        };
        QHash<const Node*, Piece> pieces;
        for (int i = 0; i < int(parts.size()); ++i)
            for (int k = 0; k < parts[i]->Ports.size(); ++k)
                if (const Node* n = parts[i]->Ports.at(k)->Connection) pieces[root(n)].pins.append({i, k});
        for (const Component* g : grounds)
            for (const Port* p : g->Ports)
                if (p->Connection != nullptr) pieces[root(p->Connection)].grounded = true;
        const auto labelOn = [&](const Conductor* c, const Node* n) {
            if (!c->hasLabel()) return;
            Piece& piece = pieces[root(n)];
            if (!piece.labels.contains(c->label()->Name)) piece.labels << c->label()->Name;
        };
        for (const Wire* w : sch->a_DocWires)
            if (w->Port1 != nullptr) labelOn(w, w->Port1);
        for (const Node* n : sch->a_DocNodes) labelOn(n, n);
        // A label on wires to no pin, of a name no pin's net has: it joins
        // nothing - dropped, and said.
        QSet<QString> onPins;
        for (const Piece& piece : std::as_const(pieces))
            if (!piece.pins.isEmpty())
                for (const QString& l : piece.labels) onPins << l;
        QStringList dropped;
        for (const Piece& piece : std::as_const(pieces))
            if (piece.pins.isEmpty())
                for (const QString& l : piece.labels)
                    if (!onPins.contains(l) && !dropped.contains(l)) dropped << l;
        const Nets was = withoutGroundSymbols(sch, before, dropped);

        // Columns by signal flow: the sources first, each part a column to
        // the right of what drives it - through the nets that are not ground
        // nor a rail (a supply: a DC source's net with three parts or more,
        // or any net of more than five), which join too much.
        const int groundNet = before.netOf.value(QStringLiteral("ground"), -1);
        std::vector<QList<int>> pinNet(parts.size());   // each pin's net
        QHash<int, QSet<int>> partsOfNet;
        for (int i = 0; i < int(parts.size()); ++i)
            for (int k = 0; k < parts[i]->Ports.size(); ++k) {
                const int net = before.netOf.value(keyOf.value(parts[i]) + QLatin1Char('.') + QString::number(k + 1), -1);
                pinNet[i] << net;
                if (net >= 0) partsOfNet[net].insert(i);
            }
        QSet<int> rails;
        for (auto it = partsOfNet.cbegin(); it != partsOfNet.cend(); ++it)
            if (it.key() != groundNet && it.value().size() > 5) rails << it.key();
        for (int i = 0; i < int(parts.size()); ++i)
            if (parts[i]->Model == QLatin1String("Vdc") || parts[i]->Model == QLatin1String("Idc"))
                for (int net : std::as_const(pinNet[i]))
                    if (net >= 0 && net != groundNet && partsOfNet.value(net).size() >= 3) rails << net;
        // And a DC source's net to a supply pin (a 741's VCC) or of a
        // supply's name (vcc, vee, vdd ...), however few parts are on it: the
        // flow of the signal does not go through it.
        {
            static const QRegularExpression supplyName(QStringLiteral("^(v(cc|dd|ee|ss|s[+-]|[+-]|p|n|pos|neg)\\d*|(pos|neg)rail|avdd|dvdd|vbat)$"),
                                                       QRegularExpression::CaseInsensitiveOption);
            QSet<int> supplyish;
            for (int i = 0; i < int(parts.size()); ++i)
                for (int k = 0; k < parts[i]->Ports.size(); ++k)
                    if (supplyName.match(parts[i]->Ports.at(k)->Name).hasMatch() && pinNet[i][k] >= 0) supplyish << pinNet[i][k];
            for (auto it = before.netOf.cbegin(); it != before.netOf.cend(); ++it)
                if (it.key().startsWith(QLatin1String("label ")) && supplyName.match(it.key().mid(6)).hasMatch()) supplyish << it.value();
            for (int i = 0; i < int(parts.size()); ++i)
                if ((parts[i]->Model == QLatin1String("Vdc") || parts[i]->Model == QLatin1String("Idc")) && pinNet[i].size() == 2
                    && (pinNet[i][0] == groundNet || pinNet[i][1] == groundNet))
                    for (int net : std::as_const(pinNet[i]))
                        if (net >= 0 && net != groundNet && supplyish.contains(net)) rails << net;
        }
        const auto railish = [&](int net) { return net < 0 || net == groundNet || rails.contains(net); };
        QList<Piece> ownPieces;   // (a pin's own: of a rail, or on ground)
        QSet<QString> railNames;
        // Supplies as labels: each pin on a rail a piece of its own with the
        // rail's label, each pin on ground one with a ground symbol - no
        // wires for them. A rail without a label is named after its source's
        // polarity: VCC above ground, VEE below.
        if (railLabels) {
            QSet<QString> taken;
            for (auto it = before.netOf.cbegin(); it != before.netOf.cend(); ++it)
                if (it.key().startsWith(QLatin1String("label "))) taken << it.key().mid(6).toLower();
            QHash<int, QString> railName;
            QList<int> ordered(rails.cbegin(), rails.cend());
            std::sort(ordered.begin(), ordered.end());
            for (int net : std::as_const(ordered)) {
                QStringList own;
                for (auto it = before.netOf.cbegin(); it != before.netOf.cend(); ++it)
                    if (it.value() == net && it.key().startsWith(QLatin1String("label "))) own << it.key().mid(6);
                own.sort();
                if (!own.isEmpty()) {
                    railName.insert(net, own.first());
                    continue;
                }
                QString base = QStringLiteral("RAIL");
                for (int i = 0; i < int(parts.size()); ++i) {
                    if ((parts[i]->Model != QLatin1String("Vdc") && parts[i]->Model != QLatin1String("Idc")) || pinNet[i].size() != 2
                        || parts[i]->Props.isEmpty())
                        continue;
                    const int plus = pinNet[i][0], minus = pinNet[i][1];
                    if (!((plus == net && minus == groundNet) || (minus == net && plus == groundNet))) continue;
                    const qucs_s::units::Reading r = qucs_s::units::read(parts[i]->Props.first()->Value);
                    const double v = r.kind == qucs_s::units::Reading::Number ? r.value : 1.0;
                    base = (plus == net) == (v >= 0) ? QStringLiteral("VCC") : QStringLiteral("VEE");
                    break;
                }
                QString name = base;
                for (int n = 2; taken.contains(name.toLower()); ++n) name = base + QString::number(n);
                taken << name.toLower();
                railName.insert(net, name);
            }
            for (const QString& n : std::as_const(railName)) railNames << n;
            for (auto it = pieces.begin(); it != pieces.end(); ++it) {
                QList<QPair<int, int>> kept;
                for (const auto& [i, k] : std::as_const(it->pins)) {
                    const int net = pinNet[i][k];
                    if (net >= 0 && net == groundNet) ownPieces.append(Piece{{{i, k}}, {}, true});
                    else if (railName.contains(net)) ownPieces.append(Piece{{{i, k}}, {railName.value(net)}, false});
                    else kept << QPair<int, int>{i, k};
                }
                it->pins = kept;
                if (kept.isEmpty()) {
                    it->labels.clear();
                    it->grounded = false;
                }
            }
        }
        std::vector<QSet<int>> next(parts.size());
        for (auto it = partsOfNet.cbegin(); it != partsOfNet.cend(); ++it) {
            if (railish(it.key())) continue;
            for (int a : it.value())
                for (int b : it.value())
                    if (a != b) next[a].insert(b);
        }
        // A source on rails and ground alone is a supply: a column of its own
        // at the left.
        std::vector<bool> supply(parts.size(), false);
        for (int i = 0; i < int(parts.size()); ++i)
            if (isSignalSource(parts[i]))
                supply[i] = std::all_of(pinNet[i].cbegin(), pinNet[i].cend(), railish);
        // (Two on one centre - a broken or imported schematic - by their
        // order: onward, below, comes from a set.)
        const auto byPlace = [&parts](int a, int b) {
            return std::tuple(parts[a]->cx, parts[a]->cy, a) < std::tuple(parts[b]->cx, parts[b]->cy, b);
        };
        // Groups of parts joined so, each laid out on its own, one below the other.
        std::vector<int> groupOf(parts.size(), -1);
        std::vector<std::vector<int>> groups;
        for (int i = 0; i < int(parts.size()); ++i) {
            if (groupOf[i] >= 0 || supply[i]) continue;
            std::vector<int> members{i};
            groupOf[i] = int(groups.size());
            for (std::size_t m = 0; m < members.size(); ++m)
                for (int j : next[members[m]])
                    if (groupOf[j] < 0 && !supply[j]) {
                        groupOf[j] = groupOf[i];
                        members.push_back(j);
                    }
            // (In order: found through sets, whose order is other in every
            // run - and so was the layout.)
            std::sort(members.begin(), members.end());
            groups.push_back(members);
        }
        // Where a group's flow starts: its sources; else what a supply feeds;
        // else its ports; else its part furthest left.
        const auto sourcesIn = [&](const std::vector<int>& g) {
            std::vector<int> s;
            for (int i : g)
                if (isSignalSource(parts[i])) s.push_back(i);
            if (s.empty())
                for (int i : g)
                    if (std::any_of(pinNet[i].cbegin(), pinNet[i].cend(), [&](int net) { return rails.contains(net); })) s.push_back(i);
            if (s.empty())
                for (int i : g)
                    if (parts[i]->Model == QLatin1String("Port")) s.push_back(i);
            if (s.empty()) s.push_back(*std::min_element(g.begin(), g.end(), byPlace));
            std::sort(s.begin(), s.end(), [&parts](int a, int b) { return std::tuple(parts[a]->cy, parts[a]->cx, a) < std::tuple(parts[b]->cy, parts[b]->cx, b); });
            return s;
        };
        // (Those with a source first, the larger first.)
        std::stable_sort(groups.begin(), groups.end(), [&](const std::vector<int>& a, const std::vector<int>& b) {
            const bool sa = std::any_of(a.begin(), a.end(), [&](int i) { return isSignalSource(parts[i]); });
            const bool sb = std::any_of(b.begin(), b.end(), [&](int i) { return isSignalSource(parts[i]); });
            return sa != sb ? sa : a.size() > b.size();
        });
        std::vector<int> layer(parts.size(), -1);
        std::vector<std::vector<std::vector<int>>> columnsOf;   // each group's columns
        for (const std::vector<int>& g : groups) {
            std::vector<int> queue = sourcesIn(g);
            for (int s : queue) layer[s] = 0;
            for (std::size_t q = 0; q < queue.size(); ++q) {
                std::vector<int> onward(next[queue[q]].begin(), next[queue[q]].end());
                std::sort(onward.begin(), onward.end(), byPlace);
                for (int j : onward)
                    if (layer[j] < 0) {
                        layer[j] = layer[queue[q]] + 1;
                        queue.push_back(j);
                    }
            }
            int layers = 0;
            for (int i : g) layers = std::max(layers, layer[i] + 1);
            std::vector<std::vector<int>> columns(layers);
            for (int i : queue) columns[layer[i]].push_back(i);
            columnsOf.push_back(columns);
        }
        // Feedback parts, with 'feedback' below or above: a two-pin part on
        // two nets of one part of three pins or more (Rf from an op-amp's
        // output to its inverting input) goes in that part's column, below
        // or above it, lying as its pins run - not a column to its right.
        std::vector<int> hostOf(parts.size(), -1);
        if (feedback != QLatin1String("inline") && !keepPlaces) {
            for (int i = 0; i < int(parts.size()); ++i) {
                if (parts[i]->Ports.size() != 2 || supply[i] || isSignalSource(parts[i]) || groupOf[i] < 0) continue;
                const int a = pinNet[i][0], b = pinNet[i][1];
                if (a == b || railish(a) || railish(b)) continue;
                for (int j : partsOfNet.value(a))
                    if (j != i && parts[j]->Ports.size() >= 3 && partsOfNet.value(b).contains(j) && groupOf[j] == groupOf[i]
                        && (hostOf[i] < 0 || j < hostOf[i]))
                        hostOf[i] = j;
            }
            for (int i = 0; i < int(parts.size()); ++i) {
                if (hostOf[i] < 0) continue;
                for (std::vector<int>& column : columnsOf[groupOf[i]]) std::erase(column, i);
                layer[i] = layer[hostOf[i]];
            }
        }
        // Which of the part's pins on a feedback part's two nets is its
        // output: the one named so (OUT), else the one on a net named so (a
        // label out), else the one standing out furthest (an op-amp's, at
        // its tip), the right one of two as far. (By how far alone, a
        // subcircuit's box with every pin at 110 took its inverting input
        // for the output, and the load for Rg.)
        std::vector<int> outPinOf(parts.size(), -1);    // a feedback part's host's output pin
        {
            static const QRegularExpression inName(QStringLiteral("^(v?in\\d*|input)$"), QRegularExpression::CaseInsensitiveOption);
            const auto roleOf = [](const QString& role) {
                return role == QLatin1String("output") ? 1 : role == QLatin1String("input") ? -1 : 0;
            };
            QHash<int, int> netRole;
            for (auto it = before.netOf.cbegin(); it != before.netOf.cend(); ++it)
                if (it.key().startsWith(QLatin1String("label "))) {
                    const QString name = it.key().mid(6);
                    const int role = inName.match(name).hasMatch() ? -1 : roleOf(qucs_s::erc::pinRole(name));
                    if (role != 0) netRole.insert(it.value(), role);
                }
            for (int f = 0; f < int(parts.size()); ++f) {
                if (hostOf[f] < 0) continue;
                const int h = hostOf[f];
                std::tuple<int, int, int, int> best;
                for (int m = 0; m < parts[h]->Ports.size(); ++m) {
                    if (pinNet[h][m] != pinNet[f][0] && pinNet[h][m] != pinNet[f][1]) continue;
                    const Port* p = parts[h]->Ports.at(m);
                    const std::tuple<int, int, int, int> key{roleOf(qucs_s::erc::pinRole(p->Name)), netRole.value(pinNet[h][m]), std::abs(p->x), p->x};
                    if (outPinOf[f] < 0 || key > best) {
                        best = key;
                        outPinOf[f] = m;
                    }
                }
            }
        }
        // And with it, a two-pin part from its end on the input's net (the
        // net of the part's pin it joins that is not its output: an
        // op-amp's inverting input) to ground - Rg of a non-inverting
        // amplifier: the feedback network, not a column of the load's (its
        // wire ran the width of the schematic).
        std::vector<int> shuntOf(parts.size(), -1);     // the feedback part it goes with
        std::vector<int> shuntFor(parts.size(), -1);    // a feedback part's
        std::vector<int> inputNetOf(parts.size(), -1);  // a feedback part's net toward the input
        for (int f = 0; f < int(parts.size()); ++f) {
            if (hostOf[f] < 0) continue;
            const int h = hostOf[f];
            const int outNet = pinNet[h][outPinOf[f]];
            inputNetOf[f] = pinNet[f][0] == outNet ? pinNet[f][1] : pinNet[f][0];
            for (int i = 0; i < int(parts.size()) && shuntFor[f] < 0; ++i) {
                if (i == f || parts[i]->Ports.size() != 2 || hostOf[i] >= 0 || shuntOf[i] >= 0 || supply[i] || isSignalSource(parts[i])
                    || groupOf[i] != groupOf[f])
                    continue;
                const int a = pinNet[i][0], b = pinNet[i][1];
                if ((a == inputNetOf[f] && b == groundNet) || (b == inputNetOf[f] && a == groundNet)) {
                    shuntOf[i] = f;
                    shuntFor[f] = i;
                    for (std::vector<int>& column : columnsOf[groupOf[i]]) std::erase(column, i);
                    layer[i] = layer[h];
                }
            }
        }
        QRect was0;
        for (Component* c : parts)
            was0 = was0.isNull() ? c->boundingRectIncludingProperties() : was0.united(c->boundingRectIncludingProperties());

        // Taken up: the wires and ground symbols, the labels on pins (all
        // put back below); the parts lifted off.
        std::vector<Wire*> wires(sch->a_DocWires.begin(), sch->a_DocWires.end());
        sch->deleteWires(wires);
        sch->deleteComps(grounds);
        for (Node* n : sch->a_DocNodes)
            if (n->hasLabel()) n->dropLabel();
        // (All at once: one at a time, each looked through every node.)
        std::vector<Component*> lifted(parts.begin(), parts.end());
        lifted.insert(lifted.end(), blocks.begin(), blocks.end());
        sch->detachComps(lifted);
        for (Component* c : parts)
            for (Port* p : c->Ports) p->Connection = nullptr;

        // Two-pin parts turned as a schematic has them: in series lying
        // down, the pin toward what drives it on the left; to ground or a
        // supply standing, ground below and a supply above. (The circuit is
        // the same: only where the pins are.)
        for (int i = 0; i < int(parts.size()) && turn; ++i) {
            Component* c = parts[i];
            if (c->Ports.size() != 2) continue;
            const int a = pinNet[i][0], b = pinNet[i][1];
            std::function<bool()> placed;
            if (railish(a) || railish(b)) {
                const int below = a == groundNet ? 0 : b == groundNet ? 1 : railish(a) && !railish(b) ? 1 : railish(b) && !railish(a) ? 0 : 1;
                placed = [c, below] {
                    const Port *p = c->Ports.at(below), *q = c->Ports.at(1 - below);
                    return p->x == q->x && p->y > q->y;
                };
            } else {
                int left = 0;
                for (int k = 1; k >= 0; --k)
                    for (int j : partsOfNet.value(pinNet[i][k]))
                        if (j != i && layer[j] >= 0 && layer[j] < layer[i]) left = k;
                // (A feedback part: its pin toward the part's input on the
                // left, unless the output is left of the input.)
                if (const int h = hostOf[i]; h >= 0) {
                    const int kIn = pinNet[i][0] == inputNetOf[i] ? 0 : 1;
                    const int xOut = parts[h]->Ports.at(outPinOf[i])->x;
                    int xIn = INT_MAX;
                    for (int m = 0; m < parts[h]->Ports.size(); ++m)
                        if (pinNet[h][m] == inputNetOf[i]) xIn = std::min(xIn, parts[h]->Ports.at(m)->x);
                    left = xIn <= xOut ? kIn : 1 - kIn;
                }
                placed = [c, left] {
                    const Port *p = c->Ports.at(left), *q = c->Ports.at(1 - left);
                    return p->y == q->y && p->x < q->x;
                };
            }
            for (int r = 0; r < 4 && !placed(); ++r) c->rotate();   // (pins not in a line: four turns bring it back)
        }

        // Each one's symbol and texts from its centre, as it is turned now.
        std::vector<QRect> rel(parts.size());
        for (int i = 0; i < int(parts.size()); ++i) rel[i] = parts[i]->boundingRectIncludingProperties().translated(-parts[i]->center());
        const int gapX = 2 * spacing, gapY = spacing;
        std::vector<QPoint> target(parts.size());
        QJsonArray columnsSaid;
        int top = was0.top();
        int bottom = top;
        // The supplies, stacked at the left; the groups to their right.
        int x0 = was0.left();
        {
            int half = 0, y = top;
            for (int i = 0; i < int(parts.size()); ++i)
                if (supply[i]) half = std::max({half, -rel[i].left(), rel[i].right()});
            for (int i = 0; i < int(parts.size()); ++i) {
                if (!supply[i]) continue;
                int cx = x0 + half, cy = y - rel[i].top();
                sch->setOnGrid(cx, cy);
                target[i] = QPoint(cx, cy);
                y = cy + rel[i].bottom() + gapY;
                bottom = std::max(bottom, cy + rel[i].bottom());
            }
            if (half > 0) x0 += 2 * half + gapX;
        }
        for (std::size_t gi = 0; gi < groups.size(); ++gi) {
            const std::vector<int>& g = groups[gi];
            const std::vector<std::vector<int>>& columns = columnsOf[gi];
            const int layers = int(columns.size());
            // Each column as wide as its widest part, both sides of its centre.
            std::vector<int> centreX(layers);
            int x = x0;
            for (int l = 0; l < layers; ++l) {
                int half = 0;
                for (int i : columns[l]) half = std::max({half, -rel[i].left(), rel[i].right()});
                centreX[l] = x + half;
                x += 2 * half + gapX;
            }
            // Down each column, each part at the height that puts its pin
            // level with the pin it joins on each part to its left (a chain
            // runs straight) when there is room, else below the last.
            int groupBottom = top;
            for (int l = 0; l < layers; ++l) {
                std::vector<std::pair<int, int>> wanted;   // part, preferred centre y
                for (int i : columns[l]) {
                    long sum = 0;
                    int n = 0;
                    for (int j : next[i]) {
                        if (layer[j] < 0 || layer[j] >= l || groupOf[j] != groupOf[i] || hostOf[j] >= 0 || shuntOf[j] >= 0) continue;
                        for (int k = 0; k < pinNet[i].size(); ++k)
                            for (int m = 0; m < pinNet[j].size(); ++m)
                                if (pinNet[i][k] >= 0 && pinNet[i][k] == pinNet[j][m] && !railish(pinNet[i][k])) {
                                    sum += target[j].y() + parts[j]->Ports.at(m)->y - parts[i]->Ports.at(k)->y;
                                    ++n;
                                }
                    }
                    wanted.push_back({i, n > 0 ? int(sum / n) : (l == 0 ? parts[i]->cy : top)});
                }
                if (l == 0) std::stable_sort(wanted.begin(), wanted.end(), [&parts](auto a, auto b) { return parts[a.first]->cy < parts[b.first]->cy; });
                else std::stable_sort(wanted.begin(), wanted.end(), [](auto a, auto b) { return a.second < b.second; });
                // (A column right of the first may start above the group's
                // top, to line a pin up: the group is moved down after.)
                int lastBottom = l == 0 ? top - gapY : std::numeric_limits<int>::min() / 4;
                QJsonArray said;
                for (const auto& [i, preferred] : wanted) {
                    int cy = l == 0 ? lastBottom + gapY - rel[i].top() : std::max(preferred, lastBottom + gapY - rel[i].top());
                    int cx = centreX[l];
                    sch->setOnGrid(cx, cy);
                    if (cy + rel[i].top() < lastBottom + gapY) cy += gy * ((lastBottom + gapY - cy - rel[i].top() + gy - 1) / gy);
                    target[i] = QPoint(cx, cy);
                    lastBottom = cy + rel[i].bottom();
                    groupBottom = std::max(groupBottom, lastBottom);
                    said.append(parts[i]->Name);
                }
                // Each part's feedback parts below (or above) it, centred on
                // it, the others of the column moved on to make room.
                for (const auto& [host, preferred] : wanted) {
                    std::vector<int> round;
                    for (int f = 0; f < int(parts.size()); ++f)
                        if (hostOf[f] == host) round.push_back(f);
                    if (round.empty()) continue;
                    const bool below = feedback == QLatin1String("below");
                    int y = below ? target[host].y() + rel[host].bottom() + gapY : target[host].y() + rel[host].top() - gapY;
                    const int start = y;
                    for (int f : round) {
                        // Its pin under (or over) the part's output (Rf's
                        // under an op-amp's output): that wire goes straight
                        // down, and the other comes down at its pin's side,
                        // clear of the part's symbol - not along its tip.
                        const int m = outPinOf[f];
                        const int k = pinNet[f][0] == pinNet[host][m] ? 0 : 1;
                        int cx = target[host].x() + parts[host]->Ports.at(m)->x - parts[f]->Ports.at(k)->x;
                        int cy = below ? y - rel[f].top() : y - rel[f].bottom();
                        sch->setOnGrid(cx, cy);
                        target[f] = QPoint(cx, cy);
                        y = below ? cy + rel[f].bottom() + gapY : cy + rel[f].top() - gapY;
                        said.append(parts[f]->Name);
                    }
                    const int room = gy * ((std::abs(y - start) + gy - 1) / gy);
                    for (int i : columns[l])
                        if (i != host && (below ? target[i].y() > target[host].y() : target[i].y() < target[host].y()))
                            target[i] += QPoint(0, below ? room : -room);
                    // Their parts to ground (Rg), standing, ground below: under
                    // the feedback part's end on the input's net (below), or
                    // under the part's input pin (above) - the others of the
                    // column below moved on to make room.
                    int under = below ? target[host].y() + rel[host].bottom() + room + gapY : target[host].y() + rel[host].bottom() + gapY;
                    const int underStart = under;
                    for (int f : round) {
                        const int g = shuntFor[f];
                        if (g < 0) continue;
                        const int net = inputNetOf[f];
                        const int kg = pinNet[g][0] == net ? 0 : 1;
                        int x = target[f].x();
                        if (below) {
                            for (int k = 0; k < parts[f]->Ports.size(); ++k)
                                if (pinNet[f][k] == net) x = target[f].x() + parts[f]->Ports.at(k)->x;
                        } else {
                            for (int m = 0; m < parts[host]->Ports.size(); ++m)
                                if (pinNet[host][m] == net) x = target[host].x() + parts[host]->Ports.at(m)->x;
                        }
                        // (Below the feedback part: just under its texts.)
                        const int topAt = below ? target[f].y() + rel[f].bottom() + gapY / 2 : under;
                        int cx = x - parts[g]->Ports.at(kg)->x, cy = topAt - rel[g].top();
                        sch->setOnGrid(cx, cy);
                        target[g] = QPoint(cx, cy);
                        under = std::max(under, cy + rel[g].bottom() + gapY);
                        said.append(parts[g]->Name);
                    }
                    if (under > underStart) {
                        // (The ground symbol goes under it too: room for it.)
                        const int more = gy * ((under - underStart + 3 * gy + gy - 1) / gy);
                        for (int i : columns[l])
                            if (i != host && target[i].y() > target[host].y()) target[i] += QPoint(0, more);
                        for (int f : round)
                            if (shuntFor[f] >= 0) groupBottom = std::max(groupBottom, target[shuntFor[f]].y() + rel[shuntFor[f]].bottom() + 3 * gy);
                    }
                    for (int f : round) groupBottom = std::max(groupBottom, target[f].y() + rel[f].bottom());
                    for (int i : columns[l]) groupBottom = std::max(groupBottom, target[i].y() + rel[i].bottom());
                }
                if (groups.size() == 1) columnsSaid.append(said);
            }
            int highest = top;
            for (int i : g) highest = std::min(highest, target[i].y() + rel[i].top());
            if (highest < top) {
                const int down = gy * ((top - highest + gy - 1) / gy);
                for (int i : g) target[i] += QPoint(0, down);
                groupBottom += down;
            }
            top = groupBottom + 2 * gapY;
            bottom = std::max(bottom, groupBottom);
        }

        // Put down where they go, the blocks in a row below. (Where they
        // were, with keep_places: only the wiring is drawn again.)
        if (keepPlaces) target = home;
        // 'straighten': a part nudged by up to four grid steps so the two
        // pins of a piece of wiring between two parts line up, and its wire
        // runs straight - a jog of 10 across the page is a hand pass saved.
        // Greedy: the move that lines up the most such pairs, the part's
        // symbol and texts clear of every other's, until none lines up more.
        QStringList nudged;
        if (straighten && parts.size() <= 2000) {
            const int gx = std::max(sch->getGridX(), 1);
            struct Pair {
                int a, ka, b, kb;
            };
            std::vector<Pair> pairs;
            for (const Piece& piece : std::as_const(pieces))
                if (piece.pins.size() == 2 && piece.pins.at(0).first != piece.pins.at(1).first)
                    pairs.push_back({piece.pins.at(0).first, piece.pins.at(0).second, piece.pins.at(1).first, piece.pins.at(1).second});
            // (The pieces come from a hash: in order, the same moves every run.)
            std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) {
                return std::tie(x.a, x.ka, x.b, x.kb) < std::tie(y.a, y.ka, y.b, y.kb);
            });
            std::vector<std::vector<int>> pairsOf(parts.size());
            for (int p = 0; p < int(pairs.size()); ++p) {
                pairsOf[pairs[p].a].push_back(p);
                pairsOf[pairs[p].b].push_back(p);
            }
            const auto pinAt = [&](int i, int k) { return target[i] + QPoint(parts[i]->Ports.at(k)->x, parts[i]->Ports.at(k)->y); };
            // Which way each pin leaves its symbol: as its stub goes (the
            // line of the symbol that ends at it, from its other end), else
            // toward the nearest side of the symbol's box. (By the box alone
            // a coupled line's pins, at its corners, faced sideways - their
            // stubs go up and down.)
            const auto outOf = [&](int i, int k) {
                const Port* pin = parts[i]->Ports.at(k);
                const auto sign = [](double v) { return (v > 0) - (v < 0); };
                for (const qucs::Line* l : std::as_const(parts[i]->Lines)) {
                    const bool first = l->x1 == pin->x && l->y1 == pin->y, second = l->x2 == pin->x && l->y2 == pin->y;
                    if (first == second) continue;
                    const double dx = pin->x - (first ? l->x2 : l->x1), dy = pin->y - (first ? l->y2 : l->y1);
                    if ((dx == 0) != (dy == 0)) return QPoint(sign(dx), sign(dy));
                }
                return outwardOf(parts[i]->boundingRect().translated(-parts[i]->center()), QPoint(pin->x, pin->y));
            };
            // Whether a straight wire can join the two along a row (rows)
            // or a column: each pin leaves its symbol toward the other. (By
            // their places alone, VCC's + and an op-amp's VCC pin, both
            // facing up, were lined up by moving VCC down - the wire went
            // over the top all the same, and VCC's ground was pushed aside
            // onto a jog the VEE wire crossed.)
            const auto facing = [&](const Pair& p, bool rows) {
                const QPoint a = pinAt(p.a, p.ka), b = pinAt(p.b, p.kb);
                const auto sign = [](int v) { return (v > 0) - (v < 0); };
                if (rows) return a.x() != b.x() && outOf(p.a, p.ka) == QPoint(sign(b.x() - a.x()), 0) && outOf(p.b, p.kb) == QPoint(sign(a.x() - b.x()), 0);
                return a.y() != b.y() && outOf(p.a, p.ka) == QPoint(0, sign(b.y() - a.y())) && outOf(p.b, p.kb) == QPoint(0, sign(a.y() - b.y()));
            };
            const auto aligned = [&](const Pair& p) {
                const QPoint a = pinAt(p.a, p.ka), b = pinAt(p.b, p.kb);
                return a == b || (a.y() == b.y() && facing(p, true)) || (a.x() == b.x() && facing(p, false));
            };
            const auto alignedOf = [&](int i) {
                int n = 0;
                for (int p : pairsOf[i]) n += aligned(pairs[p]) ? 1 : 0;
                return n;
            };
            const auto clear = [&](int i, QPoint at) {
                const QRect r = rel[i].translated(at);
                for (int j = 0; j < int(parts.size()); ++j)
                    if (j != i && r.intersects(rel[j].translated(target[j]))) return false;
                return true;
            };
            const int most = 4 * std::max(gx, gy);
            const std::vector<QPoint> from = target;
            for (int round = 0; round < int(pairs.size()); ++round) {
                int gain = 0, best = -1;
                QPoint bestTo;
                for (const Pair& p : pairs) {
                    if (aligned(p)) continue;
                    const QPoint d = pinAt(p.b, p.kb) - pinAt(p.a, p.ka);
                    // Pins facing each other across: the rows lined up; up
                    // and down: the columns; else no move straightens it.
                    const bool rows = facing(p, true), columns = facing(p, false);
                    if (!rows && !columns) continue;
                    const QPoint shift = rows ? QPoint(0, d.y()) : QPoint(d.x(), 0);
                    if (std::abs(shift.x()) > most || std::abs(shift.y()) > most) continue;
                    for (const auto& [part, sign] : {std::pair{p.a, 1}, std::pair{p.b, -1}}) {
                        const QPoint to = target[part] + shift * sign;
                        if (!clear(part, to)) continue;
                        const int before = alignedOf(part);
                        const QPoint was = target[part];
                        target[part] = to;
                        const int after = alignedOf(part);
                        target[part] = was;
                        if (after - before > gain) {
                            gain = after - before;
                            best = part;
                            bestTo = to;
                        }
                    }
                }
                if (best < 0) break;
                target[best] = bestTo;
            }
            for (int i = 0; i < int(parts.size()); ++i) {
                const QPoint d = target[i] - from[i];
                if (d.isNull()) continue;
                QStringList way;
                if (d.x() != 0) way << tr("%1 %2").arg(std::abs(d.x())).arg(d.x() > 0 ? tr("right") : tr("left"));
                if (d.y() != 0) way << tr("%1 %2").arg(std::abs(d.y())).arg(d.y() > 0 ? tr("down") : tr("up"));
                nudged << QStringLiteral("%1 %2").arg(parts[i]->Name, way.join(QLatin1Char(' ')));
            }
        }
        for (int i = 0; i < int(parts.size()); ++i) parts[i]->moveCenter(target[i].x() - parts[i]->cx, target[i].y() - parts[i]->cy);
        int bx = was0.left();
        for (Component* c : blocks) {
            if (keepPlaces) break;
            const QRect r = c->boundingRectIncludingProperties().translated(-c->center());
            int cx = bx - r.left(), cy = bottom + 2 * gapY - r.top();
            sch->setOnGrid(cx, cy);
            c->moveCenter(cx - c->cx, cy - c->cy);
            bx += r.width() + spacing;
        }
        {
            // (Their nodes found by place, as the loader finds them.)
            const Schematic::IndexedInsertion indexed(sch);
            for (Component* c : parts) sch->insertRawComponent(c, false);
            for (Component* c : blocks) sch->insertRawComponent(c, false);
        }

        // Where each piece's labels and ground go: its first pin from the
        // left, its lowest pin. (Places from here on: a way tried and taken
        // back makes the elements anew.)
        struct Put {
            QPoint label, ground;
            QStringList labels;
            bool grounded;
        };
        QList<Put> puts;
        QList<Piece> allPieces = pieces.values();
        allPieces << ownPieces;
        for (const Piece& piece : std::as_const(allPieces)) {
            if (piece.pins.isEmpty() || (piece.labels.isEmpty() && !piece.grounded)) continue;
            QList<QPoint> at;
            for (const auto& [i, k] : piece.pins) at << parts[i]->center() + QPoint(parts[i]->Ports.at(k)->x, parts[i]->Ports.at(k)->y);
            const QPoint left = *std::min_element(at.begin(), at.end(), [](QPoint a, QPoint b) { return std::pair(a.x(), a.y()) < std::pair(b.x(), b.y()); });
            const QPoint low = *std::max_element(at.begin(), at.end(), [](QPoint a, QPoint b) { return std::pair(a.y(), -a.x()) < std::pair(b.y(), -b.x()); });
            puts.append({left, low, piece.labels, piece.grounded});
        }
        // (In order of place: the pieces come from a hash, whose order is
        // other in every run - and the first put takes the room.)
        std::sort(puts.begin(), puts.end(), [](const Put& a, const Put& b) {
            return std::tuple(a.label.x(), a.label.y(), a.ground.x(), a.ground.y()) < std::tuple(b.label.x(), b.label.y(), b.ground.x(), b.ground.y());
        });
        // Wires for labels: a name on its first piece alone - the others,
        // no longer joined by it, are wired to it below like any net in
        // pieces.
        if (wireLabels) {
            QSet<QString> named;
            for (Put& put : puts) {
                QStringList kept;
                for (const QString& name : std::as_const(put.labels))
                    if (railNames.contains(name)) {   // (a supply's, on each of its pins)
                        kept << name;
                    } else if (!named.contains(name)) {
                        named.insert(name);
                        kept << name;
                    }
                put.labels = kept;
            }
        }
        const int arrangedCount = int(parts.size());
        parts.clear();
        blocks.clear();
        int labelsPut = 0, groundsPut = 0;
        // (Their nodes found by place; what is in the way looked up by
        // place too - each of them against every part was quadratic.)
        std::optional<Schematic::IndexedInsertion> indexed(std::in_place, sch);
        RectCells taken;   // what a label's text must keep clear of: the parts and their texts, other labels
        for (const Component* c : sch->a_DocComps) taken.add(c->boundingRectIncludingProperties().adjusted(-2, -2, 2, 2));
        for (const Put& put : std::as_const(puts))
            for (const QString& name : put.labels) {
                Node* n = sch->findNode(put.label);
                if (n == nullptr || n->hasLabel()) continue;
                // Its text above right of the pin, else below right, above or below left.
                const int w = 8 * int(name.size()) + 10, h = 16;
                QPoint text = put.label + QPoint(30, -30);
                for (const QPoint& d : {QPoint(30, -30), QPoint(30, 30), QPoint(-30 - w, -30), QPoint(-30 - w, 30), QPoint(10, -50)}) {
                    const QRect box(put.label + d - QPoint(0, h), QSize(w, h));
                    if (!taken.meets(box)) {
                        text = put.label + d;
                        break;
                    }
                }
                int xl = text.x(), yl = text.y();
                sch->setOnGrid(xl, yl);
                taken.add(QRect(QPoint(xl, yl - h), QSize(w, h)));
                n->setName(name, QString(), xl, yl);
                ++labelsPut;
            }
        GroundRoom room(sch);
        for (const Put& put : std::as_const(puts)) {
            if (!put.grounded) continue;
            QPoint gp;
            if (!placeGround(sch, put.ground, &gp, &room)) {
                // (No room: on the pin all the same - it joins.)
                if (Component* g = newComponent(QStringLiteral("GND"))) {
                    g->setSchematic(sch);
                    g->recreate();
                    g->moveCenter(put.ground.x() - g->cx - g->Ports.first()->x, put.ground.y() - g->cy - g->Ports.first()->y);
                    sch->insertComponent(g);
                    room.addPart(g);
                }
            }
            ++groundsPut;
        }
        indexed.reset();

        // Every net joined again by wires that join nothing else, and
        // checked against what it was.
        int pinCount = 0;
        for (const Component* c : sch->a_DocComps) pinCount += int(c->Ports.size());
        const auto check = [&] { return netChanges(was, withoutGroundSymbols(sch, netsOf(sch, nullptr), dropped), QString(), true, nullptr); };
        QString why;
        const QStringList early = check();   // (a label or ground put back that joins what it should not)
        bool done = early.isEmpty();
        if (!done) why = early.join(QStringLiteral("; "));
        else done = joinPieces(sch, was, QString(), {}, check, &why, 4 * pinCount + 64);
        const QStringList changed = done ? netChanges(was, withoutGroundSymbols(sch, netsOf(sch, nullptr), dropped), QString(), false, nullptr)
                                         : QStringList();
        if (!done || !changed.isEmpty()) {
            faults << tr("with %1 of room%2: %3").arg(spacing).arg(turn ? QString() : tr(", the parts turned as they were"))
                          .arg(!changed.isEmpty() ? changed.join(QStringLiteral("; ")) : why);
            sch->restore(state);
            continue;
        }
        relabelOnWires(sch, railNames);
        if (!netChanges(was, withoutGroundSymbols(sch, netsOf(sch, nullptr), dropped), QString(), false, nullptr).isEmpty()) {
            // (It cannot join or part a net: its name stays on it. Were it
            // to, the labels go back where they were.)
            faults << tr("with %1 of room: the labels moved on to the wires changed a net").arg(spacing);
            sch->restore(state);
            continue;
        }

        // What it now covers, and the diagrams and paintings that were
        // there moved aside to its right.
        QRect covers;
        for (const Component* c : sch->a_DocComps) covers = covers.isNull() ? c->boundingRectIncludingProperties() : covers.united(c->boundingRectIncludingProperties());
        for (const Wire* w : sch->a_DocWires) covers = covers.united(QRect(w->P1(), w->P2()).normalized());
        QStringList aside;
        int dn = 0;
        for (Diagram* d : sch->a_DocDiags) {
            ++dn;
            if (keepPlaces || !d->boundingRect().intersects(covers)) continue;
            d->moveCenter(covers.right() + 2 * spacing - d->boundingRect().left(), 0);
            aside << tr("diagram %1").arg(dn);
        }
        int pn = 0;
        for (Painting* p : sch->a_DocPaints) {
            ++pn;
            if (keepPlaces || !p->boundingRect().intersects(covers)) continue;
            p->moveCenter(covers.right() + 2 * spacing - p->boundingRect().left(), 0);
            aside << tr("painting %1").arg(pn);
        }
        finish(sch, {covers.center()});
        QJsonObject result{{QStringLiteral("document"), titleOf(sch)},
                           {QStringLiteral("arranged"), keepPlaces ? tr("%1 parts with pins where they were, %2 wires drawn again, "
                                                                        "every net as it was").arg(arrangedCount).arg(sch->a_DocWires.size())
                                                                   : tr("%1 parts with pins in columns by signal flow, %2 wires drawn again, "
                                                           "every net as it was").arg(arrangedCount).arg(sch->a_DocWires.size())},
                           {QStringLiteral("grounds"), groundsPut},
                           {QStringLiteral("labels"), labelsPut},
                           {QStringLiteral("spacing"), spacing},
                           {QStringLiteral("one step to undo"), true}};
        if (!columnsSaid.isEmpty()) result.insert(QStringLiteral("columns"), columnsSaid);
        if (straighten)
            result.insert(QStringLiteral("straightened"),
                          nudged.isEmpty() ? tr("nothing to nudge: no part could line up a wire's pins by moving 4 grid steps or less, "
                                                "clear of the others")
                                           : tr("%1 parts nudged so their wires run straight: %2").arg(nudged.size()).arg(nudged.join(QStringLiteral(", "))));
        if (!aside.isEmpty()) result.insert(QStringLiteral("moved aside"), QJsonArray::fromStringList(aside));
        if (!dropped.isEmpty())
            result.insert(QStringLiteral("dropped"), tr("net labels on wires that reached no pin: %1").arg(dropped.join(QStringLiteral(", "))));
        if (!keepPlaces && (spacing != asked || !turn))
            result.insert(QStringLiteral("note"), tr("Every wire could not be drawn with %1 of room and the parts turned as a schematic has them: %2 was "
                                                     "used%3.").arg(asked).arg(spacing).arg(turn ? QString() : tr(", the parts turned as they were")));
        return jsonResult(result);
    }
    return errorResult(tr("Not arranged - %1 is as it was: not every net could be wired again (%2). Tidy it by hand with move "
                          "and connect, or arrange the parts in fewer at a time.")
                           .arg(titleOf(sch), faults.join(QStringLiteral("; "))));
}

QJsonObject QucsControl::connectPins(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    // "ground" at one end: a ground symbol of the pin's own.
    const auto isGround = [](const QJsonValue& v) {
        const QString s = v.toString().trimmed().toLower();
        return v.isString() && (s == QLatin1String("ground") || s == QLatin1String("gnd"));
    };
    const QJsonValue fromArg = args.value(QLatin1String("from")), toArg = args.value(QLatin1String("to"));
    if (isGround(fromArg) && isGround(toArg)) return errorResult(tr("Both ends are ground: one of them is a pin."));
    if (isGround(fromArg) || isGround(toArg)) return groundPin(sch, isGround(fromArg) ? toArg : fromArg);
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
    // The way: through the points given; else round a side first, when one
    // is asked for; else the shortest that touches nothing.
    const QString side = args.value(QLatin1String("side")).toString().trimmed().toLower();
    if (!side.isEmpty() && side != QLatin1String("above") && side != QLatin1String("below") && side != QLatin1String("left")
        && side != QLatin1String("right"))
        return errorResult(tr("'side' is above, below, left or right: the side of the two ends the wire goes round."));
    QList<QPoint> via;
    for (const QJsonValue& v : args.value(QLatin1String("via")).toArray()) {
        const QJsonArray p = v.toArray();
        if (p.size() != 2 || !p.at(0).isDouble() || !p.at(1).isDouble())
            return errorResult(tr("'via' is a list of points the wire goes through, [[x, y], ...]."));
        int x = p.at(0).toInt(), y = p.at(1).toInt();
        sch->setOnGrid(x, y);
        via << QPoint(x, y);
    }
    if (!via.isEmpty() && !side.isEmpty()) return errorResult(tr("Give 'via' or 'side', not both."));
    prepare(sch);
    // A wire that joins these two nets and nothing else: one going over a
    // pin on its way would join that pin's net too.
    const auto check = [&] { return netChangesBeyond(before, netsOf(sch, nullptr, {a, b}), ends); };
    const QList<qucs_s::erc::Issue> wiringBefore = qucs_s::erc::wiring(sch);
    QHash<const Component*, int> acrossBefore;   // (the parts' bodies the wires cross, before it)
    for (Component* c : sch->a_DocComps) acrossBefore.insert(c, wiresAcross(sch, c));
    int preferred = 0, used = -1;
    const std::vector<std::vector<QPoint>> ways = !via.isEmpty() ? std::vector<std::vector<QPoint>>{wayThrough(a, via, b)}
                                                  : !side.isEmpty() ? waysAround(sch, a, b, side, &preferred)
                                                                    : waysBetween(sch, a, b);
    if (!wireAlong(sch, a, b, ways, check, &error, &used, via.isEmpty(), via.isEmpty() && side.isEmpty()))
        return errorResult(!via.isEmpty() ? tr("Not wired through the points given: %1. Other points, or 'side', or connect "
                                               "without them.").arg(error)
                                          : tr("Not wired: %1. Give the way with add_wire, or move a part out of it.").arg(error));
    finish(sch, {a, b});
    QString text = tr("Wired %1 to %2 (%3, %4 to %5, %6), going over no other pin or wire.")
                       .arg(from, to).arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
    if (!via.isEmpty()) {
        QStringList points;
        for (const QPoint& p : std::as_const(via)) points << QStringLiteral("%1, %2").arg(p.x()).arg(p.y());
        text += QLatin1Char(' ') + tr("Through %1.").arg(points.join(QStringLiteral("; ")));
    } else if (!side.isEmpty()) {
        text += QLatin1Char(' ') + (used < preferred ? tr("Round the %1 side.").arg(side == QLatin1String("above") ? tr("upper")
                                                                                     : side == QLatin1String("below") ? tr("lower") : side)
                                                     : tr("Not round the %1 side: no way there went over nothing - the shortest "
                                                          "other way.").arg(side));
    }
    if (const QStringList look = newWiringIssues(sch, wiringBefore); !look.isEmpty())
        text += QLatin1Char(' ') + tr("Look: %1.").arg(look.join(QStringLiteral("; ")));
    // What "over no other pin or wire" leaves unsaid: a part's body the wire
    // crosses, and a pin that faces away from the other end - the wire goes
    // round its own part then.
    QStringList crossed;
    for (Component* c : sch->a_DocComps)
        if (wiresAcross(sch, c) > acrossBefore.value(c, 0)) crossed << c->Name;
    if (!crossed.isEmpty())
        text += QLatin1Char(' ') + tr("It crosses the body of %1 (no pin of it): move it, or give 'side' or 'via'.").arg(crossed.join(QStringLiteral(", ")));
    for (const auto& [here, there, otherName] : {std::tuple{a, b, to}, std::tuple{b, a, from}})
        for (Component* c : sch->a_DocComps)
            for (int i = 0; i < c->Ports.size(); ++i) {
                const Port* p = c->Ports.at(i);
                if (QPoint(c->cx + p->x, c->cy + p->y) != here) continue;
                // The way it faces: out from the part's centre, along the
                // longer of its offsets.
                const QPoint out = std::abs(p->x) >= std::abs(p->y) ? QPoint((p->x > 0) - (p->x < 0), 0) : QPoint(0, (p->y > 0) - (p->y < 0));
                const QPoint d = there - here;
                if (out.isNull() || out.x() * d.x() + out.y() * d.y() >= 0) continue;
                // The rotation that turns it to the other end: a quarter turn
                // moves (x, y) to (y, -x).
                int best = c->rotated, bestDot = INT_MIN;
                QPoint o = out;
                for (int k = 1; k <= 3; ++k) {
                    o = QPoint(o.y(), -o.x());
                    if (const int dot = o.x() * d.x() + o.y() * d.y(); dot > bestDot) {
                        bestDot = dot;
                        best = (c->rotated + k) % 4;
                    }
                }
                static const char* const faces[] = {QT_TRANSLATE_NOOP("QucsControl", "right"), QT_TRANSLATE_NOOP("QucsControl", "down"),
                                                    QT_TRANSLATE_NOOP("QucsControl", "left"), QT_TRANSLATE_NOOP("QucsControl", "up")};
                const int way = out.x() > 0 ? 0 : out.y() > 0 ? 1 : out.x() < 0 ? 2 : 3;
                text += QLatin1Char(' ') + tr("%1's pin %2 faces %3, away from %4: the wire goes round %1. At rotation %5 (now %6) "
                                              "it faces %4.")
                                               .arg(c->Name).arg(i + 1).arg(tr(faces[way]), otherName).arg(best).arg(c->rotated);
            }
    return textResult(text);
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
    // (Places all one: "drawn", and nothing was.)
    if (std::all_of(points.cbegin(), points.cend(), [&points](const QPoint& p) { return p == points.first(); }))
        return errorResult(tr("A wire needs two different places: %1, %2 is all of them.").arg(points.first().x()).arg(points.first().y()));
    // What is at its places is joined; what it goes over between them must
    // not be.
    const Nets before = netsOf(sch, nullptr, points);
    QStringList places;
    for (const QPoint& p : points) places << QStringLiteral("at %1,%2").arg(p.x()).arg(p.y());
    prepare(sch);
    const QString state = sch->snapshot();
    const QList<qucs_s::erc::Issue> wiringBefore = qucs_s::erc::wiring(sch);
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
    QString text = tr("Wire drawn through %1 places (%2 wires now).").arg(points.size()).arg(sch->a_DocWires.size());
    // A wire that crosses another net's is drawn - a crossing is no
    // junction - but said: it may be meant to join it.
    if (const QStringList look = newWiringIssues(sch, wiringBefore); !look.isEmpty())
        text += QLatin1Char(' ') + tr("Look: %1. To join a wire, end this one on it (or add_wire through that place).")
                                       .arg(look.join(QStringLiteral("; ")));
    return textResult(text);
}

QJsonObject QucsControl::setLabel(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    QPoint p;
    if (!pointOf(sch, args.value(QLatin1String("at")), &p, &error)) return errorResult(error);
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    if (const QString bad = name.isEmpty() ? QString() : badNetName(name); !bad.isEmpty()) return errorResult(bad + QLatin1Char('.'));
    Node* node = sch->findNode(p);
    Wire* wire = node == nullptr ? sch->selectedWire(p.x(), p.y()) : nullptr;
    if (node == nullptr && wire == nullptr) return errorResult(tr("There is no pin or wire at %1, %2.").arg(p.x()).arg(p.y()));
    Element* labelled = wire != nullptr ? sch->getWireLabel(wire->Port1) : sch->getWireLabel(node);
    if (labelled != nullptr && (labelled->Type & isComponent))
        return errorResult(tr("The net is ground: it cannot be labelled."));
    const QString before = labelled != nullptr && static_cast<Conductor*>(labelled)->hasLabel()
                               ? static_cast<Conductor*>(labelled)->label()->Name : QString();
    // (Its initial value kept when the name is: moving its text, say.)
    const QString initial = labelled != nullptr && static_cast<Conductor*>(labelled)->hasLabel() && before == name
                                ? static_cast<Conductor*>(labelled)->label()->initValue : QString();
    // Where its text goes: given, else above right of the place.
    int xl = p.x() + 30, yl = p.y() - 30;
    if (args.contains(QLatin1String("text_at"))) {
        const QJsonArray t = args.value(QLatin1String("text_at")).toArray();
        if (t.size() != 2 || !t.at(0).isDouble() || !t.at(1).isDouble())
            return errorResult(tr("'text_at' is where the label's text goes, [x, y] on the schematic."));
        const QPoint within = Schematic::withinModelLimit(QPoint(t.at(0).toInt(), t.at(1).toInt()));
        xl = within.x();
        yl = within.y();
    }
    prepare(sch);
    if (labelled != nullptr) static_cast<Conductor*>(labelled)->dropLabel();
    if (!name.isEmpty()) {
        sch->setOnGrid(xl, yl);
        if (wire != nullptr) wire->setName(name, initial, p.x(), p.y(), xl, yl);
        else node->setName(name, initial, xl, yl);
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

void QucsControl::renameEverywhere(Schematic* sch, const std::function<QString(const QString&)>& rename,
                                   QStringList* changed, QString* rewritten)
{
    const QList<Schematic*> showing = showingDataOf(sch);
    for (Schematic* doc : showing) {
        int n = 0;
        bool any = false;
        for (Diagram* d : doc->a_DocDiags) {
            ++n;
            for (Graph* g : d->Graphs) {
                const QString renamed = rename(g->Var);
                if (renamed == g->Var) continue;
                *changed << tr("%1, diagram %2: %3 is %4").arg(titleOf(doc)).arg(n).arg(g->Var, renamed);
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
            const QString renamed = rename(p->Value);
            if (renamed == p->Value) continue;
            *changed << tr("%1: %2 is %3").arg(c->Name, p->Name + QLatin1Char('=') + p->Value, renamed);
            p->Value = renamed;
        }
    }
    if (sch->getDocName().isEmpty() || sch->getDataDisplay().isEmpty()) return;
    const QString dpl = QFileInfo(sch->getDocName()).absoluteDir().filePath(sch->getDataDisplay());
    for (Schematic* doc : showing)
        if (sameFile(doc->getDocName(), dpl)) return;   // open: changed above, to undo
    QFile file(dpl);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    file.close();
    int count = 0;
    for (QString& line : lines) {
        // A trace: <"ngspice/tran.v(out)" #0000ff 2 3 0 0 0>
        const QString t = line.trimmed();
        if (!t.startsWith(QLatin1String("<\""))) continue;
        const QString var = t.section(QLatin1Char('"'), 1, 1);
        const QString renamed = rename(var);
        if (renamed == var) continue;
        line.replace(QLatin1Char('"') + var + QLatin1Char('"'), QLatin1Char('"') + renamed + QLatin1Char('"'));
        ++count;
    }
    if (count > 0 && !aboutToWrite(dpl).isEmpty()) return;   // (a file not to write: left)
    if (count > 0 && file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        file.write(lines.join(QLatin1Char('\n')).toUtf8());
        *rewritten = tr("%1 (not open) was rewritten: %2 traces renamed.").arg(QFileInfo(dpl).fileName()).arg(count);
    }
}

QJsonObject QucsControl::renameNet(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString from = args.value(QLatin1String("from")).toString().trimmed();
    const QString to = args.value(QLatin1String("to")).toString().trimmed();
    if (from.isEmpty() || to.isEmpty()) return errorResult(tr("Which net, and its new name? ('from', 'to')"));
    if (from == to) return textResult(tr("The net is called %1 already.").arg(to));
    // (get_schematic calls ground's net gnd: it is not renamed.)
    if (from == QLatin1String("0") || from.compare(QLatin1String("gnd"), Qt::CaseInsensitive) == 0)
        return errorResult(tr("%1 is ground: it keeps its name (a part is taken off ground by deleting or moving its ground).").arg(from));
    if (const QString bad = badNetName(to); !bad.isEmpty()) return errorResult(bad + QLatin1Char('.'));
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
    QString rewritten;
    renameEverywhere(sch, [&](const QString& text) { return renameNetIn(text, from, to); }, &changed, &rewritten);
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
    const PartIndex parts(sch);
    for (const QJsonValue& v : args.value(QLatin1String("names")).toArray()) {
        if (Component* c = parts.find(v.toString(), &error); c != nullptr && !c->isSelected) {
            c->isSelected = true;
            ++n;
        } else if (c == nullptr) {
            // (GND among several: which there are.)
            missing << (toldByNumber(sch, v.toString()) ? error : v.toString());
        }
    }
    for (const QJsonValue& v : args.value(QLatin1String("diagrams")).toArray()) {
        if (Diagram* d = diagramOf(sch, v, &error)) {
            d->isSelected = true;
            ++n;
        } else {
            missing << tr("diagram %1").arg(v.toVariant().toString());
        }
    }
    for (const QJsonValue& v : args.value(QLatin1String("paintings")).toArray()) {
        if (Painting* p = paintingOf(*sch->a_Paintings, v, &error)) {
            p->isSelected = true;
            ++n;
        } else {
            missing << tr("painting %1").arg(v.toVariant().toString());
        }
    }
    sch->viewport()->update();
    QString text = n == 0 ? tr("Nothing is selected.") : tr("%1 selected.").arg(n);
    if (!missing.isEmpty()) text += QLatin1Char(' ') + notFound(missing);
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
    // The files the tools wrote, put back (not a document's own changes).
    if (!redo && args.contains(QLatin1String("files"))) {
        const QJsonValue v = args.value(QLatin1String("files"));
        const int steps = v.isBool() ? (v.toBool() ? 1 : 0) : v.toInt(0);
        if (steps < 1 || steps > 50 || args.contains(QLatin1String("steps")) || args.contains(QLatin1String("to")))
            return errorResult(tr("'files' is how many calls' files to put back, 1 to 50 (or true: the last one's), alone - "
                                  "not with 'steps' or 'to', which undo a document's changes."));
        return undoFiles(steps);
    }
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    a_app->showDocument(QucsApp::documentWidget(doc));
    QMetaObject::invokeMethod(a_app, "slotHideEdit", Qt::DirectConnection);
    const QJsonValue stepsValue = args.value(QLatin1String("steps"));
    if (!stepsValue.isUndefined() && !stepsValue.isNull()
        && (!stepsValue.isDouble() || stepsValue.toDouble() < 1 || stepsValue.toDouble() > 1000 || stepsValue.toDouble() != std::floor(stepsValue.toDouble())))
        return errorResult(tr("'steps' is how many, 1 to 1000."));
    int steps = stepsValue.isDouble() ? int(stepsValue.toDouble()) : 1;
    // To a step undo_history numbers: back, or forward, to how it was after it.
    auto* schematicDoc = dynamic_cast<Schematic*>(doc);
    if (args.contains(QLatin1String("to"))) {
        if (schematicDoc == nullptr) return errorResult(tr("'to' is for a schematic's steps (undo_history lists them)."));
        const int to = args.value(QLatin1String("to")).toInt(-1), at = schematicDoc->undoIndex();
        const int last = schematicDoc->undoCount() - 1;
        if (!args.value(QLatin1String("to")).isDouble() || to < 0 || to > last)
            return errorResult(tr("'to' is a step from 0 (as loaded) to %1, as undo_history lists them.").arg(last));
        if (to == at) return textResult(tr("It is at step %1 already.").arg(at));
        // (A redo goes forward only: one that undid thirteen steps, from a
        // step misremembered, was a trap. Undo's 'to' goes either way.)
        if (redo && to < at)
            return errorResult(tr("Step %1 is before the current step %2: redo goes forward only - undo with 'to': %1 goes back there.")
                                   .arg(to).arg(at));
        redo = to > at;
        steps = std::abs(to - at);
    }
    const QString before = schematicDoc != nullptr ? schematicDoc->snapshot() : QString();
    int made = 0;
    for (int i = 0; i < steps; ++i) {
        bool ok = true;
        if (auto* sch = dynamic_cast<Schematic*>(doc)) {
            ok = redo ? sch->redo() : sch->undo();
        } else if (auto* text = qobject_cast<TextDoc*>(QucsApp::documentWidget(doc))) {
            ok = redo ? text->document()->isRedoAvailable() : text->document()->isUndoAvailable();
            if (ok) {
                if (redo) text->redo();
                else text->undo();
            }
        }
        if (!ok) break;
        ++made;
    }
    if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->viewport()->update();
    if (made == 0) return errorResult(redo ? tr("There is nothing to redo.") : tr("There is nothing to undo."));
    QString text = steps == 1 ? (redo ? tr("Redone.") : tr("Undone.")) : (redo ? tr("Redone %1 steps.").arg(made) : tr("Undone %1 steps.").arg(made));
    if (made < steps) text += QLatin1Char(' ') + (redo ? tr("There was no more to redo.") : tr("There was no more to undo."));
    // What that did, part by part - after it, the state the undo stack
    // keeps, not made again (half a second at 37,500 parts).
    if (schematicDoc != nullptr) {
        const int at = schematicDoc->undoIndex();
        const QString after = !schematicDoc->getSymbolMode() && at >= 0 && at < schematicDoc->undoCount() ? schematicDoc->undoState(at)
                                                                                                          : schematicDoc->snapshot();
        const QStringList what = describeChanges(before, after, 10);
        // (What it changed, as that - "Now:" read as how things are.)
        if (!what.isEmpty())
            text += QLatin1Char(' ') + (redo ? tr("The redo changed: %1.") : tr("The undo changed: %1.")).arg(what.join(QStringLiteral("; ")));
        text += QLatin1Char(' ') + tr("(Step %1 of %2.)").arg(schematicDoc->undoIndex()).arg(schematicDoc->undoCount() - 1);
    }
    return textResult(text);
}

namespace {

QJsonObject pngContent(const QImage& image)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return {{QStringLiteral("type"), QStringLiteral("image")},
            {QStringLiteral("data"), QString::fromLatin1(png.toBase64())},
            {QStringLiteral("mimeType"), QStringLiteral("image/png")}};
}

// Not wider than a picture to look at needs to be (a window on a Retina
// screen is twice its size in pixels).
QImage fitted(const QImage& image)
{
    return image.width() > 1800 ? image.scaledToWidth(1800, Qt::SmoothTransformation) : image;
}

} // namespace

QJsonObject QucsControl::screenshot(const QJsonObject& args)
{
    QString area = args.value(QLatin1String("area")).toString(QStringLiteral("paper"));
    if (area == QLatin1String("all")) area = QStringLiteral("paper");
    if (area == QLatin1String("visible")) area = QStringLiteral("screen");
    if (area != QLatin1String("paper") && area != QLatin1String("screen") && area != QLatin1String("window"))
        return errorResult(tr("'area' is paper (all), screen (visible) or window."));
    // The whole window - its panes, tabs, docks - and each dialog open over it.
    if (area == QLatin1String("window")) {
        QJsonArray content{pngContent(fitted(a_app->grab().toImage()))};
        QStringList shown{tr("the window, %1 x %2").arg(a_app->width()).arg(a_app->height())};
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w == a_app || !w->isVisible() || w->isMinimized()) continue;
            if (qobject_cast<QDialog*>(w) == nullptr && !w->isModal()) continue;
            content.append(pngContent(fitted(w->grab().toImage())));
            shown << tr("the dialog \"%1\"").arg(w->windowTitle().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->windowTitle());
        }
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), tr("Pictures of %1.").arg(shown.join(QStringLiteral(", ")))}});
        return {{QStringLiteral("content"), content}, {QStringLiteral("isError"), false}};
    }
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    QImage image;
    auto* sch = dynamic_cast<Schematic*>(doc);
    // One diagram, or a region: that part of the paper picture.
    if (args.contains(QLatin1String("diagram")) || args.contains(QLatin1String("region"))) {
        if (sch == nullptr) return errorResult(tr("%1 is not a schematic: 'diagram' and 'region' are of a schematic.").arg(titleOf(doc)));
        QRect wanted;
        QString what;
        if (args.contains(QLatin1String("diagram"))) {
            Diagram* d = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
            if (d == nullptr) return errorResult(error);
            wanted = d->boundingRect().adjusted(-20, -20, 20, 20);
            what = tr("diagram %1").arg(args.value(QLatin1String("diagram")).toInt());
        } else {
            const QJsonArray r = args.value(QLatin1String("region")).toArray();
            if (r.size() != 4) return errorResult(tr("'region' is [x1, y1, x2, y2]."));
            wanted = regionOf(r);
            what = tr("the region %1, %2 - %3, %4").arg(wanted.left()).arg(wanted.top()).arg(wanted.right()).arg(wanted.bottom());
        }
        const QRect all = qucs_s::graphicsexport::area(sch, false);
        const QRect part = wanted.intersected(all);
        if (part.isEmpty()) return errorResult(tr("There is nothing of %1 at %2.").arg(titleOf(doc), what));
        qucs_s::graphicsexport::Options options;
        // Sharp enough to read, the part at most 1600 pixels a side.
        options.scale = std::clamp(std::min(1600.0 / part.width(), 1600.0 / part.height()), 0.1, 4.0);
        // (Not bigger than a picture should be: the whole page at that scale.)
        while (options.scale > 0.1 && double(all.width()) * all.height() * options.scale * options.scale > 60e6) options.scale *= 0.8;
        const QImage page = qucs_s::graphicsexport::image(sch, options);
        if (page.isNull()) return errorResult(tr("No picture could be made of %1.").arg(titleOf(doc)));
        const double k = double(page.width()) / all.width();
        image = page.copy(QRect(int((part.left() - all.left()) * k), int((part.top() - all.top()) * k), int(part.width() * k), int(part.height() * k)));
        return {{QStringLiteral("content"),
                 QJsonArray{pngContent(fitted(image)),
                            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                        {QStringLiteral("text"), tr("%1, %2 on paper - %3 x %4 pixels.").arg(titleOf(doc), what).arg(image.width()).arg(image.height())}}}},
                {QStringLiteral("isError"), false}};
    }
    if (sch != nullptr && area == QLatin1String("paper")) {
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
    const QString what = sch == nullptr ? tr("as its window shows it")
                       : area == QLatin1String("paper") ? tr("all of it on white paper, as printed and exported")
                                                        : tr("as the canvas shows it now, in the theme's colours, at its zoom");
    return {{QStringLiteral("content"),
             QJsonArray{pngContent(fitted(image)),
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                    {QStringLiteral("text"), tr("%1, %2 - %3 x %4 pixels.").arg(titleOf(doc), what).arg(image.width()).arg(image.height())}}}},
            {QStringLiteral("isError"), false}};
}

namespace {

// What a file is, by its name: the kinds list_documents tells; empty for
// files it leaves out.
QString kindOfFile(const QString& name, QString* simulator = nullptr)
{
    const QString lower = name.toLower();
    static const QList<QPair<QString, QString>> datasets{{QStringLiteral(".dat.ngspice"), QStringLiteral("ngspice")},
                                                         {QStringLiteral(".dat.xyce"), QStringLiteral("xyce")},
                                                         {QStringLiteral(".dat.spopus"), QStringLiteral("spiceopus")},
                                                         {QStringLiteral(".dat"), QStringLiteral("qucsator")}};
    for (const auto& [ending, who] : datasets)
        if (lower.endsWith(ending)) {
            if (simulator != nullptr) *simulator = who;
            return QStringLiteral("dataset");
        }
    static const QHash<QString, QString> kinds{
        {QStringLiteral("sch"), QStringLiteral("schematic")},  {QStringLiteral("sym"), QStringLiteral("symbol")},
        {QStringLiteral("dpl"), QStringLiteral("data display")}, {QStringLiteral("cir"), QStringLiteral("netlist")},
        {QStringLiteral("net"), QStringLiteral("netlist")},    {QStringLiteral("ckt"), QStringLiteral("netlist")},
        {QStringLiteral("sp"), QStringLiteral("netlist")},     {QStringLiteral("spi"), QStringLiteral("netlist")},
        {QStringLiteral("spice"), QStringLiteral("netlist")},  {QStringLiteral("cdl"), QStringLiteral("netlist")},
        {QStringLiteral("lib"), QStringLiteral("netlist")},    {QStringLiteral("inc"), QStringLiteral("netlist")},
        {QStringLiteral("mod"), QStringLiteral("netlist")},    {QStringLiteral("va"), QStringLiteral("verilog-a")},
        {QStringLiteral("pdf"), QStringLiteral("pdf")},        {QStringLiteral("md"), QStringLiteral("markdown")},
        {QStringLiteral("markdown"), QStringLiteral("markdown")}, {QStringLiteral("csv"), QStringLiteral("spreadsheet")},
        {QStringLiteral("tsv"), QStringLiteral("spreadsheet")}, {QStringLiteral("xlsx"), QStringLiteral("spreadsheet")},
        {QStringLiteral("xls"), QStringLiteral("spreadsheet")}, {QStringLiteral("png"), QStringLiteral("picture")},
        {QStringLiteral("jpg"), QStringLiteral("picture")},    {QStringLiteral("jpeg"), QStringLiteral("picture")},
        {QStringLiteral("svg"), QStringLiteral("picture")},    {QStringLiteral("gif"), QStringLiteral("picture")},
        {QStringLiteral("bmp"), QStringLiteral("picture")},    {QStringLiteral("webp"), QStringLiteral("picture")},
        {QStringLiteral("tif"), QStringLiteral("picture")},    {QStringLiteral("tiff"), QStringLiteral("picture")},
        {QStringLiteral("eps"), QStringLiteral("picture")},    {QStringLiteral("txt"), QStringLiteral("text")},
        {QStringLiteral("log"), QStringLiteral("text")},       {QStringLiteral("m"), QStringLiteral("text")},
        {QStringLiteral("py"), QStringLiteral("text")},        {QStringLiteral("json"), QStringLiteral("text")},
        {QStringLiteral("xml"), QStringLiteral("text")},       {QStringLiteral("yaml"), QStringLiteral("text")},
        {QStringLiteral("yml"), QStringLiteral("text")},       {QStringLiteral("ini"), QStringLiteral("text")},
        {QStringLiteral("sh"), QStringLiteral("text")},        {QStringLiteral("tcl"), QStringLiteral("text")},
        {QStringLiteral("vhd"), QStringLiteral("text")},       {QStringLiteral("vhdl"), QStringLiteral("text")},
        {QStringLiteral("v"), QStringLiteral("text")},         {QStringLiteral("c"), QStringLiteral("text")},
        {QStringLiteral("cpp"), QStringLiteral("text")},       {QStringLiteral("h"), QStringLiteral("text")}};
    return kinds.value(QFileInfo(lower).suffix());
}

} // namespace

QJsonObject QucsControl::listDocuments(const QJsonObject& args)
{
    const QString workspace = QucsSettings.qucsWorkspaceDir.absolutePath();
    // The open project's files, when one is open ("my files"); else the
    // workspace's, and its projects.
    const bool inProject = !a_app->ProjName.isEmpty();
    QString root = inProject ? QucsSettings.QucsWorkDir.absolutePath() : workspace;
    const QString folder = args.value(QLatin1String("folder")).toString().trimmed();
    if (!folder.isEmpty()) {
        // A folder, or a project by its name (amp for amp_prj).
        const QString asFolder = absolute(folder);
        const QString asProject = QDir(workspace).filePath(folder + QStringLiteral("_prj"));
        if (QFileInfo(asFolder).isDir()) root = asFolder;
        else if (QFileInfo(asProject).isDir()) root = asProject;
        else return errorResult(tr("There is no folder or project %1 (list_documents without 'folder' lists the workspace and its projects).").arg(folder));
    }
    root = QDir::cleanPath(root);
    static const QStringList kinds{QStringLiteral("schematic"), QStringLiteral("symbol"),   QStringLiteral("data display"),
                                   QStringLiteral("dataset"),   QStringLiteral("netlist"),  QStringLiteral("text"),
                                   QStringLiteral("pdf"),       QStringLiteral("spreadsheet"), QStringLiteral("markdown"),
                                   QStringLiteral("picture"),   QStringLiteral("verilog-a")};
    const QString kind = args.value(QLatin1String("kind")).toString().trimmed().toLower();
    if (!kind.isEmpty() && !kinds.contains(kind)) return errorResult(tr("'kind' is one of: %1.").arg(kinds.join(QStringLiteral(", "))));
    const QString search = args.value(QLatin1String("search")).toString().trimmed();
    const QString sort = args.value(QLatin1String("sort")).toString(QStringLiteral("newest"));
    if (sort != QLatin1String("newest") && sort != QLatin1String("name")) return errorResult(tr("'sort' is newest or name."));

    QSet<QString> open;
    for (QucsDoc* doc : a_app->allDocuments())
        if (!doc->getDocName().isEmpty()) open.insert(QFileInfo(doc->getDocName()).absoluteFilePath());
    struct Entry {
        QFileInfo info;
        QString kind, simulator;
    };
    QList<Entry> entries;
    int looked = 0;
    bool cut = false;
    constexpr int kDepth = 4, kMostLooked = 50000;
    std::function<void(const QString&, int)> walk = [&](const QString& dir, int depth) {
        const QFileInfoList list = QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fi : list) {
            if (++looked > kMostLooked) {
                cut = true;
                return;
            }
            if (fi.fileName().startsWith(QLatin1Char('.'))) continue;
            if (fi.isDir()) {
                // (A linked project is a link: followed at the top only.)
                if (depth < kDepth && (!fi.isSymLink() || depth == 0)) walk(fi.filePath(), depth + 1);
                continue;
            }
            QString simulator;
            const QString k = kindOfFile(fi.fileName(), &simulator);
            if (k.isEmpty() || (!kind.isEmpty() && k != kind)) continue;
            if (!search.isEmpty() && !fi.fileName().contains(search, Qt::CaseInsensitive)) continue;
            entries.append(Entry{fi, k, simulator});
        }
    };
    walk(root, 0);
    if (sort == QLatin1String("newest"))
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.info.lastModified() > b.info.lastModified(); });
    else
        std::stable_sort(entries.begin(), entries.end(), [&root](const Entry& a, const Entry& b) {
            return QDir(root).relativeFilePath(a.info.filePath()).compare(QDir(root).relativeFilePath(b.info.filePath()), Qt::CaseInsensitive) < 0;
        });
    // The open schematics, and the traces of the diagrams that show their
    // data (theirs, their data displays'): each with the dataset it reads.
    struct Shown {
        QString trace, document, file, variable;
        int diagram;
    };
    QHash<QString, QList<Shown>> tracesOf;   // by schematic (absolute path)
    for (QucsDoc* doc : a_app->allDocuments()) {
        auto* sch = dynamic_cast<Schematic*>(doc);
        if (sch == nullptr || sch->getDocName().isEmpty() || !sch->getDocName().endsWith(QLatin1String(".sch"))) continue;
        QList<Shown> list;
        for (Schematic* shown : showingDataOf(sch)) {
            int n = 0;
            for (Diagram* d : shown->a_DocDiags) {
                ++n;
                for (Graph* g : d->Graphs) {
                    QString variable;
                    const QString file = QFileInfo(datasetOfTrace(sch, g->Var, &variable)).absoluteFilePath();
                    list.append(Shown{g->Var, titleOf(shown), file, variable, n});
                }
            }
        }
        tracesOf.insert(QFileInfo(sch->getDocName()).absoluteFilePath(), list);
    }
    QHash<QString, std::shared_ptr<qucs_s::dataset::Dataset>> read;
    const auto datasetAt = [&read](const QString& file) {
        auto& data = read[file];
        if (!data) {
            data = std::make_shared<qucs_s::dataset::Dataset>();
            if (!data->read(file)) data.reset();
        }
        return data;
    };
    constexpr int kMost = 300;
    QJsonArray files;
    for (const Entry& e : std::as_const(entries)) {
        if (files.size() >= kMost) break;
        QJsonObject f{{QStringLiteral("path"), QDir(root).relativeFilePath(e.info.filePath())},
                      {QStringLiteral("kind"), e.kind},
                      {QStringLiteral("size"), qint64(e.info.size())},
                      {QStringLiteral("changed"), e.info.lastModified().toString(Qt::ISODate)}};
        if (open.contains(e.info.absoluteFilePath())) f.insert(QStringLiteral("open"), true);
        if (e.kind == QLatin1String("dataset")) {
            // Imported (import_data, the Import tab): no simulator's; from where.
            if (qucs_s::dataimport::Origin origin; qucs_s::dataimport::originOf(e.info.absoluteFilePath(), &origin)) {
                f.insert(QStringLiteral("imported from"), shownFrom(e.info.absolutePath(), origin.source));
                f.insert(QStringLiteral("traces"), e.info.completeBaseName() + QStringLiteral(":variable"));
            } else {
                f.insert(QStringLiteral("simulator"), e.simulator);
            }
            // The schematic beside it of its name: rc.dat.ngspice is rc.sch's.
            QString base = e.info.fileName();
            base.truncate(base.toLower().indexOf(QLatin1String(".dat")));
            const QString sch = e.info.absoluteDir().filePath(base + QStringLiteral(".sch"));
            if (QFileInfo(sch).isFile()) f.insert(QStringLiteral("of"), QDir(root).relativeFilePath(sch));
            // The traces of the open diagrams that read it and that it has not.
            QJsonArray lacking;
            const QString file = e.info.absoluteFilePath();
            for (const Shown& t : tracesOf.value(QFileInfo(sch).absoluteFilePath())) {
                if (t.file != file || lacking.size() >= 20) continue;
                const auto data = datasetAt(file);
                if (data && data->find(t.variable) == nullptr)
                    lacking.append(QJsonObject{{QStringLiteral("trace"), t.trace}, {QStringLiteral("document"), t.document},
                                               {QStringLiteral("diagram"), t.diagram}});
            }
            if (!lacking.isEmpty()) f.insert(QStringLiteral("traces it does not have"), lacking);
        }
        // An open schematic: the traces whose dataset is not there at all
        // (a trace names its simulator: ngspice/... reads name.dat.ngspice).
        if (e.kind == QLatin1String("schematic") && tracesOf.contains(e.info.absoluteFilePath())) {
            QJsonArray missing;
            for (const Shown& t : tracesOf.value(e.info.absoluteFilePath())) {
                if (QFileInfo::exists(t.file) || missing.size() >= 20) continue;
                QJsonObject m{{QStringLiteral("trace"), t.trace}, {QStringLiteral("diagram"), t.diagram},
                              {QStringLiteral("document"), t.document}, {QStringLiteral("needs"), QFileInfo(t.file).fileName()}};
                // A prefix on a trace of a dataset of no simulator: its name.dat is there.
                const QString bare = qucs_s::dataset::withoutSimulator(t.trace);
                if (bare != t.trace && bare.contains(QLatin1Char(':'))) {
                    const QString name = bare.section(QLatin1Char(':'), 0, 0);
                    const QString plain = QFileInfo(t.file).absoluteDir().filePath(name + QStringLiteral(".dat"));
                    qucs_s::dataimport::Origin origin;
                    if (qucs_s::dataimport::originOf(plain, &origin))
                        m.insert(QStringLiteral("instead"), tr("%1: %2.dat is imported (from %3), read without a prefix")
                                                                .arg(bare, name, shownFrom(QFileInfo(plain).absolutePath(), origin.source)));
                    else if (QFileInfo(plain).isFile())
                        m.insert(QStringLiteral("instead"), tr("%1: %2.dat is there, a dataset of no simulator").arg(bare, name));
                }
                missing.append(m);
            }
            if (!missing.isEmpty()) f.insert(QStringLiteral("traces without their dataset"), missing);
        }
        files.append(f);
    }
    QJsonObject result{{QStringLiteral("folder"), QDir::toNativeSeparators(root)}, {QStringLiteral("files"), files}};
    // (The workspace's projects, also while one is listed: the others stay
    // in sight.)
    const bool ofProject = inProject && folder.isEmpty();
    if (ofProject)
        result.insert(QStringLiteral("project"),
                      tr("%1, open now: its files ('folder' with the workspace's path, %2, lists the workspace)")
                          .arg(a_app->ProjName, QDir::toNativeSeparators(workspace)));
    if (sameFile(root, workspace) || ofProject) {
        QJsonArray projects;
        for (const QFileInfo& fi : QDir(workspace).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (qucs_s::workspace::isProjectFolder(fi.filePath()))
                projects.append(QJsonObject{{QStringLiteral("project"), qucs_s::workspace::projectName(fi.fileName())},
                                            {QStringLiteral("folder"), fi.fileName()},
                                            {QStringLiteral("linked"), fi.isSymLink()}});
        result.insert(QStringLiteral("projects"), projects);
    }
    QStringList left;
    if (entries.size() > kMost) left << tr("%1 more files (kind, search or folder narrows the list)").arg(entries.size() - kMost);
    if (cut) left << tr("folders past the first %1 entries were not looked into").arg(kMostLooked);
    if (!left.isEmpty()) result.insert(QStringLiteral("left out"), left.join(QStringLiteral("; ")));
    if (files.isEmpty())
        result.insert(QStringLiteral("note"), kind.isEmpty() && search.isEmpty() ? tr("No documents there.") : tr("None of that kind or name there."));
    return jsonResult(result);
}

QJsonObject QucsControl::exportImage(const QJsonObject& args)
{
    namespace gx = qucs_s::graphicsexport;
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    QString file = args.value(QLatin1String("save_as")).toString().trimmed();
    if (file.isEmpty()) return errorResult(tr("'save_as' names the file to write."));
    if (const QString bad = badFileName(QFileInfo(file).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
    file = absolute(file);
    if (const QString bad = badFileName(QFileInfo(file).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
    // The format: as given, else the file's suffix.
    static const QList<QPair<QString, gx::Format>> names{
        {QStringLiteral("png"), gx::Format::Png},   {QStringLiteral("jpeg"), gx::Format::Jpeg}, {QStringLiteral("jpg"), gx::Format::Jpeg},
        {QStringLiteral("bmp"), gx::Format::Bmp},   {QStringLiteral("tiff"), gx::Format::Tiff}, {QStringLiteral("webp"), gx::Format::Webp},
        {QStringLiteral("svg"), gx::Format::Svg},   {QStringLiteral("pdf"), gx::Format::Pdf},   {QStringLiteral("eps"), gx::Format::Eps},
        {QStringLiteral("pdf_tex"), gx::Format::PdfTex}};
    std::optional<gx::Format> format;
    const QString formatName = args.value(QLatin1String("format")).toString().trimmed().toLower();
    if (!formatName.isEmpty()) {
        for (const auto& [name, f] : names)
            if (name == formatName) format = f;
        if (!format) return errorResult(tr("'format' is png, jpeg, bmp, tiff, webp, svg, pdf, eps or pdf_tex."));
    } else {
        format = gx::formatOf(file);
        if (!format) return errorResult(tr("%1 has no suffix of a picture's format: give 'format', or a name ending in .png, .svg, .pdf, ...")
                                            .arg(QFileInfo(file).fileName()));
    }
    if (!gx::formats().contains(*format)) return errorResult(tr("This build of Qucs-S does not write %1.").arg(gx::description(*format)));
    file = gx::withSuffix(file, *format);
    if (!QFileInfo(QFileInfo(file).absolutePath()).isDir()) return errorResult(tr("There is no folder %1.").arg(QFileInfo(file).absolutePath()));

    gx::Options options;
    const QJsonValue scale = args.value(QLatin1String("scale"));
    if (!scale.isUndefined() && (!scale.isDouble() || scale.toDouble() < 0.1 || scale.toDouble() > 20))
        return errorResult(tr("'scale' is a raster image's pixels per unit of the schematic, 0.1 to 20 (1 is 96 dpi)."));
    options.scale = scale.isDouble() ? scale.toDouble() : 2.0;
    const QString colours = args.value(QLatin1String("colours")).toString(QStringLiteral("colour")).trimmed().toLower();
    if (colours == QLatin1String("colour") || colours == QLatin1String("color")) options.colours = gx::Colours::Colour;
    else if (colours == QLatin1String("grayscale") || colours == QLatin1String("greyscale")) options.colours = gx::Colours::Grayscale;
    else if (colours == QLatin1String("monochrome")) options.colours = gx::Colours::Monochrome;
    else return errorResult(tr("'colours' is colour, grayscale or monochrome."));
    options.transparent = args.value(QLatin1String("transparent")).toBool();
    QStringList notes;
    if (options.transparent && !gx::hasTransparency(*format)) {
        options.transparent = false;
        notes << tr("%1 cannot be transparent: it has its paper.").arg(gx::description(*format));
    }

    // One diagram alone: selected by itself for the export, and what was
    // selected before selected again after.
    Diagram* diagram = nullptr;
    if (args.contains(QLatin1String("diagram"))) {
        diagram = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
        if (diagram == nullptr) return errorResult(error);
    }
    options.selectionOnly = diagram != nullptr || args.value(QLatin1String("selection")).toBool();
    if (const QString no = aboutToWrite(file); !no.isEmpty()) return errorResult(no);
    if (*format == gx::Format::PdfTex) {
        if (const QString no = aboutToWrite(gx::pdfOf(file)); !no.isEmpty()) return errorResult(no);
    }
    QList<Element*> selected;
    const auto keep = [&selected](Element* e) {
        if (e->isSelected) selected << e;
    };
    if (diagram != nullptr) {
        for (Component* c : *sch->a_Components) keep(c);
        for (Wire* w : *sch->a_Wires) {
            keep(w);
            if (w->hasLabel()) keep(w->label());
        }
        for (Node* n : *sch->a_Nodes)
            if (n->hasLabel()) keep(n->label());
        for (Diagram* d : *sch->a_Diagrams) keep(d);
        for (Painting* p : *sch->a_Paintings) keep(p);
        sch->deselectElements(nullptr);
        diagram->isSelected = true;
    }
    const QRect area = gx::area(sch, options.selectionOnly);
    const bool written = !area.isEmpty() && gx::write(sch, file, *format, options, &error);
    if (diagram != nullptr) {
        sch->deselectElements(nullptr);
        for (Element* e : std::as_const(selected)) e->isSelected = true;
        sch->viewport()->update();
    }
    if (area.isEmpty())
        return errorResult(options.selectionOnly ? tr("Nothing is selected (select selects components, diagrams and paintings).")
                                                 : tr("%1 is empty.").arg(titleOf(sch)));
    if (!written) return errorResult(tr("%1 was not written: %2").arg(QDir::toNativeSeparators(file), error));
    QJsonObject result{{QStringLiteral("written"), QDir::toNativeSeparators(file)},
                       {QStringLiteral("format"), gx::description(*format)},
                       {QStringLiteral("bytes"), qint64(QFileInfo(file).size())}};
    if (gx::isVector(*format)) {
        result.insert(QStringLiteral("size in units"), QJsonArray{area.width(), area.height()});
    } else {
        // As written: the file's own (the selection an export of one
        // diagram made is gone by now).
        QSize pixels = QImageReader(file).size();
        if (!pixels.isValid()) pixels = gx::pixelSize(sch, options);
        result.insert(QStringLiteral("pixels"), QJsonArray{pixels.width(), pixels.height()});
    }
    if (*format == gx::Format::PdfTex) result.insert(QStringLiteral("pdf"), QDir::toNativeSeparators(gx::pdfOf(file)));
    if (diagram != nullptr) result.insert(QStringLiteral("diagram"), args.value(QLatin1String("diagram")).toInt());
    if (!notes.isEmpty()) result.insert(QStringLiteral("note"), notes.join(QLatin1Char(' ')));
    return jsonResult(result);
}

QJsonObject QucsControl::setSimulator(const QJsonObject& args)
{
    static const QList<QPair<QString, int>> known{{QStringLiteral("ngspice"), spicecompat::simNgspice},
                                                  {QStringLiteral("xyce"), spicecompat::simXyce},
                                                  {QStringLiteral("spiceopus"), spicecompat::simSpiceOpus},
                                                  {QStringLiteral("qucsator"), spicecompat::simQucsator}};
    const auto nameOf = [](int simulator) {
        for (const auto& [name, code] : known)
            if (code == simulator) return name;
        return QStringLiteral("none");
    };
    QString wanted = args.value(QLatin1String("simulator")).toString().trimmed().toLower();
    wanted.remove(QLatin1Char(' '));
    int code = -1;
    for (const auto& [name, c] : known)
        if (name == wanted) code = c;
    if (code < 0) return errorResult(tr("'simulator' is ngspice, xyce, spiceopus or qucsator."));
    QComboBox* list = a_app->simulatorList();
    QJsonArray installed;
    int index = -1;
    for (int i = 0; list != nullptr && i < list->count(); ++i) {
        installed.append(nameOf(list->itemData(i).toInt()));
        if (list->itemData(i).toInt() == code) index = i;
    }
    if (index < 0)
        return errorResult(tr("%1 is not installed, or Qucs-S does not know where it is (Simulation > Simulators Settings... says). "
                              "Installed: %2.")
                               .arg(wanted, installed.isEmpty() ? tr("none") : QVariant(installed.toVariantList()).toStringList().join(QStringLiteral(", "))));
    if (a_app->simulationConsole() != nullptr && a_app->simulationConsole()->isRunning())
        return errorResult(tr("A simulation is running: the simulator is chosen after it has ended."));
    const QString before = nameOf(QucsSettings.DefaultSimulator);
    list->setCurrentIndex(index);
    QMetaObject::invokeMethod(a_app, "slotChangeSimulator", Qt::DirectConnection, Q_ARG(int, index));
    QJsonObject result{{QStringLiteral("simulator"), nameOf(QucsSettings.DefaultSimulator)},
                       {QStringLiteral("was"), before},
                       {QStringLiteral("installed"), installed}};
    if (QucsSettings.DefaultSimulator != code) result.insert(QStringLiteral("note"), tr("The simulator could not be changed."));
    return jsonResult(result);
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

QString QucsControl::refusedAction(QAction* action) const
{
    if (action == a_app->fileQuit) return tr("Claude does not quit Qucs-S.");
    if (action == a_app->fileOpen || action == a_app->fileSaveAs)
        return tr("%1: open_document and save_document do this.").arg(cleanText(action->text()));
    if (action == a_app->filePrint || action == a_app->filePrintFit)
        return tr("%1 prints on paper, which Claude does not do: export_image writes a picture of the schematic or of a "
                  "diagram.").arg(cleanText(action->text()));
    // (The Claude Code panel's own: its prompts, permissions and settings
    // are the user's.)
    if (QDockWidget* claude = a_app->claudeDockWidget())
        for (QObject* o = action; o != nullptr; o = o->parent())
            if (o == claude) return tr("%1 is the Claude Code panel's: it is the user's.").arg(cleanText(action->text()));
    return {};
}

void QucsControl::triggerAction(const QJsonObject& args, const Done& done)
{
    // The document it is for in front first: the menus act on that one.
    if (!args.value(QLatin1String("path")).toString().trimmed().isEmpty()) {
        QString error;
        QucsDoc* doc = document(args, &error);
        if (doc == nullptr) {
            done(errorResult(error));
            return;
        }
        a_app->showDocument(QucsApp::documentWidget(doc));
        QMetaObject::invokeMethod(a_app, "slotHideEdit", Qt::DirectConnection);
    }
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
    if (const QString why = refusedAction(action); !why.isEmpty()) {
        done(errorResult(why));
        return;
    }
    if (!action->isEnabled()) {
        done(errorResult(tr("%1 cannot be used now.").arg(cleanText(action->text()))));
        return;
    }
    // Triggered from the event loop; answered as soon as it is known: when
    // the action is over (a moment later, for a dialog it opens after
    // it), or when a dialog it opened is up - a modal one runs an event
    // loop of its own until it is answered, and the action returns only
    // then.
    QPointer<QAction> target(action);
    const QString name = cleanText(action->text());
    auto answered = std::make_shared<bool>(false);
    const auto answer = [this, done, name, answered] {
        if (*answered) return;
        *answered = true;
        QWidget* dialog = openDialog();
        done(textResult(dialog != nullptr
                            ? tr("%1: it opened “%2”, which waits for an answer (get_dialog reads it, set_dialog answers it).")
                                  .arg(name, dialog->windowTitle())
                            : tr("%1: done.").arg(name)));
    };
    auto* watch = new QTimer(this);
    watch->setInterval(10);
    connect(watch, &QTimer::timeout, this, [this, watch, answer, answered] {
        if (!*answered && openDialog() != nullptr) answer();
        if (*answered) watch->deleteLater();
    });
    watch->start();
    QTimer::singleShot(0, a_app, [this, target, answer] {
        {
            const QtFileDialogs qt;
            if (target) target->trigger();
        }
        QTimer::singleShot(30, this, answer);
    });
}

namespace {

// A batch under way: its calls one after another - each may answer later
// (a dialog, a simulation), the next going on from there - and their
// results together at the end, each under a line that names it (images, a
// screenshot's, as they are). Atomic, the schematics open when it began
// are kept as they were, to go back to when a call fails.
class BatchRun : public QObject
{
public:
    BatchRun(QucsControl* control, QucsApp* app, const QJsonArray& calls, bool keepGoing, bool atomic,
             const QList<Schematic*>& schematics, std::function<void(const QJsonObject&)> done)
        : QObject(control), a_control(control), a_app(app), a_calls(calls), a_keepGoing(keepGoing && !atomic), a_atomic(atomic),
          a_done(std::move(done))
    {
        if (atomic)
            for (Schematic* sch : schematics) a_before.append(Before{QPointer<Schematic>(sch), sch->snapshotAll(), sch->revision()});
        noteFront();
    }

    /// Each call that succeeds said in a line of its own: what it made or
    /// changed, not its whole answer (those that fail in full).
    void setBrief(bool brief) { a_brief = brief; }

    void next()
    {
        if (a_stopped || a_next >= a_calls.size()) {
            finish();
            return;
        }
        const int index = a_next++;
        const QJsonObject call = a_calls.at(index).toObject();
        const QString tool = call.value(QLatin1String("tool")).toString();
        QJsonObject arguments = call.value(QLatin1String("arguments")).toObject();
        // A call's own max_chars, as every tool takes it: its answer cut.
        int most = 0;
        const QJsonValue given = arguments.take(QLatin1String("max_chars"));
        const bool badMost = !given.isUndefined() && !qucs_s::mcp::readMaxChars(given, &most);
        const QPointer<BatchRun> self(this);
        const auto answered = [self, index, tool, arguments, most](const QJsonObject& result) {
            if (!self) return;
            self->record(index, tool, arguments, most > 0 ? qucs_s::mcp::trimmedTo(result, most) : result);
            QTimer::singleShot(0, self, [self] {
                if (self) self->next();
            });
        };
        // A call with no 'path' acts on the document in front - the one
        // the batch's calls left in front, not whichever the user (or a
        // tab closed meanwhile) brought there: the rest of a batch landed
        // in another document.
        if (tool != QLatin1String("batch") && arguments.value(QLatin1String("path")).toString().trimmed().isEmpty() && a_hadFront) {
            if (!a_front) {
                answered(errorResult(tr("%1, the document this batch works on, was closed: this call, which names no 'path', "
                                        "is not run on another.").arg(a_frontTitle)));
                return;
            }
            if (QucsApp::documentWidget(a_app->getDoc()) != a_front) a_app->showDocument(a_front);
        }
        const auto after = [self, answered](const QJsonObject& result) {
            if (self) self->noteFront();
            answered(result);
        };
        if (tool == QLatin1String("batch")) answered(errorResult(tr("A batch cannot hold another batch.")));
        // (A batch runs alone: every other call would wait with it.)
        else if (tool == QLatin1String("wait_for")) answered(errorResult(tr("wait_for is not for a batch, which every other call waits for: call it alone.")));
        else if (badMost) answered(errorResult(qucs_s::mcp::maxCharsRefusal(given)));
        else a_control->callTool(tool, arguments, after);
    }

private:
    struct Before {
        QPointer<Schematic> schematic;
        QPair<QString, QString> state;
        quint64 revision;
    };

    // The document in front now: the one the next call without 'path' is for.
    void noteFront()
    {
        QucsDoc* doc = a_app->DocumentTab->count() > 0 ? a_app->getDoc() : nullptr;
        a_hadFront = doc != nullptr;
        a_front = QucsApp::documentWidget(doc);
        if (doc != nullptr) a_frontTitle = titled(QucsApp::documentWidget(doc));
    }
    // A document's title: its file's whole name, or its tab's (untitled).
    QString titled(QWidget* w) const
    {
        if (QucsDoc* doc = QucsApp::docIn(w); doc != nullptr && !doc->getDocName().isEmpty()) return QFileInfo(doc->getDocName()).fileName();
        const QTabWidget* pane = a_app->paneOf(w);
        return pane != nullptr ? pane->tabText(pane->indexOf(w)).remove(QLatin1Char('&')).trimmed() : QString();
    }

    void record(int index, const QString& tool, const QJsonObject& arguments, const QJsonObject& result)
    {
        const bool error = result.value(QLatin1String("isError")).toBool();
        (error ? a_failed : a_succeeded)++;
        if (error && !a_keepGoing) a_stopped = true;
        // The steps to undo it made, document by document (each change
        // is one; undo and redo take them back and forth).
        static const QSet<QString> changing{
            QStringLiteral("set_schematic"), QStringLiteral("add_component"), QStringLiteral("edit_component"), QStringLiteral("delete"),
            QStringLiteral("connect"),       QStringLiteral("add_wire"),      QStringLiteral("set_label"),      QStringLiteral("add_diagram"),
            QStringLiteral("edit_diagram"),  QStringLiteral("add_trace"),     QStringLiteral("edit_trace"),     QStringLiteral("add_marker"),
            QStringLiteral("edit_marker"),   QStringLiteral("delete_marker"), QStringLiteral("rename_net"),     QStringLiteral("add_painting"),
            QStringLiteral("edit_painting")};
        if (!error) {
            const QString where = arguments.value(QLatin1String("path")).toString().trimmed();
            const int steps = std::max(1, arguments.value(QLatin1String("steps")).toInt(1));
            if (changing.contains(tool)) a_steps[where] += 1;
            else if (tool == QLatin1String("undo")) a_steps[where] -= steps;
            else if (tool == QLatin1String("redo")) a_steps[where] += steps;
        }
        if (a_brief && !error) {
            a_content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                         {QStringLiteral("text"), QStringLiteral("[%1] %2: %3").arg(index + 1).arg(tool, briefOf(result))}});
            return;
        }
        a_content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                     {QStringLiteral("text"),
                                      QStringLiteral("[%1] %2%3:").arg(index + 1).arg(tool, error ? tr(" failed") : QString())}});
        for (const QJsonValue& v : result.value(QLatin1String("content")).toArray()) a_content.append(v);
    }

    // An answer in a line: a few of its fields that say what it made (a
    // part's name, type and place; a document; a note), or its first
    // sentence; a picture as one.
    static QString briefOf(const QJsonObject& result)
    {
        QString text;
        bool picture = false;
        for (const QJsonValue& v : result.value(QLatin1String("content")).toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value(QLatin1String("type")).toString() == QLatin1String("image")) picture = true;
            else if (text.isEmpty()) text = o.value(QLatin1String("text")).toString();
        }
        QString said;
        const QJsonDocument parsed = QJsonDocument::fromJson(text.toUtf8());
        if (parsed.isObject()) {
            const QJsonObject o = parsed.object();
            QStringList fields;
            for (const char* key : {"name", "type", "x", "y", "document", "instance", "subcircuit", "succeeded", "dataset written", "arranged",
                                    "diagram", "found", "value"}) {
                const QJsonValue v = o.value(QLatin1String(key));
                if (v.isString() && !v.toString().isEmpty()) fields << QStringLiteral("%1 %2").arg(QLatin1String(key), v.toString().left(80));
                else if (v.isDouble() || v.isBool()) fields << QStringLiteral("%1 %2").arg(QLatin1String(key), v.toVariant().toString());
            }
            if (const QString note = o.value(QLatin1String("note")).toString(); !note.isEmpty()) fields << QStringLiteral("note: %1").arg(note.left(160));
            said = fields.isEmpty() ? QStringLiteral("done (%1 fields)").arg(o.size()) : fields.join(QStringLiteral(", "));
        } else {
            said = text.section(QLatin1Char('\n'), 0, 0);
            if (const qsizetype stop = said.indexOf(QLatin1String(". ")); stop > 0) said = said.left(stop + 1);
            if (said.size() > 200) said = said.left(200) + QStringLiteral(" ...");
            if (said.isEmpty()) said = QStringLiteral("done");
        }
        return picture ? said + QStringLiteral(" (a picture)") : said;
    }

    void finish()
    {
        QString head = tr("%1 of %2 calls done").arg(a_succeeded).arg(a_calls.size());
        if (a_failed > 0) head += tr(", %1 failed").arg(a_failed);
        if (a_stopped && a_next < a_calls.size())
            head += a_atomic ? tr("; stopped there, the rest not run") : tr("; stopped there, the rest not run (keep_going goes on)");
        head += QLatin1Char('.');
        if (a_failed > 0 && a_atomic) {
            // All or nothing: back as they were.
            QStringList back;
            for (const Before& b : std::as_const(a_before))
                if (b.schematic && b.schematic->revision() != b.revision && b.schematic->restoreAll(b.state))
                    back << titled(b.schematic);   // (an untitled one was named "")
            head += QLatin1Char(' ')
                  + (back.isEmpty() ? tr("Atomic: nothing had changed.")
                                    : tr("Atomic: the changes of the calls before it were undone - %1 as it was (one step to undo "
                                         "brings the changes back). Files written, simulations run and documents opened stay.")
                                          .arg(back.join(QStringLiteral(", "))));
        } else if (a_failed > 0 && a_stopped) {
            // What the calls before it left, to undo in one call.
            QStringList made;
            int most = 0;
            for (auto it = a_steps.cbegin(); it != a_steps.cend(); ++it) {
                if (it.value() <= 0) continue;
                most = std::max(most, it.value());
                made << (it.key().isEmpty() ? tr("%1 on the document in front").arg(it.value())
                                            : tr("%1 on %2").arg(it.value()).arg(QFileInfo(it.key()).fileName()));
            }
            if (!made.isEmpty()) {
                head += QLatin1Char(' ') + tr("The calls before it made changes that stay: %1 (undo with 'steps' takes them back%2; atomic: "
                                              "true would have).")
                                               .arg(made.join(QStringLiteral(", ")))
                                               .arg(most > int(QucsSettings.maxUndo) - 1
                                                        ? tr(" - Edit > Undo keeps %1 steps, fewer than that").arg(QucsSettings.maxUndo)
                                                        : QString());
            }
        }
        QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), head}}};
        for (const QJsonValue& v : std::as_const(a_content)) content.append(v);
        const auto done = std::move(a_done);
        deleteLater();
        done(QJsonObject{{QStringLiteral("content"), content}, {QStringLiteral("isError"), a_failed > 0}});
    }

    QucsControl* a_control;
    QucsApp* a_app;
    QPointer<QWidget> a_front;   // the document the calls without 'path' are for
    QString a_frontTitle;
    bool a_hadFront = false;
    QJsonArray a_calls;
    bool a_keepGoing;
    bool a_brief = false;
    bool a_atomic;
    std::function<void(const QJsonObject&)> a_done;
    QList<Before> a_before;
    QHash<QString, int> a_steps;   // by the 'path' of the calls ("": the document in front)
    QJsonArray a_content;
    int a_next = 0;
    int a_succeeded = 0;
    int a_failed = 0;
    bool a_stopped = false;
};

} // namespace

void QucsControl::runBatch(const QJsonObject& args, const Done& done)
{
    const QJsonArray calls = args.value(QLatin1String("calls")).toArray();
    if (calls.isEmpty()) {
        done(errorResult(tr("batch needs 'calls': [{\"tool\": ..., \"arguments\": {...}}, ...].")));
        return;
    }
    const bool atomic = args.value(QLatin1String("atomic")).toBool();
    QList<Schematic*> schematics;
    if (atomic)
        for (QucsDoc* doc : a_app->allDocuments())
            if (auto* sch = dynamic_cast<Schematic*>(doc)) schematics << sch;
    auto* run = new BatchRun(this, a_app, calls, args.value(QLatin1String("keep_going")).toBool(), atomic, schematics, done);
    run->setBrief(args.value(QLatin1String("brief")).toBool());
    run->next();
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

QList<QWidget*> QucsControl::dialogControls(QWidget* dialog, bool ui) const
{
    QList<QWidget*> controls;
    QList<QWidget*> all = dialog->findChildren<QWidget*>();
    // A panel that is a control itself (the Problems list, a log).
    if (ui) all.prepend(dialog);
    for (QWidget* w : std::as_const(all)) {
        if (w != dialog && !shown(w, dialog)) continue;
        if (usersOnly(w)) continue;   // (the user's alone: the Claude chip)
        // The line edit inside a combo box or a spin box is theirs.
        if (qobject_cast<QLineEdit*>(w) != nullptr
            && (qobject_cast<QComboBox*>(w->parentWidget()) != nullptr || qobject_cast<QAbstractSpinBox*>(w->parentWidget()) != nullptr))
            continue;
        bool control = qobject_cast<QLineEdit*>(w) != nullptr || qobject_cast<QPlainTextEdit*>(w) != nullptr
                       || (qobject_cast<QTextEdit*>(w) != nullptr && !static_cast<QTextEdit*>(w)->isReadOnly())
                       || qobject_cast<QComboBox*>(w) != nullptr || qobject_cast<QAbstractSpinBox*>(w) != nullptr
                       || qobject_cast<QAbstractButton*>(w) != nullptr || qobject_cast<QTableWidget*>(w) != nullptr
                       || qobject_cast<QListWidget*>(w) != nullptr || qobject_cast<QTabWidget*>(w) != nullptr
                       || qobject_cast<QTreeWidget*>(w) != nullptr;
        // A part of the window: its views of files and parts, sliders, and
        // logs too (not a scroll bar, a header, or a combo box's list).
        if (ui && !control)
            control = (qobject_cast<QAbstractItemView*>(w) != nullptr && qobject_cast<QHeaderView*>(w) == nullptr
                       && qobject_cast<QComboBox*>(w->parentWidget() != nullptr ? w->parentWidget()->parentWidget() : nullptr) == nullptr)
                      || (qobject_cast<QAbstractSlider*>(w) != nullptr && qobject_cast<QScrollBar*>(w) == nullptr)
                      || qobject_cast<QTextEdit*>(w) != nullptr;
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

// The label just above \a w in a column of its layout - above it, or above
// the row it is in (a field and its Browse button): Simulators Settings'
// paths.
QLabel* labelAbove(QWidget* w)
{
    QWidget* parent = w->parentWidget();
    QLayout* holding = parent != nullptr ? layoutHolding(parent->layout(), w) : nullptr;
    if (holding == nullptr) return nullptr;
    // What stands in the column: \a w, or its row.
    QLayout* column = holding;
    QLayout* row = nullptr;
    if (auto* box = qobject_cast<QBoxLayout*>(holding);
        box != nullptr && (box->direction() == QBoxLayout::LeftToRight || box->direction() == QBoxLayout::RightToLeft)) {
        row = holding;
        column = nullptr;
        const std::function<QLayout*(QLayout*)> parentOf = [&](QLayout* l) -> QLayout* {
            for (int i = 0; l != nullptr && i < l->count(); ++i) {
                if (QLayout* sub = l->itemAt(i)->layout()) {
                    if (sub == row) return l;
                    if (QLayout* found = parentOf(sub)) return found;
                }
            }
            return nullptr;
        };
        column = parentOf(parent->layout());
    }
    auto* box = qobject_cast<QBoxLayout*>(column);
    if (box == nullptr || (box->direction() != QBoxLayout::TopToBottom && box->direction() != QBoxLayout::BottomToTop)) return nullptr;
    int index = -1;
    for (int i = 0; i < box->count(); ++i)
        if ((row != nullptr && box->itemAt(i)->layout() == row) || (row == nullptr && box->itemAt(i)->widget() == w)) index = i;
    if (index <= 0) return nullptr;
    return qobject_cast<QLabel*>(box->itemAt(index - 1)->widget());
}

// The cell of an item view \a w is the editor or widget of (or inside
// one): the view and the cell's index; none when it is in no view's cell.
QAbstractItemView* cellOf(QWidget* w, QModelIndex* cell)
{
    for (QWidget* up = w->parentWidget(); up != nullptr; up = up->parentWidget()) {
        auto* view = qobject_cast<QAbstractItemView*>(up);
        if (view == nullptr) continue;
        const QAbstractItemModel* model = view->model();
        if (model == nullptr || !view->viewport()->isAncestorOf(w)) return nullptr;
        for (int r = 0; r < model->rowCount(view->rootIndex()) && r < 1000; ++r)
            for (int k = 0; k < model->columnCount(view->rootIndex()) && k < 50; ++k) {
                const QModelIndex index = model->index(r, k, view->rootIndex());
                if (QWidget* there = view->indexWidget(index); there != nullptr && (there == w || there->isAncestorOf(w))) {
                    *cell = index;
                    return view;
                }
            }
        return nullptr;
    }
    return nullptr;
}

// A field that is a view's cell editor (Edit Component Properties edits
// each value in one, open all the time): its text given to the cell, as
// the view's delegate does when the user leaves it or presses Return - set
// alone, the dialog read the cell's old value on OK. (The lists in cells
// are widgets their dialogs read themselves.)
void commitEditor(QLineEdit* e)
{
    QModelIndex cell;
    QAbstractItemView* view = cellOf(e, &cell);
    if (view == nullptr || view->indexWidget(cell) != e) return;
    if (QAbstractItemDelegate* delegate = view->itemDelegateForIndex(cell)) delegate->setModelData(e, view->model(), cell);
}

// What a field or list in a table's cell is: its row's name and its
// column's - "R (Value)", Edit Component Properties' value of R.
QString cellLabel(QWidget* w)
{
    if (qobject_cast<QAbstractButton*>(w) != nullptr) return {};   // (its own text: "Remove")
    QModelIndex cell;
    QAbstractItemView* view = cellOf(w, &cell);
    if (view == nullptr) return {};
    const QString row = cell.column() > 0 ? cell.sibling(cell.row(), 0).data().toString().trimmed() : QString();
    const QString column = view->model()->headerData(cell.column(), Qt::Horizontal).toString().trimmed();
    if (row.isEmpty()) return column.isEmpty() ? QString() : QStringLiteral("%1 %2").arg(column).arg(cell.row());
    return column.isEmpty() ? row : QStringLiteral("%1 (%2)").arg(row, column);
}

// Whether an item of a table, list or tree has a check box: one it may
// tick (its flag) and a state to show. (A new item has the flag - a table
// cell made not editable with flags() ^ ItemIsEditable kept it - with no
// box drawn, its text all there is.)
bool hasCheckBox(Qt::ItemFlags flags, const QVariant& state)
{
    return (flags & Qt::ItemIsUserCheckable) && state.isValid();
}

// What a field is called: its label in a form, the label that names it as
// its buddy, the label before it in its layout or just to its left, the
// label above it, else what it says of itself. A table cell's field: its
// row's and its column's names. A button says what it is itself - but a
// check box with no text of its own is named by the label before it in its
// row, as a field is (Application Settings' "Ground pin (gnd) in exported
// subcircuits:" beside its box; its tool tip, a paragraph, named it) - by
// its tool tip still when its row has none.
QString labelOf(QWidget* w, QWidget* dialog)
{
    if (const QString cell = cellLabel(w); !cell.isEmpty()) return cell;
    for (QFormLayout* form : dialog->findChildren<QFormLayout*>())
        if (QWidget* label = form->labelForField(w))
            if (auto* l = qobject_cast<QLabel*>(label)) return cleanText(l->text()).remove(QLatin1Char(':'));
    for (QLabel* l : dialog->findChildren<QLabel*>())
        if (l->buddy() == w) return cleanText(l->text()).remove(QLatin1Char(':'));
    auto* check = qobject_cast<QCheckBox*>(w);
    const bool textless = check != nullptr && cleanText(check->text()).isEmpty();
    if (qobject_cast<QAbstractButton*>(w) == nullptr || textless)
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
        if (QLabel* l = labelAbove(w); l != nullptr && !l->text().trimmed().isEmpty())
            return cleanText(l->text()).remove(QLatin1Char(':'));
    }
    if (auto* b = qobject_cast<QAbstractButton*>(w)) return cleanText(b->text()).isEmpty() ? b->toolTip() : cleanText(b->text());
    if (auto* e = qobject_cast<QLineEdit*>(w); e != nullptr && !e->placeholderText().isEmpty()) return e->placeholderText();
    if (!w->toolTip().isEmpty()) return w->toolTip();
    if (!w->accessibleName().isEmpty()) return w->accessibleName();
    return w->objectName();
}

// A control of a part of the window by what it is, when nothing names it:
// "log", "tree view".
QString kindName(QWidget* w)
{
    if ((qobject_cast<QPlainTextEdit*>(w) != nullptr && static_cast<QPlainTextEdit*>(w)->isReadOnly())
        || (qobject_cast<QTextEdit*>(w) != nullptr && static_cast<QTextEdit*>(w)->isReadOnly()))
        return QStringLiteral("log");
    if (qobject_cast<QTreeWidget*>(w) != nullptr) return QStringLiteral("tree");
    if (qobject_cast<QTableWidget*>(w) != nullptr) return QStringLiteral("table");
    if (qobject_cast<QListWidget*>(w) != nullptr || qobject_cast<QComboBox*>(w) != nullptr) return QStringLiteral("list");
    if (qobject_cast<QTreeView*>(w) != nullptr) return QStringLiteral("tree view");
    if (qobject_cast<QTableView*>(w) != nullptr) return QStringLiteral("table view");
    if (qobject_cast<QAbstractItemView*>(w) != nullptr) return QStringLiteral("list view");
    if (qobject_cast<QAbstractSlider*>(w) != nullptr) return QStringLiteral("slider");
    return {};
}

// What a control is called where it is read and used: its label, else
// (in a part of the window) what it is.
QString shownLabel(QWidget* w, QWidget* root, bool ui)
{
    const QString label = labelOf(w, root);
    return label.isEmpty() && ui ? kindName(w) : label;
}

} // namespace

QJsonObject QucsControl::getDialog()
{
    QWidget* dialog = openDialog();
    if (dialog == nullptr) return textResult(tr("No dialog is open."));
    QJsonObject o = describeControls(dialog, false);
    o.insert(QStringLiteral("title"), dialog->windowTitle());
    o.insert(QStringLiteral("modal"), dialog == QApplication::activeModalWidget());
    return jsonResult(o);
}

namespace {

// A view's rows as the user sees them, in order: a tree's children under
// each expanded item, with their depth. At most \a most.
void viewRows(const QAbstractItemView* view, const QModelIndex& parent, int depth, int most, QList<QModelIndex>* rows)
{
    const QAbstractItemModel* model = view->model();
    if (model == nullptr) return;
    const auto* tree = qobject_cast<const QTreeView*>(view);
    for (int r = 0; r < model->rowCount(parent) && rows->size() < most; ++r) {
        const QModelIndex index = model->index(r, 0, parent);
        if (tree != nullptr && tree->isRowHidden(r, parent)) continue;
        if (auto* list = qobject_cast<const QListView*>(view); list != nullptr && list->isRowHidden(r)) continue;
        rows->append(index);
        if (tree != nullptr && tree->isExpanded(index)) viewRows(view, index, depth + 1, most, rows);
    }
    Q_UNUSED(depth);
}

int depthOf(const QAbstractItemView* view, QModelIndex index)
{
    int depth = 0;
    for (; index.parent().isValid() && index.parent() != view->rootIndex(); index = index.parent()) ++depth;
    return depth;
}

// A view's row named by \a value: its number in viewRows()' order, a path
// of names ("Schematics > amp.sch") from the top, or a name among the rows
// shown (exactly, else the first that has it).
QModelIndex viewRow(QAbstractItemView* view, const QJsonValue& value)
{
    QList<QModelIndex> rows;
    viewRows(view, view->rootIndex(), 0, 5000, &rows);
    if (value.isDouble()) {
        const int n = value.toInt();
        return n >= 0 && n < rows.size() ? rows.at(n) : QModelIndex();
    }
    const QString text = value.toString().trimmed();
    if (text.contains(QLatin1String(" > "))) {
        QModelIndex at = view->rootIndex();
        for (const QString& part : text.split(QStringLiteral(" > "))) {
            QModelIndex found;
            for (int r = 0; r < view->model()->rowCount(at) && !found.isValid(); ++r) {
                const QModelIndex child = view->model()->index(r, 0, at);
                if (child.data().toString().trimmed().compare(part.trimmed(), Qt::CaseInsensitive) == 0) found = child;
            }
            if (!found.isValid()) return {};
            at = found;
        }
        return at;
    }
    for (const QModelIndex& index : rows)
        if (index.data().toString().trimmed().compare(text, Qt::CaseInsensitive) == 0) return index;
    for (const QModelIndex& index : rows)
        if (index.data().toString().contains(text, Qt::CaseInsensitive)) return index;
    return {};
}

// A mouse click (and a double one) on \a w at \a at, as the user's.
void clickOn(QWidget* w, const QPoint& at, Qt::MouseButton button, bool twice)
{
    const QPointF local(at), global(w->mapToGlobal(at));
    const auto send = [&](QEvent::Type type, Qt::MouseButtons held) {
        QMouseEvent e(type, local, global, button, held, Qt::NoModifier);
        QApplication::sendEvent(w, &e);
    };
    send(QEvent::MouseButtonPress, button);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
    if (twice) {
        send(QEvent::MouseButtonDblClick, button);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
    }
}

} // namespace

QJsonValue QucsControl::typedValue(QWidget* w) const
{
    if (auto* e = qobject_cast<QLineEdit*>(w)) return e->echoMode() == QLineEdit::Normal ? QJsonValue(e->text()) : QJsonValue(QStringLiteral("(hidden)"));
    if (auto* b = qobject_cast<QAbstractButton*>(w)) return b->isChecked();
    if (auto* c = qobject_cast<QComboBox*>(w)) return c->currentText();
    if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) return d->value();
    if (auto* n = qobject_cast<QSpinBox*>(w)) return n->value();
    if (auto* p = qobject_cast<QPlainTextEdit*>(w)) return p->toPlainText();
    if (auto* t = qobject_cast<QTextEdit*>(w)) return t->toPlainText();
    if (auto* table = qobject_cast<QTableWidget*>(w); table != nullptr && table->property("paths").toBool()) {
        // A list of folders (the search paths): each row's.
        QJsonArray folders;
        for (int r = 0; r < table->rowCount(); ++r)
            if (table->item(r, 0) != nullptr) folders.append(table->item(r, 0)->text());
        return folders;
    }
    if (auto* table = qobject_cast<QTableWidget*>(w)) {
        QJsonArray rows;
        for (int r = 0; r < table->rowCount(); ++r) {
            QJsonArray row;
            for (int k = 0; k < table->columnCount(); ++k)
                row.append(table->item(r, k) != nullptr ? table->item(r, k)->text() : QString());
            rows.append(row);
        }
        return rows;
    }
    return {};
}

QJsonArray QucsControl::typedSettings(QWidget* dialog, QHash<QString, QWidget*>* byKey) const
{
    QJsonArray list;
    QHash<QString, int> seen;
    for (QWidget* w : dialogControls(dialog, false)) {
        // Its settings, not its buttons and pages.
        auto* button = qobject_cast<QAbstractButton*>(w);
        if ((button != nullptr && !button->isCheckable()) || qobject_cast<QTabWidget*>(w) != nullptr) continue;
        QString label = labelOf(w, dialog).trimmed();
        if (label.endsWith(QLatin1Char(':'))) label.chop(1);
        if (label.isEmpty()) continue;
        const QString tab = tabOf(w, dialog);
        QString key = tab.isEmpty() ? label : tab + QLatin1Char('/') + label;
        if (const int n = ++seen[key]; n > 1) key += QStringLiteral(" (%1)").arg(n);
        QJsonObject o{{QStringLiteral("key"), key}, {QStringLiteral("value"), typedValue(w)}};
        if (qobject_cast<QCheckBox*>(w) != nullptr || qobject_cast<QRadioButton*>(w) != nullptr || button != nullptr) {
            o.insert(QStringLiteral("type"), qobject_cast<QRadioButton*>(w) != nullptr ? QStringLiteral("option") : QStringLiteral("bool"));
        } else if (auto* c = qobject_cast<QComboBox*>(w)) {
            o.insert(QStringLiteral("type"), c->isEditable() ? QStringLiteral("text") : QStringLiteral("choice"));
            QJsonArray items;
            for (int k = 0; k < c->count() && k < 100; ++k) items.append(c->itemText(k));
            o.insert(c->isEditable() ? QStringLiteral("suggestions") : QStringLiteral("choices"), items);
        } else if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) {
            o.insert(QStringLiteral("type"), QStringLiteral("number"));
            o.insert(QStringLiteral("minimum"), d->minimum());
            o.insert(QStringLiteral("maximum"), d->maximum());
        } else if (auto* n = qobject_cast<QSpinBox*>(w)) {
            o.insert(QStringLiteral("type"), QStringLiteral("integer"));
            o.insert(QStringLiteral("minimum"), n->minimum());
            o.insert(QStringLiteral("maximum"), n->maximum());
        } else if (qobject_cast<QTableWidget*>(w) != nullptr && w->property("paths").toBool()) {
            o.insert(QStringLiteral("type"), QStringLiteral("folders"));
        } else if (qobject_cast<QTableWidget*>(w) != nullptr) {
            o.insert(QStringLiteral("type"), QStringLiteral("table"));
        } else {
            o.insert(QStringLiteral("type"), QStringLiteral("text"));
        }
        if (!w->isEnabled()) o.insert(QStringLiteral("enabled"), false);
        list.append(o);
        if (byKey != nullptr) byKey->insert(key, w);
    }
    return list;
}

bool QucsControl::setTyped(QWidget* dialog, QWidget* w, const QJsonValue& value, QString* why)
{
    reveal(w, dialog);   // as the user would, on its tab
    if (!w->isEnabled()) {
        *why = tr("it cannot be changed now (it is greyed out)");
        return false;
    }
    if (auto* e = qobject_cast<QLineEdit*>(w)) {
        if (e->echoMode() != QLineEdit::Normal) {
            *why = tr("it is a secret field: the user types it");
            return false;
        }
        e->setText(propertyValue(value));
        e->setModified(true);
        return true;
    }
    if (auto* b = qobject_cast<QAbstractButton*>(w)) {
        if (!value.isBool()) {
            *why = tr("it takes true or false");
            return false;
        }
        if (qobject_cast<QRadioButton*>(w) != nullptr && !value.toBool()) {
            *why = tr("an option is left by choosing another");
            return false;
        }
        if (b->isChecked() != value.toBool()) b->click();
        return true;
    }
    if (auto* c = qobject_cast<QComboBox*>(w)) {
        const QString text = propertyValue(value);
        int index = c->findText(text, Qt::MatchFixedString);
        if (index < 0 && !c->isEditable()) index = c->findText(text, Qt::MatchContains);
        if (index >= 0) c->setCurrentIndex(index);
        else if (c->isEditable()) c->setEditText(text);
        else {
            QStringList items;
            for (int k = 0; k < c->count(); ++k) items << c->itemText(k);
            *why = tr("it is one of: %1").arg(items.join(QStringLiteral(", ")));
            return false;
        }
        return true;
    }
    if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) {
        bool ok = value.isDouble();
        const double v = ok ? value.toDouble() : propertyValue(value).toDouble(&ok);
        if (!ok || v < d->minimum() || v > d->maximum()) {
            *why = tr("it is a number from %1 to %2").arg(d->minimum()).arg(d->maximum());
            return false;
        }
        d->setValue(v);
        return true;
    }
    if (auto* n = qobject_cast<QSpinBox*>(w)) {
        bool ok = value.isDouble();
        const int v = ok ? value.toInt() : propertyValue(value).toInt(&ok);
        if (!ok || v < n->minimum() || v > n->maximum()) {
            *why = tr("it is a whole number from %1 to %2").arg(n->minimum()).arg(n->maximum());
            return false;
        }
        n->setValue(v);
        return true;
    }
    if (auto* p = qobject_cast<QPlainTextEdit*>(w)) {
        p->setPlainText(propertyValue(value));
        return true;
    }
    if (auto* t = qobject_cast<QTextEdit*>(w)) {
        t->setPlainText(propertyValue(value));
        return true;
    }
    if (qobject_cast<QTableWidget*>(w) != nullptr && w->property("paths").toBool()) {
        // The whole list: folders that are there, each a full path.
        QStringList folders;
        const QJsonArray given = value.isArray() ? value.toArray() : QJsonArray{value};
        for (const QJsonValue& v : given) {
            const QString folder = v.toString().trimmed();
            if (!v.isString() || folder.isEmpty()) {
                *why = tr("it is a list of folders, each a full path: [\"/path/to/libs\"] ([] for none)");
                return false;
            }
            if (!QFileInfo(folder).isAbsolute() || !QFileInfo(folder).isDir()) {
                *why = tr("%1 is not a folder here (each is a full path to one that exists)").arg(folder);
                return false;
            }
            folders << QDir::cleanPath(folder);
        }
        if (!QMetaObject::invokeMethod(dialog, "setPathList", Q_ARG(QString, w->objectName()), Q_ARG(QStringList, folders))) {
            *why = tr("this dialog does not take a list of folders");
            return false;
        }
        return true;
    }
    *why = qobject_cast<QTableWidget*>(w) != nullptr ? tr("it is a table: set_dialog sets its cells, the dialog open")
                                                      : tr("it is not one to set");
    return false;
}

bool QucsControl::canvasPoint(Schematic* sch, const QJsonObject& on, QPoint* point, QString* what, QString* error) const
{
    if (on.contains(QLatin1String("part"))) {
        const QString name = on.value(QLatin1String("part")).toString().trimmed();
        Component* c = componentOf(sch, name, error);
        if (c == nullptr) {
            if (error->isEmpty()) *error = tr("There is no component %1 in %2.").arg(name, titleOf(sch));
            return false;
        }
        *point = QPoint(c->cx, c->cy);
        *what = tr("part %1").arg(name);
        return true;
    }
    if (on.contains(QLatin1String("diagram"))) {
        Diagram* d = diagramOf(sch, on.value(QLatin1String("diagram")), error);
        if (d == nullptr) return false;
        *point = QPoint(d->cx + d->x2 / 2, d->cy - d->y2 / 2);
        *what = tr("diagram %1").arg(on.value(QLatin1String("diagram")).toInt(1));
        return true;
    }
    const QJsonArray xy = on.value(QLatin1String("canvas")).toArray();
    if (xy.size() != 2 || !xy.at(0).isDouble() || !xy.at(1).isDouble()) {
        *error = tr("'canvas' is [x, y], a point of the schematic (its coordinates, as get_schematic gives them).");
        return false;
    }
    *point = QPoint(xy.at(0).toInt(), xy.at(1).toInt());
    *what = tr("the canvas at %1, %2").arg(point->x()).arg(point->y());
    return true;
}

QJsonObject QucsControl::describeControls(QWidget* dialog, bool ui) const
{
    QJsonArray controls;
    const QList<QWidget*> list = dialogControls(dialog, ui);
    for (int i = 0; i < list.size(); ++i) {
        QWidget* w = list.at(i);
        QJsonObject o{{QStringLiteral("id"), QStringLiteral("c%1").arg(i + 1)}, {QStringLiteral("label"), shownLabel(w, dialog, ui)}};
        if (const QString tab = tabOf(w, dialog); !tab.isEmpty()) o.insert(QStringLiteral("tab"), tab);
        // Where it is in the area's (or the dialog's) picture - send_input's
        // pixels -, while it is shown.
        if (w != dialog && dialog->isVisible() && w->isVisible()) {
            const QPoint at = w->mapTo(dialog, QPoint(0, 0));
            o.insert(QStringLiteral("at"), QJsonArray{at.x(), at.y(), w->width(), w->height()});
        }
        if (!w->isEnabled()) o.insert(QStringLiteral("enabled"), false);
        auto* edit = qobject_cast<QLineEdit*>(w);
        if (edit != nullptr && edit->echoMode() != QLineEdit::Normal) {
            // (What it holds is the user's: a password, a key.)
            o.insert(QStringLiteral("kind"), QStringLiteral("secret field"));
            o.insert(QStringLiteral("value"), QStringLiteral("(hidden)"));
        } else if (edit != nullptr) {
            o.insert(QStringLiteral("kind"), QStringLiteral("field"));
            o.insert(QStringLiteral("value"), edit->text());
        } else if (ui && ((qobject_cast<QPlainTextEdit*>(w) != nullptr && static_cast<QPlainTextEdit*>(w)->isReadOnly())
                          || (qobject_cast<QTextEdit*>(w) != nullptr && static_cast<QTextEdit*>(w)->isReadOnly()))) {
            // A log: its end, which is what is new.
            const QString all = qobject_cast<QPlainTextEdit*>(w) != nullptr ? static_cast<QPlainTextEdit*>(w)->toPlainText()
                                                                             : static_cast<QTextEdit*>(w)->toPlainText();
            o.insert(QStringLiteral("kind"), QStringLiteral("log"));
            o.insert(QStringLiteral("lines"), int(all.count(QLatin1Char('\n'))) + (all.isEmpty() ? 0 : 1));
            o.insert(QStringLiteral("value"), all.size() > 4000 ? QStringLiteral("… ") + all.right(4000) : all);
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
                        if (item != nullptr && hasCheckBox(item->flags(), item->data(Qt::CheckStateRole))) row.append(item->checkState() == Qt::Checked);
                        else row.append(item != nullptr ? item->text() : QString());
                    }
                }
                rows.append(row);
            }
            o.insert(QStringLiteral("columns"), columns);
            o.insert(QStringLiteral("rows"), rows);
        } else if (auto* tree = qobject_cast<QTreeWidget*>(w)) {
            // Its rows in order, each item before its children (a search's
            // results, each with its check box: which of them are acted on).
            o.insert(QStringLiteral("kind"), QStringLiteral("tree"));
            QJsonArray columns;
            for (int k = 0; k < tree->columnCount(); ++k) columns.append(tree->headerItem() != nullptr ? tree->headerItem()->text(k) : QString::number(k));
            QJsonArray rows, checked;
            bool checkable = false;
            int n = 0;
            for (QTreeWidgetItemIterator it(tree); *it != nullptr && n < 200; ++it, ++n) {
                QJsonArray row;
                for (int k = 0; k < tree->columnCount(); ++k) row.append((*it)->text(k));
                rows.append(row);
                const bool box = hasCheckBox((*it)->flags(), (*it)->data(0, Qt::CheckStateRole));
                checkable = checkable || box;
                checked.append(box ? QJsonValue((*it)->checkState(0) == Qt::Checked) : QJsonValue(QJsonValue::Null));
            }
            o.insert(QStringLiteral("columns"), columns);
            o.insert(QStringLiteral("rows"), rows);
            if (checkable) o.insert(QStringLiteral("checked"), checked);
        } else if (auto* lw = qobject_cast<QListWidget*>(w)) {
            o.insert(QStringLiteral("kind"), QStringLiteral("list"));
            o.insert(QStringLiteral("value"), lw->currentItem() != nullptr ? lw->currentItem()->text() : QString());
            // Each item, and whether it is ticked when it has a check box
            // (Create Library's subcircuits: which go into the library).
            QJsonArray items, checked;
            bool checkable = false;
            for (int k = 0; k < lw->count() && k < 200; ++k) {
                items.append(lw->item(k)->text());
                const bool box = hasCheckBox(lw->item(k)->flags(), lw->item(k)->data(Qt::CheckStateRole));
                checkable = checkable || box;
                checked.append(box ? QJsonValue(lw->item(k)->checkState() == Qt::Checked) : QJsonValue(QJsonValue::Null));
            }
            o.insert(QStringLiteral("items"), items);
            if (checkable) o.insert(QStringLiteral("checked"), checked);
        } else if (auto* slider = qobject_cast<QAbstractSlider*>(w)) {
            o.insert(QStringLiteral("kind"), QStringLiteral("slider"));
            o.insert(QStringLiteral("value"), slider->value());
            o.insert(QStringLiteral("minimum"), slider->minimum());
            o.insert(QStringLiteral("maximum"), slider->maximum());
        } else if (auto* view = qobject_cast<QAbstractItemView*>(w)) {
            // A view of the files, the projects, the parts: its rows as
            // shown, a tree's with their depth and whether open.
            const bool isTree = qobject_cast<QTreeView*>(view) != nullptr;
            o.insert(QStringLiteral("kind"), isTree ? QStringLiteral("tree view")
                                                     : qobject_cast<QTableView*>(view) != nullptr ? QStringLiteral("table view")
                                                                                                    : QStringLiteral("list view"));
            QList<QModelIndex> shownRows;
            viewRows(view, view->rootIndex(), 0, 200, &shownRows);
            // The columns shown beside the name: none in a list, a tree's
            // and a table's that are not hidden.
            QList<int> columns;
            const int columnCount = view->model() != nullptr ? std::min(view->model()->columnCount(view->rootIndex()), 8) : 0;
            for (int k = 1; k < columnCount; ++k) {
                if (auto* tv = qobject_cast<QTreeView*>(view); tv != nullptr && !tv->isColumnHidden(k)) columns << k;
                if (auto* tb = qobject_cast<QTableView*>(view); tb != nullptr && !tb->isColumnHidden(k)) columns << k;
            }
            QJsonArray rows;
            for (const QModelIndex& index : std::as_const(shownRows)) {
                QJsonObject row{{QStringLiteral("text"), index.data().toString()}};
                QJsonArray more;
                for (int k : std::as_const(columns))
                    if (const QString cell = index.sibling(index.row(), k).data().toString(); !cell.isEmpty()) more.append(cell);
                if (!more.isEmpty()) row.insert(QStringLiteral("more"), more);
                if (isTree) {
                    if (const int depth = depthOf(view, index); depth > 0) row.insert(QStringLiteral("depth"), depth);
                    if (view->model()->hasChildren(index)) row.insert(QStringLiteral("open"), static_cast<QTreeView*>(view)->isExpanded(index));
                }
                if (view->selectionModel() != nullptr && view->selectionModel()->isSelected(index)) row.insert(QStringLiteral("selected"), true);
                rows.append(row);
            }
            o.insert(QStringLiteral("rows"), rows);
            if (view->currentIndex().isValid()) o.insert(QStringLiteral("value"), view->currentIndex().data().toString());
        }
        controls.append(o);
    }
    QJsonArray texts;
    if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
        texts.append(box->text());
        if (!box->informativeText().isEmpty()) texts.append(box->informativeText());
    } else {
        for (QLabel* l : dialog->findChildren<QLabel*>())
            if (l->isVisibleTo(dialog) && !l->text().trimmed().isEmpty() && l->buddy() == nullptr && texts.size() < 80 && !usersOnly(l))
                texts.append(cleanText(l->text()));
    }
    return QJsonObject{{QStringLiteral("texts"), texts}, {QStringLiteral("controls"), controls}};
}

QJsonArray QucsControl::optimumOf(Schematic* sch, const QString& output, bool apply)
{
    namespace ng = qucs_s::ngopt;
    QList<Component*> blocks;   // (in the netlist's order)
    for (Component* c : sch->a_DocComps)
        if (c->Model == QLatin1String(".NGOPT") && c->isActive == COMP_IS_ACTIVE) blocks << c;
    if (blocks.isEmpty() || ng::unsupported(output)) return {};
    const QList<ng::Result> results = ng::parseResults(output);
    // Where a knob is defined: a parameter of an equation block (.PARAM,
    // .GLOBAL_PARAM, Eqn), or a part's value (R1) or property (@m1[w]).
    const auto definition = [sch](const ng::Knob& k, QString* where, QString* why) -> Property* {
        if (k.kind == ng::KnobKind::Param) {
            for (Component* c : sch->a_DocComps) {
                if (c->isActive != COMP_IS_ACTIVE || !isEquationKind(c)
                    || (c->Model != QLatin1String("SpicePar") && c->Model != QLatin1String("SpGlobPar") && c->Model != QLatin1String("Eqn")))
                    continue;
                for (Property* p : c->Props)
                    if (p->Name.compare(k.name, Qt::CaseInsensitive) == 0) {
                        *where = c->Name + QLatin1Char('.') + p->Name;
                        return p;
                    }
            }
            *why = tr("no .PARAM or equation of the schematic defines %1").arg(k.name);
            return nullptr;
        }
        if (k.kind == ng::KnobKind::Model) {
            *why = tr("%1 is a .model's parameter: its model card holds it").arg(k.name);
            return nullptr;
        }
        // A part (R1), or a part's property (@m1[w]).
        static const QRegularExpression at(QStringLiteral("^@([^\\[]+)\\[([^\\]]+)\\]$"));
        const QRegularExpressionMatch m = at.match(k.name.trimmed());
        const QString part = m.hasMatch() ? m.captured(1) : k.name.trimmed();
        for (Component* c : sch->a_DocComps) {
            if (c->Name.compare(part, Qt::CaseInsensitive) != 0 || c->Props.isEmpty()) continue;
            Property* p = nullptr;
            if (!m.hasMatch()) p = c->Props.first();
            else
                for (Property* q : c->Props)
                    if (q->Name.compare(m.captured(2), Qt::CaseInsensitive) == 0) p = q;
            if (p == nullptr) break;
            *where = c->Name + QLatin1Char('.') + p->Name;
            return p;
        }
        *why = tr("no part of the schematic is %1").arg(k.name);
        return nullptr;
    };
    QJsonArray list;
    bool changing = false;
    QSet<Component*> touched;
    for (int i = 0; i < blocks.size(); ++i) {
        QJsonObject o{{QStringLiteral("block"), blocks.at(i)->Name}};
        if (i >= results.size()) {
            o.insert(QStringLiteral("found"), tr("nothing: ngspice reported no optimum (see the last lines)"));
            list.append(o);
            continue;
        }
        const ng::Result& r = results.at(i);
        const ng::Command command = ng::Command::read(blocks.at(i));
        o.insert(QStringLiteral("summary"), r.summary);
        if (!r.status.isEmpty()) o.insert(QStringLiteral("status"), r.status);
        if (r.interrupted) o.insert(QStringLiteral("interrupted"), true);
        if (!r.search.isEmpty()) o.insert(QStringLiteral("search"), QJsonArray::fromStringList(r.search));
        if (!r.constraints.isEmpty()) o.insert(QStringLiteral("constraints"), QJsonArray::fromStringList(r.constraints));
        if (!r.notes.isEmpty()) o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(r.notes));
        // Not written in: values of no solution (the initial ones), or
        // those that miss a constraint.
        QString refused;
        if (r.status == QLatin1String("nosolve"))
            refused = tr("no: no evaluation solved, so these are no optimum; check the knobs' ranges");
        else if (r.status == QLatin1String("infeasible"))
            refused = tr("no: a constraint was not met (see 'constraints'); loosen it or widen the knobs' ranges, or "
                         "edit_component writes these values in");
        QJsonObject found, applied, notApplied;
        for (int k = 0; k < r.values.size(); ++k) {
            const QString value = misc::num2str(r.values.at(k).second, -1, QString());
            const ng::Knob knob = k < command.knobs.size() ? command.knobs.at(k) : ng::Knob{};
            const QString name = knob.name.isEmpty() ? r.values.at(k).first : knob.name;
            found.insert(name, value);
            if (!apply || knob.name.isEmpty() || !refused.isEmpty()) continue;
            QString where, why;
            Property* p = definition(knob, &where, &why);
            if (p == nullptr) {
                notApplied.insert(name, why);
                continue;
            }
            if (!changing) {
                prepare(sch);
                changing = true;
            }
            p->Value = value;
            for (Component* c : sch->a_DocComps)
                if (c->Props.contains(p)) touched.insert(c);
            applied.insert(name, where);
        }
        o.insert(QStringLiteral("found"), found);
        if (apply && !refused.isEmpty()) {
            o.insert(QStringLiteral("applied"), refused);
        } else if (apply) {
            if (!applied.isEmpty()) o.insert(QStringLiteral("applied to"), applied);
            if (!notApplied.isEmpty()) o.insert(QStringLiteral("not applied"), notApplied);
        } else {
            o.insert(QStringLiteral("applied"),
                     tr("no: the knobs of %1 start from these now (Qucs-S wrote them in after the run), but the parameters keep "
                        "their values, which a run without the optimizer uses. simulate with 'apply_optimum' writes them in (one "
                        "undo step), or edit_component does.").arg(blocks.at(i)->Name));
        }
        list.append(o);
    }
    if (changing) {
        for (Component* c : std::as_const(touched)) c->recreate();
        finish(sch, {});
        // The dataset is the optimum's, which the parameters now hold: the
        // netlist kept for it is the one a run would be given now (else it
        // read as of another circuit - stale).
        const QString dataset = datasetFile(sch->getDocName(), sch->getDataSet(), spicecompat::simNgspice);
        QTemporaryDir temporary;
        const QString now = temporary.filePath(QStringLiteral("now.cir"));
        misc::ErrorCapture capture;
        SimulationRun netlister(sch, false);
        if (QFileInfo(dataset).isFile() && netlister.writeNetlist(now))
            if (QFile f(now); f.open(QIODevice::ReadOnly | QIODevice::Text))
                misc::keepRunNetlist(dataset, QString::fromUtf8(f.readAll()), sch->getDocName());
    }
    return list;
}

QString QucsControl::commandsRefused(Schematic* sch, const QJsonObject& args) const
{
    // A schematic opened from anywhere - a project's, a download - runs,
    // simulated, what it carries with the user's rights: a System command
    // part, ngspice's shell, an Octave script. With the Simulator Settings'
    // check of commands on: asked for ('allow_commands', which the client's
    // prompt shows), or not run. Off (the default): run as they are.
    if (!QucsSettings.CheckCommands) return QString();
    const QStringList commands = qucs_s::erc::commandsRun(sch);
    if (commands.isEmpty() || args.value(QLatin1String("allow_commands")).toBool()) return QString();
    return tr("%1 runs commands besides the simulator when it is simulated, with the user's rights: %2. Nothing was run. Give "
              "'allow_commands': true to simulate it with them - when the user has seen them.")
        .arg(titleOf(sch), commands.join(QStringLiteral("; ")));
}

namespace {
// What a table's or tree's rows (columns) are, for a row out of them.
QString rowsSaid(int rows, const QString& what)
{
    if (rows == 0) return QucsControl::tr("the %1 has no rows").arg(what);
    return rows == 1 ? QucsControl::tr("the %1 has 1 row (0)").arg(what) : QucsControl::tr("the %1 has %2 rows (0 to %3)").arg(what).arg(rows).arg(rows - 1);
}
QString columnsSaid(int columns, const QString& what)
{
    return columns == 1 ? QucsControl::tr("the %1 has 1 column (0)").arg(what)
                        : QucsControl::tr("the %1 has %2 columns (0 to %3)").arg(what).arg(columns).arg(columns - 1);
}
} // namespace

void QucsControl::setDialog(const QJsonObject& args, const Done& done)
{
    QWidget* dialog = openDialog();
    if (dialog == nullptr) {
        done(errorResult(tr("No dialog is open.")));
        return;
    }
    fillControls(dialog, args, done, false);
}

void QucsControl::fillControls(QWidget* dialog, const QJsonObject& args, const Done& done, bool ui)
{
    const QList<QWidget*> list = dialogControls(dialog, ui);
    const auto find = [&](const QString& key) -> QWidget* {
        const QString k = key.trimmed();
        static const QRegularExpression id(QStringLiteral("^c(\\d+)$"));
        if (const auto m = id.match(k); m.hasMatch()) {
            const int n = m.captured(1).toInt();
            return n >= 1 && n <= list.size() ? list.at(n - 1) : nullptr;
        }
        for (QWidget* w : list)
            if (shownLabel(w, dialog, ui).compare(k, Qt::CaseInsensitive) == 0) return w;
        for (QWidget* w : list)
            if (shownLabel(w, dialog, ui).startsWith(k, Qt::CaseInsensitive)) return w;
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
        QString why;   // (what it does take, when that is not plain)
        QString said;  // (what was set, when its label does not say: a cell)
        reveal(w, dialog);   // as the user would, on its tab
        // A row of a view chosen as a click would (an 'action'): a panel's
        // view always; a dialog's table, list or tree when one is asked -
        // Diagram Properties' variables take a trace by a double click.
        auto* view = qobject_cast<QAbstractItemView*>(w);
        const bool widgetView = qobject_cast<QTableWidget*>(w) != nullptr || qobject_cast<QTreeWidget*>(w) != nullptr
                                || qobject_cast<QListWidget*>(w) != nullptr;
        if (view != nullptr && (widgetView || !ui) && !change.contains(QLatin1String("action")))
            view = nullptr;   // (their own way, unless an action is asked)
        const QString action = change.value(QLatin1String("action")).toString(QStringLiteral("select"));
        if (auto* secret = qobject_cast<QLineEdit*>(w); secret != nullptr && secret->echoMode() != QLineEdit::Normal) {
            ok = false;
            why = tr("it is a secret field (a password, a key): the user types it");
        } else if (ui && ((qobject_cast<QPlainTextEdit*>(w) != nullptr && static_cast<QPlainTextEdit*>(w)->isReadOnly())
                          || (qobject_cast<QTextEdit*>(w) != nullptr && static_cast<QTextEdit*>(w)->isReadOnly()))) {
            ok = false;
            why = tr("it is read-only");
        } else if (view != nullptr) {
            // A row: selected (a click), activated (a double click: it
            // opens), or a tree's opened or closed.
            const QModelIndex index = viewRow(view, value);
            if (!index.isValid()) {
                ok = false;
                why = tr("no row %1 (get_ui lists the rows shown; a tree's hidden one by its path, \"Schematics > amp.sch\")").arg(text);
            } else if (action == QLatin1String("expand") || action == QLatin1String("collapse")) {
                auto* tree = qobject_cast<QTreeView*>(view);
                if (tree == nullptr) {
                    ok = false;
                    why = tr("it is no tree");
                } else {
                    for (QModelIndex up = index.parent(); up.isValid(); up = up.parent()) tree->expand(up);
                    tree->setExpanded(index, action == QLatin1String("expand"));
                }
            } else if (action == QLatin1String("select") || action == QLatin1String("activate")) {
                if (auto* tree = qobject_cast<QTreeView*>(view))
                    for (QModelIndex up = index.parent(); up.isValid(); up = up.parent()) tree->expand(up);
                view->scrollTo(index);
                const QRect at = view->visualRect(index);
                if (at.isValid() && view->viewport()->rect().contains(at.center())) {
                    const QtFileDialogs qt;
                    clickOn(view->viewport(), at.center(), Qt::LeftButton, action == QLatin1String("activate"));
                } else {
                    // Not on screen (the window is hidden): as a click would.
                    view->setCurrentIndex(index);
                    if (action == QLatin1String("activate")) {
                        const QtFileDialogs qt;
                        emit view->doubleClicked(index);
                        emit view->activated(index);
                    } else {
                        emit view->clicked(index);
                    }
                }
            } else {
                ok = false;
                why = tr("'action' is select, activate, expand or collapse");
            }
        } else if (auto* slider = ui ? qobject_cast<QAbstractSlider*>(w) : nullptr) {
            // As a drag would: pressed, moved, let go.
            const int to = text.toInt(&ok);
            if (ok && (to < slider->minimum() || to > slider->maximum())) {
                ok = false;
                why = tr("it goes from %1 to %2").arg(slider->minimum()).arg(slider->maximum());
            } else if (ok) {
                slider->setSliderDown(true);
                slider->setValue(to);
                slider->setSliderDown(false);
            }
        } else if (auto* e = qobject_cast<QLineEdit*>(w)) {
            e->setText(text);
            e->setModified(true);
            if (ui) emit e->textEdited(text);   // (as typed: a panel's search goes by it)
            commitEditor(e);
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
                why = cell.size() < 3 ? tr("a cell is [row, column, value]")
                                      : r < 0 || r >= table->rowCount() ? rowsSaid(table->rowCount(), tr("table"))
                                                                        : columnsSaid(table->columnCount(), tr("table"));
            } else if (QWidget* cw = table->cellWidget(r, k)) {
                // (A cell's editor - its value given to the cell, as the
                // user's Return does - or a widget in it.)
                if (auto* cc = qobject_cast<QComboBox*>(cw)) {
                    cc->setCurrentIndex(std::max(0, cc->findText(propertyValue(cell.at(2)))));
                } else if (auto* cb = qobject_cast<QAbstractButton*>(cw)) {
                    if (cb->isChecked() != cell.at(2).toBool()) cb->click();
                } else if (auto* ce = qobject_cast<QLineEdit*>(cw)) {
                    ce->setText(propertyValue(cell.at(2)));
                    ce->setModified(true);
                    commitEditor(ce);
                }
            } else {
                // A cell with a check box takes true or false; one with
                // text, a text - when the user may edit it.
                QTableWidgetItem* item = table->item(r, k);
                if (item == nullptr) table->setItem(r, k, item = new QTableWidgetItem);
                const bool box = hasCheckBox(item->flags(), item->data(Qt::CheckStateRole));
                if (box != cell.at(2).isBool()) {
                    ok = false;
                    why = box ? tr("its cell %1, %2 is a check box: true or false").arg(r).arg(k)
                              : tr("its cell %1, %2 has no check box: it takes a text").arg(r).arg(k);
                } else if (box) {
                    item->setCheckState(cell.at(2).toBool() ? Qt::Checked : Qt::Unchecked);
                } else if (!(item->flags() & Qt::ItemIsEditable)) {
                    ok = false;
                    why = tr("its cell %1, %2 is not one to edit (an 'action' chooses a row: select, activate)").arg(r).arg(k);
                } else {
                    item->setText(propertyValue(cell.at(2)));
                }
                if (ok) table->setCurrentCell(r, k);
            }
            if (ok) {
                const QString row = table->item(r, 0) != nullptr && k > 0 ? table->item(r, 0)->text().trimmed() : QString();
                const QString column = table->horizontalHeaderItem(k) != nullptr ? table->horizontalHeaderItem(k)->text().trimmed() : QString();
                said = tr("%1's cell %2, %3%4").arg(shownLabel(w, dialog, ui).isEmpty() ? QStringLiteral("c%1").arg(list.indexOf(w) + 1) : shownLabel(w, dialog, ui))
                           .arg(r).arg(k)
                           .arg(row.isEmpty() && column.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(QStringList({row, column}).join(QLatin1Char(' ')).trimmed()));
            }
        } else if (auto* tree = qobject_cast<QTreeWidget*>(w)) {
            // [row, column, true/false] checks a row or not; [row, column,
            // text] sets a cell's text. Rows as get_dialog numbers them.
            const QJsonArray cell = value.toArray();
            const int r = cell.at(0).toInt(-1), k = cell.at(1).toInt(-1);
            QTreeWidgetItem* item = nullptr;
            int n = 0;
            for (QTreeWidgetItemIterator it(tree); *it != nullptr && item == nullptr; ++it, ++n)
                if (n == r) item = *it;
            if (cell.size() < 3 || item == nullptr || k < 0 || k >= tree->columnCount()) {
                ok = false;
                int rows = 0;
                for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) ++rows;
                why = cell.size() < 3 ? tr("a row is [row, column, true or false (checked), or a text]")
                                      : item == nullptr ? rowsSaid(rows, tr("tree")) : columnsSaid(tree->columnCount(), tr("tree"));
            } else if (cell.at(2).isBool() && (item->flags() & Qt::ItemIsUserCheckable)) {
                item->setCheckState(k, cell.at(2).toBool() ? Qt::Checked : Qt::Unchecked);
            } else if (!cell.at(2).isBool() && (item->flags() & Qt::ItemIsEditable)) {
                item->setText(k, propertyValue(cell.at(2)));
            } else {
                ok = false;
                why = cell.at(2).isBool() ? tr("row %1 has no check box").arg(r) : tr("row %1's text is not one to edit").arg(r);
            }
            if (ok) tree->setCurrentItem(item, k);
        } else if (auto* lw = qobject_cast<QListWidget*>(w)) {
            // [item or row, true/false] ticks an item or not; an item alone
            // chooses it.
            const QJsonArray tick = value.toArray();
            if (value.isArray() && tick.size() == 2 && tick.at(1).isBool()) {
                QListWidgetItem* item = nullptr;
                if (tick.at(0).isDouble()) item = lw->item(tick.at(0).toInt(-1));
                else if (const QList<QListWidgetItem*> found = lw->findItems(tick.at(0).toString(), Qt::MatchFixedString); !found.isEmpty())
                    item = found.first();
                if (item == nullptr) {
                    ok = false;
                    why = rowsSaid(lw->count(), tr("list"));
                } else if (!(item->flags() & Qt::ItemIsUserCheckable)) {
                    ok = false;
                    why = tr("%1 has no check box").arg(item->text());
                } else {
                    item->setCheckState(tick.at(1).toBool() ? Qt::Checked : Qt::Unchecked);
                    lw->setCurrentItem(item);
                }
            } else {
                const QList<QListWidgetItem*> items = lw->findItems(text, Qt::MatchFixedString);
                if (items.isEmpty()) ok = false;
                else lw->setCurrentItem(items.first());
            }
        } else if (auto* b = qobject_cast<QAbstractButton*>(w)) {
            // A check box as a click would set it (its signals go).
            const bool want = value.isBool() ? value.toBool() : text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
            if (b->isCheckable() && b->isChecked() != want) b->click();
            else if (!b->isCheckable()) ok = false;
        }
        // Named as get_dialog names it: its label, else its id (a control
        // with none - a frame's field, a list in a table).
        const QString name = shownLabel(w, dialog, ui).isEmpty() ? QStringLiteral("c%1").arg(list.indexOf(w) + 1) : shownLabel(w, dialog, ui);
        if (ok) changed << (said.isEmpty() ? name : said);
        else if (why.isEmpty()) problems << tr("%1 does not take %2").arg(name, text);
        else problems << tr("%1 does not take %2: %3").arg(name, text, why);
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
    const QString name = cleanText(button->text()).isEmpty() ? button->toolTip() : cleanText(button->text());
    QTimer::singleShot(0, a_app, [target] {
        const QtFileDialogs qt;
        if (target && target->isEnabled()) target->click();
    });
    QTimer::singleShot(500, this, [this, done, report, name, was, ui] {
        QWidget* now = openDialog();
        QString after;
        if (ui) after = now == nullptr ? tr("%1 pressed.").arg(name) : tr("%1 pressed; “%2” is open now (get_dialog reads it).").arg(name, now->windowTitle());
        else if (now == nullptr) after = tr("%1 pressed; no dialog is open now.").arg(name);
        else if (now == was.data()) after = tr("%1 pressed; the dialog is still open.").arg(name);
        else after = tr("%1 pressed; “%2” is open now.").arg(name, now->windowTitle());
        done(textResult(report.isEmpty() ? after : report + QLatin1Char(' ') + after));
    });
}

// ----------------------------------------------------------------------
// Simulation

namespace {

// The schematic in \a dir whose dataset a run kept as \a keepAs would
// write over (keepAs.sch there); empty when none is.
QString keptOwner(const QDir& dir, const QString& keepAs)
{
    const QString sch = dir.absoluteFilePath(keepAs + QStringLiteral(".sch"));
    return QFileInfo::exists(sch) ? sch : QString();
}

} // namespace

// The dataset a run of \a simulator wrote for \a doc since \a before:
// its file, whether it was written, its variables, a copy kept as
// \a keepAs, the data display, and the traces that show nothing.
QJsonObject QucsControl::comparedRuns(Schematic* doc, const QJsonObject& compare, const QJsonValue& simulator)
{
    const QString with = compare.value(QLatin1String("with")).toString().trimmed();
    static const QStringList stats{QStringLiteral("min"), QStringLiteral("max"), QStringLiteral("mean"), QStringLiteral("rms"),
                                   QStringLiteral("final"), QStringLiteral("peak_to_peak")};
    QJsonArray table;
    for (const QJsonValue& item : compare.value(QLatin1String("measure")).toArray()) {
        const QJsonObject m = item.toObject();
        const QString variable = m.value(QLatin1String("variable")).toString().trimmed();
        const QString what = m.value(QLatin1String("what")).toString(QStringLiteral("final")).trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_'));
        const QString field = m.value(QLatin1String("field")).toString(QStringLiteral("value"));
        QJsonObject read{{QStringLiteral("path"), doc->getDocName()}, {QStringLiteral("variables"), QJsonArray{variable}},
                         {QStringLiteral("compare"), with}, {QStringLiteral("points"), 0}};
        if (!simulator.isUndefined() && !simulator.isNull()) read.insert(QStringLiteral("simulator"), simulator);
        for (const char* key : {"from", "to", "level", "tolerance", "fundamental", "harmonics", "periods", "decibels", "form"})
            if (m.contains(QLatin1String(key))) read.insert(QLatin1String(key), m.value(QLatin1String(key)));
        const bool stat = stats.contains(what);
        if (!stat) read.insert(QStringLiteral("measure"), QJsonArray{what});
        QJsonObject row{{QStringLiteral("variable"), variable}, {QStringLiteral("what"), what}};
        const QJsonObject got = getDataset(read);
        if (got.value(QLatin1String("isError")).toBool()) {
            row.insert(QStringLiteral("error"), textOf(got).left(300));
            table.append(row);
            continue;
        }
        QJsonObject v = QJsonDocument::fromJson(textOf(got).toUtf8()).object().value(QStringLiteral("variables")).toArray().first().toObject();
        if (v.contains(QStringLiteral("curves"))) v = v.value(QStringLiteral("curves")).toArray().first().toObject();   // (a sweep: its first curve)
        const QJsonObject other = v.value(QStringLiteral("other run")).toObject();
        const QString statKey = QString(what).replace(QLatin1Char('_'), QLatin1Char(' '));
        const QJsonValue after = stat ? v.value(statKey) : v.value(QStringLiteral("measurements")).toObject().value(what).toObject().value(field);
        const QJsonValue before = stat ? other.value(statKey) : other.value(QStringLiteral("measurements")).toObject().value(what).toObject().value(field);
        if (after.isDouble()) row.insert(QStringLiteral("after"), after);
        if (before.isDouble()) row.insert(QStringLiteral("before"), before);
        if (after.isDouble() && before.isDouble()) {
            const double a = after.toDouble(), b = before.toDouble();
            row.insert(QStringLiteral("change"), a - b);
            if (b != 0) row.insert(QStringLiteral("change %"), std::round((a - b) / std::abs(b) * 10000.0) / 100.0);
        } else {
            // Why not: the run it failed on, and its error - or the field
            // not among those it gives.
            const QJsonObject measuredAfter = v.value(QStringLiteral("measurements")).toObject().value(what).toObject();
            const QJsonObject measuredBefore = other.value(QStringLiteral("measurements")).toObject().value(what).toObject();
            QString note;
            if (v.contains(QStringLiteral("compared"))) note = v.value(QStringLiteral("compared")).toString();
            else if (!stat && measuredAfter.contains(QStringLiteral("error")))
                note = tr("not measured on this run: %1").arg(measuredAfter.value(QStringLiteral("error")).toString());
            else if (!stat && measuredBefore.contains(QStringLiteral("error")))
                note = tr("not measured on %1: %2").arg(with, measuredBefore.value(QStringLiteral("error")).toString());
            else if (!stat && !measuredAfter.isEmpty() && !measuredAfter.contains(field))
                note = tr("%1 gives no '%2': 'field' is one of %3").arg(what, field, measuredAfter.keys().join(QStringLiteral(", ")));
            else note = tr("not measured on both runs (%1)").arg(field);
            row.insert(QStringLiteral("note"), note);
        }
        table.append(row);
    }
    return QJsonObject{{QStringLiteral("with"), with}, {QStringLiteral("table"), table}};
}

QJsonObject QucsControl::datasetOfRun(Schematic* doc, int simulator, const QDateTime& before, const QString& keepAs,
                                      bool* written)
{
    QJsonObject result;
    const QFileInfo info(doc->getDocName());
    const QFileInfo dataset(datasetFile(info.absoluteFilePath(), doc->getDataSet(), simulator));
    // (Newer than it was: a file made in the second the run began, and not
    // written by it, read as written when the time was all compared.)
    *written = dataset.isFile() && (!before.isValid() || dataset.lastModified() > before);
    result.insert(QStringLiteral("dataset"), QDir::toNativeSeparators(dataset.absoluteFilePath()));
    result.insert(QStringLiteral("dataset written"), *written);
    if (*written) {
        // What it holds (nothing, when the simulator made no output).
        qucs_s::dataset::Dataset data;
        QJsonArray names;
        int count = 0;
        if (data.read(dataset.absoluteFilePath()))
            for (const auto& v : data.variables())
                if (!v.independent && ++count <= 40) names.append(v.name);
        // The first 40 by name, and how many there are (a list cut at 40
        // read as 40).
        if (count > names.size()) names.append(tr("... %1 more").arg(count - names.size()));
        result.insert(QStringLiteral("variables"), names);
        result.insert(QStringLiteral("variable count"), count);
        // A copy to compare with later runs: name.dat.ngspice.
        if (!keepAs.isEmpty()) {
            const QString suffix = dataset.fileName().mid(dataset.fileName().indexOf(QLatin1String(".dat")));
            const QString kept = info.absoluteDir().filePath(keepAs + suffix);
            QString why;
            if (misc::isSameFile(kept, dataset.absoluteFilePath())) {
                // keep_as the schematic's own name (amp, or Amp on macOS):
                // that is the dataset itself, which a copy would destroy.
                result.insert(QStringLiteral("kept as"), tr("not kept: %1 is the dataset of this run itself - give keep_as another name")
                                                             .arg(QDir::toNativeSeparators(kept)));
            } else if (const QString owner = keptOwner(info.absoluteDir(), keepAs); !owner.isEmpty()) {
                result.insert(QStringLiteral("kept as"), tr("not kept: %1 is the dataset of %2 - give keep_as another name")
                                                             .arg(QDir::toNativeSeparators(kept), owner));
            } else if (misc::copyFileOver(dataset.absoluteFilePath(), kept, &why)) {
                const QString prefix = simulatorPrefix();
                result.insert(QStringLiteral("kept as"), QDir::toNativeSeparators(kept));
                result.insert(QStringLiteral("its traces"), (prefix.isEmpty() ? QString() : prefix + QLatin1Char('/'))
                                                                + keepAs + QStringLiteral(":<variable>"));
            } else {
                result.insert(QStringLiteral("kept as"), tr("not kept: %1").arg(why));
            }
        }
    }
    result.insert(QStringLiteral("data display"), QDir::toNativeSeparators(info.absoluteDir().filePath(doc->getDataDisplay())));
    // The traces that show nothing (the documents are reloaded by now).
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
    return result;
}

// untitled.sch, untitled-2.sch ... in the scratch folder: its netlist and
// dataset go beside it, away from the user's files.
bool QucsControl::saveInScratch(Schematic* sch, QString* note, QString* error)
{
    const QString title = titleOf(sch);
    const QDir dir(misc::projectScratch(QucsSettings.QucsWorkDir.absolutePath()));
    if (!dir.mkpath(QStringLiteral("."))) {
        *error = tr("%1 has no file yet, and the scratch folder %2 could not be made: save_document with 'as' first.")
                     .arg(title, QDir::toNativeSeparators(dir.absolutePath()));
        return false;
    }
    QString file;
    for (int n = 1; file.isEmpty() && n < 10000; ++n) {
        const QString candidate = dir.absoluteFilePath(n == 1 ? QStringLiteral("untitled.sch") : QStringLiteral("untitled-%1.sch").arg(n));
        bool open = false;
        for (QucsDoc* d : a_app->allDocuments()) open = open || (!d->getDocName().isEmpty() && sameFile(d->getDocName(), candidate));
        if (!open && !QFileInfo::exists(candidate)) file = candidate;
    }
    if (file.isEmpty() || !a_app->saveDocumentAs(sch, file)) {
        *error = tr("%1 has no file yet, and it could not be saved in the scratch folder: save_document with 'as' first.").arg(title);
        return false;
    }
    // (Not one of the user's Recent Documents: they filled up with these.)
    a_app->forgetRecentFile(sch->getDocName());
    *note = tr("%1 had no file: it is saved as %2, in the scratch folder, to be simulated (its netlist and dataset go beside it). "
               "save_document with 'as' puts it where it belongs.")
                .arg(title, QDir::toNativeSeparators(file));
    return true;
}

namespace {

// For ngspice's "Unable to find definition of model X" when X's .model
// card names a type no built-in device has - a Verilog-A module whose
// compiled library was not loaded: where Qucs-S looked for it (the open
// project, the schematic's folder) and where it is, if anywhere it can
// see. Empty for another error, or a module it did load. Older ngspice
// (42) says "could not find a valid modelname" without the name: it is
// a word of the device's netlist line.
QString osdiHint(const QString& message, const QString& netlistLine, const QStringList& netlist, const QString& schematic,
                 const QString& project)
{
    static const QRegularExpression unfound(QStringLiteral("unable to find definition of model\\s+([^\\s:]+)"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression unnamed(QStringLiteral("could not find a valid modelname"), QRegularExpression::CaseInsensitiveOption);
    QStringList models;
    if (const QRegularExpressionMatch m = unfound.match(message); m.hasMatch()) models << m.captured(1);
    else if (unnamed.match(message).hasMatch())
        for (const QString& word : netlistLine.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).mid(1))
            if (!word.contains(QLatin1Char('='))) models << word;
    // Its card: .model amp_model amp (gain=10).
    QString model, type;
    for (const QString& name : std::as_const(models)) {
        const QRegularExpression card(QStringLiteral("^\\s*\\.model\\s+%1\\s+([A-Za-z_][\\w$]*)").arg(QRegularExpression::escape(name)),
                                      QRegularExpression::CaseInsensitiveOption);
        for (const QString& line : netlist)
            if (const QRegularExpressionMatch c = card.match(line); c.hasMatch()) {
                model = name;
                type = c.captured(1);
                break;
            }
        if (!type.isEmpty()) break;
    }
    static const QSet<QString> builtIn{"r", "c", "l", "d", "npn", "pnp", "njf", "pjf", "nmos", "pmos", "nmf", "pmf", "sw", "csw",
                                       "ltra", "urc", "vdmos", "hfet", "tra", "txl", "cpl", "isource", "vsource", "res", "cap", "ind"};
    if (type.isEmpty() || builtIn.contains(type.toLower())) return {};
    // Who defines it: an .osdi's module or a .va's.
    const auto defines = [&type](const QString& file) {
        QStringList names;
        if (file.endsWith(QLatin1String(".osdi"), Qt::CaseInsensitive)) {
            if (!qucs_s::vamodule::osdiModules(file, &names)) return false;
        } else {
            QFile f(file);
            if (!f.open(QIODevice::ReadOnly)) return false;
            names = qucs_s::vamodule::sourceModules(QString::fromUtf8(f.readAll()));
        }
        return std::any_of(names.cbegin(), names.cend(), [&](const QString& n) { return n.compare(type, Qt::CaseInsensitive) == 0; });
    };
    const QStringList patterns{QStringLiteral("*.va"), QStringLiteral("*.osdi")};
    const QDir folder = QFileInfo(schematic).absoluteDir();
    QStringList looked;   // where Qucs-S loads them from
    if (!project.isEmpty()) {
        looked << QDir(project).absolutePath();
        for (const QString& f : misc::projectFiles(QDir(project), patterns))
            if (defines(QDir(project).absoluteFilePath(f))) return {};   // (loaded: another cause)
    }
    for (const QString& f : folder.entryList(patterns, QDir::Files))
        if (defines(folder.absoluteFilePath(f))) return {};
    const QString where = project.isEmpty()
        ? QObject::tr("no project is open, so only the .va and .osdi files beside the schematic are (%1)").arg(QDir::toNativeSeparators(folder.absolutePath()))
        : QObject::tr("those are the open project's and the ones beside the schematic");
    // Elsewhere in the workspace?
    const QDir workspace(QucsSettings.qucsWorkspaceDir);
    QStringList elsewhere;
    int read = 0;
    for (const QString& f : misc::projectFiles(workspace, patterns)) {
        if (++read > 400) break;
        const QString file = workspace.absoluteFilePath(f);
        if (defines(file))
            elsewhere << QDir::toNativeSeparators(file.startsWith(folder.absolutePath() + QLatin1Char('/')) ? folder.relativeFilePath(file) : file);
        if (elsewhere.size() >= 3) break;
    }
    if (!elsewhere.isEmpty())
        return QObject::tr("Qucs-S: %1's type %2 is a Verilog-A module, defined in %3 - not loaded: %4. Put it there (build_verilog_a "
                           "compiles a .va), or open its project.")
            .arg(model, type, elsewhere.join(QObject::tr(" and ")), where);
    return QObject::tr("Qucs-S: no .va or .osdi it can see defines a module %1 (%2's type), so none was loaded: %3. Write the module "
                       "to a .va file there - build_verilog_a compiles it.")
        .arg(type, model, where);
}

} // namespace

namespace {

// A run's answer at its timeout: it goes on, followed by \a id.
QString stillRunning(int timeout, int id)
{
    return QucsControl::tr("Still running after %1 s: it goes on. simulation_status {\"id\": %2} says how it goes and gives its "
                           "outcome once it has ended; wait_for {\"event\": \"simulation_finished\", \"id\": %2} waits for its end; "
                           "stop_simulation stops it.").arg(timeout / 1000).arg(id);
}

} // namespace

void QucsControl::simulate(const QJsonObject& args, const Done& given)
{
    // Followed by an id - begun with 'background', or still running at its
    // 'timeout' -: its outcome kept (simulation_status, wait_for) once it
    // has ended, not answered (the answer went).
    const bool background = args.value(QLatin1String("background")).toBool();
    auto followed = std::make_shared<int>(0);
    const Done doneGiven = [this, given, followed](const QJsonObject& r) {
        if (*followed == 0) given(r);
        else endSimRun(*followed, r);
    };
    QString error;
    Schematic* sch = schematic(args, &error, false);
    // A schematic not open: opened first, as open_document opens it (a run
    // is of a document in its tab), and said.
    QString openedNote;
    if (sch == nullptr) {
        const QString path = args.value(QLatin1String("path")).toString().trimmed();
        const QString file = path.isEmpty() ? QString() : absolute(path);
        if (!file.isEmpty() && QFileInfo(file).isFile() && QFileInfo(file).suffix().compare(QLatin1String("sch"), Qt::CaseInsensitive) == 0
            && a_app->findDoc(file) == nullptr) {
            const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), file}});
            if (opened.value(QLatin1String("isError")).toBool()) {
                doneGiven(opened);
                return;
            }
            error.clear();
            sch = schematic(QJsonObject{{QStringLiteral("path"), file}}, &error, false);
            openedNote = tr("%1 was not open: it is open now, in a tab of its own.").arg(QFileInfo(file).fileName());
        }
    }
    if (sch == nullptr) {
        doneGiven(errorResult(error));
        return;
    }
    // Commands a run executes besides the simulator: not run unasked.
    if (const QString refused = commandsRefused(sch, args); !refused.isEmpty()) {
        doneGiven(errorResult(refused));
        return;
    }
    SimulationConsole* console = a_app->simulationConsole();
    if (console == nullptr || console->isRunning() || !a_app->findChildren<SimMessage*>().isEmpty()) {
        bool busy = console == nullptr || console->isRunning();
        for (SimMessage* m : a_app->findChildren<SimMessage*>()) busy = busy || m->SimProcess.state() != QProcess::NotRunning;
        if (busy) {
            doneGiven(errorResult(tr("A simulation is running already.")));
            return;
        }
    }
    // A simulator for this run alone ('simulator'): the setting is put back
    // when the run has ended.
    int simulator = QucsSettings.DefaultSimulator;
    const QString oneOff = args.value(QLatin1String("simulator")).toString().trimmed().toLower().remove(QLatin1Char(' '));
    if (!oneOff.isEmpty()) {
        static const QHash<QString, int> known{{QStringLiteral("ngspice"), spicecompat::simNgspice},
                                               {QStringLiteral("xyce"), spicecompat::simXyce},
                                               {QStringLiteral("spiceopus"), spicecompat::simSpiceOpus},
                                               {QStringLiteral("qucsator"), spicecompat::simQucsator}};
        if (!known.contains(oneOff)) {
            doneGiven(errorResult(tr("'simulator' is ngspice, xyce, spiceopus or qucsator.")));
            return;
        }
        simulator = known.value(oneOff);
        bool installed = false;
        QStringList names;
        if (QComboBox* list = a_app->simulatorList())
            for (int i = 0; i < list->count(); ++i) {
                installed = installed || list->itemData(i).toInt() == simulator;
                names << list->itemText(i);
            }
        if (!installed) {
            doneGiven(errorResult(tr("%1 is not installed, or Qucs-S does not know where it is. Installed: %2.")
                                      .arg(oneOff, names.isEmpty() ? tr("none") : names.join(QStringLiteral(", ")))));
            return;
        }
    }
    if (simulator == spicecompat::simNotSpecified) {
        doneGiven(errorResult(noSimulatorText()));
        return;
    }
    a_app->showDocument(sch);
    // The operating point alone: a DC bias run (Simulation > Calculate DC
    // bias), whatever analyses the schematic has - a transient-only one's
    // too; its datasets are left as they are.
    const bool operatingPoint = args.value(QLatin1String("operating_point")).toBool();
    if (operatingPoint && (simulator == spicecompat::simQucsator || sch->isDigitalCircuit())) {
        doneGiven(errorResult(sch->isDigitalCircuit() ? tr("A digital schematic has no operating point to run.")
                                                      : tr("The operating point alone is run with a SPICE simulator (simulator: ngspice).")));
        return;
    }
    // No analysis, nothing to run: said so, not "Failed to start simulator:
    // Unknown error" (the reason came only among the check's warnings).
    if (!operatingPoint
        && std::none_of(sch->a_DocComps.cbegin(), sch->a_DocComps.cend(), [](const Component* c) {
               return c->isActive == COMP_IS_ACTIVE && c->Model.startsWith(QLatin1Char('.'));
           })) {
        doneGiven(errorResult(tr("%1 has no analysis to run (no .AC, .TR, .DC, .SP ... block): add_analysis adds one - or simulate "
                                 "with operating_point for the DC operating point alone.").arg(titleOf(sch))));
        return;
    }
    // (In the background: no limit unless given - then it is stopped at it.)
    const int timeout = background ? (args.contains(QLatin1String("timeout")) ? std::clamp(args.value(QLatin1String("timeout")).toInt(), 5, 7 * 86400) * 1000 : 0)
                                   : std::clamp(args.value(QLatin1String("timeout")).toInt(120), 5, 3600) * 1000;
    const QString keepAs = args.value(QLatin1String("keep_as")).toString().trimmed();
    if (!keepAs.isEmpty() && !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,64}$")).match(keepAs).hasMatch()) {
        doneGiven(errorResult(tr("'keep_as' is a name of letters, digits, _ and -.")));
        return;
    }
    // Before and after: what is measured on this run and on one kept
    // before (keep_as), side by side - "did it improve" in one call.
    const QJsonObject compare = args.value(QLatin1String("compare")).toObject();
    if (args.contains(QLatin1String("compare"))) {
        const QJsonArray items = compare.value(QLatin1String("measure")).toArray();
        bool readable = !compare.value(QLatin1String("with")).toString().trimmed().isEmpty() && !items.isEmpty() && items.size() <= 12;
        for (const QJsonValue& v : items) readable = readable && !v.toObject().value(QLatin1String("variable")).toString().trimmed().isEmpty();
        if (!readable || operatingPoint) {
            doneGiven(errorResult(operatingPoint ? tr("'compare' is of an analysis run's dataset, not of the operating point alone.")
                                                 : tr("'compare' is {\"with\": \"before\", \"measure\": [{\"variable\": \"ac.v(out)\", \"what\": "
                                                      "\"bandwidth\"}, ...]}: a run kept with keep_as, and up to 12 measurements as tune's "
                                                      "'measure' takes them.")));
            return;
        }
    }
    const bool brief = args.value(QLatin1String("brief")).toBool();
    // What Check Schematic finds, before the run - the moment it matters:
    // put first in the answer.
    QJsonArray checkErrors, checkWarnings;
    for (const auto& i : qucs_s::erc::check(sch))
        (i.severity == qucs_s::erc::Severity::Error ? checkErrors : checkWarnings).append(issueJson(i));
    QString checkText;
    if (!checkErrors.isEmpty() || !checkWarnings.isEmpty()) {
        QStringList found;
        for (const QJsonValue& v : checkErrors) found << tr("error: %1").arg(v.toObject().value(QLatin1String("message")).toString());
        for (const QJsonValue& v : checkWarnings) found << tr("warning: %1").arg(v.toObject().value(QLatin1String("message")).toString());
        if (found.size() > 12) found = found.mid(0, 12) << tr("... (check_schematic lists them all)");
        checkText = tr("Check Schematic, before the run: %1").arg(found.join(QStringLiteral("; ")));
    }
    // Untitled: saved in the scratch folder first, and said.
    QString savedNote;
    if (sch->getDocName().isEmpty() && !saveInScratch(sch, &savedNote, &error)) {
        doneGiven(errorResult(error));
        return;
    }
    // Not the name of a dataset imported beside it: a trace of keepAs:x is
    // the import's (no simulator's prefix), so the kept run could not be
    // named, nor chosen in the Data tab.
    if (!keepAs.isEmpty()) {
        qucs_s::dataimport::Origin origin;
        if (qucs_s::dataimport::originOf(QFileInfo(sch->getDocName()).absoluteDir().filePath(keepAs + QStringLiteral(".dat")), &origin)) {
            doneGiven(errorResult(tr("'keep_as' %1 is the name of a dataset imported from %2: its traces are %1:variable, so the "
                                     "kept run could not be named. Give it another name (run1, %1-run).")
                                      .arg(keepAs, QDir::toNativeSeparators(origin.source))));
            return;
        }
    }
    // Not over another schematic's dataset: keep_as rc beside rc.sch
    // replaced its results with this run's, and its diagrams showed them.
    if (!keepAs.isEmpty())
        if (const QString owner = keptOwner(QFileInfo(sch->getDocName()).absoluteDir(), keepAs); !owner.isEmpty()
            && !sameFile(owner, sch->getDocName())) {
            doneGiven(errorResult(tr("'keep_as' %1 would write over the dataset of %2: give it another name (run1, %1-run).")
                                      .arg(keepAs, QDir::toNativeSeparators(owner))));
            return;
        }
    const int previous = QucsSettings.DefaultSimulator;
    QucsSettings.DefaultSimulator = simulator;
    auto restored = std::make_shared<bool>(simulator == previous);
    const auto restore = [restored, previous] {
        if (*restored) return;
        *restored = true;
        QucsSettings.DefaultSimulator = previous;
    };
    const QPointer<Schematic> checked(sch);
    const Done done = [this, checked, doneGiven, checkText, checkErrors, checkWarnings, oneOff, savedNote, openedNote](const QJsonObject& r) {
        QJsonObject result = r;
        QJsonArray content = result.value(QStringLiteral("content")).toArray();
        // Into the report itself, too.
        bool savedSaid = savedNote.isEmpty();
        if (content.size() == 1 && !result.value(QStringLiteral("isError")).toBool()) {
            QJsonObject report = QJsonDocument::fromJson(content.at(0).toObject().value(QStringLiteral("text")).toString().toUtf8()).object();
            if (!report.isEmpty()) {
                if (!savedSaid) report.insert(QStringLiteral("saved"), savedNote);   // (where the file now is)
                if (!openedNote.isEmpty()) report.insert(QStringLiteral("opened"), openedNote);
                savedSaid = true;
                const bool failed = report.contains(QStringLiteral("succeeded")) && !report.value(QStringLiteral("succeeded")).toBool();
                // A run that failed: the errors of its subcircuits too - two
                // sources in parallel inside one, which ngspice names by a
                // node alone (v.x1.vx#branch). (Not before every run: a
                // hierarchy is read from disk for it.)
                QJsonArray subErrors;
                QStringList subText;
                if (failed && checked) {
                    const auto open = [this](const QString& file) { return dynamic_cast<Schematic*>(a_app->findDoc(file)); };
                    for (const auto& sub : qucs_s::erc::checkSubcircuits(checked, open))
                        for (const auto& i : sub.issues) {
                            if (i.severity != qucs_s::erc::Severity::Error) continue;
                            QJsonObject o = issueJson(i);
                            o.insert(QStringLiteral("file"), QDir::toNativeSeparators(sub.file));
                            subErrors.append(o);
                            subText << tr("error in %1: %2").arg(QFileInfo(sub.file).fileName(), i.message);
                        }
                }
                if (!checkErrors.isEmpty() || !checkWarnings.isEmpty() || !subErrors.isEmpty()) {
                    // (Named to come first in the answer, which lists its
                    // fields in order of their names.)
                    QJsonObject before{{QStringLiteral("errors"), checkErrors}, {QStringLiteral("warnings"), checkWarnings}};
                    if (!subErrors.isEmpty()) before.insert(QStringLiteral("errors in its subcircuits"), subErrors);
                    report.insert(QStringLiteral("before the run"), before);
                    // A run that failed: the cause may be among them - a
                    // part's pin connected to nothing, not ngspice's
                    // "argument out of range for db" it led to. First in its
                    // errors: the check's errors, and its warnings of a part
                    // (not a loose wire end's), and its subcircuits' errors.
                    if (failed) {
                        QJsonArray errors;
                        const auto take = [&errors](QJsonObject o) {
                            o.insert(QStringLiteral("found"), o.contains(QStringLiteral("file")) ? tr("by Check Schematic, in a subcircuit")
                                                                                              : tr("before the run, by Check Schematic"));
                            errors.append(o);
                        };
                        for (const QJsonValue& v : checkErrors) take(v.toObject());
                        for (const QJsonValue& v : checkWarnings)
                            if (v.toObject().contains(QStringLiteral("component"))) take(v.toObject());
                        for (const QJsonValue& v : std::as_const(subErrors)) take(v.toObject());
                        for (const QJsonValue& v : report.value(QStringLiteral("errors")).toArray()) errors.append(v);
                        report.insert(QStringLiteral("errors"), errors);
                    }
                    // The run's log begins with them: where they are read.
                    QString said = checkText;
                    if (!subText.isEmpty())
                        said += (said.isEmpty() ? QString() : QStringLiteral("\n")) + tr("Check Schematic, of its subcircuits: %1").arg(subText.mid(0, 12).join(QStringLiteral("; ")));
                    report.insert(QStringLiteral("last lines"), said + QStringLiteral("\n\n") + report.value(QStringLiteral("last lines")).toString());
                }
                if (!oneOff.isEmpty())
                    report.insert(QStringLiteral("simulator in the settings"),
                                  tr("unchanged: %1 ran for this run alone (get_dataset with 'simulator' reads its dataset; the "
                                     "diagrams show the simulator's in the settings)").arg(oneOff));
                content = QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                 {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Compact))}}};
            }
        }
        else if (!checkText.isEmpty())   // (an error: told after it)
            content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), checkText}});
        if (!savedSaid) content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), savedNote}});
        result.insert(QStringLiteral("content"), content);
        doneGiven(result);
    };

    QPointer<Schematic> doc(sch);
    const QString title = titleOf(sch);
    // The dataset as it is now: newer after the run, the simulator did
    // produce something.
    QDateTime started;
    {
        const QFileInfo info(sch->getDocName());
        const QFileInfo dataset(datasetFile(info.absoluteFilePath(), sch->getDataSet(), simulator));
        if (dataset.isFile()) started = dataset.lastModified();
    }
    // What it simulates: the schematic at this revision (an edit while it
    // runs is not in its results).
    const quint64 revision = sch->revision();
    const quint64 caller = a_callers.isEmpty() ? 0 : a_callers.last();

    // What was edited while it ran - the user, another conversation: its
    // results are of the schematic as it was when it began.
    const auto changedWhileRunning = [this, doc, revision, caller](QJsonObject& result) {
        if (!doc || doc->revision() == revision) return;
        QHash<QString, int> by;
        QDateTime last;
        for (const QucsDoc::Edit& e : doc->recentEdits())
            if (e.revision > revision && e.by != QucsDoc::kSimulation) {   // (the run's own: an optimizer's result, its answer's 'optimum')
                by[whoMade(e.by, caller)]++;
                last = e.at;
            }
        if (by.isEmpty()) return;
        QStringList who;
        for (auto it = by.cbegin(); it != by.cend(); ++it) who << tr("%1 by %2").arg(it.value()).arg(it.key());
        result.insert(QStringLiteral("changed while it ran"),
                      tr("%1 was changed while the simulation ran - %2 edit(s)%3, revision %4 to %5: its results are of the "
                         "schematic as it was when the run began, not as it is now. Simulate again for the schematic as it is.")
                          .arg(titleOf(doc), who.isEmpty() ? tr("some") : who.join(QStringLiteral(", ")),
                               last.isValid() ? tr(", the last at %1").arg(last.toString(QStringLiteral("HH:mm:ss"))) : QString())
                          .arg(revision).arg(doc->revision()));
    };

    const QString simulatorName = spicecompat::getDefaultSimulatorName(simulator);
    // In the background: answered now, with its id.
    if (background) {
        *followed = beginSimRun(sch->getDocName(), simulatorName);
        QJsonObject begun{{QStringLiteral("id"), *followed},
                          {QStringLiteral("running"), true},
                          {QStringLiteral("schematic"), QDir::toNativeSeparators(sch->getDocName())},
                          {QStringLiteral("simulator"), simulatorName},
                          {QStringLiteral("note"), tr("It runs in the background%1: simulation_status {\"id\": %2} says how it goes, and gives "
                                                      "its outcome once it has ended (as simulate would have); wait_for {\"event\": "
                                                      "\"simulation_finished\", \"id\": %2} waits for its end; stop_simulation stops it.")
                                                       .arg(timeout > 0 ? tr(", stopped after %1 s").arg(timeout / 1000) : QString())
                                                       .arg(*followed)}};
        if (!checkText.isEmpty()) begun.insert(QStringLiteral("before the run"), checkText);
        if (!savedNote.isEmpty()) begun.insert(QStringLiteral("saved"), savedNote);
        if (!openedNote.isEmpty()) begun.insert(QStringLiteral("opened"), openedNote);
        given(jsonResult(begun));
    }
    // Past its timeout, not in the background: answered, and followed from
    // then on by an id (it goes on). In the background: stopped.
    const auto pastTimeout = [this, followed, background, timeout, path = sch->getDocName(), simulatorName](QObject* process) -> int {
        if (background) {
            if (SimRun* r = simRun(*followed)) {
                r->stoppedBy = tr("its timeout of %1 s").arg(timeout / 1000);
                stopProcess(process);
            }
            return 0;
        }
        const int id = beginSimRun(path, simulatorName);
        if (SimRun* r = simRun(id)) r->process = process;
        return id;
    };

    if (simulator == spicecompat::simQucsator) {
        // Qucsator runs in a window of its own (SimMessage): its end waited
        // for as a SPICE run's is.
        const QList<SimMessage*> before = a_app->findChildren<SimMessage*>();
        // (Its own schematic, in front: closed meanwhile, none - not the
        // document then in front.)
        QTimer::singleShot(0, a_app, [app = a_app, doc] {
            if (!doc) return;
            app->showDocument(doc);
            app->slotSimulate();
        });
        QTimer::singleShot(0, this, [=, this] {
            SimMessage* sim = nullptr;
            for (SimMessage* m : a_app->findChildren<SimMessage*>())
                if (!before.contains(m)) sim = m;
            if (sim == nullptr) {
                restore();
                done(errorResult(tr("The simulation with Qucsator did not start.")));
                return;
            }
            if (SimRun* r = simRun(*followed)) r->process = sim;
            // Answered at its end - or at its timeout, and its end then kept
            // under the id the answer gave.
            auto answered = std::make_shared<int>(0);   // 1: at its timeout, 2: at its end
            QPointer<SimMessage> message(sim);
            auto report = [=, this](int status, bool timedOut, int id) {
                if (*answered == 2 || (*answered == 1 && timedOut)) return;
                *answered = timedOut ? 1 : 2;
                if (!timedOut) restore();
                if (!doc) {
                    done(errorResult(tr("%1 was closed while it was simulated: its results were discarded.").arg(title)));
                    return;
                }
                QJsonObject result{{QStringLiteral("finished"), !timedOut}, {QStringLiteral("simulator"), QStringLiteral("Qucsator")}};
                if (message) {
                    QStringList out = message->ProgText->toPlainText().split(QLatin1Char('\n'));
                    while (!out.isEmpty() && out.last().trimmed().isEmpty()) out.removeLast();
                    result.insert(QStringLiteral("last lines"), out.mid(std::max<qsizetype>(0, out.size() - 40)).join(QLatin1Char('\n')));
                    QJsonArray problems;
                    for (const QString& line : message->ErrText->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                        if (problems.size() < 40) problems.append(QJsonObject{{QStringLiteral("message"), line.trimmed()}});
                    result.insert(status == 0 ? QStringLiteral("warnings") : QStringLiteral("errors"), problems);
                }
                bool written = false;
                if (doc && !timedOut) result = mergedJson(result, datasetOfRun(doc, spicecompat::simQucsator, started, keepAs, &written));
                if (!timedOut) {
                    result.insert(QStringLiteral("succeeded"), status == 0);
                    if (status == 0 && !written)
                        result.insert(QStringLiteral("note"), tr("Qucsator reported no error but wrote no new dataset."));
                } else {
                    result.insert(QStringLiteral("note"), stillRunning(timeout, id));
                }
                changedWhileRunning(result);
                done(jsonResult(result));
            };
            connect(sim, &SimMessage::SimulationEnded, this, [report](int status, SimMessage*) { report(status, false, 0); });
            // Ended already (it could not start).
            if (sim->SimProcess.state() == QProcess::NotRunning && !sim->ErrText->toPlainText().trimmed().isEmpty())
                QTimer::singleShot(0, this, [report] { report(1, false, 0); });
            if (timeout > 0)
                QTimer::singleShot(timeout, this, [report, answered, followed, pastTimeout, sim = QPointer<SimMessage>(sim), restore] {
                    if (*answered != 0) return;
                    const int id = pastTimeout(sim);
                    if (id == 0) return;   // (in the background: stopped, its end reported)
                    report(0, true, id);
                    *followed = id;   // (its end, from now on, kept under it)
                    // Put back when it does end.
                    if (sim) connect(sim, &SimMessage::SimulationEnded, sim, [restore] { restore(); });
                    else restore();
                });
        });
        return;
    }

    // (A DC bias run is one whose schematic's bias is 0 when it starts;
    // any other value, a run of its analyses.)
    if (operatingPoint) sch->setShowBias(0);
    else if (sch->getShowBias() == 0) sch->setShowBias(-1);
    // Started from the event loop (the legacy window runs one of its
    // own); the run it made is watched after.
    const int logBefore = console->statusLog()->count();
    // (Its own schematic, in front: one closed meanwhile is not run, nor the
    // document then in front in its place - its log came back as this one's.)
    QTimer::singleShot(0, a_app, [app = a_app, doc] {
        if (!doc) return;
        app->showDocument(doc);
        app->slotSimulateWithSpice();
    });
    QTimer::singleShot(0, this, [this, done, timeout, doc, title, console, started, simulator, keepAs, operatingPoint, restore, changedWhileRunning, logBefore,
                                 compare, brief, followed, pastTimeout, caller, simulatorArg = args.value(QLatin1String("simulator")),
                                 applyOptimum = args.value(QLatin1String("apply_optimum")).toBool()] {
        SimulationRun* run = console->currentRun();
        // Closed before it began (a call right behind this one): said -
        // not "did not start" with no reason, nor another run's log.
        if (!doc) {
            restore();
            done(errorResult(tr("%1 was closed before its simulation began: nothing was simulated.").arg(title)));
            return;
        }
        if (run == nullptr) {
            restore();
            // What this attempt wrote (not the runs' before it).
            QStringList log;
            for (int i = std::min(logBefore, console->statusLog()->count()); i < console->statusLog()->count(); ++i)
                log << console->statusLog()->item(i)->text();
            if (doc && doc->getShowBias() == 0) doc->setShowBias(-1);   // (the next run is its analyses)
            done(errorResult(tr("The simulation did not start. %1").arg(log.join(QLatin1Char('\n')))));
            return;
        }
        run->setQuiet(true);   // (its errors in the answer, not in a box)
        if (SimRun* f = simRun(*followed)) f->process = run;
        // Answered at its end - or at its timeout, and its end then kept
        // under the id the answer gave.
        auto answered = std::make_shared<int>(0);   // 1: at its timeout, 2: at its end
        auto report = [this, done, answered, doc, title, console, started, simulator, keepAs, operatingPoint, restore, changedWhileRunning,
                       compare, brief, simulatorArg, applyOptimum, timeout, caller](SimulationRun* r, bool timedOut, int id) {
            if (*answered == 2 || (*answered == 1 && timedOut)) return;
            *answered = timedOut ? 1 : 2;
            if (!timedOut) restore();
            if (!doc) {
                done(errorResult(tr("%1 was closed while it was simulated: its results were discarded.").arg(title)));
                return;
            }
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
                if (doc && simulator == spicecompat::simNgspice)
                    if (const QString hint = osdiHint(o.value(QStringLiteral("message")).toString(),
                                                      o.value(QStringLiteral("netlist line")).toString(), netlist, doc->getDocName(),
                                                      a_app->ProjName.isEmpty() ? QString() : QucsSettings.QucsWorkDir.absolutePath());
                        !hint.isEmpty())
                        o.insert(QStringLiteral("hint"), hint);
                const bool isError = o.take(QStringLiteral("severity")).toString() == QLatin1String("error");
                QJsonArray& list = isError ? errors : warnings;
                if (list.size() < 40) list.append(o);
            }
            result.insert(QStringLiteral("errors"), errors);
            result.insert(QStringLiteral("warnings"), warnings);
            bool written = false;
            if (doc && operatingPoint) {
                // A DC bias run: its node values and each device's quantities.
                const QJsonObject op = operatingPointOfRun(doc, misc::scratchDirFor(doc->getDocName()));
                if (!op.isEmpty()) result.insert(QStringLiteral("operating point"), op);
                result.insert(QStringLiteral("analysis"), tr("the DC operating point alone (as Simulation > Calculate DC bias): the "
                                                             "schematic shows its bias now; its datasets are as they were - simulate "
                                                             "without operating_point runs its analyses"));
                if (doc->getShowBias() == 0) doc->setShowBias(-1);   // (not shown: the next run is its analyses)
            }
            if (doc && !operatingPoint) result = mergedJson(result, datasetOfRun(doc, simulator, started, keepAs, &written));
            // Not written (a read-only file or folder): why, in the answer
            // - no box waits for a click.
            if (r != nullptr && !r->datasetError().isEmpty()) {
                written = false;
                result.insert(QStringLiteral("dataset written"), false);
                result.remove(QStringLiteral("variables"));
                errors.append(QJsonObject{{QStringLiteral("message"), r->datasetError()}});
                result.insert(QStringLiteral("errors"), errors);
            }
            if (r != nullptr && !timedOut) {
                // The simulator's own word: it ran to its end, reported no
                // error and exited so (the dataset's name is its affair).
                const bool succeeded = r->wasSimulated() && !r->hasError() && r->exitCode() == 0;
                result.insert(QStringLiteral("succeeded"), succeeded);
                // A DC bias run that failed shows no bias (the devices of the
                // run before were forgotten as it started).
                if (operatingPoint && !succeeded)
                    result.insert(QStringLiteral("analysis"), tr("the DC operating point alone (as Simulation > Calculate DC "
                                                                 "bias): it failed, so the schematic shows no bias and there "
                                                                 "is no operating point to read; its datasets are as they were"));
                result.insert(QStringLiteral("stopped"), r->wasStopped());
                result.insert(QStringLiteral("exit code"), r->exitCode());
                // (Stopped - by the user, stop_simulation, a timeout - is no
                // crash: its exit code is the stop's. An exit code comes
                // before having written no results: it says why.)
                if (r->exitCode() != 0 && (errors.isEmpty() || r->wroteNoResults()) && !r->wasStopped())
                    errors.prepend(QJsonObject{{QStringLiteral("message"),
                                                r->exitCode() < 0 ? tr("The simulator crashed or did not start.")
                                                                  : tr("The simulator ended with exit code %1.").arg(r->exitCode())}});
                if (r->wasStopped())
                    result.insert(QStringLiteral("note"), tr("Stopped before it ended: its dataset is the one from before the run, or "
                                                             "what the simulator wrote of it up to then."));
                result.insert(QStringLiteral("errors"), errors);
                if (succeeded && !written && !operatingPoint)
                    result.insert(QStringLiteral("note"), tr("The simulator reported no error but wrote no new dataset: its analyses may "
                                                             "save nothing (an operating point alone), or a post-processing step failed."));
                if (succeeded && operatingPoint && !result.contains(QStringLiteral("operating point")))
                    result.insert(QStringLiteral("note"), tr("The simulator reported no error but its operating point was not found."));
                if (!succeeded && errors.isEmpty() && !r->wasStopped())
                    result.insert(QStringLiteral("note"), tr("The simulator failed without an error message of its own: see the last lines."));
            }
            if (timedOut) result.insert(QStringLiteral("note"), stillRunning(timeout, id));
            changedWhileRunning(result);
            // Before and after, measurement by measurement.
            if (!compare.isEmpty() && doc && result.value(QStringLiteral("dataset written")).toBool())
                result.insert(QStringLiteral("compared"), comparedRuns(doc, compare, simulatorArg));
            // An ngspice optimize's result: what it found, and whether the
            // parameters now hold it.
            if (doc && !timedOut && simulator == spicecompat::simNgspice && !operatingPoint) {
                // (Written in for its caller, who asked: its edit.)
                const quint64 editor = QucsDoc::editor();
                QucsDoc::setEditor(caller);
                const QJsonArray optimum = optimumOf(doc, output, applyOptimum);
                QucsDoc::setEditor(editor);
                if (!optimum.isEmpty()) result.insert(QStringLiteral("optimum"), optimum);
            }
            // Brief: what came of it, not the log and the long lists.
            if (brief) {
                result.remove(QStringLiteral("last lines"));
                result.remove(QStringLiteral("data display"));
                for (const char* key : {"errors", "warnings", "variables", "traces without data"}) {
                    QJsonArray list = result.value(QLatin1String(key)).toArray();
                    const int keep = QLatin1String(key) == QLatin1String("errors") ? 5 : 3;
                    // (The variables by their count: their list is cut at 40.)
                    const int total = QLatin1String(key) == QLatin1String("variables") && result.contains(QStringLiteral("variable count"))
                                          ? result.value(QStringLiteral("variable count")).toInt()
                                          : int(list.size());
                    if (total <= keep) continue;
                    const int more = total - keep;
                    while (list.size() > keep) list.removeLast();
                    list.append(tr("... %1 more").arg(more));
                    result.insert(QLatin1String(key), list);
                }
            }
            done(jsonResult(result));
        };
        connect(run, &SimulationRun::simulated, this, [report](SimulationRun* r) { report(r, false, 0); });
        // Put back when it ends, even after the answer went (timed out).
        connect(run, &SimulationRun::simulated, this, [restore] { restore(); });
        if (timeout > 0)
            QTimer::singleShot(timeout, this, [report, answered, followed, pastTimeout, live = QPointer<SimulationRun>(run)] {
                if (*answered != 0) return;
                const int id = pastTimeout(live);
                if (id == 0) return;   // (in the background: stopped, its end reported)
                report(nullptr, true, id);
                *followed = id;   // (its end, from now on, kept under it)
            });
    });
}
