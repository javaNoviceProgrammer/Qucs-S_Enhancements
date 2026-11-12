# RF 2.5D's Parametric Sweep

Phase 6 of the plan to give RF 2.5D a Parametric Sweep and an optimizer. It builds on the solve without the tab
(phase 2), the parametric geometry (phases 3 and 4) and the named values a solution gives (phase 5).

## What there is now

- **An Analyses node** at the top of the tree (`rf_analyses`, at most one), and under it **Parametric Sweeps**
  (`rf_sweep`). Phase 7 adds the Optimization beside them. A sweep holds:
  - **Parameters:** one or two of the model's parameters, each over a List of values (`30[um], 50[um], 80[um]`), or
    from From to To in Points, linear or logarithmic. Values are checked against the parameter's unit.
  - **Two parameters:** crossed (each value of the first with each of the second) or paired (the first of each, then
    the second, and so on).
  - **Quantities:** what each point reads, an expression of the solution's values (phase 5: `S21_dB`, `Z0`, `C11`,
    `GD21`...) and of the parameters. Each is read over a window From to To, Over its last (the default), its min or max,
    or At one frequency. A sweep has no goal, so a quantity cannot be read by its worst.
  - **Solves at once:** points solved together, 1 by default.

  A plan that cannot be run is refused, and why.
- **The run** (`runSweep()` in `rf25_optimize`):
  - Each point is the model with its parameters written in (`withParameters()`), solved as its tab solves it by its
    study (`solveModel()`), and its quantities read.
  - A point that does not solve is kept, with why.
  - A quantity no solution can give ends the run, with why.
  - A stop reaches the solves under way at once (`RunControl`), and keeps what was solved.
- **On the tab:**
  - **RF 2.5D > Run Parametric Sweep** runs the sweep chosen in the panel, else the first; `runAnalysis()` does the same.
  - The run is in the background. The bar counts its points, and its Stop stops the run. One runs at a time.
  - What it found is kept in its node (`result`, saved in the `.qrf25`). That is no edit to undo, but the model on disk
    is then not the one in the tab, and the tab says so.
  - What it found is shown in a page below the layout, when its node is chosen, as it runs, and when the model is opened
    again:
    - a summary (points, solved, how long, stopped);
    - a plot of the quantity chosen against the first parameter, with a line for each value of the second;
    - a table of the points, with a note of why for one that did not solve.
  - It is marked old once the model changes in anything but the parameters it varies (`analysisSetup()`).
  - **Write Touchstone Files…** (`writeSweepTouchstones()`) writes each full-wave point's S beside the model, as
    `<model>_<sweep>_<n>.sNp` with its parameters' values in the comments.

## Checked

- **`test_rf25_optimize`'s `aParametricSweepsPlan` (new):**
  - Two parameters crossed into six points, a logarithmic one, two paired.
  - The quantities' windows and Overs.
  - Ten plans refused, each with its reason.
  - A point's model.
  - A result kept as JSON and read back, its S with it.
  - What a result is of: not the parameters it varies or what it found, but the rest.
- **`test_rf25_doc`'s `aParametricSweepVariesTheWidth` (new)**, a strip of the model's own, L by w, quasi-static:
  - Run from the menu: one at a time, three points solved. Z0 falls and C11 rises with w, and the 50 µm point's Z0 is
    the model's own solved alone, to 1e-9.
  - The page: three rows, two quantities, a plot of three points.
  - Two at a time gives the same values exactly.
  - Saved, opened again and shown again.
  - Old after L is changed.
  - w by L crossed: four points, two lines.
  - A stop keeps fewer, and says so.
  - Full wave: two points, two Touchstone files with their w in the comments.
- **`test_rf25_optimize`'s `aStopReachesTheSolvesUnderWay` (new):** a stop reaches each solve under way, and stops a
  solve begun after it.
- All the RF 2.5D suites and `test_qucs_control` pass under ctest, as do the four lints.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.

## Not done

- The RF 2.5D panel has no Run on a sweep's node, and Claude has no tool for it yet (phase 8). Run it from the RF 2.5D
  menu.
