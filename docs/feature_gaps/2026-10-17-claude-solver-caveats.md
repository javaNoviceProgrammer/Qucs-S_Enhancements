# The sparse solvers: what a review of them found

A review of the Accelerate solver (pasted into the conversation, 2026-10-17) left four caveats. On top of those, CI's first macOS run after the push (run 39015500222) crashed in four tests. Each was reproduced first.

| Caveat | Reproduced | Now |
|---|---|---|
| **Singular matrices:** Accelerate reports success on a matrix singular to rounding (a potential left floating) and returns garbage; its check catches only zero rows and columns; Eigen the same | Worse than written in one place, better in another. See below. | Refused in the physics where it can be seen, and in the solver where its matrices' conditioning allows it to be. |
| **No way to choose the solver in the app**, and the log never says which ran | As written | Multiphysics > Settings… has a sparse direct solver choice. Each solve's last log line names the solvers that ran. |
| **Sweep points at once not tested** (its earlier version crashed the app) | Tested here, headless, apart from the user's window | Five stress runs, below: no crash, the results alike run to run. |
| **Log noise:** the OLED log repeats 18 unit warnings in about 1,150 lines | 1,148 lines, 20 distinct | Each warning said once: 20 lines. |
| *(CI)* **macOS 15.7: four semiconductor tests crashed** (SIGSEGV inside the system's sparse library) | Not reproducible on macOS 26 | Accelerate's LU only on macOS 26 and later. |

## 1. Singular matrices

### What was found

**A box left floating.** Electrostatics, two boxes that do not touch, one grounded, a space charge in the other:

| Mesh | Solver | Before |
|---|---|---|
| 5,879 unknowns (Eigen's either way) | Eigen's Cholesky (LDLT) | "Solved": the floating box at −2.9e12 V, the energy −1.954 J |
| 279k unknowns | Accelerate | Refused, but the message said "the matrix is singular - the matrix is singular" |
| 279k unknowns | Eigen (conjugate gradients from 400,000) | Ran for over ten minutes. Its iteration limit was about 170,000, some hour. |

The physics' own check was global: a value set anywhere passed it.

**A PDE nothing sets** (zero flux all round, a source, no absorption). The reviewer's case exactly:

| Solver | Before |
|---|---|
| Accelerate's LU | "Solved": u = −2.86e7 |
| Eigen's LU | "Solved": u = +2.56e7 |

Accelerate's backward-error check did not catch it, so the caveat holds for the PDE. For the floating electrostatics it did catch it (0.054), so "only exactly zero rows and columns" was too strong.

### Why a solver cannot always tell

A stable solve of a singular matrix leaves a small backward error. The garbage is in a huge solution along the null vector, not in the residual. What does show it is the condition: x is some 1e16 times b. So each check solve now also gives a condition estimate, ‖R A C‖ ‖C⁻¹ x‖ / ‖b̂‖, for the matrix scaled by its rows (R) and columns (C). That is a lower bound of the condition, and free of the unknowns' scales.

**Calibrated on the suite** (127 tests, every factorization logged, refusal off):

| Factorization | Largest condition estimate in the suite |
|---|---|
| Cholesky (Eigen's LDLT, Accelerate's) | 5.2e8 |
| A linear system's fallback LU, the PDE's LU | 1.5e6 |
| The semiconductors' LU | **1.7e22**, solved well (backward error 7e-16, the tests' physics right) |
| The waves' LU | 1.2e16 |

A matrix singular to rounding shows 3e16 to 6e17. The semiconductors' Jacobians are genuinely that ill-conditioned (their carriers' exponentials) and still solve. A threshold for every LU would refuse working models.

### Now

