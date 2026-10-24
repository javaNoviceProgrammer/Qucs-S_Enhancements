# RF 2.5D, phase A: the document, its tab and its panel

The second slice of phase A of `docs/proposals/2026-10-23-rf-2p5d-moment-method.md`. The first (`8f9ec51`)
built the model: the tree of the proposal, its own kinds, the file it is written to and read from, and the
stack read out of it in SI with every check the proposal asks for. It had no document, so nothing could
open a `.qrf25` or look at one. This slice gives it a document, a tab, a panel and a view: a user can make
a model, open one, edit it node by node with Undo behind every change, save it, and see its layout drawn
with its stack beside it. There is still no solver - phases B onwards.

## What it was

`rf25/rf25_model.{h,cpp}` and `test_rf25_model`, reachable from nothing: no window code mentioned the
model, `.qrf25` was an unknown suffix (a model dropped on the window went to the text editor, the file
being JSON), and the model had no way to be changed - no `find()`, no `add()`, no `remove()`, so a panel
could not have edited a node even if there had been one.

## What it is now

**The document** (`rf25doc.{h,cpp}`). `RF25Doc` beside `MultiphysicsDoc`: `load()`, `save()`, `writeTo()`
and `reload()` through `Model::toJson`/`fromJson`, `setModel()` and `editNode()` each one step of a
`QUndoStack` (the same whole-model command the multiphysics document uses - a model is small), the undo
stack's clean state driving `signalFileChanged`, and the window's Undo and Redo enabled from it while the
tab is in front. A read stamps the file **as it was when the bytes were taken** (`stampedAs(asRead)`,
captured before the read) and a save stamps the file it wrote (`stampedFromFile()`), which is the rule
`5c11931` left behind: stamped by the clock, a save would read as another program's change and the watcher
would throw the steps of Undo away.

