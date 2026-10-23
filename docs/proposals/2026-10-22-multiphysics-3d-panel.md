# Proposal: a Multiphysics 3D panel, and fields passed between 2D and 3D

*22 October 2026, after Qucs-S 27.0.3 (519f625). A written proposal only; nothing here is built.*

*The request: "initiate a 3D version of our multiphysics and add a new panel called 'Multiphysics 3D'.
My goal is to keep the original 'Multiphysics' panel specifically for 2D simulations and this new panel
with focus on 3D simulations. The idea is to be able to pass simulation data and results between the
original 2D and this new 3D in a seamless way. Sometimes it is needed to run a 3D simulation and find
the results in a 2D cross-section and then pass it to a 2D solver for further physics simulations."*

*It builds on [the 3D proposal of 12 October](2026-10-12-multiphysics-3d.md). That one is about the
engine (its stages 0, 1 and 2), and it stands. This one adds the panel, the document and the passing of
fields between 2D and 3D. The facts about this tree were checked in it on 22 October 2026. Sizes and
weeks are estimates.*

## In short

- **A 3D model is a document of its own.** It is a `.qfem3d` file with its own tab of the left dock,
  *Multiphysics 3D*, and a 3D view. The *Multiphysics* tab stays 2D: none of its nodes gain 3D options.
- **Both run on one engine and one model schema.** They share the tree, parameters, expressions,
  materials, studies and derived values. What differs is the geometry, the mesh, the elements and the
  plots.
- **Fields pass between models through links.** A link reads a field of one model's solution in
  another model, through a mapping:
  - a cut plane (3D to 2D);
  - an average across a thickness (3D to 2D);
  - an extrusion or a revolution (2D to 3D);
  - a face (2D onto a 3D boundary).

  A link's fields become variables in the receiving model, so a physics, a boundary, an initial value,
  a plot or a derived value can use them as it uses x and y.
- **Your workflow is one command.** Solve in 3D, take a cross-section, solve more physics on it in 2D:
  in the 3D view, *New 2D Model from This Cut…* makes the 2D model's geometry from the cut, with each
  region's name and material, and links the 3D fields on the plane.
- **A link is live or frozen.** A live link is read again when its source is solved again, and a stale
  one is said. A frozen link is a snapshot kept with the receiving model, so its file stands alone.
- **The order of work:**
  1. links between 2D models first: useful at once, and testable without any 3D;
  2. then the 3D document, panel, view and the engine's first stage (extruded, layered models on
     prisms);
  3. then the mappings between 3D and 2D, and the cut-to-2D workflow;
  4. then the semiconductor and solid mechanics in 3D.

## What there is to build on

