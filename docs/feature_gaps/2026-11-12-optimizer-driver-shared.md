# The 2D optimizer's driver, opened to another kind of model

Phase 1 of the plan to give RF 2.5D a Parametric Sweep and an optimizer equal to the 2D multiphysics one (the user:
"Plan it first", then the plan approved, then "Do this task here").

The 2D optimizer (`multiphysics/fem_optimize.{h,cpp}`) was one block. Its search loop - the cache, the scales, the
limits' rounds, the polish and the starts, design centering, the NSGA-II front, the result - did not depend on the
model, apart from one step: solving a candidate and measuring the objectives on it. Its plan reader did not either,
apart from the studies. Both are now open, so that RF 2.5D can supply its own solve and its own objective columns. What
a 2D model does is unchanged: its tests pass as they were, and the shipped capacitor example reaches the same optimum.

## The new entry points (`fem_optimize.h`)

- **`runOptimizationPlan(plan, parameters, solve, progress, listener, lastNotes)`** runs a plan with any solver.
  - `solve` is an `OptimizationSolver`: it takes an `OptimizationCandidate` and returns an `OptimizationMeasured`.
  - Called on worker threads, several at once when the plan says `parallel`.
  - `lastNotes` adds to the result's notes what only the solver knows, after the run.
- **`OptimizationCandidate`**:
  - `overrides`: each varied parameter's value, as an expression in SI with its unit;
  - `progress`: how far its solve has got; when it returns false, the run is being stopped;
  - `stopped()`: whether the run was asked to stop.
- **`OptimizationMeasured`**:
  - `solved`, `stopped`, `fatal` (a value no candidate could give), `why`;
  - `y` and `dims`: each objective's value and unit, with a sweep's points already judged by the worst or the last;
  - `warnings`: told once among the result's notes;
  - `yield` and `cpk`, the driver's own, for design centering.
- **The plan readers.** `optimizationPlan()` for a 2D model is now made of these, in this order:
  1. `readOptimizationSettings()`: the mode, the method, every setting.
  2. `readOptimizationVariables()`: the rows of parameters to vary, each checked against a list of the model's
     parameter names.
  3. `readOptimizationObjective()`: an objective row's first four cells (expression, kind, value, weight). A row with
     no expression is skipped, not refused. The cells past those are the caller's: a 2D model reads Over, Study and
     Mode; RF 2.5D will read its own frequency window.
  4. `checkOptimizationObjectives()`: something to reach; within the counts of targets and limits.
  5. `checkOptimizationPlan()`: what the method can take; design centering's spreads; the method's warnings. It also
     refuses a target's or limit's value that moves with a varied parameter, through an `evaluate` callback that gives
     the parameters with overrides.
- **Parameter rows, not a whole model:**
  - `evaluateParameters(QList<Model::Parameter>, overrides)` and `evaluateParameterRows()` (`fem_model.h`). The 2D
    model's own version evaluates its functions first, then the rows.
  - `parameterText(rows, ...)`, `optimizationVariableRow(rows, ...)`.
  - `applyOptimum(rows, parameters, node, plan, point, ...)`. The 2D overloads wrap these.

`OptimizationRow`, `OptimizationResult` with its JSON, and the results page (`multiphysicsoptimization.{h,cpp}`) are
used as they are.

## What moved

- The 2D candidate's solve is `FemSolver` in `fem_optimize.cpp`. It holds the model, its topology and mesh made once
  and shared, which parameters shape the geometry, and whether a candidate was meshed again.
  - Its "each candidate was meshed again" note is its `lastNotes`.
  - Its studies' warnings come back in `OptimizationMeasured::warnings`.
  - It looks at `stopped()` between studies, as it did at the run's own flag.
- `Runner` holds the plan, the parameters, the solver and the notes callback, and nothing of a 2D model.

## Checked

- `test_fem_optimize`, `test_multiphysics_optimize` and `test_optimizer_core` pass unchanged under ctest. That includes
  `theShippedExample`, the capacitor optimum.
- **New `anotherModelsPlanRunsByItsOwnSolve`:**
  - A plan is read from a bare node by the shared pieces.
  - A closed form, f = (x − 1)² + (y + 2)² with x + y ≥ −0.5, is solved by its own callback four at a time.
  - The optimum on the limit, (1.25, −1.75), is found within 0.02.
  - The solver's warning is told once, and its last note comes after the others.
  - The optimum is written into the parameter rows in their unit.
  - A limit whose value moves with a varied parameter is refused.
  - A stop said through a candidate's progress keeps the best point. The candidate sees `stopped()`.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.
