# Bug hunt: RF 2.5D's open items of 8 November

*8 November 2026 - on `main` at `719cd9ae`, in the worktree `claude/kind-goodall-0b21d2`. The items are the
reviewer's `qucs-reviews/RF2p5D/2026-11-08-claude-rf25-open-items.md` (its sources in
`archived/`, and `2026-11-08-claude-rf25-palace-benchmark.md`). Two batches: the first (commit `2849717a`) B6, B1,
B7, B2's warning, B4 and B3; the second B2's physics, B5, the small items, and the accuracy items and feature plans
reported. Each fix has tests built from its acceptance criteria and was broken in turn.*

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
| **B2** (physics) | The side walls' charge missing | The comb 25.7 fF against Palace's 31.8 | A capacitance between facing wall cells from a 2D solve: 31.40 fF, the rise 1.137; films solved as a sheet |
| **B5** | The two-face CPW's εeff moving with the mesh | 2.3395 (6 across) and 2.4012 (12) | Was the compressed solve of the ports' calibration strips: 2.3521 and 2.3602 since B3; a strip's own warnings now said |
| Small items | All eleven | - | A face terminal off the metal refused, an idle terminal and overlapping returns said, the count at a time, datasets named, two `set_dialog` faults; then the bar in `get_ui`, the settings in `get_settings`, the first `show_layout`'s overlays, and memory offered back after a Stop (held by macOS's allocator) |

The accuracy items A1-A4 are modelling limits or not reproducible here, with numbers; F1-F6 are planned, not built.
The checks of each batch are at the end.

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

## B2: the warning (the physics: the second batch)

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

## Second batch

### B2's physics: the side walls' charge

**Fixed.** Neither the sheet nor the two faces held charge on a metal's side walls. Now, for a metal solved as its
two faces, each wall along x or y faces the nearest wall across a gap no wider than 16 times the thickness. The
two walls' edge cells get a capacitance between them (`wallGapsOf()`, `rf25_mesh.cpp`): the gap's length times
ε0·εr times h(t/S). The εr is that of what fills the room beside the walls. h(t/S) is the excess a 2D solve finds
of solid metal over its two faces with no walls (`walls2d.py`, finite differences at S/32 and S/64, extrapolated in
the grid): 0.008, 0.020, 0.049, 0.126, 0.349, 0.981 and 2.60 at t/S 1/16, 1/8, 1/4, 1/2, 1, 2 and 4, as t/S squared
below and nought under 1/32 (`wallExcess()`).
- **The full wave:** each such capacitance is a run of its own after the vias, with no length, direction or
  inductance and an impedance of 1/jωC. The dense and compressed solves take it as a run: its own impedance and its
  charges, no kernel. It is said: "The side walls across their narrow gaps ...: 1483 capacitance(s) between facing
  edge cells, 5.742 fF in all ...".
- **The quasi-static solve:** the capacitance goes into the Maxwell matrix between the two nets. The vacuum matrix
  takes it over εr, and the per-metre matrices take those gaps whose walls run along the line through its middle.
  This happens after the lossy layers' derivatives, so a wall's field in a lossy layer is not in the loss.
