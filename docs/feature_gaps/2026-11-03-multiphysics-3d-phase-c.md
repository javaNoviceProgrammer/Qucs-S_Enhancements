# Multiphysics 3D phase C begun, 3 November: heat, electrostatics and currents solved in 3D

The request: "let's move on to the next phase of implementation", after phase B's first part (58c0fd7, fcd2457).
Asked which phase, the answers were:

- **Phase C next:** heat, electrostatics and currents in 3D, before phase B's leftovers (the layout feature through
  RF 2.5D's processing, Extrude 2D Model, wall angles, picking, the GPU view) and before phase A.
- **AMGCL may be downloaded** and vendored.
- **Decision 6, a direct solver off macOS: iterate only.** No SuiteSparse.
- **Decision 8, the validation devices:** the heater over a ring, a TSV, a VCSEL array and the photodiode with
  fingers, against published results.

This is phase C's first part: the solver, its first validations, and the window's Compute. What phase C still
lacks is listed at the end.

## Multigrid, measured first

The proposal's benchmark again: the P1 Laplacian of an n × n × n grid of cubes, each cut into Kuhn's six
tetrahedra, the bottom held, solved to a relative residual of 1e-9 (M2 Ultra, one thread for AMGCL, which has no
OpenMP here). "Contrast" puts a block of k = 400 (copper) in k = 1.4 (oxide), a ratio of 285.
`docs/feature_gaps/2026-11-03-multiphysics-3d-phase-c/amg_bench.cpp` is the program, `amg_bench.tsv` every run.