- **The scalar physics** (electrostatics, electric currents, heat) **check each part on its own.** Domains connected through their elements, a floating terminal tying its boundary in one, each need a set value or a convective flux. The message names them: *"Electrostatics: domains 2 (b), 3 (blob) touch no set potential (no Ground, Electric Potential or Terminal at a voltage on them or on what they touch): their potential is not determined - left floating"*. With nothing set anywhere, the message is the old one.
- **Cholesky is refused over 1e15**, on either backend. A linear system then says so ("the matrix is singular - singular to rounding: its condition … at least 3e16") instead of trying an LU, which would solve it.
- **An LU asked to check** (`setSingularChecked`) applies the same limit. The PDE's Jacobian, and a linear system's fallback LU (a symmetric matrix whose Cholesky found a negative pivot of rounding's size), ask. The semiconductors' LU and the eigenvalue solvers' shift-and-invert (near singular on purpose) do not.
- **Accelerate's backward-error check stays** (a pivot of nought replaced). Its message no longer repeats "the matrix is singular".
- **Conjugate gradients run in rounds of 500**, each starting from the last. They stop where five rounds have not halved the error: the floating grid's in 3,500 iterations and 1.1 s, not an hour.
- **The probes again:**
  - the floating electrostatics is refused at both sizes, by the physics before any matrix is built;
  - the PDE nothing sets is refused by both backends at both sizes (3.6e17 to 6.2e17);
  - the same PDE with absorption is solved (u = 1.000).

**What is left:** an LU that does not ask, such as a semiconductor's, is not checked for this. Its physics must say what leaves it singular: a semiconductor without a contact is refused already.

## 2. Which solver, and saying it

- **Multiphysics > Settings… → Solver:**
  - **Automatic:** Accelerate from 20,000 unknowns, Eigen below.
  - **Eigen** at every size.
  - **Accelerate** at every size, where this macOS has it.
- **Kept and applied.** The choice is kept as `Multiphysics/solver`, given to the solver on OK and at every start (the GUI and `--mcp-server` alike).
- **What is in effect is said.** For example: "Accelerate's LU and Cholesky from 20,000 unknowns, Eigen's below", or "Accelerate's Cholesky …; Eigen's LU (this macOS is older than 26 …)". `QUCS_FEM_SOLVER=eigen|accelerate|auto` is said over it.
- **Each solve's last log line names the solvers that ran:** "Solved 681054 degrees of freedom in 4.10 s with Accelerate's LU", "… with Eigen's Cholesky (LDLT)", "… with Eigen's conjugate gradients". Factorizations are counted by solver, and a step reports the difference. Points solved at once count together.
- The tools' format text says the same.

## 3. Sweep points at once

Run headless, each in its own process and settings, away from the user's window:

| Sweep | Points at once | Runs | Result |
|---|---|---|---|
| The grating example (26,488 unknowns a point, Accelerate's LU) | 9 | 10 | 0.17 s each, identical to the last bit |
| Eight box widths, so eight meshes and eight analyses at once (more than the four the analysis cache keeps: evictions while others hold theirs) | 8 | 5 | 5.3 s each, alike to 1e-13 |
| Six wavelengths of a 280k-unknown waves box | 6 | 3 | 2.3 s each, alike to 7e-14 |
| The OLED model's three wavelengths (681k each) | 3 | 2 | 7.3 s each, alike to 4e-13 |

No crash and no error. Accelerate's analyses run one at a time (the lock of 009c356); its factorizations run at once.

## 4. Log noise

`Problem::compile` checks a coefficient's unit each time it is compiled, which is once per domain. The OLED's flux and source were said once per domain: 1,148 lines, 20 distinct. Each warning is now said once: the OLED log has 20 lines.

## 5. macOS 15: Accelerate's LU

CI's macOS runner (macOS 15.7.9, Xcode 16.4's 15.5 SDK) crashed in four tests, each in a semiconductor solve: a junction, the VCSEL laser example, a MOSFET, and the solve the cancel test stops. All four crashed inside the system's sparse library. The same tests pass on macOS 26 here, under the sanitizers too, with Accelerate at every size.

Semiconductor Newton matrices are the ones whose pattern comes and goes: they make many analyses, cached and evicted, and their values change the most between refactorizations. The SDK's headers note that an LU's delayed pivots can need more factor storage than its analysis sized. Their LU is new in macOS 15.5, and macOS 26 adds orderings and scalings to it. A library fault there would fit, but it cannot be shown from macOS 26.

**Now:** Accelerate's LU only on macOS 26 and later. The check is `macOS 16.0`, which macOS 26 reports to a program built with an older SDK. macOS 15 takes Eigen's LU and keeps Accelerate's Cholesky, which passed there. The Settings line says why. CI's macOS job now runs Eigen's LU.

**Not done:** finding the fault on macOS 15. A diagnostic branch on GitHub's macOS 15 runner could tell (refactorizations against fresh factorizations, the default pivot tolerance) and give 15.x its speed back.

## Tests

- **test_fem_sparse `theSolverChosen` (new):**
  - Eigen chosen: no Accelerate, its LU counted and named;
  - Accelerate chosen: from 0 unknowns, its LU where this macOS has it, its Cholesky;
  - `QUCS_FEM_SOLVER` over the choice, each way.
- **test_fem_sparse `singularToRounding` (new):**
  - a grid's Laplacian with nothing set: refused by Cholesky and by a checked LU on either backend, solved by an unchecked one;
  - a sound grid scaled from 1e-3 to 1e3: solved by each;
  - a linear system: said singular, with no LU made after its Cholesky;
  - the same grid beside −1 on a diagonal (singular and indefinite, so its Cholesky fails by its sign): said by its LU;
  - its conjugate gradients: stopped, stalled, in 3,500 iterations.
- **test_fem_physics `aPartLeftFloating` (new):**
  - the floating box named (b, blob, not a);
  - nothing set at all: the old message;
  - both grounded: solved;
  - a floating conductor in a grounded box: solved; alone in the other: refused;
  - heat: refused, and solved with a convective flux.
- **test_fem_pde `aPdeNothingSets` (new):**
  - the pure Neumann PDE refused on both backends, solved with absorption;
  - the general form's unit warnings each said once over six domains.
- **test_multiphysics_doc `theSettingsHaveTheSolver` (new):**
  - the box and its three choices;
  - the status line, Automatic and Eigen;
  - Cancel keeps nothing; OK keeps and applies;
  - a solve through the tools says "with Eigen's Cholesky (LDLT)", then back on Automatic "with Accelerate's Cholesky";
  - `QUCS_FEM_SOLVER` said over it.


Seventeen changes broken on purpose, all caught:

- **The physics' check:**
  - the per-part check off;
  - a convective flux not counted;
  - set values not counted.
- **The solver's checks:**
  - the Cholesky's condition unread;
  - a linear system taking an LU after a singular Cholesky (caught by the no-LU count);
  - a linear system's LU unchecked (caught by the indefinite case);
  - the PDE's LU unchecked;
  - Eigen's LU check skipped;
  - Accelerate's LU condition unread;
  - stalled conjugate gradients not stopped.
- **The log and the choice:**
  - unit warnings repeated;
  - the solve line without the solver;
  - OK keeping the solver without applying it;
  - Accelerate chosen from 20,000, not 0;
  - the environment ignored;
  - the status without the environment;
  - Eigen chosen keeping Accelerate.


Runs:

- **The Release suite:** 126 of 127. test_file_browser failed once: a file watcher's 5 s wait wanted 5.1 s under the suite's load. It passes alone, three times of three.
- **Under the sanitizers, no report,** each with Eigen's solvers and with the default:
  - test_fem_sparse (whole): 39 s, after its sanitized grid was made 50 and its choice test 200 unknowns (229 s before);
  - the new functions of test_fem_physics and test_fem_pde: 1 s each;
  - test_multiphysics_doc's two Settings functions: 35 s.
- **The lints pass.**
