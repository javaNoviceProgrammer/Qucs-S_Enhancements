# RF 2.5D's analyses: Claude's tools and the panel

Phase 8, the last, of the plan to give RF 2.5D a Parametric Sweep and an optimizer equal to the 2D multiphysics one.

## The series

| Phase | Write-up | What it gave |
|---|---|---|
| 1 | `2026-11-12-optimizer-driver-shared.md` | The 2D optimizer's search run by any model's solve; its plan readers and Apply Optimum shared |
| 2 | `2026-11-12-rf25-solve-untabbed.md` | `solveModel()`: an RF 2.5D model solved as its tab solves it, on any thread |
| 3 | `2026-11-12-rf25-own-shapes.md` | Shapes of the model's own whose numbers are expressions of the parameters |
| 4 | `2026-11-12-rf25-dimensions.md` | Dimensions: the layout file's own corners moved by a parameter |
| 5 | `2026-11-12-rf25-solution-values.md` | A solution's values by name (S21_dB, GD21, Z0, C11...) and readings over a frequency window |
| 6 | `2026-11-12-rf25-parametric-sweep.md` | The Parametric Sweep, its page and Touchstone files |
| 7 | `2026-11-12-rf25-optimization.md` | The Optimization, its page, Apply Optimum, two shipped examples |
| 8 | this one | Claude's tools and the panel |

## What there is now

- **`rf25_edit`** (new tool) changes an RF 2.5D model node by node, as its panel does, each change one step of Undo:
  - `add` a node of a `type` under a `parent`, with its `label` and `properties`;
  - `set` a `node`'s properties (a null takes one back to its default), label, enabled;
  - `remove` a node.

  A node is named by its tag, its label, or its kind where the model has one of it (`analyses`, `shapes`). A property
  its kind lacks is refused, with the ones it has. Until now Claude could change an RF 2.5D model only by writing its
  file; writing a Shape's numbers or a Dimension's By as expressions is what this is for.
- **`rf25_sweep`** (new tool) runs a Parametric Sweep, as the RF 2.5D menu does:
  - It waits up to `timeout` seconds. Past that it says how far it has got; calling it again waits more, and
    `rf25_stop` stops it.
  - It returns the columns with their units, each point's values and quantities (at most `rows`), why a point did not
    solve, and whether the result is old.
  - `action` `result` reads what the node keeps; `touchstone` writes the full-wave points' files.
- **`rf25_optimize`** (new tool) runs an Optimization the same way and returns its result as the multiphysics
  `fem_optimize` does, with at most `rows` of its history and whether it is old. `action` `result` reads what the node
  keeps; `apply` writes the optimum (a Pareto front's design `point`) into the parameters, one step of Undo, and solves
  there.
- **`rf25_stop`** stops a sweep or an optimization running, as well as a solve, keeping what it has.
- **`fem_describe`'s format text** for RF 2.5D tells the new kinds and their properties: Shapes, Shape, Dimension,
  Analyses, Parametric Sweep, Optimization. It also lists the values their expressions read, and points at the new
  tools. Its kinds list has them already, from the kind table.
- **The panel:**
  - Its Add offers the new kinds where they go, from their kinds: Analyses under the model, a Parametric Sweep and an
    Optimization under it, Shapes and a Dimension under the Layout, a Shape under Shapes.
  - An analysis's menu has **Run** (Stop while it runs) and, once it has run, **Apply Optimum** for an Optimization or
    **Write Touchstone Files** for a Parametric Sweep.

## Checked

- **`test_qucs_control`'s `anRf25ModelIsOptimizedThroughItsTools` (new)**, on the shipped microstrip:
  - `rf25_edit` adds a sweep under `analyses` and sets it, and refuses a kind, a property and a node there are not.
  - `rf25_sweep` runs three points (Z0 falling with w), reads two back, and refuses a quasi-static sweep's Touchstone
    files.
  - `rf25_optimize` reaches 101.8 µm, reads back, and applies.
  - An optimization that cannot end on its own, its timeout passed, is running; `rf25_stop` stops it, and it is then
    "interrupted".
  - The format's text tells the new kinds.
- **`test_rf25_doc`'s `thePanelRunsAndAppliesAnAnalysis` (new):**
  - On a new model, Add offers Analyses, the sweep and the optimization, Shapes, the Dimension and the Shape.
  - On the shipped microstrip, Run from the Optimization's menu, Stop offered while it runs, then Apply Optimum (not
    offered before) writes w in.
- All the RF 2.5D suites, `test_qucs_control`, `test_layout_doc`, `test_fem_optimize` and `test_multiphysics_optimize`
  pass under ctest, and the four lints pass, with the tool lint counting 167 tools.
- On the Cocoa platform too: `thePanelRunsAndAppliesAnAnalysis` and `theShippedMicrostripReachesItsWidth` (the page as
  each solve ends, a stop, the result reopened, Apply Optimum) pass as offscreen.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.
