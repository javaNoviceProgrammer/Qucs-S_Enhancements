# Proposal: 3D in the multiphysics FEM

*12 October 2026 - after Qucs-S 26.1.8. A written proposal only, as the review of 12 October
asked for ("FEM semiconductor capability", its gap 9: "2D and axisymmetric only - no FinFET,
GAA or 3D corner effects"; and its addendum's finding 6: which mix of modes a silicon
waveguide launches into a germanium photodiode). Nothing here is built. The facts about this
tree were checked in it, and the libraries' licences on their own pages and packages, on
12 October 2026. Sizes and weeks are estimates.*

**In short:** 3D is not a switch on the 2D engine. Every layer of the engine is 2D:
geometry, mesh, elements, assembly, solvers and plots. But most of what the review names
(FinFETs, nanosheets, corner effects, interconnects, a photodiode's contact fingers) is
**layers of 2D shapes stacked in z**. A mesh of prisms extruded from one 2D Delaunay mesh
keeps the semiconductor's box method sound in 3D. That is the hard part of 3D TCAD, and it
comes for free here. So I propose three stages:

- **Stage 0**, no 3D at all: an eigenmode expansion across a junction of cross-sections.
  It answers finding 6 directly. About 1,500 lines, one to two weeks.
- **Stage 1**: extruded, layered 3D, on prisms. About 10,000-14,000 lines, two to three
  months.
- **Stage 2**: general solids on tetrahedra. A project of its own, only for a concrete
  need, or done by driving an external engine.

## What is 2D now

The engine is about 28,000 lines in `qucs/multiphysics/`. On top of it sit about 5,600
lines of document, panel, view and tools (`multiphysicsdoc.cpp`, `multiphysicspanel.cpp`,
`multiphysicsview.cpp`, `qucscontrol_fem.cpp`) and about 8,200 lines of `test_fem_*.cpp`.

| Layer | Now (2D) | What 3D needs |
|---|---|---|
| Geometry (`fem_geometry.cpp`, 1,915 lines) | Rectangles, circles, polygons, arcs and imported drawings; booleans on them. Its topology numbers domains, boundaries and points from the left, then from below. | Solids, faces, edges and vertices. Robust booleans on solids, extrusion and revolution. Selections by face as well as by edge. |
| Mesh (`fem_mesh.cpp`, 1,265 lines, on CDT) | Constrained Delaunay triangles, sized by domain, boundary, point and curvature, graded. Kept Delaunay for semiconductors: edges flipped, obtuse segments split. | Tetrahedra or prisms, sized the same ways, conforming to every interface. For semiconductors they must also be Delaunay, which is the hard part (below). |
| Elements (`fem_fe.cpp`, 262 lines; Nédélec in `fem_waves.cpp`) | Lagrange P1 and P2 on triangles, triangle and line rules, curved P2 edges. | P1 and P2 on tetrahedra or wedges, and their rules. Nédélec on tetrahedra for 3D waves and magnetics. |
| Physics (`fem_scalar.cpp`, `fem_semiconductor.cpp`, `fem_elastic.cpp`, `fem_magnetic.cpp`, `fem_waves.cpp`, ...) | Element loops over triangles. The semiconductor's box method weighs each edge by its cotangents (`fem_semiconductor.cpp:1019-1023`). Magnetics solves A out of the plane, a scalar. Waves are a cross-section's modes, or waves in the plane. | Element loops over 3D cells; the box method's weights from 3D Voronoi faces. 3D magnetics needs a vector A on edge elements and a gauge. 3D full-wave is a different and much heavier problem. |
| Solvers | Eigen's `SparseLU` everywhere (`fem_semiconductor.cpp:2074`, `fem_pde.cpp:754`, `fem_waves.cpp:531`). | A direct solver's fill-in grows much faster in 3D: a nested-dissection LU of N unknowns takes about N^1.5 operations in 2D but N^2 in 3D. A million 3D unknowns is beyond a laptop's direct solve. Iterative solvers with algebraic multigrid or ILU are needed beyond some 10^5 unknowns. |
| Results (`multiphysicsview.cpp`, 792 lines) | Painted triangles: surface, contour, arrow and mesh plots; line graphs; derived values. | Cut planes (each a 2D dataset, so every existing plot works on it), boundary surfaces in a rotatable 3D view, isosurfaces, picking faces for selections. |
| Model, file and tools | Domains, boundaries and points; a thickness for 2D; axisymmetric models. | A third level (faces apart from edges), the new geometry features in the panel, and Claude's tools for them. |

## What needs 3D, and what does not

- **FinFETs, gate-all-around nanosheets, trigates, SOI corner effects.** These are 3D
  because the gate wraps the channel. But they are built of boxes: each z-range is a 2D
  cross-section of rectangles. They are extruded and layered.
- **Interconnects, vias, TSVs, a pad's capacitance, a heater over a substrate.** These are
  layers of a layout, so they are extruded too. The layout viewer's *Cross-Section to
  Multiphysics…* already reads a layout cell and a layer stack. The same two inputs give a
  layered 3D model.
- **A photodiode's launch mix (finding 6).** This needs the silicon-to-germanium junction
  along the propagation, not a 3D field solve. Mode matching between the two 2D
  cross-sections answers it (stage 0).
- **Truly 3D shapes**: spheres, fillets, continuously varying tapers, bond wires. They need
  general solids (stage 2). They are the rare case among the devices this tool models.

## The ways

The first proposal's three ways (`2026-10-10-multiphysics-fem.md`) still stand:

1. **Drive an external engine.** Elmer (GPL; its solver library LGPL) and GetDP with Gmsh
   are general 3D field solvers. DEVSIM (Apache 2.0) is an open drift-diffusion TCAD that
   solves 1D, 2D and 3D structures and imports 3D meshes. This gives general 3D soonest.
   But the user installs them, results come back through files, and each feature is
   translated twice.
2. **Embed a 3D library**: MFEM (BSD), deal.II or NGSolve (LGPL). They are strong in 3D but
   heavy to build on three platforms, and each has its own model of the world.
3. **Our own engine, staged**: extruded 3D on what the 2D engine already does, and general
   solids later. This is the proposal.

## Stage 0: an eigenmode expansion across a junction (no 3D)

A mode junction step: the modes of two cross-sections, for example a silicon waveguide's
and the germanium photodiode's.

- The mode solver already finds them, full-vector and normalized to 1 W. The step takes
  their overlaps at the interface, ∫(E₁ × H₂)·ẑ dA, unconjugated so that lossy modes work.
- From the overlaps it solves for the transmitted and reflected amplitudes by mode matching
  (tangential E and H continuous).
- The optical generation is then fed the launched mix: each mode's power with its own α
  along the device, and the beat between modes as an option.

**What it answers:** how much of the waveguide's light goes into each of the photodiode's
modes. This is the 6 % in responsivity between modes that the review found. The same step
gives a butt coupling's loss (the published 10 µm photodiode's 0.5 A/W against our 1.23,
`aPublishedLateralPhotodiode`) and a taper built in steps.

