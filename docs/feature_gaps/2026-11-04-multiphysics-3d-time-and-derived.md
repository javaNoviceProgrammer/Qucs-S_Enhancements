# Multiphysics 3D: heat in time, the Parametric Sweep and derived values, 4 November

The request: "Finish phase C", after decision 8's devices were held to their publications (f96773d, 77fad7a).
What phase C still lacked, from its write-up (2026-11-03-multiphysics-3d-phase-c.md) and the proposal's table:

1. **Time**: heat in time and its ρ Cp; a sweep.
2. **3D results**: plot groups (surface, slice, isosurface, clip), derived values, VTU, Field on a Grid in 3D,
   Interpolation of three arguments.
3. **The `fem_*` tools on 3D models.**
4. **Estimates and a limit** before a solve.
5. **A user's anisotropic tensor, Newton's method, parallel assembly.**

They are done in that order, each piece committed when it holds: results and the tools read a solution in time
and a sweep's points, so those come first. Tetrahedra stay phase F's (Gmsh's), and decision 7 (METIS for the 2D models)
is the user's still.

This is the first part: time, sweeps, and the derived values that read them.

## Heat in time

A 3D study takes a **Time Dependent** step, as a 2D one does, with the same properties and the same stepping. The
stepper is the 2D solver's `transientStep`, ported to the 3D problems:

- **Methods**: BDF of order 2, with backward Euler for its first two steps; backward Euler alone; the implicit
  midpoint rule on fixed steps.
- **Steps**: adaptive by Milne's device (the solution against a predictor of its order, to the relative tolerance
  of how much the fields have changed), or fixed. A step whose nonlinear iterations fail is taken again a quarter
  as long.
- **What steps**: heat, with its ρ Cp ∂T/∂t. ρ and Cp come from the Solid feature (user defined) or the domain's
  material, and are asked for only by a step in time: a domain without them is refused, named.
- **What is at rest**: electrostatics and currents, solved at each step at its time. A terminal's value (V0, Q0,
  I0) may be of t now, so a terminal can be driven: one of x, y or z is refused.
- **Coupled**: physics that read each other (a heat source of ec.Qrh, σ(T)) are solved in turn at each step until
  they agree.
- **Kept**: the fields, the terminals and every global at each output time, the start included, as snapshots of
  the solution. ht.P at the start is what the sources make there, measured before a step.