| Part | Now | What it gives this proposal |
|---|---|---|
| The engine (`qucs/multiphysics/`) | About 68,700 lines, 2D and axisymmetric: geometry, CDT meshes, P1/P2 and Nédélec elements, twelve physics (electrostatics, currents, heat, solid mechanics, magnetics, mode analysis, frequency-domain waves, eigenfrequency, semiconductor, geometrical optics, quantum wells, PDE) | The physics to carry to 3D, and the 2D solvers a cut is passed to |
| Document, panel, view, tools | About 8,900 lines: `multiphysicsdoc.cpp`, `multiphysicspanel.cpp` (the left dock's tab: the tree and the settings of the node chosen), `multiphysicsview.cpp`, `qucscontrol_fem.cpp` | The pattern the 3D document and panel follow |
| Tests | About 34,600 lines of `test_fem_*.cpp` and `test_multiphysics_doc.cpp` | The 2D side, unchanged |
| The model file | JSON with a `components` array, of which one is used (`fem_model.h`). Its `space` is `2D` or `axisymmetric` | The schema the 3D file shares |
| Fields from outside | *Interpolation* functions: a table of one argument, or two on a grid, bilinear (an imported optical generation map) | The file-based form of a link today |
| Results into another model | A semiconductor's doping *From the process flow*: the only hand-over between steps, within one model | The precedent for a link |
| Evaluation anywhere | `Solution` and its `PointEvaluator`: any expression of the fields at any point. A `Locator` finds a point's triangle (`fem_results.h`) | What a link reads |
| Mode profiles | A mode on a grid as `.mat`, `.npz` or CSV, and a mode's overlap with another field (bc35507) | An exchange format, and half of mode matching |
| GPU drawing | Qt's RHI canvases (`rhicanvas.h`: `LayeredCanvas`, `Viewport2D`) since d01d425, Metal, Direct3D 11 or OpenGL. Every viewer draws on the CPU by default, the GPU a choice per viewer | The base of the 3D view |
| Solvers | Sparse direct only: Apple Accelerate's (Cholesky; LU on macOS 26) or Eigen's | Enough for 2D; 3D needs an iterative path |
| Saved solutions | None: a model is solved again after it is reopened (the parked "solution sidecar") | Live links to closed models need one |

## The panel and the document

### Two kinds of model

| | Multiphysics (2D) | Multiphysics 3D |
|---|---|---|
| File | `.qfem`, "qucs-s multiphysics 1" | `.qfem3d`, "qucs-s multiphysics 3d 1" |
| Document | `MultiphysicsDoc`: the 2D view | `Multiphysics3DDoc`: the 3D view |
| Left dock | *Multiphysics*: its tree | *Multiphysics 3D*: its tree |
| Space | 2D, axisymmetric | 3D |

The panel follows the tab in front, as the 2D one does today (`qucs.cpp:3802`): a 2D model brings up
*Multiphysics*, a 3D model *Multiphysics 3D*. Each panel shows its own kind only.

**Why a separate kind, and not a 3D component inside the `.qfem`:**
- it is the separation asked for;
- the 2D panel's settings do not grow a z;
- the files stay what they are.

The schema is shared, so a file holding both kinds of component stays possible later, if it is ever
wanted.

### The 3D model's tree

- **Global Definitions:** parameters, functions, interpolations, as in 2D. New: *Linked Fields* (below).
- **Geometry** of the engine's first stage (extruded and layered):
  - *Layer Stack*: z-ranges, each named and given a material;
  - *Extrude*: a 2D model's geometry, or a sketch drawn as in 2D, through a z-range;
  - *Layout Cell*: a layout and a layer stack, as *Cross-Section to Multiphysics…* already takes them;
  - boxes and z-axis cylinders on layers;
  - unions and differences by layer.
- **Materials:** the same library and the same nodes.
- **Physics:** the kinds ported to 3D, each marked in the node kinds' registry as 2D only, 3D only or
  both.
- **Mesh:**
  - the base 2D mesh (CDT over every layer's shapes, as in 2D);
  - each layer's divisions in z, graded.
- **Studies:** the same kinds: stationary, time, frequency, sweeps.
- **Results:**
  - 3D plot groups: a surface on the boundaries, slices, isosurfaces, arrows in a volume (only when
    asked);
  - datasets: a cut plane, a cut line, a cut point;
  - derived values over volumes, faces and lines;
  - exports.

### The 3D view

- Orbit, pan and zoom. View presets: top, front, side, isometric.
- Picking domains, faces, edges and points for selections, as clicking does in 2D.
- A section box that clips the model, and a cut-plane tool: a plane dragged through the model, or typed
  in (x, y or z = c, or a point and a normal).
- Legends, and the light and dark themes.
- It draws on the RHI canvas, with a CPU fallback for offscreen runs, the tests, and where the GPU is
  not chosen (the GPU stays a choice, as for every viewer). The fallback is the triangles of the
  boundary faces in depth order, through QPainter. That is enough for a model's faces, if not for
  millions of cells.

## Links: fields between models

### What a link is

A link names:
- its source: a model file (2D or 3D), a study, and a solution's instant or sweep point (the
  `Instant` and `SweepPoint` property kinds exist);
- a mapping;
- the variables taken.

It gives them a name in the receiving model. A 2D link called `th3`, cut from a 3D thermal model,
gives `th3.T`, `th3.ht.q`, and so on. Anywhere an expression of x and y is taken, they can be used:
- the semiconductor's lattice temperature: `th3.T`;
- a heat source: `th3.Qh`;
- an optical generation rate;
- a boundary's temperature, or an initial condition;
- a plot, or an integral.

### The mappings

| Mapping | From, to | A point of the receiving model reads |
|---|---|---|
| Cut plane | 3D to 2D | the source at origin + u e1 + v e2, for the 2D point (u, v) |
| Average across | 3D to 2D | the source's mean (or integral) along the plane's normal, over a thickness |
| Extrude | 2D to 3D | the source at (x, y), at every z of a range, or times a profile of z (exp(-α z)) |
| Revolve | 2D axisymmetric to 3D | the source at (r = √(x² + y²), z) |
| Face | 2D onto a 3D boundary | the source on a face mapped into its plane (a boundary condition, a load) |
| Same space | 2D to 2D, 3D to 3D | the source at the same point, or moved or turned |

**Vectors and tensors** are taken in the receiving model's axes:
- **A cut:** the in-plane components along e1 and e2 become the 2D model's x and y. The normal
  component becomes the 2D out-of-plane component: a 3D current density normal to the plane is a 2D
  magnetics' Jz.
- **An extrusion:** the reverse. A stress tensor is turned the same way, into a 2D plane-strain model's
  initial stress.

**Units and frames:**
- each model keeps its own length unit;
- the link keeps its transform (origin and axes), and the panel shows it;
- a 2D model made from a cut has its x along e1 and its origin at the plane's origin.

**Complex and time-dependent fields:**
- complex fields (frequency domain, modes) pass with their phase;
- a time-dependent source is read at an instant, or at the receiving study's own times (a 2D transient
  reading a 3D transient, between its stored times).

