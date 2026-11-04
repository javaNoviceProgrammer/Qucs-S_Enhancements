# Multiphysics 3D: a user's tensor, Newton's method, parallel assembly, 4 November

Phase C's sixth and last part, after its estimates and memory limit (2026-11-04-multiphysics-3d-estimates). The
proposal's "a user's anisotropic tensor, Newton's method, parallel assembly".

## A user's anisotropic tensor in 3D

A physics' domain feature of the user's own value (heat's Solid, electrostatics' Charge Conservation, currents'
Current Conservation) takes a tensor in a 3D model as in a 2D one.

- **Anisotropy**: Diagonal or Symmetric, as well as Isotropic. A 3D model had Isotropic alone; its tensors were
  the plane's.
- **The keys**, in the model's axes (x, y, z):
  - kxx, kyy and, in 3D, kzz;
  - symmetric adds kxy and, in 3D, kxz and kyz;
  - the same with eps and sigma for electrostatics and currents.
- **The 3D solver** reads each component by its own key, its unit and parameters as the property's. A crystal's
  tensor is still its material's, turned by its orientation.

## Newton's method in 3D

A field that reads itself - a conductivity of T, a source of T, radiation's film - was solved by Picard's
iterations alone in 3D. A step's Nonlinear method is now Automatic, Newton or Picard, in 3D as in 2D.
Automatic and Newton take Newton's method.

- **Newton's matrix** is the system's and the derivatives' rest:
  - ∂(c ∇u)/∂u_j = c ∇φ_j + c' φ_j ∇u, where c is the coefficient, a tensor too;
  - −Q' φ_j, where Q is a source;
  - a film's h' u − g'.
  Each derivative is found numerically, by a perturbation of the field's value at the point of 1e-7 (|u| + 1),
  as the 2D solver finds them (`PointEvaluator3::perturb`).
- **It is not symmetric:**
  - Asked direct: an LU, Accelerate's or Eigen's.
  - Otherwise: BiCGStab with the same smoothed-aggregation multigrid, its smoother ILU(0). AMGCL reads
    compressed rows, so Newton's matrix is handed over by rows: Eigen keeps columns, and read as rows they are
    its transpose.
  - Where multigrid does not converge, an incomplete LU's BiCGStab takes over.
- **Each step** is (A + J) δ = b − A x from the field as it is. It stops at the step's tolerance in the relative
  change, as Picard's iterations stop. Each step is said ("Heat Transfer in Solids, Newton's step 3: relative
  change 2.1e-07").
- **In time** a capacity of T's own derivative is left out. Its steps are then an inexact Newton's, and converge
  linearly.

## Parallel assembly

A 3D physics' elements are assembled in parts on threads: one part for each 2,000 elements, at most 64 parts on
16 threads.

- **Each part keeps its own:**
  - an evaluator;
  - its triplets;
  - a record of what it adds to F, to the fixed rows' F and to the power made.
- **The parts are joined in the elements' order**, so the matrix and vectors are one thread's, bit for bit,
  whatever the parts.
- **The triplets are read in turn, not copied.** A forward iterator over the parts' lists hands them to Eigen's
  `setFromTriplets`. The assembly's memory is one thread's, as the estimates count it.
- **Newton's terms** are made in the same parts.
- **The faces' films** stay on one thread: there are far fewer of them.
- **A long lambda names its captures**: a `[&]` one crashed CI's GCC (d7ae967).
- **`setAssemblyParts()`** sets the parts, for a test.

## Validated

