# Multiphysics 3D begun, 3 November: a 3D model's document, panel and menu; layered solids made one; the swept mesh; the 3D view

The proposal of 2 November (`docs/proposals/2026-11-02-multiphysics-3d.md`) was taken up with the user's choices of
its decisions: **phase B first** (the 3D document and view, before the saved solutions and links of phase A), its
**own `.qfem3d` document**, links **live** by default when they come, and **heat, electrostatics and currents** as
the first 3D physics. This round builds the first two parts of phase B: the 3D engine - the schema, layered solids
made one, their topology and selections, the swept mesh - and the window's part - the document, the *Multiphysics
3D* panel and menu, and a 3D view drawn on the CPU. Nothing is solved in 3D yet (phase C), and the 2D panel and
engine answer as they did.

| Part | Now |
|---|---|
| The file | **`.qfem3d`**, "qucs-s multiphysics 3d 1": the 2D file's shape, its component's `space` 3D. A 2D document refuses one, and the 3D document a 2D model |
| The schema | **One registry of kinds, each marked by its spaces** (2D, axisymmetric, 3D): a box a 3D model's, a rectangle a 2D one's or an extrude's sketch's, a material, a union, a selection, a mesh's size both. `Model::add` refuses a kind of another space; the panels offer what applies. Selections gain an **edge** level, offered in 3D alone |
| Solids | **Box** and **Cylinder** along z; **Extrude**: a sketch of the 2D features (rectangles, circles, ellipses, polygons, imports of DXF, SVG, GDSII, OASIS; moves, arrays, booleans) from a bottom up through a height, each object it ends with a solid of its name; **Layer Stack**: rows of name, layer, z and thickness in µm - a Layout Cross-Section's - each across a window, or where a layout draws its layer in it; **Union**, **Difference**, **Intersection** of solids |
| Form Union | **The plan and its slabs**: one 2D Form Union of every solid's outline, levels where solids begin and end; each plan domain in each slab a cell, held by the solids whose region and range hold it; a domain the cells held by the same solids that touch; faces, edges and points where they meet; numbered from below, then from the left, then from the front |
| Selections | **The 2D rules in 3D** - `of` with sides left, right, front, back, bottom, top; `box` of six numbers - and three new: **`layer`**, **`normal`**, **`plane`** |
| Mesh | **Prisms swept from the plan's mesh** through each slab, divided in z by the element size graded from thin layers at the growth rate, by a level asked fine, or by **Distribution in z**; every node a plan node at a height |
| The window | **The Multiphysics 3D dock tab** after Multiphysics, **its menu** after Multiphysics', a start page with two examples; **the tab**: Build Geometry, Build Mesh, Show, Top, Front, Side, Iso, Fit, a cut across x, y or z, its messages; **the 3D view** on the CPU |

## The schema

`NodeKind` has a `spaces` mask; the kinds a 3D model shares with 2D ones (the tree's parts, parameters and functions,
selections, unions, materials, a mesh and its sizes) are marked so, the 3D model's own (Box, Cylinder, Extrude, Layer
Stack, Distribution in z) 3D alone, and everything else stays 2D. `childKinds(parent, space)` is what may be added
under a node, and `Model::spaceUnder()` says the space there - the model's, but the plan's under an Extrude, whose
children are its sketch. `Model::add` refuses the rest with why: "a Rectangle is of the plan: in a 3D model it goes
in an Extrude's sketch", "a Box is a 3D model's, not a 2D one's". A choice may be offered in some spaces alone
(`PropertyDef::choiceSpaces`): a named selection's level offers edges in 3D. **The 2D panel** asks the same two
questions now - its Add menus from `childKinds()`, its choices those of its model's space - so a 2D model's geometry
offers no Box and its selections no edges, as before this round; a value a file already holds stays shown.

The component kind is one: its `space` is "3D" in a 3D model, and its space choice and out-of-plane thickness are
hidden there by their own `showIf`. `Model::blank3d()` is the 3D model's start, with no study (none solves yet).
`toObject` writes "qucs-s multiphysics 3d 1" for a 3D model; `fromJson` takes a space of "3D", and a file whose
format says 3D is read as 3D whatever its space says, with a note. `objectNames()` lists an extrude's sketch objects
and a stack's rows too, so that a solid's name is its own across the whole model.

