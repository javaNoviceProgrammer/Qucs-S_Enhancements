# Proposal: an "RF 2.5D" panel - a method-of-moments S-parameter solver for planar structures

*23 October 2026, after Qucs-S 27.0.4 (1be35c3). A written proposal only; nothing here is built.*

*The request: "add a new panel called 'RF 2.5D' that implements a 2.5D RF Electromagnetic propagation
solver for wave propagation that can calculate RF s-parameters similar to what [a commercial planar
solver] can do based on method of moments for planar structures. The idea is to still have a 2D GUI
similar to our current multiphysics gui, but the tree allows defining RF ports (edge ports or internal
ports on metals) and also defining the material stack in the z-direction (for dielectrics and
conductors) or loading it from a file. It should also allow importing a GDS or OASIS format and provide
some layout processing such as merging small VIAs into each other."*

*The facts about this tree were checked on 23 October 2026. Sizes and weeks are estimates, and the
honest ones are wide.*

## Is it doable? Yes, in stages - with one thing said plainly

**Doable.** A 2.5D method-of-moments solver that gives trustworthy S-parameters for planar passives -
spiral inductors, transformers, baluns, MIM capacitors, transmission lines, pads and interconnect - is
a well-trodden problem, and this tree already holds an unusual amount of what it needs: a GDS and
OASIS reader, a frequency-domain solver with ports and an S-matrix, Touchstone export, a layer-stack
notion in the process flow, a model tree and panel to copy, and dense complex linear algebra with
GMRES.

**Not a match for the established tools.** Their value is not the method - the method is in
textbooks - but twenty years of acceleration, robustness and silicon correlation: millions of unknowns
on a full RFIC block, Green's functions that hold from DC to 100 GHz over lossy silicon, meshing that
survives real layouts with dummy fill, and de-embedding that matches measurement. What is proposed is a
solver that is right on structures of up to a few tens of thousands of unknowns and says when it is out
of its depth. Anyone comparing it with a commercial planar solver on a full block will find it slower
and smaller, and the write-up should say so rather than imply otherwise.

The order below is chosen so that **something useful arrives early**: the layout and stack work, then a
quasi-static extraction that is already what many inductor and interconnect questions need, then
full-wave, then speed.

## In short

- **An RF 2.5D model is a document of its own**: a `.qrf25` file, its own tab *RF 2.5D* in the left
  dock, and the layout drawn in a 2D view - the same shape of GUI as *Multiphysics*, a tree on the left
  and the chosen node's settings below it.
- **The z-direction is a stack, not geometry.** A *Stack* node holds an ordered list of layers from the
  substrate up: dielectrics (thickness, εr, tanδ or σ) and metals (thickness, conductivity, roughness),
  with vias between named metals. It is typed in, or loaded from a file, and each GDS/OASIS layer and
  datatype is mapped to a metal or via of the stack.
- **The x-y geometry is the layout.** GDS and OASIS are imported with gdstk (which this tree already
  uses), one cell chosen, and *layout processing* runs before meshing: merging vias that are closer
  than a distance, dropping slivers and shapes below a size, snapping to a grid, healing polygons,
  cutting dummy fill out.
- **Ports are tree nodes**: an *Edge Port* on a polygon's edge (with a reference plane and a line to
  de-embed), and an *Internal Port* between two metals or between a metal and ground at a point.
- **The solver is a mixed-potential integral equation** on the metals: RWG basis functions on
  triangles, rooftops on the rectangles that layouts are mostly made of, vertical attachment modes for
  vias, and layered-medium Green's functions. The matrix is dense; a frequency point is a fill and a
  solve.
- **Results are S-parameters** over a frequency sweep, written as Touchstone through the export node
  that is already there, with the currents drawable on the layout.
