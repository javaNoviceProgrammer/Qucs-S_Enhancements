# Multiphysics optimization: every routine the ngspice side has, on a 2D model

*7 November 2026, begun on `4a7186b`; the last builds stand on `86a8458`, as `82d25ce` and `86a8458` landed in this shared checkout meanwhile. Apple M2 Ultra, macOS 27.2, Qt 6.12.0 from Homebrew. CI pins Qt 6.10.3 and none of
this has run on it, nor on Linux or Windows (see "What could not be verified").*

The request: "Looks like our 2D multiphysics doesn't offer any optimization. Look at all the optimization routines we
implemented for ngspice and add support for all of them in multiphysics. The goal is to use the parameters of a model and
define one or more objectives for a multiphysics setup and optimize the model parameters to achieve the optimization
targets."

## What you can do now

Add an optimization under the **Optimization** node of a 2D model's tree (its menu, or `fem_edit` with `type` optimization under `optimizations`). In it:

- **Parameters to vary:** any of the model's parameters, each with a range and a type: linear or logarithmic, whole
  numbers, or preferred values E3 to E192. A parameter that shapes the geometry or the mesh gets a new geometry and mesh for
  each candidate.
- **Objectives:** rows of what is asked of the results. Each is an expression of the global values, the parameters and the
  Derived Values nodes (`es.C11`, `ht.Tmax - 300[K]`, `lp1.Z0`), with a kind:
  - `MIN`, `MAX`: as small or as large as it can be.
  - `TARGET`: at a value, by least squares.
  - `LE`, `GE`, `EQ`: limits.
  - `MON`: only shown.

  Several are weighed against each other by their weights, each relative to its value at the start or to its target.
  On a sweep, output times or frequencies a row counts by its worst point, or its last (**Over**). On a mode analysis it reads
  one mode: the one its step chose, else the first, or the one its **Mode** column names (`all`: every mode, by its worst).
- **Any method:** `de` with its ten variants, `pso`, `sa`, `cmaes`, `bayes`, `nm`, `tr`, `lm`, `nsga2` (a Pareto front),
  `tune`.
- **Run it** with the node's **Run**, the toolbar's **Optimize**, or `fem_optimize`. It runs in the background with its
  progress and Cancel, the window going on. Each candidate is a whole solve; **Solves at once** runs several together.
- **See what it found** in the tab's **Optimization** page: why it stopped, the best values, each limit at the optimum, the
  history of every solve with its cost falling, and a Pareto front's designs to choose from.
- **Apply Optimum** writes the best values into the parameters as one step of Undo, each in the unit its expression has,
  and computes the study there.
- **It is kept:** the setup and the last result are saved in the `.qfem` and shown again on opening, with no solve. A result
  whose model has changed since is marked old.

The shipped `examples/multiphysics/capacitor_optimization.qfem` is one to try: the least area for 100 pF within a field
limit, its answer known in closed form.

## The inventory

Read from the code: `optimization.*`, `extsimkernels/optimizer.*`, `components/opt_sim.*`, `components/optimizedialog.*`,
`ngoptimize.*`, `components/ngopt_sim.*`, `components/ngoptdialog.*`, `qucscontrol_ngspice.cpp`, the `apply_optimum` and
`optimum` of `simulate` in `qucscontrol.cpp`, `tune` and `tuneKnobs` in `qucscontrol_design.cpp`, both tests, and
`ngspice-46/src/frontend/com_optimize.c` of the Ngspice_OpenVAF_Enhancements fork, where an NgOpt block's methods live. The
list given to start from was missing `tune`, and `nsga2` and `-center`, which the fork's command has and a block does not
write. All of them are here.

**Nothing was dropped.** Each routine below is supported, or says why it does not apply. No question to the user was needed.

### The methods

