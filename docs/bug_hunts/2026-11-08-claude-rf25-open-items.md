# Bug hunt: RF 2.5D's open items of 8 November, first batch

*8 November 2026 - on `main` at `719cd9ae`, in the worktree `claude/kind-goodall-0b21d2`. The items are the
reviewer's `qucs-reviews/RF2p5D/2026-11-08-claude-rf25-open-items.md` (its sources in
`archived/`, and `2026-11-08-claude-rf25-palace-benchmark.md`). This batch: B6, B1, B7, B2's warning, B4 and B3,
each fixed with tests built from its acceptance criteria and broken in turn. Still open: B2's physics, B5, the
accuracy items, the small items, the feature plans and the two QFEM findings' routing (end of this report).*

The order was agreed with the user after the triage: B6, B1, B7, B2's warning, B4, B3, B2's physics, B5.

**Two caveats of the review, checked.** The commits it cites (`71705df`, `b393b6e`, `e26e7a9`, `48d668d`) are in
no ref of this repository, so nothing was taken as fixed by them; RF 2.5D's source had not changed since
`3710593e`. The reviewer's models in the workspace are gone; the Palace benchmark's
are there (B5 and B6's were copied, never solved in place - a
solve writes its solution beside its model). The others were rebuilt from the reports' descriptions by the scripts
beside this report (`2026-11-08-claude-rf25-open-items/`): the MOM comb (`momcap.py`), a filter-like set of
touching shapes (`b1.py`), the shorted coplanar line (`b7.py`), the line drawn inside its pads (`b4.py`).

## Summary

| | Finding | Reproduced on `719cd9ae` | Fixed |
|---|---|---|---|
| **B6** | A crash in a backgrounded `rf25_solve` | `test_qucs_control` segfaults: the model deleted while `rf25_solve` waits on it | A model held while a tool waits on it, closed when free |
| **B1** | Touching shapes lose their connection in the full wave, silently | S21 -17.2 dB against -0.04 dB for the same outline as one polygon; passive, reciprocal, no word | The union joins what touches along an edge; the mesh joins pieces however drawn; a warning names any it cannot |
| **B7** | A port across one conductor cannot be modelled | Both refused | Allowed apart from the port's cells, said; refused on or beside them |
| **B2** (warning) | Thick metal in narrow gaps: no word that the walls' capacitance is missing | Nothing said on the comb (1 µm in 1 µm gaps) | Said at thickness/gap 1/4 and over, on both models |
| **B4** | One cross-section, two meshes: cells sized from the whole conductor's 2A/P | The line in its pads 1.4 % (3 across) and 3.3 % (6 across) from the line alone by L-2L | Slanted shapes cut at their corners, each piece by its own width; the line's mesh the line alone's |
| **B3** | GMRES stalls as the mesh is refined | Worse than reported: 10 and 12 across converge at no frequency | The near field's preconditioner a cell further; progress, a near miss and a sweep that goes on |

Every rf25 suite and `test_qucs_control` pass in Release; the full suite, the rf25 suites under ASan/UBSan and the
four lints are at the end.

## B6: the crash in a backgrounded `rf25_solve`

**The report.** `~/Library/Logs/DiagnosticReports/qucs-s-2026-11-07-181949.ips`, Qucs-S 27.0.6, main thread:
`EXC_BAD_ACCESS` at `0xbe867f7ae50d34da` ("possible pointer authentication failure": a freed block's poisoned
pointer) in `QucsControl::rf25Solve` +3868, directly under `QucsControl::call` from a posted event - so not inside
its wait loop, but after it. (The other report of that evening, `-214417`, is `LayoutDoc::~LayoutDoc` in
`QucsApp::closeFile`: the layout-close crash, fixed in `docs/bug_hunts/2026-11-07-layout-close-crash.md`.)

**The cause.** `rf25Solve` spins `RF25Doc::waitForSolved()` (`processEvents` every 20 ms) until the solve ends or the
call's timeout passes. Anything in that loop that deletes the model frees it under the call: Stop and Close at the
window, another conversation's `close_document`, or - the user's case, "sent to the background" - a tab kept running
in the background (Keep Running in Background) and let go once its solve ended (`Tracker::releaseOne`,
`trimParked`, `releaseParked`). The loop then reads `a_solveJob` from freed memory until the timeout, and
`rf25Solve` dereferences the freed model as it returns: some fifteen minutes in, as in the report, at
`rf25_solve`'s timeout of the reviewer's call. `MultiphysicsDoc` has had a guard for exactly this
(`beingWaitedFor()`/`closeWhenFree()`); `RF25Doc` had none.

