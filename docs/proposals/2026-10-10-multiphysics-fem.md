# Proposal: a 2D multiphysics finite element solver, with a Multiphysics panel

*10 October 2026 - Qucs-S 26.1.7. A proposal; its phases 1 to 4 are built, and semiconductors
and ray optics after them (see "Phase 1, as built" to "Semiconductors and ray optics, as
built" at the end). It answers "I'd like to
build a 2D finite element solver in Qucs similar to the commercial multiphysics tools. Add a
new panel to the left called Multiphysics that has a tree and allows the user to build
the multiphysics project: geometry, material properties, FEM mesher, solver engine...".
The facts about this tree were checked in it, those about the libraries on their own
pages, on 10 October 2026. Sizes and days are estimates.*

**In short:** a 2D multiphysics tool is a large project, but a feasible one, if it is
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

It is not a commercial multiphysics suite. Those have 3D, dozens of modules and decades of work. 3D is
left out on purpose: it needs a solid modelling kernel and a 3D mesher, a project of
its own.

## What a model is (the model builder's way, kept)

A model builder's tree is also the order of work: global parameters,
then a component's geometry (a sequence of features, rebuilt when a parameter
changes), materials on its domains, physics with their domain and boundary
conditions, multiphysics couplings, a mesh, studies (stationary, time dependent,
frequency, eigenvalue, parametric sweeps) and results (datasets, plots, derived
values, exports). Choosing a node shows its *Settings*; the *Graphics* window shows
the geometry, lets the user pick domains, boundaries and points, and shows the mesh
and the fields. That way of working is what users of the commercial tools, ANSYS and Elmer expect,
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
   The tight loop of select, solve and evaluate that makes an integrated tool pleasant is hard to
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
(study). A node that needs its parent built again shows a mark.

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
"Form Union" gives domains, which change when the geometry does: this is the most
common complaint about such tools. Picking domains in the view writes numbers; *Make a Rule*
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

Each study is a list of steps:
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
| **4b. Semiconductors and rays** (asked for after phase 4) | drift-diffusion: Poisson's equation and the carriers' continuity, Scharfetter-Gummel currents, contacts, recombination, at rest, in time and small-signal; the plasma dispersion into the modes; geometrical optics: rays refracted and reflected (Snell, Fresnel), graded media, absorption as a heat or generation source | 3,000 + 900, as built | 1 |
| **4c. Quantum wells** (asked for after 4b) | the effective-mass Schrödinger equation of electrons, heavy and light holes in 1D along the growth axis and in 2D over the cross-section; III-V compounds and alloys with their band parameters, strain, a field or another physics' potential; the states, their transitions, absorption and gain spectra | 2,200 + 900, as built | 1 |
| **4d. PDEs of your own** (asked for after 4c) | the coefficient and general form PDE, any number of variables, nonlinear by Newton's method, in 2D or along a line in 1D (ODEs); Dirichlet, flux and zero flux conditions; at rest, in time (first and second order) and its eigenvalues | 1,500 + 600, as built | 1 |
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
- **What it is not:** no 3D; no CFD. (Semiconductors and ray optics were left out here;
  they were built after phase 4: "Semiconductors and ray optics, as built".)
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
   their own on the right, a *Settings* window.

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
  lengths in the component's unit: `L/2 - 0.6` with `L = 2[mm]` is
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

## Semiconductors and ray optics, as built

