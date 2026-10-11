# Proposal: a 2D multiphysics finite element solver, with a Multiphysics panel

*10 October 2026 - Qucs-S 26.1.7. A proposal; its phases 1 to 4 are built (see "Phase 1,
as built" to "Phase 4, as built" at the end). It answers "I'd like to
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

## Phase 2, as built

Built on 10 October 2026, Qucs-S 26.1.8, as phase 2 above says: time-dependent heat;
Joule heating, temperature-dependent materials, thermal expansion; solid mechanics in
plane stress, plane strain and about an axis; 2D axisymmetric for every physics;
parametric sweeps; cut lines and 1D results as Qucs datasets; DXF and SVG import.

The solver is now in four files: `fem_solver` (the solution, its variables and
snapshots, the sparse systems, the studies), `fem_scalar` (electrostatics, currents,
heat), `fem_elastic` (solid mechanics) and `fem_import` (the drawings). Each physics is
solved for an increment of its field from the residual F - K u (+ the mass terms): once
when it is linear, again and again by Newton's method when it reads its own field.

**How it is done:**
- **Time:** heat's ρ Cp ∂T/∂t by BDF2 with a variable step (Euler for its first two
  steps), each step's error estimated against a predictor of its own order - Milne's
  device with variable-step BDF2's error constant (1+ω)²/(6ω(1+2ω)) - and held under
  the relative tolerance of how much the field has changed. The steps land on the
  output times. The physics without a time derivative are solved at rest at each step,
  in turn with heat until they agree. Fixed steps and Euler are there to choose.
- **Newton:** a coefficient's or source's derivative in the field's value, taken
  numerically at each integration point (k(T), Q(T), ρCp(T), a film's h(T)): the
  Jacobian is not symmetric, so LU. Radiation is εσ(Tamb² + T²)(Tamb + T)(Tamb - T), a
  film of T itself.
- **Mechanics:** σ = D(ε - ε0), ε0 the free thermal strain α(T - Tref) - (1+ν)αΔT in
  the plane for plane strain. About the axis the hoop strain u/r, and u = 0 on the
  axis. A Roller on a boundary along x or y fixes that component; on any other, a
  stiff spring along its normal.
- **About an axis:** every integral weighs 2πr in place of the thickness; terminals'
  charges and currents, powers, energies are of the whole body.
- **Sweeps:** each point's parameters evaluated anew; the geometry and mesh made again
  only when a parameter that changed is named in the geometry, the mesh or a function.
- **Results:** a solution keeps its fields and globals at each output time. A plot
  group shows one time and parameter value; a 1D plot group writes a Qucs dataset
  (time, the swept parameter, a cut line's x or arc length as its independents) and
  opens a data display through the same path as the Python editor's `qucs.display()`.
- **Drawings:** a Bézier or spline is a curve of its own kind, a polyline whose
  parameter counts its points (so an affine map keeps it), not many curves: one
  boundary.

**Where it differs from the plan above:**
- **Newton is within each physics; physics in turn between them** (segregated), not
  one Newton over all of them. The derivative is in the field's value, not its
  gradient: a coefficient of |E| converges as Picard's iterations do.
- **Only heat changes in time.** Electric currents' and solid mechanics' time
  derivatives (charge relaxation, inertia) are not there: they are solved at rest at
  each step.
- **Eigenfrequencies of solids** come with the mode solver and Spectra (phase 4).
- **Point loads and springs** of solids are not there.
- **A sweep's points are solved one after another**, not in parallel.
- **Solutions are still not saved** beside the model.

**Checked:** `test_fem_physics` against closed forms:
- a slab's mode decaying in time within 5 mK of 10 K;
- BDF2's error 4.1 times smaller with the step halved, Euler's 1.96;
- k(T) against Kirchhoff's transform, and Newton's solution equal to Picard's to 10⁻⁸ K;
- radiation against the balance it solves;
- a cantilever within 0.21 % of Timoshenko, its root stress within 2 % of beam theory;
- Lamé's thick cylinder about its axis to 10⁻⁴;
- thermal expansion free (no stress) and held (σ = -EαΔT);
- a spherical capacitor about its axis to 1.1 × 10⁻⁵;
- sweeps that mesh again or not;
- cut lines; DXF and SVG drawings;
- the new examples: a heater in time, a bimetal strip (0.584 mm against Timoshenko's
  0.582 mm), a through-silicon via (124 fF; its liner alone 114 fF), Z0 against a
  microstrip's width, a heat sink drawn in SVG.

`test_multiphysics_doc` (20) adds the window's: times and sweep points shown, 1D plot
groups as Qucs data displays, a solid drawn deformed, an axisymmetric model, a drawing
read again when it changes, and Claude's tools at times and sweep points.

## Phase 3, as built

Built on 10 October 2026, Qucs-S 26.1.8, as phase 3 above says: magnetostatics with B-H
curves (Newton); eddy currents in the frequency domain; R, L, G and C per metre over
frequency; export to Touchstone, SPICE and Qucs subcircuits; GDSII and OASIS import and a
layout's cross-section. The magnetic fields are in `fem_magnetic`, the exports in
`fem_export`; the layouts are read in `fem_import` and cut in `fem_geometry`.

**How it is done:**
- **Magnetic fields:** the vector potential out of the plane, ∇×(ν ∇×A) = J, B = (∂A/∂y,
  -∂A/∂x); about the axis Aφ, B = (-∂A/∂z, ∂A/∂r + A/r), and A = 0 on the axis. Magnetic
  Insulation (A = 0) holds on the physics' outer boundaries only, not between its domains.
  A magnet adds ∫ ν Br·∇×N to the right-hand side.
- **B-H curves:** H(B) a monotone cubic through the curve's points (PCHIP's slopes), as in
  vacuum beyond the last, its energy ∫ H dB exactly. Newton's tangent is ν I + (H' - ν)/|B|²
  B Bᵀ: symmetric, and positive definite while H rises with B, so a sparse Cholesky solves
  it. A step that makes the residual larger is halved. A ring round a wire saturating
  converges in nine steps from nothing.