### Live and frozen

- **Live (the default):**
  - read from the source's solution in memory, or from its saved solution when it is closed;
  - when the source is solved again, the receiving model is marked stale, and its studies and panel say
    so.
- **Frozen:**
  - the mapped fields are kept with the receiving model, on its own mesh (or on the cut's
    triangulation), so the file stands alone;
  - *Refresh* reads them again.

Live links to closed models need saved solutions. That is the parked "solution sidecar": a model's
fields kept beside it, checked against the model when it is opened. I propose to build it here (phase
A). A 3D solve can take minutes, and solving it again at every opening would make live links useless.

### Accuracy

- **No resampling.** A link evaluates the source with its own shape functions (P1 or P2, on triangles,
  prisms or tetrahedra), at the receiving model's quadrature points. Nothing is resampled onto a grid,
  so a field is passed as exactly as the source has it. 3D needs a locator of its own (a bucket grid of
  cells); the 2D `Locator` exists.
- **Totals both sides.** A power or a heat passed as a source is integrated by the receiving model's
  quadrature. The link says both totals, the source's over the region and the receiver's, and how far
  apart they are, so a mesh too coarse to carry a source is seen.
- **Outside the source.** A point of the receiving model outside the source's domain takes a value
  given (zero, or an expression), and the link says how much of the area that is.

## Your workflow: a 3D result, a 2D cross-section, more physics in 2D

