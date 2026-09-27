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
#include "erc.h"
#include "dialogs/simmessage.h"
#include "ngstatistics.h"
#include "textdoc.h"
#include "wire.h"
#include "wirelabel.h"
#include "workspace.h"

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
#include <QTextEdit>
#include <QSet>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace {

// The tools, as MCP's tools/list gives them.
const char* const kTools = R"JSON([
{"name": "get_state",
 "description": "The state of the Qucs-S window: its panes (each one's number, row and column, rectangle, documents and the one in front, which is active; move_to_pane arranges them), the documents open (path, title, kind, unsaved changes, which is in front, its pane; each one's revision - it counts every edit, undo and reload, whoever made it - and who made the last edit: you, the user or another conversation; when its dataset was last written), the simulator in the settings, the workspace folder, a simulation under way, a dialog waiting for an answer. Start here. Every tool's result also says what changed since your last call that you did not change: the user's edits, a simulation the user ran.",
 "inputSchema": {"type": "object", "properties": {}}},
{"name": "move_to_pane",
 "description": "Puts a document (the one in front unless path names another) in another pane, to see documents side by side: 'pane' is a pane's number as get_state lists them, or right or below - a new pane beside the document's (at most two a row, two rows). Returns the state with the panes.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "pane": {"description": "A pane's number, or \"right\" or \"below\""}}, "required": ["pane"]}},
{"name": "open_document",
 "description": "Opens a file in a tab of Qucs-S - a schematic (.sch), a symbol (.sym), a data display (.dpl), a netlist or any text file, a PDF document (read in Qucs-S's viewer: a datasheet, a report) - or brings it to the front if it is open. A relative path is taken from the open project's folder, else the workspace folder; a file's name alone finds the open document of that name.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "The file"}}, "required": ["path"]}},
{"name": "new_document",
 "description": "Opens a new, untitled document in front: a schematic, or a text document - or a schematic's data display (data_display: its .dpl, made when it has none; 'path' the schematic, the one in front unless given), where diagrams and paintings for a report go, the schematic kept clean; export_image writes a picture of it.",
 "inputSchema": {"type": "object", "properties": {"kind": {"type": "string", "enum": ["schematic", "text", "data_display"]}, "path": {"type": "string"}}}},
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
 "description": "Reads a schematic as it is in Qucs-S now, unsaved changes included. 'summary' (the default) lists its components - name, type, place, rotation, mirroring, whether active, properties ('properties': non_default - those shown or not at the type's default, the default; shown; all), and each pin's place, whether anything is on it and its net (a label's name, gnd, or net1, net2, ...) - its nets with the pins on each (those with two pins or more, or a name), its wires, net labels, paintings (numbered as the painting tools take them: each one's type and fields by name - a text's place, text, size and colour, an arrow's ends, a box's corner and size, ...), its settings (dataset, data display, frame), and its diagrams, numbered as the diagram tools take them, with their axes, traces - each trace's variable, look, and points or why it shows no data - and markers. 'components' (names) or 'region' ([x1, y1, x2, y2]) lists only those components, with their nets and wires; a list of more than 200 is cut short and says what is left out. 'symbol' lists the paintings of its symbol as well (its ports and name text among them; so does a document that shows its symbol). 'overview' tells it at a glance - its parts counted by type, its analyses, its named nets, its extent, its diagrams - a few hundred bytes for thousands of parts: start there with a large schematic. 'text' is the text its .sch file would have. Coordinates are the schematic's units; the grid is usually 10.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "format": {"type": "string", "enum": ["summary", "overview", "text"]}, "symbol": {"type": "boolean"},
   "properties": {"type": "string", "enum": ["non_default", "shown", "all"]},
   "components": {"type": "array", "items": {"type": "string"}},
   "region": {"type": "array", "items": {"type": "integer"}, "minItems": 4, "maxItems": 4},
   "selection": {"type": "boolean", "description": "What the user selected: those components (or that region)"}}}},
{"name": "check_schematic",
 "description": "Checks a schematic for what a simulation would fail on, or do otherwise than meant - as Simulation > Check Schematic does - each finding with its place and part. errors: no ground, two parts of one name, a part the simulator cannot take, ... warnings: pins and wire ends connected to nothing; a wire's end or a pin on another net's wire mid-way (not joined: a wire joins only where it ends); two nets' wires over each other; parts connected to no ground (floating); nets that reach ground only through capacitors or current sources (no DC path: no operating point); no simulation block. notes, fine if meant: wires of two nets crossing without a junction (no connection there), a net label on one pin alone (a plotted node - or a label meant to match another). Use it after building or rewiring a circuit, before simulate; get_schematic's summary counts them too.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}}},
{"name": "set_schematic",
 "description": "Replaces the elements of a schematic with those of 'text': a .sch file's text, or any of its <Components>, <Wires>, <Diagrams> and <Paintings> sections (sections left out stay as they are; <Properties> and <Symbol> are not taken). One step to undo; the diagrams read their data again. Returns what it read of each section it replaced: the components by name and type, the wires' count, the diagrams as get_schematic lists them (each trace's points or why it has none, each marker and the sample it shows), the paintings' lines. When the text does not read, the schematic stays as it was and the error is told - and so for a component line with more property values than its type has properties (the values are positional: one too many in the middle puts every value after it in the wrong property); one with fewer is taken, the rest at their defaults, and the result says so. To hide or show a property, or move a part's text, edit_component does it without rewriting the line. describe_format gives the lines' fields. For diagrams, traces and markers add_diagram, edit_diagram, add_trace, edit_trace, add_marker and edit_marker are simpler and safer.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "text": {"type": "string"}}, "required": ["text"]}},
{"name": "describe_format",
 "description": "The lines of a .sch file, field by field - a component, a wire, a diagram (all its ~30 fields), a trace, a marker, a painting - as set_schematic takes them and get_schematic's 'text' gives them; for a painting also the fields add_painting and edit_painting take, type by type. Without 'element', all of them.",
 "inputSchema": {"type": "object", "properties": {"element": {"type": "string", "enum": ["component", "wire", "diagram", "trace", "marker", "painting"]}}}},
{"name": "add_component",
 "description": "Places a component from the library at x, y (snapped to the grid): 'type' is its model - R, C, L, GND, Vdc, Vac, Idc, Iac, Diode, _BJT, _MOSFET, OpAmp, Sub, .DC, .AC, .TR, .SP, ... (list_component_types lists them all). Properties by name (as get_schematic shows them, e.g. {\"R\": \"4.7k\"}); rotation in quarter turns from the type's own orientation, as Rotate makes them; mirror about the x axis; 'shown' which properties are written on the schematic, 'name_shown', 'text_at' where its text goes. An equation block (Eqn, NutmegEq, .PARAM, .OPTIONS, ...) takes its equations as 'equations' ({\"gain_db\": \"db(v(out))\"}, or a list of \"name=value\" in their order) - they replace its placeholder y=1; an ngspice Monte Carlo (.NGMONTECARLO) or corners block its 'records' and 'specs'. Returns the component with its pins' places - and a note when a pin came down on a wire or another pin (it is joined to it) or on another net's wire without joining it.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "type": {"type": "string"}, "x": {"type": "integer"}, "y": {"type": "integer"},
   "name": {"type": "string", "description": "Its name; the next free one (R1, R2, ...) when not given"},
   "properties": {"type": "object", "additionalProperties": {"type": "string"}},
   "rotation": {"type": "integer", "minimum": 0, "maximum": 3}, "mirror": {"type": "boolean"},
   "shown": {"type": "object", "additionalProperties": {"type": "boolean"}, "description": "Which properties are shown on the schematic: {\"R\": true, \"Temp\": false}"},
   "name_shown": {"type": "boolean"}, "text_at": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Where its text begins (the top left corner), [dx, dy] from its centre"}, "equations": {"description": "An equation block's equations (Eqn, NutmegEq, .PARAM, .OPTIONS, .FUNC, .IC, ...): {\"gain_db\": \"db(v(out))\"} or, in their order, a list of \"name=value\" (or [name, value]); a value \"\" or null takes one away"},
   "replace_equations": {"type": "boolean", "description": "The equations become those given alone (else each given is set or added)"},
   "records": {"type": "array", "items": {}, "description": "An ngspice Monte Carlo's or corners' values recorded for each sample: [{\"name\": \"gain\", \"expression\": \"db(v(out))\"}] or \"gain|db(v(out))\" - the list it records"},
   "specs": {"type": "array", "items": {}, "description": "Their limits: [{\"expression\": \"gain\", \"min\": \"19\", \"max\": \"21\"}] (one limit may be left out) or \"gain|19|21\" - a sample passes within all"}},
   "required": ["type", "x", "y"]}},
{"name": "edit_component",
 "description": "Changes a component: its properties (by name), its name, its place (x, y: where its centre goes), its rotation (0-3 quarter turns from the type's own orientation, as get_schematic gives it) and mirroring, whether it is active (an inactive one is left out of the simulation), and its text: 'shown' which properties are written on the schematic ({\"Is\": false} hides one - no need to rewrite its line with set_schematic), 'name_shown', 'text_at' ([dx, dy] from its centre: where its text begins, to move it off another part). An equation block's equations by name ('equations': each given set, a new one added, one given \"\" taken away; 'replace_equations' for exactly those given), a Monte Carlo's or corners' 'records' and 'specs' (the lists replaced) - no need for set_schematic. 'rename' renames it, and the traces, equations and markers that name it follow (i(V1), V1.It, @R1[i], R1's parameters in an equation), as rename_net does for a net. What is not given stays. Turned or moved, the circuit stays as it was: its pins are wired again to the nets they were on, and wires of other nets that its pins would come down on are moved out of the way. A change that cannot keep every net as it was is not made, and the error says why. A pin with nothing on it that comes down on another part's pin joins that pin's net, and the result's 'note' says so.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "name": {"type": "string"}, "rename": {"type": "string"},
   "properties": {"type": "object", "additionalProperties": {"type": "string"}},
   "x": {"type": "integer"}, "y": {"type": "integer"}, "rotation": {"type": "integer", "minimum": 0, "maximum": 3},
   "mirror": {"type": "boolean"}, "active": {"type": "boolean"},
   "shown": {"type": "object", "additionalProperties": {"type": "boolean"}, "description": "Which properties are shown on the schematic: {\"Is\": false, \"Bf\": true}"},
   "name_shown": {"type": "boolean"}, "text_at": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2, "description": "Where its text begins (the top left corner), [dx, dy] from its centre"}, "equations": {"description": "An equation block's equations (Eqn, NutmegEq, .PARAM, .OPTIONS, .FUNC, .IC, ...): {\"gain_db\": \"db(v(out))\"} or, in their order, a list of \"name=value\" (or [name, value]); a value \"\" or null takes one away"},
   "replace_equations": {"type": "boolean", "description": "The equations become those given alone (else each given is set or added)"},
   "records": {"type": "array", "items": {}, "description": "An ngspice Monte Carlo's or corners' values recorded for each sample: [{\"name\": \"gain\", \"expression\": \"db(v(out))\"}] or \"gain|db(v(out))\" - the list it records"},
   "specs": {"type": "array", "items": {}, "description": "Their limits: [{\"expression\": \"gain\", \"min\": \"19\", \"max\": \"21\"}] (one limit may be left out) or \"gain|19|21\" - a sample passes within all"}}, "required": ["name"]}},
{"name": "delete",
 "description": "Deletes components (by name), net labels (by the net's name), wires (by their two ends, [x1, y1, x2, y2]), diagrams (by their numbers as get_schematic lists them), traces ({\"diagram\": n, \"trace\": its number or variable}) and paintings (by their numbers; the symbol's with 'symbol') from a schematic, as one step to undo.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "names": {"type": "array", "items": {"type": "string"}},
   "wires": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}, "minItems": 4, "maxItems": 4}},
   "diagrams": {"type": "array", "items": {"type": "integer"}},
   "traces": {"type": "array", "items": {"type": "object", "properties": {"diagram": {"type": "integer"}, "trace": {}}}},
   "paintings": {"type": "array", "items": {"type": "integer"}}, "symbol": {"type": "boolean"},
   "selection": {"type": "boolean", "description": "What the user selected: its parts, wires, diagrams, paintings"}}}},
{"name": "add_analysis",
 "description": "Adds an analysis set up as it usually is - and, with 'plot', a diagram of what it gives, below the circuit - in one call instead of several coordinated edits. 'kind': ac ('from' and 'to' in Hz, 1 Hz to 100 MHz unless given; 'points', 101; 'scale' log unless lin; plotted in dB over a log frequency axis), tran ('stop' time, 1 ms unless given; 'points' 201: the print step is stop/(points-1); plotted over time), op (the DC operating point, a .DC block), sweep (a parameter sweep of 'analysis' - its name: TR1, AC1, DC1 - over 'parameter': a component's name, whose value is swept (R2), or a parameter an equation defines; 'from', 'to', 'points' 11, 'scale'; the analysis's curves become one for each value). 'plot': what to show - nodes (out), v(out), i(v1). x, y place the block (beside the other analyses unless given); 'name' names it. Values are numbers or text with units (\"10 kHz\"). Returns the block and the diagram; each a step to undo.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "kind": {"type": "string", "enum": ["ac", "tran", "op", "sweep"]},
   "from": {}, "to": {}, "stop": {}, "points": {}, "scale": {"type": "string", "enum": ["lin", "log"]},
   "analysis": {"type": "string"}, "parameter": {"type": "string"},
   "plot": {"type": "array", "items": {"type": "string"}}, "x": {"type": "integer"}, "y": {"type": "integer"}, "name": {"type": "string"}},
   "required": ["kind"]}},
{"name": "create_subcircuit",
 "description": "Makes a subcircuit of several components: they and the wiring among them go into a new schematic 'save_as' (a .sch beside this one; 'replace' writes over one there), with a port for each net that also reaches the rest of the circuit - and a ground for each pin on ground - and in their place comes one subcircuit component ('name', SUB1 unless given) whose pins are joined to those nets by net labels. Analyses stay outside. Returns the file, the instance and its ports (port number, net). One step to undo here (the file stays written); make_symbol then lays out its symbol.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "names": {"type": "array", "items": {"type": "string"}}, "save_as": {"type": "string"},
   "name": {"type": "string"}, "replace": {"type": "boolean"},
   "selection": {"type": "boolean", "description": "The parts the user selected, instead of names"}}, "required": ["save_as"]}},
{"name": "move",
 "description": "Moves several components together by dx, dy (in the schematic's units, on its grid: 100 is ten steps of a grid of 10), keeping every net as it was: the wiring among them moves with them, and the wires that go out to the rest of the circuit are drawn on to their pins' new places (as the cursor keys move a selection). 'diagrams' and 'paintings' (their numbers) may move along; a diagram's title (add_diagram's 'title') is part of it. A move that would join or split a net is not made. To make room for a stage, or tidy a circuit. One step to undo.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "names": {"type": "array", "items": {"type": "string"}},
   "dx": {"type": "integer"}, "dy": {"type": "integer"},
   "diagrams": {"type": "array", "items": {"type": "integer"}}, "paintings": {"type": "array", "items": {"type": "integer"}},
   "selection": {"type": "boolean", "description": "What the user selected - its parts, diagrams and paintings - instead of names"}}}},
{"name": "connect",
 "description": "Draws a wire between two pins or places, with right angles, by a way that goes over no other pin or wire (a wire joins whatever it runs over): around the parts when it can, else over them. It joins the two nets and nothing else - or, when no way would, draws nothing and says why; a crossing of another net's wire on the way (no connection) is said. A pin is \"R1.1\" (the component's name and the pin's number, from 1, or the pin's name); a place is [x, y].",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"},
   "from": {"description": "\"R1.2\" or [x, y]; a ground is GND.1 when there is one, GND#2.1 the second of several (get_schematic's 'ref')"}, "to": {"description": "\"C1.1\" or [x, y]"}}, "required": ["from", "to"]}},
{"name": "add_wire",
 "description": "Draws a wire through places, [[x, y], [x, y], ...], a segment from each to the next (a step that is not straight gets a right angle). What is at its places is joined - a wire joins only where it ends or bends; one that would run along over another pin or wire between them is not drawn, and the error names what is in the way. One that crosses another net's wire is drawn, but a crossing is no junction - no connection - and the result says so (Look: ...): end the wire on the other to join it.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "points": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2}, "minItems": 2}},
   "required": ["points"]}},
{"name": "set_label",
 "description": "Names the net at a pin or at a place on a wire (a net label: nets with the same name are connected). An empty name takes the label away.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "at": {"description": "\"R1.1\" or [x, y]"}, "name": {"type": "string"}}, "required": ["at", "name"]}},
{"name": "select",
 "description": "Selects components by name, diagrams and paintings by their numbers as get_schematic lists them, in a schematic (the rest are deselected); nothing given deselects everything.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "names": {"type": "array", "items": {"type": "string"}},
   "diagrams": {"type": "array", "items": {"type": "integer"}}, "paintings": {"type": "array", "items": {"type": "integer"}}}}},
{"name": "zoom",
 "description": "Zooms a schematic: 'all' shows all of it, 'selection' the selection, 'in' and 'out' a step, 'none' the scale of 1.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "to": {"type": "string", "enum": ["all", "selection", "in", "out", "none"]}}, "required": ["to"]}},
{"name": "undo",
 "description": "Undoes the last change of a document (the one in front unless path names another), as Edit > Undo; 'steps' undoes so many (a batch that stopped half-way says how many changes it made); 'to' goes to a step of a schematic as undo_history numbers them - back, or forward again. Says what it changed back, part by part.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "steps": {"type": "integer", "minimum": 1, "maximum": 1000},
   "to": {"type": "integer", "minimum": 0, "description": "A step as undo_history lists them: the schematic as it was after it (0: as it was loaded)"}}}},
{"name": "undo_history",
 "description": "A schematic's steps to undo, in words - \"step 7: R2 R 47k → 67k; step 8: diagram 2: trace 2's look changed\" - the last 'steps' (10 unless given) up to where it is, and those that can be redone after it: so that undo can go to a known point ('to') rather than counting.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "steps": {"type": "integer", "minimum": 1, "maximum": 200}}}},
{"name": "redo",
 "description": "Redoes the last change undone, as Edit > Redo; 'steps' redoes so many.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "steps": {"type": "integer", "minimum": 1, "maximum": 1000}}}},
{"name": "screenshot",
 "description": "A picture (PNG). 'diagram' (its number) or 'region' ([x1, y1, x2, y2] of the schematic) takes that alone, on paper - a plot checked without the whole page. 'area': paper (or all, the default) - the whole schematic drawn as it is printed and exported: on white paper, whatever the theme, in its print colours; screen (or visible) - the canvas as the user sees it now: the theme's paper and colours (a dark canvas in the dark theme), its zoom, what is scrolled into view - take this one to see what the user sees, a problem of how something looks on screen above all; window - the whole Qucs-S window (its panes and tabs, docks, the status bar), and each dialog open over it, as pictures of their own. For a text document or a PDF, paper and screen are what its tab shows.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "area": {"type": "string", "enum": ["paper", "screen", "window", "all", "visible"]},
   "diagram": {"type": "integer"}, "region": {"type": "array", "items": {"type": "integer"}, "minItems": 4, "maxItems": 4}}}},
{"name": "list_component_types",
 "description": "The components of the library: the type to give add_component, what it is, its category. 'search' keeps those whose type, name or category has it. describe_component_type tells a type's properties.",
 "inputSchema": {"type": "object", "properties": {"search": {"type": "string"}}}},
{"name": "list_actions",
 "description": "The actions of Qucs-S's menus: the menu path to give trigger_action (\"Simulation > Simulate\"), whether it can be used now, whether it is checked, its shortcut. 'search' keeps those whose path has it.",
 "inputSchema": {"type": "object", "properties": {"search": {"type": "string"}}}},
{"name": "trigger_action",
 "description": "Uses a menu action as a click on it would: 'action' is its menu path (\"Edit > Rotate\") or its object name. When it opens a dialog, the dialog stays open: get_dialog reads it, set_dialog fills it in and closes it. Actions that open the system's file or print dialogs are refused: use open_document and save_document.",
 "inputSchema": {"type": "object", "properties": {"action": {"type": "string"},
   "path": {"type": "string", "description": "The document to use it on, brought to the front first; the one in front when not given"}},
  "required": ["action"]}},
{"name": "get_dialog",
 "description": "The dialog of Qucs-S that waits for an answer (or another window of it that is open): its title, its texts and its controls - fields, lists, check boxes, tabs, tables, buttons - each with its label, value and an id for set_dialog.",
 "inputSchema": {"type": "object", "properties": {}}},
{"name": "set_dialog",
 "description": "Fills in the open dialog and presses a button. 'set' changes controls, each by its id or label from get_dialog: a field takes text, a list an item, a check box true or false, a spin box a number, tabs a tab's title, a table [row, column, text]. 'press' names the button pressed after (OK, Cancel, Apply, ... or its id).",
 "inputSchema": {"type": "object", "properties": {
   "set": {"type": "array", "items": {"type": "object", "properties": {"control": {"type": "string"}, "value": {}}, "required": ["control", "value"]}},
   "press": {"type": "string"}}}},
{"name": "simulate",
 "description": "Simulates a schematic (the one in front unless path names another; it must have been saved once) with the simulator in the settings - or 'simulator' for this run alone, the setting left as it is - as Simulation > Simulate, and waits for the end (Qucsator's too). Check Schematic's errors and warnings come first, found before the run ('schematic check'). It says whether it succeeded (the simulator ran to its end and reported no error), its errors and warnings - each with its message and, where the simulator names them, the netlist line (its number and text), the part of the schematic and the node - the dataset it wrote (name.dat.ngspice for ngspice, .dat.xyce, .dat.spopus; name.dat for Qucsator) and its variables, the traces of the diagrams that show no data and why, whether the schematic was changed while it ran (by the user or another conversation: then the results are of the schematic as it was when the run began), and the last lines of the output. 'operating_point' runs the DC operating point alone instead (as Simulation > Calculate DC bias; a transient-only schematic's too) and returns it structured: node voltages, branch currents and, with ngspice, each transistor's gm, ic, vbe, gpi, ... under its component, with re = 1/gm, rpi, beta and ro worked out - the numbers that explain a gain; the datasets are left as they were. 'timeout' in seconds, 120 unless given. 'keep_as' keeps a copy of the dataset under that name, to compare runs: get_dataset reads it by its file, and a trace shows it beside the current run as ngspice/<name>:tran.v(out).",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "timeout": {"type": "integer"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"], "description": "For this run alone (an installed one); set_simulator changes the setting"},
   "keep_as": {"type": "string", "description": "A name of letters, digits, _ and -: the copy is <name>.dat.ngspice (or .xyce, ...) beside the schematic"},
   "operating_point": {"type": "boolean", "description": "Run the DC operating point alone, whatever analyses the schematic has, and return it: each node's voltage and branch current, and (ngspice) each device's quantities - gm, ic, vbe, gpi, gds, ... - with re = 1/gm, beta, ro"}}}},
{"name": "get_netlist",
 "description": "The netlist of a schematic as text: as a simulation with the simulator in the settings would write it now, or with 'last' the one the last simulation ran (Simulation > Show Last Netlist; the one the line numbers of simulate's errors are of). 'numbered' puts each line's number before it. 'format': spice (the default) or cdl (Simulation > Save CDL netlist, with the CDL settings). export_netlist writes it to a file.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "last": {"type": "boolean"}, "numbered": {"type": "boolean"},
   "format": {"type": "string", "enum": ["spice", "cdl"]}}}},
{"name": "export_netlist",
 "description": "Writes the netlist of a schematic to a file, as Simulation > Save netlist and Save CDL netlist do - their file dialogs cannot be answered: 'save_as' (a path, or a name in the project's folder, else the workspace's; one there is written over), 'format' spice (the default) or cdl, 'last' the one the last simulation ran. Returns the file and its lines.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "save_as": {"type": "string"}, "last": {"type": "boolean"},
   "format": {"type": "string", "enum": ["spice", "cdl"]}}, "required": ["save_as"]}},
{"name": "get_dataset",
 "description": "Reads a simulation's results as numbers, from the dataset the simulator wrote (its 'written' time says which run). Without 'variables': the variables it holds - the independent ones (time, frequency, a swept parameter: their range and points) and the others (what they depend on, their points, whether complex, their range, their units when known - dB, V, A, degrees - and what an equation defines them as) with the name a trace takes - and the operating point of a DC simulation (op): each node's value and, with ngspice, each device's quantities (id, gm, vgs, ...) under its component. 'operating_point': only that, every device in full. With 'variables': for each, over the range from 'from' to 'to' of its x, its statistics (min and max and where, mean and RMS weighted over x, initial and final value) and, as asked, samples ('points': at most so many, spread evenly over the samples in the range), values at given x ('at': interpolated), and measurements on the full data ('measure'): rise_time and fall_time (10%-90% of the swing, the first edge), overshoot (percent of the step), settling_time (within 'tolerance' of the step, 0.02 unless given, from 'from'), period, frequency and duty_cycle (at 'level', the middle of the swing unless given), crossings (of 'level'), bandwidth (the -3 dB points: of a magnitude, 1/sqrt(2) of its peak; of a curve in dB - db(...), vdb(...), an equation making one; 'decibels' says so when that cannot be told - 3 dB below its peak; refused on a curve that goes below 0 and is not in dB), thd (of a transient: the total harmonic distortion in percent and dB, and each harmonic's amplitude, over the last 'periods' (1 unless given) whole periods of 'fundamental' (Hz; the curve's own frequency unless given) before 'to', harmonics 2 to 'harmonics' (9 unless given) - as ngspice's .four), gain (of an AC curve: at the lowest frequency and at its peak, as a ratio and in dB, and the unity-gain frequency where it falls through 1, 0 dB), phase_margin and gain_margin (of a loop gain, a complex AC variable: 180 degrees plus its phase where its magnitude falls through 1; minus its gain in dB where its phase falls through -180 degrees; each with the frequency), distribution (the values as samples - a Monte Carlo's, one per run: mean, standard deviation, median, 5th and 95th percentiles, a histogram; with 'level', the share at or above it), fft (of a transient: its spectrum, resampled evenly with a Hann window - the strongest lines with their amplitudes and dBc, dc, the noise floor, the resolution), eye (of a transient folded at 'bit_period' from 'offset': the eye's height at the bits' centres, its width, the crossings' jitter peak to peak and rms, the high and low levels). A variable swept over a parameter gives a curve for each of its values - and, measured, a table: a row for each value (bandwidth against R, overshoot for each sample). A table - a .csv, .tsv or .xlsx file, a script's results or a Monte Carlo's workbook - is read as a dataset: each column of numbers a variable, over the first column when it rises (time), else over the row. Complex values (AC) come as magnitude and phase in degrees unless 'form' says otherwise; statistics and measurements are of the magnitude. A variable written as complex numbers with no imaginary part (a Nutmeg equation's db(...)) is read as real, its sign kept, and says so. A variable may be an expression of others - v(out)/v(in), db(ac.v(out)/ac.v(in)) - evaluated sample by sample, and is then measured like any. 'compare' (a name simulate's keep_as gave, or a dataset file) puts another run beside each: its statistics and measurements over the same range, and the difference - the largest and where, its mean and RMS.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "A schematic or data display, open or not (the document in front when not given), or a dataset file (.dat, .dat.ngspice, ...)"},
   "simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"], "description": "Whose dataset of a schematic: the simulator in the settings unless given (else the newest there is)"},
   "variables": {"type": "array", "items": {"type": "string"}, "description": "As get_dataset lists them (tran.v(out)), as a trace names them (ngspice/tran.v(out)), without the analysis (v(out): each analysis's), a node's name (out: its voltage) - or an expression of them: v(out)/v(in), v(out)-v(in), db(ac.v(out)/ac.v(in)), with + - * / ^ and db, abs, mag, phase, real, imag, sqrt, log10, ln, exp, conj, in complex numbers for AC"},
   "compare": {"type": "string", "description": "Another run to compare with: a name simulate's keep_as gave (run1), or a dataset file - each variable gets that run's statistics and measurements, and the difference"},
   "from": {"type": "number"}, "to": {"type": "number"},
   "points": {"type": "integer", "minimum": 0, "maximum": 5000, "description": "Samples of each curve: 100 unless 'at' or 'measure' is given, then none"},
   "at": {"type": "array", "items": {"type": "number"}},
   "measure": {"type": "array", "items": {"type": "string", "enum": ["rise_time", "fall_time", "overshoot", "settling_time", "period", "frequency", "duty_cycle", "crossings", "bandwidth", "thd", "gain", "phase_margin", "gain_margin", "distribution", "fft", "eye"]}},
   "bit_period": {"type": "number", "description": "eye: a bit's length (seconds)"}, "offset": {"type": "number", "description": "eye: where the first bit begins, after the range's start"},
   "level": {"type": "number"}, "tolerance": {"type": "number"},
   "fundamental": {"type": "number", "description": "thd: the fundamental's frequency in Hz; the curve's own frequency unless given"},
   "harmonics": {"type": "integer", "minimum": 2, "maximum": 100, "description": "thd: the highest harmonic counted, 9 unless given"},
   "periods": {"type": "integer", "minimum": 1, "maximum": 10000, "description": "thd: how many whole periods of the fundamental, ending at 'to' (the end), 1 unless given"},
   "decibels": {"type": "boolean", "description": "Whether the variables' values are in dB (bandwidth: 3 dB below the peak); told from their names and equations when not given"},
   "operating_point": {"type": "boolean", "description": "Only the operating point: every node's value and every device's quantities"},
   "form": {"type": "string", "enum": ["magnitude_phase", "db_phase", "real_imaginary"]}}}},
{"name": "reload_data",
 "description": "Reads the datasets again and redraws the diagrams of a document (the document named by path, or every open one), as Simulation > Reload Simulation Data: after a simulation outside Qucs-S, or when a plot is blank. Says which traces still show no data, and why.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}}},
{"name": "add_diagram",
 "description": "Places a diagram on a schematic or a data display, its traces showing the dataset's data at once. 'type': rect (x-y, the default), polar, smith, admittance_smith, polar_smith, smith_polar, tab (a table), timing, truth, 3d, locus, histogram. x, y: its lower left corner - left out, it goes below everything on the schematic (a free place; the result says where, and when a diagram lies over another or over parts); width and height, 240 x 160 unless given; 'title', drawn above it - part of the diagram, it moves with it. 'traces': each a variable (as get_dataset names it - tran.v(out), or v(out) or out when that says which) or an object as add_trace takes it. The axes, grid and legend as edit_diagram takes them. Returns the diagram as get_schematic lists it: its number, and each trace's points or why it has none.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "type": {"type": "string"}, "x": {"type": "integer"}, "y": {"type": "integer"},
   "width": {"type": "integer"}, "height": {"type": "integer"}, "title": {"type": "string", "description": "Drawn above it, and moved, selected and exported with it"},
   "traces": {"type": "array", "items": {}},
   "x_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}},
   "y_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}},
   "y2_axis": {"type": "object", "properties": {"label": {"type": "string"}, "log": {"type": "boolean"}, "auto": {"type": "boolean"}, "from": {"type": "number"}, "to": {"type": "number"}, "step": {"type": "number"}, "units": {"type": "string", "enum": ["none", "dB", "dBuV", "dBm"]}}},
   "grid": {"type": "boolean"}, "legend": {"type": "string", "enum": ["off", "top_left", "top_right", "bottom_left", "bottom_right"]}}}},
{"name": "edit_diagram",
 "description": "Changes a diagram: its place (x, y: the lower left corner) and size, its title (above it, moving with it), its axes (x_axis, y_axis, and y2_axis on the right: label, log, auto, from, to, step, units), grid, legend. What is not given stays. 'diagram' is its number as get_schematic lists them (it may be left out when there is one). One step to undo; the traces are read again from the dataset.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "diagram": {"type": "integer"},
   "x": {"type": "integer"}, "y": {"type": "integer"}, "width": {"type": "integer"}, "height": {"type": "integer"}, "title": {"type": "string", "description": "\"\" takes it away"},
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
{"name": "add_marker",
 "description": "Places a marker on a trace of a diagram (it shows the sample nearest where it is put, and the value there). 'at': an x value, or where on the trace: peak (or max), min, -3dB (3 dB below the peak - of a curve in dB, its peak less 3; of a magnitude, the peak over sqrt(2) - past the peak first, else before it; 'reference' dc or a level, 0 dB for a filter's spec, puts it below that instead), crossing:<y> (the first crossing of y). Returns the marker as get_schematic lists it and what 'at' found (the exact crossing; the sample the marker is on). 'label' is where its box's top left corner goes, [x, y] on the schematic; 'label_offset' [dx, dy] puts it that far from the point it marks (y down). 'precision': digits; 'format' of complex values; 'transparent' background; 'indicator' at the point; text and background colours (#rrggbb, #aarrggbb, a name, or auto). One step to undo.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "diagram": {"type": "integer"}, "trace": {"description": "Its number (from 1) or its variable"},
   "at": {"description": "An x value (a number), or \"peak\", \"max\", \"min\", \"-3dB\", \"crossing:<y>\""},
   "reference": {"description": "What -3dB is 3 dB below: \"peak\" (the default), \"dc\" (the value at the curve's start, the lowest frequency), or a level - 0 for a filter's spec in dB (on a magnitude, 1 is a gain of one)"},
   "label": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2},
   "label_offset": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2},
   "precision": {"type": "integer", "minimum": 1, "maximum": 12},
   "format": {"type": "string", "enum": ["real_imaginary", "magnitude_degrees", "magnitude_radians"]},
   "transparent": {"type": "boolean"}, "indicator": {"type": "string", "enum": ["off", "square", "triangle"]},
   "text_color": {"type": "string"}, "fill_color": {"type": "string"}},
  "required": ["at"]}},
{"name": "edit_marker",
 "description": "Changes a marker of a diagram - 'marker' is its number as get_schematic lists them - as add_marker takes it: 'at' moves it (its label moves with it), and its label, precision, format, transparency, indicator, colours. What is not given stays. One step to undo.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "diagram": {"type": "integer"}, "marker": {"type": "integer"},
   "at": {"description": "An x value (a number), or \"peak\", \"max\", \"min\", \"-3dB\", \"crossing:<y>\""},
   "reference": {"description": "What -3dB is 3 dB below: \"peak\" (the default), \"dc\" (the value at the curve's start, the lowest frequency), or a level - 0 for a filter's spec in dB (on a magnitude, 1 is a gain of one)"},
   "label": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2},
   "label_offset": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2},
   "precision": {"type": "integer", "minimum": 1, "maximum": 12},
   "format": {"type": "string", "enum": ["real_imaginary", "magnitude_degrees", "magnitude_radians"]},
   "transparent": {"type": "boolean"}, "indicator": {"type": "string", "enum": ["off", "square", "triangle"]},
   "text_color": {"type": "string"}, "fill_color": {"type": "string"}}}},
{"name": "delete_marker",
 "description": "Deletes a marker of a diagram ('marker': its number as get_schematic lists them). One step to undo.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "diagram": {"type": "integer"}, "marker": {"type": "integer"}}}},
{"name": "rename_net",
 "description": "Renames a net - its labels, or a net get_schematic calls net1, net2, ... gets a label - and whatever names its voltage: the traces of the schematic's diagrams and of its data displays (open ones as a change to undo; the .dpl file of a closed one is rewritten) and its equations: v(out) becomes v(out1), out.v out1.v. Refused when a net has the new name already (that would join the two). The dataset keeps the old name until the next simulation.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "from": {"type": "string"}, "to": {"type": "string"}}, "required": ["from", "to"]}},
{"name": "describe_component_type",
 "description": "A component type of the library described: what it is, its category, how its parts are named, its pins (their places relative to its centre, unturned), its properties in their order - name, default value, unit, what it means, whether shown on the schematic, and the simulators it counts for - the simulators it works with, the netlist line it makes with its defaults under the simulator in the settings, and notes on what is easy to get wrong with it (a Vpulse is one pulse: Vrect repeats).",
 "inputSchema": {"type": "object", "properties": {"type": {"type": "string", "description": "As list_component_types gives it: R, Vpulse, .TR, ..."}}, "required": ["type"]}},
{"name": "batch",
 "description": "Runs several of these tools in one call, in order - far quicker than one call each: place and wire a circuit, set many properties, add a diagram and its traces at once. Each change is one step of Edit > Undo, as when called alone. It stops at the first that fails unless 'keep_going', and then says how many changes the calls before it made (undo with 'steps' takes them back); with 'atomic' those changes are undone at once - all or nothing (files written, simulations run and documents opened stay). It gives each one's result in order.",
 "inputSchema": {"type": "object", "properties": {
   "calls": {"type": "array", "minItems": 1, "items": {"type": "object", "properties": {
     "tool": {"type": "string", "description": "A tool's name: add_component, connect, ..."},
     "arguments": {"type": "object"}}, "required": ["tool"]}},
   "keep_going": {"type": "boolean", "description": "Go on after one that fails"},
   "atomic": {"type": "boolean", "description": "All or nothing: when one fails, the changes of those before it are undone"}}, "required": ["calls"]}},
{"name": "add_painting",
 "description": "Draws a painting - a text, an arrow, a line, a box, a text box, a table, a dimension, a formula - on a schematic, or on its symbol with 'symbol' (the document is switched to show its symbol, as Edit Circuit Symbol does; a .sym file is all symbol): to mark up a result, label a part of the circuit, draw a subcircuit's symbol. 'type': text, line, arrow, rectangle, ellipse, arc, polyline, image (from 'file'), rounded_rectangle, polygon, brace, waveform, text_box (kind block, note or callout, with a 'tip' it points at), table, dimension, formula (TeX). Its fields by name: a text's x, y, text (_x or _{xy} a subscript, ^ a superscript, as in TeX), size, color, angle; a line's or an arrow's from and to ([x, y]) and an arrow's head (open or filled); a box's x, y (its top left corner), width and height; color, thickness, style, fill_color, fill_style, filled; ... - describe_format with element painting lists every type's fields. Returns it as get_schematic lists it, with its number. One step to undo.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "symbol": {"type": "boolean", "description": "On the schematic's symbol"},
   "type": {"type": "string", "enum": ["text", "line", "arrow", "rectangle", "ellipse", "arc", "polyline", "image", "rounded_rectangle", "polygon", "brace", "waveform", "text_box", "table", "dimension", "formula"]},
   "x": {"type": "integer"}, "y": {"type": "integer"}, "width": {"type": "integer"}, "height": {"type": "integer"},
   "from": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2}, "to": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2},
   "points": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}}},
   "text": {"type": "string"}, "size": {"type": "integer"}, "color": {"type": "string"}, "thickness": {"type": "integer"},
   "style": {"type": "string"}, "fill_color": {"type": "string"}, "fill_style": {"type": "string"}, "filled": {"type": "boolean"},
   "angle": {"type": "integer"}, "head": {"type": "string", "enum": ["open", "filled"]}, "file": {"type": "string"},
   "around": {"type": "string", "enum": ["selection"], "description": "Placed by what the user selected: a box about it, a text above it, an arrow to it, a brace beside it, a dimension under it (the fields given stay)"}},
   "required": ["type"]}},
{"name": "edit_painting",
 "description": "Changes a painting - 'painting' is its number as get_schematic lists them (a painting of the symbol with 'symbol') - by the fields add_painting takes for its type: what is not given stays; it keeps its type. A symbol's ports can be moved (x, y); its name text moved, and given its 'prefix' (SUB: the instances are SUB1, SUB2, ...) and its 'parameters' - a subcircuit's, [{\"name\": \"R\", \"default\": \"1k\", \"description\": ..., \"type\": ..., \"shown\": true}], each instance then takes them (save the file for them to see it). Returns it as get_schematic lists it. One step to undo.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "symbol": {"type": "boolean"}, "painting": {"type": "integer"},
   "x": {"type": "integer"}, "y": {"type": "integer"}, "width": {"type": "integer"}, "height": {"type": "integer"},
   "from": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2}, "to": {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2},
   "points": {"type": "array", "items": {"type": "array", "items": {"type": "integer"}}},
   "text": {"type": "string"}, "size": {"type": "integer"}, "color": {"type": "string"}, "thickness": {"type": "integer"},
   "style": {"type": "string"}, "fill_color": {"type": "string"}, "fill_style": {"type": "string"}, "filled": {"type": "boolean"},
   "angle": {"type": "integer"}, "head": {"type": "string", "enum": ["open", "filled"]}, "file": {"type": "string"}},
   "required": ["painting"]}},
{"name": "list_documents",
 "description": "The files of the workspace, of a project or of a folder - schematics, symbols, data displays, datasets, netlists, texts, PDFs, spreadsheets, pictures - newest first: each one's path (from the folder listed), kind, size, when it was changed and whether it is open; a dataset says which simulator wrote it, the schematic it is of, and the traces of the open diagrams that read it but that it has not; an open schematic, the traces whose dataset is not there at all (ngspice/... reads name.dat.ngspice: the Qucsator-versus-ngspice mix-up). Without 'folder', the workspace's, with its projects listed. 'folder': a project's name (amp, or amp_prj) or a folder (from the workspace, or absolute). 'kind' keeps one kind, 'search' the files whose name has it, 'sort' newest (the default) or name. Folders inside are looked into, 4 deep; 300 files at most, with what is left out.",
 "inputSchema": {"type": "object", "properties": {
   "folder": {"type": "string"},
   "kind": {"type": "string", "enum": ["schematic", "symbol", "data display", "dataset", "netlist", "text", "pdf", "spreadsheet", "markdown", "picture", "verilog-a"]},
   "search": {"type": "string"}, "sort": {"type": "string", "enum": ["newest", "name"]}}}},
{"name": "export_image",
 "description": "Writes a picture of a schematic, a symbol or a data display (the one in front unless path names another) to a file, as File > Export as image does, without its dialog: 'save_as' (a path, or a name in the project's folder, else the workspace's; a file there is written over; its suffix gives the format when 'format' is not given), 'format' png, jpeg, bmp, tiff, webp, svg, pdf, eps or pdf_tex (a PDF with its text in a LaTeX file beside it), 'scale' (a raster image's pixels per unit of the schematic, 1 being 96 dpi; 2 unless given), 'colours' colour (the default), grayscale or monochrome, 'transparent' (no paper, where the format can have none), 'diagram' (its number as get_schematic lists them: that diagram alone - a frequency response for a report) or 'selection' (what is selected alone). Returns the file, its format, its size in pixels or units.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "save_as": {"type": "string"},
   "format": {"type": "string", "enum": ["png", "jpeg", "bmp", "tiff", "webp", "svg", "pdf", "eps", "pdf_tex"]},
   "scale": {"type": "number", "minimum": 0.1, "maximum": 20}, "colours": {"type": "string", "enum": ["colour", "grayscale", "monochrome"]},
   "transparent": {"type": "boolean"}, "diagram": {"type": "integer"}, "selection": {"type": "boolean"}},
   "required": ["save_as"]}},
{"name": "build_verilog_a",
 "description": "Compiles a Verilog-A source with OpenVAF (the one under Application Settings > Locations), as Build Verilog-A does, and waits for it: a syntax error found now, not at the next simulation's cost. 'file' is the .va (relative to the project, else the workspace; the .va document in front when not given); an open one with unsaved changes needs 'unsaved': save or as_saved. Returns whether it compiled, the errors and warnings - each with its message, line and column, the source line and OpenVAF's marks - the .osdi written and its modules; describe_component_type with a module's name then lists its parameters.",
 "inputSchema": {"type": "object", "properties": {"file": {"type": "string"}, "unsaved": {"type": "string", "enum": ["save", "as_saved"]},
   "timeout": {"type": "integer", "description": "Seconds, 120 unless given"}}}},
{"name": "tune",
 "description": "Makes a number come out right: sets a component's property, simulates, measures, and again - searching 'range' for the value that makes the measurement 'target' (false position, on a log scale over decades: a few runs), within 'tolerance' (0.5% of the target unless given), at most 'max_runs' (12) simulations. Or 'values': each simulated and measured, a table (with a target, the closest is taken). 'measure': {\"variable\": \"tran.v(out)\", \"what\": \"final\"} - what: min, max, mean, rms, initial, final, peak_to_peak, or a measurement of get_dataset's (bandwidth, overshoot, rise_time, settling_time, frequency, gain, thd, phase_margin, ...: its value, or 'field'), 'at' an x instead, 'from' and 'to' the range, the measurement's options as get_dataset takes them; or {\"operating_point\": \"e\"} - a node's DC voltage (or a device's quantity, Q1.ic), each run the operating point alone. The value found is set as one step to undo ('apply' false leaves it as it was) and simulated, so the diagrams show it. Returns each run's value and measurement, the value found and what it gives. Sweep RE until the emitter sits at 5 V; C until the peaking is 1 dB.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string"}, "component": {"type": "string"}, "property": {"type": "string", "description": "Its first property unless given (R of a resistor); of an equation block, a variable it defines"},
   "target": {"type": "number"}, "range": {"type": "array", "items": {}, "minItems": 2, "maxItems": 2, "description": "[low, high]: numbers, or text with units (1k)"},
   "values": {"type": "array", "items": {}},
   "measure": {"type": "object"}, "tolerance": {"type": "number"}, "max_runs": {"type": "integer", "minimum": 2, "maximum": 40},
   "apply": {"type": "boolean"}, "simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"]},
   "timeout": {"type": "integer", "description": "Seconds for each run, 120 unless given"}},
   "required": ["component", "measure"]}},
{"name": "read_pdf",
 "description": "The text of a PDF - a datasheet, an application note, a report - page by page, to take a model's parameters or a table's values from it in the same window. 'path' (relative to the project, else the workspace; the PDF in front when not given); 'pages' [3, 4], \"2-5\" or 7 (the first 3 unless given); 'search' finds a word or value on every page (or those given) and returns the lines around each find. A scanned page has no text: screenshot of its tab shows it.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "pages": {}, "search": {"type": "string"}}}},
{"name": "find_library_component",
 "description": "Searches the component libraries - Qucs-S's own and the user's (user_lib), and the SPICE model files (.model cards in .lib, .mod, .inc, .cir) of the project and the workspace - for a part by what it is and by its values: 'search' words in its name or description (2N3904, NPN 40V), 'type' npn, pnp, nmos, pmos, njf, pjf, diode (or a Qucs model: _BJT, _MOSFET, Diode, ...), 'near' values of its parameters ({\"Bf\": 200}: the nearest first, on a log scale), 'library' one library. Returns each part with its library, description, the values asked about, and how to place it: a Qucs library part is add_component with type Lib and its Lib and Comp; a SPICE model comes with its .model card. A plain resistor, capacitor or inductor is add_component R, C or L with its value.",
 "inputSchema": {"type": "object", "properties": {"search": {"type": "string"}, "type": {"type": "string"}, "near": {"type": "object"},
   "library": {"type": "string"}, "limit": {"type": "integer", "minimum": 1, "maximum": 100}}}},
{"name": "new_project",
 "description": "Makes a project in the workspace (NAME_prj with its Scratch folder, as Project > New Project does) and opens it unless 'open' is false - opening closes the documents, so it is not opened while one has unsaved changes. Relative paths are taken from the open project.",
 "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "open": {"type": "boolean"}}, "required": ["name"]}},
{"name": "open_project",
 "description": "Opens a project of the workspace ('name': amp or amp_prj, or a project's folder), as Project > Open Project does: the documents are closed first (refused while one has unsaved changes). Relative paths are taken from it after.",
 "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]}},
{"name": "copy_document",
 "description": "Copies a schematic ('path', open or not - an open one as it is, unsaved changes too) to 'to': a name beside it (amp2), a path, or a folder or project to copy it into - with its results unless 'results' is false: its datasets (each simulator's) and its data display, renamed with it and pointing at each other. 'replace' writes over a copy there. Returns the files written.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "to": {"type": "string"}, "results": {"type": "boolean"},
   "replace": {"type": "boolean"}}, "required": ["to"]}},
{"name": "clean_scratch",
 "description": "Clears a schematic's scratch files - its subfolder of the project's Scratch folder: the netlists, the simulator's output and logs its runs left - to the system's trash; with 'datasets', its datasets too (name.dat, .dat.ngspice, ...). Made again by the next run.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "datasets": {"type": "boolean"}}}},
{"name": "make_symbol",
 "description": "Draws a subcircuit's symbol anew: a box with each of its ports on a side - 'sides' by port name or number ({\"in\": \"left\", \"out\": \"right\", \"vdd\": \"top\", \"gnd\": \"bottom\"}); a port not given goes by its name (a supply - vdd, vcc, v+ - on the top, a ground or negative supply - gnd, vss, v- - on the bottom) or its type (in left, out right), the rest left and right in turn - its name text below. The document shows its symbol after (as Edit Circuit Symbol does). One step to undo. To finish what create_subcircuit began.",
 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "sides": {"type": "object", "additionalProperties": {"type": "string", "enum": ["left", "right", "top", "bottom"]}}}}},
{"name": "import_netlist",
 "description": "Makes a schematic of a SPICE netlist ('text', or 'file': .cir, .sp, .net), in a new document: each element a SPICE part of its kind carrying its netlist text as written (R_SPICE, C_SPICE, S4Q_V for a source - SIN, PULSE and all - NPN_SPICE with its model, NMOS_SPICE, DIODE_SPICE, VCVS for a linear E, SPICE_dev for an X instance, K_SPICE, ...), placed in rows, each pin's net a net label on it and node 0 a ground; its .model cards in SpiceModel blocks, .param, .options, .include and .lib as blocks, .tran, .ac and .op as analyses, its .subckt definitions into a library file beside it, included. Rough but simulated as the netlist was: move and connect tidy it. The first line is the title unless it reads as an element ('title_line'). 'save_as' saves it. Returns the parts, nets and what was not taken.",
 "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}, "file": {"type": "string"}, "save_as": {"type": "string"},
   "title_line": {"type": "boolean"}, "spacing": {"type": "integer", "minimum": 120, "maximum": 600}}}},
{"name": "set_simulator",
 "description": "Chooses the simulator that simulate runs and get_netlist writes for, as the toolbar's list of simulators does (a setting kept for next time): ngspice, xyce, spiceopus or qucsator - one that is installed. For one run of another, simulate takes 'simulator'. To compare two engines: simulate, then simulate with 'simulator' - get_dataset with 'simulator' reads each. Returns the simulator in use and those installed.",
 "inputSchema": {"type": "object", "properties": {"simulator": {"type": "string", "enum": ["ngspice", "xyce", "spiceopus", "qucsator"]}}, "required": ["simulator"]}}
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
    {"make_symbol", QT_TRANSLATE_NOOP("QucsControl", "draw a subcircuit's symbol in Qucs-S")},
    {"import_netlist", QT_TRANSLATE_NOOP("QucsControl", "make a schematic of a netlist in Qucs-S")},
};

