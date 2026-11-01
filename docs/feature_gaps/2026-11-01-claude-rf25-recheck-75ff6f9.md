# RF 2.5D: the recheck of 75ff6f9, 1 November (evening)

The reviewer's recheck (`~/Desktop/qucs-reviews/RF2p5D/2026-11-01-claude-rf25-phase-c-recheck-phase-d.md`, second
version) was made on the build of 1 November 14:37, to `75ff6f9`. It confirmed that round's fixes:
- the coplanar calibration: \|S11\| −57.2 dB at 60 GHz, \|S21\| −0.074 dB against QFEM's −0.077;
- the strips cut at eight across;
- the copper loss;
- the microstrip loss;
- the no-line nit.

It left two items open, as that round's write-up had. Both were reproduced on `75ff6f9`'s code, through
`qucs-s --mcp-server` on copies of the reviewer's own models (`~/QucsWorkspace/rf25_check/fw/`), and against
QFEM's own runs of the same line (`~/QucsWorkspace/rf25_check/qfem/gcpw_ref*.dat`). (The ports on the layout,
asked for during this round, were done between its investigation and its fixes:
`2026-11-01-rf25-ports-on-layout.md`, `43750fd` and `5fd0097`.)

## Findings

| Feedback | Now |
|---|---|
| **Metal thickness's capacitance**: Cu CPW eps_eff 5.58–5.69 against QFEM's 5.46–5.56 | **Done: a metal can be solved as its two faces.** Reproduced: 5.589, 5.595 and 5.701 at 10, 30 and 60 GHz at four across, 2.3 to 2.6 % over QFEM; 2.6 to 3.2 % at eight. A 2D solve of the cross-section, by moments and again by finite differences (they agree to 0.2 %), says this is the whole of the thickness. 3 µm of metal beside 50 µm gaps lowers eps_eff 3.8 % and Z0 2.9 %, with QFEM's via fences or without, against a sheet: the walls hold field in the air, where the one sheet had it half in the alumina. QFEM's line is that thick line plus its copper's inductance and its dispersion. **A metal's new "Solved as" is a sheet (the default) or its two faces.** As its two faces, the solvers have a sheet at its bottom and one at its top, its thickness over it. The room between them is filled with what is over the metal, the air here. Its walls join the two along every edge of its cells that is its conductor's edge (table below). **On the check's line with both coplanar ports calibrated, eps_eff is now 0.4 % under QFEM's at 10 GHz on four cells across, and the same as QFEM's to 0.02 % on eight; from 1 to 60 GHz it is within −1.1 to −0.1 % on four** (table below). As a sheet it stays as it was. |
| **1–5 GHz loss under**: QFEM 12 against 8 dB/m at 1 GHz | **Done as far as the sheet goes: 10.8 dB/m at 1 GHz (9 % under), 12.8 at 2 (12 %), 19.1 at 5 (11 %), where 75ff6f9 read 8.0 at 1.** Reproduced: 7.96 at 1 GHz, 33 % under. The sheet's edge cells were stopped at the thickness's stopping distance plus Pearl's spreading, which assumes a metal many skin depths thick. 3 µm of copper is 1.4 skin depths at 1 GHz and 3 at 5. There the half slab's own resistance is above a thick metal's two faces, and that excess is spread over the cell as it is through the metal, not crowded. **An edge cell now loses the thick metal's crowding at Rs/2, plus the half slab's resistance beyond it, evenly.** The crowding fades where the cell is narrow beside Pearl's length, the spread a metal thin beside its skin depth makes, and at direct current it is one. A 2D eddy-current solve of the current through the check's cross-section gives QFEM's copper R within 2 % (4.5–6 % at 5–10 GHz). On the check's line, the sheet's R is now within about 3 % of it from 1 to 10 GHz. What is left of the loss gap is mostly Z0: alpha is R/2Z0, and the line's Z0 is 51.6 to 52 Ω at four cells across, where the true thick line's is 47–48 (a coarse mesh's C is low). The two faces (above) take Z0 to 50.6, and at eight across to 49.4–49.8 (table below). The microstrip tests now hold to the same 2D solve: 1 µm of copper at 2 GHz within 10 % of its 241.3 Ω/m (measured +4.4 %), 3 µm at 10 GHz within 20 % of its 219 Ω/m. |

### The check's 1 mm GCPW, both coplanar ports calibrated

The signal 100 µm, the gaps 50 µm, the grounds 250 µm with vias every 150 µm, on 200 µm of 9.8, with 3 µm copper. The
calibrated line's eps_eff, as the strips read it, against QFEM's copper line:

| GHz | QFEM | sheet, 4 across | its faces, 4 across | sheet, 8 across | its faces, 8 across |
|---|---|---|---|---|---|
| 1 | 5.574 | 5.628 (+1.0 %) | 5.514 (−1.1 %) | 5.680 (+1.9 %) | 5.547 (−0.5 %) |
| 2 | 5.527 | 5.620 (+1.7 %) | 5.480 (−0.8 %) | 5.664 (+2.5 %) | 5.513 (−0.3 %) |
| 5 | 5.486 | 5.613 (+2.3 %) | 5.456 (−0.6 %) | 5.654 (+3.1 %) | 5.489 (+0.05 %) |
| 10 | 5.461 | 5.589 (+2.3 %) | 5.432 (−0.5 %) | 5.630 (+3.1 %) | **5.462 (+0.01 %)** |
| 30 | 5.465 | 5.595 (+2.4 %) | 5.439 (−0.5 %) | 5.633 (+3.1 %) | 5.457 (−0.15 %) |
| 60 | 5.557 | 5.701 (+2.6 %) | 5.549 (−0.1 %) | 5.740 (+3.3 %) | 5.562 (+0.1 %) |

Perfect metal as its faces reads 5.350 at 1–5 GHz at four across and 5.374 at eight. The 2D solve's quasi-static box
is 5.369. The copper adds its own inductance: QFEM 2.9 % at 2 GHz, the faces 2.4 % (2.6 at eight).

Its loss, dB/m, and the line's impedance:

| GHz | QFEM | sheet, 4 across | its faces, 4 across | sheet, 8 across | its faces, 8 across |
|---|---|---|---|---|---|
| 1 | 11.97 | 10.84 | 10.31 | 10.82 | 10.76 |
| 2 | 14.52 | 12.77 | 12.23 | 12.65 | 12.69 |
| 5 | 21.44 | 19.12 | 18.61 | 18.85 | 19.24 |
| 10 | 30.88 | 28.08 | 27.37 | 28.32 | 28.79 |
| 30 | 54.14 | 49.66 | 49.00 | 56.38 | 54.87 |
| 60 | 76.86 | 70.51 | 71.16 | 71.01 | 70.49 |
| Z, Ω | (47–48) | 51.6–52.1 | 50.6–51.1 | 50.5–51.3 | 49.4–50.6 |

At eight across, the strips are cut to 1.0 and 1.9 mm at 30 and 60 GHz as its faces, with their ports the nearer.
Its perfect metal's 30 GHz reading is 5.60, where the strips' ports couple through the surface wave. As a sheet,
the strips at eight across now fit whole under the full wave's new limit (below): 1.9 and 3.7 mm, \|S11\| −57.7 dB
at 60 GHz, where the check saw −46.7 on cut strips.

## What reproducing found

- **The gap is the whole of the thickness, and QFEM agrees with it.** Two 2D solves of the check's cross-section
  agree to 0.2 %. One is by moments over the stack's images, every face of each conductor in cells. The other
  is by finite differences on a grid 0.25 µm fine at the metal. The sheet gives eps_eff 5.583 and Z0 48.6 Ω
  (with the via fences); the 3 µm box gives 5.369 and 47.2. QFEM's copper line is 5.454 at 15–20 GHz. That is
  the box with its copper's own inductance (1.0 % there by QFEM's own loss) and a little dispersion. The
  sheet's own eps_eff at four cells across is 1.4 % low in the full wave (0.6 % at eight), where its L and C
  converge from either side. So the 2 to 3 % the reviewer saw is the 3.8 % of the walls less that.
