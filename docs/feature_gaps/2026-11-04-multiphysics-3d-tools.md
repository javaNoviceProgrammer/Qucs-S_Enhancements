# Multiphysics 3D: Claude's fem tools on 3D models, 4 November

Phase C's fourth part, after its time and derived values, its plot groups and its exports
(2026-11-04-multiphysics-3d-time-and-derived, -plot-groups, -exports). The proposal's "the `fem_*` tools on 3D
models". Until now a 3D model (.qfem3d) was refused by every tool, which said it was Multiphysics 3D's. Now the
same tools act on it, in its Multiphysics 3D tab. A model is taken as 3D by its tab, its file, or (fem_model new)
'space' 3D or a 3D model given whole.

## The tools

| Tool | On a 3D model |
|---|---|
| `fem_model` | `new`: a blank 3D model (`space` 3D), or a whole one as JSON (its format "qucs-s multiphysics 3d 1", or its component's space 3D), in a Multiphysics 3D tab, saved to `path` (.qfem3d added) when given. `open`, `save` (`as`), `set` (the whole model, one step of Undo) |
| `fem_describe` | `model`: its tree, parameters, what is built and solved (output times, sweep points), how far a job is. `geometry`: each domain's volume, bounds and a point in it; each face's domains, objects, area, kind (floor or wall) and normal; each edge's faces, length and middle - each by its number - and with `selection` and `level` (edge too) what a rule picks. `variables`: its variables and, solved, its globals. `format` (with `space` 3D, or a 3D model named or in front): a 3D model's kinds and properties alone - an Extrude's sketch kinds too, marked so - and a paragraph on the 3D file |
| `fem_edit` | Its nodes changed alike, each one step of Undo; its geometry built again in the background. A 3D model stays 3D: its space made 2D is refused |
| `fem_build` | The geometry built in the background and waited for: its domains, faces and edges. `what` mesh: its prisms, nodes, plan triangles, layers, smallest angle and worst stretch. `until` refused: a 3D model's geometry is built whole |
| `fem_solve` | Its study solved in the background, waited for (`timeout`): its seconds, unknowns, log, globals, output times, each sweep point's globals, each Derived Values node's values. Past `timeout` it goes on, said |
| `fem_stop` | What it builds or solves stopped |
| `fem_evaluate` | `at` points [[x, y, z]]; `along` a line ({from, to, points}); `over` domain, boundary (faces) or edge with `selection` and `kind` integral, average, maximum or minimum; else globals (several split by ';', each named if wanted) or a Derived Values node's values. At each output time and sweep point (`all`), or one (`time`, `point`); `unit`; `reference` measured against, as the 2D models' are |
| `fem_plot` | `plot` a 3D Plot Group; `expression` one made of it - its boundaries coloured unless `surface` is false, `slices` across x, y or z at coordinates, `isosurfaces` (a number of levels or their values), `clip` ({"x": 1.5}) -, one step of Undo; `show` geometry, mesh or solution; `camera` top, front, side or iso, `azimuth`, `elevation`, `zoom`; `time`, `point`. Returns what is shown, the camera, the legend (as shown, and in SI) and what does not read |
| `fem_export` | An export node of the model: VTU (a .pvd collection of several output times or points) and Field on a Grid in 3D. `copy` refused: it is a 2D line's Qucs part |
| `fem_converge` | Refused, said: it checks 2D models alone yet |

What is a 2D model's alone - a process flow's `step`, `frequency`, `mode`, `state`, contours, SIMS profiles - is
refused on a 3D model and said, never dropped in silence. So is another study than the one solved.

## Found and fixed on the way

- **The files a call writes, for the guard.** Before a call, the guard lists the files it will write and asks
  about any outside the workspace. That list knew only 2D models: a 3D model's save was named .qfem, not
  .qfem3d, and its exports were not listed at all. So a 3D export to a folder outside the workspace would have
  been refused when it was written (the write's own check), not asked about first. It lists 3D models' files now,
  by their own suffix and cache folder.
- **The plot group's range.** The tab kept the coloured range (SI) of the Solution view alone. A plot group's
  legend was text only, so a tool could not read it as numbers. It is kept for plot groups too
  (`Multiphysics3DDoc::plotRange()`), and `fem_plot` returns it.
- **Refusals that said the tools act on 2D models alone.** The 2D path's three messages ("fem_model makes 2D
  models", "the fem_ tools act on 2D models") now say what is true: a 3D model is made in a Multiphysics 3D tab,
  and what acts on 2D models alone says it is one. Two tests of the 2D suite had pinned the old refusals:
  `aThreeDModelIsRefusedByTheTwoDTools` now holds that a 3D model given whole makes a 3D tab, never a 2D one's,
  and `aThreeDModelsFileIsSaidToBeOne` that the tools act on a 3D file by its path and by its tab's title.

## Validated

The heated block: 2 × 1 × 0.5 mm, k 2 W/(m K), its bottom held at 300 K, 1e4 W/m² in through its top, its walls
insulated. T = 300 K + q z / k is linear in z, so order 1 gives it exactly.

| Check | What it holds |
|---|---|
| Made whole as JSON | A Multiphysics 3D tab, its file written; its geometry 1 domain, 6 faces (2 floors), 12 edges and 8 points; the rule {of blk, side top} picks its top, a floor at z = 0.5 |
| Solved | ht.Tmax 302.5 K and dT = ht.Tmax - ht.Tmin 2.5 K, named, to 1e-6 |
| At points, along a line | T at (1, 0.5, 0.25) mm 301.25 K; five points up the middle 300, 300.625 ... 302.5 K |
| Over its domain, its top, an edge | ∫ 1 dV 1e-9 m³; the top's average T 302.5 K; ∫ 1 along a 2 mm edge 2e-3 m; the domain's maximum 302.5 K |
| Against a reference | ht.Tmax against 302.5: one value measured, its difference under 1e-6 |
| An edit | Its height 1 mm: built again (bounds to z = 1), solved, ht.Tmax 305 K |
| Plots | A group of T with a slice at z = 0.25 mm, an isosurface at 301 K and a clip across x at 1.5 mm: its surface, slice and isosurface, the clip on the view, its legend 300 to 302.5 K in SI. Without its surface, slices at x = 0.5 and 1.5 mm of T - 300 K alone, its legend 0 to 2.5 K. The camera from above (elevation 90°); the mesh at azimuth 30° |
| Exports | A VTU added by `fem_edit` and written: its file, "<VTKFile" at its start |
| A sweep | q of 1e4 and 2e4 W/m²: two points, ht.Tmax 302.5 and 305 K, each named "q = ..."; its first point alone 302.5 K; a third refused |
| Refusals | A field with no place; `frequency`; another study; `until`; `fem_converge`; `contours`; `copy`; plotting before a solve. Each said |

## Tests

| Test | What it holds |
|---|---|
| `test_multiphysics3d_doc` `theToolsOnA3DModel`, `theToolsPlotAndExportA3DModel` (new) | The checks above |
| `test_multiphysics_doc` `aThreeDModelIsRefusedByTheTwoDTools`, `aThreeDModelsFileIsSaidToBeOne` (changed) | A 3D model is never a 2D tab's; the tools act on a 3D file by its path and its tab's title; fem_converge says it is 2D models' alone |
| `test_qucs_control`, `test_mcp_server`, `test_mcp_wishlist`, `test_claude_fuzz` | Unchanged, passing: the tools' schemas and descriptions (each within 2048 characters) |
| The full ctest | 161 tests at eight at once, in the worktree of 400820c with this change alone: 160 passed. `aThreeDModelIsRefusedByTheTwoDTools` counted tabs, and the window opens an untitled schematic when its last tab closes, so a suite with none open read one. It counts 3D tabs now; `test_multiphysics_doc` whole, 88 functions, passes |

## Broken on purpose

`docs/feature_gaps/2026-11-04-multiphysics-3d-tools/breaks.py` holds 18 breaks. It ran in a detached worktree
of 400820c with this change alone, so the RF 2.5D session building from the checkout never built a broken
source:

- **The dispatch:**
  - fem_evaluate not sent to a 3D model;
  - a whole 3D model's format and space not read;
  - its space made 2D by fem_edit;
  - its mesh not built by fem_build.
- **fem_evaluate:**
  - points taken in metres, not the geometry's unit;
  - a line's points short of its end;
  - 'over' edge taken as faces;
  - 'kind' left out;
  - a global's name left out;
  - a reference not measured;
  - a sweep's point counted from 0.
- **fem_plot:**
  - a slice's planes across z whatever is asked;
  - its clip not set;
  - 'surface' false not heard;
  - the camera's top the front;
  - a plot group's range not kept.
- **The format:**
  - every kind listed for a 3D model;
  - an Extrude's sketch left out.

All 18 are caught at their first run, each at its own check. The worktree was rebuilt after the breaks. The four
lints are green (140 tools, no disagreement between a tool's schema and what its handler reads), and nothing was
written to the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan:

- `theToolsOnA3DModel` and `theToolsPlotAndExportA3DModel` together: 10.6 s.
- The three changed 2D functions: 2.7 s.

No sanitizer said anything.

## What phase C still lacks

- **Estimates and a limit** before a solve.
- **A user's anisotropic tensor, Newton's method, parallel assembly.**
- `fem_converge` on 3D models: refining a 3D mesh by its sizes and its distribution in z.
