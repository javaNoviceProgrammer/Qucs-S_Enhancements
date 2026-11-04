# Multiphysics 3D: estimates before a solve, and a memory limit, 4 November

Phase C's fifth part, after Claude's tools on 3D models (2026-11-04-multiphysics-3d-tools). The proposal's "Sizes
said before a solve":

- Build Mesh reports, for each physics, the unknowns, the matrix's nonzeros, and the estimated memory and time of
  a direct and of an iterative solve.
- Compute refuses a solve past a limit (Multiphysics 3D > Settings…; by default half of this machine's memory),
  and says how to get under it: the iterative solver, a coarser mesh, or fewer divisions in z.
- `fem_build` answers the estimate; `fem_solve` says it before it starts and refuses past the limit with what
  would fit, takes `solver` (direct, iterative) for this solve alone, and `force` to go past the limit.

## What is estimated

`fem3_estimate.{h,cpp}`, for each enabled physics on its order's space:

- **Unknowns**: the space's degrees of freedom (the fixed ones among them).
- **Nonzeros**: the pairs of degrees of freedom that share a prism, counted, not guessed. Each row is sorted and
  counted once, its rows in chunks on threads: 0.12 s for a million unknowns (0.9 s on one thread).
- **A solve's peak**: the larger of its assembly and its solve.
  - The assembly is the elements' triplets (16 bytes each, elements × per element²) and the matrix made of
    them twice.
  - The solve is the matrix with the solver's own part:
    - multigrid's hierarchy and the rest, 85 bytes a nonzero;
    - the Cholesky's factor, 74.3 bytes × n^1.248 × (its nonzeros a row)^0.480.
- **Its time**:
  - assembly 0.05 s + 25 ns an element-matrix entry;
  - multigrid 0.15 µs a nonzero;
  - the Cholesky 0.25 s + 2.14e-9 s × n^1.387 × (nonzeros a row)^1.292.
- **How it would be solved**:
  - `iterative`;
  - `direct` (asked);
  - `auto`, where the solver may factorize a matrix of many right-hand sides (no more than 300,000 unknowns, on
    macOS). Auto's peak is the larger of the two.
- **A study's peak** is its largest physics'. Each physics solves alone and lets go of its system (`System3` is
  local to a solve).

The Cholesky's factor grows with the rows and, more slowly, with a row's nonzeros. Order 2's rows are twice as
dense as order 1's (43 nonzeros a row against 20), so it fills more for the same unknowns. A fit in the
nonzeros alone, phase C's, put order 2's factor at twice what it is.

## Fitted, and held to what was measured

`docs/feature_gaps/2026-11-04-multiphysics-3d-estimates/estimate_probe.cpp` solves a 1 mm box of heat with a
source through it and its bottom held. Each run is one size, order and solver in a process of its own, built
against the engine. It prints the estimate, the seconds, and the peak footprint over the footprint before
(sampled every 5 ms). It ran on the M2 Ultra while other sessions worked; `estimate_probe.tsv` beside it holds
each run.

The fits combine those solves with phase C's benchmark of the solvers alone (order-1 Laplacians to 1,020,100
unknowns). The estimates against the measured, after fitting:

| Order, solver | Unknowns | Nonzeros | Memory, estimated / measured | Time, estimated / measured |
|---|---|---|---|---|
| 1, multigrid | 44,070 | 881,496 | 84 / 86 MB | 0.26 / 0.19 s |
| 1, multigrid | 119,187 | 2,415,765 | 229 / 236 MB | 0.62 / 0.57 s |
| 1, multigrid | 309,852 | 6,342,570 | 602 / 615 MB | 1.54 / 1.61 s |
| 1, multigrid | 1,052,215 | 21,733,965 | 2,063 / 2,126 MB | 5.17 / 4.56 s |
| 1, Cholesky | 44,070 | 881,496 | 200 / 170 MB | 0.66 / 0.39 s |
| 1, Cholesky | 119,187 | 2,415,765 | 686 / 595 MB | 1.66 / 1.22 s |
| 1, Cholesky | 309,852 | 6,342,570 | 2,250 / 1,942 MB | 5.23 / 4.02 s |
| 2, multigrid | 46,531 | 2,029,291 | 190 / 181 MB | 0.44 / 0.35 s |
| 2, multigrid | 114,431 | 5,052,341 | 473 / 468 MB | 1.03 / 0.93 s |
| 2, multigrid | 338,837 | 15,158,747 | 1,419 / 1,401 MB | 3.00 / 3.28 s |
| 2, Cholesky | 46,531 | 2,029,291 | 317 / 334 MB | 1.23 / 0.70 s |
| 2, Cholesky | 114,431 | 5,052,341 | 965 / 956 MB | 3.50 / 1.99 s |

What follows from it:

- **Memory is what the limit is held to, and it is close:** multigrid's within 5 % at every size to a million
  unknowns, the Cholesky's within 18 % (over, the safe side). Below some 100,000 unknowns a solve is under a
  few hundred MB and the limit does not come into it.
- **Time is said, not held to:**
  - multigrid's within 11 % past 100,000 unknowns;
  - the Cholesky's over by up to 1.8 times. Its fixed cost (threads, the METIS ordering) varied from run to run
    under the machine's other work.
- **Multigrid's own memory is 85 bytes a nonzero in the app's solves**, 80 to 88 at orders 1 and 2. Phase C's
  benchmark of the solver alone had 74.

## The tab and the tools