**Reproduced:** `test_qucs_control::anRf25ModelClosedWhileItsSolveIsWaitedForIsLeftAlone` - the slow 40-film
model's `rf25_solve`, and a second conversation's `close_document` 0.8 s into it - segfaults on `719cd9ae` in
Release after its 300 s timeout.

**Fixed:**
- `RF25Doc::Held` holds the model from a tool's first wait to its answer (every `rf25_*` tool) and in each
  `waitFor...()`; `closeWhenFree()` lets its read, mesh and solve go, starts nothing, and deletes it once the last
  hold is let go; every wait ends at once when it is closed, and the tool answers "... was closed while this call
  waited on it".
- `QucsDoc` gains `beingWaitedFor()` and `closeWhenFree()` (virtual, nothing by default; `MultiphysicsDoc`'s two now
  `override`), and every place a document is deleted asks any document, not only a multiphysics model:
  `closeFile`, `closeAllFiles` and the close-tabs loop in `qucs.cpp` (the last two deleted even a waited-for
  multiphysics model), `releaseParked`, `trimParked` and `releaseOne` in `simulationruns.cpp`.
- The test passes in Release and under ASan/UBSan (no report); `beingWaitedFor()` made false fails it.

## B1: touching shapes losing their connection

**Two faults, found apart.**
1. **gdstk's union leaves pieces that touch along part of an edge apart.** `gdstk_merge_fuzz.cpp`: of 300 000 random
   sets of 2-8 rectangles on a 100 µm grid, each on a side of one already there, 4 777 (1.6 %) came out of
   `gdstk::merge` in more pieces than they are joined along edges - for example four rectangles in two, the lower
   pad's top edge `(-950..1550, 0)` lying on the big rectangle's bottom edge and left apart (Clipper's
   join of common edges). Inserting T-junction vertices first fixed none; merging the output again fixed 86 % and
   left 651 sets split; growing each group of touching pieces by the union's resolution (1 nm), uniting and
   shrinking back fixed all of them (it also joins corner-only contacts, so it is applied only to pieces that touch
   along an edge).
2. **The mesh joins two touching pieces only when both cut into rectangles.** A piece with a slanted side was gridded
   whole, its cells never sharing a whole edge with its neighbour's, so no run of current crossed. The four
   rectangles above join anyway; one of them chamfered does not.

**Reproduced** (`b1.py`, 1.6 mm εr 2.2 board, 2 GHz, 3 across): the chamfered set "merged 4 shapes into 2", S21
-17.18 dB, S11 -0.08 dB; the same outline drawn as one polygon -0.04 and -20.0 dB. Passive, reciprocal, nothing
said.

