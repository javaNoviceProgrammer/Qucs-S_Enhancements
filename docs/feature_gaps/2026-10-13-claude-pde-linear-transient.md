# Feature gaps: a linear PDE in time was solved as a nonlinear one

*13 October 2026 — on `3701196`. The review: `PDE_bug.md`, 12 October, on `18b5157`.*

Claude built a time-domain wave model with the `pde` physics: the 3D scalar wave
equation about a point source, axisymmetric on a half disc of 1 m, quadratic elements at
hmax 0.012, an absorbing arc (`g = -ut/c`, `q = 1/R`), 340 fixed steps of 10 µs by BDF2. It
took 18 to 19 s a step, 5.9 GB, and was cancelled after 61 steps; the whole run would
have taken 1.8 h.

The cause was as the review says. `PdeProblem::solve` ran Newton's method every step:

- a local `SparseLU`, its pattern analysed and the matrix factored each step;
- two iterations, the second only to find the first had converged;
- the Jacobian by differences at each point of integration.

All for a matrix that, its coefficients constant and its step fixed, does not change.

Here, before the fix, the review's model (on a copy, its 152 648 unknowns: u and ut at
76 324 nodes) took 26.8 s for 3 steps and 4.8 GB. Now:

- **The run asked for:** all 340 steps take 26.6 s (20 take 9.5 s: the mesh, the
  assembly and the one factorization; each step after them about 0.05 s), and 1.5 GB.
- **Its answer:** R·u at R = 0.25, 0.5 and 0.75 m peaks at t0 + R/c, to 3e-5 s:
  - 0.9327, 0.9325 and 0.9323 by the implicit midpoint rule (below);
  - 0.927, 0.923 and 0.918 by BDF2, its damping.

  The exact peak is not the review's 1, which is a point source's. This source is a
  Gaussian of radius s = 2 cm, and over a sphere its pulse is spread by s/c, so its peak is
  τ/√(τ² + (s/c)²) = 0.9321.
- **The arc:** nothing comes back from it. Run on to 6 ms (the example below), past the
  time an echo would take back to each point (4.1 to 5.6 ms), |R·u| there stays below
  3e-6 once its pulse has passed.

The model, with the midpoint rule and 600 steps to 6 ms, is now an example:
`examples/multiphysics/point_source_wave.qfem`. Through `fem_solve` it takes 34 s and
factors its matrix once. Its outputs are 20 µs apart, so R·u's plotted peaks are 0.929,
0.9325 and 0.929 (where the outputs fall), and after each pulse it is below 3e-6.

