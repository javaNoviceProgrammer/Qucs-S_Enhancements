# RF 2.5D's solve without its tab

Phase 2 of the plan to give RF 2.5D a Parametric Sweep and an optimizer (phase 1:
`2026-11-12-optimizer-driver-shared.md`). A sweep's point and an optimization's candidate are a model solved on a
worker thread from a copy: they cannot go through the tab, which solves the one model it shows, one solve at a time,
with its progress bar and its kept solution. Until now the solve lived inside the tab (`RF25Doc::solveWith`, its
`SolveJob`, `solveWaveOn`, `trlOn`, `convergeOn`). Only the TRL standards had a path from a model file to an answer,
and that path was a shortened one: raw full wave, with no de-embedding, no adaptive sweep, and no quasi-static solve.

## What there is now: `rf25/rf25_solve.{h,cpp}`

- **`SolveRequest` and `SolveJob`** moved there as they were. `RF25Doc::SolveRequest` and `RF25Doc::SolveJob` are
  aliases for them, so the tab, `qucscontrol_rf25.cpp` and the tests are unchanged.
- **`setUpSolve(job, model, processed, mesh, request, folder)`** is what the tab's Solve read out of the model:
  - the conductors, the stack, the layers that end, the Return Conductors;
  - the ports, with a word for a terminal no port's returns name;
  - the frequencies, the matrix, the adaptive sweep and its tolerance;
  - a TRL calibration's standards, found from `folder`, the model's own folder;
  - else the loop from the first two ports, for the inductance.

  It returns empty, or the reason the model cannot be solved, in the tab's own words. The stamps and the test holds stay
  the tab's.
- **`runSolve(job)`** is what the tab's thread ran:
  - full wave: de-embedded, adaptive and calibrated as asked;
  - else the quasi-static extraction and the loop's inductance;
  - in both cases the mesh's note goes first.
- **`solveWaveOn`, `trlOn` and `convergeOn`** moved as they were, from the tab's static members.
- **`solveModel(job, model, layout, called, folder, request)`** does the whole of it on the caller's thread:
  1. processed (`processRead`);
  2. meshed as the tab meshes (`tabMeshSettings`: compressed as its study holds it, gaps graded);
  3. set up and solved.

  It returns a `ModelSolution`: the processed shapes, the mesh, the extraction, the inductance or the full wave, the
  ports' notes, the line's length as the tab says it, the problem, and whether it was stopped. Another thread can stop
  it through `job.stop`. A convergence check stays the tab's.
- **Moved with them:** the helpers the solve reads the model by. These are `studyMatrix`, `meshedCompressed`,
  `tabMeshSettings`, `returnMarksOf`, `portsOf`, `facingOf`, `studyIsFullWave` and `meshNoteOf`, plus, used inside,
  `solvedStandard`, `rawS` and `returnsOf`. Their messages keep the tab's translation context, so every message reads
  as before.

The tab's `solveWith` keeps its waiting for an edit's mesh, its threading, its progress bar and its kept solution. It
calls `setUpSolve` and runs `runSolve` (or `convergeOn`) on its thread.

## Checked

- **New `aModelSolvedWithoutItsTabIsTheTabsAnswer`:** a strip with two ports, solved by the tab and by `solveModel` from
  the same model and layout.
  - Full wave: the same mesh, frequencies and port names; every S term within 1e-12, every Z within 1e-12 of itself;
    the same first line.
  - Quasi-static: the same nets, capacitance matrix and per-metre Maxwell matrix exactly; the same length; the same
    loop inductance.
  - A coplanar line, short and wide (150 µm long, 332 µm across, 6 µm gaps), quasi-static:
    - the same mesh, so its gaps are graded as the tab grades them;
    - the same capacitance;
    - its length is its own along the line (150 µm), not the longer side solved.
  - A stop set before the solve begins gives nothing and says it was stopped.
  - A layer map that sends nothing to the stack gives no mesh, and says why.
  - A convergence check is refused.
- `test_rf25_doc` and the other RF 2.5D suites pass under ctest, as do `test_qucs_control` and the four lints.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.
