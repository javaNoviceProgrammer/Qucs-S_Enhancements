# Proposal: a Multiphysics 3D panel, and models, fields and results passed between 2D and 3D

*2 November 2026, after Qucs-S 27.0.5 (c6d4dc8). A written proposal only; nothing here is built.*

*The request, as it was handed over: a new "Multiphysics 3D" panel for 3D simulations, with the
"Multiphysics" panel kept for 2D, and models, data and results passed seamlessly between the two. Above
all: run a 3D simulation, take its results on a 2D cross-section, and hand them to a 2D solver for further
physics. The reverse matters too: a 2D model extruded or revolved into 3D, or 2D results carried into 3D.*

*It builds on [the proposal of 12 October](2026-10-12-multiphysics-3d.md) (the engine's stages: prisms
first, general solids later) and [that of 22 October](2026-10-22-multiphysics-3d-panel.md) (the panel,
and links between models). It keeps their shape and settles what they left open: the geometry kernel,
the mesher, the 3D solvers and their memory, the 3D view, the bridge in detail and Claude's tools. Where
it differs from 22 October, this one stands.*

*The facts about this tree were checked in it on 2 November 2026; the libraries' licences and versions on
their own pages that day; the solver figures were measured on this machine (an M2 Ultra, 24 cores, 64 GB,
macOS 26.5). Sizes and weeks are estimates, for one person.*

## In short

- **A 3D model is a document of its own:** a `.qfem3d` file, a tab with a 3D view, and a *Multiphysics 3D*
  tab in the left dock. The *Multiphysics* tab stays 2D; no 2D node gains a z.
- **One schema, one engine core.** Both kinds share the tree's nodes, parameters, expressions (z added in
  3D), materials, studies, derived values and Claude's tools. What is new in 3D is the geometry, the mesh,
  the elements, the assembly and the view.
