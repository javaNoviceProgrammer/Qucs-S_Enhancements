# RF 2.5D: the solver wired in - a Solve, a capacitance in the panel, and a tool

Four rounds of solver had landed with nothing calling any of it. `extract()` was library code with tests:
the panel showed no capacitance, the Results node said nothing, and Claude had no way to ask. This round
is the wiring, and it is deliberately ahead of the rest of phase B - a solver nobody can run gets no
feedback, and feedback is worth more right now than R and L on top of it.

## A Solve, and when it runs

The shapes and the mesh are made again at every edit because they cost nothing. A dense matrix of the
cells is cubic in them, so the solve is **asked for**: a `Solve` button on the tab's bar, which turns into
`Stop` while it runs. It works exactly as the layout read and the prepare do - a job on a thread of its
own, holding its own copy of the mesh, the conductors and the stack, so an edit while it runs changes
nothing under it, and letting it go is dropping the result.

The one rule worth naming: **a capacitance of shapes that are no longer there is worse than none.** When a
run of the processing or the mesh ends - which is to say after any edit that changes them - the extraction
is thrown away and a solve in flight goes with it. The Results node then says that nothing is solved
rather than showing a number of a layout that has moved on.

A line's parameters need a length, and the solve takes the longer side of what it solved - which is a
straight line's own length, and the honest default for a layout that is one. `rf25_solve` takes a
`length` of its own for anything else.

## What it says, and where

- **The tab's notes** list what it solved to under *What it solves to*, each net's capacitance and each
  net as a line.
- **The status line** carries the first line of that, so an impedance is in view without opening anything.
- **The Results node** in the panel shows the same, and says `Nothing is solved yet` when there is none -
  the node that the proposal gave the solver from the start, with something behind it at last.
- Anything the extraction refuses - a layer that is no metal, a mesh past the dense limit - appears under
  *What stands in the way of a solve* beside the processing's and the mesh's own refusals.

## The tool

`rf25_solve` is the one the proposal's *Claude's tools* asks for over phase B's solver. It drives **the
tab's own solve**, not the library underneath, so what a tool answers and what the panel shows are the
same run - there is no second path that can drift from the first. It waits for the layout read and the
mesh first, since the tab makes those at every edit, and answers:

- each net - a conductor, or several joined by a via (`M1+M2`) - with its capacitance to everything else;
- the capacitance matrix in its mutual form, and the same with every dielectric vacuum;
- each net as a uniform line: `C`, `C0`, `eps_eff`, `Z0`, `L`, `v`, `n_eff`;
- the lines the processing, the mesh and the solve each said, so the numbers come with their provenance;
- and a note saying plainly that this is the electrostatics alone - no resistance, no inductance of a
  loop, no S-parameters.

It is 134 tools now, and `check-tool-arguments.py` reads every key of it.

## Tested

| Test | What it holds to |
|---|---|
| `theModelIsSolvedAndSaysWhatItFound` | Nothing solved until asked; the button turning to `Stop` and back; a capacitance above zero over the layout's own 1200 µm; a line's pF/m and Z0 in the notes, the status line and the Results node; and the whole of it thrown away by an edit |
| `aSolveWithNothingToSolveOnIsRefused` | A model that names no layout refusing with "no mesh", in the notes and under the Results node |
| `anRf25ModelIsSolvedThroughItsTool` | `rf25_solve` over a real GDS: the model's name, the unknowns, the net, its capacitance, `Z0` and `eps_eff`; a `length` of its own halving C per metre when doubled; a length that is no length refused; and a schematic refused as no RF 2.5D model |

## Broken on purpose

`docs/feature_gaps/2026-10-24-rf25-solve-wired/breaks.py` holds nineteen: the button not solving; a solve
with no mesh running anyway; the length taken as the shorter side; the solve keeping the cells of the run
before; a capacitance of shapes that have gone kept; the button stuck on `Stop`; the notes, the status
line and a line's impedance each going quiet; the length not the one the solve used; the Results node
silent, or silent about having nothing; the panel not told when a solve ends; and six of the tool - not
waiting for the mesh, answering a model that is none, ignoring or accepting a bad `length`, answering
without the line, and answering nothing of what it did.

## What the breaks taught

One of the nineteen went uncaught, and it is the third time this exact mistake has shown up in the RF 2.5D
rounds: **a test that chooses the node anew cannot see a panel that was never told.** The report under the
Results node was checked by selecting another node and selecting it back, which rebuilds the form by
itself, so cutting the `solvedChanged` connection changed nothing. The node now stays chosen across the
solve - which is where a user's cursor would be, having just pressed the button next to it - and the
assertion is that the report changed without anyone touching the tree.

It is worth saying once more because it generalises past this codebase: when the behaviour under test is
"X is told", the test must not do X's job for it.

## Not done

- **Still the electrostatics alone.** R from the surface impedance, the partial inductances a loop needs,
  G from tan delta and the S-parameters are the rest of phase B, and the tool's note says so rather than
  letting a caller assume otherwise.
- **No export.** The `rf_exports` node and its Touchstone child exist in the tree and nothing writes one;
  there is nothing to write until there are S-parameters.
- **The currents are not drawn.** The proposal's workflow has them on the layout after a solve; the
  charge is solved for and thrown away once the capacitance is out of it.
- **One length for every net.** A layout with two lines of different lengths gets one `length` for both.
  The honest answer is per-net lengths, or the two-length differencing the tests use, and neither is here.
- **Nothing is kept with the document.** A solve lives in the tab until the next edit; a model reopened
  has to be solved again.