- **Of the ways to give a sheet its thickness, two sheets and the walls joining them is the one that converges.**
  The same 2D solve tried each on the solver's own cells, against the box (the box's change from the sheet
  taken as 100 %):

  | Model | 4 across | 8 | 10 | 20 |
  |---|---|---|---|---|
  | Two sheets | 75 % | 83 % | 84 % | 88 % |
  | The edge cells' copies at the top | 57 % | 58 % | 58 % | 55 % |
  | A wall of one cell at each edge | 102 % | 76 % | 71 % | 60 % |
  | Two sheets and walls of charge | 143 % | 121 % | 117 % | 108 % |

  Two sheets alone come at the box from below and steadily. The walls' own charge is half of what the coarse
  sheets miss at their edges, and over-corrects there. Joined by walls that carry current and no charge, which
  is what a via already is, the two sheets are solved as two metals and a via between them are. So that is how
  they are made.
- **A face of the metal's current is not the sheet's half.** Each face carries what the field at it asks:
  - **Its own surface impedance.** Zs coth(γt), the slab's with the field on its side alone.
  - **What the other face's current adds through the metal.** Zs csch(γt).

  The two together, alike, are the sheet's (Zs/2) coth(γt/2), exactly. Thin beside the skin depth both are
  1/(σt): the faces are one metal, and how they share a current costs nothing (direct current at 10 MHz to 1.4e−3,
  as the sheet's). Many skin depths thick they are apart. Without the second term, two faces 3 µm apart had half
  the direct-current resistance.
- **The faces' current is the thick metal's, so a sheet's edge factor over-counts it.** On the test's
  microstrip at 10 GHz, the faces carry 56 % on the bottom and 44 % on top, where a 2D eddy-current solve of the
  real 3 µm strip has 59 to 41. That is the last round's open item, "a wide strip's current favours its lower
  face", answered where the metal is solved as its two faces. The outer cells carry 0.469 of the current, against
  that solve's 0.474 and the sheet's 0.444: the walls draw it outward, and the sheet misses that. But Holloway and Kuester's stopping
  distance gives an edge cell's loss for a sheet's thinner edge current. On the faces' current it had the
  microstrip lose 28 % more than the 2D solve, where the sheet loses 18.5 % more. That solve's own edge cells
  lose 1.50 times their current's average at four cells across and 1.28 at eight (microstrip), and 1.55 and 1.34
  at the coplanar line's signal edge. The sheet's factor says 1.82 and 1.64. **The faces stop the crowding three
  times as far out** (t/97): 1.50 and 1.33, within −4 to +10 % of the 2D solve from 2 to 60 GHz and from 1 to
  3 µm thick. Only the coplanar line's ground edge at four across is under it, by 12 %, where the ground's current
  decays over more than one coarse cell.
- **The excess loss at an edge goes on the two faces' current together.** It is the walls' and corners' loss,
  within the metal's thickness of its edge, where the two faces' current crowds as one. Its reactance is its
  resistance, a thick metal's: the copper's own inductance at 2 GHz on the microstrip is then 1.55 % of L, where
  the 2D solve has 1.46 %.
- **A thick metal's port drives its whole wall.** Driving the bottom face alone is the same to four digits at
  10 GHz, but at 60 GHz the line read moves 0.2 % in eps_eff and 4 % in loss. A port, and a coplanar port's
  grounds, now drive both faces, half each. An internal port, or a via up from such a metal, stands on its top:
  a wide metal's faces meet only round its edges, as a thick conductor's do once its skin is thin. The via up
  is 2 µm shorter for it, and its inductance is its closed form from there, within the 1.4 % the bottom face's
  current adds by climbing the walls beside it.
- **Two faces double a metal's cells, and the full wave's limit was 3000 runs and cells.** A millimetre of the
  check's line at eight across is 3144 as its faces, and its strips more. The limit is now 6000, about 750 MB
  and 8 s a frequency on a desktop (5588 measured: a 1.9 mm line at eight across as its faces at 60 GHz). As a
  side effect, the sheet's strips at eight across, which were cut to 1.0 and 1.9 mm to fit, now fit whole.
- **The mesh refused a strip that only its top face made too many charges for.** That put the calibration out
  where it should have been cut short. The mesh now refuses only on its own cells. The solvers refuse what
  they cannot hold once the faces are made, and the de-embedding cuts its strips for that.

## How it is made

`thickened()` (`rf25_mesh.h`) takes the stack, the shapes and the mesh, and hands the solvers what they already
know: sheets and vias. For each metal solved as its two faces:
- **Its bottom face** is the metal itself.
- **The room its thickness takes** is a dielectric of that thickness, filled with the first dielectric over the
  metal, or the air. The layers above are then as high as the stack draws them.
- **Its top face** is a metal layer of its own, "M1 (top)", with the same conductivity.
- **Its walls** are a via layer between the two. A wall cell stands along each side of a cell that is its
  conductor's edge, a tenth of the cell deep. It lands on the cell and on the same cell of the top face.
- **The shapes and the cells.** Each shape is copied to the top face, and to the walls' layer, which joins the two
  into one conductor for the extraction. The cells are copied to the top face after the mesh's own, which keep
  their places.
- **A via up from the metal** leaves its top face.

The extraction solves that as it is. The top face is in a net's charge, not in its name, and not in Wheeler's
rectangles, which are the metal's own whole thickness. How the charge crowds is of the metal's cell, both faces
together. The full wave solves it as two metals and their vias. Each run on a face is coupled to its twin on the
other face (`faceImpedances()`), and an edge's excess loss goes on the two together. A port drives both faces,
half each, and says so ("half of it on M1's top face"). An internal port toward a metal above stands on the top
face ("on M1's top face"). What it hands back is of the mesh as given: a top face's run is folded into its bottom
twin's (the current through the whole metal there), the walls are left out, and a via's cells are on its
metal's. The panel's count of unknowns counts the top faces and walls.

## Tested

| Test | What it holds to |
|---|---|
| `aMetalSolvedAsItsTwoFacesIsTwoSheetsAndItsWalls` (`test_rf25_mesh`) | As sheets nothing is made. As its two faces: the room 0.6 µm of the dielectric over it (3.5, tan δ 0.01) and the air over a top metal; the top face, the walls from bottom to top, M2 0.6 µm higher; a via up re-pointed to the top face, one from the ground not; two strips touching end to end with 16 walls, none where they touch, each inside an edge cell; cells in place, copies with their outlines; made once; no thickness, no faces; the panel's count 28 more; a mesh whose top faces alone make 10000 charges still a mesh, said |
| `aMetalSolvedAsItsTwoFacesHoldsItsWallsField` (`test_rf25_static`) | The check's line without its fences, four across: the sheet's eps_eff 5.595 against the 2D 5.576; as its faces eps_eff 2.71 % and Z0 1.98 % lower, between 80 % of the 2D two sheets' 2.94 and 2.15 % and the whole box's 3.94 and 2.99 %; the same nets and names, twice the unknowns, said; Wheeler's R the same to 1e−9, the direct-current R the same; the charge's crowding, middle and whole, within 10 % of the sheet's (measured +1 %) |
| `aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent` (`test_rf25_fullwave`) | 3 µm copper microstrip, 100 on 100 µm of 9.8, four across: said; handed back as the mesh, runs as the sheet's, currents on its own cells; ports half on the top face; 10 MHz R′ 1/(σWt) to 5e−3; the current through the middle the sheet's to 1e−3; perfect metal at 100 MHz, C′ the extraction's to 1e−3 and L′ to 1.5 %; eps_eff and Z0, full wave and extraction, lower by 80 % of the 2D two sheets' to the whole box's (measured 1.49 and 1.10 %, the 2D 1.48 and 1.13); 10 GHz alpha within 20 % of the 2D eddy solve's (+15.5 %, the sheet +18.5 %); 2 GHz copper's own inductance within 25 % of the 2D solve's 1.46 % (1.55) |
| `aCoplanarPortReturnsThroughItsGrounds` | As its faces: the port and its grounds half on their top faces, said; reciprocal, passive; the current back alike along each ground; the feed's fences found as the layout has them, its walls none of them |
| `aViaBetweenTwoMetalsJoinsThem` | M1 2 µm as its faces: the via up from its top face, its closed form from there plus less than 2 % (measured 1.4 %); the internal port "on M1's top face"; the via's cells its run, on the mesh as given |
| `whatItCannotSolveIsSaid` | A plate of more than 6000 runs and cells refused, "(6000 in all)" |
| `aSubstratesAndAMetalsLossesAreTheirs`, `aMetalsLossIsItsThickEdgesWhereTheCurrentCrowds` | The 1–5 GHz item: 1 µm at 2 GHz within 10 % of the 2D solve's 241.3 Ω/m; 3 µm at 10 GHz within 15 % of Wheeler and 20 % of 219 Ω/m |

## Broken on purpose

`docs/feature_gaps/2026-11-01-claude-rf25-recheck-75ff6f9/breaks.py` holds twenty-seven:

- **The edge loss:** the crowding not faded over Pearl's length; the slab's resistance beyond Rs/2 crowded too.
- **The stack and the mesh:** "Solved as" not read; the room filled with what is under the metal; the room the
  air whatever is over it; the top face at the bottom's height; no walls; the faces not one conductor; a via up
  left at the bottom face; the panel's count a sheet's; the mesh refusing what only its top faces make.
- **The extraction:** solved as a sheet; a top face's charge apart from its cell's, in the whole and in the
  middle; the top face's rectangles in Wheeler's too; a top face in its net's name.
- **The full wave:** solved as a sheet; the faces uncoupled; each face the sheet's impedance; the edge stopped
  where a sheet's is; the edge's excess reactance the slab's; a port on its bottom face alone; an internal port on
  the bottom face; the feed's vias looked for among the walls; the top faces' current not handed back; the walls'
  runs handed back; the limit 3000 again.

**Twenty-six of the twenty-seven are caught**:
- **The Pearl fade.** Caught by the 2 GHz loss test, though not by the 10 GHz edge test, where Pearl's length is
  short.
- **The via left at the bottom face.** Caught by the mesh test, though not by the via's inductance. Its own
  inductance and that of the walls' current beside it nearly cancel: 0.12 % between the two.

Running the breaks found a fault: **the via's direction was read from the layers' heights**, which a stack made by
hand, as the tests make theirs, does not have. So the test's via had stayed on the bottom face. It is now read
from the layers' order, which is their order in z.

**The one not told apart is the edge's excess reactance**, taken as a thick metal's (its resistance) or as the
slab's. On the test's microstrip, the copper's own inductance at 2 GHz is 1.55 % of L as a thick metal's and 1.38 %
as the slab's; the 2D eddy-current solve's 1.46 % lies between them. On the check's line, where QFEM implies 2.9 %
at 2 GHz, the two give 2.4 and 2.1 %. That is why it is the thick metal's: where that excess lies, within the
metal's thickness of its edge, the metal is many skin depths across. No unit test separates them more sharply
without solving the check's own line and its strips.

The full ctest (149) and the scenario script (97 checks) were green, both lints too. Under the sanitizers (a
Debug build, as CI's Linux job is), the new and changed functions ran with no report:

| Function | Time |
|---|---|
| The two faces' full wave | 77 s |
| The via | 82 s |
| The coplanar port | 46 s |
| The large plate refused | 33 s |
| The edge loss | 25 s |
| The static two faces | 13 s |
| The 2 GHz loss | 8 s |
| The mesh | 4 s |

The whole full-wave suite took 357 s sanitized. CI's runner took 266 s on it at `ac6ce8f`, about twice this
machine's time then. At that rate the suite is some 710 s there now, three quarters of its 300 s limit times
three. Its limit is now 900 s, as `test_rf25_static`'s and `test_rf25_sparams`' are. The two faces' function was 124 s sanitized. A line of two faces
is twice the unknowns, its walls' kernels and three pairs of heights, at each frequency of each line. So a
sanitized build solves it on lines of 1000 and 1500 µm, whose ends cancel in the low frequencies' differences as
well (eps_eff −1.50 % and Z0 −1.11 %, against −1.49 and −1.10). It leaves out the 10 and 2 GHz checks, which want
the two-line method's twenty substrate heights and run in the Release jobs.

## Not done

- **"Its two faces" is not the default.** It doubles a metal's cells, so its unknowns, which costs four times the
  memory and some eight times the time. A sheet is right where the metal is thin beside its gaps, as most of the
  stacks solved so far are. The panel counts what the faces cost.
- **The walls carry the current between the faces and none along them.** Their share of the line's current is in
  the edge cells' crowding, stopped three times as far out as a sheet's. That factor came from a 2D solve, and
  on the coplanar line's ground edge at four across it is 12 % under, where the ground's current decays over more
  than one coarse cell.
- **The loss at coarse meshes.** Alpha is R/2Z0, and Z0 is 7 % over at four cells across (5 % at eight) because a
  coarse mesh's C is low. That, more than R, is what is left of the loss gap.
- **Strips at fine meshes** are still cut to fit the full wave as their faces, and short strips' ports couple
  through the substrate's surface wave from 30 GHz. Perfect metal at eight across reads 5.60 at 30 GHz.
- **Phase B's Wheeler R for a metal thinner than two skin depths** and **a continuous via wall at fine meshes**,
  from earlier rounds.