- **Geometry: no kernel at first.** The first 3D models are *layered*: 2D sketches (drawn with the 2D
  features, imported, or a layout's layers) extruded through z-ranges, a layer stack, tapered walls. A
  boolean of such solids is a 2D boolean slab by slab, so it is exact and needs nothing new. General
  solids (spheres, cylinders off the z axis, sweeps along curves, fillets, booleans across directions,
  STEP) come in phase F, through **OpenCASCADE driven by the Gmsh API**, both linked, as an optional part
  of the build.
- **Mesh: prisms of our own, tetrahedra from Gmsh.** A layered model is meshed as prisms swept from one 2D
  Delaunay mesh: exact cuts, a node-for-node match with 2D, and a semiconductor box method that stays
  sound. General solids get Gmsh tetrahedra (phase F). Not TetGen (AGPL), and not a tetrahedral mesher of
  our own.
- **Solvers: what the numbers say.** Measured here on a 3D P1 problem of a million unknowns, a direct
  Cholesky with the ordering the tree uses now takes 24 GB and 113 s; with a METIS ordering 9.3 GB and
  31 s; conjugate gradients 3.3 GB and 10 s. So 3D solves iterate by default, with algebraic multigrid
  (AMGCL: MIT licence, header-only, vendored as Eigen and CDT are), and factorize (METIS-ordered) where a
  matrix is small or solved many times. Heat, electrostatics and currents come first; then solid mechanics
  and the semiconductor; magnetics and full-wave only on demand.
- **The 3D view** shows surfaces, slices, isosurfaces and clip planes: on the Qt RHI with a depth buffer
  when the GPU is chosen, and on the CPU by default (a small z-buffer rasterizer of our own, which also
  draws offscreen for the tests).
- **The bridge is links.** A link reads one model's solution in another through a mapping: a cut plane, or
  an average across a thickness or about an axis (3D to 2D); an extrusion, a revolution or a face (2D to
  3D); the same space (2D to 2D, 3D to 3D). Its fields become variables of the receiver, such as `cut1.T`,
  usable as initial values, sources, coefficients, boundary values, plots and integrals. A link records
  where its fields came from (the source's file, study, instance, and a stamp of its solve). It is marked
  stale when the source is solved again, and refreshed by a click or by solving the chain.
- **Your workflow is one command each way.** *New 2D Model from This Cut…* makes the 2D model's geometry
  from the cut, each region named and with its material, the boundary conditions carried, and the 3D
  fields linked. *Extrude into a 3D Model…* and *Revolve into a 3D Model…* do the reverse.
- **Solutions are kept beside the model** (`<model>.solution`, as RF 2.5D keeps its own), so a link can
  read a closed model and a 3D solve is not done again at each opening. This takes up the parked
  "solution sidecar", for 2D models too.
- **Claude:** the `fem_*` tools take 3D models, and three new tools are added: `fem_cut`, `fem_extrude`
  and `fem_link`.
- **Phases A to G.** A to D (about 33,000 lines with tests, 22-27 weeks) give your workflow both ways on
  layered models. E adds solid mechanics and the semiconductor, F general solids, and G the rest on
  demand.

## What changed since 22 October

- **RF 2.5D was built:** `qucs/rf25/` (15,232 lines), its document, panel and tools, and 12,861 lines of
  tests. It is a third document kind with its own kind table, panel, dock page, menu and tools. It shares
  `Node`, `NodeKind`, `PropertyDef`, `propertyVisible()` and the panel's glyphs with the multiphysics
  model. A 3D document follows the same pattern.
- **RF 2.5D keeps its solve beside the model** (`<model>.solution`): a compressed binary stream stamped with
  the SHA-1 of the model's JSON (without its exports), the SHA-1 of the layout's bytes, the mesh's cells,
  the build and the time. Multiphysics models still keep nothing.
- **Versions:** OpenCASCADE is at 8.0.1, and Gmsh at 4.15.2 (24 March 2026).
- **Nothing of the 22 October plan was built:** no links, no 3D, no mode matching.

**What this proposal settles that 22 October left open:**
- the geometry kernel (none at first, then OpenCASCADE through the Gmsh API);
- the mesher (prisms of our own, then Gmsh);
- the solver path, from measurements;
- how the 3D view draws without a GPU;
- the exact cut geometry for layered models;
- the conditions and uses carried across a cut;
- the tools' names, arguments and answers;
- saved solutions in the first phase.

## Goals and non-goals

**Goals**

1. A *Multiphysics 3D* panel and document beside the 2D ones, as alike as the space allows, with the same
   tools for Claude.
2. Layered 3D models first (a layout and a layer stack, extrusions), then general solids.
3. Heat, electrostatics, currents, solid mechanics and the semiconductor in 3D.
4. A bridge in both directions for geometry, materials, conditions and fields, traceable and refreshable.
5. Sizes said before a solve, and solves that fit this machine's memory.
6. Results in the form 2D users know (a cut is a 2D dataset) and the forms 3D needs (slices, isosurfaces,
   clip planes), with an export to ParaView.

**Non-goals**

- **A CAD modeller.** No 3D sketching with constraints, no assemblies with mates, no direct editing of
  faces. Geometry is a sequence of features in a tree, as in 2D.
- **3D full-wave electromagnetics** of packages and antennas, and **3D magnetics**, in phases A to F. They
  need edge elements in 3D and solvers of their own (phase G, on demand).
- Clusters (MPI) and GPU solvers.
- **Changes to the 2D panel** beyond what the bridge adds (a *Linked Fields* node and two menu items). The
  2D engine's answers stay bit for bit what they are.
- **Two-way coupling iterated to agreement** between a 3D and a 2D model. A link goes one way per solve; a
  chain is solved in order, and a cycle is refused.
- **Replacing RF 2.5D for planar passives.** It remains the tool for their S-parameters.

## What there is to build on

| Part | Now | What it gives this proposal |
|---|---|---|
| The engine | `qucs/multiphysics/`, 68,963 lines: 2D and axisymmetric geometry, CDT meshes, P1/P2 and Nédélec triangles, twelve physics | The physics to port, and the 2D solvers a cut is handed to |
| Document, panel, view, tools | `multiphysicsdoc.cpp` 3,606 lines, `multiphysicspanel.cpp` 2,224, `multiphysicsview.cpp` 1,037 (its GPU canvas 493), `qucscontrol_fem.cpp` 2,706 | The pattern the 3D ones follow |
| A third document kind | RF 2.5D: `rf25doc.cpp` 2,755, `rf25panel.cpp` 1,034, `qucs_rf25.cpp`, `SidePage::Rf25`, `qucscontrol_rf25.cpp` 1,032 | How a new kind joins the window: its dock page, its menu, the lists of document kinds in `qucs_actions.cpp`, its tools |
| The file | JSON with a `components` array of which the first is read; `space` is 2D or axisymmetric, and any other space is noted as "not built yet" (`fem_model.cpp`) | A schema ready for a third space |
| Layouts | gdstk (`layout.cpp`, `layoutdoc.cpp`); `readLayoutShapes()` and `cutIntervals()`; the *Layout Cross-Section* feature and its `StackRow` (name, layer, z, thickness, material); the RF 2.5D stack with each layer's z0 and z1, its layer map and its processing (merge vias, drop slivers, snap, heal, remove fill) | A layout and a stack made 3D, and a cut checked against the cross-section |
| Fields from outside | *Interpolation* functions: one argument, or two on a grid, bilinear | The file form of a link today; a third argument is cheap |
| Results into another model | A semiconductor's doping *From the process flow* (within one model); Mode Overlap and Mode Profile exports (`.mat`, `.npz`, `.csv`) | The precedents for links and their formats |
| Evaluation anywhere | `Solution`, `PointEvaluator` (any expression at a point of an element), `Locator` (a bucket grid of triangles) | What a link reads, generalised to 3D |
| Solvers | On macOS the Accelerate Cholesky, and its LU from macOS 26; elsewhere the Eigen simplicial LDLT and SparseLU; conjugate gradients with an incomplete Cholesky from 400,000 unknowns where Accelerate is absent; complex systems as real equivalents; a Krylov-Schur eigensolver | The 2D solvers; for 3D, measured below |
| GPU drawing | `rhicanvas.h`: `LayeredCanvas` (QPainter layers and GPU steps composed) and `Viewport2D`; shaders compiled beforehand (`shaders/*.qsb`); 2D only, no depth buffer. Every viewer draws on the CPU by default since 8d85227 | The base of the 3D view's GPU path |
| Kept solutions | RF 2.5D's `.solution` (`rf25_kept.h`) | The format and the stamps for multiphysics |
| Third-party code in the tree | CDT 2.0.0 and Eigen 5.0.1 (MPL 2.0), gdstk 1.0.1 with Clipper 6.4.2 (Boost Software License), all pinned in `third_party/` with their changes marked | The way a new library joins |
| Tests | `test_fem_*.cpp`, 29,657 lines; `test_multiphysics_doc.cpp`, 5,176 | The 2D side, which must not move |

## How 3D sits beside 2D

### Its own document, on the same schema

| | Multiphysics (2D) | Multiphysics 3D |
|---|---|---|
| File | `.qfem`, "qucs-s multiphysics 1" | `.qfem3d`, "qucs-s multiphysics 3d 1" |
| Document | `MultiphysicsDoc`, the 2D Graphics view | `Multiphysics3DDoc`, the 3D view |
| Left dock | *Multiphysics* | *Multiphysics 3D* (`SidePage::Multiphysics3D`, "multiphysics 3d" in a session file) |
| Menu | Multiphysics | Multiphysics 3D |
| Space | 2D, axisymmetric | 3D |

**Why not one model with a dimension** (a third `space` of `.qfem`):
- it is the separation asked for, and each panel then never shows a node that does not apply;
- every 2D editor, selection rule and tool answer would learn a z, and the 2D tests would be at risk for
  a feature they do not use;
- a 2D model stays readable by every build that reads it now.

**Why the same schema:**
- The 3D file has the 2D file's top-level shape.
- The node kinds live in one registry, each marked with the spaces it applies to (2D, axisymmetric, 3D).
  Most physics features (a Temperature, a Ground, a Terminal, a Heat Flux) are the same in both. RF 2.5D
  needed a table of its own only because none of its kinds were shared.
- A file holding a 3D component and 2D components together stays possible later, if it is ever wanted.

### The window

```
┌ heater.qfem3d ──────────────────────────────────────────────────────────────┐
│ Build Geometry · Build Mesh · Compute │ Show [Plot ▾] [pg1 ▾] [t = 1 ms ▾]  │
│ Top · Front · Side · Iso · Fit │ Clip · Cut Plane · Image │ ▓▓▓▓▓░░ Cancel  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│               the 3D view: orbit, pan, zoom, pick; the legend               │
│                                                                             │
├────────────────── Messages │ Table │ Links ─────────────────────────────────┤
└─────────────────────────────────────────────────────────────────────────────┘
```

- **The toolbar and the bottom tabs** are the 2D tab's (Build Geometry, Build Mesh, Compute, the plot and
  instance boxes, the progress row with Cancel, Messages, the table of derived values), plus the camera's
  presets, the clip and cut-plane tools, and a *Links* tab listing each link and whether it is stale.
- **The panel** opens on a start page: *New 3D Model*, *New from a Layout…*, *New from a 2D Model…*,
  *Open*, examples and recent models. Once a model is open, it shows the tree and, below it, the settings
  of the node chosen, as in 2D.
- **The panel follows the tab in front**, as both panels do now (`qucs.cpp:3829`).
- **The 2D panel gains three things:** a *Linked Fields* node under Global Definitions, and *Extrude into a
  3D Model…* and *Revolve into a 3D Model…* on its Geometry menu.

### The tree

```
Model (3D)
├── Global Definitions
│   ├── Parameters
│   ├── Functions               analytic; interpolation of 1, 2 or 3 arguments
│   └── Linked Fields           cut1 ← ring2d.qfem, study std1, live
├── Component (3D)              unit µm
│   ├── Definitions             named selections
│   ├── Geometry
│   │   ├── Layer Stack         rows: name, z, thickness, material, layout layer
│   │   ├── Layout              chip.gds, cell, window, layer map, processing
│   │   ├── Extrude             a sketch of 2D features from z0 to z1, a wall angle
│   │   ├── Extrude 2D Model    a .qfem's geometry from z0 to z1
│   │   ├── Box, Cylinder       along z (an extruded rectangle, circle)
│   │   ├── Union, Difference, Intersection
│   │   ├── (phase F)           Sphere, Cone, Torus, Cylinder on any axis, Revolve, Sweep,
│   │   │                       Loft, Fillet, Import STEP / IGES / STL
│   │   └── Form Union
│   ├── Materials
│   ├── Heat Transfer · Electrostatics · Electric Currents · Solid Mechanics · Semiconductor · PDE
│   └── Mesh                    Swept (prisms) or Free (tetrahedra); Size; Distribution in z
├── Study                       Stationary, Time Dependent, Frequency Domain, Parametric Sweep
└── Results
    ├── 3D Plot Group           Surface, Slice, Isosurface, Arrow (when asked), Mesh
    ├── Datasets                Cut Plane, Cut Line 3D, Cut Point 3D
    ├── Derived Values          over volumes, faces, edges, at points; globals
    └── Export                  VTU, Field on a Grid, a Cut Plane's fields
```

### Selections in 3D

- **Levels:** domains (volumes), boundaries (faces), a new edge level, and points.
- **The 2D rules carry over as they are:**
  - `all`, `numbers`, `objects`, `only`;
  - `of`, whose `side` takes left and right (along x), front and back (along y), bottom and top (along z);
  - `between`, `exterior`, `interior`, `adjacent`;
  - `box` (six numbers), `named` and `except`.
- **Three rules are new:**
  - `{"layer": "M1"}`: a stack row's solids;
  - `{"normal": [0, 0, 1]}`: the planar faces facing that way;
  - `{"plane": {"z": 3}}`: the faces lying on that plane.
- **Numbering:** domains are numbered by their lowest point, from below, then from the left, then from the
  front, much as 2D numbers from the left, then from below.
- **Picking:** a click picks the face under the pointer (the domain, in a domain selection) by a ray cast
  against the boundary's triangles, and edges and points near the pointer.

## 3D geometry

### Layered solids (phases B to E): no kernel

**A layered solid is a 2D region times a z-interval**, and its region may shrink or grow with height (a
wall angle). The regions come from:
- a sketch: the 2D geometry features (rectangles, circles, polygons, imports of DXF, SVG, GDSII and OASIS,
  moves, arrays, booleans), built by the 2D builder as they are;
- a row of a layer stack: a layout layer's shapes, or the whole window;
- a 2D model's domains (*Extrude 2D Model*);
- a box or a cylinder along z: a rectangle or a circle extruded.

**How Form Union builds them:**
1. The z breakpoints are every solid's bottom and top. Between two breakpoints is a slab.
2. One 2D Form Union over every region's outline gives the base topology. In each slab, each base domain
   is owned by the last solid present there (later overrides, as in 2D), or by none (left out).
3. A 3D domain is a base domain in a slab, joined to its neighbours above, below and beside where the same
   solid owns them.
4. The rest follows:
   - side faces: a base boundary through slabs;
   - horizontal faces: base domains at an interface between slabs;
   - vertical edges: base points through slabs;
   - horizontal edges: base boundaries at an interface;
   - points: base points at interfaces.

**Booleans** (union, difference, intersection) of layered solids are 2D booleans slab by slab, done by the
Clipper the 2D builder already uses. They are exact.

**Wall angles** (an etch's sloped sidewall) are a slab's outline moved inward with height, by tan θ per unit
of z:
- The 2D mesh's nodes are moved by a harmonic map (a 2D solve on the 2D mesh, which the engine has).
- The walls are straight within the slab.
- The move made at a slab's top is carried up through the slabs above it, so a cladding over a sloped rib
  meets it face to face.
- A prism made flat or inverted is refused, with where it is.

**Revolution into layered solids** is exact where an axisymmetric model's domains are rectangles in
(r, z): they revolve into stacked discs and rings, which are circles and annuli extruded. A sloped side
(a cone frustum) becomes a wall angle where the shape allows it. That covers a VCSEL, a mesa, a pillar, a
via and a TSV. Other shapes (a lens, a sphere) wait for phase F.

**Layouts and stacks:**
- **The rows** are those of the *Layout Cross-Section*: a name, a layout layer (or none: the whole window),
  a bottom and a thickness, and a material. An RF 2.5D model's stack can be read in place of typed rows,
  so one chip has one stack.
- **The layer map and the processing steps** are the RF 2.5D ones (`rf25_layout`): merge vias, drop
  slivers, snap, heal, remove fill. They matter as much here, because every outline is imprinted in one 2D
  mesh.
- **The window** is a rectangle of the layout to model, with a margin of the stack's lower and upper rows
  (substrate, oxide, air) around it. It is chosen on the layout drawn in the layout viewer's own canvas
  (`LayoutView`), as RF 2.5D draws its layout.
- **In the layout viewer**, *3D from Layout…* sits beside *Cross-Section to Multiphysics…*, and takes the
  view or a box drawn on it as the window.

### General solids (phase F): OpenCASCADE through the Gmsh API

- **Primitives:** box, sphere, cylinder and cone on any axis, torus, wedge.
- **From sketches:** extrude, revolve, sweep along a path, and loft through sections, with each 2D sketch
  placed on any plane.
- **Booleans:** fuse, cut, intersect, and fragment (which is Form Union); fillets and chamfers.
- **Layered solids join them:** each layered solid becomes an OpenCASCADE solid, so a layout's stack and a
  bond wire are one model.
- **Imports:**
  - STEP, IGES and BREP: each solid a domain named after the file's own names; units from the file; healed
    (sewn, small edges and faces removed); a solid that is not closed is refused, with which one.
  - STL: a closed triangulated surface, split into faces by an angle and meshed inside it. An open or
    self-intersecting surface is refused, with where.
- **Cuts of general solids** are the OpenCASCADE section of the solids by the plane (the Gmsh boolean
  fragment), and each face of the section knows the solid it lies in.

### The kernel: why none at first, then OpenCASCADE

- **A CSG of our own beyond layered solids is a geometry kernel**: booleans of curved surfaces that do not
  fail on tangencies and slivers. That is years, not weeks.
- **On triangle meshes such booleans exist** (Manifold, Apache 2.0, version 3.5.4). But spheres and
  cylinders become facets, losing a curved P2 element's accuracy, and STEP would still be missing.
- **OpenCASCADE is the one mature open kernel that reads STEP into true surfaces.** It is large, so it
  waits until the 3D engine stands on layered models. It is an optional part of the build: without it, the
  panel builds layered models and says what a general feature needs.
- **Through the Gmsh API rather than the OpenCASCADE classes**, because the primitives, the booleans,
  extrude, revolve, pipe, loft and fillet, and the STEP, IGES, BREP and STL readers are all there, beside
  the mesher that meshes their result. That means far less code of ours against OpenCASCADE, and one pair
  of libraries to pin.

| Library | Version (2 Nov 2026) | Licence | With Qucs-S (GPL 2 or later) | Proposed |
|---|---|---|---|---|
| OpenCASCADE (OCCT) | 8.0.1 | LGPL 2.1 with an additional exception | Compatible | Phase F, through Gmsh |
| Gmsh | 4.15.2 | GPL 2 or later, with an exception for linking | Compatible. Built as a library, without its GUI and without the contributions tetrahedral meshing does not need, Blossom (a licence of its own) among them; each contribution built is checked against the list in the Debian copyright file | Phase F |
| Netgen | - | LGPL | Compatible | Only as the mesh optimizer inside Gmsh: alone it builds no geometry |
| TetGen | 1.6.0 (1.6.1 packaged) | AGPL 3, or a paid licence | Would make the binaries AGPL | No |
| Manifold | 3.5.4 | Apache 2.0 | Compatible with GPL 3 | No: facets, no STEP |
| AMGCL | - | MIT; header-only; Boost only for its runtime interface, which is not used | Compatible | Phase C, vendored |
| CDT, Eigen | 2.0.0, 5.0.1 | MPL 2.0 | In the tree already | - |

### Imports, by phase

| Source | Route | Phase |
|---|---|---|
| A 2D model (`.qfem`) | *Extrude 2D Model*; *Revolve* into layered solids where its shapes allow | B (extrude), D (revolve) |
| A GDSII or OASIS layout with a stack | *Layout* and *Layer Stack* | B |
| An RF 2.5D model (`.qrf25`) | Its stack and layout | B |
| DXF, SVG | Sketches inside an *Extrude* | B |
| STL | General solid | F |
| STEP, IGES, BREP | General solids | F |

## 3D meshing

### Swept: prisms from the 2D mesh (phases B to E)

- **One 2D mesh over the base topology**, made by the CDT mesher as it is, with the 2D sizes (domain,
  boundary, point, growth, curvature, narrow gaps). For a semiconductor it is kept Delaunay.
- **Each slab is divided in z** by a count or a size, graded toward its faces, with one division at least.
- **Each triangle times each division is a prism** (6 nodes; 18 for P2). A node of the 3D mesh is a 2D node
  at a level. That exact correspondence is what the bridge and the invariance tests use.
- **The semiconductor stays sound.** Each prism's Voronoi cell is its triangle's times its height, so every
  box-method weight is positive (12 October):
  - an edge in the plane weighs its 2D cotangent weight times the height;
  - a vertical edge weighs its 2D Voronoi area over the height.
- **The price:** a size in the plane costs in every slab. A refinement for one thin layer's lateral feature
  runs through the whole height. Meshes that differ from slab to slab, glued by mortars or by tetrahedra
  between them, are not proposed. The mesh statistics say what the sweep costs.

### Free: tetrahedra from Gmsh (phase F)

- **Gmsh meshes the OpenCASCADE shapes:**
  - by Delaunay (its HXT mesher among them);
  - to the Size features: a field per domain, face, edge or point, and growth by distance;
  - with second-order curved tetrahedra for P2;
  - with prism boundary layers on the faces asked;
  - optimized by the Netgen optimizer inside it.
- **A model with any general feature is meshed whole by Gmsh**, so a layout's stack with a bond wire meshes
  as one. Its layered parts then lose the swept mesh's node-for-node match with 2D, which the bridge does
  not need: it locates any point.
- **The semiconductor stays on swept meshes.** Tetrahedra are not Delaunay to every interface, so some
  box-method weights go negative. Semiconductors on tetrahedra wait for a check of every weight, with
  refinement where one is negative (phase G).
- **Why not the others:**
  - TetGen is AGPL.
  - Netgen meshes but builds no geometry: the booleans and STEP would be ours to write against
    OpenCASCADE.
  - A tetrahedral mesher of our own means boundary recovery and sliver removal, years of work in the
    projects that have them.

### Sizes said before a solve

- *Build Mesh* reports the cells, the nodes and the quality (smallest dihedral angle, worst aspect).
- For each physics, it reports the unknowns, the matrix's nonzeros, and the estimated memory and time of a
  direct and of an iterative solve, fitted to the table below.
- *Compute* refuses a solve past a limit (Multiphysics 3D > Settings…; by default half of this machine's
  memory), and says how to get under it: the iterative solver, a coarser mesh, or fewer divisions in z.

## Solvers

### What carries over

| Layer | Carried over as it is | Generalised | New |
|---|---|---|---|
| Model and expressions | The tree, parameters, functions, units, the expression language (z added), materials and their library | | |
| Studies | Stationary, time dependent (BDF), frequency domain; sweeps with points solved at once; coupled physics solved in turn; Newton's method | | |
| Algebra | `LinearSystem`, `ComplexSystem`, `SparseLu`, `SparseCholesky`; the Krylov-Schur eigensolver | The Accelerate symbolic options: a METIS ordering for 3D | AMG (AMGCL) |
| Geometry and mesh | | `Topology` (faces and edges), `Mesh` → `Mesh3` | Layered solids; the swept mesh; later the Gmsh route |
| Elements | | `Space` over cells of a type; `PointEvaluator` at a cell and (ξ, η, ζ); `Locator` as a grid of cells (on a swept mesh, the 2D locator and a search in z) | Tetrahedra and prisms, P1 and P2, their quadrature |
| Physics | How each reads its features, selections, materials and conditions | | Each physics' assembly over 3D cells; the box method on prisms |
| Results | Derived-value kinds, globals, export formats | | Slices, isosurfaces, clip planes; VTU |

**The 2D code is not rewritten.** 3D gets its own types beside the 2D ones. The physics' reading of the
model moves into functions both use, step by step, with the 2D tests unchanged at each step.

### Elements and orders

- **Lagrange P1 and P2** on tetrahedra (4 and 10 nodes) and prisms (6 and 18 nodes).
- **Quadrature:** Keast's rules on tetrahedra; the triangle rules that exist times Gauss's on prisms.
- **Curved P2:** the Gmsh second-order nodes on tetrahedra; on prisms, the 2D mesh's curved edges swept.
- **Defaults:**
  - P1 for heat, electrostatics and currents, for memory, with P2 a choice;
  - P2 for solid mechanics, because P1 tetrahedra lock in bending;
  - P1 for the semiconductor (the box method).
- **Nédélec elements** on tetrahedra and prisms, for 3D magnetics and waves: phase G.
- **Unknowns:** P1 has one per node; P2 has about eight per node, since a tetrahedral mesh has some seven
  edges per node.

### Physics, in order

1. **Heat** (stationary and in time), **electrostatics** (terminals and capacitance matrices), **electric
   currents** (resistance, Joule heat), and their couplings: phase C.
2. **Solid mechanics:** linear elastic, an anisotropic C, thermal expansion from the heat: phase E.
3. **The semiconductor** on prisms, with all its 2D models: phase E.
4. **PDE** (coefficient form): cheap once the first ones are done.
5. **Magnetics** (vector A on edge elements, with a gauge) and **frequency-domain waves** (edge elements,
   PMLs, ports): phase G, on demand. For light along z, the mode matching of 12 October (its stage 0) comes
   first.

**What stays 2D:**
- the mode analysis (a cross-section's problem; the bridge brings 3D fields to it);
- the quantum wells;
- the process flow (its doping reaches 3D through the bridge);
- the body-of-revolution resonator;
- rays.

### Direct or iterative: measured

**The benchmark** (a scratch program, not committed): the P1 Laplacian of an n × n × n grid of cubes, each
cut into six tetrahedra, the bottom face held. It has about 15 nonzeros per row, as an unstructured
tetrahedral mesh has. "Vector" carries three unknowns per node, coupled as solid mechanics couples them.

It was solved four ways:
- the Accelerate Cholesky with the ordering the tree uses now (`symbolicOptions()` in `fem_sparse.cpp`
  takes the default);
- the Accelerate Cholesky with a METIS ordering;
- the Eigen conjugate gradients with an incomplete Cholesky (the tree's path above 400,000 unknowns where
  Accelerate is absent), to 1e-9;
- the Eigen simplicial LDLT (the direct solver off macOS), at small sizes.

Direct columns give the factor's size and the factorization's time (symbolic and numeric); iterative
columns the time, the iterations and the peak memory.

| Unknowns (scalar) | Nonzeros | Cholesky, the tree's ordering | Cholesky, METIS | Conjugate gradients + incomplete Cholesky |
|---|---|---|---|---|
| 28,830 | 0.41 M | 114 MB, 0.08 s | 73 MB, 0.17 s | 0.10 s, 93 iterations, 104 MB |
| 67,240 | 0.97 M | 417 MB, 0.32 s | 234 MB, 0.45 s | 0.31 s, 122, 232 MB |
| 130,050 | 1.9 M | 1.1 GB, 1.5 s | 553 MB, 0.97 s | 0.71 s, 154, 432 MB |
| 270,400 | 4.0 M | 3.3 GB, 6.9 s | 1.6 GB, 2.9 s | 1.8 s, 197, 0.9 GB |
| 524,880 | 7.7 M | 9.2 GB, 32.6 s | 3.7 GB, 8.9 s | 4.1 s, 247, 1.7 GB |
| 1,020,100 | 15.1 M | 24.4 GB, 113 s | 9.3 GB, 30.5 s | 10.0 s, 309, 3.3 GB |

| Unknowns (vector) | Cholesky, the tree's ordering | Cholesky, METIS | Conjugate gradients + incomplete Cholesky |
|---|---|---|---|
| 86,490 | 1.0 GB, 1.4 s | 615 MB, 1.05 s | 1.06 s, 187 iterations, 641 MB |
| 201,720 | 3.4 GB, 8.4 s | 2.0 GB, 3.6 s | 3.3 s, 258, 1.5 GB |
| 390,150 | 10.0 GB, 32.0 s | 4.9 GB, 11.2 s | 7.7 s, 330, 2.9 GB |
| 811,200 | 25.1 GB, 117 s (30.6 GB peak) | 13.8 GB, 47.6 s | 20.8 s, 433, 6.0 GB |

The Eigen simplicial LDLT took 0.21 s at 8,820 unknowns, 2.9 s at 28,830 and 23.3 s at 67,240. The
Accelerate Cholesky took 0.02, 0.08 and 0.32 s for the same matrices. (MB and GB here are 2^20 and 2^30
bytes.)

**What follows from it:**
- **The direct solve's memory grows as about N^1.4 with METIS, and N^1.5 with the tree's ordering.** A
  million P1 unknowns, an ordinary size for a chip region in 3D, is past a laptop's direct solve and costs
  9-24 GB here.
- **3D factorizations take the METIS ordering:** 2.6 times less memory and 3.7 times faster at a million
  scalar unknowns. It is one field of the symbolic options. Its effect on the 2D models is to be measured
  before it changes there; that is a change of its own.
- **3D iterates by default.** Conjugate gradients take memory in proportion to the size (3.3 GB per million
  P1 unknowns here, mostly the incomplete factor and the matrix) and seconds.
- **The iterations grow with the mesh's divisions** (62 at n = 20, 309 at n = 100), which is what an
  incomplete Cholesky does. Smoothed-aggregation AMG keeps them nearly constant. That is why the
  preconditioner is AMGCL, with the incomplete Cholesky kept as its fallback.
- **AMG was not measured here.** AMGCL is not in the tree, and nothing was downloaded for this proposal.
  Phase C's first task measures it against these figures on the same problems, and on a high-contrast one
  (copper beside oxide in a heat model, a thermal conductivity ratio near 300), where both preconditioners
  can stall.
- **Direct wins where one matrix is solved many times:**
  - a capacitance matrix of N terminals;
  - heat in time with fixed coefficients;
  - a frequency sweep.

  At 524,880 unknowns, one METIS factorization (8.9 s) and then 0.35 s per solve beat 4.1 s per
  conjugate-gradient solve from the third solve on. A solve that knows it will repeat factorizes, within
  the memory limit.
- **Off macOS, 3D iterates.** The Eigen simplicial LDLT is seventy times slower at 67,240 unknowns. The
  parked sparse backend (SuiteSparse, chosen on 18 October and then parked) decides whether a direct 3D
  solve is offered there.
- **Solid mechanics' AMG needs the rigid-body modes** as its near-null space, which AMGCL takes. Without
  them, AMG on elasticity converges slowly.
- **The semiconductor's Jacobian** is nonsymmetric and scaled over decades:
  - a direct LU while it fits (the Accelerate LU, used from macOS 26: on macOS 15.7 it stopped the
    program in semiconductor solves);
  - else GMRES with the Eigen ILUT on its scaled rows;
  - a direct fallback when the iteration stalls.

  It is measured in phase E before it is trusted.
- **Threads:** without OpenMP (the build has none), the AMGCL builtin backend runs on one thread. Its
  matrix-vector products can run on the engine's own threads (`parallelFor`) through a backend of ours, if
  measuring shows it worth it.

## Results and the 3D view

### What a 3D plot group shows

- **Surface:** an expression on the domains' boundaries (all of them, or those chosen), coloured, with the
  mesh's edges when asked.
- **Slice:** planes (x, y or z = c, or a point and a normal; several, or a count across), each the cells it
  cuts, coloured.
- **Isosurface:** an expression's surfaces at levels, by marching tetrahedra (a prism is cut into three).
- **Clip:** a half-space removed, its cut face drawn as a slice so that the model looks solid.
- **Arrow Volume:** only when asked; plots are surfaces and contours by default.
- **Mesh:** the cells, coloured by quality.

**Defaults after a solve:** a surface of the main variable and a slice through the middle, no arrows.

### Cuts are 2D datasets

- **A Cut Plane** is a dataset whose values lie in the plane's own axes (u, v): the 2D `SurfaceData`. Every
  2D plot works on it unchanged: surface, contour, line graph, and derived values over it.
- *Show Cut in 2D* opens it in a 2D Graphics view, drawn by the 2D view's own code.
- A **Cut Line 3D** feeds line graphs, as the 2D Cut Line does; a **Cut Point 3D** feeds point graphs.

### The view

- **On the GPU** (Settings > Viewers > *Multiphysics 3D*, off by default like every viewer):
  - a depth-tested pass in the `LayeredCanvas`: shaded surfaces (the colour map per vertex, under a light
    at the eye), lines, slices and isosurfaces, from buffers;
  - the legend, the axes and the title drawn by QPainter over it, as now;
  - new shaders compiled beforehand like the others (`scripts/compile-shaders.sh`);
  - the RHI lessons kept: widgets built without a parent and parented after, no buffer freed in the middle
    of a frame (`GpuCanvas::retire`), a failed render retried rather than given up on.
- **On the CPU** (the default, offscreen, and in the tests):
  - a z-buffer rasterizer of our own into a QImage: triangles with interpolated colour and depth, drawn in
    tiles on threads;
  - QPainter for the text over it;
  - deterministic, so the tests compare pixels;
  - aimed at some hundreds of thousands of triangles at the speed of a drag (to be measured in phase B).
    Beyond that, only the boundary is drawn, and slices on demand.
- **Camera:** orbit, pan, zoom, presets (top, front, side, isometric), fit; perspective or parallel; an
  axes triad.
- **Picking:** CPU ray casting against the boundary's triangles, through a bounding-volume tree.
- **Images:** a PNG of the view; a slice as SVG or PDF, since it is 2D.

### Derived values and exports

- **Derived values:** integrals, averages, maxima and minima over volumes, faces and edges; values at
  points (x, y, z); globals (`ht.Tmax`, `es.C11`, `ec.R`) as in 2D.
- **VTU:** the VTK XML unstructured grid, with a `.pvd` collection for times and sweeps. Tetrahedra and
  prisms; P2 as quadratic cells. Written by our own code (no VTK library), and opened in ParaView.
- **Field on a Grid:** 3D grids (nx, ny, nz) as CSV and NPY.
- **A cut's fields:** in the Mode Profile formats (`.mat`, `.npz`, `.csv`).

## The bridge between 2D and 3D

### One mechanism: the link

**A link reads fields of another model's solution, through a mapping**, and gives them a name in the
receiving model:

```json
{"type": "link", "tag": "cut1", "label": "The heater's temperature",
 "props": {
   "source": "heater.qfem3d",
   "study": "std1",
   "instance": {"point": 0, "time": "", "frequency": "", "mode": 0},
   "mapping": "cut",
   "frame": {"origin": [0, 0, 1.5], "u": [1, 0, 0], "v": [0, 0, 1]},
   "variables": [],
   "outside": "NaN",
   "mode": "live",
   "stamp": {"model": "4f1c…", "mesh": "9a02…", "solvedAt": "2026-11-02T14:02:11Z",
             "solvedBy": "27.1.0 (0123abc)"}}}
```

- **`source`** is relative to the receiver's folder; the frame is in the source's unit.
- **An empty `variables`** takes all of them.
- **The fields keep the source's names with the link's tag in front:** `cut1.T`, `cut1.ht.qx`,
  `cut1.ec.Qrh`. Their components are in the receiver's axes: `cut1.ht.qx` is the heat flux along the 2D
  model's x, and `cut1.ht.qz` along the cut's normal (the 2D model's out-of-plane direction).
- **They are used wherever an expression of x and y is taken**:
  - a material's temperature (the semiconductor's T, a refractive index through its dn/dT);
  - a source (a heat source's Q0, a user-defined generation);
  - a boundary value (a Temperature's T0);
  - an initial value (heat's Initial Values T0);
  - a plot, a cut line, an integral.

### The mappings

| Mapping | From → to | A receiver's point reads the source at | Vectors and tensors |
|---|---|---|---|
| Cut plane | 3D → 2D | O + x u + y v | a·u and a·v in the plane; a·n out of it; a stress turned as R σ Rᵀ, R = (u, v, n) |
| Average across | 3D → 2D | The mean (or the integral, per length) along n from s0 to s1, through O + x u + y v | As the cut, averaged |
| Average about an axis | 3D → 2D axisymmetric | The mean over φ at radius x and height y about the axis | (a_r, a_z), and a_φ out of the plane |
| Extrude | 2D → 3D | (x, y) for z0 ≤ z ≤ z1, times a profile p(z) when one is given | The in-plane parts as x and y, the out-of-plane part as z |
| Revolve | 2D axisymmetric → 3D | r, the distance from the axis, and z along it | a_r and a_φ turned by φ; a body-of-revolution mode of order m with its e^(-jmφ) |
| Face | 2D → a planar face of a 3D model | The face's own (u, v) | As the cut |
| Same space | 2D → 2D, 3D → 3D | The point itself, or moved and turned | Turned |

### How a link is evaluated

- **No resampling.** The receiver's quadrature points (or its nodes, for nodal values) are mapped into the
  source and located there: on a swept mesh with the 2D locator and a search in z, otherwise on a grid of
  cells. They are evaluated with the source's own shape functions (P1 or P2, on triangles, prisms or
  tetrahedra), so a field passes as exactly as the source has it.
- **Located once.** Where each receiver point falls is kept while neither mesh changes. Reading another
  solution of the same source (a time, a sweep point, a new solve) is then a weighted sum.
- **Exact cases:**
  - a 2D field extruded onto a swept mesh made from its own 2D mesh is copied node for node;
  - a level cut of a swept mesh is that level's nodes.
- **Time.** A stationary receiver reads one instant (a time, a frequency, a sweep point, a mode). A
  time-dependent receiver reads the source at its own times: linearly between the source's stored times,
  and held past the last one, which is said.
- **Sweeps.** A receiver's sweep of a parameter that the source swept too reads the source's point of the
  same value (`"follow": "w"`).
- **Complex fields** (frequency domain, modes) keep their phase: `cut1.V` and `cut1.V_im`, as the source
  names them.
- **Outside the source.** Points outside its domains take the link's `outside` value: an expression, NaN by
  default, which stops a solve that reads it and says where. The share of the receiver's area or volume
  that lies outside is said.
- **Totals on both sides.** Where a link feeds a source (a heat, a generation), it says the source's total
  over the region, the receiver's total (its thickness counted), and how far apart they are. A mesh too
  coarse to carry the source shows.

### Cutting a 3D solution into a 2D model

*New 2D Model from This Cut…* (the cut-plane tool's menu, or the Cut Plane node's) carries:

1. **Geometry.** The cut of the 3D domains by the plane, as 2D domains, each named after the 3D object it
   came from, so selections by object and the materials apply as they are. For layered models the cut is
   exact:
   - **a level cut** (z = c) is that slab's base topology, arcs included;
   - **a vertical cut** along a line in the plan is the *Layout Cross-Section* construction:
     `cutIntervals()` of each slab's regions along the line, rectangles of the slab's heights. A 2D model
     cut from a 3D layout model therefore equals the cross-section of the same layout, rows and cut, and
     a test holds them equal;
   - **an oblique plane** cuts each slab's prisms, and the pieces are joined per domain;
   - **general solids** (phase F) are cut by the OpenCASCADE section.
2. **Materials.** Each 2D domain gets a copy of its 3D domain's material: its properties and its library
   entry.
3. **Boundaries and their conditions.**
   - A 2D boundary that lies on a 3D face takes that face's condition (a Temperature, a Ground, a Terminal,
     a Heat Flux, an Insulation, a Fixed Constraint, a Boundary Load): the same kind with the same
     expressions, where the 2D model has that physics.
   - A 2D boundary made where the plane runs through the inside of the 3D model takes, by choice, the 3D
     field (T = `cut1.T`), the 3D flux (`cut1.ht.qx`, `cut1.ht.qy` along its normal), or insulation
     (symmetry).
4. **Physics.** None by default, since the usual next step is another physics. The same physics on request:
   a 2D heat model at the 3D boundary values, for a finer look at one cross-section.
5. **Fields.** A link `cut1`, live, with all variables. The dialog proposes their uses by physics:
   - as initial values (heat's Initial Values T0 = `cut1.T`);
   - as sources (a Heat Source's Q0 = `cut1.ec.Qrh`; a User-Defined Generation's rate from a linked one);
   - as coefficients (the semiconductor's lattice temperature; a mode analysis's index through n(T) at
     T = `cut1.T`);
   - as boundary values.

   Or **frozen**: the values stored on the 2D mesh, in the receiver's `.solution`. That is interpolated
   data that stands alone.
6. **Parameters** the geometry uses are copied. **The unit** is the 3D model's. **The thickness** is 1 m
   (per metre), unless the source's extent along the normal is asked for.

The new model opens in a tab and in the *Multiphysics* panel. *Send to an Open 2D Model…* adds the link
(and, by choice, the conditions) to a model that exists, keeping its own geometry; the points outside the
source are said.

### Extruding or revolving 2D into 3D

*Extrude into a 3D Model…* and *Revolve into a 3D Model…* (the 2D panel's Geometry menu) carry:

- **Geometry.** The 2D model's domains from z0 to z1 (or a stack of ranges, each with its own domains), or
  revolved: into layered solids where the shapes are rectangles in (r, z), otherwise in phase F.
- **Materials:** copied.
- **Conditions.** Each 2D boundary's goes to the side faces it sweeps. The top and bottom faces are
  insulated by default, or as chosen.
- **Fields:** a link by extrusion or revolution. Three uses:
  - a 2D process flow's doping into a 3D device's semiconductor;
  - a 2D mode's generation along z times e^(-α(z - z0)) (a photodiode);
  - an axisymmetric VCSEL's heat into an array.

*Use in a 3D Model…* adds the link to a 3D model that exists: by extrusion, by revolution, or onto a face.

### Traceability and refresh

- **The stamp.** A link records its source's stamp: the SHA-1 of the source model's JSON without its
  exports and plots (the rule of `rf25::solveStampOf()`), the mesh's hash, the study, the instance, the
  build and the time.
- **Solved with what.** The receiver's own solution records the stamps it was solved with. It then says
  "solved with heater.qfem3d as solved at 14:02".
- **Stale.** When the source is solved again, a link becomes stale:
  - in memory, by a signal;
  - on disk, by its `.solution`'s stamp, which the file watcher sees.

  The receiver's Linked Fields node, its studies and its *Links* tab mark it. The banner offers:
  - **Refresh:** read the source again;
  - **Solve Chain:** solve the stale sources upstream in order, then this model.
- **Live and frozen.**
  - A live link reads the source's solution in memory, or its `.solution` when the source is closed.
  - A frozen link reads its stored values. It stands alone and survives its source being moved or deleted.
    When the source changes it says so but keeps working, until *Refresh*.
- **Chains and cycles.** A chain such as 3D heat → 2D modes → (by extrusion) a 3D semiconductor is allowed.
  A cycle is refused when the link is made.
- **A source moved or renamed.** The relative path does not find it: the link is stale ("source missing"),
  and its frozen values are used if it has them.
- **Provenance travels.** `fem_describe` and `fem_link` list each link with its stamp and state, and
  exports that have a header (VTU, CSV) name the links used in it.

### Units and coordinate frames

- **Units.** Each model keeps its own length unit. A link converts with both models' unit scales; fields
  are in SI.
- **The cut's frame:**
  - origin O and orthonormal u, v (n = u × v), in the source's unit;
  - shown in the panel, and drawn in the 3D view with its u and v;
  - a v not perpendicular to u is made so, and that is said.
- **The 2D model made from a cut** has its x along u, its y along v, and its origin at O.
- **The presets keep "up" as y.** x = c has u = y and v = z; y = c has u = x and v = z; z = c has u = x and
  v = y. A vertical cut of a layered chip therefore shows the stack's height as the 2D y, as the *Layout
  Cross-Section* does.
- **A vertical cut along a line** from (x0, y0) to (x1, y1) has the *Layout Cross-Section*'s frame and
  origin choices (the cut's start, or its middle).
- **Layouts:** a 3D model's x and y are the layout's (µm, y up), and its z the stack's height.
- **Axisymmetric:** x is r and y is z, as in 2D. Revolve's axis is the 3D model's z axis through a chosen
  point.
- **Time and frequency** are in SI, and angles in degrees, as everywhere else.

### Saved solutions

- **`<model>.solution` beside the model**, for `.qfem` and `.qfem3d` alike: the RF 2.5D file's name and
  pattern (a magic, a format version, a head of stamps, then the body compressed), written whole or not at
  all (`QSaveFile`).
- **The body:** the mesh (nodes, cells, domains), each field's values at each snapshot, the globals, the
  links' stamps, and the frozen links' values.
- **Taken only where the stamps match.** A model opened again takes its solution only where the model's
  stamp and the mesh's both match. Otherwise it says why and solves again, as now.
- **Size:**
  - a million nodes with three fields and twenty snapshots is some 460 MB before compression;
  - a model can keep its last snapshot only, or the snapshots it names;
  - values are kept in double precision, because links read them.
- **2D models gain it too.** The VCSEL bench's 1-6 minute solves are kept as well: the parked sidecar,
  taken up.

### Your workflow, and the devices it serves

**A heater over a ring modulator: the shift of the resonance per mW.**
1. In the layout viewer, *3D from Layout…*: the window around the ring; rows for the substrate, the buried
   oxide, the silicon, the cladding, the heater's metal and the air.
2. Heat Transfer, with the heater as a heat source of 10 mW (or Electric Currents with Joule heating
   between its pads), the chip's bottom at 300 K, and convection on top.
3. Compute. The swept mesh is fine at the waveguide, with three divisions in the oxide.
4. In the 3D view, a cut plane across the waveguide at its hottest point.
5. *New 2D Model from This Cut…*: the core, the oxide and the heater as 2D domains with their materials,
   and a live link `cut1` used as the materials' temperature.
6. In the 2D model, a mode analysis: n_eff at that temperature. A sweep of the heater's power in the 3D
   model, then *Solve Chain*: n_eff against power, and the tuning in pm/mW as a derived value.
7. Change the heater's width in 3D. The 2D model says its link is stale, and *Solve Chain* brings both up
   to date.

**Other devices the bridge serves:**
- **A photodiode with contact fingers:** 2D modes give the generation, extruded along z with
  e^(-α(z - z0)); a 3D semiconductor (phase E) collects it between the fingers.
- **A TSV's thermal stress:** 3D solid mechanics, then a cut, then a 2D semiconductor or a mode analysis
  in that stress field.
- **A VCSEL array's thermal crosstalk:** each axisymmetric VCSEL's heat revolved into one layered 3D model,
  and the array's temperature brought back to one VCSEL by an average about its axis.
- **Interconnect:** a 3D capacitance matrix around vias and crossings, and a cut along a line to compare
  with the 2D line parameters.
- **A process flow:** a 2D flow's doping extruded into a 3D device's layers.
- **An RF passive's heating:** an RF 2.5D solve's loss in its metals as the heat source of a 3D model of the
  same stack. That is a link of its own kind, after phase D, since the RF 2.5D solution is currents on
  sheets rather than fields in volumes.

## Claude's tools

### The existing tools, on 3D models

| Tool | On a 3D model |
|---|---|
| `fem_describe` | `what` geometry lists domains (number, objects, volume, bounds), boundaries (number, the two domains, objects, area, the normal of a planar face, side or top or bottom), edges (number, faces, length) and points. `selection` with `level` domain, boundary, edge or point tests a rule. `what` links lists the links as `fem_link` does. |
| `fem_model` | `action` new takes `"space": "3D"` (or a `.qfem3d` path). `action` layout makes a 3D model of a layout: `path`, `cell`, `window` [x0, y0, x1, y1], and `stack` (rows, or a `.qrf25` to take them from). `action` import takes STEP, IGES and STL (phase F). |
| `fem_edit` | Unchanged in form; the 3D kinds and the 3D selection rules. |
| `fem_build` | `what` mesh answers cells, nodes, prisms and tetrahedra, quality, and per physics the unknowns and the estimate: memory and time, direct and iterative. |
| `fem_solve` | Says the estimate before it starts, and refuses past the limit with what would fit. `solver` (direct, iterative) for this solve alone; `force` to go past the limit. |
| `fem_stop` | Unchanged. |
| `fem_evaluate` | `at` takes [[x, y, z], ...]; `over` takes domain, boundary or edge; `along` {from, to, points} is a 3D cut line. |
| `fem_plot` | 3D plot groups. `expression` makes one: a surface, with `slices` ([{"z": 1.5}, {"x": 0}]), `isosurfaces` (levels) and `clip` ({origin, normal}). `camera` takes a preset (top, front, side, iso) or azimuth, elevation, zoom and target; `hide` takes a selection of domains. `screenshot` takes the picture. |
| `fem_export` | VTU; Field on a Grid with `nz`. |
| `fem_converge` | Unchanged; the refinement divides the sizes and multiplies the divisions in z. |

### New tools

**`fem_cut`: a 3D model cut by a plane.**
- Arguments:
  - `path`: the 3D model;
  - `plane`: {"z": 1.5}, {"x": 10} or {"y": 0}; or {"from": [x0, y0], "to": [x1, y1]} (a vertical cut
    along a line in the plan, its x along the line and y up); or {"origin": [x, y, z], "normal": [nx, ny,
    nz]}, with "u" when the in-plane axes matter;
  - `action`: preview, new, send or dataset;
  - `study`, and the instance: `time`, `frequency`, `point`, `mode`;
  - `to`: the new model's file (untitled when not given); `target`: an open 2D model, for send;
  - `carry`: some of materials, conditions, physics and parameters (default: materials, conditions and
    parameters);
  - `use`: [{"variable": "T", "as": "coefficient", "on": "semi"}, {"variable": "ec.Qrh", "as": "source",
    "on": "ht"}], with `as` initial, source, coefficient or boundary;
  - `variables` (default: all), `mode` (live or frozen), `outside`, and `name`: the link's tag (`cut1`).
- Answer:
  - the model's path or title, its unit, and the frame (origin, u, v, normal);
  - its domains: number, name, material, area, and the 3D domain each came from;
  - its boundaries, with the condition carried and where it came from (a 3D face, or the cut);
  - the link: tag, variables, stamp;
  - the totals compared, and notes (points outside, arcs made polylines).
- `preview` answers the same without making anything. `dataset` makes a Cut Plane in the 3D model and
  answers its tag, for `fem_plot` and `fem_evaluate`.

**`fem_extrude`: a 2D model into 3D.**
- Arguments:
  - `path`: the 2D model;
  - `how`: extrude (`z` [z0, z1] or `ranges` [[z0, z1, [objects]], ...], `divisions`, `angle` for a wall)
    or revolve (`axis` {point, direction}, `angle`);
  - `action`: new or send; `to`; `target` (a 3D model);
  - `carry`, `use`, `variables`, `mode`, `name`, as `fem_cut` takes them.
- Answer: the 3D model, its domains (number, name, material, volume), the faces carried (side, top,
  bottom, with their conditions), the link, and whether the revolution was layered or waits for phase F.

**`fem_link`: a model's links.**
- Arguments:
  - `path`: the receiving model;
  - `action`: list, add, refresh, freeze, live, remove or check;
  - `tag`;
  - for add: `source`, `study`, the instance, `mapping` (cut, average, axisaverage, extrude, revolve, face,
    same), `frame`, `range` or `profile`, `variables`, `mode`, `outside`, `name`.
- Answer, for each link:
  - its tag, source (path, whether it exists, whether it is open), study, instance, mapping, frame,
    variables and mode;
  - `stale` and why: the source solved at 15:10 after the link read it at 14:02, its model changed, or the
    source is missing;
  - the nodes that use it;
  - with check, the totals compared and the share outside.

**Around them:**
- **The window's generic tools.** `screenshot` of a 3D tab uses the camera `fem_plot` set; `get_state`
  lists 3D models; `get_ui` and `set_ui` reach the 3D panel's tree and the view's toolbar, as for 2D;
  `open_document` opens `.qfem3d`.
- **Permissions as now.** Undoable edits are allowed in auto mode. A new model, a `.solution` or an export
  is a new file. A link to a file outside the workspace is asked about by the files guard, as any other
  read is.
- **Each description stays under 2048 characters**, and the tool-arguments lint covers the new keys.

## Testing and validation

**Analytic cases:**
- **Invariance.** A model uniform in z, solved in 3D on a swept mesh of its own 2D mesh, equals the 2D model
  times its thickness, to 1e-10. That holds for heat, electrostatics and currents, and for solid mechanics
  in plane strain with the matching conditions. It checks every weight and every face.
- **Patch tests:** a linear field exact for P1 and a quadratic one for P2, on tetrahedra and on prisms,
  distorted ones included.
- **Manufactured solutions:** convergence as h² for P1 and h³ for P2 in L2, on both cell types.
- **A cube's capacitance:** 0.66068 × 4πε0 a, the known numerical value.
- **A sphere's capacitance:** 4πε0 R, within 0.1 % on curved P2 tetrahedra (phase F).
- **Spreading:** a disc contact's resistance 1/(4σa) on a half-space, and a disc heater's thermal
  resistance 1/(4ka).
- **Solid mechanics:**
  - a cantilever against beam theory;
  - a pressurized thick cylinder and sphere against Lamé;
  - free thermal expansion without stress;
  - a fully held block at -EαΔT/(1 - 2ν).
- **The semiconductor:**
  - a 3D diode equal to the 1D one;
  - a FinFET wide enough to equal the 2D MOSFET per unit width.

**Against 2D, through the bridge:**
- **Round trip.** A 2D field extruded into 3D and cut at any z equals the 2D field at the 2D nodes, to
  rounding, for P1 and P2.
- **Revolution.** An axisymmetric field revolved and cut through its axis gives the 2D field and its
  mirror image.
- **Linear fields.** a x + b y + c z cut by an oblique plane is the exact linear 2D function.
- **Vectors and tensors.** A known vector field splits into its in-plane and normal parts, and a stress
  keeps its invariants when turned.
- **Conservation.** A 3D heat source's integral over a slab equals the linked 2D source's integral times
  the thickness: to rounding where the mapping is exact, and as reported otherwise.
- **Cut geometry.** A vertical cut of a 3D layout model equals the *Layout Cross-Section* of the same
  layout, rows and cut: domains, names, materials and areas.
- **The chain.** The heater over a ring: as the heater's length grows, the temperature on the 3D model's
  middle cut approaches the 2D model's for an endless heater, and n_eff with it.
- **Staleness:**
  - a source solved again marks its receivers stale;
  - a frozen link survives reopening without its source;
  - a saved solution that no longer matches its model is not used, and says why.

**The window and the tools:**
- The two panels follow the tab in front.
- The 3D view's CPU path draws offscreen, compared by pixels.
- The GPU path is tested after `gpu::setWanted`, as for every viewer.
- The window's actions (Undo, Redo, Save) are driven through the window, not the document. RF 2.5D found
  that its document's undo worked while Edit > Undo did nothing.
- Claude's tools on both kinds of model.

**Within CI's limits:**
- the Linux job is Debug with ASan (330 minutes, 125-180 when green);
- the macOS runner has 3 CPUs and 7 GB;
- so 3D tests use coarse meshes under `slowBuild()`, the heavy ones take the `fem_memory` resource lock,
  and their timeouts are set as the laser tests' are.

**And a breaks round for each phase**, as for every feature.

## Phases, sizes and risks

| Phase | What | Lines with tests | Weeks | Its risks |
|---|---|---|---|---|
| **A** | Saved solutions (`.solution` for `.qfem`); the field-source interface; links between 2D models (same space, moved or turned), live and frozen; stamps, staleness, *Solve Chain*; `fem_link`. No 3D | 5,000 | 3-4 | A stamp too strict (a solve lost to a trivial edit) or too loose (a wrong one kept): RF 2.5D's rule is the start. Solution files of hundreds of MB |
| **B** | The 3D document, panel, menu and dock page; layered geometry (stack, extrude with a wall angle, box and cylinder, layout with stack and processing, layered booleans); 3D topology and selections; the swept mesh; the 3D view (CPU, then GPU opt-in), geometry, mesh and picking | 14,000 | 9-11 | The 3D topology's numbering and selections, the part users touch most. The CPU view's speed on large layouts. The known RHI traps |
| **C** | Heat, electrostatics and currents in 3D (P1 and P2 on prisms and tetrahedra), terminals, Joule heating, time; AMGCL and the METIS ordering, measured first; estimates and the limit; slices, isosurfaces, clip planes, 3D derived values, VTU, Field on a Grid in 3D, Interpolation of three arguments; the `fem_*` tools on 3D models | 8,000 | 6-7 | AMG on high-contrast coefficients. Memory at size |
| **D** | The bridge: cut, average across, average about an axis, extrude, revolve and face mappings; *New 2D Model from This Cut…*, *Send to an Open 2D Model…*, *Extrude* and *Revolve into a 3D Model…*; conservation and outside reports; `fem_cut`, `fem_extrude`; the heater-over-ring chain test | 6,000 | 4-5 | Oblique cuts of layered models. What a condition means when carried across |
| **E** | Solid mechanics in 3D (P2, thermal stress); the semiconductor on prisms with all its models; GMRES with ILUT and a direct fallback; a semiconductor's initial state from a link | 7,000 | 6-8 | The 3D Jacobian's iterative solve. Three unknowns per node |
| **F** | General solids: Gmsh with OpenCASCADE as an optional part of the build; primitives on any axis; revolve, sweep, loft; booleans and fillets; STEP, IGES, BREP and STL with healing; free tetrahedral meshes (curved P2, boundary layers); layered and general solids in one model; cuts of general solids; packaging on all six release platforms | 6,000, and the build and CI work | 6-8 | Packaging: the macOS 13 target, which Homebrew bottles built for newer systems do not meet (so both are built from pinned sources); Windows on ARM; some tens of MB per package (to be measured). The pairing of OpenCASCADE 8.0.1 with Gmsh 4.15.2 (pin a 7.9 release if they do not build together). CI time. Bad STEP files: healed or refused, never a crash (OpenCASCADE exceptions caught at the boundary) |
| **G** | On demand: 3D magnetics and frequency-domain waves (Nédélec on tetrahedra and prisms, PMLs, ports); mode matching across cross-sections (stage 0 of 12 October); the semiconductor on tetrahedra | - | - | Each a proposal of its own |

**Totals:**
- **A to D: about 33,000 lines and 22-27 weeks.** That is your workflow both ways (a 3D thermal or
  electrical field, cut, and handed to 2D physics; a 2D model or result carried into 3D), on layered
  models.
- **A to F: about 46,000 lines, of which some 13,000 are tests, and 34-43 weeks.**
- For scale, the 2D engine is 68,963 lines for twelve physics.

**Each phase is useful when it stops:**
- **A alone:** one thermal model feeding a semiconductor model in another file; a process flow feeding
  several device models; one long solve read by many; and 2D models no longer solved again at each
  opening.
- **After B and C:** 3D heat, electrostatics and currents on layouts and stacks, with their 3D results.
- **After D:** the workflow.

### Files

| File | What | Lines |
|---|---|---|
| `multiphysics/fem_kept.{h,cpp}` | Saved solutions | 1,200 |
| `multiphysics/fem_link.{h,cpp}` | Links: the field-source interface, mappings, stamps, staleness | 3,000 |
| `multiphysics/fem3_geometry.{h,cpp}` | Layered solids, layered Form Union, 3D topology and selections, the cut's geometry | 3,500 |
| `multiphysics/fem3_mesh.{h,cpp}` | The swept mesh, divisions in z, wall angles, quality | 1,500 |
| `multiphysics/fem3_fe.{h,cpp}` | Tetrahedra and prisms, P1 and P2, quadrature, the 3D space and locator | 1,500 |
| `multiphysics/fem3_scalar.cpp`, `fem3_elastic.cpp` | 3D assembly of heat, electrostatics, currents, solid mechanics | 2,500 |
| `multiphysics/fem3_semiconductor.cpp` | The box method on prisms | 3,000 |
| `multiphysics/fem3_results.{h,cpp}` | Slices, isosurfaces, cut lines, 3D derived values, VTU | 2,000 |
| `multiphysics/fem_amg.{h,cpp}`, `third_party/amgcl/` | AMG behind `LinearSystem` | 600 |
| `multiphysics/fem3_occ.{h,cpp}` | The Gmsh and OpenCASCADE route (an optional library) | 3,500 |
| `multiphysics3ddoc.{h,cpp}`, `multiphysics3dpanel.{h,cpp}`, `qucs_multiphysics3d.cpp`, and the 2D panel's links | The document, the panel, the menu, the dialogs of the bridge | 5,000 |
| `view3d.{h,cpp}`, `view3dgpu.{h,cpp}`, `shaders/fem3_*` | The 3D view, CPU and GPU | 3,200 |
| `qucscontrol_fem.cpp` and `qucscontrol_fem3d.cpp` | The tools | 2,500 |
| `tests/test_fem3_*.cpp`, `test_fem_link.cpp`, `test_multiphysics3d_doc.cpp` | The tests | 13,000 |

## Risks

- **3D sizes.** A million unknowns is an ordinary 3D model and a costly direct solve (measured above).
  Mitigations:
  - iterative by default;
  - the METIS ordering;
  - the estimate before every solve, and a limit;
  - coarse defaults in z.
- **AMG is not proven here yet.** It is chosen from its design and published use, and measured first in
  phase C against the incomplete Cholesky, which stays as the fallback.
- **High-contrast coefficients** (metal beside oxide, a semiconductor's doping over decades) are where
  iterative solvers stall. Mitigations: the direct path while it fits, and said stalls rather than silent
  ones.
- **The 3D topology is what users touch**: numbering, selections, picking. It needs the 2D one's care, and
  its tests.
- **The 3D view.** The CPU path must stay quick on layouts of many shapes. The GPU path carries the known
  RHI traps (parenting, frames, buffers).
- **Live links and memory.** A source's solution is shared, not copied into each receiver, and closed
  sources are read on demand and let go least recently used first.
- **Phase F's dependency.** OpenCASCADE and Gmsh are large. It is an optional part of the build, so the
  rest never waits for it.
- **Scope.** "3D" can absorb years. The fence is the non-goals above: no CAD modeller, no 3D full-wave or
  3D magnetics before phase G.

## Decisions for you

1. **The file.** A separate `.qfem3d` document, as proposed, or one `.qfem` holding 2D and 3D components?
2. **Where to start.** Phase A first (saved solutions and links between 2D models, useful before any 3D),
   as proposed, or the 3D document and view first (phase B)?
3. **The first 3D physics.** Heat, electrostatics and currents, as proposed, or is solid mechanics or the
   semiconductor more urgent?
4. **General solids.** Gmsh with OpenCASCADE linked in phase F, as proposed? Or Gmsh found as an installed
   program (as ngspice is), with no packaging work but no seamlessness? Or no general solids at all? STEP
   import is the main reason for the dependency: is it needed?
5. **A link's default.** Live, as proposed, or frozen?
6. **A direct solver off macOS.** Take up the parked sparse backend (SuiteSparse) for 3D, or iterate only
   on Linux and Windows?
7. **The METIS ordering for 2D models.** Measure it on the 2D models as a change of its own, or leave 2D
   as it is?
8. **Validation targets.** Which two or three devices must it get right first: the heater over a ring, the
   photodiode with fingers, a TSV, a VCSEL array? And against what: your measurements, another solver's
   results, or published ones? They shape the tests more than anything else here.