- **The frequency domain:** K + jωM is complex symmetric with K positive definite. Its real
  equivalent [K, -ωM; -ωM, -K] is symmetric and quasi-definite, so an LDLᵀ of it exists in
  any order. A sparse LDLᵀ of it is faster than Eigen's complex LU, which stays as the fall-back.
- **Conductors:** a single conductor's current flows as σ (g U - jωA), where g is 1 in the
  plane and 1/2πr round the axis, and U is its voltage per metre (round the axis, its loop
  voltage). Each U is eliminated: one solve per conductor and a small system for the U's.
  A dense row for each U would have filled the factors (a coax took 9 s; it takes 1.3 s).
  Turns of wire carry N I/A evenly, with no eddy currents in them. Each coil gives its
  voltage, flux linkage, resistance and inductance; the coils, each at 1 A in turn, give the
  inductance matrix (at rest) or the impedance matrix (in frequency).
- **A line's R, L, G, C:**
  - C from electrostatics' terminal;
  - G = (ω ∫ ε0 εr tan δ |E|² + ∫ σ |E|²) / V0², the dielectrics' losses at the field as
    solved;
  - R and L from the signal coil's impedance less the return's (Z_ss - Z_sr - Z_rs + Z_rr;
    with no return coil, a perfect ground at A = 0);
  - from these, γ, Zc, α in dB/m, β and εeff.
- **Exports:**
  - Touchstone 1.1, S from the line's ABCD matrix at each frequency;
  - SPICE: a line as sections of R, L, G and C, or ngspice's and Xyce's LTRA;
    capacitance and conductance matrices as capacitors and resistors between the
    terminals and to ref; an inductance matrix as inductors coupled by K, with their
    resistances;
  - a Qucs subcircuit: an RLCG part between ports 1 and 2 under a box symbol, its dataset
    named after its file so Qucs opens it without asking, or the part alone on the
    clipboard to paste.
- **Layouts:**
  - gdstk reads the cell and flattens it: placements, arrays, and paths as their outlines.
    Clipper makes each layer's polygons one, in 0.1 nm integers.
  - Seen from above, each layer is an object, `name_layer_datatype`.
  - A cut along a line becomes, for each row of a layer stack, a rectangle for each
    stretch where its layer lies under the cut, from its z to z + its thickness. A row
    without a layer runs all along the cut.
  - The layout viewer offers this on its last ruler, the layers under the cut as the
    stack to begin with.
  - gdstk's one log is guarded by one lock, shared with the viewer's reads.
