# RF 2.5D's two-face metal: its walls' current, charge and loss

Phase 2 of the plan for RF 2.5D's accuracy limits. It covers A1 (the walls' current), and also A2 (the metal's loss),
which the plan had as phase 4. The two could not be separated: walls that carry current change where the loss is,
and neither the old loss nor an even one was right with them (see "Why A2 came with A1"). The references are those of
phase 1's `test_rf25_accuracy` (`2026-11-12-rf25-accuracy-references.md`).

## What changed

A metal solved as its two faces had its current and charge on the faces alone. Its walls only joined them, as vias.
Now:

- **The walls carry current** (`wallsOf()`, `rf25_fullwave.cpp`).
  - A run along an edge is its face's strip plus its half of the wall on each free side: a strip standing upright on
    the edge's line, from the face to the metal's middle. The current is shared between them by length, as
    `walls.py` beside the 4 November write-up showed in 2D.
  - The runs' static partial inductances are summed over their parts (`staticPair()`) in one place. Both the dense
    fill and the compressed solve's kernel use it, so the two stay one matrix.
  - `Filament::rise` gives a run an upright breadth. Near pairs with an upright part take the double integral
    averaged over both cross-sections, its logarithm apart: the smooth part at four Gauss points across each, and
    K ln ρ's mean between the two segments in closed form. Two strips that meet at a corner, and runs as short as
    they are wide, need both: the plain averaged distance is wrong at the corner, and a geometric mean distance alone
    was 0.3 % off at ten times as long.
- **The walls hold charge where they face another conductor's across a narrow gap** (`wallCharges()`,
  `rf25_edge.cpp`).
  - These are B2's facing walls, no more than sixteen thicknesses apart (`wallGapsOf()`'s pairs). Each such edge
    cell is its face's rectangle plus its half of that wall, the charge shared by area.
  - The static potentials sum over the parts in both solvers: `staticPotential()` in the full wave and
    `wallCorrection()` in the quasi-static solve. There the stack's images stay at the face, as the full wave's
    remainder stays at the centres, so the quasi-static solve is still the full wave's low frequency.
  - **This replaces B2's lumped capacitance.** The 2D excess between facing walls would now be counted twice, so
    both solvers stop adding it (`wallGapsOf()` still finds the pairs).
  - **An edge with nothing near holds no wall charge.** The faces' edge cells already hold its corner's charge near
    enough. A microstrip of 3 µm copper read Z0 1.53 % under the sheet's with them alone, where Hammerstad and
    Jensen's thickness correction says 1.49 %. With charge on its walls as well it read 2.26 %, its capacitance a
    point too high.
- **An edge run's loss is its band's of the edge solved in its cross-section** (`rf25_edge.{h,cpp}`, new).
  - **The solve:** once per metal and frequency, kept, eddy.py's method in C++.
    - The metal's edge is cut into rectangles of uniform current, graded to a fifth of a skin depth at the face and
      the wall.
    - The mean of ln r between them is in closed form.
    - The strip is mirrored about its mid-thickness and 20 thicknesses in.
    - It takes milliseconds.
  - **The run's loss:** the run's band, its face's strip from the edge plus its half of the wall, loses what its
    rectangles do. The run's own resistance is made that loss, less its twin's share through the metal.
  - Each part's reactance is its own surface's: the face's slab and the wall's.
  - `crowding()` is left for sheets, and for a strip one cell wide with both its walls.

## Why A2 came with A1

With the walls' current alone, R jumped on the thick lines: alumina +16 to +27 %, narrow +33 %. More current ran
along the edges, and `crowding()` already stood for the walls' loss. Giving each part its own even loss put L right
on every line (within 1.5 %), but R a fifth to a third under: an even current misses the crowding at the corner. The
edge solve gives that crowding, and R came within a few per cent.

## The numbers (`test_rf25_accuracy`, its references as in phase 1)

| | before (phase 1) | now |
|---|---|---|
| A perfect metal's L, against the boundary-element solve | +5.5 % | +0.73 % |
| The benchmark's CPW (2 µm gold), Z0 at 10 / 30 / 50 GHz, against Palace 2D and QFEM | +2.73 / +2.71 / +2.76 % | +0.37 / +0.37 / +0.48 % |
| its eps_eff | +0.66 / +0.62 / +0.46 % | +0.29 / +0.25 / +0.22 % |
| The five lines' L, the worst of each | +6.3, +3.7, +5.4, +9.2, +2.8 % | +1.0, +1.5, −0.6, −0.8, +1.7 % |
| their R, the worst of each | +14.8, −9.3, −14.9, −12.6 % (and the microstrip's) | +4.4, +4.3, −3.7, −6.5 % |
| their attenuation against the 2D solves, on the whole | crowding(): 8.6 % on average, 19.9 % at worst (the 4 November emulator) | 3.4 % on average, 6.8 % at worst |

The five lines are alumina, copper, film, narrow and the microstrip, in that order.

**Elsewhere:**
- **The comb of B2** (1 µm metal in 1 µm gaps) reads 31.94 fF against Palace's 31.8 (+0.4 %; the lumped capacitance
  read 31.40). Its rise from 1 to 100 GHz is 1.1371 against 1.135, and its quasi-static solve is within 0.3 % of its
  full wave.
- **The microstrip's 30 GHz** is the one figure left out of the whole: R +35 %, as it was +32 % before. That is loss
  beyond its metal (radiation or a substrate wave), which the plan's phase 6 takes up beside the pads' loss.

## Tests

- **`test_rf25_accuracy`:** each line's bounds tightened (R within 5 to 8 %, L within 1.5 to 2 %, α within 5 to
  8 %). New `theLinesLossOnTheWhole` holds the attenuation within 5 % on average and 10 % at worst. The perfect
  metal's L is held within 1 %, the CPW's Z0 within 1 % and its eps_eff within 0.6 %.
- **New `test_rf25_edge`:**
  - at a low frequency the current is even, a band losing its direct-current resistance and carrying its share;
  - at a high one the edge crowds, losing over 5 % more than an even spread, while a band 15 thicknesses in loses
    as an even sheet within 5 %;
  - the solutions are kept.
- **`test_rf25_inductance`** `anUprightStripBesideAFlatOneIsTheirDoubleIntegral` (new): a wall at its face's
  corner, two walls end to end, and a wall across a gap from a face, at 200 and 30 µm long. Each is within 5e-4 of
  the double integral averaged over both cross-sections; the pair is symmetric; an upright strip's own is a flat
  one's.
- **`test_rf25_fullwave`:**
  - `aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent` now holds the thickness's effect to Hammerstad
    and Jensen within half a point: eps_eff 1.92 % and Z0 1.49 % lower, the full wave 2.34 and 1.53, the extraction
    1.48 and 1.13. Its 2D reference figures, 1.84 and 1.47, are the same thing.
  - The comb is held within 1 % of Palace (it was 3 %), and the walls' note is the new one.
- All 26 RF 2.5D suites and `test_qucs_control` pass under ctest at `-j4`. `test_rf25_accuracy` takes 293 s, where
  it took 276 before. The four lints pass.
- `breaks.py` beside this file undoes each piece. Its results are in `results.txt`.

## Not done

- The loop inductance (`rf25_inductance`'s solve) has no two-face model, so its loops have no walls.
- The edge solve's band is the edge's own. A narrow gap's neighbour crowds the current further, which the method of
  moments resolves only between cells: the narrow line's −6.5 % at 30 GHz is that.
- A strip one cell wide, with both walls, keeps `crowding()`.