// Tools that only look (or move the view): used without asking.
const char* const kReadOnly[] = {"get_state", "get_schematic", "screenshot", "list_component_types", "list_actions",
                                 "get_dialog", "show_document", "select", "zoom", "get_netlist", "get_dataset",
                                 "reload_data", "describe_component_type", "describe_format", "list_documents", "check_schematic",
                                 "read_pdf", "find_library_component", "undo_history"};

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
    return ca == cb;
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

// Sets properties by name; false and which are unknown in \a error.
bool setProperties(Component* c, const QJsonObject& properties, QString* error)
{
    QStringList unknown;
    for (auto it = properties.begin(); it != properties.end(); ++it)
        if (c->getProperty(it.key()) == nullptr) unknown << it.key();
    if (!unknown.isEmpty()) {
        *error = noSuchProperty(c, unknown.join(QStringLiteral(", ")));
        return false;
    }
    for (auto it = properties.begin(); it != properties.end(); ++it) c->getProperty(it.key())->Value = propertyValue(it.value());
    return true;
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
// A value null or "" takes that one away.
using Equations = QList<std::pair<QString, QString>>;
bool equationsOf(const QJsonValue& v, Equations* out, QString* error)
{
    const auto take = [&](const QString& name, const QJsonValue& value) {
        const QString n = name.trimmed();
        if (n.isEmpty() || n.contains(QLatin1Char('=')) || n.contains(QLatin1Char('"')) || n.contains(QLatin1Char('\n'))) {
            *error = tr("An equation's name is its variable - letters, digits, _ - with no = or quotes (%1).").arg(name);
            return false;
        }
        if (!value.isNull() && !value.isString() && !value.isDouble()) {
            *error = tr("%1's value is its expression, as text.").arg(n);
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
            if (eq <= 0) {
                *error = tr("\"%1\" is not name=expression.").arg(s);
                return false;
            }
            if (!take(s.left(eq), s.mid(eq + 1))) return false;
        } else if (e.isArray() && e.toArray().size() == 2 && e.toArray().at(0).isString()) {
            if (!take(e.toArray().at(0).toString(), e.toArray().at(1))) return false;
        } else if (e.isObject() && e.toObject().value(QLatin1String("name")).isString()) {
            if (!take(e.toObject().value(QLatin1String("name")).toString(), e.toObject().value(QLatin1String("value")))) return false;
        } else {
            *error = tr("Each of 'equations' is \"name=expression\", [name, expression] or {\"name\": ..., \"value\": ...}.");
            return false;
        }
    }
    return true;
}

// Sets an equation block's equations: each one given takes its value (a
// new one goes after the others), an empty one goes; with \a replace they
// are the equations given alone. The fixed fields stay where they are.
void setEquations(Component* c, const Equations& equations, bool replace)
{
    if (replace)
        for (int i = int(c->Props.size()) - 1; i >= 0; --i)
            if (!isFixedField(c->Props.at(i))) delete c->Props.takeAt(i);
    for (const auto& [name, value] : equations) {
        int at = -1;
        for (int i = 0; i < c->Props.size(); ++i)
            if (!isFixedField(c->Props.at(i)) && c->Props.at(i)->Name == name) at = i;
        if (value.isEmpty()) {
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

// 'equations' (with 'replace_equations'), 'records' and 'specs' on a
// component (a \a fresh one: being placed): checked, and applied unless
// \a check. False and why when they do not read or the component takes none.
bool setListsOf(Component* c, const QJsonObject& args, QString* error, bool check, bool fresh = false)
{
    const bool eq = args.contains(QLatin1String("equations")), rec = args.contains(QLatin1String("records")),
               spec = args.contains(QLatin1String("specs"));
    if (!eq && !rec && !spec) return true;
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
    if (eq && !equationsOf(args.value(QLatin1String("equations")), &equations, error)) return false;
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
    if (args.contains(QLatin1String("text_at"))) {
        const QJsonArray a = args.value(QLatin1String("text_at")).toArray();
        if (a.size() != 2 || !a.at(0).isDouble() || !a.at(1).isDouble() || std::abs(a.at(0).toDouble()) > 100000
            || std::abs(a.at(1).toDouble()) > 100000) {
            *error = tr("'text_at' is [dx, dy]: where its text begins (the top left corner) from its centre.");
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

namespace {

// \a a with \a b's fields too.
QJsonObject mergedJson(QJsonObject a, const QJsonObject& b)
{
    for (auto it = b.begin(); it != b.end(); ++it) a.insert(it.key(), it.value());
    return a;
}

QJsonObject issueJson(const qucs_s::erc::Issue& i)
{
    QJsonObject o{{QStringLiteral("message"), i.message}, {QStringLiteral("at"), QJsonArray{i.where.x(), i.where.y()}}};
    if (!i.component.isEmpty()) o.insert(QStringLiteral("component"), i.component);
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
    else if (tool == QLatin1String("export_image"))
        subject = s("save_as") + (a.contains(QLatin1String("diagram")) ? tr(" (diagram %1)").arg(a.value(QLatin1String("diagram")).toInt()) : QString());
    else if (tool == QLatin1String("set_simulator")) subject = s("simulator");
    else if (tool == QLatin1String("build_verilog_a")) subject = s("file").isEmpty() ? tr("the .va in front") : s("file");
    else if (tool == QLatin1String("new_project") || tool == QLatin1String("open_project")) subject = s("name");
    else if (tool == QLatin1String("import_netlist")) subject = s("file").isEmpty() ? tr("%1 lines").arg(s("text").count(QLatin1Char('\n')) + 1) : s("file");
    else if (tool == QLatin1String("copy_document")) subject = (s("path").isEmpty() ? tr("the schematic in front") : s("path")) + QStringLiteral(" → ") + s("to");
    else if (tool == QLatin1String("clean_scratch")) subject = (s("path").isEmpty() ? tr("the schematic in front") : s("path"))
                                                               + (a.value(QLatin1String("datasets")).toBool() ? tr(", datasets too") : QString());
    else if (tool == QLatin1String("read_pdf")) subject = s("search").isEmpty() ? s("path") : tr("%1 in %2").arg(s("search"), s("path"));
    else if (tool == QLatin1String("find_library_component")) subject = (s("type") + QLatin1Char(' ') + s("search")).trimmed();
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
        "These tools drive the Qucs-S window the user is looking at. get_state says what is open. Documents: "
        "open_document, show_document, new_document, save_document, close_document. A schematic: get_schematic "
        "(its components with their pins' places, or its .sch text), then change it with add_component, "
        "edit_component, connect (pin to pin, e.g. \"R1.2\" to \"C1.1\"), add_wire, set_label, delete, or "
        "set_schematic (new .sch sections at once); select, zoom, undo, redo; screenshot to see it. "
        "list_component_types gives add_component's types, describe_component_type a type's properties. "
        "rename_net renames a net with the traces that show it. batch runs several of these in one call: use it for "
        "several changes at once (place and wire parts, set properties, add a diagram and its traces) rather than a "
        "call each. Menus: list_actions, trigger_action; a dialog it opens "
        "is read with get_dialog and answered with set_dialog. simulate runs the simulator and reports its errors; "
        "get_netlist gives the netlist. get_dataset reads the results as numbers and measures them (rise time, "
        "overshoot, values at a time, ...): use it rather than a screenshot to tell what a simulation gave. "
        "Diagrams: add_diagram, edit_diagram, add_trace, edit_trace, delete; markers: add_marker (at x, the peak, "
        "-3 dB, a crossing), edit_marker, delete_marker; reload_data reads the data again. Paintings - texts, arrows, "
        "boxes, text boxes, tables, dimensions, formulas - to mark up a result or draw a symbol: add_painting, "
        "edit_painting, delete. list_documents lists the files of the workspace or a project; export_image writes a "
        "picture of a schematic or one diagram; set_simulator chooses the simulator. tune sets a part's value, "
        "simulates and measures until a number comes out right - one call for what took three an iteration. "
        "build_verilog_a compiles a .va now, with its errors' lines; describe_component_type tells a Verilog-A "
        "module's parameters. find_library_component finds a part by its values (an NPN with Bf near 200); read_pdf "
        "reads a datasheet's text; import_netlist makes a schematic of a SPICE netlist; make_symbol draws a "
        "subcircuit's symbol. new_project, open_project, copy_document, clean_scratch tend the files. undo_history "
        "tells the steps to undo in words. \"selection\": true takes what the user selected (move, delete, "
        "create_subcircuit, get_schematic). What the user changes between your calls is told part by part. Changes "
        "appear in the window at once, each one step to undo: prefer these tools to editing the file of a schematic "
        "that is open. Coordinates are the schematic's (grid 10 as a rule; place pins on it). The system's file and "
        "print dialogs cannot be filled: use open_document and save_document instead. A document is its path "
        "(relative to the open project's folder, else the workspace) or, when it is open, its file's name alone (amp.sch). "
        "A conversation the user has pinned to a schematic says so in its prompts: then the tools act on that schematic when given no path, "
        "whichever document is in front, and trigger_action brings it to the front first.");
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

void QucsControl::callToolFor(quint64 caller, const QString& tool, const QJsonObject& arguments,
                              std::function<void(const QJsonObject&)> done)
{
    const QStringList changes = changesSince(caller);
    a_callers.append(caller);
    QucsDoc::setEditor(caller);
    callTool(tool, arguments, [this, caller, changes, done](const QJsonObject& r) {
        a_callers.removeOne(caller);
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
        int byUser = 0, byOthers = 0;
        QDateTime last;
        for (const QucsDoc::Edit& e : doc->recentEdits()) {
            if (e.revision <= it->revision || e.by == caller) continue;
            (e.by == 0 ? byUser : byOthers)++;
            last = e.at;
        }
        if (byUser + byOthers > 0) {
            const QString who = byOthers == 0 ? tr("the user") : byUser == 0 ? tr("another conversation") : tr("the user and another conversation");
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
    // A dialog waiting for an answer - the user's, editing a part or a
    // diagram, or one Claude opened - holds on to what it edits: a change
    // under it (an undo rebuilds the whole schematic) freed what it held.
    // Only what looks, and what answers the dialog, runs until it closes.
    static const QSet<QString> whileADialogWaits{
        QStringLiteral("get_state"), QStringLiteral("get_schematic"), QStringLiteral("screenshot"),
        QStringLiteral("list_component_types"), QStringLiteral("list_actions"), QStringLiteral("get_dialog"),
        QStringLiteral("set_dialog"), QStringLiteral("get_netlist"), QStringLiteral("get_dataset"),
        QStringLiteral("describe_component_type"), QStringLiteral("describe_format"), QStringLiteral("batch"),
        QStringLiteral("list_documents"), QStringLiteral("check_schematic"), QStringLiteral("read_pdf"),
        QStringLiteral("find_library_component"), QStringLiteral("undo_history")};
    if (QWidget* dialog = QApplication::activeModalWidget(); dialog != nullptr && !whileADialogWaits.contains(tool))
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
    if (tool == QLatin1String("delete")) return remove(args);
    if (tool == QLatin1String("move")) return moveGroup(args);
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
    if (tool == QLatin1String("undo_history")) return undoHistory(args);
    if (tool == QLatin1String("new_project")) return newProject(args);
    if (tool == QLatin1String("open_project")) return openProject(args);
    if (tool == QLatin1String("copy_document")) return copyDocument(args);
    if (tool == QLatin1String("clean_scratch")) return cleanScratch(args);
    if (tool == QLatin1String("make_symbol")) return makeSymbol(args);
    if (tool == QLatin1String("import_netlist")) return importNetlist(args);
    if (tool == QLatin1String("find_library_component")) return findLibraryComponent(args);
    async = true;
    if (tool == QLatin1String("batch")) runBatch(args, done);
    else if (tool == QLatin1String("trigger_action")) triggerAction(args, done);
    else if (tool == QLatin1String("set_dialog")) setDialog(args, done);
    else if (tool == QLatin1String("simulate")) simulate(args, done);
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
    return pane != nullptr ? cleanText(pane->tabText(pane->indexOf(w))) : QString();
}

QJsonObject QucsControl::withSelection(const QString& tool, const QJsonObject& args, QString* error) const
{
    Schematic* sch = schematic(args, error, false);
    if (sch == nullptr) return {};
    error->clear();
    QJsonArray names, diagrams, paintings, wires;
    QRect bounds;
    for (Component* c : sch->a_DocComps)
        if (c->isSelected) {
            names.append(c->Name);
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
        const int n = where.toInt();
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
        if (!pinned.isEmpty() && !doc->getDocName().isEmpty() && sameFile(doc->getDocName(), absolute(pinned))) {
            d.insert(QStringLiteral("this conversation's document"), true);
            pinnedOpen = true;
        }
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
        region = QRect(QPoint(r.at(0).toInt(), r.at(1).toInt()), QPoint(r.at(2).toInt(), r.at(3).toInt())).normalized();
    }
    const bool filtered = !wantedNames.isEmpty() || region.isValid();
    const auto wanted = [&](Component* c) {
        if (!wantedNames.isEmpty() && !wantedNames.contains(c->Name)) return false;
        return !region.isValid() || region.contains(c->cx, c->cy);
    };
    // What most can take in: a schematic of thousands of parts is listed
    // in part, and says how to see the rest.
    constexpr int kMost = 200;
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
    }
    for (Component* c : sch->a_DocComps) {
        const QString base = keyBase(c, seen);
        if (!wanted(c)) continue;
        if (components.size() >= kMost) {
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
            if (!netNames.contains(*it)) netNames.insert(*it, QStringLiteral("net%1").arg(order.size() + 1));
            if (!order.contains(*it)) order << *it;
            QJsonObject pin = pins.at(i).toObject();
            pin.insert(QStringLiteral("net"), netNames.value(*it));
            pins[i] = pin;
            netsListed.insert(*it);
        }
        json.insert(QStringLiteral("pins"), pins);
        if (refs.contains(c)) json.insert(QStringLiteral("ref"), refs.value(c));
        components.append(json);
    }
    // The pins on each net: of every component (a net's pins are all of
    // them, whichever are listed).
    {
        QHash<QString, int> again;
        for (Component* c : sch->a_DocComps) {
            const QString base = keyBase(c, again);
            for (int i = 0; i < c->Ports.size(); ++i) {
                const auto it = nets.netOf.constFind(base + QLatin1Char('.') + QString::number(i + 1));
                if (it == nets.netOf.constEnd()) continue;
                if (!netNames.contains(*it)) netNames.insert(*it, QStringLiteral("net%1").arg(order.size() + 1));
                if (!order.contains(*it)) order << *it;
                pinsOn[*it] << QStringLiteral("%1.%2").arg(refs.value(c, c->Name.isEmpty() ? c->Model : c->Name)).arg(i + 1);
            }
        }
    }
    int omittedNets = 0, omittedWires = 0;
    for (int id : order) {
        if (filtered && !netsListed.contains(id)) continue;
        if (pinsOn.value(id).size() > 1 || !netNames.value(id).startsWith(QLatin1String("net"))) {
            if (netList.size() >= kMost) {
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
        if (wires.size() >= kMost) {
            ++omittedWires;
            continue;
        }
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
    // Each painting by its fields (its type, place, text, look), numbered
    // as the painting tools take them.
    paintings = paintingsJson(sch->a_DocPaints, kMost);
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
    if (omittedPaintings > 0) left << tr("%1 more paintings").arg(omittedPaintings);
    if (!left.isEmpty()) {
        QString note = tr("Left out: %1.").arg(left.join(QStringLiteral("; ")));
        if (omittedComponents + omittedNets + omittedWires > 0)
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
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)},
                       {QStringLiteral("errors"), errors},
                       {QStringLiteral("warnings"), warnings},
                       {QStringLiteral("notes"), notes},
                       {QStringLiteral("found"), tr("%1 errors, %2 warnings, %3 notes").arg(errors.size()).arg(warnings.size()).arg(notes.size())}};
    if (errors.isEmpty() && warnings.isEmpty())
        result.insert(QStringLiteral("verdict"), notes.isEmpty() ? tr("Nothing found.") : tr("Nothing wrong found; the notes are fine if meant."));
    return jsonResult(result);
}

QJsonObject QucsControl::setSchematic(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    prepare(sch);
    const QString text = args.value(QLatin1String("text")).toString();
    QStringList short_;
    if (!sch->replaceContent(text, &error, &short_))
        return errorResult(tr("Not changed: %1").arg(error));
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
             "zAuto zMin zStep zMax rotX rotY rotZ notation yUnits zUnits legend decimals [extra] \"xLabel\" \"yLabel\" \"zLabel\" [\"title\"]>\n"
             "  <\"variable\" ...> a trace, one line each (see trace), each followed by its markers (see marker)\n"
             "</Type>\n"
             "Type: Rect, Polar, Smith, ySmith, PS, SP, Tab, Time, Truth, Rect3D, Curve (locus), Histogram. x y: its lower left corner; "
             "width height. flags: 1 grid on, plus 2 to hide the lines (a table). gridColor: #rrggbb; gridStyle: a Qt pen style "
             "(1 solid, 2 dash, 3 dot, ...). logs: two digits written together - the x axis logarithmic (1) or not (0), then "
             "the y and right y axes (1 y, 2 right y, 3 both): \"10\" is a log x axis. Each axis (x, y, right y = z): "
             "automatic (1) or not (0), and its minimum, step and maximum (used when not automatic). rotX rotY rotZ: the "
             "view of a 3D diagram. notation of the numbers: 0 automatic, 1 engineering, 2 scientific, 3 engineering "
             "exponent. yUnits zUnits: of a log y axis, 0 none, 1 dB, 2 dBuV, 3 dBm. legend: 0 off, 1 top left, 2 top "
             "right, 3 bottom left, 4 bottom right. decimals: -1 automatic. extra, a histogram's only: bins, height (0 counts, "
             "1 percent, 2 density), flags (1 normal fit, 2 statistics), lower and upper limit. The labels last, in quotes "
             "(x, y, right y), then the title in quotes when it has one. add_diagram and edit_diagram set all of this by name.")},
        {QStringLiteral("trace"), QStringLiteral(
             "<\"variable\" #rrggbb thickness precision numbers style axis [autoColor [pointMarker]]>\n"
             "variable: as a trace names it (ngspice/tran.v(out)). precision: a table's digits. numbers of complex values in "
             "a table: 0 real/imaginary, 1 magnitude/degrees, 2 magnitude/radians. style: 0 solid, 1 dash, 2 dot, 3 long "
             "dash, 4 stars, 5 circles, 6 arrows. axis: 0 left, 1 right. autoColor: 1 each swept curve a colour of its own. "
             "pointMarker: 0 none, 1 auto, 2 circle, 3 square, 4 triangle, 5 diamond, 6 triangle down, 7 cross, 8 plus. "
             "add_trace and edit_trace set all of this by name.")},
        {QStringLiteral("marker"), QStringLiteral(
             "<Mkr x[/y...] labelX labelY precision numbers transparent [indicator [textColor fillColor]]>\n"
             "On the line after its trace's. x: the value of the independent variable it marks (and of a swept one after "
             "a /); it shows the sample nearest it. labelX labelY: the top left corner of its box, from the diagram's "
             "lower left corner (y up is negative). precision: digits. numbers: 0 real/imaginary, 1 magnitude/degrees, "
             "2 magnitude/radians. transparent: 1 a clear background. indicator: 0 off, 1 square, 2 triangle (the default). "
             "textColor fillColor: #rrggbb or #aarrggbb, - for automatic. add_marker and edit_marker set all of this, and "
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
    if (!setListsOf(c, args, &error, false, true) || !setProperties(c, args.value(QLatin1String("properties")).toObject(), &error)
        || !setTextOf(c, args, &error, false)) {
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
    sch->enlargeView(c);
    finish(sch, {QPoint(c->cx, c->cy)});
    QJsonObject result = componentJson(c);
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
    if (!notes.isEmpty()) result.insert(QStringLiteral("note"), notes.join(QStringLiteral("; ")) + QLatin1Char('.'));
    return jsonResult(result);
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
        if (c->getProperty(it.key()) == nullptr) return errorResult(noSuchProperty(c, it.key()));
    if (!setTextOf(c, args, &error, false) || !setListsOf(c, args, &error, true)) return errorResult(error);
    prepare(sch);
    const QString before = sch->snapshot();
    const QList<qucs_s::erc::Issue> wiringBefore = qucs_s::erc::wiring(sch);
    if (!props.isEmpty() || args.contains(QLatin1String("equations")) || args.contains(QLatin1String("records"))
        || args.contains(QLatin1String("specs"))) {
        setProperties(c, props, &error);
        setListsOf(c, args, &error, false);
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
    sch->enlargeView(c);
    finish(sch, {QPoint(c->cx, c->cy)});
    QJsonObject result = componentJson(c);
    if (!renamedToo.isEmpty()) result.insert(QStringLiteral("renamed too"), QJsonArray::fromStringList(renamedToo));
    if (!rewritten.isEmpty()) result.insert(QStringLiteral("rewritten"), rewritten);
    if (!renamedToo.isEmpty() || !rewritten.isEmpty())
        result.insert(QStringLiteral("dataset"), tr("The dataset still names it %1: the traces show it again after the next simulation.")
                                                     .arg(name));
    landed << newWiringIssues(sch, wiringBefore);
    if (!landed.isEmpty()) result.insert(QStringLiteral("note"), landed.join(QStringLiteral("; ")) + QLatin1Char('.'));
    return jsonResult(result);
}

QJsonObject QucsControl::moveGroup(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    QList<Component*> group;
    QStringList missing;
    for (const QJsonValue& v : args.value(QLatin1String("names")).toArray()) {
        Component* c = sch->getComponentByName(v.toString().trimmed());
        if (c == nullptr) missing << v.toString();
        else if (!group.contains(c)) group << c;
    }
    if (!missing.isEmpty()) return errorResult(tr("There is no component %1.").arg(missing.join(QStringLiteral(", "))));
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
    if (dx == 0 && dy == 0) return errorResult(tr("Nothing to move: dx and dy are 0 (or less than a step of the grid, %1).").arg(gx));

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
        return errorResult(tr("Not moved: moved so, %1. Try another dx, dy.").arg(changes.join(QStringLiteral("; "))));
    }
    QList<QPoint> where;
    for (Component* c : std::as_const(group)) where << QPoint(c->cx, c->cy);
    finish(sch, where);
    QStringList names;
    for (Component* c : std::as_const(group)) names << c->Name;
    QJsonObject result{{QStringLiteral("moved"), QJsonArray::fromStringList(names)},
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
    const QJsonArray plot = args.value(QLatin1String("plot")).toArray();
    if (!plot.isEmpty() && kind == QLatin1String("op"))
        return errorResult(tr("An operating point has nothing to plot: simulate with operating_point, or get_dataset reads it."));
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
    if (!plot.isEmpty()) {
        // What the dataset will call them: ac.v(out) for out, v(out) or ac.v(out).
        const bool qucsator = QucsSettings.DefaultSimulator == spicecompat::simQucsator;
        QJsonArray traces;
        for (const QJsonValue& v : plot) {
            QString var = v.toString().trimmed();
            if (var.isEmpty()) continue;
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

QJsonObject QucsControl::createSubcircuit(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    if (sch->getDocName().isEmpty()) return errorResult(tr("%1 has no file yet: save it first (the subcircuit's file goes beside it).").arg(titleOf(sch)));
    QList<Component*> group;
    QStringList missing;
    for (const QJsonValue& v : args.value(QLatin1String("names")).toArray()) {
        Component* c = sch->getComponentByName(v.toString().trimmed());
        if (c == nullptr) missing << v.toString();
        else if (c->isSimulation) return errorResult(tr("%1 is an analysis: a subcircuit holds the circuit, its analyses stay outside.").arg(c->Name));
        else if (!group.contains(c)) group << c;
    }
    if (!missing.isEmpty()) return errorResult(tr("There is no component %1.").arg(missing.join(QStringLiteral(", "))));
    if (group.isEmpty()) return errorResult(tr("'names' are the components that go into the subcircuit."));
    QString file = args.value(QLatin1String("save_as")).toString().trimmed();
    if (file.isEmpty()) return errorResult(tr("'save_as' names the subcircuit's file (a .sch beside this schematic)."));
    const QDir here = QFileInfo(sch->getDocName()).absoluteDir();
    if (!file.endsWith(QLatin1String(".sch"), Qt::CaseInsensitive)) file += QStringLiteral(".sch");
    file = QFileInfo(file).isAbsolute() ? QDir::cleanPath(file) : QDir::cleanPath(here.filePath(file));
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
            if (label.isEmpty()) {
                int n = 1;
                const QString base = QFileInfo(file).completeBaseName();
                while (labelsInUse.contains(QStringLiteral("%1_n%2").arg(base).arg(n))) ++n;
                label = QStringLiteral("%1_n%2").arg(base).arg(n);
                labelsInUse.insert(label);
            }
            boundaries.append({net, label, at, said(key, {})});
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
        wires << labelLine(pin, boundaries.at(k).label) << labelLine(boundaries.at(k).at, boundaries.at(k).label);
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
    {
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return errorResult(tr("%1 could not be written.").arg(QDir::toNativeSeparators(file)));
        out.write(text.toUtf8());
    }

    // In this schematic: the group gives way to one subcircuit, its pins
    // joined to the nets by labels of the same names - one step to undo.
    std::unique_ptr<Component> sub{newComponent(QStringLiteral("Sub"))};
    if (!sub) return errorResult(tr("The library has no subcircuit component."));
    sub->setSchematic(sch);
    sub->Props.first()->Value = here.relativeFilePath(file);
    QString subName = args.value(QLatin1String("name")).toString().trimmed();
    if (subName.isEmpty()) {
        int n = 1;
        while (sch->getComponentByName(QStringLiteral("SUB%1").arg(n)) != nullptr) ++n;
        subName = QStringLiteral("SUB%1").arg(n);
    } else if (sch->getComponentByName(subName) != nullptr && !in.contains(sch->getComponentByName(subName))) {
        return errorResult(tr("There is a component named %1 already.").arg(subName));
    }
    sub->Name = subName;
    sub->recreate();
    if (sub->Ports.size() != boundaries.size())
        return errorResult(tr("The subcircuit written has %1 pins where %2 were meant: nothing changed here (%3 is written).")
                               .arg(sub->Ports.size()).arg(boundaries.size()).arg(QFileInfo(file).fileName()));
    int cx = box.center().x(), cy = box.center().y();
    sch->setOnGrid(cx, cy);
    sub->moveCenter(cx - sub->cx, cy - sub->cy);
    // What the group leaves behind with nothing to do: a ground that was
    // on its pins alone, a wire to where one of its pins was (and on from
    // there, as far as it goes to nothing) - not the ends the labels keep.
    QSet<QPoint> gone;
    for (Component* c : std::as_const(group))
        for (const Port* p : c->Ports) gone.insert(QPoint(c->cx + p->x, c->cy + p->y));
    QSet<QPoint> labelled;
    for (const Boundary& b : std::as_const(boundaries)) labelled.insert(b.at);
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
    QStringList mine, theirWires;
    for (Component* c : std::as_const(kept)) mine << QStringLiteral("  ") + c->save();
    mine << QStringLiteral("  ") + sub->save();
    for (Wire* w : std::as_const(wiresKept)) theirWires << QStringLiteral("  ") + w->save();
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel() && !insideNodes.contains(n)) theirWires << QStringLiteral("  ") + n->label()->save();
    QJsonArray ports;
    for (int k = 0; k < boundaries.size(); ++k) {
        const QPoint pin(sub->cx + sub->Ports.at(k)->x, sub->cy + sub->Ports.at(k)->y);
        theirWires << labelLine(boundaries.at(k).at, boundaries.at(k).label) << labelLine(pin, boundaries.at(k).label);
        ports.append(QJsonObject{{QStringLiteral("port"), k + 1}, {QStringLiteral("net"), boundaries.at(k).label},
                                 {QStringLiteral("was at"), boundaries.at(k).pin}});
    }
    // (Their names now: the schematic is made anew below, and they with it.)
    QStringList names;
    for (Component* c : std::as_const(group)) names << c->Name;
    prepare(sch);
    const QString sections = QStringLiteral("<Components>\n%1\n</Components>\n<Wires>\n%2</Wires>\n")
                                 .arg(mine.join(QLatin1Char('\n')), theirWires.isEmpty() ? QString() : theirWires.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    QString why;
    if (!sch->replaceContent(sections, &why))
        return errorResult(tr("The schematic could not take the subcircuit (%1 is written): %2").arg(QFileInfo(file).fileName(), why));
    QJsonObject result{{QStringLiteral("subcircuit"), QDir::toNativeSeparators(file)},
                       {QStringLiteral("instance"), subName},
                       {QStringLiteral("moved in"), QJsonArray::fromStringList(names)},
                       {QStringLiteral("ports"), ports},
                       {QStringLiteral("grounds inside"), int(grounded.size())},
                       {QStringLiteral("one step to undo"), true},
                       {QStringLiteral("note"), tr("Its pins are joined to their nets by net labels. The file stays written when this is "
                                                   "undone. open_document opens it; its symbol is a box with a pin for each port until one "
                                                   "is drawn (add_painting with symbol).")}};
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
    if (dot <= 0) {
        *error = tr("%1 is not a pin (\"R1.2\") or a place ([x, y]).").arg(pin);
        return false;
    }
    const QString name = pin.left(dot), which = pin.mid(dot + 1);
    Component* c = sch->getComponentByName(name);
    if (c == nullptr) {
        // A part without a name (a ground): by its type, when it is the
        // only one of it - else by its number among them, GND#2 (as
        // get_schematic's 'ref' gives it).
        QString model = name;
        int nth = 0;
        if (const qsizetype hash = name.indexOf(QLatin1Char('#')); hash > 0) {
            model = name.left(hash);
            nth = name.mid(hash + 1).toInt();
        }
        QList<Component*> ofType;
        for (Component* pc : sch->a_DocComps)
            if (pc->Model == model && (pc->Name.isEmpty() || pc->Name == QLatin1String("*"))) ofType << pc;
        if (nth > 0) {
            if (nth > ofType.size()) {
                *error = tr("There are %1 of %2, not %3.").arg(ofType.size()).arg(model).arg(nth);
                return false;
            }
            c = ofType.at(nth - 1);
        } else if (ofType.size() == 1) c = ofType.first();
        else if (ofType.size() > 1) {
            *error = tr("There are %1 of %2: say which, %2#1.1 to %2#%1.1 (get_schematic gives each its 'ref'), or give the pin's place, [x, y].")
                         .arg(ofType.size()).arg(model);
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
    const QList<qucs_s::erc::Issue> wiringBefore = qucs_s::erc::wiring(sch);
    if (!wireUp(sch, a, b, check, &error))
        return errorResult(tr("Not wired: %1. Give the way with add_wire, or move a part out of it.").arg(error));
    finish(sch, {a, b});
    QString text = tr("Wired %1 to %2 (%3, %4 to %5, %6), going over no other pin or wire.")
                       .arg(from, to).arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
    if (const QStringList look = newWiringIssues(sch, wiringBefore); !look.isEmpty())
        text += QLatin1Char(' ') + tr("Look: %1.").arg(look.join(QStringLiteral("; ")));
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
    for (const QJsonValue& v : args.value(QLatin1String("names")).toArray()) {
        if (Component* c = sch->getComponentByName(v.toString().trimmed())) {
            c->isSelected = true;
            ++n;
        } else {
            missing << v.toString();
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
        const int last = int(schematicDoc->undoStates().size()) - 1;
        if (!args.value(QLatin1String("to")).isDouble() || to < 0 || to > last)
            return errorResult(tr("'to' is a step from 0 (as loaded) to %1, as undo_history lists them.").arg(last));
        if (to == at) return textResult(tr("It is at step %1 already.").arg(at));
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
    // What that did, part by part.
    if (schematicDoc != nullptr) {
        const QStringList what = describeChanges(before, schematicDoc->snapshot(), 10);
        if (!what.isEmpty()) text += QLatin1Char(' ') + tr("Now: %1.").arg(what.join(QStringLiteral("; ")));
        text += QLatin1Char(' ') + tr("(Step %1 of %2.)").arg(schematicDoc->undoIndex()).arg(schematicDoc->undoStates().size() - 1);
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
            wanted = QRect(QPoint(r.at(0).toInt(), r.at(1).toInt()), QPoint(r.at(2).toInt(), r.at(3).toInt())).normalized();
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
    QString root = workspace;
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
            f.insert(QStringLiteral("simulator"), e.simulator);
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
            for (const Shown& t : tracesOf.value(e.info.absoluteFilePath()))
                if (!QFileInfo::exists(t.file) && missing.size() < 20)
                    missing.append(QJsonObject{{QStringLiteral("trace"), t.trace}, {QStringLiteral("diagram"), t.diagram},
                                               {QStringLiteral("document"), t.document},
                                               {QStringLiteral("needs"), QFileInfo(t.file).fileName()}});
            if (!missing.isEmpty()) f.insert(QStringLiteral("traces without their dataset"), missing);
        }
        files.append(f);
    }
    QJsonObject result{{QStringLiteral("folder"), QDir::toNativeSeparators(root)}, {QStringLiteral("files"), files}};
    if (sameFile(root, workspace)) {
        QJsonArray projects;
        for (const QFileInfo& fi : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
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
    file = absolute(file);
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
        if (target) target->trigger();
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
    BatchRun(QucsControl* control, const QJsonArray& calls, bool keepGoing, bool atomic, const QList<Schematic*>& schematics,
             std::function<void(const QJsonObject&)> done)
        : QObject(control), a_control(control), a_calls(calls), a_keepGoing(keepGoing && !atomic), a_atomic(atomic),
          a_done(std::move(done))
    {
        if (atomic)
            for (Schematic* sch : schematics) a_before.append(Before{QPointer<Schematic>(sch), sch->snapshotAll(), sch->revision()});
    }

    void next()
    {
        if (a_stopped || a_next >= a_calls.size()) {
            finish();
            return;
        }
        const int index = a_next++;
        const QJsonObject call = a_calls.at(index).toObject();
        const QString tool = call.value(QLatin1String("tool")).toString();
        const QJsonObject arguments = call.value(QLatin1String("arguments")).toObject();
        const QPointer<BatchRun> self(this);
        const auto answered = [self, index, tool, arguments](const QJsonObject& result) {
            if (!self) return;
            self->record(index, tool, arguments, result);
            QTimer::singleShot(0, self, [self] {
                if (self) self->next();
            });
        };
        if (tool == QLatin1String("batch")) answered(errorResult(tr("A batch cannot hold another batch.")));
        else a_control->callTool(tool, arguments, answered);
    }

private:
    struct Before {
        QPointer<Schematic> schematic;
        QPair<QString, QString> state;
        quint64 revision;
    };

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
        a_content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                     {QStringLiteral("text"),
                                      QStringLiteral("[%1] %2%3:").arg(index + 1).arg(tool, error ? tr(" failed") : QString())}});
        for (const QJsonValue& v : result.value(QLatin1String("content")).toArray()) a_content.append(v);
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
                    back << QFileInfo(b.schematic->getDocName()).fileName();
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
    QJsonArray a_calls;
    bool a_keepGoing;
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
    (new BatchRun(this, calls, args.value(QLatin1String("keep_going")).toBool(), atomic, schematics, done))->next();
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

// The dataset a run of \a simulator wrote for \a doc since \a started:
// its file, whether it was written, its variables, a copy kept as
// \a keepAs, the data display, and the traces that show nothing.
QJsonObject QucsControl::datasetOfRun(Schematic* doc, int simulator, const QDateTime& started, const QString& keepAs,
                                      bool* written)
{
    QJsonObject result;
    const QFileInfo info(doc->getDocName());
    const QFileInfo dataset(datasetFile(info.absoluteFilePath(), info.completeBaseName() + QStringLiteral(".dat"), simulator));
    *written = dataset.isFile() && dataset.lastModified() >= started;
    result.insert(QStringLiteral("dataset"), QDir::toNativeSeparators(dataset.absoluteFilePath()));
    result.insert(QStringLiteral("dataset written"), *written);
    if (*written) {
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
            QString why;
            if (misc::isSameFile(kept, dataset.absoluteFilePath())) {
                // keep_as the schematic's own name (amp, or Amp on macOS):
                // that is the dataset itself, which a copy would destroy.
                result.insert(QStringLiteral("kept as"), tr("not kept: %1 is the dataset of this run itself - give keep_as another name")
                                                             .arg(QDir::toNativeSeparators(kept)));
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

void QucsControl::simulate(const QJsonObject& args, const Done& doneGiven)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) {
        doneGiven(errorResult(error));
        return;
    }
    if (sch->getDocName().isEmpty()) {
        doneGiven(errorResult(tr("%1 has no file yet: save_document with 'as' first.").arg(titleOf(sch))));
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
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(120), 5, 3600) * 1000;
    const QString keepAs = args.value(QLatin1String("keep_as")).toString().trimmed();
    if (!keepAs.isEmpty() && !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,64}$")).match(keepAs).hasMatch()) {
        doneGiven(errorResult(tr("'keep_as' is a name of letters, digits, _ and -.")));
        return;
    }
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
    const int previous = QucsSettings.DefaultSimulator;
    QucsSettings.DefaultSimulator = simulator;
    auto restored = std::make_shared<bool>(simulator == previous);
    const auto restore = [restored, previous] {
        if (*restored) return;
        *restored = true;
        QucsSettings.DefaultSimulator = previous;
    };
    const Done done = [doneGiven, checkText, checkErrors, checkWarnings, oneOff](const QJsonObject& r) {
        QJsonObject result = r;
        QJsonArray content = result.value(QStringLiteral("content")).toArray();
        // Into the report itself, too.
        if (content.size() == 1 && !result.value(QStringLiteral("isError")).toBool()) {
            QJsonObject report = QJsonDocument::fromJson(content.at(0).toObject().value(QStringLiteral("text")).toString().toUtf8()).object();
            if (!report.isEmpty()) {
                if (!checkErrors.isEmpty() || !checkWarnings.isEmpty()) {
                    report.insert(QStringLiteral("schematic check"), QJsonObject{{QStringLiteral("errors"), checkErrors},
                                                                                  {QStringLiteral("warnings"), checkWarnings}});
                    // The run's log begins with them: where they are read.
                    report.insert(QStringLiteral("last lines"),
                                  checkText + QStringLiteral("\n\n") + report.value(QStringLiteral("last lines")).toString());
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
        result.insert(QStringLiteral("content"), content);
        doneGiven(result);
    };

    QPointer<Schematic> doc(sch);
    // A dataset written since now: the simulator did produce something.
    const QDateTime started = QDateTime::currentDateTime().addSecs(-1);
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
            if (e.revision > revision) {
                by[whoMade(e.by, caller)]++;
                last = e.at;
            }
        QStringList who;
        for (auto it = by.cbegin(); it != by.cend(); ++it) who << tr("%1 by %2").arg(it.value()).arg(it.key());
        result.insert(QStringLiteral("changed while it ran"),
                      tr("%1 was changed while the simulation ran - %2 edit(s)%3, revision %4 to %5: its results are of the "
                         "schematic as it was when the run began, not as it is now. Simulate again for the schematic as it is.")
                          .arg(titleOf(doc), who.isEmpty() ? tr("some") : who.join(QStringLiteral(", ")),
                               last.isValid() ? tr(", the last at %1").arg(last.toString(QStringLiteral("HH:mm:ss"))) : QString())
                          .arg(revision).arg(doc->revision()));
    };

    if (simulator == spicecompat::simQucsator) {
        // Qucsator runs in a window of its own (SimMessage): its end waited
        // for as a SPICE run's is.
        const QList<SimMessage*> before = a_app->findChildren<SimMessage*>();
        QTimer::singleShot(0, a_app, [app = a_app] { app->slotSimulate(); });
        QTimer::singleShot(0, this, [=] {
            SimMessage* sim = nullptr;
            for (SimMessage* m : a_app->findChildren<SimMessage*>())
                if (!before.contains(m)) sim = m;
            if (sim == nullptr) {
                restore();
                done(errorResult(tr("The simulation with Qucsator did not start.")));
                return;
            }
            auto answered = std::make_shared<bool>(false);
            QPointer<SimMessage> message(sim);
            auto report = [=](int status, bool timedOut) {
                if (*answered) return;
                *answered = true;
                if (!timedOut) restore();
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
                    result.insert(QStringLiteral("note"), tr("Still running: its end was not waited for any longer."));
                }
                changedWhileRunning(result);
                done(jsonResult(result));
            };
            connect(sim, &SimMessage::SimulationEnded, this, [report](int status, SimMessage*) { report(status, false); });
            // Ended already (it could not start).
            if (sim->SimProcess.state() == QProcess::NotRunning && !sim->ErrText->toPlainText().trimmed().isEmpty())
                QTimer::singleShot(0, this, [report] { report(1, false); });
            QTimer::singleShot(timeout, this, [report, sim = QPointer<SimMessage>(sim), restore] {
                report(0, true);
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
    QTimer::singleShot(0, a_app, [app = a_app] { app->slotSimulateWithSpice(); });
    QTimer::singleShot(0, this, [this, done, timeout, doc, console, started, simulator, keepAs, operatingPoint, restore, changedWhileRunning, logBefore] {
        SimulationRun* run = console->currentRun();
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
        auto answered = std::make_shared<bool>(false);
        auto report = [this, done, answered, doc, console, started, simulator, keepAs, operatingPoint, restore, changedWhileRunning](SimulationRun* r, bool timedOut) {
            if (*answered) return;
            *answered = true;
            if (!timedOut) restore();
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
                if (succeeded && !written && !operatingPoint)
                    result.insert(QStringLiteral("note"), tr("The simulator reported no error but wrote no new dataset: its analyses may "
                                                             "save nothing (an operating point alone), or a post-processing step failed."));
                if (succeeded && operatingPoint && !result.contains(QStringLiteral("operating point")))
                    result.insert(QStringLiteral("note"), tr("The simulator reported no error but its operating point was not found."));
                if (!succeeded && errors.isEmpty() && !r->wasStopped())
                    result.insert(QStringLiteral("note"), tr("The simulator failed without an error message of its own: see the last lines."));
            }
            if (timedOut) result.insert(QStringLiteral("note"), tr("Still running: its end was not waited for any longer."));
            changedWhileRunning(result);
            done(jsonResult(result));
        };
        connect(run, &SimulationRun::simulated, this, [report](SimulationRun* r) { report(r, false); });
        // Put back when it ends, even after the answer went (timed out).
        connect(run, &SimulationRun::simulated, this, [restore] { restore(); });
        QTimer::singleShot(timeout, this, [report] { report(nullptr, true); });
    });
}