1. **The cut.** In the 3D view, a cut plane is made (Results > Cut Plane, or the toolbar's plane tool).
   It is a dataset, so every 2D plot works on it: surface, contour, line graph. A cut plane is drawn
   with the same `SurfaceData` the 2D view draws (`fem_results.h`).
2. **The 2D model.** The cut's menu offers *New 2D Model from This Cut…*:
   - **its geometry:** the 3D domains' intersection with the plane, as 2D polygons, each region with its
     3D domain's name and material. For an extruded model this is exact and cheap:
     - a cut at a z is that layer's 2D geometry;
     - a vertical cut is a rectangle for each layer's span along the line.
   - **a link** called `cut1`: the 3D solution on that plane, all its variables, live;
   - **the parameters** the geometry uses, copied; the unit the 3D model's.

   The new model opens in a tab and in the *Multiphysics* panel.
3. **More physics on it.** The user adds physics that read `cut1.*`. Examples:
   - a 2D semiconductor at the 3D temperature;
   - a 2D mode analysis with the thermo-optic index of the 3D thermal field;
   - 2D modes in a waveguide strained in 3D (the photoelastic index);
   - a 2D drift-diffusion with the generation of a 3D optical model.
4. **Into an existing model.** *Send to an Open 2D Model…* adds the link to a model that exists, keeping
   its geometry. The two need not match: the points outside the source are said.

**The other way:**
- *Extrude into a 3D Model…* in the 2D panel: its geometry and materials through a z-range, and its
  solved fields linked by extrusion.
- *Use in a 3D Model…*: a link added to an open 3D model, by extrusion, revolution or onto a face.

**Devices of this tool that would use it:**
- **A heater over a ring modulator.** 3D heat, with the heater's finite length and its contacts. Then a
  cut across the waveguide, and 2D modes with the thermo-optic index: the resonance's shift per mW.
- **A photodiode with contact fingers.** 2D modes give the optical generation, extruded into 3D with the
  mode's decay along the absorber, exp(-α z). 3D drift-diffusion (phase E) collects it between the
  fingers.
- **A TSV's thermal stress.** 3D mechanics, then a cut, then a 2D semiconductor's mobility under that
  stress (if the 2D semiconductor takes stress then).
- **A VCSEL array's thermal crosstalk.** Each axisymmetric VCSEL revolved into one 3D heat model.
- **A process flow in 3D.** A 2D process flow's doping, extruded into a 3D device's layers.

## The 3D engine

The 12 October proposal's stages stand:
- **Stage 0:** mode matching across a junction of cross-sections. Not built yet; the overlaps it needs
  are (bc35507).
- **Stage 1:** extruded, layered 3D on prisms. A mesh of prisms extruded from one 2D Delaunay mesh keeps
  the semiconductor's box method sound: each prism's Voronoi cell is the 2D cell times its z-interval,
  so every weight stays positive.
- **Stage 2:** general solids on tetrahedra, only for a concrete need, or through an external program.

What has changed since then:
- **The engine is larger.** 28,000 lines then, 68,700 now: more physics to port, but more to cut to.
- **Solvers.** Accelerate's LU makes moderate 3D direct solves possible on macOS 26. An iterative path
  is still needed beyond some 10⁵ unknowns:
  - CG with algebraic multigrid for the symmetric scalar problems (AMGCL, MIT licence, header-only);
  - BiCGSTAB or GMRES with ILU for drift-diffusion, measured before it is trusted;
  - a direct fallback when the iteration stalls.
- **The 3D view's GPU layer exists,** on the RHI canvases.
- **Layered inputs.** The process flow and the layout's cross-section already make layered inputs.

**The physics, in order:**
1. electrostatics, electric currents and heat: one assembly per scalar physics (`fem_scalar.cpp`),
   written over a cell type;
2. solid mechanics;
3. the semiconductor;
4. optics, through 2D modes and mode matching. 3D full-wave and 3D magnetics need edge elements in 3D,
   which is stage 2.

## One engine for both dimensions

How the code would be shared rather than copied:

- **The schema.** The node kinds' registry (`fem_model.cpp`) marks each kind 2D only, 3D only, or both.
  Each panel offers its own and the shared ones.
- **Expressions.** x, y and z in a 3D model's scope, as x and y in 2D. The same units and functions.
- **Meshes.** A `Mesh3` beside `Mesh`: nodes in 3D, cells (prisms, later tetrahedra), faces with their
  boundaries, edges.
- **Elements and assembly.** Written over the cell (triangle, prism, tetrahedron) where the loops are
  the same, as the scalar physics' are.
- **Solutions.**
  - A `Solution3` holds the same `Field` and `Variable` names as a 2D solution, so plots, derived values
    and Claude's tools read either.
  - Both answer one interface: a field's value at a point, and integrals over a domain or a boundary.
    Links, cut planes and Claude's `fem_evaluate` read through it.
- **Results.** A cut plane is a 2D dataset, drawn by the 2D view's own plots.
- **Studies.** The same kinds, the same solver backends, and the iterative path added.

## Claude's tools

- **The existing tools take either kind.**
  - `fem_model` makes, opens and saves `.qfem3d` models too.
  - `fem_edit`, `fem_build`, `fem_solve`, `fem_stop` take either kind.
  - `fem_evaluate` takes points (x, y, z).
  - `fem_plot` shows 3D plot groups, and draws a cut plane as a 2D plot.
  - `fem_export` also writes VTK (`.vtu`) for ParaView, and a cut's fields as CSV or `.npz`.
- **New tools.**
  - `fem_link`: a model's links listed (what is stale said); a link added, refreshed, frozen or removed.
  - `fem_cut`: a 3D model's cut plane made into a new 2D model, or sent to an open one.
- **The window.**
  - `screenshot` of a 3D view takes a camera: a preset, or an azimuth and an elevation.
  - `get_state` lists 3D models.
  - The 3D panel's tree and the 3D view's bar work through `get_ui` and `set_ui`, as the 2D ones do.
- **Permissions as now.** Undoable edits are allowed in auto mode. A new model or a sidecar is written
  anew. A link to a file outside the workspace is asked about by the files guard, as any other read is.

## Files

- **`.qfem3d`.** JSON with the same top-level shape as `.qfem`. A link keeps:
  - its source's path, relative to the receiving model;
  - the study's tag, and the instant or sweep point;
  - the mapping and its transform;
  - the source model's revision, for staleness.
- **Saved solutions.** `<model>.qfem.sol` and `<model>.qfem3d.sol`: the fields, and each snapshot of a
  time or a sweep, with a hash of the model they belong to. A solution not matching its model is
  ignored, and said.
- **Frozen links.** In the receiving model's sidecar.
- **Exports.** VTK for 3D solutions; the existing mode-profile formats for cuts.

## Tests

- **Invariance:** a 3D model uniform in z equals the 2D one times its thickness, for each physics, to
  10⁻¹⁰ (from 12 October).
- **Round trip:** a 2D field extruded into 3D, then cut at any z, equals the 2D field at the 2D nodes,
  exactly for P1.
- **Revolution:** an axisymmetric field revolved, then cut through the axis, gives the 2D field and its
  mirror image.
- **Analytic mappings:**
  - a linear 3D field, a x + b y + c z, cut by an oblique plane, is the exact linear 2D function;
  - a known vector field splits into its in-plane and normal parts correctly.
- **Conservation:** a 3D heat source's total over a slab equals the linked 2D source's integral times
  the thickness.
- **The cut's geometry:**
  - a vertical cut of an extruded model gives the right rectangles, names and materials;
  - a cut at a z gives that layer's 2D geometry.
- **Staleness:**
  - a source solved again marks its receivers stale;
  - a frozen link does not go stale, and survives reopening without its source;
  - a saved solution that no longer matches its model is not used.
- **The window:**
  - the two panels follow the tab in front;
  - the 3D view's CPU path draws offscreen;
  - Claude's tools work on both kinds.
- **Breaks,** as for every feature.

## Plan and size

| Phase | What | Lines (with tests) | Weeks |
|---|---|---|---|
| A | Links between 2D models: the field-source interface, the *Linked Field* node (same space), live and frozen, staleness, saved solutions, `fem_link` | ~3,500 | 3-4 |
| B | The 3D document, panel and view; layer stack, extrusion and layout-cell geometry; the prism mesh; cut planes as datasets; VTK export | ~6,000 | 6-8 |
| C | Electrostatics, currents and heat in 3D; the iterative solver path; the invariance tests | ~3,500 | 4-5 |
| D | The 2D-3D mappings (cut, average, extrude, revolve, face); *New 2D Model from This Cut…*; *Extrude into a 3D Model…*; `fem_cut` | ~3,000 | 3-4 |
| E | The semiconductor and solid mechanics in 3D | ~5,000 | 6-8 |
| F | Mode matching (stage 0) for devices along z. 3D full-wave, 3D magnetics and general solids only for a concrete need (stage 2) | - | - |

**Totals:**
- Phases A to D give your workflow (a 3D thermal or electrical field, cut, and passed to 2D physics) in
  about 16 to 21 weeks.
- Phases A to E come to about 21,000 lines and 22 to 29 weeks.

**Phase A is useful on its own,** before any 3D:
- one thermal model feeding a semiconductor model in another file;
- a process flow feeding several device models;
- one long solve read by many models.

## Risks

- **3D sizes.** A million unknowns is beyond a laptop's direct solve. Mitigations:
  - the iterative path;
  - a solve's estimated size and memory said before it starts;
  - coarse defaults in z.
- **Semiconductors off prisms.** Tetrahedral meshes (stage 2) need Delaunay meshes that conform to
  every interface, or corrected weights. Prisms avoid this, which is why stage 1 is extruded.
- **Cutting general solids** needs a geometry kernel. For extruded models it is exact and cheap.
- **The 3D view.** It needs its CPU fallback for offscreen runs and the tests. The GPU canvases' lessons
  so far apply: the RHI widgets' set-up order, and a buffer not freed in the middle of a frame.
- **Live links and memory.** Large solutions are read on demand and cached, not copied into each
  receiving model.

## Decisions for you

1. **The file:** a separate `.qfem3d` kind (proposed), or one `.qfem` holding both 2D and 3D components?
2. **Where to start:** phase A first (links between 2D models, and saved solutions: the parked sidecar
   taken up), or the 3D document and view first (phase B)?
3. **The first 3D physics:** heat, electrostatics and currents (proposed), or another?
4. **A link's default:** live (proposed), or frozen?
5. **The 3D view:** GPU with a CPU fallback (proposed), or CPU only at first?
6. **General solids (stage 2):** later, through a library or an external program. Either way no AGPL
   code (TetGen).
