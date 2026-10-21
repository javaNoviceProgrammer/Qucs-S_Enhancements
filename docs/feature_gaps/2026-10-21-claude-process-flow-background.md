# A process flow off the GUI thread

The task (2026-10-21), from the fuzz hunt of the same day (`docs/bug_hunts/2026-10-21-fuzz.md`, N2):
*"Run process flows off the GUI thread"*. Ordinary flows - an implant, anneals and oxidations of half an
hour on windows of a few micrometres - froze the window for 5.7 to 71 s each time their model was opened,
and again after each change of their parameters or geometry.

## What it was

A process flow is a geometry feature: `MultiphysicsDoc::buildGeometry` ran it, on the GUI thread, with no
way to stop it. That was:
- on opening the model, and 120 ms after a change of its parameters, its geometry or (with a flow) its
  materials;
- on Build Geometry (the toolbar, the Multiphysics menu, the panel's Build All), and before Build Mesh and
  Compute;
- for the tools: `fem_build`, `fem_describe`, `fem_edit` and the others that report the geometry;
- for a step's state: choosing a step in the panel's tree (on every click), `fem_build` with `until`, a
  step's state as a solution (`fem_evaluate` and `fem_plot` of a step, the panel's plot buttons);
- for a Lumerical script's export, which builds the geometry and the mesh again.

The progress row and its Cancel were the mesh's and the study's alone.

## Now

**The flow tells how far it is, and stops when asked.** `runProcess`, `buildGeometry`, `processSolution`
and `lumericalScript` take a progress function, told:
- before each step, its share of the steps and the step at work ("Diffuse (Anneal), step 3 of 3");
- through each anneal, oxidation and intermixing, at each time step, its share of the step's time.

False from it stops the flow there, its error "cancelled". The anneal is asked through a ticker of its
own: its `hook` (the oxidation's interface) makes each time step stiff, so the numbers would have changed.
They are the same, told or not, to the last bit. A sweep's point whose geometry is built again (a
parameter of the flow swept) passes the study's progress on, so Compute's Cancel stops it there too.