| Unknowns | AMG, SPAI-0 | AMG, ILU(0) | CG + incomplete Cholesky | Cholesky, METIS | Cholesky, the tree's order |
|---|---|---|---|---|---|
| 28,830 | 0.05 s, 17 it | 0.06 s, 11 it | 0.10 s, 92 it | 0.18 s, 94 MB | 0.07 s, 129 MB |
| 130,050 | 0.26 s, 16 it | 0.30 s, 12 it | 0.63 s, 147 it | 1.0 s, 634 MB | 1.6 s, 1.1 GB |
| 524,880 | 1.09 s, 21 it | 1.21 s, 13 it | 4.0 s, 238 it | 9.2 s, 4.2 GB | 33 s, 9.7 GB |
| 1,020,100 | 1.89 s, 21 it | 2.19 s, 15 it | 9.8 s, 307 it | 31.7 s, 7.9 GB | (the proposal's: 113 s, 24 GB) |
| 1,020,100, contrast | 2.17 s, 23 it | 2.38 s, 15 it | 11.6 s, 368 it | 31.5 s, 7.9 GB | |

Memory for the iterative solves, the hierarchy or the incomplete factor over the matrix, is 0.66 GB (incomplete
Cholesky), 0.88 GB (SPAI-0) and 1.05 GB (ILU(0)) at a million unknowns. What follows:

- **Algebraic multigrid's iterations hardly grow** with the mesh, from 11 to 15 with ILU(0) and from 16 to 23
  with SPAI-0, against the incomplete Cholesky's 92 to 368. Copper beside oxide changes them by two at most.
- **For one right-hand side it is quicker than either factorization at every size:** 0.06 s at 28,830 unknowns
  against 0.07 s (the tree's order) and 0.18 s (METIS); 2.2 s at a million against 32 s.
- **A factorization pays only where many right-hand sides share one matrix:** past about 6 of them at 28,830
  unknowns, 9 at 130,050, 17 at 270,400 and some 80 at a million (against METIS's order).
- **The smoother is ILU(0)**, chosen on the chip heater below rather than on the benchmark. Its thin films make
  long, flat prisms, and order 2's matrices are no M-matrices. At 160,357 order-2 unknowns (three Picard solves),
  SPAI-0 ran past 500 iterations in two of the three, the incomplete Cholesky finishing them, 35 s in all;
  Gauss-Seidel took 146 iterations in 6.5 s; ILU(0) took 52 in 5.6 s. On the plain benchmark ILU(0) is also the
  fewest iterations, and 15% slower than SPAI-0.

So 3D solves by conjugate gradients with AMGCL's smoothed aggregation and ILU(0) everywhere: "auto". On macOS,
where more right-hand sides than about 6 + n/20,000 share a matrix of no more than 300,000 unknowns (a sweep of
many terminals), Accelerate's Cholesky in METIS's order. "Direct" asks for the Cholesky, which is Eigen's off macOS, as
decision 6 has it; "iterative" asks for multigrid. A solve multigrid cannot finish in 500 iterations goes on by the
incomplete Cholesky's, and the log says so. The METIS ordering is a new choice of `SparseCholesky`
(`setOrdering`), used by 3D alone; the 2D models are as they were (decision 7 still open).

AMGCL 1.5.0 (3 August 2026, MIT) is in `third_party/amgcl`: its `amgcl/` headers, all but the MPI ones, its
licence and our README and CMake file. The archive was 3.0 MB, not the 1 MB I guessed when asking: GitHub gives no
size for a generated archive.

## The elements

`fem3_fe.{h,cpp}`:

- **Lagrange prisms of order 1 (6 nodes) and 2 (18)**: the triangle's functions of that order times the line's
  along z, the 18-node one the tensor product of the quadratic triangle and the quadratic line.
- **Rules**: Dunavant's on the triangle times Gauss's along z.
- **`Space3`**: the degrees of freedom of a swept mesh.
  - Order 1's are the mesh's nodes as they are numbered.
  - Order 2's are the plan's 2D order-2 space at every height and every layer's middle: the mesh's nodes first,
    numbered as they are, then the rest (edges' middles, vertical edges' middles, walls' centres) as the prisms
    meet them.
- **The map**: a prism's map is its triangle's times the line between its heights. Its Jacobian is block
  diagonal, and an order 2 triangle curved on an arc (a cylinder's wall) keeps the 2D space's curved map.
- **Faces**: a mesh face's points come from the face itself, a floor's triangle or a wall's edge times its height,
  each with the prism beside it on the side asked for, so a condition that reads the field reads it there. The
  swept mesh's faces now record their plan triangle or edge and their height.

## The physics

`fem3_solver.{h,cpp}`, `fem3_scalar.cpp`, `fem3_problem.h`:

- **Heat, electrostatics and electric currents**, with the 2D physics' own kinds, features and properties, now of
  every space. The coefficient is the feature's (isotropic) or the material's: one value, or a crystal's tensor
  turned by its orientation and then into a 3D model's axes. In 2D a wafer's normal is y; in 3D it is z, and along
  is y, so a tensor turns 90° about x (T₃ = M T₂ Mᵀ). A user's tensor of the plane's form is refused in 3D, said.
- **Sources**: a space charge, a current source, a heat source as a density or as a total power spread over its
  domains' volume.
- **Faces**:
  - Ground, potential and temperature held.
  - Terminals: at a voltage, or floating, of a charge or a current fed.
  - Zero charge and insulation.
  - Surface charge, normal current, a heat flux, a boundary heat source.
  - A convective film h(Text − T).
  - Surface-to-ambient radiation, a film of T's own.
- **Terminals**: the charge or current of each, by reactions. Each at 1 V in turn gives the capacitance or
  conductance matrix: one matrix, a right-hand side a terminal.
- **Globals**: as 2D's: `es.V_<n>`, `es.Q_<n>`, `es.C11`, `es.C_a_b`, `es.W`, `ec.I_<n>`, `ec.G..`, `ec.R` with
  the resistance matrix, `ec.P`, `ht.Tmax`, `ht.Tmin`, `ht.P`, `ht.Rth`.
- **Variables**: x, y, z, t and dom, then each field's own (V, T), with z added to 2D's:
  - es and ec: E's three components and its norm; es D and its norm, We, epsr.
  - ec: J's three components and its norm, Qrh, sigma.
  - ht: gradT's components and norm, q's components and norm, k.
- **Coupling**: a coefficient or a source that reads a field (silicon's k(T), a heat source of ec.Qrh) is solved
  by Picard's iterations within its physics, and in turn between physics, until the relative change is under the
  step's tolerance.
- **The study**: Stationary steps, each its chosen physics. A step of another kind, and a physics of the plane's
  alone (in a file: `add()` refuses one), is refused, said.

**A linear prism is lumped.** A thin layer's prisms, far wider than tall, couple neighbours in the plane the wrong
way: the vertical term's in-plane factor, the triangle's consistent mass, has positive off-diagonals. A field then
undershoots where it falls steeply in z. The chip heater's oxide top, far from its cold back, came out 0.72 K below
the 293.15 K held there, 2.7 % of the rise, at order 1 (order 2 had none). Prisms far taller than wide do the same
between a column's two levels, through the in-plane term's factor along z: a one-layer slab over a bottom split
between 300 and 400 K fell to 277.2 K, 23 K under its bound, with its top insulated. Order 1 now lumps the vertical term's
factor in the plane and the in-plane term's along z, each to its row's sum. The tensor's cross terms stay as they
are. A linear field and a field uniform in z stay exact, and the error's rate stays 2. On the chip, every
temperature is then at or above its back's. Multigrid (SPAI-0's, then) went from 478 iterations over the three
Picard solves to 22, since an M-matrix is what smoothed aggregation is made for.

As the chip is refined, order 1 (lumped) and order 2 close on one answer from below. Tmax rises by 28 to 29 K:

| Mesh | Order 1 | Order 2 |
|---|---|---|
| The model's preset | 318.41 K | 321.88 K |
| hmax 3 µm, 8 divisions in the substrate | 320.17 K | 322.25 K |
| hmax 1.5 µm, 16 | 321.31 K | 322.48 K |

## The window

- **Compute**: Multiphysics 3D > Compute, the tab's Compute and F2 solve the model's first study in the
  background. The geometry and the mesh are built first where they are stale, then solved, with the bar, the stage
  and Cancel.
- **The solution shown**: Show > Solution. Its faces are coloured by Plot, an editable box of each field's
  variables (ht.T, ht.normq, es.V, es.normE, es.normD, ec.V, ec.normJ, ec.Qrh, ec.normE), or any expression of the
  solution's variables and globals. The values at the mesh's nodes are each prism's at its corner, averaged.
- **The cut**: where the view is cut (x, y or z), the slice of the solution there is drawn, exactly: a layer's
  triangles across z, each prism's cut swept between its heights across x or y.
- **The legend**: a colour bar at the right with the expression, its unit and its least and greatest values,
  drawn in the tab and in its pictures. The view's triangles can carry a colour at each corner, blended across.
- **The messages**: the solver's lines (unknowns, method, iterations, residual), the Picard and coupling
  iterations, and every global with what it is.
- **Staleness**: an edit leaves the solution stale. It is still shown, with "the solution is of the model before
  its last change: Compute again" beside the bar.
- **The panel**: the tree shows the study and its steps. The Add menu offers heat, electrostatics and currents
  on the component, and their features. A step's Physics is a list to check, and its properties of the plane's
  alone (adaptive refinement, the light loop) are not shown. Newton is not among its nonlinear methods, since 3D
  iterates by Picard. A user's anisotropy offers isotropic alone.
- **New models**: a new 3D model has a study with a Stationary step. A 3D physics is made of order 1, and read as
  order 1 where its file gives none. Order 2 is its choice.
- **The example**: the chip heater now solves its 10 mW heater, the substrate's back held at 20 °C. Tmax is
  318.4 K at its preset mesh, and its Rth 2526 K/W.

## Validated

| Check | What it holds |
|---|---|
| Patch tests | A linear temperature held on the faces of two boxes is order 1's exactly; a quadratic one with its source is order 2's, at every degree of freedom |
| Convergence | sin·sin·sin in a cube: the L2 error falls as h² (order 1, 0.1 to 0.05 mm) and h³ (order 2) |
| Uniform in z | Two materials side by side, a source, a held wall and a film, top and bottom insulated: equal to the 2D model of its plan, on the 3D model's own plan mesh, times its thickness, to 1e-9 - every degree of freedom's temperature, potential and current's potential, and ht.Tmax, ht.P, es.C11, es.C12, es.C22, es.Q_1, es.W, ec.G11, ec.G12, ec.R, ec.I_2, ec.P - at order 1 and 2 |
| A TSV's oxide liner | The coaxial capacitance 2π ε0 εr h / ln(b/a), the oxide capacitance of G. Katti, M. Stucchi, K. De Meyer and W. Dehaene (IEEE Trans. Electron Devices 57, 256-262, 2010): order 2 on its arcs within 0.1 %; order 1's polygon within 5 % and further off |
| Terminals | A bar's resistance L/(σA) and its current; E from the terminal to the ground along +x, J = σE; three terminals' capacitance matrix symmetric, its diagonal positive and the rest negative; a space charge's terminal holding -ρV exactly (Gauss's law in discrete form); a floating conductor between two at half the volt |
| Joule heating | A bar's rise σ(V/L)² L²/(8k) in its middle, order 2 exactly, ec and ht in turn, its heat its power |
| A film, radiation | A slab held below with a film above: its parabola exactly; radiating instead, the 1D quartic's root |
| A crystal | k of 1; 2; 3 along its axes: in 3D its x2 (a wafer's normal) is z and its x3 is y, the flux each way says so |
| Thin and tall prisms | A film heated from a pad above, its back held: no temperature under its back's; a tall slab of one layer over a split bottom: every temperature between its two held ones (lumped order 1) |
| Slices | Across x, y and z of a linear field: each corner on the plane, each value the field's, the section's area |
| Multigrid | A 3D Laplacian to its tolerance, as a direct solve has it, its iterations hardly growing, copper beside oxide alike; too few iterations, the fallback, said |

The other three devices, and the TSV's resistance and thermal resistance, are not yet held to a publication.

## Tests

| Test | What it holds |
|---|---|
| `test_fem_amg` (new, 2 functions) | Multigrid's solves, its iterations and its fallback |
| `test_fem3_fe` (new, 4 functions) | The prisms' shapes (each one at its node, together one, their slopes), the rules' exactness, the spaces (order 2's count and each prism's nodes where its map puts them), the faces' points (a box's area, a cylinder's wall 2πrh within 1e-4 at order 2 and order 1's polygon short of it) |
| `test_fem3_solve` (new, 13 functions) | The validations above, and what is refused: a field held nowhere, a domain without its material, a user's tensor of the plane, a step in time, a physics of the plane's alone |
| `test_multiphysics3d_doc` (2 new functions, others changed) | The chip heater computed in its tab and by F2: its solution, legend, plots, slice, staleness and its rise as twice the power gives it (a little more than twice: silicon's k falls as it warms); physics through the panel; a shaded triangle blended; a new model's study |
| `test_fem3_geometry` `schema` | A 3D model's component takes heat, electrostatics and currents alone, its root a study; order 1, made and read |

## Broken on purpose

`docs/feature_gaps/2026-11-03-multiphysics-3d-phase-c/breaks.py` holds forty-nine:

- **The elements:**
  - order 2's middle function along z halved;
  - order 1's derivative along z turned;
  - a rule's line weight dropped;
  - order 2's layer middles at the layer's bottom;
  - the map's z its top's alone, and its Jacobian without the height;
  - a wall's area without its edge's length;
  - a floor's prism below read at its bottom;
  - a wall's edge run one way in every triangle;
  - a wall's height one layer low.
- **The physics:**
  - each of the two lumpings made consistent;
  - ε0 dropped;
  - a film's h not in the matrix;
  - a face's flux not added;
  - a held value read with x and y swapped;
  - a floating group's dofs each their own;
  - a reaction with its sources added;
  - the sweep's other terminals at 1 V too;
  - Picard's iterations one;
  - a crystal's axes the plane's;
  - a total power not spread by volume;
  - a field held nowhere solved;
  - a user's plane tensor taken.
- **The study and variables:** coupled physics one round; Joule heat J alone; E as +∇V; a point's ζ not scaled
  by its layer's height; a step of another kind solved.
- **Results:** a z slice read at its layer's bottom; an x slice's quadrilateral its bottom twice; nodal values
  summed.
- **Multigrid:** its tolerance not set; no fallback; a direct solve asked and multigrid given.
- **The window:**
  - a solution not kept, and left fresh after an edit;
  - the legend never shown;
  - the cut's slice not drawn;
  - F2 not computing;
  - the panel showing the plane's properties, and hiding the study;
  - the view's colours not blended.
- **The schema:**
  - the scalar physics the plane's alone;
  - a 3D physics made of order 2, and read without its order;
  - the plane's tensors offered;
  - a step's adaptive refinement shown;
  - a new model without a study.

All forty-nine are caught. Four needed a second look after their first run:

- **The map's Jacobian without its height:** caught by the spaces test. The patch tests have layers alike in
  height, and a scale alike in every element leaves their solution as it is, so the break lists the first alone.
- **A wall's height one layer low:** its points and its area agreed with each other, wrong together. The face
  test now holds each face's points between its own nodes, and its order 1 dofs to be its nodes.
- **A point's layer one too high:** not a fault. The search looks at the layers beside it and found the point
  anyway. It was replaced by a point's ζ not scaled by its layer's height, caught by the film.
- **A linear prism's in-plane term consistent along z:** no test had tall prisms, and a tall slab's first test,
  its top held at 300 K, could not see it. Where a column's top nodes are all one value, the couplings of the wrong
  sign cancel against the rest. A tall slab of one layer, its bottom at 300 K but for a patch at 400 K and its top
  insulated, can: consistent, its top fell to 277.2 K, 23 K under the bound; lumped, every temperature is between
  300 and 400 K.

Rebuilt after the breaks: the full ctest, 159 tests, all passed in 375 s at eight at once (a run before the last
fixes had `test_claude_code`'s `aReaderScrolledUpStaysPut` 2 px off under the load, as before, passing alone). The
four lints green; nothing written to the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan, as CI's Linux job is, each function alone (the process's start included):

| Function | Time |
|---|---|
| test_fem_amg `solvesALaplacian` (35,000 unknowns, both cases) | 19.3 s |
| test_fem_amg `fallsBack` | 3.1 s |
| test_fem3_fe `prismShapes`, `prismRules`, `spaces`, `facePoints` | 0.3 to 1.4 s |
| test_fem3_solve `convergence` | 8.8 s |
| test_fem3_solve `coaxialLiner` (order 2 on 0.05 mm) | 16.3 s |
| test_fem3_solve `patchTests`, `invariantInZ` (2D's solves too) | 1.7, 1.1 s |
| test_fem3_solve `terminals`, `jouleHeating`, `filmAndRadiation`, `crystalTensor`, `iterativeAndDirect`, `thinLayersStayWithinBounds`, `tallPrismsStayWithinBounds`, `slices`, `refusals` | 0.3 to 0.7 s each |
| test_fem3_geometry `schema` | 1.4 s |
| test_multiphysics3d_doc `aModelSolvedInItsTab` (the chip solved twice) | 30.2 s |
| test_multiphysics3d_doc `aNewModel`, `theExamples`, `physicsThroughThePanel`, `theView` | 1.7 to 4.9 s |

No sanitizer said anything.

## What phase C still lacks

- **The `fem_*` tools on 3D models** (`fem_model` new and open of a `.qfem3d`, `fem_build`, `fem_solve`,
  `fem_evaluate`, `fem_plot`). Claude opens, lists, pictures and saves one meanwhile, and the 2D tools refuse it.
- **Time**: heat in time (BDF) and ρCp; a sweep.
- **Results nodes**: 3D Plot Groups (Surface, Slice, Isosurface, Clip, Arrow Volume when asked), 3D derived values
  (integrals over volumes, faces and edges, values at points), VTU export, Field on a Grid in 3D, Interpolation of
  three arguments. A solution is shown through the tab's Show, Plot and cut meanwhile.
- **Estimates and a limit** before a solve.
- **Validation against publications**: the heater over a ring, a VCSEL array's crosstalk, the photodiode's finger
  capacitance, and a TSV's resistance and thermal resistance.
- Tetrahedra (Gmsh's, phase F).
- A user's anisotropic tensor in 3D.
- Newton's method in 3D.
- Parallel assembly.
- **Phase B's leftovers**: the layout feature through RF 2.5D's processing, Extrude 2D Model, wall angles,
  picking, the GPU view.