**Tests:**
- Two identical sections pass everything in their own mode.
- A slab step's transmission matches the analytic overlap of its TE modes.
- Powers balance: transmitted + reflected + radiated (the unmatched remainder) = 1.

About 1,500 lines with tests, one to two weeks.

## Stage 1: extruded, layered 3D

**Geometry.** A 3D component is a stack of z-ranges. Each z-range has a 2D geometry: the
existing sequence of features, imports and layout cells. Its topology follows from them:
- domains: (2D domain, z-range);
- side faces: (2D boundary, z-range);
- top and bottom faces: (2D domain, z-interface);
- edges and points likewise.

A layout cell and a layer stack, as the cross-section tool already takes them, become a
3D model.

**Mesh.** One 2D mesh is built over the union of every z-range's shapes. CDT already takes
the constraints, so every layer's boundaries are imprinted. It is extruded through each
z-range in graded steps, giving prisms (6-node wedges).

Because the 2D mesh is Delaunay, each prism's Voronoi cell is the 2D cell times its
z-interval. Every box-method weight stays positive:
- an in-plane edge's weight is its cotangent weight times dz;
- a vertical edge's weight is its node's 2D cell area over dz.

The semiconductor then runs in 3D with all its models, as in 2D. A boundary-conforming
Delaunay tetrahedral mesh is a different matter: slivers give negative weights, and the
carriers oscillate (the reason `MeshSizes::delaunay` exists in 2D). That is the hard part
of 3D TCAD meshing, and prisms avoid it.

**Elements.** P1 and P2 on wedges. Their quadrature is the triangle rule times the line
rule, and both rules exist. Nédélec wedges come later.