| Routine | Where it lives on the ngspice side | On a 2D model |
|---|---|---|
| `de`, differential evolution | an NgOpt block (the fork's `differential_evolution`); an optimization component (`.Opt`, `optimization.cpp`) | **Supported.** One implementation, the component's class, now in the shared core. The fork's own (DE/rand/1/bin, updated within a generation) is not ported: see "Where it differs from the fork". |
| The ten DE variants (DE/best/1/exp ... DE/rand/2/bin) | `.Opt` | **Supported** (`strategy`), with its `F`, `CR` and minimum cost variance (`mincostvariance`). The default is the fork's DE/rand/1/bin, F 0.8, CR 0.9 (15/n past 16 knobs). |
| `pso`, particle swarm | NgOpt | **Supported.** Ported. A generation evaluated together. |
| `sa`, simulated annealing | NgOpt | **Supported.** Ported. |
| `cmaes` | NgOpt | **Supported.** Ported. |
| `bayes`, Gaussian process and expected improvement | NgOpt | **Supported.** Ported, its hand-off to the local method too. |
| `nm`, Nelder-Mead | NgOpt | **Supported.** Ported. |
| `tr`, trust region on a quadratic model | NgOpt | **Supported.** Ported. |
| `lm`, Levenberg-Marquardt | NgOpt | **Supported.** Ported. Its finite difference is a setting here (`fdstep`), since a mesh has noise a circuit has not. |
| `nsga2`, a Pareto front of 2 to 8 objectives | the fork's `optimize` in a Nutmeg script; a block refuses it | **Supported.** Ported; a front in the page, each design written in on request. It also takes limits, which the fork's refuses: see below. |
| `-center`, design centering: the worst Cpk over an inner Monte Carlo | the fork's `optimize` in a Nutmeg script | **Supported** (`mode` center): `LE` and `GE` rows are the specifications; `variation` rows the spreads, `samples`, `lhs` (Latin hypercube), every candidate seeing the same draws. The yield and Cpk are reported. A `.qfem` has no random parameters, so the spreads are given here. |
| `tune`, false position for one knob, Broyden for 2 to 4 | the Claude tool of that name, on a schematic | **Supported** as a method. The search is written again on residuals: the tool's is woven through its simulation calls, and not worth untangling for this. Its `values` table is a Parametric Sweep, which a model has. Its `hold` is any limit row: a value that breaks one is not the best. Its `compare` is the history's first row against the best. |

### The options

| Option | Where | On a 2D model |
|---|---|---|
| `MaxIter`, `Tol`, `Size` (population), `Seed` | NgOpt | `maxiter`, `tol`, `population`, `seed`. Same meanings and defaults. |
| `Polish`, `Starts` | NgOpt | `polish`, `starts`. Same rules: `lm` for a fit and `tr` otherwise; a share of the iterations a start; CMA-ES's population doubling. |
| `Verbose`, every iteration in the console | NgOpt | `refresh` 1. |
| Output refresh cycle | `.Opt` | `refresh`, a line every so many iterations. |
| Knobs: `.param` | NgOpt | **Supported**: what a model's parameters are. |
| Knobs: instance (`-param`) and `.model` (`-mparam`) | NgOpt | **Do not apply.** They are how ngspice changes a device in place without re-reading its deck. Every value in a `.qfem` is an expression of its parameters: make it a parameter and vary that. |
| Variables: active or held, `LIN_DOUBLE`, `LOG_DOUBLE`, `LIN_INT`, `LOG_INT`, `E3`...`E192` | `.Opt` | **Supported**, by the same `Variable` class. |
| Goals: `MIN`, `MAX`, `LE`, `GE`, `EQ`, `MON` | `.Opt` | **Supported**, with the same reading of a sweep: a row counts by its worst point. |
| Objective and constraint weights | `.Opt` | `constraints` penalty, with `objectiveweight` and `constraintweight`, by the same cost. |
| An end once every limit is met | `.Opt` | **Supported** (only limits, all met). |
| `Minimize`, an expression after an analysis | NgOpt | A `MIN` row. Several are allowed here; the fork takes one. |
| `Target`: an expression, a value, a weight, its analysis | NgOpt | A `TARGET` row. Its analysis is its `study` column, solved for each candidate, 8 studies at most as 8 stages there. A row's last value is `over` last. |
| `Constraint`: an expression, min, max, its analysis; `CTol`; an augmented Lagrangian | NgOpt | `LE` and `GE` rows (or `EQ`), `ctol`, `constraints` lagrangian. Same rounds, same penalty, its multipliers reported. |
| Status: converged, completed, maxiter, nosolve, unchanged, interrupted, infeasible | NgOpt | **Supported**, the same words. |
| Notes: on a bound, an objective that never moved, what did not solve, the iteration cap | NgOpt | **Supported**, with the first solve's error as well. |
| Each constraint's state: active with its multiplier, slack, VIOLATED | NgOpt | **Supported.** |
| A candidate at a time as a whole simulation, several at once | `.Opt`'s `Optimizer` | `parallel`, 1 by default, as a sweep's: each solve holds its own matrices. |
| The optimum written in: `apply_optimum`, `optimum`; "Copy current values to equation" | `simulate`, `.Opt`'s dialog | `fem_optimize` with `apply_optimum` or `action` apply; Apply Optimum. The values are written in, not the node's starts: a model has its parameters to hold them. |
| Carrying a value across a netlist's resets (`carryLines`) | NgOpt | **Does not apply.** It works around how ngspice re-reads a deck. |

### Neighbours, and why they are not on the list

| Neighbour | Decision |
|---|---|
| `NgMonteCarlo` (`montecarlo`), `NgCorners` (`corners`), `highsigma`, `wcd`, `mccorr`, `mcsample` | **Not optimization.** They measure a yield, a corner or a failure rate at one design and move nothing. Design centering's inner Monte Carlo is the part that optimizes, and it is here. |
| `NgSweep` (`sweep`), the Parametric Sweep | **Not optimization**, and a model already has one. Reused, not copied: a study's own sweep runs inside each candidate. |
| The tuner dialog | **Not optimization**: sliders, moved by hand. |
| ASCO, an optimization component under qucsator | **Not a ngspice routine.** Its method is DE, which is here. |

## The gap, confirmed

Nothing in `multiphysicsdoc.*`, `multiphysicspanel.*`, `qucs_multiphysics.cpp`, `qucscontrol_fem.cpp`, `fem_*` or the `.qfem`
format searched, minimized or held an objective. The word appeared only as "optimised" in comments about other things. What
was there and is reused:

- `runStudy`, each candidate one call, with its Progress for Cancel.
- `evaluateParameters`, the reading of a candidate's values.
- `SweepPlan::reshapes`, whether a parameter shapes the geometry or the mesh.
- `instances` and `select`, a sweep's points and output times.
- `evaluateDerived`, a Derived Values node's value.
- The job, its progress row and Cancel.
- The tree's generic settings form and `Rows` tables.
- `whenJobEnds`, `fem_stop`, `fem_describe`'s "at work".

## What was built

### The shared core: `multiphysics/optimizer_core.{h,cpp}`

The searches, knowing nothing of what they search: a callback is asked for the cost of points in the cube `[0, 1]^n`. It is in
the `fem` library, the one both sides link.

- **Moved here from `optimization.*`, unchanged:** `Variable`, `eSeries`, `Goal`, `Settings`, `measure`, `cost`, `scaleOf`,
  `number`, `DifferentialEvolution`. An optimization component now runs this code. `Variable::parse`, `Goal::parse` and
  `Settings::parse` stay in `optimization.cpp`, because they read a number as a schematic's property does
  (`valuereading.cpp`, which is above this library).
- **The method table:** `ngoptimize.cpp`'s `kMethods` moved here (`searchMethods()`), with two more that a block does not offer.
  A block's `methods()`, `methodNote()`, `isGlobal()` and the rest read it.
- **Ported from `com_optimize.c`:** `nelder_mead`, `levenberg_marquardt`, `particle_swarm`, `simulated_annealing`,
  `trust_region`, `cma_es`, `bayes_opt`, `al_solve`, `nsga2`, the starts and the polish. The port keeps the C code's steps,
  constants, stops and stream of random numbers (see "Checked against the fork").

ngspice's own methods stay where they are: they are C inside another program, on its side of a process. So this is a port,
not a sharing: the two can drift, and only the check below would notice. What can be shared is: the DE, the variables, the
goals and the method table have one implementation for both of Qucs-S's users.

#### Where it differs from the fork, and why

| Difference | Why |
|---|---|
| `de` is the component's generational class, not the fork's, which updates its population within a generation | One DE for both sides. Evaluated a generation at a time it runs on any number of threads. Its stops are the class's and the fork's stagnation. |
| `pso` evaluates a generation together: each particle pulled toward the best as it stood before it | The same search on any number of solves at a time. The fork moves the best within a generation, which needs one at a time. |
| The best point reported is the best seen, feasible first, not the method's last point | With limits an earlier round may have met them better. Without limits the two are the same, as the check shows. |
| A local method whose start does not solve looks over the range (`findStart`) before it gives up | The fork's simplex and fit stop there, with NO SOLUTION from one bad corner. A whole solve has many ways to fail at one point. |
| `nsga2` takes limits, by Deb's rule: a design that breaks one loses to one that does not | The fork refuses them. A front that breaks a limit is of no use. |
| A failed design is dominated by every other under `nsga2`, a maximized objective too | The fork had this fault, found while porting: a failed value was set to 1e15 and then negated for `-maximize`, so a design that did not solve read as the best of all. Reproduced and fixed there afterwards as its Enhancement-918. |
| A target's miss is relative: `w (y - t) / t`, or to its value at the start for a target of 0 | The fork's is absolute, with the weight to make it relative. In SI a capacitance is 1e-10: its square, 1e-20, is under the tolerance's floor and every search "converges" at once. So a weight here is how much a row counts, a number. |

### A model's optimization: `multiphysics/fem_optimize.{h,cpp}`

- **`optimizationPlan`** reads and checks the node, before anything is solved: a parameter the model has not, a range that is
  not one, a value with no unit or another unit, nothing asked, a method that cannot take what is asked. Its limits are the
  fork's: 128 parameters, 128 targets, 32 limits, 8 objectives, 8 studies.
- **`runOptimization`** runs it. Each candidate is written into a copy of the model, a study's sweep reads its parameters from
  there. It gets its own geometry and mesh only where what it changes shapes them; otherwise one mesh is shared.
- **The start is solved first.** It sets the scales, and what is asked is checked on its solution: a row the results do not
  have ends the run there, with what they do have. A start that does not solve is one bad point, not the end.
- **A candidate that does not solve** counts as the worst there is, its row says why, and the search goes on.
- **A candidate already solved is not solved again.** Whole numbers and preferred values round many candidates to one.
- **`applyOptimum`** writes into the parameters, each in the unit its expression has. `optimizationSetup` says what a result
  was of; the varied parameters' own expressions are not in it, so writing the optimum in does not make it old.

### The file

A top-level `"optimizations"` array, written only where there is one: a model without any writes what it wrote before, byte
for byte. Each node has its settings, `variables` and `objectives` tables and, once run, `result`. The result's history is
one string a row, so a file diffs by the line, and at most 1,000 rows: those that improved, and the latest.

### The window

- **The node** in the tree: one Optimization node, between the studies and the results and always there on a 2D model, with each optimization under it, by the generic settings form. The node is the tree's alone: the file keeps its top-level list, and adding to the model still lands under it. `Rows` gained `columnChoices`: a
  box in a cell, or the model's parameters or studies. **Add** fills a row: the first parameter not varied yet, a factor of
  two each way, in its unit.
- **`multiphysicsoptimization.{h,cpp}`**, the tab's Optimization page. It is there only where the model has an optimization.
- **`MultiphysicsDoc::optimize`** is a job like Compute. The result goes into the node as one step of Undo, made by the
  simulation as a schematic's optimum is.
- **The toolbar's Optimize**, **Multiphysics > Optimize**, the node's menu and buttons.

### The tools

`fem_optimize`: `action` run, result or apply; `apply_optimum`; `point`, a Pareto front's design; `history`; `timeout`. As
`fem_solve`, past its timeout it goes on and says how far. `fem_stop` stops it with its best point. `fem_describe` lists each
optimization and leaves its history out. `fem_edit` and `fem_model` take the node and the section as any others.
`fem_describe what=format` has its paragraph and its kind.

A 3D model refuses it, and its kind is not offered there. Sharing that was not free: `fem3` solves through its own study runner.

## How it was tested

### Checked against the fork

The same divider through the fork's `optimize` (`/Users/person/bin/ngspice`) and through the port, in closed form:

| `optimize -param R1 1k 100 10k -analysis op ...` | The fork | The port (plus the 1 the fork spends leaving the circuit at the optimum) |
|---|---|---|
| `-minimize (v(out)-0.3)^2 -method nm` | 39 evaluations, 1.33956e-15 | 38 + 1, 1.3411e-15 |
| `-method tr` | 11, 3.6176e-16 | 10 + 1, 3.60377e-16 |
| `-method cmaes -seed 1` | 173, 3.12547e-14 | 172 + 1, 3.12413e-14 |
| `-method sa -seed 1` | 1214, 2.01905e-09, r1 = 2333.83 | 1213 + 1, 2.01905e-09, r1 = 2333.83 |
| `-method bayes -maxiter 40 -seed 1` | 24; surrogate predicted 0.000117779; polish from 1.43414e-06 to 1.5627e-13 | 23 + 1; 0.000117779; from 1.43414e-06 to 1.56268e-13 |
| `-target v(out) 0.3 -method lm` | 12, r1 = 2333.33 | within 2 |
| `-target ... -method nm -starts 2 -seed 1` | start 1 of 3 won, polished by lm, 126 | the same, within 6 |
| `-minimize ... -method cmaes -maxiter 6 -seed 1 -polish` | polish from 1.67666e-06, 33 | the same, within 2 |
| `-minimize abs(i(v1)) -constrain v(out) -min 0.2 -method nm`, `tr` | r1 about 4000, multiplier 0.0009155, 0.0009749, penalty 10 | the same, multipliers within 5 % |
| `-param R1 ... -param R2 ... -minimize ... -maximize ... -method nsga2 -maxiter 5 -swarmsize 8 -seed 1` | 48 evaluations, a front of 8 designs (one printed twice) | 48, its 7 designs to 5 digits |

The small differences are rounding: ngspice's `v(out)` comes from a matrix, the port's from a formula. `test_optimizer_core`
holds these numbers, with room for that rounding on other platforms. `pso` and `de` differ by design.

### New tests

| Suite | What | Release | ASan, UBSan |
|---|---|---|---|
| `test_optimizer_core` (102 cases) | The fork's numbers; known minima; Rastrigin from the wrong valley; ZDT1's front; multipliers; an infeasible limit; a stop at the 7th evaluation by every method; failures; NaN and infinity; every point in the cube; the order of a batch changing nothing. | 0.1 s | 2.3 s |
| `test_fem_optimize` (102 cases) | Below. | 2.2 s | 73 s |
| `test_multiphysics_optimize` (16 cases) | The tree, a run, Cancel, Undo, saved and opened, Apply, a front, the tools. | 3.2 s | 19 s |

Closed forms in `test_fem_optimize`:

- **A capacitor's gap,** `C = ε0 εr w / d` exactly on any mesh, on a logarithmic scale: every method.
- **A minimum inside a range:** `R/5Ω + w/2mm` for a bar, at `w` = 2 mm exactly: every scalar method.
- **Two objectives weighed:** `4 R/Rs + w/ws` at `w = 2 ws`, MIN and MAX.
- **Two targets, two parameters:** a capacitance and an area, `d = sqrt(ε A / C)`, by `lm`, `tune`, `nm`.
- **A limit:** the least width with `R ≤ 2.5 Ω` is 4 mm, by Lagrangian and by penalties; `GE`, `EQ`; one that cannot be met.
- **A Pareto front:** a bar's `R` against its area, the front at the least `L` with `R A = L²/σ`.
- **Design centering:** `C` between two limits under 2 % of `d`, its optimum halfway and its Cpk `(hi - lo) / 2 / 3σ`.
- **A transient:** `Tmax = 300 K + rate t`, read at the last output time, and at its worst.
- **A sweep's worst point and its last,** a Derived Values node, another study's value, whole numbers and E12.
- **The shipped example,** by three methods.

What can go wrong:

- **A stop** at the 5th solve, by every method: the best of those solved kept, and it may be applied. A stop before anything
  is solved.
- **A failed solve during a run**, by every method: below 0.5 mm the gap's permittivity is not a number. The start failing too.
- **Nothing solves**, by every method.
- **What the results lack:** a name, a field that is not one number, a value of another unit.
- **Bad setups:** 44 of them, each said before any solve.
- **A kept result damaged:** in any way, read as far as it can be; a history row cut short or run on is left out.
- **Old files:** every shipped `.qfem` read and written with no section added.
- **Solves at once:** by every method that can use them, 1 and 4 give the same rows, to the last digit.

Every run is bounded by its evaluations and tolerances. No test reads a clock: the one that shows the window was not frozen
counts that its events ran while the job was busy, at least once. A long run is annealing with a million levels, stopped.

### Breaks

`breaks.py` (beside this file) breaks each part in a copy of the source, and runs the test that should catch it.
It lists 83, in the core, the engine, the file, the window and the tools.

On the first run 75 were caught and 8 were not. Each of the 8 was a test that proved less than it seemed to, and each now has a test of its own. Run again, all 8 are caught, so 83 of 83. `results.txt` has every line.

| Break that was missed | Why it was missed | The test now |
|---|---|---|
| `lm` never lowers its damping | On the divider and on a line, the damping hardly matters | `aFitAlongACurvedValley`: Rosenbrock's residuals, under 100 evaluations (54 here) |
| A backward difference with the wrong sign | No fit started at the top of its range | `aFitFromItsUpperBound`, by `lm` and `tune` |
| A swarm without a particle's own best | One run on any function says little | `aSwarmKeepsItsOwnBest`: 40 seeds on Rastrigin, a mean of 6.6 here and 9.9 broken, bound at 8.25 |
| The surrogate's improvement with another sign | The polish hid it | `theSurrogateLooksWhereItExpectsBetter`: its first 8 picks over 10 seeds, a mean distance of 0.026 here and 0.20 broken, bound at 0.08 |
| NSGA-II's front of the lower ranks too | After many generations every parent is of the first rank | `anEarlyFrontHoldsOnlyItsFirstRank`: two generations |
| The DE variants all one | The test read their names and their answers, which agree | `eachDifferentialEvolutionSearchesItsOwnWay`: ten different paths from one seed |
| More starts with all the iterations | Only the count of a method that stops itself was held | `startsShareTheIterations`: annealing's schedule, under 1.5 times a single run |
| A history row of too few cells read | No test gave a short row | `aRowOfAnotherLengthIsNotRead`. This one crashes when broken |

The two statistical bounds are averages over a fixed list of seeds, and each sits between the measured value and the broken one. They read no clock.

### The fuzz

`docs/bug_hunts/2026-10-16-fuzz/fuzz_qfem.py` now makes one mutant in three an optimization's (`QFEM_OPT_SHARE`): the section
added to a small capacitor that solves in milliseconds, or to a shipped model, and damaged alone (its settings, its rows, a
kept result's rows), then run by `fem_optimize`, stopped, read, written in and saved.

Run under ASan and UBSan on the final code, with `QFEM_OPT_SHARE=0.9` so nearly every mutant was an optimization's: seeds 21, 22 and 23, 250 mutants each, **750 mutants, 0 findings**. `fem_optimize` was called 2,025 times, the longest call 9.7 s, and 1,312 of the calls were refused, each with a reason the summary counts. 248 mutants' optimizations did run, some of them stopped on purpose. Most damage to a section is caught by its check, so the fuzz spends most of its time on that check.

### A real model, through the server

`drive.py` (beside this file), through `qucs-s --mcp-server`. Its output is in `drive.txt`.

| Model (shipped example) | Asked | Method | Solves | Found | Known |
|---|---|---|---|---|---|
| Microstrip | `w` for `lp1.Z0` = 50 Ω | `tune` | 9 | 3.02807 mm, 50.0036 Ω | |
| | | `lm`, `fdstep` 0.02 | 7 | 3.02844 mm, 50 Ω | |
| | | `tr` | 9 | 3.02844 mm, 50 Ω | |
| | | `bayes` | 23 | 3.02844 mm, 50 Ω | |
| Coaxial line | `a` for `es.C11` = 100 pF | `tune` | 11 | 466.306 µm | 466.35 µm in closed form |
| | | `cmaes`, polished | 56 | 466.346 µm | |
| | | `nm` | 20 | 466.309 µm | |
| Resistor | `ec.P` against `ht.Tmax` | `nsga2` | 78 | 12 designs, from 5.98 W at 426 K to 0.17 W at 300 K | |

Each ran in about a second or less. `action` apply then wrote `w = 3.02807287762[mm]` in its own unit, and a solve there gave 50 Ω. `cmaes`, polished, meets 100 pF to the digits shown and lands 0.001 % from the closed form. `tune` and `nm` stopped at their tolerance, at 99.993 pF, 0.01 % away.

### What else ran

On the final code:

- **Release, all of CTest:** 167 of 169 passed.
  - `test_library_export` did not run. It needs `qucsconv_rf`, whose build needs `dos2unix`, which is not installed here. It tests nothing changed here, and this is the limit the task named.
  - `test_file_browser` failed once, in `theFolderShownTakenAwayAndBack`, a 5 s wait on a folder coming back, while the break tests were loading this machine. It then passed three times alone, and passed in the run before. Nothing here touches that code, but it is a wall-clock wait that can go red under load. It turned out to be a real fault of the File Browser, not the wait, and was fixed after this write-up: a folder removed and made again could stop being followed.
- **ASan and UBSan:** `test_optimizer_core`, `test_fem_optimize`, `test_multiphysics_optimize`, `test_optimization`, `test_ngopt`, `test_multiphysics_doc` (842 s), `test_qucs_control` (237 s): 7 of 7, with no sanitizer report in the log. Only these seven were run under the sanitizers.
- **Lints:** `check-json-nesting`, `check-shaders` (14 shaders), `check-third-party`, `check-tool-arguments` (144 tools, 0 disagreements).

The new tests add little to CTest's limits: under ASan `test_fem_optimize` takes 73 s against its 900 s, `test_optimizer_core`
2.3 s against 900 s, and `test_multiphysics_optimize` 19 s against its 300 s. All are far inside what `3972088` and `c6c7eb2` keep,
even at the 1.9 times of CI's slowest runner.

## Decisions worth a look

- **Limits are held by an augmented Lagrangian by default.** That is the fork's. A stiff first penalty can make a local method
  slow: the example needs its 400 iterations of `tr`. Each is a millisecond there; on a slow model, `cmaes` or `penalty`.
- **The default for a sweep is its worst point** (the component's), not its last (the fork's). `over` chooses per row. For a
  `TARGET` over output times you almost always want `last`: the worst of a transient from 300 K is its start.
- **A result is kept in the model, as a step of Undo.** So a run makes the document unsaved. An hour's run should not be lost by
  closing a tab; solutions are not kept, as they are large.
- **`Solves at once` is 1 by default**, as a sweep's `parallel`, unlike the component's up-to-8: a model's solve can hold
  gigabytes.
- **Mesh noise.** A parameter that shapes the geometry remeshes each candidate, and a finite element value then has a small
  discontinuity. `lm` differences over `fdstep`, 1e-3 of the range: raise it for such a parameter, or use `tr`, `nm` or `cmaes`.
  The microstrip above needed 0.02.

## What could not be verified

- **Qt 6.10.3**, the version CI pins, and **Linux and Windows**: all of this ran on macOS with Qt 6.12.0 alone. Nothing here uses
  a Qt newer than the code around it, which is not the same as having run.
- **CI's runners:** the times above are this machine's. `test_optimizer_core`'s numbers from the fork have room for rounding, but
  a platform whose `exp` or `erfc` rounds differently may walk the Bayesian surrogate's chaotic fit to another path. If that test
  goes red on CI, its tolerance is the first place to look.
- **A long run on a large model.** The longest here took seconds, on models of milliseconds a solve. Hours of a semiconductor's
  solves have not been run, nor `parallel` on a model of gigabytes.
- **Process flows as candidates.** `optimize()` builds a flow's geometry first, as Compute does, and a stop reaches a flow's anneal
  through `runStudy`'s Progress; a parameter that shapes a flow has not been run through an optimization.
- **A person's eye on it:** driven offscreen, its page looked at by screenshot. No real display, no mouse.

## The review of 7 November

[2026-11-07-claude-qfem-optimization-check.md](qucs-reviews/Multiphysics/archived/2026-11-07-claude-qfem-optimization-check.md),
black-box runs of `e723e56` against closed forms. Every finding was checked here in the code before it was changed, and every one
was real. Each has a test that fails on the code before this change.

**B1, the one that mattered: a limit in small SI units counted as met.** A limit's miss was divided by `max(1, |value|)`, so
for 900 pF a miss of 546 pF read as 5e-10. A limits-only run then "converged" at its start, its own limit line saying VIOLATED,
and that point could be written in. The history marked rows that broke the limit as feasible, and an EQ goal by penalties read
met 35 pF away. The search itself judged by a share of the value (`conScale`), so the optimum of a run with an objective was
right: only what was said about it was wrong. This write-up's tests had their limits at 2.5 Ω, where the two agree.

- Every limit is now judged one way, as the search judges it: its miss as a share of its value (of 1 for a value of 0), against
  "Limits met within". That covers the end of a limits-only run, the history's marks, and the penalty lines.
- `Goal::met` for EQ is a share of its target. It is the shared core's, so a schematic's optimization component's EQ goal has
  the fix too: there a target of 1 mV was met within 1e-9 V before, and is met within 1e-12 V now. A target of 1 V or more is
  judged as before.
- Apply refuses a best point that does not meet its limits, as it refused an infeasible status.
- Tests: `limitsInSmallUnits` (400 pF and 10 nF by six methods and penalties), `theHistoryMarksWhatBreaksALimit`,
  `penaltyLimitsAsTheSearchJudgesThem`.

| # | Finding | Now |
|---|---|---|
| B2 | Apply wrote the varied parameters and not those held at a start of their own, so the optimum did not reproduce | Those are written too, each marked "held at it in the run". A held one with no start is the model's own value and is left as it is. `heldParametersAreWrittenIn` solves there and gets the run's 200 pF. |
| B3 | A target's or a limit's value using a varied parameter was read once and frozen | Refused before any solve, naming the parameter and what to write instead (`ht.Tmax - Tb LE 50[K]`). Found by moving each varied parameter to its other end and reading the value again, so a dependence through another parameter is found too. Following the candidate was the other choice; refusing is plain and the rewrite is always possible. `aValueThatMovesWithAVariedParameter`. |
| B4 | `fem_optimize` applied a result of a changed model and said nothing | Its answer has "changed since", as its result already had. The tab's Apply still asks nothing: the page shows the result as old above the button. |
| B5 | A limit held by penalties read NOT MET at any miss, even 7e-10 pF beside an active bound | Met within "Limits met within", as above. |
| B6 | Apply wrote over a formula in silence, in 17 digits of SI | The line says "in place of its expression h/2", and a value with no unit of its own is written in 12 digits. `aFormulaWrittenOverIsSaid`. |
| B7 | The `undo` tool answered "Undone." on a `.qfem` and did nothing | It undoes and redoes the steps of any document that keeps an undo stack: a 2D or 3D model, an RF 2.5D model, a sheet, an archive. One with none says so, instead of "Undone.". Tested in `theToolsRunIt`, through undo and redo. |
| | `point` past a front's end got the message for no point | "The front has 12 designs: there is no design 99." |
| | A spread with no unit did not say that a share is taken | "... give it a unit ("0.02[m]") - or a share of its value: 5 %" |

The review's own runs were on the installed app; they were not repeated against it here. Its repro files are in the user's
workspace, and these tests are the same cases scaled to the test capacitor.

## The recheck of 7 November

[2026-11-07-claude-qfem-optimization-recheck.md](qucs-reviews/Multiphysics/2026-11-07-claude-qfem-optimization-recheck.md),
on the app of `4eaf02f`: all seven findings fixed, each original repro as it should be. It found one fault more, and four small points.

**M1: a row on a mode analysis read every mode, by the worst.** Confirmed: each objective was read on `instances(...)` with every
mode of the step, and reduced by the sweep's worst-point rule, as if the modes of one solve were points of a sweep. A TARGET on
`emw.neff` with two modes asked fitted the minimax of TE0's and TE1's misses, and said nothing. Now:
- A row reads one mode: by default the one its step chose, else the first, as a solve's globals and `fem_evaluate` read it.
- The objectives table has a seventh column, **Mode**: a mode's number from 1, or `all` for every mode by its worst, the old
  reading. An empty one is the default.
- `aModeAnalysisReadsOneMode` on a 500 nm silicon slab's two TM modes (3.15 and 2.07): the default and mode 1 read 3.15, mode 2
  reads 2.07. It fails on the code before, where every row read the same.

**The small points:**

| Point | Now |
|---|---|
| B3's suggested rewrite was the same text for every row | Built from the row: `(es.C11) - (200[pF]*d/1[mm]) TARGET 0` |
| A flat objective stopped quietly as `converged` | An objective the same at every point to rounding (1e-12 of its size) is `unchanged`, with its note, as an exactly equal one was. A finite element value meshed again differs in its last digits where nothing it depends on moved; the fork compares exactly, as a circuit's solve repeats to the bit. |
| The tab's Apply on an old result asked nothing | It asks, and No writes nothing (`savedAndOpenedAgain`). |
| Mesh noise was said only in this write-up | A run whose candidates were meshed again says so in its notes, and to check the optimum with a Mesh Convergence node or a finer mesh. It is said whenever the mesh is made again, not only when the history jumps: it is true of every such run. |

The schematic component's EQ goal and mid-run behaviour were not rechecked by the reviewer, nor here.

## Fixed on the way

A Parametric Sweep's **Unit** cell, edited alone, did not reach the model until another cell of the table was edited: `MultiphysicsPanel::parametersEditor` skipped every table's third column, a check meant for the parameters' read-only Value. It now skips that column in the parameters table alone. `aSweepsUnitIsCommittedAtOnce` in `test_multiphysics_doc` failed before the fix and passes after.

## Left open


- **`.Opt` could offer the ported methods.** An optimization component still runs DE alone on a schematic. The core is there for
  it; its `Optimizer` is the asynchronous one and would need a driver for them.
- **A warm start.** Each candidate solves from nothing. A semiconductor's walk from the last candidate's solution, as a sweep's
  points do, would save a lot where the mesh is the same.
- **Multiphysics 3D.** The core and the node's form carry over; `fem3`'s study runner and its own result reading do not.
- **The history as a dataset.** The page draws its own small plot. A Qucs dataset and diagrams, as a 1D plot group's, would let a
  front be plotted properly.
- **`tune`, the tool, and the core's `tune` are two searches.** Moving the tool onto the core needs its simulation calls turned
  inside out.