Built on 11 October 2026, Qucs-S 26.1.8, before phase 5, as asked ("before phase 5, let's
add semiconductors (drift-diffusion) and ray optics"): two physics this proposal had left
out. The drift-diffusion equations are in `fem_semiconductor`, the rays in `fem_rays`.
About 3,000 lines and 900 of tests.

**How semiconductors are done:**
- **The equations:** Poisson's, -∇·(ε∇V) = q (p - n + Nd - Na), and the carriers'
  continuity, ∇·Jn = q (R - G) + q ∂n/∂t and ∇·Jp = -q (R - G) - q ∂p/∂t. The unknowns are
  V, n and p (Boltzmann's statistics) at the nodes of linear triangles; V is the
  intrinsic level's (Ei = -qV), so equilibrium's Fermi level is 0.
- **The currents:** each edge's is Scharfetter and Gummel's,
  Jn = q µn Vt/h (n_j B(Δ) - n_i B(-Δ)) with B(x) = x/(e^x - 1) and Δ = (Vj - Vi)/Vt. It
  is weighed by the edge's cotangent weights (half the cotangent of the angle opposite it
  in each triangle): the edge-averaged finite elements of Xu and Zikatanov, which are the
  box method's on a Delaunay mesh and keep the carriers positive. The few edges of a mesh
  that are not Delaunay (negative weights) are left out of the currents, and said.
- **Charge and recombination** are lumped at the nodes, on each node's share of its
  semiconductor triangles: Shockley-Read-Hall, Auger and direct recombination, a
  generation G of x, y, t or another field.
- **Newton's method** solves V, n and p together, with a sparse LU. Its unknowns are in
  units of Vt and the largest doping, and each row is divided by its largest entry. A step
  moves V by 10 Vt at most and keeps the carriers above a twentieth of what they were.
- **From equilibrium:** Poisson's equation alone, n = ni e^(V - φ)/Vt, p = ni e^(φ - V)/Vt,
  is solved first. Then the contacts' voltages are raised in steps, doubled when Newton
  takes them in a few iterations and quartered when it fails. A sweep's next point starts
  from the last on the same mesh: a diode's I-V does not start again from 0 V.
- **When no current can flow** (the contacts that hold carriers at one voltage, no
  generation: a MOS capacitor, an unbiased junction), Poisson's equation alone is exact.
  The edges' currents vanish identically there, and np = ni². It is solved alone: Newton
  on all three was ill-conditioned in inversion, the minority carriers held only by
  recombination.
- **Contacts:**
  - ohmic: the carriers at charge neutrality and equilibrium;
  - Schottky: V = Va + (Ec - Ei - φB)/q, with thermionic emission q v (n - n_eq) over the
    barrier;
  - a gate on an insulator: V = Va - (Φm - χ - (Ec - Ei))/q.

  The current into the device is the continuity rows' reaction at the contact, and the
  metal's charge is Poisson's.
- **In time:** BDF, the masses q A on n's and p's rows. The displacement current is the
  contacts' charges' change, the same BDF formula applied to them.
- **Small-signal:** in a frequency step after a stationary one, (J + jωM) δx = -∂F/∂V_k for
  each contact k, solved in the quasi-Fermi potentials' changes: δn = n/Vt (δV - δφn),
  δp = p/Vt (δφp - δV). In n and p, a current near nought is the difference of two large
  terms, and that loses digits. The admittance matrix is Y = δI + jω δQ, giving semi.G and
  semi.C. A minority carrier that cannot follow at 1 MHz gives the high-frequency C-V
  curve.
- **The low-frequency limit:** a MOS capacitor's inversion layer is held by almost nothing
  (generation, and diffusion from the back contact through 10⁹ electrons per m³), so at
  1 µHz the small-signal system is singular to within a double's digits. It gave 0.9 to 1.2
  Cox on two meshes. At equilibrium the stationary step instead gives the gates'
  quasi-static capacitance, semi.C0: Poisson's equation linearized with the carriers
  following and their Fermi level held. That is the low-frequency limit, well conditioned,
  and agrees with dQ/dV between solutions 10 mV apart.
- **Into the modes:** a wave feature's plasma dispersion takes semi.n and semi.p:
  Δn = -(8.8e-22 N + 8.5e-18 P^0.8) and Δα = 8.5e-18 N + 6.0e-18 P (cm⁻³, cm⁻¹), Soref
  and Bennett's at 1550 nm, scaled as λ².

**How rays are done:**
- **Straight in a uniform medium:** a ray meets the domain's own boundary pieces - lines
  exactly, arcs of ellipses (a circle's too) as a quadratic in the arc's frame, the
  normal from the implicit form - not the mesh's facets. A lens's focus is the true
  curve's.
- **Graded media:** where n or κ reads x, y or a field, the ray equation d(n dr/ds)/ds = ∇n
  is solved by RK4. The steps are a third of the mesh's mean size, ∇n is taken by central
  differences, and a boundary is found on each step's chord. In the sech profile every
  ray has one period, and a quarter-pitch GRIN lens focuses to 0.4 µm.
- **At an interface:** Snell's law, and Fresnel's rs and rp on each ray's s and p powers.
  The reflected part is released as a ray of its own (above the threshold), and total
  internal reflection takes place beyond the critical angle. Mirrors reflect R, absorbers
  and the physics' outside stop the ray, coatings let it through whole, and detectors
  record it (stopping it or not).
- **Absorption:** e^(-4πκ s/λ) along the path. The power lost is put in the triangles
  along it, in pieces a quarter of the mesh's size, and becomes a nodal density gop.Q:
  each node gets the mean of its absorbing triangles', by area. Its integral over the
  absorbing media is the power absorbed exactly. Heat takes it as a source; a
  semiconductor takes gop.Q λ/(hc) as its generation.
- **Wavelengths:** each ray's is set on its point evaluator (its frequency c/λ), and an
  index may read lambda0: BK7 and fused silica by Sellmeier's formula.
- **Threads:** the releases are traced in threads, each with its own deposit, which are
  summed at the end.
- **Stopping:** a ray ends past its interactions or path length, or when its power falls
  below the threshold.

**Where they differ from what such tools have:**
- **Boltzmann's statistics only:** no Fermi-Dirac, no incomplete ionization, no band-gap
  narrowing.
- **Heterojunctions are not modelled:** the carriers are continuous at a node between two
  semiconductors. One semiconductor material per device, or several with the same bands.
- **Isothermal:** the temperature is one constant (no hydrodynamic or thermal model of
  the lattice), and there is no tunnelling (impact ionization since 2026-10-12).
- **Velocity saturation lags a Newton step** (its force from the step before). Since
  2026-10-12 the force is each carrier's quasi-Fermi level's gradient, no more than |E|, by
  default (the element's |E| is still a choice). The mobilities are held once the potential
  settles, which leaves them about 1e-4 from the final carriers'. Lombardi's surface
  scattering at an insulator was added the same day.
- **Rays are in the plane,** and no diffraction or interference; they are traced at rest
  (not in time); a diffuse wall, gratings and ray-to-mode coupling are not there.

**Fast enough** (M-series Mac, Release):
- A pn diode at 11 voltages, with 1 kHz small-signal at each: 1 s.
- A MOS capacitor's C-V at 25 gate voltages, quasi-static and at 1 MHz: 5 s.
- The phase shifter (drift-diffusion, then the TE mode, at 5 biases): 12 s.
- The ray examples, a photodiode lit by rays among them: under 1 s each.

**Checked:**
- `test_fem_semiconductor` (13):
  - a bar's resistance against q n µ V A/L (1.2 × 10⁻⁴); by Caughey and Thomas's mobility
    at 1e17 cm⁻³ (10⁻⁴); saturated at 10⁷ V/m against q n v(E) (exact - each element's
    mobility by its own field; by the field along each edge it was 20 % high);
  - a junction's built-in voltage (exact), and its small-signal capacitance against the
    depletion approximation's ε/W at 0 and -2 V (0.14 %, 0.04 %);
  - a short diode's current at 0.4 V against Shockley's (0.06 %);
  - a MOS capacitor at flat band (0.1 %), in accumulation (0.98 Cox), and in inversion
    at 1 MHz (Cmin, 2.3 %) and quasi-statically (0.97 Cox); C0 and 1 MHz agreeing where the
    majority carriers alone respond;
  - the plasma dispersion against the index given (10⁻⁷);
  - doping of x (an expression), and direct recombination's G/(B n) under a generation
    (0.2 %);
  - a Schottky diode: its reverse current 12 % under thermionic emission's q vn Nc e^-φB/kT
    (the drift through the depletion not infinitely fast), its ideality 1.006;
  - a fixed oxide charge moving the flat band by -Qf/Cox (exact);
  - carriers' rise under a generation switched on, G τ (1 - e^-t/τ) (0.3 %);
  - the examples.
- `test_fem_rays` (10):
  - Snell's law and Fresnel's split, transmitted and reflected (10⁻¹²);
  - total internal reflection;
  - a plano-convex lens's paraxial focus (4 µm in 101.7 mm), its margin nearer;
  - a graded-index rod's axis crossings (0.6 µm);
  - absorption, transmitted and absorbed (10⁻⁹), gop.Q's integral the power absorbed;
  - a prism's exit angles against Snell at both faces (10⁻⁹ rad);
  - a mirror's reflectance, a detector that lets rays pass, a Gaussian beam's weights, a
    point's fan and rays from a boundary (each exact);
  - the examples (the thermal lens's heat equal to the power absorbed).
- `test_multiphysics_doc` (29) adds:
  - a diode swept in the window, its I-V and bands as datasets;
  - rays drawn by wavelength, and a rays model's default plots;
  - Claude's tools for a photodiode, and the new kinds described.

**Examples:**
- `pn_diode`: I-V from -2 to 0.7 V (ideality 1.005) and C-V.
- `mos_capacitor`: C-V at low frequency (quasi-static) and 1 MHz.
- `pn_phase_shifter`: a rib modulator reversed to -4 V, its TE mode's Δneff and loss;
  Vπ·L 2.1 V·cm.
- `lens_focus`: a plano-convex BK7 lens and its spherical aberration.
- `prism_dispersion`: seven wavelengths through BK7, 1.81° apart.
- `grin_lens`: a quarter-pitch sech rod focusing a beam to a point.
- `thermal_lens`: rays and heat in turn; 0.01 to 10 W narrows the beam from 0.50 to
  0.25 mm rms.
- `photodiode`: rays absorbed in silicon making carriers; 0.48 A/W, 95 % of the ideal.

## Quantum wells, as built

Built on 11 October 2026, Qucs-S 26.1.8, as asked ("let's add the 1D and 2D multi quantum
well (MQW) analysis"): a physics, *Quantum Wells (Schrödinger)*, in `fem_quantum`; the III-V
compounds and their alloys in `fem_materials`; two study steps. About 2,200 lines and 900 of
tests.

**How it is done:**
- **The equation:** -∇·(ħ²/2m ∇ψ) + V ψ = E ψ for electrons (V = Ec), heavy holes and light
  holes (V = -Ehh, -Elh: their energies counted downward), each carrier on its own. Its weak
  form makes 1/m ∂ψ/∂n continuous across a heterojunction: BenDaniel and Duke's condition,
  with nothing added. ψ = 0 where the physics ends (Zero Wavefunction), or free on the
  boundaries a Zero Flux feature picks.
- **1D** (the default): a line across the layers, along the growth axis through x = at (the
  middle of the physics' domains unless given). The mesh's triangles give the stretches of
  it in each domain exactly - where the line crosses a boundary between two layers is a node
  - and each layer gets quadratic elements of its own (0.1 nm by default: a 10 nm well's
  levels to a part in 10⁶, whatever the 2D mesh). A gap in the physics is a hard wall on
  either side. The states are free in the plane of the layers.
- **2D:** the mesh's own linear or quadratic triangles, the states free along z (a wire); about
  an axis, the states of m = 0 round it (a dot), weighed by r.
- **Masses:** the electron's from its compound (isotropic); the holes' from Luttinger's
  parameters - heavy 1/(γ1 - 2γ2) along the growth axis and 1/(γ1 + γ2) across it, light
  1/(γ1 + 2γ2) and 1/(γ1 - γ2). Along and across are the axis's and the plane's in 2D; in
  1D the masses across set the densities of states in the plane.
- **Bands:** a domain's Ev is its compound's valence band offset (Vurgaftman's common scale,
  InSb's top at 0) and Ec = Ev + Eg(T), Varshni's. A material without a compound gives Eg and
  VBO (or χ: the affinity's rule), me, γ1, γ2; a Quantum Well Material of its own gives Ec
  and Ev as expressions of x and y (a parabolic well, a graded one) and the masses.
- **Strain:** with a substrate (a material, a compound or a lattice constant), each layer
  takes its lattice in the plane - ε∥ = (a_s - a)/a, ε⊥ = -2 C12/C11 ε∥ - and its bands move
  by Bir and Pikus: Ec by ac (2ε∥ + ε⊥); the heavy holes by -P - Q, the light ones by
  -P + (Q - Δ + √(Δ² + 2ΔQ + 9Q²))/2 with P = -av (2ε∥ + ε⊥), Q = -b (ε∥ - ε⊥) (Chuang's
  signs; av positive, the gap opening by (ac - av) times the volume's change).
- **A field and a potential:** F along the growth axis adds q F (s - s0) to every band (s0
  the physics' middle); another physics' potential (semi.V, es.V) adds -q V.
- **The states:** each carrier's lowest, by a shift and invert below the lowest potential -
  (K - σM) positive definite, a sparse Cholesky - and the Krylov-Schur eigensolver the mode
  solver uses; a degenerate pair comes out of it mixed and complex, so its vectors' real and
  imaginary parts are made M-orthonormal and the states found again in their span (Rayleigh
  and Ritz). Under 600 unknowns, a dense generalized eigensolver. A state is bound when it
  lies inside the band edge where the physics ends.
- **The compounds:** GaAs, AlAs, InAs, InP, GaP, GaSb, AlSb and InSb with Vurgaftman, Meyer
  and Ram-Mohan's parameters (J. Appl. Phys. 89, 5815, 2001); ternaries linear with their
  bowings (AlGaAs's gap bowing -0.127 + 1.310 x); InGaAsP from its four ternaries (their
  (6.4)) and AlGaInAs from its three ((6.5)). On InP's lattice the ternaries alone leave
  InGaAsP's gap up to 70 meV high; a bowing of its own, -0.36 (1 - x) y (1 - y) eV (nought on
  its edges but InAsP's), brings it within 15 meV of Nahory's 1.35 - 0.72 y + 0.12 y². Other
  physics read an alloy's gap, affinity (its conduction band against GaAs's), densities of
  states, permittivity and index from its composition too - its mobilities and thermal
  conductivity roughly (Matthiessen's rule; an alloy's disorder 0.3 m K/W).
- **A Quantum States step** makes each state a solution, as a mode is: qw.Ee1, qw.Ehh1,
  qw.Elh1..., the transitions qw.Ee1hh1 and overlaps qw.Oe1hh1 (the first four of each),
  the effective gap qw.Eg and its wavelength qw.lambda, and the direct transition qw.Edir:
  the lowest electron to heavy hole one of at least half the largest overlap. Under a field
  a stack's lowest electron state and highest hole state lie in different wells, their
  transition indirect and dark; Edir is each well's own e1-hh1, the quantum-confined Stark
  effect's. Variables: |ψ|² (qw.P_e), ψ (its largest 1), the band edges, the masses, the
  strain, and qw.wave_e - a level with its |ψ|² drawn on it, the band diagram's picture.
  The bands are kept at each triangle's corners (each its own: the offsets stay sharp) once
  for all the states.
- **An Optical Spectrum step** at photon energies (or wavelengths): k∥ conserved, each
  e_i to h_j transition weighed by its overlap and its polarization at the band edge (TE:
  heavy 3/2, light 1/2 of |Mb|² = m0 Ep/6; TM: 0 and 2), α = π q² ħ/(n c ε0 m0² E) |ê·p|²
  |<e|h>|² ρ_r / L_w - ρ_r the 2D reduced density of states m_r/πħ² (a wire's 1D one), L_w the
  wells' thickness (the domains of the smallest gap). Broadened by a Lorentzian, in cells
  whose integrals are exact; without carriers a step's is closed. With carriers each
  quasi-Fermi level is found from the states (Σ m kT/πħ² ln(1 + e^((F - E)/kT)) per area),
  the occupations f_v - f_c along each transition's dispersion; gain is -α.

**Where it differs from what such tools have:**
- **Decoupled bands:** electrons, heavy and light holes each by itself, parabolic; no k·p
  (no valence bands' mixing, no nonparabolicity), no split-off band's states.
- **No excitons:** the absorption edge is the free carriers' steps, and the Stark shift the
  free pair's (without the exciton's binding, smaller than measured).
- **No self-consistency of its own:** the carriers' charge does not act back on the
  potential (Schrödinger and Poisson) - a doped well's bands come from another physics' V,
  once.
- **A 1D line's ends and gaps are hard walls;** there are no open boundaries (tunnelling out,
  a superlattice's Bloch states).
- **Spectra:** absorption and gain of the wells, not a waveguide's modal gain (Γ g: the
  confinement factor from a mode is the user's); no spontaneous emission, no many-body
  shrinkage of the gap; a dot's spectrum about an axis is not found.

**Fast enough** (M-series Mac, Release):
- A well's four states of each carrier along a 50 nm line (1,000 unknowns): 10 ms.
- The five-well MQW's states at three fields, and its absorption from 24 states of each
  carrier at 181 energies: 0.5 s.
- A strained laser's gain at four densities and 161 wavelengths: 0.4 s.
- A wire's states over its cross-section (quadratic triangles): 0.3 s.

**Checked:**
- `test_fem_quantum` (13):
  - an infinite well's levels, electrons' and heavy holes', against n²π²ħ²/2mL² (10⁻⁶),
    its density integrating to 1 and its overlaps 1 and 0;
  - a 10 nm GaAs well in Al0.3Ga0.7As, its electrons' and heavy holes' first two levels
    against BenDaniel and Duke's transcendental equations (10⁻⁶);
  - a 10 by 6 nm box in 2D, four levels and the heavy holes' first with their two masses
    (2 × 10⁻⁴);
  - a harmonic oscillator's ħω(n + ½) (10⁻⁶);
  - the second-order Stark shift of an infinite well, (15 - π²)/24π⁴ m q² F² L⁴/ħ²
    (0.005 %);
  - In0.2Ga0.8As strained on GaAs: ε∥, Ec, Ehh and Elh against Bir and Pikus (10⁻⁹ eV),
    the heavy holes on top, the barriers unstrained, the well's emission at 997 nm;
  - the alloys' gaps and lattices against the literature (InGaAs 0.74 eV on InP's lattice,
    InGaAsP Q1.3 at 0.97 eV), their corners their binaries, compositions out of range
    refused;
  - a well across a strip in 2D, its sides free, against the 1D line (2 × 10⁻⁵ eV), the next
    2D state the first with a node across the strip;
  - absorption's steps against their closed form (10⁻⁴), and the cells' integral against
    it with next to no carriers (2 × 10⁻³);
  - gain: the carriers' count from the quasi-Fermi level found (10⁻⁶), and the gain's zero
    at the levels' separation (0.6 meV);
  - the examples.
- `test_multiphysics_doc` (31) adds a well in the window - its band diagram a dataset of a
  curve for each state, its absorption over the photon's energy, a bare model's default
  plots - and Claude's tools for a well.

**Examples:**
- `gaas_quantum_well`: 10 nm of GaAs in Al0.3Ga0.7As; e1-hh1 at 1.461 eV (849 nm), the
  absorption's staircase, TE and TM.
- `ingaas_mqw_qcse`: five In0.53Ga0.47As wells in In0.52Al0.48As on InP at 1.52 µm; at 0,
  50 and 100 kV/cm each well's transition moves down 2.4 and 9.7 meV and its overlap falls
  from 0.98 to 0.75, while the stack's lowest and highest states go to different wells.
- `strained_laser_gain`: 8 nm of In0.2Ga0.8As on GaAs; TE gain at 986 nm rising from
  absorption at 1e18 cm⁻³ to 1900 /cm at 3e18, TM absorbing.
- `quantum_wire`: a 20 by 10 nm GaAs wire in Al0.3Ga0.7As; its states over the
  cross-section, the second 30 meV above the first with a node across its width.

## PDEs and ODEs of your own, as built

Built on 11 October 2026, as asked ("let's add support for solving any user-defined 1D ODE or
2D PDE with user-defined boundary conditions"): a physics, *PDE (Coefficient and General
Form)*, in `fem_pde`, and an *Eigenvalue* study step; the line a 1D physics is solved along
(`fem_line`), now shared with the quantum wells. About 1,500 lines and 600 of tests.

**How it is done:**
- **The equation:** ea ∂²u/∂t² + da ∂u/∂t + ∇·Γ = F for one or several dependent variables,
  named as the user likes (u, or u and v: each with ux, uy, and c.u, c.ux). The general
  form takes Γ (x, y) and F as written; the coefficient form builds them, Γ = -c∇u - αu + γ
  and F = f - β·∇u - au, its c isotropic or a 2 × 2 matrix. A property holds one item for
  each variable, split by ';'.
- **Any expression:** a coefficient may read x, y, t, the variables, their gradients and
  other physics' fields. The weak form, ∫ (Γ·∇φ + Fφ) + ∮ (g - qu) φ = 0, is solved by
  **Newton's method**. Its Jacobian comes from the expressions themselves, by differences:
  at each point of integration each component's value and each of its derivatives is
  nudged in turn (the point evaluator's shift), and Γ and F are evaluated again. This is
  exact for a linear coefficient up to rounding, and catches nonlinear ones (e^u, u(1 - u),
  a c(u)) and couplings between variables. A step is halved while it makes F larger; the
  first is taken whole (a linear problem's answer), as is one already within the
  tolerance.
- **2D:** the mesh's linear or quadratic triangles; about an axis the integrals are weighed
  by 2πr, so ∇· is the cylinders' divergence.
- **1D (an ODE):** a line along x or y through the physics' domains. The mesh's triangles
  give its stretch in each domain exactly; it has its own quadratic elements (200 by
  default), and its points of integration are placed in the mesh once. Its variables are
  read along the line (u, ux, uy and us, the derivative along it) wherever a plot asks.
- **Conditions:** Dirichlet (u = r, any of the variables), flux (-n·Γ = g - q u, Robin; g and
  q may read the variables, their derivatives in the Jacobian too), zero flux by default.
  In 1D they apply where the line meets the boundaries chosen - at its ends, or at an inner
  boundary it crosses.
- **In time:** the time stepper's BDF, da ∂u/∂t its mass. A second derivative (any ea not 0)
  adds each variable's rate w = ∂u/∂t as a component of its own, with w - ∂u/∂t = 0. This is
  integrated with the rest by the same BDF and shown as ut.
- **Eigenvalues:** -λ da u + ∇·Γ = F linearized where it is (its Dirichlet conditions at 0),
  A x = λ M x with A = -J and M the da mass (else ea's, else 1). These are solved by a shift
  and invert about the shift asked and the Krylov-Schur solver; each eigenfunction is a
  solution, with c.lambda (and its imaginary part when a convection makes it complex).

**Where it differs from what such tools have:**
- **Weak and time forms:** no weak form of the user's own, no point or edge PDEs, no
  global (ODE) variables, no algebraic constraints of the variables (a DAE).
- **Second derivatives:** none in the expressions (P2's are piecewise constant).
- **Periodic conditions:** not there.
- **Eigenproblems:** linear only (no quadratic one in λ with both ea and da); ∂u/∂t cannot be
  read in a first-order equation's expressions.
- **1D:** a 1D line lives inside a 2D geometry: an interval is drawn as a strip.

**Fast enough** (M-series Mac, Release):
- the Bratu ODE at four λ: 16 ms;
- a drum's six modes on 3,000 quadratic triangles: 0.1 s;
- Fisher's front over 500 elements and 140 steps in time: 1 to 2 s.

**Checked:**
- `test_fem_pde` (12):
  - Poisson's equation's manufactured solution in a square, its error 13.6 times smaller
    when h is halved;
  - the Bratu ODE against its closed form (2 × 10⁻¹³), in 3 Newton iterations;
  - convection and diffusion at a Péclet number of 20 (10⁻⁷);
  - a Robin condition and two layers' flux continuity (10⁻¹⁰, exact);
  - heat decaying as e^(-π² t) (10⁻⁴), and a standing wave a period later as it was (10⁻⁵);
  - a string's eigenvalues n² held (10⁻⁸) and free (0, 1, 4), and a square membrane's
    2π², 5π² twice, 8π² (10⁻⁵);
  - two coupled variables (their coupling through F, 2 Newton iterations);
  - the general form against the coefficient form (10⁻¹⁰);
  - anisotropic diffusion;
  - a disc about its axis (exact);
  - the examples.
- `test_multiphysics_doc` (33) adds a 1D ODE in the window (its default plot a line graph
  along its line, a dataset), a drum's modes and eigenvalues, and Claude's tools for them.

**Examples:**
- `bratu_ode`: u'' + λ e^u = 0 swept toward thermal runaway; u(½) to 10⁻⁷ at each λ.
- `drum_modes`: a circular membrane's six modes, λ = j² of Bessel's zeros (10⁻⁵).
- `fisher_front`: Fisher's equation's front travelling at 1.995 (2√(rD) = 2).
- `annulus_laplace`: Laplace's equation in a ring on curved elements, ln r / ln 2 and the flux
  2π / ln 2 through the outer circle (3 × 10⁻⁴).

## Photodiodes and electroabsorption modulators, as built

Built on 12 October 2026, as asked ("let's add the physics for simulating photodiodes such
as germanium on silicon; also add the physics for electro-absorption modulator (EAM)
simulations"). It brings heterojunctions to the semiconductors, a direct gap's absorption
and the Franz-Keldysh effect to the mode analysis (`fem_optics`), and an *Optical
Generation* feature that feeds a mode's light into a photodiode. About 700 lines and 500
of tests.

**How it is done:**
- **Heterojunctions:** a semiconductor's unknown V is the first semiconductor's intrinsic
  level (Ei = -qV); another's is offset by δ = (χ + (Ec - Ei) - the first's)/q, so that
  Ei = -q (V + δ) - Anderson's rule. Each carrier's Scharfetter-Gummel current steps over
  its own band potential: V + θn for electrons and V + θp for holes, θ = δ ± Vt ln(ni/ni_ref)
  (the densities of states in ni). This is exact at equilibrium (no current where the
  quasi-Fermi level is flat), and so are the equilibrium's carriers, n = ni e^((V + δ - φ)/Vt).
  Ohmic and Schottky contacts subtract δ. A node on an interface belongs to its first
  semiconductor; the bands' step is taken along the edges into the other (an abrupt
  junction graded over one element). The band edges plotted are each element's own (a step
  at the interface); the quasi-Fermi levels are the nodes' own, linear between them (no
  step). One semiconductor alone is as before (δ = θ = 0).
- **A direct gap's absorption:** α = A √((ħω - Eg)/1 eV) (1 eV/ħω) without a field. Under one,
  the Franz-Keldysh effect (Callaway, Tharmalingam):
  α = A (ħθ/1 eV)^½ π (Ai'(β)² - β Ai(β)²) (1 eV/ħω), with β = (Eg - ħω)/ħθ and
  ħθ = (q²ħ²F²/2µ)^⅓. This gives a tail below the gap and ripples above it. Airy's functions
  are our own (libc++ has none): a power series near nought, Poincaré's expansions beyond
  5.5 and -8 (6 × 10⁻⁹ against SciPy's).
  - An optional Gaussian broadening σ of the gap stands for a real edge's tail. It is
    integrated by Simpson's rule, in t = √(ħω - Eg - s) without a field, to smooth the
    √ kink.
  - A wave feature's *Direct-gap absorption* adds α λ/4π to κ, the field F read from an
    expression (semi.normE). The gap, reduced mass and strength (Egd, mr, Afk) come from the
    material: Germanium 0.80 eV, Germanium on silicon - 0.25 % tensile, a new entry - 0.773 eV;
    both µ 0.03, A 2 × 10⁴ /cm, 8000 /cm at 1310 nm and 4100 /cm at 1550 nm on silicon.
    They can also be given as the user's own.
- **What a mode absorbs:** at each mode selected, each triangle's n, the κ absorbed across a
  gap (the medium's own and the direct gap's, not the free carriers'), and the direct gap's
  κ with and without a field are kept with the mode.
  - Variables, the mode at 1 W: emw.Qh (all it absorbs, ½ ω ε0 Σ -Im εk |Ek|²), emw.Qgap
    (ω ε0 n κ |E|² across the gap), emw.alphagap, emw.dalphaEA.
  - Globals: emw.loss_gap (dB/m absorbed across the gap) and emw.dloss_EA (what the field
    adds over none at all).
  - emw.loss_gap equals emw.loss from Im β to 10⁻¹² when the gap absorbs all of it -
    Poynting's theorem holding on the discrete fields.
- **Optical Generation** (a semiconductor's domain feature: the mode analysis's tag, P0, η,
  a load RL):
  - Each photon the mode loses across the gap makes η pairs: G = η P0 emw.Qgap × mean / hν.
  - The power falls along the device as e^(-2αz); mean = (1 - e^(-2αL))/2αL over its length
    L, the model's thickness, so the semiconductor's current is the whole device's.
  - Before the mode is solved (a dark step) there are none.
  - Globals: semi.Gop (pairs per second), semi.Iop = q Gop, semi.Pin. A responsivity is
    semi.I_k/semi.Pin.
- **Its response, modulated:** a frequency step solves the small-signal system once more,
  the light modulated wholly (δG = G), the contacts held. Each contact's current comes by
  the same weights as the admittance's: semi.Hop_<c> = |δI_c|/(q Gop) - at low frequency the
  share collected, falling with the carriers' transit.
  - semi.HopRL_<c> puts the device's own Y_cc beside the load: × RL⁻¹/(RL⁻¹ + Y_cc).
  - The 3 dB bandwidth is where it is 1/√2 of its low frequency's.
- **Its study:** a stationary semi step (dark), a mode emw step (reading semi.normE), a
  stationary semi step (lit: reading emw.Qgap), then a frequency step. Swept, each point
  starts from the last. A frequency step after a mode analysis makes a series of its own,
  each snapshot with the mode's fields and globals. It does not follow the mode's snapshot
  at the light's 193 THz, which a 1D plot group would draw along the modulation's
  frequencies.

**Where it differs from what such tools have:**
- **Heterojunctions:** graded over an element, not thermionic emission over an abrupt step
  (no double nodes); Boltzmann's statistics, so a heavily doped heterojunction's degenerate
  side is not exact; no band-to-band tunnelling.
- **The Franz-Keldysh effect:** one reduced mass (germanium's heavy and light holes
  together, µ 0.03); no excitons; no electrorefraction (Δn by Kramers and Kronig) - a
  modulator's chirp is not given; germanium's indirect absorption is not modelled (small
  at 1550 nm).
- **The quantum-confined Stark effect:** the quantum wells physics gives its spectra (the
  InGaAs/InAlAs example), but they do not feed the mode analysis yet: a multiple quantum
  well modulator's loss against its bias is not modelled.
- **Coupling:** the steps in turn, not iterated - a photocurrent's space charge is felt by
  the next mode step only.
- **Free carriers in germanium:** the plasma dispersion is silicon's (Soref and Bennett's),
  so it is left off germanium.

**Fast enough** (M-series Mac, Release):
- a Ge-on-Si photodiode's dark, lit and 16 frequencies on 4,200 elements: 2 s;
- its spectral response at 8 wavelengths: 13 s;
- a modulator against 5 biases: 8 s; its 26 spectra points: 36 s.

**Checked:**
- `test_fem_optoelectronics` (5; 15 s, 4.5 minutes under the sanitizers on a coarser mesh):
  - Airy's functions against SciPy's at 19 points (6 × 10⁻⁹), the Franz-Keldysh absorption
    at five photon energies and fields (10⁻⁶), and broadened at four (2 × 10⁻³);
  - a 15 µm photodiode:
    - its mode's loss and ∫ ω ε0 n κ |E|² (10⁻⁶);
    - the pairs against the photons it absorbs, P (1 - e^(-2αL))/hν (0.6 %);
    - the photocurrent and 92 % collected;
    - the 1 MHz response against the share collected (0.3 %), falling by 100 GHz;
  - a 50 µm modulator from 0 to -3 V:
    - its loss tenfold;
    - the rise against dloss_EA's (10⁻²), the zero field's share the same at both;
    - the local α against the formula at the semiconductor's field (10⁻⁵);
  - the two examples against their descriptions.
  - Fourteen deliberate breaks were run against it and the heterojunction's test, one at a
    time; each fails at least one:
    - in the heterojunctions: the currents without the bands' offsets, an ohmic contact
      without δ, Ec without δ;
    - in the absorption: the Franz-Keldysh formula without π, Airy's expansions from 3.5,
      the broadening's t for t²;
    - in the photodiode: no mean along the device, a photon's energy hc, the modulated
      light without its holes;
    - in the mode's absorption: loss_gap without n, κ across the gap without the direct
      gap's, dloss_EA not less its zero field, no field read, no broadening read.
- `test_fem_semiconductor` adds a heterojunction:
  - n-Si beside n-Ge at equilibrium: each side's Ec - EF kT ln(Nc/Nd) to 0.1 meV, the steps
    0.050 and 0.510 eV, the Fermi level flat to 10⁻¹⁶ eV, no current;
  - a p-Ge n⁺-Si short diode's current, q Dn ni_Ge² / (Na W) (e^(V/Vt) - 1), forward and
    reversed (0.3 %).

**Examples:**
- `ge_photodiode`: a lateral p-i-n of germanium on a silicon waveguide, 15 µm long, at -1 V.
  - Responsivity: 1.15 A/W at 1550 nm (92 % collected). The pairs counted by triangle
    (2026-10-12) took it from 1.15 to 1.14: a node on the germanium's edge had taken its light
    over the silicon's share too, 0.6 % more pairs than photons. Saturation driven by the force
    along the current (2026-10-12), not the whole field, took it back to 1.149.
  - Dark current: 7.2 nA.
  - Bandwidth: 29 GHz, transit and diffusion (27 while saturation was driven by the whole field:
    the doped germanium's carriers, diffusing out, slowed by the heterojunction's field across
    them).
  - Along the wavelength, the mode followed from 1500 nm: 1.11 to 1.15 A/W to 1580 nm, then
    0.42 A/W at 1620 nm past the strained edge. (Until 2026-10-12 the sweep searched about
    neff 3.9 at each point and from 1580 nm took another branch, 3.956: 1.18 A/W there.)
  - Its heterojunction's bands: a 0.52 eV valence step keeps the holes in the germanium.
- `gesi_eam`: a germanium-silicon (Egd 0.82 eV, broadened 8 meV) stripe, 50 µm long.
  - At 1550 nm: 0.65 dB inserted at 0 V, 5.6 dB of extinction at -3 V and 7.3 dB at -4 V.
  - Photocurrent: 0.95 mA at -3 V.
  - Its spectra at 0 and -3 V (a curve per bias, the two-parameter sweep's axes): above the
    gap the field lowers the absorption, below it raises it.