- **The thin limit:** two faces under 2 % of the layer's narrowest conductor's width (2·area/perimeter) are a film,
  solved as a sheet and said. 0.01 µm on the comb's 1 µm fingers, as two faces, took GMRES 2492 iterations (the
  small comb) or never converged (the reviewers' comb) and read 1.5 % over the sheet. A first try measured against
  the finest cell instead turned 3 µm metal on 12.5 µm cells into a sheet and failed six tests; the conductor's
  width is what decides whether a metal is a film.
- B2's warning now says, for two faces, that the walls are in the answer from a 2D solve of straight walls; for a
  sheet it keeps saying they are missing, and that two faces would have them. The `model` tooltip says the same.

**Acceptance:**

| | asked | found |
|---|---|---|
| The comb, 1 µm metal, 6 across | within 3 % of 31.8 fF | **31.40 fF (-1.3 %)** in the test, 31.44 through the tool; 3 across 30.84 (-3.0 %) |
| C(100)/C(1) | within 1 % of 1.135 | **1.1368 (+0.2 %)**; at 100 GHz 35.70 fF against Palace's 36.1 (-1.1 %) |
| The thin limit, 0.01 µm on two faces | the sheet's, not over 14.45 by 1 % | **14.365 fF, the sheet's 14.364**: -0.6 % from Palace's |
| Quasi-static against full wave | - | 30.737 against 30.719 fF at 3 across |
| 3 µm in 50 µm gaps keeps its 2-3 % | unchanged | 50 µm is past 16 thicknesses (48 µm): no wall added; in 40 µm gaps h is 0.0098, about a hundredth of ε per metre |
| The endless comb, per gap | within 3 % of 74.3 pF/m | **not met as stated**: 67.1 pF/m (4 across, eight 40 µm strips, the middle strip's mutuals to its odd neighbours) |
| The warning | at t/S 1, not at 3/50 | yes (first batch), reworded for two faces |

On the endless comb: the same strips as thin sheets give 30.3 pF/m per gap, where the reviewer's finite-difference
figure for thin metal is 36.3, and that figure is hard to square with Palace's own comb. Eleven gaps of 40 µm at
36.3 pF/m are 16.0 fF before the fingers' tips and the buses, against Palace's 14.45 fF in all; RF 2.5D's 30.3 gives
13.2 fF plus the ends, its 14.38 within 0.5 % of Palace. Read as thick over thin, the reviewer's ratio is 2.05 and
RF 2.5D's 2.21 (+8 %): the 2D excess is of an isolated gap, where a comb's walls also face a second neighbour's.
The finite comb, which Palace solved, is within 1.3 %.

**Tests:** `test_rf25_mesh::theWallsAcrossANarrowGapAreItsCapacitance` (the table, three strips' two gaps of 40 µm
each the closed form, nearest walls only, 3 µm in 40 µm about 0.01 ε, a sheet none);
`test_rf25_fullwave::theSideWallsChargeIsInAThickCombsCapacitance` (the comb at 6 across, the rise, the
quasi-static against the full wave at 3 across, the thin limit, both said). Seven breaks, all caught.

### B5: the two-face line's εeff moving with the mesh

**Cause, found:** the compressed solve, not the line. The CPW's ports are calibrated by strips of their feeds, 4.1,
8.2 and 6.6 mm long, which at 12 across are past the dense limit and solved compressed. With the near field
reaching half a box's side (main), GMRES on them crawled: this branch with `kNear` set back to 0.5 reads εeff
2.4098 at 50 GHz in 601 s, and main read 2.4012 in 467 s. With the first batch's B3 change (`kNear` 1.5) the same
model reads 2.3602 in 84 s (2.3514 with the walls turned off). The installed 27.0.8, whose RF 2.5D source is main's,
gives 2.3514 in 54 s from the same copy of the model. So the path through the solver depended on something outside
the source, most likely the RF 2.5D settings the app reads (the dense limit decides whether the strips go dense);
that was not pinned down.

**On this branch, 50 GHz** (copies of the Palace benchmark's models):

| | 6 across | 12 across |
|---|---|---|
| `cmp500/rf25_cpw_c*` (2-100 GHz study), one length | 2.3521 | 2.3602 (+0.34 %) |
| `rf25/cpw_auf_c*` (5-50 GHz), one length, 500 µm | 2.3527 | 2.3602 |
| the same, L-2L 500 / 1000 µm | 2.3538 | 2.3612 |
| Palace 2D / QFEM | 2.345 / 2.350 | |

The walls add 0.38 % here (2 µm in 20 µm gaps, t/S 0.1). Z0 72.26 Ω at 12 across against QFEM's 70.44 and Palace
2D's 70.35 (+2.6 %; 72.40 without walls): A1, below.

**Also fixed:** a strip whose GMRES stops short of its residual (taken since the first batch, said in its own
solve's warnings) was used by the calibration with nothing said. Its warnings are now the calibration's and the
solve's ("A calibration strip 4100 µm long: At 50 GHz GMRES stopped short ..."):
`test_rf25_deembed::aCalibrationSaysWhatItsStripsSolvesSaid`, one break, caught.

**And a measurement caution found on the way:** εeff by L-2L from GSG tips 10 µm square on a thin line, at 3, 6, 9
and 12 across, came out 2.387, 2.470, 2.437 and 2.382, not monotone. A tip covers a part of a cell whose length
along the line changes with the mesh and with the line's length, so the two lengths' ends are not alike, which L-2L
needs. Edge ports at the lines' ends (B5's models) do not have this, and B4's pairs compare like with like.

### The small items

| Item | Done |
|---|---|
| A face terminal on no conductor solved | **Fixed**: refused unless within a thousandth of the face found ("is on no face of M1"); it took the nearest face facing its way, however far |
| A terminal nothing names solved without a word | **Fixed**: "T9 is named by no port's returns, so it does nothing in this solve." in the solve's notes |
| Two returns over the same cells solved without a word | **Fixed**: "P1's return T7 is over cells that T1 is over too: ... those cells carry it twice." among the warnings |
| The count sits at "0 of 11" when the frequencies run at once | **Fixed**: "Solving full-wave: 0 of 3 frequencies… 3 at a time, each counted as it ends." |
| Every exported circuit's dataset `equivalent.dat` | **Fixed**: the schematic's own name (`wave_part.sch` → `wave_part.dat`, `.dpl`) |
| `set_dialog`: "Held dense up to does not take Automatic", the dialog then reading Automatic | **Fixed** (`qucscontrol.cpp`, any spin box): a spin box takes its special value's words, and a word it cannot read sets nothing (it was set to nought, which read "Automatic") |
| "it took 20000 runs and cells, not 20000" | **Fixed**: a spin box's value compared without its prefix and suffix |
| `get_ui statusbar` does not see the bar | **Fixed (third batch)**: `get_ui statusbar` gives "document's progress", the tab in front's bar (its count or busy, and what it is doing); `get_ui` reads any progress bar as a control of kind "progress" |
| Memory not given back after Stop (18.9 GB) | **Asked for, and a macOS limit (third batch)**: freed memory is offered back as a solve's thread goes, stopped or not (`misc::releaseFreedMemory()`). Measured on the comb at 12 across, two faces, 25 s into its solve: 11.8 GB resident, 7.5 GB after the Stop on this build and 7.4 GB on 27.1.0. `heap` and `vmmap` find only 349 MB still allocated: the rest is freed, held by macOS's allocator as 3.4 GB of freed pages in its default zone (91 % fragmentation, footprint 3.8 GB), which its pressure relief does not hand back. glibc's `malloc_trim` does. On macOS it would take the compressed matrices' blocks in a zone of their own, destroyed with the solve |
| The RF 2.5D settings not among `get_settings`' keys | **Fixed (third batch)**: `get_settings` and `set_settings` take scope `rf25` (RF 2.5D > Settings…), searched without a scope; a spin box at a special value ("Automatic") reads as its words in both, and is taken as them |
| `show_layout`'s overlays missing on the first call on a model just opened | **Fixed (third batch)**: it waits for the model's shapes and cells as well as its layout (they are made a moment after the read; a small layout is done before a call can look, a large one was not), holding the model as B6's tools do |

Tests: `test_rf25_returns::whatAPortThatNamesItsReturnsCannotBeIsSaid` (the face terminal, the overlap),
`test_qucs_control::anRf25PortsReturnsAreNamedThroughTheTools` (the idle terminal),
`anRf25ModelIsSolvedFullWaveThroughItsTool` (the dataset), `aSpinBoxsWordsAndSuffixAreReadAsItsValue` (new),
`test_rf25_doc::aSolveShowsHowFarItHasGot` (at a time). Seven breaks, all caught.
The third batch's four: `test_qucs_control::anRf25SolvesBarAndItsSettingsAreReadByTheTools` (new: a solve left going seen in
`get_ui statusbar` and as a progress control; scope `rf25` read, set, searched, "Automatic" read and put back),
`anRf25PortsReturnsAreNamedThroughTheTools` (the first call's overlays, its shapes held back 0.6 s by
`RF25Doc::holdPreparing()`, new for tests), `test_rf25_doc::aSolveShowsHowFarItHasGot` (the release after a Stop);
six breaks, all caught (`breaks_small2.json`; `memstop.py` and `memstop2.py` the memory measurements).

### The accuracy items: modelling limits, with numbers

- **A1, Z0 high.** The two-face quartz CPW at 12 across reads 72.26 Ω (calibrated, 50 GHz) against QFEM's 70.44 and
  Palace's 70.35 (+2.6 %), where εeff is within 0.5 %: C' is right and L' is high. As `47798dbc` found in its 2D
  emulator, the walls carry no current in the two-face model: L comes out 3-6 % over, and current down half the wall
  would mend it. The walls' charge (B2) does not touch L. The thin PEC line's quasi-static Z0 (75.71 / 75.03 / 74.70 Ω
  at 6 / 12 / 24 across) halves its step at each doubling: first-order convergence, extrapolating to about 74.37 Ω,
  +0.9 % over Palace's 73.72. That fits piecewise-constant charge against the edge singularity. Both are modelling
  limits; the first is the open item of `47798dbc` (current on the walls).
- **A2, thin-metal loss** -14 % against QFEM at 5-10 GHz on the plain CPW, -2 % at 60 GHz, within 10 % over 2-100 GHz
  in comparison 1. This is the edge-crowding emulator's accuracy (`crowding()`, 8.6 % from its 2D solves on average,
  `47798dbc`), a modelling limit.
- **A3, loss through pads above 35 GHz:** not reproduced to a cause. B4 makes the line in its pads meshed as the line
  alone, so what is left is the ends' own structures. On lossless metal (σ 1e12) the L-2L α of the pad pair at 30 GHz
  is 3.7 dB/m at 3 across and 0.07 at 6, against the plain line's 0.75 and 0.31: noise of the same size as the
  reviewer's 0.01-0.05 dB errors. The ground-path coupling the reviewer suspected (-45 dB) is a candidate this batch
  did not test.
- **A4, the bandstop filter's shift of 1.4-2.1 %:** not reproducible here; it needs a design that is
  not on this machine. Of it, 0.35 % is the strip's εeff (the reviewer). Steps 3-4 of the Palace
  benchmark would settle it.

### The features: plans, smallest first (not built)

1. **F6, the circuit fitted jointly on S** after the per-branch fit (about a day): a Levenberg-Marquardt pass over every
   element at once, minimizing the rms of S over the sweep, starting from today's fit; kept only where it lowers the rms
   (the reviewer's 0.010 against 0.022); its rms said beside the per-branch one. In `rf25_circuit.cpp`.
2. **F5, `rf25_inductance` past the dense limit** (about a day): its loop solve on the compressed matrices of the full
   wave's L (rf25_fast's H-matrices, GMRES with the near field's LU) instead of a dense LU; the same answer on a case both
   can do.
3. **F1 (1)-(3), mesh convergence** (two to three days): `rf25_converge` - solve at 3, 6, 12 (or given) across, each
   value of interest (S at the ports, εeff, Z0, C), whether the changes shrink (a ratio under 0.7), and a Richardson
   extrapolation only where they do - modelled on Multiphysics' `fem_converge` (its uniform method; the
   mesh-convergence worktree's adaptive check for (4)); a mesh note in every solve with its finest and coarsest cell
   against the narrowest width and gap present (B4 has the pieces' widths); the adaptive sweep option, which does
   nothing, either hidden or made to work (rational fitting of S on a few frequencies, as Palace's adaptive sweep).
4. **F2, a dielectric with a lateral end** (about a week): layers of finite extent need either a volume term for the
   difference (a polarization current on cells of the cut layer) or a 2.5D-plus-boundary formulation. A smaller first
   step: a finite ground plate is already possible as a conductor, which the reviewer used.
5. **F3, GSG pad de-embedding and a multiline (TRL-style) calibration** (about a week): the strips' calibration
   generalized to user-given standards (thru, line(s), reflect) solved by RF 2.5D, NIST multiline TRL on them, and its
   error boxes taken out of the DUT's S; the pads' own box as the error.
6. **F4, drawing shapes and vias in the tool** (one to two weeks): the RF 2.5D tab's layout view made editable -
   rectangles, polygons, vias on a grid, written to the GDS beside the model -, reusing the layout editor's tools
   where they fit; an MCP tool to add shapes as `rf25_` calls.

## Checks of the first batch

- Every rf25 suite and `test_qucs_control`, 18 of 18, in Release (after each fix, and on the final source).
- The same 18 under ASan/UBSan: no sanitizer report; `test_rf25_wave` past its 300 s with the machine at a load of
  about 50 from other sessions, and passing alone in 143 s (152 s in `c3749bea`'s run).
- The full Release suite: 180 of 181 at once, `test_canvas_drawing`'s paint timing (a step 8.1 ms against a whole
  paint of 20.1, under that load) the one, passing alone three times of three.
- The four lints in `scripts/ci/`: green.
- No new compiler warning in the files changed (the changed `.cpp` files rebuilt; the warnings left are older lines).
- 26 deliberate breaks, all caught: `breaks_results.txt`, made by `breaks.py` from the `breaks_*.json` beside this
  report (B6's run by hand: the same change).

## Checks of the second batch

- Every rf25 suite and `test_qucs_control`, 18 of 18, in Release on the final source.
- The same 18 under ASan/UBSan: no sanitizer report; `test_rf25_wave` past its 300 s at a load of 45-58 from other
  sessions, as in the first batch, and passing alone in 176 s.
- The full Release suite: 181 of 181.
- The four lints in `scripts/ci/`: green.
- 16 more deliberate breaks, all caught; with the first batch's 26, 42 in `breaks_results.txt` (`breaks_b2.json`,
  `breaks_b5.json`, `breaks_small.json`).

## Checks of the third batch (the four small items left, on `cd3b3bdf`)

- Every rf25 suite and `test_qucs_control`, 18 of 18, in Release and under ASan/UBSan (no sanitizer report).
- The full Release suite: 185 of 186. `test_claude_code::promptDatesShowInEachPromptsCorner` fails, alone too, three
  times of three, at its check that a view too narrow for a prompt's caption and date draws no date: a test of
  Show Prompt Dates (`0fa0b3b2`, 8 November) that measures today's date, 11/09/2026, against a width made from it.
  Nothing of this batch touches the Claude Code dock; left to whoever owns that feature.
- The four lints: green.
- Six more breaks, all caught; 48 in all in `breaks_results.txt`.