The tab shows the stack in z beside the layout in x and y, and under both what the model says is still
missing (`Model::problems()`, the panel's node shows the same for the node it belongs to). `isBusy()` has
one meaning in this build: the layout file is being read.

**The layout, on the layout viewer's own canvas.** The model's Layout node names a GDSII or OASIS file,
beside the model unless the name is absolute. The document reads it with `qucs_s::layout::read()` on a
thread of its own, as `LayoutDoc` does (an 8 MB stack: a layout's hierarchy is walked by recursion), and
hands it to a `LayoutView` - the same canvas, with its zoom, pan, grid, rulers and layer styles. Each
layer of the file is drawn in the colour of the stack layer the layer map sends it to, from the viewer's
own palette (`paletteStyle`), filled for a conductor and hatched for a dielectric; a layer the map leaves
out is grey and hollow, so what is left out of the solve is plain to see. The file is read again when the
model names another file or cell, when the model is saved under a new name (the relative name then means
another file), and on demand from the toolbar, the Layout node's menu and the RF 2.5D menu.

**The stack in z** (`qucs_s::rf25::StackView`, in `rf25doc.h`) is the one thing the layout canvas cannot
show: the layers from the bottom up, each as tall as it is thick with a floor under each (a 0.6 µm metal
beside a 300 µm substrate would otherwise be a hair), a ground plane as a line below it and a cover
without height as one above, vias as bars at the right edge over the room between their metals, and the
layer the panel chose marked. A click on a layer chooses it in the tree.

**The panel** (`rf25panel.{h,cpp}`): a page of the left dock, after Multiphysics. The tree with a
status column - how thick each layer is, how many shapes the layout has (or "being read", "not read", "no
file"), how many ports, and how many things are missing on the model's own row - the settings of the node
chosen, and a node menu that adds what may go under it, renames, duplicates, disables, moves and deletes.
A part of the tree itself is none of those things. The editors are the seven kinds these nodes use:
expressions (checked against the parameters as they are typed, their value in the tooltip, red where they
do not read), texts (a layer's name offered from the stack for a via's metals and a port's conductor),
choices, flags, whole numbers, tables (the parameters and the layer map, a row at a time) and files.

**The window** (`qucs_rf25.cpp`, and the places in `qucs.cpp` and `qucs_actions.cpp` that know the kinds
of document): File > Open, the project view, a drop on the tabs, Save and Save As with the `.qrf25` filter
and suffix, the watcher reading a changed file again, the panel following the document in front, the File
Browser naming the suffix, and an RF 2.5D menu with New Model, Open Model, Read the Layout Again and Show
the RF 2.5D Panel. Simulate says there is no solver rather than doing nothing. One function decides what a
model file is - `QucsApp::isRf25File()` - and every one of those places asks it.

A document kind is only as good as the window's own actions make it, and `qucs_actions.cpp` keeps nine
lists of kinds. Edit > Undo and Redo did not know this one at first, so the window's Undo button and
Ctrl+Z did nothing on a tab whose document had a perfectly good undo stack - the tests had been calling
the document's own `undo()` and could not see it. They know it now, and so do Zoom In and Zoom Out (which
would otherwise have fallen through to the schematic's path), Select, Paste (refused), Copy (the layout as
it is drawn, as a multiphysics model copies its Graphics view), Find and Replace (nothing to search yet)
and Build Module (refused). A test drives those through the window rather than the document.

**The dock's page by page, not by number.** `SidePage::Rf25` is a member of the enum `521caaa` added for
exactly this, `sidePageName()` answers `"rf 2.5d"` for it, and the page registers itself as it is built.
Nothing jumps to a page by number, and a session keeps the name, so this page could be added without
moving the six before it.

## What is shared with the Multiphysics panel, and what is not

The task asked how much of `multiphysicspanel.{h,cpp}` this panel could use rather than copy. The two
panels show trees of the same `fem::Node` with the same `PropertyDef` properties, so the shape is the
same; but that file's 2200 lines are mostly things an RF 2.5D model has not got - selections picked in a
graphics view, the material library and its property plots, polygons' corners, physics and study
references, solutions to choose a time or a sweep point from, Build and Compute - and its generic editors
reach into `MultiphysicsDoc` for a parameter scope, a built geometry and the names a solved study offers.
Inverting those dependencies would have meant a wide refactor of a file another session is working beside,
for a panel a third of its size.

So two pieces moved to where both can use them, and the rest is this panel's own:

- **The showIf rule** is now `fem::propertyVisible(node, property, kind)` in `fem_model.cpp`, and
  `MultiphysicsPanel::visible()` is three lines calling it. The kind is handed in rather than read from
  the node, because a property the node has not set reads as its kind's default and an RF 2.5D node's
  kinds are in a table of their own. The rule was worth sharing rather than copying: it carries the fix
  of 22 October (B8) for a flag given as an expression, and two copies would have drifted. Five RF
  properties now use it - the ground's surface impedance, and what the processing steps read when they
  are off.
- **The icons** are one drawer: `MultiphysicsPanel::nodeGlyph()`, which the RF tree's ten glyph names
  were added to (stack, layer, conductor, via, layout, map, processing, ports, port, export).

The model's own read helpers became public for the panel: `valueOf`, `textOf`, `flagOf`, `integerOf` and
`nameOf`. `Node::value()` and its fellows read the multiphysics kind table, which has none of these
kinds, so they answer nothing for every RF property - an editor built on them shows an empty field for a
value that is there. One of the breaks below is exactly that.

## The model, as this slice needed it

`find()`, `parentOf()`, `newTag()`, `make()`, `add()`, `remove()` and `renameLayer()`, after
`fem::Model`'s own. Three of them are more than copies:

- `normalise()` gives the parts of the tree itself their tags - `"stack"`, `"layout"`, `"layer_map"`,
  `"study"` - which a blank model and a file read both get, so that every node can be named (the panel,
  the undo and, later, a tool name a node by tag). A file written by the first slice has no such tags and
  gets them on read. A tag two nodes share is made its own, or `find()` would answer with the wrong one.
- `make()` names what it makes: M2, ILD2, V1, P1. A layer with no name could be named by nothing - the
  layer map, the vias and the ports all name a layer by its name.
- `add()` puts a layer where it belongs in z: the substrate at the bottom, the ground and the cover at
  the ends, whatever order they are added in - the order of the tree *is* the order in z, and `stackOf()`
  says so of a stack that has them elsewhere.
- `renameLayer()`, called when the panel renames a layer, renames it in the vias that join it, the ports
  on it and the rows of the layer map that send shapes to it. Two layers may not share a name, and the
  panel says why.

## What is tested

`tests/test_rf25_doc.cpp`, 16 tests, 3.5 s: a new model in its own tab with its tree; a property typed,
undone and redone (and a whole number and a choice, each its own step); what a flag hides shown with it; a
layer added, named, moved, disabled, duplicated and deleted, each a step of Undo, and a part of the tree
refused; a layer renamed following into the via, the port and the map, and a name another layer has
refused; saved and read back byte for byte (`fromJson` and a tab both), and saved under another name; a
file that is no model refused and one with parts missing given them back; a save not read again by the
watcher and a file changed elsewhere read again with the steps of Undo gone; every page of the dock
reached by name and back; the layout read, drawn, and its layers coloured by the map (and a file that is
not there, and one that is no layout, said so); a model opened from a drop and from the project view; the
node menu offering what may go there; and the tables edited row by row, with the stack reading a
parameter.

`test_rf25_model`'s 7 tests still pass unchanged. The suites that touch the dock, the documents and the
multiphysics panel were run too: `test_drop_open`, `test_panes`, `test_project_folders`,
`test_workspace_session`, `test_app_theme`, `test_toolbar_lock`, `test_layout_doc`,
`test_multiphysics_doc` and `test_qucs_control`. Both CI lints pass
(`scripts/ci/check-tool-arguments.py`, `scripts/ci/check-json-nesting.py`), and so do
`check-third-party.py` and `check-shaders.py`.

## What was broken on purpose

`docs/feature_gaps/2026-10-24-rf25-document-and-panel/breaks.py`: sixteen breaks, each undoing one piece
of this round, every one caught by the test named beside it. The parts of the tree given no tags; a shared
tag left alone; the root's row written back into the model as its title (which is what a tree whose root
carries the file name does if a fixed node may be renamed - the bug this round found and fixed); a layer
renamed nowhere else; two layers of one name allowed; the panel reading a property through the
multiphysics table; showIf ignored (caught by both panels' tests); a save stamped by the clock; the suffix
not recognised; the new page left without a name; the layer map's colours unused; the layout file not read
beside the model; a layer added at the end wherever it belongs in z; a part of the tree deleted; the
window's Undo not knowing this document; and Zoom In falling through to the schematic.

The first run caught thirteen of fourteen: the shared-tag break passed, because nothing tested a file
whose nodes share a tag. That is the test `test_rf25_model` gained - the break is only as good as the test
behind it.

## What is not done

- **No solver.** Phase B. The study node holds the sweep and says so in its help.
- **The GDS is drawn, not imported.** The layer map and the processing steps are held and checked, but
  nothing turns shapes into a mesh: `rf_processing`'s merging, sliver-dropping, snapping and healing are
  settings with no code behind them yet, and the mesh node is read by nobody.
- **No Claude tools for a `.qrf25`.** The `fem_*` tools are the multiphysics model's. An `rf25_*` family
  (describe, model, edit) is the obvious next piece, and the tags `normalise()` gives the fixed nodes are
  what such a tool would name them by.
- **No example model**, because a useful one needs a layout file beside it; the tests write their own and
  point it at an example layout.
- **No completions in the expression fields.** The multiphysics panel's `ExpressionEdit` offers the names
  as you type; this one checks and says the value, which is most of the good of it, and the editor would
  have to leave `multiphysicspanel.cpp` to be shared.
- **A dropped `.qfem` opened in the text editor** until now (JSON reads as text), which this round fixed
  in passing: both model suffixes are in `openDroppedFile()`'s known list.