**The capacity's matrix.** At order 1 it is consistent in the plane, as the 2D triangles' is, and lumped along z,
each factor to its row's sum, as the in-plane stiffness term's already is. Consistent along z, a tall prism's two
levels couple the wrong way at a short step (m/h - k/h_z > 0 in the bottom's row). A slab of one layer of tall
prisms, its top raised 10 K, then fell to 295.01 K at its first step, 5 K under its start. Lumped, a field uniform in z is the 2D
model's exactly, and the heat a block holds is ∫ ρ Cp T dV to rounding, since lumping keeps each row's sum. At
order 2 the matrix is consistent.

## The Parametric Sweep

A 3D study takes a **Parametric Sweep**: its steps solved again for each value of its parameters, one point after
another.

- A point's geometry and mesh are made again where a parameter swept shapes them (its geometry, its mesh, the
  model's functions). Otherwise every point shares the model's own mesh.
- A point that finds no solution after the first stops the sweep there. Its points before are kept, and where and
  why is said.
- The 2D sweep's "Points at once" is the plane's alone.

**One plan for both.** The sweep's points were planned inside the 2D `runStudy`: its rows read, each value in its
unit, its combinations, each point's name with figures enough to tell its values apart. They are now `sweepPlan()`
in `fem_solver.{h,cpp}`, and both `runStudy` and the new `runStudy3` use it. The 2D code reads its points, names
and parameters from the plan, and is otherwise as it was. Its sweep tests pass unchanged.

## The tab

- **Time**: a solution in time shows a Time box (`mp3Time`) of its output times, the last chosen. Choosing one
  shows the fields, the plot, its legend and the globals at that time.
- **Point**: a sweep shows a Point box (`mp3Point`) of its points by name, the last chosen. Choosing one keeps the
  output time where the point has it.
- **Messages**: the steps taken and taken again, the output times, the globals at the last time, the sweep's
  points, and where a sweep stopped.
- **The panel**: a 3D study's Add menu offers Stationary, Time Dependent and Parametric Sweep. Newton is not
  among a step's nonlinear methods in 3D yet.

## Derived values

A 3D model's Results now show in its tree, with Derived Values and Export. Its Derived Values take the 2D kinds
that have a meaning in 3D: Global Evaluation, Integration, Average, Maximum, Minimum and Point Evaluation. Line
Parameters, Confinement Factor, Perturbation, Mode Overlap and Harmonics stay the plane's.

- **Over**: domains (volumes), boundaries (faces) or, in 3D alone, edges.
  - A volume is integrated by the prisms' rule of degree 4 by 3, mapped by the solution's highest-order space
    (curved where an order 2 triangle is).
  - A face is integrated by the mesh's faces on it, read on the side behind it, or in front where nothing is
    behind or the expression is not defined there.
  - An edge is integrated by Gauss's three points on each segment of its polyline, each point found in the mesh.
- **Average**: the integral over the measure where the expression is defined.
- **Maximum, minimum**: at the quadrature's points and the cells' corners, so a field's extremes at its nodes are
  among them: the maximum of ht.T is ht.Tmax.
- **Point Evaluation**: at points of the geometry, or at coordinates x, y, z (`coordinates3`, in the geometry's
  unit). The panel's points editor takes the columns a property names, so this one has three.
- **Global Evaluation**: expressions of the globals, each named if wanted.
- **Units**: each value is shown in the unit asked, or as its quotient is written (`shown()` and `quotientUnit()`
  of the 2D results, now public).
- **In time and over a sweep**: Solutions "auto" or "all" evaluate at each output time and each point, each value
  named by it ("t = 5 ms: ht.T", "P = 10 mW: ..."). "selected" evaluates at the one the tab shows.
- **The tab**: a derived value's Evaluate button (`mp3EvaluateButton`) puts its rows in a table beside the
  messages (`mp3Table`), in place of its last, and says them. The output time shown is shown again after.
- **A results edit**: adding or changing a derived value after a solve leaves the solution fresh. The tab now
  tells what a solve reads (the model but its results, title and description) from the rest, and a job's result
  is kept unless that changed.

## Validated

| Check | What it holds |
|---|---|
| Uniform in z, in time | Two materials of their own ρ Cp, a held face, a film, a source of t, BDF2 on fixed steps: at each of 7 output times every degree of freedom is the 2D model's of its plan to 1e-9, and ht.Tmax too, at orders 1 and 2 |
| Energy | An insulated block heated by 2 mW: ∫ ρ Cp (T - T0) dV is P t to 1e-9 at each output time, for backward Euler and BDF2 on adaptive steps and the implicit midpoint rule on fixed ones, at orders 1 and 2. Each holds a straight line exactly, and lumping keeps each row's sum. ht.P is 2 mW at the start too |
| A slab in time | Its top raised 10 K: its bottom's temperature against Carslaw and Jaeger's series at 0.05, 0.1, 0.2 and 0.4 s (α = 1e-6 m²/s, L = 1 mm), adaptive BDF2 (rtol 1e-4) within 5 mK at order 2 and 50 mK at order 1 |
| Tall prisms in time | One layer of prisms 2 mm tall, its top raised 10 K, 1 ms steps: every temperature between 300 and 310 K |
| A terminal in time | V0 = 1 mV t/s on a bar: ec.V_a, ec.I_a = V/R and ec.P = V²/R at each output time. Its Joule heat warming it in turn: ∫ ρ Cp ΔT dV within 0.1 % of V'² t³/3R on 2.5 ms steps. It is 0.35 % off on 10 ms steps and 0.084 % on 5 ms, BDF2's h² |
| A sweep | A bar's length swept (2, 4, 6 mm): each point's geometry and mesh its own, ec.R = L/(σA) at each to 1e-9, each point named "L = 4 mm". Its conductivity swept: one mesh for both. A point refused (σ = 0) after the first: the sweep stopped there, its point before kept, "point 2 of 2" said |
| Derived values | On a linear temperature (order 1's exactly): a box's volume, ∫ x dV, the average over a box (T at its centroid), the maximum and minimum (370 and 300 K, ht.Tmax's), T at a point, a face's area, the average over a face, an edge's length and T's average along it - each to 1e-10. A cylinder's surface 2πr(r + h) within 1e-4 on order 2's arcs. A bar's ∫ ec.Qrh dV is its ec.P. An insulated block's average T over time is T0 + P t/(ρ Cp V) at each output time |

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_solve` (8 new functions) | `heatInTimeInvariantInZ`, `energyInTime`, `slabInTime`, `tallPrismsInTime`, `terminalInTime`, `aSweep`, `derivedValues`, and what `refusals` refuses now: a step in time without ρ Cp, a terminal of x (a step of another kind, a frequency one, still refused) |
| `test_multiphysics3d_doc` (3 new functions, 2 changed) | `aModelInTime` (the chip heater in time: the Time box, each time's plot range and ht.Tmax, the start all at 293.15 K), `aSweptModel` (its power swept: the Point box, each point's rise), `derivedValuesInTheTab` (the kinds offered, edges among the levels, Evaluate's row, a point's three columns, a row an output time, the time shown kept); `physicsThroughThePanel` (Time Dependent offered, Frequency Domain not, Newton not), `aNewModel` (Results shown) |
| The 2D suites | Unchanged. The full ctest, 161 tests at eight at once, passed with the 2D sweep reading its points from `sweepPlan()`. After the derived values, 160 of 161: `test_claude_code`'s `aReaderScrolledUpStaysPut` was 2 px off under the load, as before, and passed alone |

## Broken on purpose

`docs/feature_gaps/2026-11-04-multiphysics-3d-time-and-derived/breaks.py` holds 33:

- **The capacity:**
  - order 1's consistent along z (the tall slab's bottom fell to 295.01 K at its first step);
  - lumped in the plane too;
  - order 2's lumped;
  - its history left out of the vector;
  - ρ Cp not required;
  - the start's heat not measured;
  - a terminal of t read at t = 0.
- **The steps:**
  - BDF2's second history weight ω/(1 + ω);
  - the midpoint rule's second half dropped;
  - the physics at rest not solved at each step;
  - no snapshot at the start;
  - a snapshot's globals not taken when chosen.
- **The sweep:**
  - a swept length on the model's own mesh;
  - the points solved with the model's own parameters;
  - plain numbers not in their row's unit (the shared plan);
  - the sweep a step of the study.
- **The tab and the schema:**
  - the plot of the time it last drew;
  - the Point box choosing nothing;
  - no Time Dependent step in 3D;
  - Newton offered for a step in time.
- **Derived values:**
  - a volume's quadrature without its Jacobian;
  - an edge's points in the geometry's unit, not metres;
  - an edge's Gauss weights wrong;
  - the extrema without the cells' corners;
  - an average the integral;
  - a point's coordinates in metres;
  - an output time asked, the last given;
  - an instance's label without its time;
  - the time shown not shown again after an evaluation;
  - the results hidden from the tree;
  - a point's editor of two columns;
  - a results edit leaving the solution stale;
  - edges not offered as a level in 3D.

All 33 are caught. One needed a second look: **a point's coordinates in metres** passed at first. The test gave
plain numbers, and a plain number is a length in the geometry's unit whatever the unit an expression is read in.
Its x is now 250[um], which the break reads as 250 µm of metres.

Rebuilt after the breaks. The four lints are green, and nothing was written to the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan, as CI's Linux job is, each function alone, on a machine another session was
loading (load average 30):

| Function | Time |
|---|---|
| test_fem3_solve `slabInTime` (two orders, rtol 1e-4, direct) | 54.3 s |
| test_fem3_solve `terminalInTime` (400 steps), `energyInTime` (six runs) | 19.8, 18.8 s |
| test_fem3_solve `heatInTimeInvariantInZ`, `derivedValues` | 5.8, 4.3 s |
| test_fem3_solve `tallPrismsInTime`, `aSweep`, `refusals` | 0.3 to 0.4 s |
| test_multiphysics3d_doc `derivedValuesInTheTab`, `aModelInTime`, `aSweptModel` (the chip heater, solved two or three times) | 37.9, 33.4, 30.5 s |
| test_multiphysics3d_doc `physicsThroughThePanel`, `aNewModel` | 1.5 s |
| Whole: test_fem3_solve 22/22, test_multiphysics3d_doc 14/14, test_fem3_geometry 13/13 | 126.5, 110.7, 1.6 s |

No sanitizer said anything.

## What phase C still lacks

- **3D results**: plot groups (surface, slice, isosurface, clip), VTU, Field on a Grid in 3D, Interpolation of three
  arguments.
- **The `fem_*` tools on 3D models.**
- **Estimates and a limit** before a solve.
- **A user's anisotropic tensor, Newton's method, parallel assembly.**
- Tetrahedra (phase F); decision 7.