## Layered solids made one

Every solid is a region of the plan times a range of z, so Form Union is a plan and levels:

1. **The pieces.** A box is a rectangle of the plan built by the 2D builder (its corner or centre, size, rotation),
   a cylinder a circle, an extrude its sketch - each built by the 2D builder's own features in a 2D model of this
   model's unit, parameters and folder (`buildObjects()`, the 2D objects before their Form Union) -, a stack's row
   the window or its layer's shapes cut to the window.
2. **The levels.** Every bottom and top, those nearer than Form Union's tolerance one; a solid thinner than it is
   refused, said.
3. **The plan.** One 2D Form Union of every region's outline (`formUnion()`, the 2D one, now callable on objects
   made elsewhere). Its domains know the regions that hold them.
4. **The cells.** Each plan domain in each slab, held by the solids whose pieces' region and range hold it - a
   boolean's by its inputs' and tools'.
5. **The domains.** The cells held by the same solids that touch, above, below and across the plan's boundaries -
   so an outline that crosses a solid at another height does not cut it, and where two solids overlap the overlap is
   a domain of its own, as in 2D. A domain's owner is the smallest solid that holds it (of two alike, the later), as
   in 2D.
6. **The faces.** Walls (a plan boundary through a slab) and floors (a plan domain at a level) between two domains or
   a domain and the outside, joined where the same two domains meet on one surface: walls up the slabs and across a
   point onto the same line or circle, floors across the plan's boundaries. A face's normal points from its back
   domain into its front; a planar face carries it.
7. **The edges and points.** Rises (up a plan point through a slab) and rims (along a plan boundary at a level) where
   two faces or more meet, chained between the corners where they do not go on alike; a closed rim, a cylinder's,
   has one point.
8. **The numbers.** Domains, faces and edges from below, then from the left, then from the front (their boxes, then
   their middles); points by z, x, y. A box's faces are its bottom, left, front, back, right and top.

**The Layer Stack** takes its rows as a Layout Cross-Section's - `[name, layer, z, thickness]`, µm or with units,
whatever the geometry's unit -, so one chip's stack serves both. A row with no layer runs across the window; one with
a layer (as 1/0) is the layout's shapes on it (`readLayoutShapes`) cut to the window by Clipper. The window is a box
(from, to, in µm), or round every other solid and the stack's layers' shapes with a margin. A row of a layer the
window holds nothing of is left out, said. The stack's name selects all its rows; `layer` selects rows alone.