- **The order of work:** the document, stack and layout first; then a quasi-static extraction (useful
  on its own, and it proves the Green's functions and the mesh); then full-wave; then ports and
  de-embedding; then acceleration; then the sweep's conveniences.

## What there is to build on

| Part | Now | What it gives this proposal |
|---|---|---|
| GDS and OASIS reading | `layout.cpp` and the layout viewer, about 5,160 lines, on gdstk: cells, hierarchy, repetitions, layers by layer and datatype, a viewer with panning, zoom, a ruler and search | The import, the layer list, and a view to draw the mesh and the currents on |
| GDS and OASIS into a model | `multiphysics/fem_import.cpp`, 1,340 lines: gdstk already reads a layout into geometry for the FEM models (and SVG and DXF) | The polygons, in the engine's own form |
| A frequency-domain solver with ports | `multiphysics/fem_waves.cpp`, 4,162 lines: ports and their S-parameters, port modes, scattering boundaries, Floquet ports | The S-matrix bookkeeping, the port abstraction, and a second opinion to check against in 2D |
| Touchstone export | The `touchstone` export node: a line's S-parameters or the waves' ports, `.s2p`/`.sNp` beside the model, coupled lines as a 2N-port, mixed-mode | The output, unchanged |
| Line parameters | The `lineparams` derived value: quasi-TEM from electrostatics, or R, L, G, C over frequency | The reference a uniform line's 2.5D answer must match |
| A layer stack | `multiphysics/fem_process.cpp`, 1,279 lines: a process flow of deposits, etches and fills, its masks taken from a layout's layers along a cut | The precedent for a stack, and an importer to share |
| The model tree and panel | `fem_model.h/.cpp` (a node's kind, its properties, what it may hold), `multiphysicsdoc.cpp`, `multiphysicspanel.cpp`, `multiphysicsview.cpp`, `qucscontrol_fem.cpp`: about 8,861 lines | The document, the tree, the settings, the tools - the shape this panel copies |
| Dense complex algebra | Eigen throughout; `fem_edge.cpp` has a complex Arnoldi/GMRES with a Schur reordering; Accelerate is linked weakly for sparse factorizations | The matrix solve, and the iterative path for later |
| Parallel work | Assembly in parallel, a sweep's points solved together (`fem_solver.h`), jobs off the GUI thread since `da357b4` | The fill and the sweep, which are where the time goes |
| Expressions and parameters | One expression language across the tree, parameters, sweeps, units | Frequencies, dimensions and stack values as expressions |
| Tests | About 29,556 lines of `test_fem_*.cpp` | The pattern: analytic cases first, then measured ones |

What is **not** there and must be written: the layered-medium Green's functions, the surface mesh for
planar metals, the matrix fill, the ports and their de-embedding, the layout processing, the stack and
its file formats, and the acceleration.

## The document and the panel

### A model of its own

As with the 3D proposal, this is a document, not a mode of the 2D one: a `.qrf25` file, a tab of its
own, and its own view. The *Multiphysics* tab stays as it is.

The view is the layout viewer's canvas, with the layers in the stack's colours, and - once there is a
solution - the mesh, the current density and the ports drawn over it.

### The tree

```
RF 2.5D model
├── Parameters                 w = 10[um], f0 = 10[GHz], ...
├── Stack                      the z-direction: dielectrics, metals, vias
│   ├── Substrate              silicon, 300[um], εr 11.9, σ 10[S/m]
│   ├── Dielectric  ILD1       SiO2, 1[um], εr 4.1, tanδ 0.002
│   ├── Metal       M1         0.6[um], 3.3e7[S/m], roughness 50[nm]
│   ├── Via         V1         M1 to M2, 2.1e7[S/m]
│   ├── Metal       M2         0.9[um], ...
│   ├── Air above              or a cover: a metal lid at a height
│   └── Ground                 the bottom: a perfect plane, an impedance, or open
├── Layout
│   ├── File                   talk.gds, cell "inductor", units
│   ├── Layer map              GDS 31/0 → M1, 32/0 → V1, 33/0 → M2, 40/0 → ignore
│   └── Processing             merge vias, drop slivers, snap, heal, remove fill
├── Ports
│   ├── Edge Port      P1      on M2's edge at x,y; reference plane; de-embed 20[um]
│   └── Internal Port  P2      M2 to M1 at x,y; 50[Ω]; or to ground
├── Mesh                       cell size, edge refinement, skin-effect layers
├── Study                      frequency: 0.1 to 40[GHz], 40 points, adaptive
├── Results                    S-parameters, Y and Z, currents, Q and L of a 1-port
└── Exports                    Touchstone, a SPICE subcircuit, a Qucs part
```

Every node is the same kind of object the FEM model already uses (`fem_model.h`: a kind, its
properties with their units and defaults, what it may hold), so the panel, the settings, the
undo, the file format, Claude's tools and the tests come almost free.

### The stack, and loading it from a file

The stack is the part a user will fight with, so it gets three ways in:

1. **Typed in the tree**, as above.
2. **A file of our own**: a small JSON (or the same `.qfem`-style JSON the models use), which is what
   *Save Stack As…* writes - so a stack can be shared between models and projects.
3. **Imported**. Which formats to read is a decision below. The honest options:
   - a **CSV or JSON table** of layers (name, kind, thickness, εr, tanδ, σ): trivial, and what most
     people can produce;
   - **ITF** (interconnect technology format, as used by parasitic extractors): a documented text
     format with `DIELECTRIC` and `CONDUCTOR` blocks - readable, and a real convenience;
   - a **Sonnet** or **ADS substrate** definition, or a commercial extractor's process file: each is
     another parser, and none is formally published. I would read ITF and our own, and add others only
     against real files the user has.

A stack is checked as it is read: layers in order, no gaps or overlaps in z, a via's two metals
adjacent in the order, conductivities and thicknesses above zero, and the frequency range the material
data is said to hold for.

### Layout processing

Before meshing, on the polygons of each mapped layer:

| Step | What it does | Why |
|---|---|---|
| Merge | Unions touching or overlapping shapes on a layer (gdstk's boolean `or`) | A layout's metal is drawn as many rectangles; the solver wants one conductor |
| Merge vias | Replaces a cluster of vias whose gaps are below *merge distance* with their union (optionally keeping the same total area, by shrinking the result) | A via farm of 400 small squares becomes one or a few shapes: this is often the difference between 80,000 unknowns and 4,000. It is an approximation, and the write-up says what it costs: the resistance of a merged farm is right to a few per cent, the current distribution within it is not |
| Drop slivers | Removes shapes thinner than a width, or smaller in area than a threshold | Fill, sub-resolution assist shapes and rounding artefacts |
| Snap | Rounds coordinates to a grid | Avoids near-degenerate triangles |
| Heal | Fixes self-intersections, repeated points and zero-width necks | gdstk and real layouts both produce them |
| Remove fill | Drops shapes on layers marked as dummy fill, or (a choice) keeps them as floating metal | Dummy fill changes capacitance a little and the unknown count a lot |

Each step says what it changed ("merged 412 vias into 7 shapes; dropped 38 slivers below 0.1 µm"), and
each can be turned off. The processed layout is drawn over the original in the view, so what the solver
sees is what the user checks.

## The solver

### The formulation

- **Equation.** The mixed-potential integral equation (MPIE) for the electric field on the metals,
  with the layered-medium Green's functions for the vector and scalar potentials. Metals are treated
  as surfaces with a complex surface impedance Zs = (1+j)/(σδ) for thickness ≫ skin depth, and as
  sheets of finite conductivity below that; a choice of a two-sided model for thick metal at low
  frequency.
- **Unknowns.** RWG basis functions on triangles, and rooftop functions on the axis-aligned rectangles
  a layout is mostly made of (far fewer unknowns for the same accuracy). Vertical **attachment modes**
  carry current between metals through a via.
- **Green's functions.** This is the heart of the work, and the risk:
  - the spectral-domain form for a stratified medium, with the transmission-line Green's functions of
    the stack (TE and TM), evaluated by **discrete complex image method** (DCIM) where it converges,
    and by direct **Sommerfeld integration** on a deformed path as the fallback and the reference;
  - tabulated against distance and interpolated, per layer pair, per frequency;
  - checked against closed forms in the cases that have them (free space; one dielectric with a ground
    plane; a microstrip's quasi-static limit).
- **Matrix.** Dense, complex, N×N. Filled in parallel (the engine already assembles in parallel), the
  near terms by singularity-extracted quadrature, the far terms by a multipole-free Gauss rule.
- **Solve.** A dense LU for N up to a few thousand (Eigen, or Accelerate's LAPACK where it is there);
  GMRES with a near-field preconditioner above that, which `fem_edge.cpp` already has the pieces for.
- **Memory.** N = 20,000 is 3.2 GB of complex doubles. This is the wall that makes acceleration not
  optional for real blocks, and the reason the early phases bound N by merging vias and by meshing
  coarsely away from the edges.

### Ports

- **Edge port.** On an edge of a polygon, with a reference plane and an optional de-embedding length:
  the current is fed as a delta-gap or a sheet source across the edge, and the line's own propagation is
  removed by solving a short and a long calibration line (the classic two-line de-embedding), or by a
  given Z0 and γ.
- **Internal port.** Between two metals at a point (a via-like feed), or between a metal and the ground
  plane: a lumped excitation across a gap, with a port impedance.
- **Renormalisation and reference.** Each port has an impedance; the S-matrix is renormalised to it,
  and the reference plane is said in the Touchstone comment, since a port's definition is where
  comparisons with other solvers go wrong.

### The sweep and the output

- A frequency study, its points solved in parallel as the FEM sweeps already are.
- **Adaptive frequency sampling**: a rational fit over the points solved so far, a new point where the
  fit and the solver disagree most, stopping on a tolerance - this is what turns 200 points into 20.
- Passivity and causality checked on the result, and said when they fail (they are the first sign of a
  bad port or an over-merged via farm).
- Output: Touchstone through the export node that exists; a 1-port's L and Q, a 2-port's Z0 and γ, as
  derived values; the current density drawn on the layout per frequency.

## Your workflow

1. New RF 2.5D model; *Stack* → *Import…* → the ITF or CSV of your process; check the layers.
2. *Layout* → the GDS or OASIS file, the cell; map 31/0 → M1, 32/0 → V1, 33/0 → M2.
3. *Processing* → merge vias within 0.4 µm, drop slivers below 0.1 µm. The view shows 412 vias became 7.
4. *Ports* → an edge port on each of the two feed lines, reference planes at the pads, de-embed 20 µm.
5. *Study* → 0.1 to 40 GHz, adaptive.
6. Solve. The currents are drawn on the layout; *Results* gives S, and L and Q of the inductor.
7. *Exports* → Touchstone → `inductor.s2p`, which a schematic's S-parameter part reads at once, so the
   inductor goes straight into a circuit simulation in the same program.

That last step is the reason to build this inside Qucs-S at all: the extracted part lands in the
simulator that is already in front of you.

## Claude's tools

The FEM tools generalise: `fem_model`, `fem_edit`, `fem_build`, `fem_solve`, `fem_stop`, `fem_export`
take the new document kind, with the node kinds above. Two new ones earn their place:

- `rf_stack`: reads or writes a stack, and says what it checked.
- `rf_layout`: the layer map and the processing, and what each step changed.

## Files

| File | What | Size |
|---|---|---|
| `multiphysics/rf_stack.{h,cpp}` | the stack, its checks, its file formats | 1,200 |
| `multiphysics/rf_layout.{h,cpp}` | the layer map and the processing steps | 1,800 |
| `multiphysics/rf_mesh.{h,cpp}` | the surface mesh: rectangles kept as rectangles, triangles elsewhere, edge refinement | 2,400 |
| `multiphysics/rf_green.{h,cpp}` | the layered Green's functions: spectral form, DCIM, Sommerfeld fallback, tables | 3,500 |
| `multiphysics/rf_mom.{h,cpp}` | the fill, the solve, the currents | 2,200 |
| `multiphysics/rf_ports.{h,cpp}` | ports, excitation, de-embedding, renormalisation | 1,500 |
| `multiphysics/rf_static.{h,cpp}` | the quasi-static extraction of phase B | 1,400 |
| `multiphysics/rf_fast.{h,cpp}` | the acceleration of phase E | 3,000 |
| `rf25doc.{h,cpp}`, `rf25panel.{h,cpp}`, `rf25view.{h,cpp}` | the document, the panel, the view | 4,500 |
| `qucscontrol_rf25.cpp` | the tools | 900 |
| `tests/test_rf_*.cpp` | the tests | 9,000 |

About **31,400 lines**, of which 9,000 are tests. For scale, the FEM engine is 68,658 lines for twelve
physics.

## Tests

The pattern the FEM tests already follow - analytic first, then published, then measured:

- **The Green's functions**: against free space; against one dielectric over a ground plane (closed
  form); the DCIM against direct Sommerfeld integration to 1e-6 over distance and frequency; symmetry
  and reciprocity.
- **The mesh**: a rectangle stays one rectangle; a via farm's merged area; no triangle below a quality
  bound; the unknown count against the cell size.
- **The static solver**: a parallel-plate capacitor, a coaxial line, a microstrip's Z0 against the
  `lineparams` extraction already in the tree (they must agree to a per cent), a two-line coupling
  against the coupled-line model.
- **Full-wave**: a 50 Ω microstrip's S11 below -30 dB and its phase against βl; a λ/2 open stub's
  resonance; a known spiral inductor against its published measurement; reciprocity (S = Sᵀ) and
  passivity on every result.
- **De-embedding**: a line of a known length de-embedded to within a degree of phase.
- **The layout**: a GDS with 400 vias merged and solved, against the same structure unmerged - the
  write-up must carry that number, since it is the approximation a user needs to judge.
- **The document**: the tree, the panel, undo, the file, the tools, as `test_multiphysics_doc.cpp` does.

## Plan and size

| Phase | What | Size | Weeks |
|---|---|---|---|
| **A** | The document, panel, tree, view; the stack with its checks and our own file; GDS/OASIS import and the layer map; the layout processing with its report; the mesh and its drawing. **No solver** | 9,000 | 3-4 |
| **B** | The quasi-static extraction: charge MoM with the static layered Green's function for C, partial inductances for L, R from the surface impedance - RLGC, Z0, and S-parameters below the first resonance. Useful on its own, and it proves the stack, the mesh and the Green's functions | 5,000 | 3-4 |
| **C** | Full-wave MPIE: the spectral Green's functions with DCIM and the Sommerfeld fallback, RWG and rooftops, via attachment modes, the dense fill and solve, the currents | 8,000 | 6-8 |
| **D** | Ports and de-embedding: edge and internal ports, calibration lines, renormalisation, the S-matrix and Touchstone, passivity and reciprocity checks | 3,500 | 2-3 |
| **E** | Acceleration: ACA or the adaptive integral method on a grid, GMRES with a near-field preconditioner, the memory bound lifted from a few thousand unknowns to a few hundred thousand | 4,000 | 4-6 |
| **F** | The conveniences: adaptive frequency sampling, a SPICE subcircuit and a Qucs part from the result, the current animation, the stack importers beyond our own | 2,000 | 2-3 |

**20-28 weeks** in all for one person, and phase C is the one that slips: layered Green's functions are
where every such project spends its unplanned time.

Phases A and B together (6-8 weeks) already give something worth having: a layout and stack front end,
and an extraction that answers inductor and interconnect questions up to a few GHz. If the work has to
stop anywhere, it should stop there, not half way through C.

## Risks

- **The Green's functions.** Correct over distance *and* frequency *and* layer pairs is a different
  problem from correct in a test. DCIM fails silently for some stacks; the Sommerfeld fallback is slow
  but must always be there as the reference, and the solver should say which it used.
- **Memory and size.** Dense MoM is N². Without phase E the solver is honest only up to a few thousand
  unknowns - which is a spiral inductor, not a block. The panel should show the unknown count before
  solving and refuse what it cannot hold, rather than filling swap.
- **Via merging is an approximation.** It must be measurable (the test above) and visible (the view),
  never silent.
- **Lossy silicon.** A conductive substrate is the hardest common case for layered Green's functions
  and the one RFIC users care about most. It belongs in phase C's tests from the start, not after.
- **Port definitions.** Most disagreements between field solvers are port and reference-plane
  disagreements. Saying exactly what a port is, in the file and in the Touchstone comment, is cheap and
  saves days.
- **Scope.** "Similar to the commercial tools" could absorb years. The phases above are the fence;
  anything outside them (3D arbitrary geometry, thick-metal volume currents, non-planar packaging,
  EM-circuit co-simulation) is another proposal.

## Decisions for you

1. **Start where?** Phase B (quasi-static, useful in weeks) before phase C, as proposed - or straight
   into full-wave, accepting that nothing works until C lands?
2. **Document or mode?** Its own `.qrf25` document and tab, as proposed, or a third `space` of the
   existing model (`2D`, `axisymmetric`, `rf25`)?
3. **Stack formats.** Our own JSON plus ITF, as proposed? Add others only against real files you have?
4. **Basis functions.** Rooftops on rectangles *and* RWG on triangles (more code, far fewer unknowns on
   real layouts), or triangles alone (simpler, slower)?
5. **Acceleration.** ACA/H-matrix (simpler, kernel-independent) or AIM/pFFT (faster for planar, more
   work)? And is phase E in scope at all, or is "a few thousand unknowns" enough for your work?
6. **The substrate.** Is a lossy silicon substrate a day-one case (it changes the Green's function work
   in C) or can it wait?
7. **Validation targets.** Which structures must it match, and against what - your own measurements,
   results you have from another solver, or published benchmarks? Name two or three now; they shape the
   tests more than anything else here.