**Physics, in order:**
1. Electrostatics, electric currents and heat (`fem_scalar.cpp`). This gives a 3D
   interconnect's capacitance matrix, a via's resistance and a heater's temperature.
2. Semiconductor drift-diffusion, with everything it has in 2D: the statistics,
   mobilities, recombination, impact ionization, tunnelling, traps, transient and
   small-signal. Its cotangent weights become prism face areas.
3. Solid mechanics: 3D elasticity, for thermal stress.

3D magnetics and 3D full-wave need edge elements in 3D, and are not in this stage.

**Solvers.**
- `SparseLU` stays for small models.
- Beyond about 2×10^5 unknowns: AMGCL (MIT, header-only, built for this) as the
  preconditioner of CG for the symmetric scalar problems. For drift-diffusion's
  nonsymmetric, badly scaled Jacobian: BiCGSTAB or GMRES with ILU or AMG on scaled rows,
  measured before it is trusted.
- A direct fallback when the iteration stalls.

**Results.**
- A cut plane (x, y, z or any plane) gives a 2D dataset, so every existing plot, line
  graph and derived value works on it unchanged.
- A 3D view of the boundary faces, coloured by a field, rotatable, and picking faces for
  selections. Qt's own OpenGL widgets would do it; the build does not link that module yet.
- Isosurfaces later.

**Tests:**
- **Invariance.** A model uniform in z, solved in 3D, equals the 2D model times its
  thickness, for each physics, to 10^-10. It is cheap and strong: it checks every weight
  and every face.
- **A cube's capacitance**, about 0.6607 × 4πε₀a (the known numerical value). A cube is
  extrudable; a sphere is not.
- **A FinFET** wide enough to be the 2D MOSFET per unit width.
- **A 3D diode** against the 1D one.
- **A TSV's capacitance** against its coaxial formula.

**What it opens:** FinFETs, nanosheets, trigates, SOI corner effects, a photodiode with its
contact fingers, a modulator's segmented electrodes, an EAM's taper in steps, and on-chip
resistors, vias and TSVs. Inductance needs 3D magnetics (stage 2).

About 10,000-14,000 lines with tests:
- geometry and topology about 2,000;
- mesh about 1,500;
- elements about 800;
- the scalar physics and semiconductor ports about 3,000;
- solvers about 1,000;
- 3D view and cut planes about 2,500;
- the model, panel and tools about 1,500.

Two to three months.

## Stage 2: general solids

- **Geometry kernel.**
  - Manifold (Apache 2.0) gives robust booleans on triangulated solids. A sphere is a fine
    triangulation, not a true surface.
  - OpenCASCADE (LGPL 2.1 with an exception) gives true curved surfaces and STEP import.
    It is very large and heavy to build on three platforms.
- **Mesher.**
  - Netgen (LGPL) as a library, or Gmsh (GPL 2 or later, with an exception) as a program
    found if installed, as ngspice is.
  - TetGen 1.5.1 and 1.6.0 are AGPLv3 (or a paid licence). Linking it would make Qucs-S's
    binaries AGPL, so I do not propose it.
- **Semiconductors on tetrahedra.** Positive box-method weights need a Delaunay mesh that
  conforms to every interface. A general mesher does not guarantee one: slivers have to be
  refined away, or the edge weights corrected. This is where the time goes.
- **3D magnetics** (vector A on Nédélec tetrahedra, with a gauge) and **3D full-wave**
  (Nédélec tetrahedra, PMLs, ports) are each a phase of their own.

Six months or more. Driving Elmer, Gmsh/GetDP or DEVSIM as external programs (way 1) gives
most of this sooner, at the price of files and installs.

## Recommendation

- **Stage 0 next.** It is small, and it answers a concrete question about a device already
  modelled.
- **Stage 1 when FinFET, nanosheet or interconnect work is wanted.** It covers most of what
  users draw here, from a layout and a layer stack, and keeps the semiconductor's box
  method sound.
- **Stage 2 only for a concrete need.** Until then, an external engine for the rare truly
  3D field problem.

## Decisions for you

1. **Stage 0:** build it now, or with stage 1?
2. **Stage 1's first physics:** the scalar ones and the semiconductor, as above, or another
   order? And should its results start with cut planes alone (sooner), or with the 3D view
   too?
3. **Stage 2:** a library inside, or an external program? Either way, no AGPL code.