**Fixed:**
- `unionOf()` (`rf25_layout.cpp`): after gdstk's union, each group of output pieces touching along an edge
  (`touchAlongAnEdge()`, new) is closed by ±1 nm; said ("1 piece(s) the union had left touching along an edge joined
  to what they touch"). The chamfered set: one shape, S the one polygon's to the digits given (S21 -0.0448 dB).
- `meshOf()`: pieces that are not rectangles meet others along every stretch of their outlines that runs along x or
  y (their grid lines shared there), and along a slanted stretch both are cut at the same points of it (every line of
  either crossing it, both ways). Merge turned off, the chamfered pieces are joined by the mesh alone (S21 -0.053 dB),
  and a line cut along a slant is one line.
- Where two conductors of a layer touch along an edge and no cell of one shares an edge with a cell of the other
  (now: shapes left unmerged over each other), the mesh warns, naming both ("M1's piece about 500, 150 µm and M1's
  piece about 1000, 185 µm touch along an edge, but no cell of one shares an edge with a cell of the other, so no
  current crosses between them here ..."); the full wave's `warnings` carry the mesh's.

**Tests:** `test_rf25_layout::shapesTouchingAlongAnEdgeAreOneShape` (the four rectangles, area kept to 1e-6, the
report line, corners alone two, 3 000 random sets each as many shapes as pieces); `test_rf25_mesh::
touchingPiecesAreJoinedOrSaid` (the chamfered pad joined with no warning; a slanted joint whose every edge on the
slant is met from the other side; the overlap warned, both named; a gap not); `test_rf25_fullwave::
shapesDrawnTouchingSolveAsTheOutlineWhole` (the four shapes against the one polygon, Z to 1e-7 - the nanometre's
3e-9 -, S21 above 0.9; the slanted line a through; the overlap's warning in the solve's). Six breaks, all caught.

## B7: a port across one conductor

**Reproduced** (`b7.py`: the reviewers' coplanar line S 50 / G 20 / grounds 160 µm on 500 µm quartz, 1 mm, shorted
by a bar at its far end): `return: terminals` - "P1's return T1 is on the same conductor as P1 (Au): it would short
the source"; `return: beside` - "there is none either side of it".

**Fixed** (`rf25_fullwave.cpp`):
- `besideFaces()` takes the port's own conductor beside it where a gap on the edge's line lies between - never the
  conductor running on along the line with no gap (two strips drawn touching are still refused, "there is none
  either side of it").
- A named return on the port's own conductor solves, refused only on the port's own cells or the cells next to them
  ("P1's return T2 is on / next to the cells P1 drives: it would short the source").
- Both say it: "P1 returns through conductors beside it that are joined to its own further on - a shorted line, a stub
  to its ground, a loop - ..." / "P1's returns T1 and T2 are on its own conductor (Au), apart from its cells ...". The
  `rf_terminal` Returns tooltip and `fem_describe`'s format say the same.

**The physics, checked** (`stub.py`, 3 across, the tab's mesh): the shorted and the open line give Z_short Z_open =
Z0² and Z_short/Z_open = -tan²(βl), so Z0 74.7 / 75.4 / 77.0 Ω and εeff 2.428-2.432 at 10 / 20 / 30 GHz by the edge
port, 74.8 / 75.4 / 76.9 Ω and 2.435-2.439 by the GSG tips - the two ways within 0.3 % of each other, against
Palace's thin-metal 73.72 Ω and 2.4005 (the bar's and the open end's own fields in both, and three cells across).

**Tests:** `test_rf25_returns::aShortedLinesPortReturnsThroughItsOwnConductor` (both ways at 20 GHz: Z0 within 4 %
and εeff within 3 % of Palace, the ways within 1 %, the note; the contiguous case refused),
`whatAPortThatNamesItsReturnsCannotBeIsSaid` (on its cells, next to them, apart), and `test_qucs_control`'s through
the tool. Five breaks, all caught.

## B2: the warning (the physics is the next batch)

The comb (`momcap.py`: 12 fingers, W = S = 1 µm, 40 µm overlap, oxide εr 4.1 from a perfect plane to 8 µm, metal
bottom at 4 µm) reproduces the report within about 2 %: at 6 across, two faces 25.71 fF and the sheet 14.41 fF at 1
GHz against Palace's 31.8 fF (the report 25.08 and 14.05); thin 14.38 fF (the report 14.03).

**Fixed:** the mesh finds each layer's narrowest gap between facing pieces and, where the metal is at least a quarter
of it thick, says "M is 1 µm thick beside gaps as narrow as 1 µm: the field between its side walls is not in this
answer - its two faces' walls carry no charge (a sheet has no walls) - so the capacitance across its gaps reads low,
by some 20 % where it is as thick as the gap is wide" - in the mesh's lines and in `rf25_solve`'s `warnings`,
quasi-static (brief too) and full wave. Not at 0.2 of the gap, not for 3 µm in 50 µm gaps (0.06), not for a thin
film; nor on the Palace benchmark's CPW (2 µm in 20 µm gaps). The `model` tooltip no longer says two faces hold "the
walls' field across a narrow gap". `test_rf25_mesh::aMetalThickBesideNarrowGapsIsSaid`; three breaks, all caught.

**Found on the way, for the physics:** the two-face model at 0.01 µm (the acceptance's thin limit) does not solve
compressed: GMRES leaves a residual of 6e-5 to 2e-3 after 3000 iterations at any near-field reach - two sheets 10 nm
apart, which the next batch has to handle before that criterion can be checked.

## B4: one cross-section, two meshes

**Reproduced** (`b4.py`, `l2l.py`): the line drawn inside GSG pads with tapers (its signal and grounds polygons with
slanted sides) against the same line drawn as three rectangles, GSG tips 200 µm in from the line's ends, εeff by L-2L
(lines of 1000 and 1600 µm). On `719cd9ae`'s meshing (piecesOf off): the pads 1.40 % (3 across) and 3.32 % (6 across)
under the line alone at 10 GHz. One polygon of the filter, its 100 µm stub had cells of 503 µm ("3 cell(s) across
the narrowest conductor (1508 µm across)").

**Fixed** (`rf25_mesh.cpp`):
- `piecesOf()`, new: a shape with a slanted side is cut into slabs between its corners, across x or y - the way its
  slanted sides run least along the cuts, so that each is in a trapezoid as thin as it can be -, a slab's span a
  rectangle where both its sides run straight across it (carried on into the next slab where that has the same span)
  and a trapezoid else. Rectangles are meshed as strips, trapezoids by their own width (the slab's mean width, or its
  depth where less) and clipped to their own outline. A first try - cutting at every corner's x and y - was dropped:
  the ground pad's corner cut the ground near the line into a 25 µm strip (-0.74 %).
- Where a trapezoid meets the rest, lines go one way: from the straight strips to the trapezoids and from those to the
  other rectangles, each piece taking its stronger neighbours' lines where it meets them in place of its own (only
  their insides: a stretch's end at a slanted corner is no line of either), then shared both ways for whatever is
  left. Shared both ways throughout, a pad's lines came through its taper into the line across its width.
- Two pieces of one conductor with that conductor between them (a line and its own pad past its taper) face no gap.

**Acceptance: inpads against inplain, by L-2L at 10 GHz: +0.04 % at 3 across and +0.06 % at 6 (asked: well under
0.1 %).** At 30 GHz +0.15 % and +0.16 %: the line's mesh is the same cell for cell, so this is the two ends' own
structures (pads and tapers, open ends) not cancelling at 30 GHz - the coupling A3 looks at. The filter's stub has
three cells across.

**Tests:** `test_rf25_mesh::aLineIsMeshedAlikeHoweverItIsDrawn` (3 and 6 across: the cells along the line the line
alone's, its end cells too, every edge at the taper's joint met from both sides; the filter's stub),
`test_rf25_returns::aLinesEpsEffIsTheSameDrawnAloneOrInItsPads` (within 0.1 % at 10 GHz, 3 across: 2.38687 and
2.38786), `aConductorHasCellsAcrossIt`'s triangle now 100 µm across, not 58.58. Five breaks, all caught.

## B3: GMRES as the mesh is refined

**Reproduced** (the comb at 100 GHz): 36 iterations at 6 across, 108 at 8, and at 10 and 12 across no convergence -
a residual of 3.4e-7 after 3000, and 7.5e-7 at 1 GHz (the report: 1192 and 2834; its comb differs from the rebuilt
one by a few per cent).

**What the report's three tries did** (`thin_c10`, 100 GHz, all with S the same to five digits):

| | iterations | s |
|---|---|---|
| as it was (near field: half a box's side) | 3000, not converged | - |
| restart 500 | 923 | 63.8 |
| near field 1 µm in metres | 23 | 9.9 |
| near field 4 µm | refused: 44.8 GB a frequency | - |
| near field 2 sides | 24 | 6.0 |

The near field measured in cells is the lever; a physical distance costs memory that grows as the mesh is refined.
Between 1 and 3 sides (6, 8 and 12 across, thin and thick; `b3`'s tables): one side 26-33 at 6-8 across but 52-69 at
12; two sides 20-27 but the 6-across comb four times slower to set up (7.4 s against 1.7); three slower still.

**Fixed:** `kNear` 1.5 (`rf25_fullwave.cpp`). At 100 GHz: 23 iterations at 6 across, 24 at 8, 26 at 10, 28 at 12;
the thick two-face comb at 12 across (47 376 cells), which did not finish a frequency in 900 s, in 96 (1 GHz) and 121
(100 GHz) iterations; a 4-finger comb at 16 across 25 where it took 1253. S at 6 and 8 across, 1 and 100 GHz, the same
to every digit printed (1e-5 dB). And:
- **Progress:** GMRES tells the solve's control its iteration and residual as it goes; the tab's count reads "Solving
  full-wave: 0 of 1 frequencies… GMRES at iteration 1400, residual 3e-06." - the bar, the status line and
  `rf25_solve`'s timeout message, which quote it.
- **A near miss:** at 80 % of the limit or more, "At 100 GHz GMRES took 2834 of its 3000 iterations: near its
  limit ..." among the warnings.
- **A sweep that goes on:** a frequency short of its residual but under 1e-4 keeps its answer, said near and not
  exact, and the sweep goes on; past 1e-4 it is still refused.

**Acceptance:** `thin_c12` under 300 iterations at 100 GHz - **28**; `thick_faces_c12` completes - **yes**; S unchanged
to 1e-4 dB - yes where it converged before (6 and 8 across; 12 across did not converge before on this comb); the
low-frequency pad on vias against dense - its tests in `test_rf25_fullwave` and `test_rf25_fast` pass; the sweep of
34 frequencies under 200 s - **92 s** through `rf25_solve` (the report's 1340 s), at most 28 iterations, no warning. `test_rf25_fullwave::gmresKeepsItsHoldAsTheMeshIsRefined`
(the 4-finger comb at 16 across within 60 iterations, the progress counted, a limit just past what it needs said near,
held short of its residual both frequencies solved and said near, S within 1e-3); five breaks, all caught.

## The two QFEM findings, for routing (not fixed here)

- **mesh/size** (`qfem_size.py`, `qfem_size2.py`, on copies of `palace_benchmark/simulations/cpw_au_rlgc.qfem`): a
  domain size on the air (5 µm) or the quartz takes 20 830 triangles to 878 938 and 245 929, so domain sizes work on
  large domains; a size of 0.2 µm on the signal (50 × 2 µm) changes nothing where the model's `layered` feature covers
  that domain (20 830 → 20 830), and without it adds some 1 150 triangles where 0.2 µm needs some 5 000; a size at
  boundary level does nothing even with no other feature (7 789 → 7 765) or with the `distribution` on the same
  boundaries (20 830 → 20 952). (The reviewer's 16 148 every time is another state of the model; the pattern is the
  same.)
- **The mode search** (`qfem_modes.py`, `qfem_cpw_modes.qfem` at 70-100 GHz, 2 modes, neff 1.53): it is ordering, not
  a lost mode. A substrate mode (Z_PV 0.02-0.7 Ω, low loss) rises from n_eff 1.486 at 70 GHz to 1.585 at 100 GHz and
  crosses the CPW mode near 85 GHz; the modes come back sorted by n_eff, not by nearness to the neff asked, so above
  the crossing "mode 1" is the slab mode and the CPW mode is mode 2 - n_eff 1.5304 and 1.5303, Z_PV 69.9 and 70.0 Ω at
  90 and 100 GHz, continuing its curve. Sorting by nearness to neff, or following a mode by its field across a sweep,
  would answer the reviewer's runner.

## Still open, the next batch

- **B2's physics:** the walls' charge. The plan, measured so far: a 2D field solve (`walls2d.py`) of two conductors
  across a gap, solid against two faces with no walls, gives the excess per gap as h(t/S)·ε - 0.005, 0.016, 0.043,
  0.118, 0.34 and 0.96 at t/S 1/16, 1/8, 1/4, 1/2, 1 and 2 at S/64 (the reviewer's comb implies 0.36 at 1) - to be
  added as a lumped capacitance between facing wall cells: in the full wave a run of no length and no width (no
  inductance, its own impedance 1/jωC), in the quasi-static solve between the two nets.
- **B5**, the two-face CPW's εeff moving with the mesh: reproduced (2.3395 at 6 across, 2.4012 at 12 at 50 GHz,
  the report's to the digit). Note: the thin line's εeff by L-2L moves too here, 2.387 (3 across) to 2.470 (6) at 10
  GHz, against its quasi-static 2.40.
- A1-A4, the small items, and the plans for F1-F6.

## Checks of this batch

- Every rf25 suite and `test_qucs_control`, 18 of 18, in Release (after each fix, and on the final source).
- The same 18 under ASan/UBSan: no sanitizer report; `test_rf25_wave` past its 300 s with the machine at a load of
  about 50 from other sessions, and passing alone in 143 s (152 s in `c3749bea`'s run).
- The full Release suite: 180 of 181 at once, `test_canvas_drawing`'s paint timing (a step 8.1 ms against a whole
  paint of 20.1, under that load) the one, passing alone three times of three.
- The four lints in `scripts/ci/`: green.
- No new compiler warning in the files changed (the changed `.cpp` files rebuilt; the warnings left are older lines).
- 26 deliberate breaks, all caught: `breaks_results.txt`, made by `breaks.py` from the `breaks_*.json` beside this
  report (B6's run by hand: the same change).