| Review | Now |
|---|---|
| 1. Detect linearity at compile time. Dependence on x, y and t keeps a problem linear; only t in the matrix's coefficients (c, a, ea, da, q) needs it factored again. | **Done, as proposed.** Each expression is read for its degree in the unknowns: the variables, their rates and gradients, along a line its own slopes (`Expression::linearity`). Γ, F and a flux's g may be affine in them, and da, ea and q free of them. A Dirichlet r must be free of them. Anything else is nonlinear: a product or power of the unknowns, a function of them (`sin(u)`, `abs(ux)`), a condition on them, a division by them, \|∇u\|. What multiplies an unknown, or da, ea and q themselves, reading t or another physics' field makes the matrix vary: `(1 + t)*ux` does, `exp(-x^2)*u + sin(t)` does not. The log says which: "linear in its unknowns - solved at once, no Newton's iterations; its matrix factored once while a time step stays" (or "at each solve"), or "nonlinear in its unknowns - Newton's method". |
| 2. A linear problem: assemble and factor the matrix once per mass, keep the LU as a member, then per step only the residual and one solve. | **Done.** The factorization is a member of the problem, kept while the mass (to 1e-9, so an output time's rounded step keeps it), the unknowns and the matrix stay. Each step after it assembles F alone and solves once. A fixed step by BDF2 factors twice, once for its first step by backward Euler and once for the rest; the midpoint rule factors once. A new global, `<physics>.factorizations`, says how many. |
| 3. A nonlinear problem: keep the symbolic analysis across steps; consider modified Newton. | **The analysis kept; modified Newton not done.** The pattern is analysed again only when it differs from the last one's. Modified Newton (the last factorization reused while it converges) is not done: it changes how a nonlinear problem converges, its iteration counts and so the steps an adaptive run takes, and nothing reviewed was slow for it. The slow case, the linear one, now has no iterations at all. |
| 4. Accept the first iteration when linear. | **Done (part of 2).** A linear problem is solved once, from the residual at the values as they are, with `iterations` 1. To make that one solve exact, its Jacobian's differences are taken with a nudge of the whole scale: any nudge gives an affine expression's slope, and a large one rounds to 1e-16, not 1e-9. |
| 5. Optionally, a multithreaded or supernodal direct solver (Pardiso, CHOLMOD, Accelerate). | **Not done.** With the matrix factored once, the factorization is no longer the run's cost: in the review's model it is made once, within the first 20 steps' 9.5 s (the mesh and the assembly with it), of the 26.6 s. MKL's Pardiso has no build for Apple's arm64, Accelerate's solvers are macOS's alone, and CHOLMOD is a new dependency, for symmetric positive definite matrices, which a PDE's are not in general (a second order in time's rates' rows, cxy ≠ cyx, the general form). The assembly was already parallel. |
| BDF2 damps waves: offer a non-dissipative scheme for second order in time. | **The implicit midpoint rule**, a Time Dependent step's method `midpoint` ("Implicit midpoint (order 2, no damping; fixed steps)"; `"method": "midpoint"` to `fem_edit`). Each step is backward Euler over half of it to v, then u(t + h) = 2v - u(t): second order, a wave's energy kept. It takes fixed steps: with adaptive ones it is refused, and the refusal says to set Steps to Fixed. A standing wave over 10 periods at 50 steps a period keeps 0.99979 of its amplitude, against BDF2's 0.98793. |
| `ut` in a flux g: check that the boundary term and its Jacobian with respect to the rate are assembled, so the arc does not reflect. | **The term was assembled, its derivative with respect to the rate was not.** A flux's Jacobian took the variables' columns alone. Newton's iterations made up for that; one linear solve could not, so the rates' columns are now taken too. In 1D, a pulse through an absorbing end (`g = -ut/c`) leaves nothing behind it (< 3e-3; 0.00000), and the same pulse at a closed end comes back whole (0.9994). The arc above does not reflect. A flux's g and q were also checked against a fixed unit, and this model's g was warned of; their units are the equation's, so they are no longer checked. |
| The Multiphysics tree is stale during a solve: "Mesh: not built" while solving, and the root "untitled.qfem" for a saved model. | **Fixed.** While a job makes the mesh (Build Mesh, or a study whose mesh is old), the mesh's status is "being made", and the study being computed says "computing". When the job ends they are what they are. The root is the file's name, set again whenever the model is saved or renamed. |
| No MCP tool to cancel a running solve. | **Already there: `fem_stop`** (`315caa2`, 13 October, after the review's build of 12 October). It stops the model's build or solve running in the background, as the tab's Cancel does, past `fem_solve`'s timeout or the user's own run. |
| Expected: the run in minutes, not hours; R·u at R = 0.25, 0.5, 0.75 m against the peak at t0 + R/c. | **26.6 s; the peaks as above.** |

## Tests

- **`test_fem_pde`:**
  - **`linearity`:** 18 expressions over unknowns u, ux and ut, a varying t and another
    field, \|∇u\| and x: each one's degree, and whether its coefficient varies and whether
    it does.
  - **`aLinearPdeInTime`:** heat on a line, a fixed step of 1 ms to 0.2 s.
    - It is solved with 1 iteration a step and 2 factorizations, the log saying so, and
      within 2e-3 of e^(-π²t) sin(πx).
    - A source of t (`sin(10t)`) keeps 2 factorizations.
    - A diffusivity 1 + t gives 200 factorizations, and e^(-π²(t + t²/2)) sin(πx).
    - A diffusivity 1 + u is nonlinear: 2 iterations or more.
  - **`anAbsorbingEnd`:** the pulse in 1D, by the midpoint rule: before the end, 0.97 or
    more of it; through the absorbing end, less than 3e-3 behind it; at a closed end, 0.9
    or more back.
  - **`theMidpointRule`:** the standing wave over 10 periods: the midpoint rule within
    1e-3 of its amplitude, factored once; BDF2 below 0.995 of it; adaptive steps refused.
  - **`theExamples`:** the point source wave at hmax 0.025 to 5.4 ms (on the sanitizers'
    builds, 0.05 to 3 ms):
    - R·u's three peaks within 2 % (6 %) of 0.9321, at t0 + R/c to 3e-5 s;
    - after 3.5 ms, less than 5e-3 back from the arc;
    - 1 factorization, no warning in the log.
- **`test_multiphysics_doc` `theTreeWhileItSolves`:** on a copy of the diode's reverse
  recovery:
  - the mesh "not built";
  - `fem_solve` with a timeout of 1 s: the mesh "being made", the study "computing";
  - `fem_stop`: the mesh's triangles, the study's status gone;
  - a new model's root "untitled.qfem", saved as named_later.qfem: "named_later.qfem".

## Breaks

Each change was broken in turn and its tests run: 25 breaks.

| Of | Breaks |
|---|---|
| linearity | a varying factor times u not making the coefficient vary; a division by u, u², sin(u) and a condition on u taken as linear |
| the unknowns | a line's own values and slopes not taken as unknowns; t taken as fixed; da, ea and q allowed a degree of 1 |
| the factorization | kept though the matrix varies; kept whatever the mass; made again every step; the linear path skipped |
| the Jacobian | a flux's rates not differentiated; a linear problem's small nudge; an element's entries merged as the last one, not their sum |
| the midpoint rule | not taken on to the step's end (v kept); a whole step; adaptive steps allowed; BDF2 after the first steps |
| units | a flux's g checked against a fixed unit |
| the tree | the root not renamed; `setName` not telling the panel; the mesh not "being made"; the study not "computing" |

Two first passed:

- **da, ea and q allowed a degree of 1.** The nonlinear case was a diffusivity 1 + u,
  whose Γ is already of degree 2. Now a heat equation with da = 1 + u is also checked to be
  nonlinear, and the break is caught.
- **The job's flags not cleared when it ends.** Nothing could see them: the panel asks
  `meshBeingMade()` and `computing()`, which are false and empty without a job. The two
  lines clearing them are removed.

The other 23 were caught, and with the da case all 24 are.

The full ctest (119 tests) passes. Under the sanitizers `test_fem_pde` takes 175 s (the
examples' wave on a coarser mesh, as above), and `theTreeWhileItSolves` passes.
