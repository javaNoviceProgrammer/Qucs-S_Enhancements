# RF 2.5D's Optimization

Phase 7 of the plan to give RF 2.5D a Parametric Sweep and an optimizer equal to the 2D multiphysics one. It stands on
the shared driver (phase 1), the solve without the tab (2), the parametric geometry (3, 4), the named values (5) and
the Analyses node with its page and run (6).

## What there is now

- **An Optimization node under Analyses** (`rf_optimization`).
  - Its settings are the multiphysics Optimization's, copied from that kind's own definitions, so their defaults, labels
    and help are the same and cannot drift apart:
    - mode, optimize or design centering;
    - method, any of the multiphysics optimizer's (`de` and its ten variants, `pso`, `sa`, `cmaes`, `bayes`, `nm`, `tr`,
      `lm`, `nsga2`, `tune`, or automatic);
    - iterations, tolerance, population, seed, polish, more starts;
    - limits by augmented Lagrangian or penalty, and their weights;
    - DE's variant, F and CR;
    - LM's finite difference;
    - solves at once;
    - the parameters to vary, with ranges and types (linear, logarithmic, whole numbers, E-series), and design
      centering's spreads.
  - Two things differ. It has no study, since an RF 2.5D model has one. Its objectives have a window of frequencies in
    place of a study and a mode: Expression, Kind (MIN, MAX, TARGET, LE, GE, EQ, MON), Value, Weight, Over (worst, the
    default; last, min, max, at), From, To, At.
- **Its plan** (`optimizationPlan()` in `rf25_optimize`) is read by the shared pieces of phase 1, with the kind's
  defaults filled in where the node says nothing. The shared readers read a node by the multiphysics kinds, which do
  not know this one. A plan that cannot be run is refused as a multiphysics plan is: a parameter the model has not, a
  method that cannot take what is asked, a limit whose value moves with a parameter varied, and so on.
- **The run** (`runOptimization()`) is the multiphysics search itself (`runOptimizationPlan()`):
  - Each candidate is the model with its parameters, solved as its tab solves it, and each objective is read as its
    Over says. Worst is judged by its goal, with a target's value read in the unit of what it reads.
  - A candidate tells its progress before and after its solve, as a multiphysics solve does. Without that, a Stop
    reached each solve but not the search, which ran on to "converged" on stopped candidates. A test caught this, and
    it is fixed.
- **On the tab:**
  - **RF 2.5D > Run Optimization** runs the Optimization chosen in the panel, else the first; `runAnalysis()` does the
    same.
  - The run is in the background. The bar shows how far the search has got, and Stop keeps the best point so far.
  - The tab's page is the multiphysics Optimization's (`OptimizationPage`), used as it is. Each solve is told to it as
    it ends. When the run ends it shows the summary, the best values, the limits at the optimum, the history and a
    Pareto front.
  - The result is kept in its node and saved with the model, shown again on opening, and marked old once the model
    changes.
  - **Apply Optimum**, the page's button or `applyOptimum()`, writes the best values into the parameters, each in the
    unit its expression has, as one step of Undo, then solves the model there.
- **Two examples** in `examples/rf25/` (new):
  - **`microstrip_width_optimization.qrf25`:** a strip on 100 µm of alumina whose width is set for 50 Ω, quasi-static.
    Hammerstad and Jensen give 97.2 µm for a strip of no thickness. The model's mesh (6 cells across) gives 101.8 µm,
    and finer meshes approach about 99.5 µm:

    | Cells across | 3 | 6 | 12 | 16 | 24 |
    |---|---|---|---|---|---|
    | Width (µm) | 104.9 | 101.8 | 100.4 | 100.0 | 99.7 |

    16 cells across costs about 7 s a solve, so the example uses 6.
  - **`open_stub_notch_optimization.qrf25`:** a 50 Ω line with an open stub at its middle, full wave at 10 GHz; the
    stub's length is set for the notch. The closed form (a quarter guided wavelength, less the line's half width and the
    open end's extension) gives 2.85 mm. The model finds 2.96 mm, with |S21| 0.04 there.

## Checked

- **`test_rf25_optimize`'s `anOptimizationsPlan` (new):**
  - The kind has the multiphysics settings with their defaults, no study, and its own columns.
  - A plan reads the kind's defaults, three objectives' Readings and a weight.
  - Six plans are refused, each with its reason.
- **`test_rf25_doc`'s `theShippedMicrostripReachesItsWidth` (new), about 25 s:**
  - Run from the menu, the page shows as it runs and the history has a row for each solve.
  - Nelder-Mead, differential evolution and Bayes each reach 101.8 µm, within 6 % of Hammerstad and Jensen, with Z0
    within 0.5 Ω of 50.
  - A limit of 80 µm is held.
  - NSGA-II gives a front.
  - A stop gives "interrupted" with the best kept.
  - The result is saved and opened again.
  - Apply Optimum writes w in µm and solves there; Undo puts it back.
- **`test_rf25_doc`'s `theShippedStubFindsItsNotch` (new), about 60 s:** Nelder-Mead finds the notch within 7 % of the
  closed form.
- `test_fem_optimize` and `test_multiphysics_optimize` still pass, as do all the RF 2.5D suites, `test_qucs_control`
  and the four lints.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.

## Not done

- The RF 2.5D panel has no Run or Apply on an Optimization's node, and Claude has no tool for it yet (phase 8). Use the
  RF 2.5D menu and the page's Apply Optimum.
- Design centering and the methods not run above (`pso`, `sa`, `cmaes`, `tr`, `lm`, `tune`) are the multiphysics
  optimizer's, which tests them. Here each candidate is only another solve.