- **Studies:** a Frequency Domain step, its frequencies as `logspace(1[kHz], 1[GHz], 31)`
  (evenly in their logarithm), range, linspace or values. The physics with no frequency
  domain are at rest at each frequency, in turn with the magnetic fields when they read each
  other. Each frequency is kept as a snapshot, and a plot group's Frequency picks one. A 1D
  plot group's dataset runs over "frequency". A step at rest after one in time or frequency
  updates its last snapshot, so heat may take a frequency's losses: induction heating.

**Where it differs from the plan above:**
- **B-H curves are at rest only.** In the frequency domain a relative permeability is
  asked for; an effective curve for harmonic fields is not there.
- **Coils are fed currents,** not voltages, and are not tied to a circuit. Forces and
  torques (Maxwell's stress) are not there.
- **A SPICE line has one frequency's R, L, G and C** (a ladder or LTRA): it does not
  follow R(f) and L(f). The Touchstone file does; W-element tables are not written.
- **A cross-section is rectangles:** sidewalls are vertical. Sloped etches and
  conformal layers are not there.
- **A study runs in time or in frequency, not both.**
- **Skin depths are meshed by a size on the conductors' boundaries.** Boundary layers
  (Gmsh, phase 5) are not there.

**Fast enough:** the coax example, 81,699 unknowns, solved at nine frequencies with both of
its coils swept at each, takes 1.8 s (M-series Mac, Release).

**Checked:**
- `test_fem_magnetics` (12):
  - a B-H curve itself;
  - a round wire's field inside and out, its inductance μ0/2π (ln(R/a) + 1/4) and
    resistance to 10⁻³;
  - a steel ring round a wire on its B-H curve (H = I/2πr), within 0.01 %, in nine
    Newton steps from nothing;
  - a magnet's Br/2 (1 - a²/R²);
  - a thick solenoid's middle about its axis (2 × 10⁻⁵);
  - a wire three skin depths thick against its Bessel-function impedance (R and L to
    10⁻⁴);
  - a two-wire line's inductance matrix, symmetric, its loop against the images' formula
    (2 × 10⁻⁵);
  - a coax's R and L against Bessel's, C, and G = ω C tan δ at two frequencies (each to
    10⁻⁴);
  - Touchstone, SPICE and Qucs exports, and SPICE's coupled inductors;
  - the examples.
- `test_fem_layout` (7): a GDSII cell, written record by record, read flattened (its
  placements, a layer's overlapping boxes as one, gzipped too, layers and windows chosen);
  an OASIS one as gdstk writes it; cut intervals; a layout from above; a cross-section,
  its areas, and heat through it.
- `test_multiphysics_doc` (23) adds the window's:
  - a coax shown at its frequencies, chosen in the box and the plot group's settings;
  - R against frequency as a Qucs dataset;
  - a Touchstone file, a Qucs subcircuit that Qucs opens, the part on the clipboard;
  - a layout's cross-section made in the viewer;
  - Claude's tools in frequency, and `fem_export`.

**Examples:**
- `coax_rlgc`: a coax's R, L, G and C from 1 kHz to 10 MHz. R rises from its DC value to
  the skin effect's; Z0 is 52 Ω.
- `gapped_core_inductor`: an inductor on a gapped steel core, its current swept. It gives
  4.7 mH falling to 2.3 mH as the steel saturates.
- `induction_heating`: a stainless billet heated by eddy currents about its axis: 32 W,
  125 °C.
- `heater_crosssection`: a thermo-optic phase shifter cut from `thermo_optic_heater.gds`.
  The waveguide warms by 3.2 K, giving π at 26 mW.

## Phase 4, as built

Built on 10 October 2026, Qucs-S 26.1.8, as phase 4 above says: the mode solver (Nédélec
elements, PML, complex and anisotropic media, thermo-optic coupling); waves in the plane
with ports, scattering boundaries and S-parameters; adaptive mesh refinement from error
estimates. The edge elements and the eigensolver are in `fem_edge`, the two physics in
`fem_waves`; the refinement is in `fem_solver`. It came to about 2,500 lines and 800 of
tests, much less than the 9,000 planned: the mesh, the expressions, the studies, the
plots and the panel did not have to change much.

**How it is done:**
- **Edge elements:** Nédélec's of the first kind on the triangles. Order 1 is Whitney's,
  one on each edge. Order 2 is hierarchical, as Webb's: two on each edge (Whitney's and
  ∇(λa λb)) and two inside (λ2 W01, λ0 W12). Each edge runs from its lower node to its
  higher, so a function's sign is the same in both of its triangles.
- **Modes:** E = (Et + ẑ Ez) e^-jβz, with Et on the edge elements and Ez on Lagrange's of
  the same order. Lee's substitution (et = β Et, ez = -j Ez) makes it a linear eigenproblem
  A x = -β² B x:
  - A = [Stt - k0² Ttt, 0; 0, 0];
  - B = [Ttt, Gtz; Gzt, Szz - k0² Tzz];
  - with a diagonal µ, the x parts take 1/µyy and the y parts 1/µxx.

  The modes nearest a guessed index come from a shift and invert about σ = -(k0 n)²: a
  sparse LU of A - σB, then Krylov-Schur (Stewart's) on (A - σB)⁻¹B. The Arnoldi basis is
  orthogonalized twice, and the Schur form is sorted by Givens rotations to restart.
  - A breakdown is measured against the operator's own size. At 1550 nm it gives about
    10⁻¹³.
  - The null space (Ez alone, λ = 0) is no mode.
  - Each mode is turned so its largest transverse value is real. It is normalized to 1 W
    of Sz over the domains not in a PML.
- **Media:** ε from n - jκ (its square) or εr (1 - j tan δ) - jσ/ωε0, each along x, y and z
  when anisotropic, and µr. Warmed, n + dn/dT (T - Tref): T is an expression, by default a
  heat physics' T. At each frequency the step solves the physics with no modes first.
- **Perfectly matched layers:** the coordinates are stretched by s = 1 - jσ0 ξ², where ξ is
  the depth into the layer. σ0 = 20.7/(k d) makes the reflection e^-13.8 at normal
  incidence. ε and µ are multiplied by diag(sy/sx, sx/sy, sx sy). A layer stretches along x
  where it lies left or right of the other domains, and along y above or below, so a frame
  round them stretches in both at its corners.
  - The layers have modes of their own, crowding round the guess. Those with more than
    half their |E|² in the layers are left out.
  - The solver asks for 2n + 4 modes, and twice as many while fewer than n are left (an
    SOI wire about 2.6: 30 were left out; about 2.2: 6).
- **Waves in the plane:** -∇·(P ∇u) - k0² q u = 0:
  - Ez: P = diag(1/µyy, 1/µxx), q = εzz;
  - Hz: P = diag(1/εyy, 1/εxx), q = µzz.

  Perfect electric conductors fix Ez, or leave Hz natural, and the reverse for magnetic
  ones. The outside is a conductor by default. A scattering boundary is first order (jk p
  u), with a plane wave coming in through it if asked.
- **Ports:**
  - A port's first mode comes from its own line: -(p e')' - k0² q e = -β² p e on the
    degrees of freedom along it. It is normalized as ∫ p e² = 1, its sign so that ∫ p e >
    0 (without that, S21's sign followed the mesh).
  - The port's condition is the mode's: a rank-one jβ c cᵀ (c = ∫ p e N_i), and 2jβ c to
    feed it.
  - Each port fed in turn gives a column: b_i = cᵀu less the incident wave at the fed
    port, and S_ij = b_i √(β_i/β_j).
  - The S-matrix is written as globals (S21, S21_im, S21dB, S21_abs, S21_arg) and as a
    Touchstone file of N ports.
- **Adaptive mesh refinement:**
  - Zienkiewicz and Zhu's indicator: the gradient is recovered at each node, the
    triangles' gradients there averaged by area, separately in each domain. Each triangle's
    error is that gradient's difference from its own in L².
  - Marking is Dörfler's: the largest triangles that hold half of the squared error.
  - Each marked triangle adds a point to the size field, at its centroid with half its
    size, and the mesh is made again from the geometry. Nothing is coarsened.
  - A stationary step refines by its physics' error, a frequency step by the last
    frequency's (real and imaginary parts). All the study is solved again each round.

**Where it differs from the plan above:**
- **A port carries its first mode only.** Higher modes are taken as matched, so a
  discontinuity's evanescent modes must have decayed before a port.
- **The scattering boundary is first order.** It reflects at grazing incidence; a PML
  does not.
- **A PML absorbs waves that propagate into it, not evanescent tails.** The complex
  stretching turns an evanescent field's phase, not its size, so a mode's β moves by
  about its tail's size there, either way. An SOI wire's TM0 with 1.4 µm of oxide to the
  layers is lossy or gaining by 0.5 dB/m at most; with 1 µm, 13 dB/m.
- **Anisotropy is diagonal,** along x, y and z. Off-diagonal and gyrotropic media, bent
  guides (a conformal map), overlaps of modes and the group index are not there.
- **Adaptive refinement is for Lagrange fields:** not for the modes' edge elements, and
  not in time.
- **The mode solver's cost is its complex LU.** Eigen's COLAMD ordering fills more than a
  nested dissection would. AMD was tried and was slower, 50 times on WR-90. A wire's
  three wavelengths take 7 s.
- **Waves in time are not there.**

**Fast enough** (M-series Mac, Release):
- WR-90's five modes: 44 ms.
- The iris filter, 14,680 unknowns at 41 frequencies: 2.5 s.
- The SOI wire's two modes at three wavelengths, inside PMLs: 7 s.
- The thermo-optic phase shifter, its heat and mode at four powers: 2.1 s.

**Checked:**
- `test_fem_waves` (14):
  - the eigensolver against dense solvers, Hermitian and complex non-Hermitian (10⁻⁸);
  - WR-90's five modes at 20 GHz (2 × 10⁻⁴; TE11 and TM11 at one cutoff), TE10's field
    along y, real, carrying 1 W at its wave impedance, and linear elements coarser;
  - a lossy filling's complex β (its α to 2 × 10⁻³);
  - a dielectric slab's TE0 against its transcendental equation (2.847780 against
    2.847782);
  - a coax's TEM mode (n = 1.45 to 10⁻⁴);
  - a warmed slab: its index raised by dn/dT ΔT, the same as one given;
  - a parallel-plate guide's β (10⁻⁴), its S21 against e^-jβL (2 × 10⁻³) with |S11|
    10⁻⁶, and Hz's TEM mode;
  - an iris: |S11|² + |S21|² = 1 (10⁻³), S12 = S21;
  - a PML ending a guide (|S11| 1.1 × 10⁻⁴);
  - adaptive refinement at rest (an L-shaped plate's energy within 0.07 % of a mesh of
    104,215 triangles, on 1,168, its smallest at the corner) and in frequency (an iris's
    S21 63 times nearer a fine mesh's, on 3 % of its triangles);
  - the examples.
- `test_multiphysics_doc` (26) adds the window's:
  - a guide's modes at two frequencies in the box, the plot group's mode and frequency;
  - the default plots (the field, the power flow, the modes' indices);
  - the indices as a Qucs dataset over mode and frequency;
  - the iris filter's S-parameters in a data display, and its Touchstone file;
  - Claude's tools for modes.

**Examples:**
- `wr90_modes`: WR-90 at 20 GHz, five modes, each index to 10⁻⁴.
- `soi_waveguide_modes`: a 500 × 220 nm silicon wire in oxide inside PMLs at 1500, 1550 and
  1600 nm. TE0 is 2.445 and TM0 1.770 at 1550 nm.
- `thermo_optic_modes`: a heater above an SOI wire; heat, then the wire's mode as warmed,
  at 0 to 30 mW: π at 24 mW over a millimetre (the cross-section of phase 3 said 26).
- `iris_filter`: two inductive irises in WR-90, 8 to 12 GHz. It passes at 10.1 GHz (|S21|
  0.996) and is a Touchstone file of its two ports.
- `adaptive_corner`: an electrode's re-entrant corner. Four refinements take it from 174
  to 1,168 triangles, within 0.07 % of a mesh ninety times as fine.