**A geometry with a process flow is built on a thread of its own** (8 MB of stack, as the jobs'), beside
the mesh's or the study's job. Its progress shows in the row below the toolbar ("Building the geometry:
Diffuse (Anneal), step 3 of 3"), and Cancel stops it in its anneal. A model without a flow is built at
once, as before: in milliseconds.
- **Opening:** the tab opens at once; the structure is shown, and the view fitted, once it is built.
- **A change while it is built:** that build is stopped (it was of the model as it was) and one of the
  model as it is started. Its result is never shown.
- **Build Geometry** (the toolbar, the menu, the panel): returns at once; the structure is shown when
  built.
- **Build Mesh and Compute:** return at once; the mesh is made, the study solved, once the geometry is
  built - or not, and why is said.
- **A step chosen in the tree:** its state is built in the background and shown if it is still the one
  chosen. A newer choice stops the older.
- **Cancel:** the geometry stays stale, and is not built again by itself until the model changes or Build
  Geometry is asked: "The geometry's build was cancelled".
- **The panel** says "being built" for the geometry meanwhile.

**The tools wait for it, the window going on.** `buildGeometry()`, `geometry()` and `currentGeometry()`
wait in an event loop when a flow is built. So do `showBuiltUpTo()` (`fem_build until`),
`processSolutions()` (`fem_evaluate`, `fem_plot` and the panel's plots of a step) and a Lumerical export
(`fem_export`, the panel's Export). `waitForJob()` waits for the geometry and for what follows it: the
mesh, the study. `fem_stop` stops it, and `jobProgress()` (`fem_describe`) says how far it is.

**Closing a model while it is built** stops the build at its next time step. A model a tool is waiting on
- the tab can be closed meanwhile now - is not deleted under that tool. Its work is stopped, none started,
and it goes once the wait is over (`closeWhenFree()`).

**A step's state as a solution** is kept across changes that touch neither its geometry nor its mesh: a
plot of it, edited, is not built again.

## Not changed

- **`solutions()` of a step, asked by the GUI** - a plot group of it shown, derived values of it, the
  property editor's list of its points: built where it is asked, if it is not kept. The property editor
  asks while it builds the panel's form, and the plot groups as the model changes, inside the edit: a wait
  there would let the model change under the edit. The panel's plot buttons and the tools build it in the
  background first, and it is kept now. Shown again after a change of its geometry or mesh, it is built
  where asked.
- **A flow's run kept between builds** (the task's other way): not done. Keyed on all a flow reads (the
  flow, the materials and the library, the parameters, the model's folder, a layout's file), a key short of
  one would give a wrong structure, silently. Built in the background, its cost no longer stops the window.

## Measured

On the Release build, the fuzz hunt's flows: `open_document` returns in 0.1 s (it took 6.8, 20.7 and 71 s).
`fem_build` waits for the background build, 0.7 to 7 s for the tests' flows, with the window going on. A
Cancel in an anneal stops it within a time step: under half a second in Release.

## Tests

- `test_fem_process aFlowsProgressAndItsCancel`:
  - what the flow tells, each share no less than the last;
  - the dopants the same, told or not;
  - stopped before a step ("Implant: cancelled") and a third through the anneal (asked once more after);
  - the geometry's build and a step's state as a solution stopped the same way.
- `test_fem_process aSweepsFlowStopsInItsAnneal`: a sweep of the anneal's time, told the flow's steps, and
  stopped in the first point's anneal.
- `test_multiphysics_doc aFlowsGeometryIsBuiltInTheBackground`:
  - opened, it is being built at once, and the window goes on (a timer's ticks) while it is;
  - its progress is shown and said; it is built;
  - Cancel in the anneal stops it within half a second (5 s under the sanitizers), leaves it stale and does
    not build it again;
  - a change while it is built makes it built once, as the model is;
  - Build Mesh, Compute and the toolbar's Build Geometry return at once and finish after;
  - `fem_build what=mesh` and `fem_solve` after a change; `fem_stop` stops it;
  - a step chosen is built in the background and shown;
  - closed while it is built, at once; closed while `fem_build` waits on it, the tool is told and the model
    goes after.
- `test_multiphysics_doc aFlowsExportIsBuiltInTheBackground`: a Lumerical CHARGE script of a flow's model,
  the window going on, "Exporting" in its progress.
- The existing `aFlowsDopantsThroughTheTools` (`fem_build until`, `fem_evaluate` of a step) and
  `aBuiltGeometryIsNotBuiltAgain` now go through the background.

Under the sanitizers (a coarser grid there): `aFlowsProgressAndItsCancel` and `aSweepsFlowStopsInItsAnneal`
15 s together, `aFlowsGeometryIsBuiltInTheBackground` 55 s, `aFlowsExportIsBuiltInTheBackground` 19 s. The
whole suite passes in Release (135 of 135).

## Broken on purpose

`docs/bug_hunts/2026-10-21-fuzz/breaks.py "N2 GUI"`: 17 breaks, each one change of this undone. All are
caught:

| Undone | Caught by |
|---|---|
| A flow's model opened, built at once | `aFlowsGeometryIsBuiltInTheBackground`: not being built after opening |
| A tool's build of a flow here, not waiting | the same: `fem_build` not stopped by the tab closed under it |
| An anneal's time steps not asked | `aFlowsProgressAndItsCancel`: told 3 times, not through the anneal; the doc test: Cancel past 0.5 s |
| A flow's steps not asked | `aFlowsProgressAndItsCancel`: not stopped before the implant |
| The geometry's builder not passing it on | `aFlowsProgressAndItsCancel`: the geometry built though told no |
| A step's state not passing it on | `aFlowsProgressAndItsCancel`: built though told no in its anneal |
| A sweep's point not passing it on | `aSweepsFlowStopsInItsAnneal`: the flow's steps not told |
| A change not stopping the build | `aFlowsGeometryIsBuiltInTheBackground`: two builds shown, not one |
| Build Mesh waiting for the geometry | the same: Build Mesh returning once it is built |
| Compute waiting for the geometry | the same: Compute returning once it is built |
| The toolbar's Build Geometry waiting | the same: not being built after the click |
| A step chosen, waited for | the same: not being built after it was chosen |
| Closed, the build not stopped | the same: closing took 1.2 s, not under 0.5 |
| Closed, deleted under a wait (ASan) | the same: `fem_build` not told the model closed |
| Cancel not reaching the build | the same: Cancel took 1.2 s, the anneal run to its end |
| `fem_stop` not seeing the build | the same: "Nothing is being built or solved" |
| A flow's export here, not waiting | `aFlowsExportIsBuiltInTheBackground`: no tick of the window while it ran |

The first run left one uncaught: a step's state not passing the progress on. The test stopped it at half
its progress, which its mesh's part reached anyway. It now stops it in its anneal, by the flow's own words
("step 3 of 3"), which only the flow says.