**An extrude's sketch** uses its own objects alone: a sketch's union of a box outside it fails, the sketch feature
named. Open curves and points of a sketch are left out (a 3D model's are solids), said.

**What a failed build shows**: the solids before the feature that failed, and its own error - not one Form Union says
of a stack left half made.

## Selections in 3D

`resolveSelection3()` takes the 2D rules as they are, a boundary being a face: `all`, `numbers`, `named`, `objects`
and `only` (domains), `of` (faces, edges, points) with `side` left, right (x), front, back (y), bottom, top (z) - the
normal out of the solids' domain within 45° -, `between`, `exterior`, `interior`, `adjacent`, `box` of six numbers,
`except`. And three new: `{"layer": ["M1"]}` a stack's rows (a solid that is not one said), `{"normal": [0, 0, 1]}`
the planar faces facing that way (an exterior face out of its domain, an interior one either way), `{"plane":
{"z": 3}}` the faces, edges or points lying in it. A name selects solids by their own name, else by their feature's
(an extrude's, a stack's), else an array's copies, as in 2D. Each rule's misuse is said as in 2D ("a side is left,
right, front, back, bottom or top, not north", "a 3D model's box is six numbers").

## The swept mesh

The plan is meshed once by the 2D mesher, to the global sizes of the mesh node's preset taken of the model's largest
side - its height too (`globalMeshSizes()`, the 2D `meshSizes()`'s first part) - and the Size features taken to it: a
domain's size to its plan domains and the slabs it fills, a wall's to its plan boundary and slabs, a floor's to its
plan domains and the levels it lies at, an edge's and a point's likewise. Each slab is divided in z by a size
function: each slab's own size (its element size, or its thickness where it is thinner - one division at least) grown
by the growth rate with the distance from it, and a level asked fine grown likewise; the least of them. **Distribution
in z** gives so many prisms across a domain's height, shared among the slabs it spans by their thickness. A
triangle's prisms run through the slabs its plan domain's cells fill; the nodes no prism uses are left out. Every node
says its plan node and height (`nodeOrigin`): the node-for-node match with 2D the bridge will use. The boundary faces -
a floor's triangles, a wall's quadrilaterals - each turned to its face's front. The mesh says its prisms, nodes, the
plan's smallest angle and worst stretch (a prism's height over its triangle's size, or the size over the height).

In the chip example a 20 µm substrate under a 4 µm oxide and a 0.5 µm heater (two across it, as its Distribution in z
asks) is divided 6, 6 and 2 deep, graded up to the thin layers: 13,426 prisms on 7,662 nodes, the plan's 1,041
triangles. The via's small circle refines the plan in every slab - the swept mesh's price, as the proposal said.

## The window

- **The dock tab** *Multiphysics 3D*, after *Multiphysics* (`SidePage::Multiphysics3D`, "multiphysics 3d" in a
  session file): a start page - New 3D Model, Open Model…, the two examples, the recent `.qfem3d` models - and, with a
  3D model in front, its tree and the settings of the node chosen. The tree hides the study and results a 3D model
  has no use for yet, and names the component *Component (3D)*. Its menus add what `childKinds` allows (an extrude's
  sketch the plan's features), a material from the library, and duplicate, disable, rename, move and delete. Its form
  is made from the kinds as the 2D panel's is: expressions with their values in the tooltip (red where they are not
  right), pairs, choices (those of the space), flags, counts, rows (a stack's with Add making the next layer on the
  last), corners, files, objects (a sketch's own before it, else the solids before it), and a 3D selection editor -
  numbers, all, solids, layers, sides, between, exterior, interior, facing, in a plane, a box, named - with what it
  picks now. The status column says domains, prisms, errors.
- **The menu** *Multiphysics 3D*, after *Multiphysics*: New 3D Model, Open Model…, Build Geometry, Build Mesh, Show the
  panel.
- **The tab** (`Multiphysics3DDoc`): the model with Undo, read and written (`.qfem3d`), read again when it changes on
  disk; its geometry built in the background after each edit that touches it (a burst coalesced), its mesh when asked,
  neither holding the window, a Cancel beside the progress; an import's or a layout's file watched. A 2D file named
  `.qfem3d` is refused ("it is a 2D model - Multiphysics opens it"), and the 2D document refuses a 3D model ("it is a
  3D model - Multiphysics 3D opens it").
- **The view** (`View3D`): orthographic; dragged to orbit, Shift- or right-dragged to pan, the wheel zooming about the
  pointer; Top, Front, Side, Iso, Fit; a cut across x, y or z with a slider, exact at each pixel. Triangles shaded by a
  light at the eye, both sides alike; edges and points over them, hidden where a face is in front, by a depth buffer of
  our own. Each domain its colour; the node chosen in the tree drawn: its selection's domains, faces, edges or points
  solid in blue and the rest as their edges - a feature's own solids so too. Copy as Image, Print and View All work as
  in the other tabs.

## Two corrections to the proposal

- **Domains are joined by the solids that hold them, not by an owner.** The proposal's "joined where the same solid
  owns them" would merge an overlap with one of the two solids; joined by the set of solids, an overlap is a domain of
  its own, as 2D's Form Union makes it - a heater's corner over a via, a trace in its oxide.
- **The stack's numbers are µm**, as the Layout Cross-Section's: a stack copied between the two means the same, and a
  layout is in µm whatever the model's unit.

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_geometry` (new, 11 functions) | The schema: the format, the space, the kinds of each space by `Model::add` and `childKinds`, the hidden space choice, the edge level in 3D alone, the round trip, a 3D format's 2D space read as 3D with a note, the objects' names; a box (1 domain, 6 faces numbered bottom, left, front, back, right, top, each side by its normal, 12 edges, 8 points); stacked boxes (2, 11, 20, 12; the face between them; V - E + F - D = 1); an L (12 faces, the bottom one face across the tower's outline, the front one); overlapping boxes (3 domains, the overlap the smaller's); a cylinder (3 faces, 2 closed edges, its wall's normal radial); an extruded sketch (each object a solid, the extrude's name all of them, a sketch's union of an outside box refused); booleans (a drilled plate: 7 faces, the hole's wall facing in; an empty intersection refused; a union of 6 faces); a layer stack over a GDSII layout (its auto window, the metal's volume, `layer`, a box in the oxide its own domain, the oxide's walls one face a side, a row of nothing left out and said, a window cutting the metal, µm in a mm model); the selections and their errors; what is refused (a plan's rectangle at the top, no height, an empty sketch, a name twice, a layer without a layout, a window round nothing, a solid thinner than the tolerance, no solids). **Every domain's surface closed and turned out**: its faces' triangles sum to no area and enclose its volume |
| `test_fem3_mesh` (new, 6 functions) | Divisions by size, by count (over the size too), graded from a thin slab at the rate, from a level asked fine; a box's prisms through its divisions; counts across a domain, shared among the slabs it spans; a thin film's neighbours graded to it; a domain's size refining the plan through every slab and its slab in z; curved walls swept; what is refused and said. **Every node its plan node at its height, every prism over its plan triangle, every face turned and each domain's closed, each domain's volume its cells' exactly** |
| `test_multiphysics3d_doc` (new, 7 functions) | The dock tab after Multiphysics and its page name, the menu after Multiphysics', the file kind, the examples listed; a new model's tree (no study, no results), what its geometry may hold, a named selection's level with edges; the 2D panel's geometry offering no 3D kind and its selections no edges; solids added and edited through the panel, the geometry built after each, Undo and Redo, a sketch object's solid drawn, a failure marked; the examples opened, built (4 domains, 2), meshed and drawn, a material's selection drawn, written as read; a 2D model refused as 3D and a 3D one as 2D; the view's presets, the frontmost face under the pointer, the cut, the zoom and View All |
| `claudesFormatOfAGroup` (`test_multiphysics_doc`) | `fem_describe` format marks a 3D model's kinds `"space": "3D"` |

## Broken on purpose

`docs/feature_gaps/2026-11-03-multiphysics-3d-phase-b/breaks.py` holds fifty-three:

- **The schema:** `Model::add` taking a kind of another space; a 3D model written as a 2D one's format; a space of
  3D not read; the kinds under a node not filtered; every choice in every space; an extrude's and a stack's objects
  not named; an extrude's sketch of the 3D space.
- **Cells and domains:** cells joined up through any solid; not joined across the plan's boundaries; the owner the
  largest.
- **Faces, edges, numbers:** walls not joined up the slabs, nor across a point on one line; floors not joined; a
  wall's triangles turned; a planar wall's normal reversed; a face's normal out of its domain the wrong way; edges not
  broken where three meet; numbered from the left first.
- **Selections:** the front side the back's; a box's z left out; an exterior face's facing by its own normal; in a
  plane its lower side alone.
- **The stack, booleans, errors:** rows in the geometry's unit; the auto window without its margin; a layer not cut
  to the window; a difference's tools not taken away; an empty result not refused; an extrude's solids not of its
  name; a failed build's error put over; a too-thin solid not refused as such.
- **The mesh:** divisions not graded; a count overruled by the size; a level asked fine not graded from; a wall's
  quadrilaterals turned; a prism's top at its bottom; a domain's size not taken to its slabs, nor to the plan; a
  point's node at the bottom; a count given to each slab whole.
- **The window:** an edit not building again; a feature's solids, a selection not drawn; the tree showing a study and
  results; the view's farthest triangle drawn; its cut ignored; the tab at the dock's end; a `.qfem` taken for a 3D
  file; each document opening the other's model; the format's 3D kinds not marked; the 2D panel offering a 3D model's
  kinds or a 3D selection's edges; the 3D panel's selections without edges.

All fifty-three are caught. Two of the first fifty needed a second look after their run. The numbering break was caught by the box's
faces and not by the stacked boxes, whose two boxes share their x: the script's expectation, not a gap, and
corrected. The guard that keeps a failed feature's own error over Form Union's was reached by no test - dropping a
stack's half-made rows when a build fails had covered the one case the tests built - so a test of a feature that
fails after leaving a solid of its own too thin was added (an extrude whose sketch names a solid a second time, its
height under the tolerance), and the break was caught when run again. Nine checks were made stronger as the breaks
were written, before they ran: each side of a box by its normal, the oxide's walls one face a side through the slabs
a heater makes, a window cutting a layer, the rows' µm in a mm model, a solid thinner than the tolerance, the
frontmost face under the pointer, a count over the size, a count shared among a domain's slabs, and the format's 3D
mark. The last three breaks were written after the fifty had run, when the write-up's reading showed that the 2D
panel's Add menu and choices had offered a 3D model's kinds and a selection's edges (refused when chosen): the 2D
panel now filters them, `theTwoDPanelKeepsToTwoD` holds it, and the three were caught on their first run.

Rebuilt after the breaks: the full ctest, 156 tests, 155 passed in 328 s at eight at once - `test_claude_code`'s
`aReaderScrolledUpStaysPut` saw its transcript 2 px shorter under that load, and passed alone three times and as a
whole suite alone; nothing of this round's. The four lints green; nothing written to the real cache. After the 2D
panel's filtering, the six suites that use either panel (the 2D document's whole among them) green again.

## Under the sanitizers

Built Debug with ASan and UBSan, as CI's Linux job is, each function alone (the process's start included):

| Function | Time |
|---|---|
| test_fem3_geometry `schema` | 1.4 s |
| test_fem3_geometry `aBox`, `stackedBoxes`, `lShape`, `overlap`, `selections`, `errors` | 0.2 s each |
| test_fem3_geometry `aCylinder`, `extrudeSketch`, `booleans`, `layerStack` | 0.3 s each |
| test_fem3_mesh `divisions` | 1.4 s |
| test_fem3_mesh `aBox`, `counts`, `sizes`, `curves`, `refusals` | 0.3 s each |
| test_multiphysics3d_doc `inTheWindow` | 4.5 s |
| test_multiphysics3d_doc `aNewModel` | 1.3 s |
| test_multiphysics3d_doc `solidsThroughThePanel` | 2.5 s |
| test_multiphysics3d_doc `theExamples` (the chip's 13,426 prisms among it) | 2.3 s |
| test_multiphysics3d_doc `eachItsOwn` | 2.4 s |
| test_multiphysics3d_doc `theView` | 3.1 s |
| test_multiphysics3d_doc `theTwoDPanelKeepsToTwoD` | 4 s |

No sanitizer said anything.

## What phase B still lacks

- **The Layout feature of the proposal** - a layout's layers through RF 2.5D's layer map and processing (merge vias,
  drop slivers, snap, heal, remove fill), the window drawn on the layout viewer's canvas, *3D from Layout…* in the
  viewer - and an RF 2.5D model's stack read in place of rows. The Layer Stack reads a layout's layers as they are.
- **Extrude 2D Model** (a `.qfem`'s domains extruded), **wall angles**, revolving an axisymmetric model.
- **Picking** in the 3D view (its `idAt()` is there), and the selection editor's pick button.
- **The `fem_*` tools on 3D models** (`fem_model` new with space 3D, the geometry and mesh answers), and
  `open_document` of a `.qfem3d` - Claude reads the kinds' 3D marks in `fem_describe` meanwhile.
- **The GPU view** (opt-in, as every viewer's since 8d85227).
- A union's interior faces kept; a sketch's open curves as sheets; edge-level mesh sizes; a box or a cylinder moved or
  turned off z.

Then phase C: heat, electrostatics and currents on the prisms, AMG measured against the incomplete Cholesky first.
