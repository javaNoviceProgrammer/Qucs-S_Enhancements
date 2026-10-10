# Proposal: a 2D multiphysics finite element solver, with a Multiphysics panel

*10 October 2026 - Qucs-S 26.1.7. A proposal; its phase 1 is built (see "Phase 1, as
built" at the end). It answers "I'd like to
build a 2D finite element solver in Qucs similar to what COMSOL Multiphysics has. Add a
new panel to the left called Multiphysics that has a tree and allows the user to build
the multiphysics project: geometry, material properties, FEM mesher, solver engine...".
The facts about this tree were checked in it, those about the libraries on their own
pages, on 10 October 2026. Sizes and days are estimates.*

**In short:** a COMSOL-like 2D tool is a large project, but a feasible one, if it is
2D (and 2D axisymmetric) only and keeps to the physics that matter next to a circuit
simulator:
- electrostatics, electric currents and heat, with their couplings;
- magnetic fields, and from them a transmission line's R, L, G and C;
- solid mechanics;
- waveguide modes, for RF and photonics.

I propose our own engine, small and built in, on four header-only libraries with
licences that fit Qucs-S's GPL:
- [Eigen](https://gitlab.com/libeigen/eigen) for the sparse linear algebra;
- [Spectra](https://github.com/yixuan/spectra) for the eigenproblems;
- [CDT](https://github.com/artem-ogre/CDT) for meshing;
- [Clipper2](https://github.com/AngusJohnson/Clipper2) for geometry.

The model is a document of its own (a `.qfem` text file) in a tab of its own. The
Multiphysics panel on the left shows its tree (geometry, materials, physics, mesh,
studies, results), with the selected node's settings under it.

It is built in five phases. The first, electrostatics, electric currents and heat on
a geometry of our own, is about 15,000 lines. What makes it more than a toy is how it
ties into the rest of Qucs-S:
- its results go to Qucs datasets and diagrams;
- an extracted line becomes a circuit part;
- a layout's cross-section becomes a model;
- Claude can build and run models through tools.

It is not COMSOL. COMSOL has 3D, about 30 modules and three decades of work. 3D is
left out on purpose: it needs a solid modelling kernel and a 3D mesher, a project of
its own.

## What a model is (COMSOL's way, kept)

COMSOL's *Model Builder* is a tree that is also the order of work: global parameters,
then a component's geometry (a sequence of features, rebuilt when a parameter
changes), materials on its domains, physics with their domain and boundary
conditions, multiphysics couplings, a mesh, studies (stationary, time dependent,
frequency, eigenvalue, parametric sweeps) and results (datasets, plots, derived
values, exports). Choosing a node shows its *Settings*; the *Graphics* window shows
the geometry, lets the user pick domains, boundaries and points, and shows the mesh
and the fields. That way of working is what users of COMSOL, ANSYS and Elmer expect,
and it is the right one to copy. The tree is the model.

## What there is

**In Qucs-S:**

| What | Where | Use here |
|---|---|---|
| The left dock: a tab strip on its west side (Projects, Content, Components, Libraries, File Browser) | `QucsApp::initView`, `qucs.cpp` (`TabView`) | *Multiphysics* is a sixth tab |
| A new kind of document in a tab of its own | as the layout viewer (`layoutdoc.cpp`, 4,600 lines and 1,500 of tests, built in a day on 6 October) and the XML editor (2,200 and 700) | the model's document and its *Graphics* view |
| A canvas that pans, zooms, measures and draws a grid over a large drawing, cached | `LayoutView` in `layoutdoc.cpp` | the *Graphics* view starts from it |
| Datasets and diagrams (Cartesian, polar, Smith, tables, markers) | `dataset.cpp`, the diagrams | 1D results and sweeps plotted as any simulation's |
| Analytic transmission lines: microstrip, coupled microstrip, coplanar, coax, stripline, rectangular waveguide | `qucs-transcalc/*.cpp` | benchmarks, and the extraction's companion |
| `RLCG`, a line given by R, L, C and G per metre | `components/rlcg.h` | where an extracted line goes |
| GDSII and OASIS read by gdstk | `layout.cpp`, `third_party/gdstk` | a layout's cross-section as a geometry |
| Claude's tools (`QucsControl`), Python in the window, git with a semantic diff | `qucscontrol*.cpp`, `python*.cpp`, `git*.cpp` | tools for models, a script API later, a model file that diffs well |
| CI on Linux (Debug, AddressSanitizer), macOS and Windows | `.github/workflows/ci.yml` | the solver's tests run on all three, under ASan |

There is no linear algebra library in the tree yet (no Eigen), and no expression
evaluator of the kind a solver needs: one that evaluates `sigma0/(1+alpha*(T-T0))`
millions of times.

**Outside it:**

| | What it is | Licence | Notes |
|---|---|---|---|
| [Eigen](https://gitlab.com/libeigen/eigen) | Dense and sparse linear algebra: `SimplicialLDLT`, `SparseLU`, `ConjugateGradient`, `BiCGSTAB` with incomplete factorisations, complex numbers | MPL 2.0 (combinable with the GPL) | Header-only. 5.0.1 (11 November 2025) and 3.4.1 (29 September 2025) are its latest releases. |
| [Spectra](https://github.com/yixuan/spectra) | Large sparse eigenproblems (implicitly restarted Arnoldi and Lanczos, shift and invert) on Eigen | MPL 2.0 | Header-only. For waveguide modes and eigenfrequencies. |
| [CDT](https://github.com/artem-ogre/CDT) | Constrained Delaunay triangulation: edges forced in, holes and regions found, Ruppert's quality refinement (smallest angle, largest area) | MPL 2.0 | Header-only or compiled. Its refinement is by one size; a size that varies over the model is ours to add. |
| [Clipper2](https://github.com/AngusJohnson/Clipper2) | Union, difference, intersection and XOR of polygons, offsets | Boost Software License 1.0 | C++17. Already named in the layout proposal for the editor. |
| [Gmsh](https://gmsh.info) | The mesher most open tools use: quads, boundary layers, size fields, CAD import | GPL 2 or later, with a linking exception | 4.15.2 (24 March 2026). Large. Best as an optional program, found if installed, as ngspice is. |
| [Triangle](https://www.cs.cmu.edu/~quake/triangle.html) | The classic 2D quality mesher | Its own: "may not be sold or included in commercial products without a license" | Not free in the GPL's sense. Not used. |
| [Elmer](https://github.com/ElmerCSC/elmerfem) | A whole open multiphysics suite (ElmerSolver, ElmerGUI, ElmerGrid), 2D and 3D | GPL; the ElmerSolver library LGPL | A possible external engine (way 2), as Xyce is to ngspice. |

## Three ways

1. **Drive an external engine** - Elmer, GetDP or FreeFEM - as Qucs-S drives ngspice:
   Qucs-S builds the model, writes the engine's input, runs it and reads its output
   back. The physics are there from the start. But each engine has its own model of
   the world, the user has to install it, the results come back as files, and every
   feature must be translated twice (Elmer's `.sif` alone has hundreds of keywords).
   The tight loop of select, solve and evaluate that makes COMSOL pleasant is hard to
   get through files.
2. **Embed a large finite element library** - MFEM, deal.II - and build the
   physics on it. Powerful, but these are 3D-first, heavy to build on three
   platforms (MPI, CMake trees of their own), and most of their power goes unused
   in 2D.
3. **Our own engine, for 2D**, on Eigen, Spectra, CDT and Clipper2. 2D finite
   elements on triangles are well understood and compact: Lagrange elements of
   order 1 and 2, Nédélec edge elements for waveguide modes, numerical integration,
   assembly, boundary conditions, Newton's method and time stepping. Each physics is
   a few hundred lines on top of that. It is ours to test under ASan on three
   platforms, to tie into the rest of Qucs-S, and to keep fast for models of a
   hundred thousand unknowns. Those are the size of almost every 2D model.

**I propose 3**, with the engine behind an interface, so that way 1 can be added
later for what we will not build (3D, semiconductors). Qucs-S already works like that
with its simulators.

## The design

### The Multiphysics panel

A sixth tab in the left dock, *Multiphysics*, beside *File Browser*. Its upper part
is the model's tree; its lower part, under a splitter, the settings of the node
chosen. When a model's tab is in front, the panel shows its tree; otherwise it offers
*New Model*, the recent models and the examples. Each node has a menu: add a
child, disable, rename, duplicate, delete, *Build* (geometry, mesh) or *Compute*
(study). A node that needs its parent built again shows a mark, as COMSOL's do.

```
◆ microstrip.qfem
├─ Global Definitions
│  ├─ Parameters            w = 3[mm], h = 1.6[mm], f0 = 1[GHz] ...
│  └─ Functions             a table of εr(f) from a file, an analytic σ(T)
├─ Component 1 (2D)          length unit: mm
│  ├─ Definitions           named selections: "trace", "ground", "air"
│  ├─ Geometry
│  │  ├─ Rectangle  substrate
│  │  ├─ Rectangle  trace
│  │  ├─ Rectangle  air box
│  │  └─ Form Union         ✓ built: 3 domains, 13 boundaries
│  ├─ Materials
│  │  ├─ FR-4               domains 1
│  │  ├─ Copper             domains 2
│  │  └─ Air                domains 3
│  ├─ Electrostatics (es)
│  │  ├─ Charge Conservation   all domains
│  │  ├─ Zero Charge           (the outer boundaries)
│  │  ├─ Ground                the ground plane
│  │  └─ Terminal 1            the trace, 1 V
│  ├─ Magnetic Fields (mf)
│  └─ Mesh                  fine; the trace's edges finer      ✓ 8,412 triangles
├─ Study 1                   Stationary, then Frequency Domain 1-10 GHz
│  └─ Parametric Sweep      w from 1 to 5 mm
└─ Results
   ├─ Datasets              Study 1, a cut line across the trace
   ├─ Electric Potential    a 2D plot: colour, contours, field arrows
   ├─ Derived Values        C = 128.6 pF/m, Z0 = 50.3 Ω, εeff = 3.31
   └─ Export                to RLCG (a part), a Touchstone file, a Qucs dataset
```

The settings below the tree are a form per node type: a selection list with *pick in
the Graphics view*, expressions with units (`10[um]`, or Qucs's `10u`), material
properties with their source (library, user), solver options. Each change is one
step of Edit > Undo.

### The model document and its Graphics view

The model is a document in a tab of its own, as a layout or a PDF is. Its view
starts from the layout viewer's canvas: the same panning, zooming, rulers and grid,
drawn into a cache. On it:

- **the geometry**, with its domains, boundaries and points picked in one of three
  modes, those of the node chosen highlighted;
- **the mesh**, with its quality shown in colour;
- **results**: a colour map with its legend, contours, arrows and streamlines, a
  deformed shape, the values under the pointer.

1D results go where Qucs-S's go: a sweep or a cut line becomes a Qucs dataset, and a
data display plots it with the diagrams, markers and tables there already. The
solver runs on a worker thread, with progress and *Cancel*, writing its log to the
simulation console.

### The model file

`name.qfem`: JSON, with the tree in it as written - parameters, the geometry's
features in order, materials, physics, the mesh's settings, studies and results'
definitions. Text, so git shows its changes and Claude can read and write it. The
mesh and the solutions are not in it: they go beside it in `name.qfem.results`
(binary, rebuilt by *Compute*, for `.gitignore`), as a schematic's dataset does.

```json
{
  "format": "qucs-s multiphysics 1",
  "parameters": [{"name": "w", "expression": "3[mm]", "description": "trace width"}],
  "components": [{
    "space": "2D", "unit": "mm",
    "geometry": [
      {"type": "rectangle", "name": "substrate", "corner": ["-10", "0"], "size": ["20", "h"]},
      {"type": "rectangle", "name": "trace", "corner": ["-w/2", "h"], "size": ["w", "0.035"]},
      {"type": "rectangle", "name": "air", "corner": ["-10", "0"], "size": ["20", "10"]},
      {"type": "form union"}
    ],
    "materials": [{"material": "FR-4", "domains": {"objects": ["substrate"]}}],
    "physics": [{"type": "electrostatics", "tag": "es", "features": [
      {"type": "terminal", "boundaries": {"of": ["trace"]}, "voltage": "1[V]"},
      {"type": "ground", "boundaries": {"bottom of": ["substrate"]}}]}],
    "mesh": {"size": "fine"}
  }],
  "studies": [{"type": "stationary", "physics": ["es"]}]
}
```

Selections are kept as **rules** where they can be: the domains of an object, the
boundaries between two objects, those in a box. They do not depend on the numbers
"Form Union" gives domains, which change when the geometry does: this is COMSOL's
most common complaint. Picking domains in the view writes numbers; *Make a Rule*
turns them into one.

### Geometry

- **Features:** rectangle, square, circle, ellipse, polygon, polyline, Bézier,
  point, interval; move, rotate, scale, mirror, array; union, difference,
  intersection; fillet and chamfer at corners. Each takes expressions of the
  parameters, so a sweep of `w` rebuilds the geometry.
- **Imports:** DXF and SVG; GDSII and OASIS layers through gdstk (a cell's shapes as
  they are, for top-view problems); and a **layout's cross-section** - a cut line
  across a cell and a layer stack (each layer's z and thickness) giving the 2D slice
  with its materials. That is where a photonics or MMIC layout meets its physics.
- **Form Union:** every object's edges, split where they cross, inserted into CDT as
  constrained edges. The regions they close are the domains, each knowing which
  objects contain it. Clipper2 does the boolean features before that.
- Curves are kept as curves (segments, arcs, Béziers), sampled finely for display and
  as the mesh needs. Quadratic elements put their middle nodes on the true curve.

### Materials

A library of JSON entries: Qucs-S's own, the user's (`user_lib`), and the project's.
Each holds properties with units, and each property may be an expression of `T`,
`f` or the parameters, or an interpolation table:

- relative permittivity and loss tangent;
- conductivity and its temperature coefficient;
- relative permeability, or a B-H curve;
- thermal conductivity, density and heat capacity;
- Young's modulus, Poisson's ratio and thermal expansion;
- refractive index, complex and anisotropic, and its thermo-optic coefficient.

The library starts with the usual:
- **metals:** copper, aluminium, gold, silver, solder;
- **dielectrics:** FR-4, Rogers RO4003C and RO4350B, alumina, PTFE;
- **semiconductors and their insulators:** silicon, SiO₂, Si₃N₄, InP, GaAs, LiNbO₃;
- air and water.

A property that a physics needs and its material lacks is said before solving, not
found as a NaN after.

### Physics, and how they couple

| Physics | Unknown | Elements | Studies | For |
|---|---|---|---|---|
| Electrostatics | potential V | Lagrange 1 or 2 | stationary | capacitance matrices, fields, breakdown margins |
| Electric currents | V (σ, or σ + jωε) | Lagrange | stationary, frequency | resistance, current density, a resistor's or a via's heat |
| Heat transfer | T | Lagrange | stationary, time dependent | thermal resistance, hot spots, heaters, transients |
| Magnetic fields (2D, current out of the plane) | Az | Lagrange | stationary (B-H curves: Newton), frequency (eddy currents) | inductance, skin and proximity effect, R(f) and L(f) |
| Solid mechanics (plane stress, plane strain) | displacement u, v | Lagrange 2 | stationary, eigenfrequency | stress, warping, resonances |
| Waves in the plane (TE, TM) | Ez or Hz | Lagrange 2, PML | frequency | scattering, 2D filters, ports and S-parameters |
| Mode analysis of a cross-section | E (transverse on edges, z on nodes) | Nédélec + Lagrange | eigenvalue | a waveguide's or a line's effective index, loss, mode shapes |

Every physics except the waves also comes in **2D axisymmetric** form: coax,
cylinders, round wires, bond wires.

**Couplings:**
- *Electromagnetic heating:* Joule losses Q = σ|E|² from electric currents or eddy
  currents feed heat transfer.
- *Temperature:* σ(T), k(T) and n(T) read the temperature, so a heater and its
  phase shift go round until they agree.
- *Thermal expansion:* α(T − T₀) as an initial strain in solid mechanics.

A coupled study solves its physics one after another until they agree (segregated),
or all at once by Newton's method (fully coupled), as asked.

### Mesh

CDT's triangulation, refined by Ruppert's algorithm, with our own **size field**:
- a global size (from "extremely fine" to "extremely coarse", or a length);
- a size per domain, per boundary or per point;
- edges divided into a given number of elements;
- a growth rate between them;
- the curvature of a boundary, and a narrow region that needs a few elements across.

Elements of order 1 or 2, those on curves curved. Its statistics (count, worst
angle, quality histogram) are shown. Gmsh, when installed, is a second mesher for
what CDT will not do: quadrilaterals, and boundary layers for thin films and skin
depths.

### The solver engine

- **Spaces:** Lagrange P1, P2 (P3 later); Nédélec of the first kind, orders 1 and 2.
- **Integration:** Dunavant's rules on triangles, Gauss on edges.
- **Assembly:** in parallel, element by element, into Eigen's compressed matrices. A
  boundary condition is imposed by elimination, a periodic one by pairing nodes.
- **Linear solvers:**
  - `SimplicialLDLT` for the symmetric ones (electrostatics, heat);
  - `SparseLU` for the others, complex too;
  - for large models, conjugate gradients and BiCGSTAB with incomplete
    factorisations.
  2D models stay small enough for direct solvers: some 10⁵ to 10⁶ unknowns.
- **Nonlinear and time:** Newton with damping (B-H curves, σ(T), k(T)); backward
  Euler and BDF2 with an adaptive step.
- **Eigenproblems:** Spectra, shift and invert about a guess (an effective index, a
  frequency).
- **Expressions:** our own small compiler from an expression to a sequence of
  operations (a few hundred lines, no library), evaluated at each integration point.
  The variables are x, y, r, z, the parameters, the fields and their derivatives
  (`es.normE`, `ht.T`), functions and tables. Units are checked as they are
  compiled.
- **Its interface:** the model goes in; meshes, solutions and messages come out. An
  external engine (Elmer, GetDP) could stand behind the same interface later.

### Studies

Each study is a list of steps, as COMSOL's:
- **steps:** stationary; time dependent (output times, a tolerance); frequency domain
  (a list or a sweep); eigenvalue (how many, near what); parametric sweep
  (parameters × values, all combinations or in step);
- **for each step:** which physics, which solver, what to keep (all solutions, or
  only some).

A sweep runs its points in parallel when memory allows.

### Results

- **Datasets:** the solutions, cut lines, cut points, a parametric dataset.
- **Plots:**
  - in the *Graphics* view: a colour map of any expression, contours, arrows,
    streamlines, deformation, the mesh;
  - in a Qucs data display: lines along a cut line, and values against a sweep's
    parameter.
- **Derived values:** a point's value; a line's or a domain's integral, average,
  maximum and minimum. Global ones per physics:
  - a capacitance matrix (from the terminals' charges);
  - resistance and conductance;
  - inductance (from energy or flux);
  - thermal resistance;
  - for a line: Z₀, εeff, attenuation and R, L, G, C per metre;
  - for modes: effective index, loss and confinement.
- **Exports:**
  - images (PNG, SVG, PDF);
  - data (CSV, and VTK for ParaView);
  - a Qucs dataset;
  - a **part**: an `RLCG` line, a Touchstone file, a subcircuit of capacitors or a
    thermal RC network.

### How it ties into the rest of Qucs-S

- **Circuits:**
  - a transmission line, a package's parasitics or a heater's thermal network,
    extracted, becomes a part in a schematic;
  - a study's sweep can be driven by the parameters a schematic sweeps (later).
- **Layouts:** a GDS/OASIS cell's cross-section becomes a model. One click from the
  layout viewer (*Cross-Section to Multiphysics*) with a layer stack, e.g. an SOI
  waveguide or a PN phase shifter's heater.
- **Datasets and diagrams:** sweeps and cut lines are plotted as simulations' are,
  with markers, tables and exports.
- **Claude:** tools that build and run a model as the panel does:
  - `fem_model` (new, open, describe the tree);
  - `fem_geometry`, `fem_material` and `fem_physics` (add, change, delete nodes);
  - `fem_mesh` and `fem_solve`;
  - `fem_evaluate` (expressions, derived values);
  - `fem_plot` (a plot shown, for a screenshot).

  With the format described by `describe_format`, Claude can also write a `.qfem` file
  whole. Every tool says what is missing before it solves.
- **The File Browser and the menus:** `.qfem` with its badge and kind; *New >
  Multiphysics Model*; a *Multiphysics* menu (Build Geometry, Build Mesh, Compute,
  Clear Solutions); examples under `examples/multiphysics`.
- **Git:** the model file is text, so git shows its changes; a semantic diff
  (parameters, features, physics changed) as the schematics' later.
- **Python:** a `qucs.fem` module in the Python console to script models (later).

## Validation

A solver that is not checked against known answers is not worth shipping. Each physics
comes with **benchmarks with closed-form answers**:
- each runs as a test on CI, under ASan, on a coarse mesh;
- each checks that its error falls at the rate its element order promises (P1, P2) as
  the mesh is refined.

| Physics | Benchmark | Known from |
|---|---|---|
| Electrostatics | coax capacitance, 2πε / ln(b/a); parallel plates with their fringe | analytic; Palmer's formula |
| Electrostatics | microstrip and coplanar Z₀ and εeff over w/h and εr | Qucs-S's own `qucs-transcalc` (Hammerstad-Jensen, Wheeler) |
| Electric currents | a bar's ρL/A; a constriction's resistance | analytic |
| Heat | a slab, a cylinder with convection; a lumped transient | analytic |
| Magnetic fields | a wire's B = μ₀I / 2πr; a coax's inductance; a round wire's skin effect (Bessel functions) | analytic |
| Solid mechanics | a cantilever's tip deflection; Lamé's thick cylinder (axisymmetric) | beam theory; analytic |
| Mode analysis | a slab waveguide's TE and TM modes; a rectangular waveguide's cut-offs; an SOI wire's effective index | analytic; `qucs-transcalc`'s waveguide; the literature |
| Couplings | a resistor's Joule heating and its temperature | analytic |

## Phases

The sizes are compared with what is built: the layout viewer (4,600 lines and 1,500 of
tests) and git (6,500 and 3,100). Each phase is useful when done, is committed and
packaged as any other, and is tested on CI.

| Phase | What | Lines (with tests) | Days, roughly |
|---|---|---|---|
| **1. Foundation: electrostatics, currents, heat** | the `.qfem` document, the panel's tree and settings, undo; geometry primitives, booleans, Form Union, rule selections; CDT mesh with a size field; P1 and P2 elements, assembly, direct solvers; expressions and units; materials library; electrostatics, electric currents and stationary heat; colour maps, contours, arrows, derived values (capacitance, resistance, thermal resistance); Claude's tools; the benchmarks | 15,000 + 4,000 | 6 - 10 |
| **2. Time, couplings, mechanics, sweeps** | time-dependent heat; Joule heating, temperature-dependent materials, thermal expansion; solid mechanics (plane stress, strain, axisymmetric); 2D axisymmetric for all; parametric sweeps; cut lines and 1D results to Qucs datasets; DXF and SVG import | 9,000 + 2,500 | 4 - 6 |
| **3. Magnetics and lines into circuits** | magnetostatics with B-H curves (Newton); eddy currents (frequency domain); R, L, G, C per metre over frequency; export to `RLCG`, Touchstone and subcircuits; GDS/OASIS import and the layout cross-section | 8,000 + 2,000 | 4 - 6 |
| **4. Waves** | the mode solver (Nédélec elements, PML, complex and anisotropic media, thermo-optic coupling); TE and TM frequency domain with ports, scattering boundaries and S-parameters; adaptive mesh refinement from error estimates | 9,000 + 2,500 | 5 - 8 |
| **5. Outward** (as wanted) | Gmsh as a second mesher; Elmer or GetDP as external engines; the Python module; a semantic diff of models; schematic-driven sweeps | 3,000 - 6,000 | as chosen |

Phase 1 alone is the largest single feature yet: about three layout viewers. The
mathematics are where the risk is, not the windows. That is why each physics comes
with its benchmarks the day it comes.

## Risks and limits

- **Thin layers and multiscale models** (35 µm of copper on 1.6 mm of FR-4; 220 nm
  of silicon in micrometres of oxide; a skin depth at 10 GHz): triangles cope with
  graded sizes, at the cost of more elements. Boundary layers and anisotropic
  meshes need Gmsh (phase 5) or more mesher work.
- **Geometry robustness:** booleans of curves, near-tangent edges and tiny slivers
  are where 2D kernels break. Clipper2 works in integers, so the coordinates are
  scaled. The tests take on such cases from the start.
- **Size:** direct solvers are right for 2D up to about a million unknowns. Beyond
  that, iterative solvers need good preconditioners (multigrid is not in Eigen).
- **What it is not:** no 3D; no semiconductors (drift-diffusion is a project of its
  own, and an external engine's job); no CFD; no ray optics.
- **CI time:** the Linux job runs Debug with AddressSanitizer, 10-30× slower than
  Release. FEM tests keep their meshes small and check convergence rates, not fine
  answers.

## What I would like you to decide

1. **The engine:** our own (way 3, proposed), or Elmer/GetDP behind Qucs-S (way 1).
2. **What comes first after the foundation:** the order above puts heat and
   mechanics (phase 2) before RF extraction (3) and photonics modes (4). If the
   photonics work (the SOI layouts, the PN phase shifter) matters most, the mode
   solver can move up to phase 2, and mechanics down.
3. **The panel's name and the file:** *Multiphysics* (the request had
   "Mutliphysics", taken as a slip) and `.qfem`.
4. **Where settings go:** under the tree in the panel (proposed), or in a dock of
   their own on the right as COMSOL's *Settings* window.

## Phase 1, as built

Decided on 10 October 2026: our own engine ("if it is fast and efficient"), the five
phases in the order above, *Multiphysics* and `.qfem`, the settings under the tree.
Phase 1 is in `qucs/multiphysics/` (the engine, Qt Core only: `fem_expr`, `fem_model`,
`fem_geometry`, `fem_mesh`, `fem_fe`, `fem_solver`, `fem_results`, `fem_materials`),
`multiphysicsview`, `multiphysicsdoc`, `multiphysicspanel` and `qucs_multiphysics` (the
window), and `qucscontrol_fem` (Claude's tools).

**Fast enough:** a coax of 108,000 quadratic triangles, 217,000 unknowns, is meshed in
0.45 s and solved in 0.9 s (M-series Mac, Release; `test_fem_engine`'s
`largeModelTiming`).

**Where it differs from the plan above:**
- **Libraries:** Eigen 5.0.1 and CDT 2.0.0 are in `third_party/` (each with a
  `README.qucs.md`). Clipper2 is not: the booleans use the Clipper 6.4.2 inside gdstk.
  Spectra comes with the mode solver (phase 4).
- **Meshing:** CDT triangulates the boundaries. The refinement is ours: circumcenters
  inserted where a triangle is larger than the size field asks, then CDT's Ruppert
  refinement for angles of 25° at least, then smoothing. The size field is a k-d tree of
  sizes on the boundaries (preset, per entity, curvature, narrow gaps) grown at the
  growth rate.
- **Units in the geometry:** a geometry's and a mesh's expressions are evaluated with
  lengths in the component's unit, as COMSOL does: `L/2 - 0.6` with `L = 2[mm]` is
  0.4 mm.
- **Selections:** "only" (a domain's smallest object) and "between" use the smallest
  object that holds a domain, whatever the order in which the objects were drawn.
- **Couplings** came early. A material or a source may read another physics' field
  (`ec.Qrh` as a heat source, a σ(T)), and such physics are solved in turn until they
  agree. Phase 2 adds Newton's method and time.
- **Claude's tools** are `fem_describe`, `fem_model`, `fem_edit`, `fem_build`,
  `fem_solve`, `fem_evaluate` and `fem_plot`. A general `fem_edit` stands in for the
  separate tools for geometry, materials and physics.
- **Solutions are not saved** beside the model yet (`name.qfem.results`). *Compute*
  makes them again, in seconds.
- **Line parameters:** quasi-TEM C, C0, εeff, Z0, L and v for a terminal come from
  the Line Parameters derived value. It solves the line again with every εr at 1. The
  microstrip example gives Z0 = 50.28 Ω and εeff = 3.318, against qucs-transcalc's
  50.26 Ω and 3.289.

**Checked:** `test_fem_engine` (22 tests, the engine alone) and `test_multiphysics_doc`
(13, the window and Claude's tools) pass in Release and under AddressSanitizer and
UBSan.