| Check | What it holds |
|---|---|
| A diagonal tensor | k 2, 5 and 7 W/(m K) along x, y and z: a cube held 10 K apart across each axis in turn; ∫ q dV / V that axis' -k ΔT / L and nought across the others, to 1e-6 (order 1, exact) |
| A symmetric tensor | kxx 3, kyy 4, kzz 5, kxy 1, kxz 0.5, kyz 0.25 W/(m K); T = 300 K + 1e4 K/m x held on every face: q at a point -K ∇T = (-3e4, -1e4, -5e3) W/m², its cross flows, to 1e-6 |
| Newton against Kirchhoff | k = 1 W/(m K) (1 + 0.01 (T - 300 K)) along a bar held at 300 and 400 K (order 2): T at its middle 358.11388 K within 2 mK; its flux 1.5e5 W/m² within 0.1 %; the same field by Newton (5 steps) and Picard (9 and 11) to 1e-7, multigrid's BiCGStab and an LU alike |
| Radiation's film | A slab's top radiating at ε 0.8 to 290 K, a source through it: the quartic's top within 1e-7 of its bisection, by Newton in fewer steps than Picard's to the same temperature |
| Multigrid not symmetric | Convection and diffusion in 1D, 5,000 unknowns: BiCGStab with multigrid to a residual under 1e-8, no fallback |
| Parallel assembly | A source of x and y in a cube, k of T (Newton's steps): its 15,776 prisms in 1 part, 13 parts and by the size (7): the field and ht.Tmax the same, bit for bit |
| The tab | A 3D step offers Newton and Picard; a user's solid offers Isotropic, Diagonal and Symmetric, and with Symmetric kzz, kxz and kyz |

**Speed.** At 1,052,215 unknowns of order 1 (multigrid), measured while another session's sanitized tests ran
(load average 26):

- one part: the solve took 7.1 to 8.2 s, with a peak footprint of 2.5 GB;
- in parts: 3.6 to 5.0 s, with a peak of 1.8 GB.

The single part's records of what it adds grow in one vector, by doubling. At order 2, 338,837 unknowns, the
difference was within the runs' noise: multigrid's solve is most of it there.

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_solve` `usersTensor`, `newtonsMethod`, `parallelAssembly` (new) | The checks above |
| `test_fem3_solve` `filmAndRadiation` (changed) | By Newton now, and against Picard's iterations; `refusals` no longer refuses a user's tensor |
| `test_multiphysics3d_doc` `physicsThroughThePanel` (changed) | The tab's choices above |
| `test_multiphysics3d_doc` `theToolsOnA3DModel` (changed) | Several named expressions at a point and over a face (below) |
| The full ctest | 161 tests at eight at once, in the worktree of 4b238d6 with this change alone: 160 passed. `test_large_schematics`' drag-all ratio (52 ms for 2,000 resistors, 629 for 8,000) failed under a load average of 36, and passes alone (13/13) |

## Found on the way

- **fem_evaluate on a 3D model took one expression at points, along a line or over a selection.** The DMG's
  drive, run first against the build tree, asked for `ht.qx; ht.qy; ht.qz` at a point, as the plane's tool
  takes it, and was refused ("unexpected ';'").
  - It takes several now, split by ';', each named if wanted, at each point, along a line and over a selection.
  - `theToolsOnA3DModel` holds "T; rise = T - 300[K]" at a point (301.25 and 1.25 K, the second named rise) and
    "T; 2*T" over the block's top (302.5 and 605 K).
- **The Newton test's bar took 192 s under ASan here**, past what CI's sanitized Linux gives a function (its
  Eigen solvers slower still). A sanitized build now solves a coarser bar (0.1 mm) in three of the six ways,
  its Kirchhoff tolerance 20 mK.
- **The parallel assembly's cube is coarser there too, but not by much.** At 4,836 prisms (2,712 equations) it
  took longer than at 15,776: 1.2 s in Release against 0.39 s, and 61 s under ASan.
  - Below AMGCL's 3,000 unknowns the whole matrix is multigrid's coarsest level, factorized by its own LU.
  - Newton sets that up again at each of its five steps.
  - At 7,319 prisms (4,095 equations) it took 0.15 s, and 18 s under ASan with Eigen's solvers.
  This is phase C's multigrid setting, not this change; whether a lower coarse size suits every 3D model is a
  question for its own measurements.

## Broken on purpose

`docs/feature_gaps/2026-11-04-multiphysics-3d-newton/breaks.py` holds 15 breaks. It ran in a detached worktree
of 4b238d6 with this change alone:

- **A user's tensor:**
  - read by its isotropic key;
  - its xz read as its xy;
  - its zz the plane's alone;
  - a diagonal tensor's cross terms read too.
- **Newton's method:**
  - the coefficient's derivative left out;
  - its term transposed;
  - a film's derivative left out;
  - the residual's sign turned;
  - Newton's matrix without its derivatives;
  - multigrid reading it by its columns;
  - Newton's matrix by conjugate gradients;
  - Picard's iterations whatever is asked.
- **Parallel assembly:**
  - the parts' records joined from the last;
  - a part's last element left out;
  - the faces' triplets left out of the matrix.

13 were caught at the first run. Two needed a second look:

- **"its zz the plane's alone"** was caught by the panel's test, as meant. It was listed against the solver's test
  as well, which reads a key set by hand either way; it lists the panel's alone now.
- **"a diagonal tensor's cross terms read too"** passed. The test's diagonal tensor had no cross terms to read,
  each left at its default 0. It now sets a symmetric tensor's left-over kxy, kxz and kyz of 1 W/(m K), which a
  diagonal one must not read, and the break is caught.

The worktree was rebuilt after the breaks. The four lints are green, and nothing was written to the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan, each function alone (and with QUCS_FEM_SOLVER=eigen, as CI's Linux solves):

| Function | Time | With Eigen's solvers |
|---|---|---|
| `test_fem3_solve` `usersTensor` | 0.2 s | |
| `test_fem3_solve` `newtonsMethod` | 24.1 s | 15.7 s |
| `test_fem3_solve` `parallelAssembly` | 18 s (0.08 mm) | 18.1 s |
| `test_fem3_solve` `filmAndRadiation`, `refusals`, `estimates` | 0.1 to 0.9 s | |
| `test_multiphysics3d_doc` `theToolsOnA3DModel`, `physicsThroughThePanel` | 11 s together | |

No sanitizer said anything.

## What is left

Phase C's list is done. Still open:

- **In time, Newton's capacity derivative**: a ρ Cp of T's own makes its steps an inexact Newton's.
- **fem_converge on 3D models.**
- **VTU's binary form.**
- **Transparency in the 3D view.**
- **Cut planes, lines and points as datasets**: phase D's first pieces.
- **Tetrahedra** stay phase F.
- **Decisions 4** (Gmsh with OpenCASCADE) **and 7** (the METIS ordering for 2D models) are the user's.