- **Build Mesh** (the menu's, the tab's) says each physics' estimate after the mesh's line, and whether it is
  within the limit:
  - "Estimated: ht (order 1): 309,852 unknowns, 6,342,570 nonzeros - by multigrid some 602 MB and 2 s, by a
    direct solve some 2.2 GB and 5 s";
  - "Within the memory limit of 32 GB (Multiphysics 3D > Settings)" (half of this machine's 64 GB), or what would bring it under.
  `estimates()` keeps it.
- **Compute** says each estimate first in the solve's log. Past the limit it is not solved: "Not solved: it
  would take some 2.2 GB, past the limit of 1.0 GB (Multiphysics 3D > Settings): the iterative solver (some
  602 MB: fem_solve's solver iterative, or the step's Solver); or a coarser mesh - 45 % of the unknowns, or
  fewer - or fewer divisions in z (the mesh's Distribution in z). fem_solve's force solves it all the same."
  The iterative solver is offered only where it would fit.
- **Multiphysics 3D > Settings…** (new): the memory a solve may take, in GB.
  - By default it is half of this machine's memory (`hw.memsize`, `sysconf`, `GlobalMemoryStatusEx`).
  - 0 means no limit, and a button sets half the machine's memory again.
  - The setting is `Multiphysics3D/memoryLimitGB`. The dialog is opened, not run: the window goes on behind it.
- **fem_build** (mesh) answers each physics' estimate (`estimates`: its unknowns, nonzeros, the memory and time
  of each solver, how it is solved), the `memory limit`, and `past the limit` with what would fit.
- **fem_solve** answers the estimates it began with.
  - `force`: solved past the limit, said.
  - `solver` (auto, direct, iterative): for this solve alone, the model left as it is.
  - A 2D model's fem_solve refuses both: its direct solver is Multiphysics > Settings', and it has no limit.
- **A refused solve and an earlier solution.** A solve refused past the limit left the model's earlier solution
  in place. `fem_solve` read that solution as its answer, as if it had solved. It reads this call's "Not solved"
  first now.

## Validated

| Check | What it holds |
|---|---|
| The count | A brute count of the pairs that share a prism, orders 1 and 2: the same nonzeros, on one thread and on seven |
| The fits | The 13 direct solves measured (the app's of orders 1 and 2, phase C's to a million unknowns): memory within 18 %, time within a factor of 2.1; the 5 multigrid solves of the app's past 40,000 unknowns: memory within 7 % |
| A mesh's estimate the solve's | `estimate3` on a mesh and the solve's estimate: the same unknowns, nonzeros and peak; said first in its log |
| Orders | An order-2 physics beside an order-1 one: each on its own space, its unknowns the order-2 space's, the study's peak the larger |
| The limit | At half the estimate: not solved, the limit and a coarser mesh said; forced: solved, said |
| A solver for one solve | Direct: a Cholesky in METIS's order; iterative: multigrid |
| What would fit | A direct solve of 10 GB past a 2 GB limit, multigrid 1 GB: the iterative solver offered; under the limit, or none: nothing said |
| The tab | Build Mesh says the estimate and the limit; its unknowns the mesh's nodes; fem_build gives the same; the Settings' value half the machine's, OK keeps 3.5 GB, Cancel keeps it, the button half again; fem_solve within, past (refused, not answered with the solution before), forced, direct, a solver that is none refused; a 2D model's fem_solve refusing both |

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_solve` `estimates` (new) | The count, the fits, the solve's estimate, orders, the limit, force, a solver for one solve, what would fit |
| `test_multiphysics3d_doc` `estimatesAndTheLimit` (new) | The tab, its Settings and the tools |
| The full ctest | 161 tests at eight at once, in a detached worktree of 501af64 with this change alone: all passed |

## Broken on purpose

`docs/feature_gaps/2026-11-04-multiphysics-3d-estimates/breaks.py` holds 20 breaks. It ran in the worktree, so
the other session building from the checkout never built a broken source:

- **The count:**
  - each prism's pairs counted, not each row's once;
  - a row's own degree of freedom left out;
  - each thread's chunk short of its last row;
  - one space for every order;
  - a study's peak its physics' sum.
- **The fits:**
  - multigrid's memory a nonzero halved;
  - the Cholesky's factor of its rows alone, not their nonzeros.
- **The limit:**
  - the limit not consulted;
  - forced, refused all the same;
  - a solver for this solve alone not taken;
  - the iterative solver not offered where it would fit;
  - past the limit when under it.
- **The tab and the tools:**
  - Build Mesh's estimate not said;
  - the limit not given to the job;
  - force not given to the job;
  - the limit's default all of the memory;
  - the Settings' OK not kept;
  - fem_build's mesh without its estimates;
  - a refused solve answered with the solution before it;
  - a 2D model's fem_solve taking solver and force.

All 20 are caught at their first run, each at its own check. The worktree was rebuilt after the breaks. The four
lints are green, and nothing was written to the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan:

- `test_fem3_solve` `estimates`: 0.3 s.
- `test_multiphysics3d_doc` `estimatesAndTheLimit`: 1.7 s.

ASan found a use after free in the test as first written: a reference to the first solve's estimate, read after
the result was solved again. The test keeps a copy now. Nothing else was said.

## What phase C still lacks

- **A user's anisotropic tensor, Newton's method, parallel assembly.**
- Time is said, not held to: a Cholesky's fixed cost varies with the machine's other work.

