# RF 2.5D: the phase B check of 24 October

The reviewer's live check of phase B (`~/Desktop/qucs-reviews/RF2p5D/2026-10-24-claude-rf25-phase-b-check.md`)
was made on the installed build of 24 Oct 18:44, which had everything up to `4a02649`. Its F5 and F7 were
done by the phase A round (`b62b55d`), which came after. Everything else was reproduced again on
`b62b55d` before anything was changed, through `qucs-s --mcp-server`, isolated as
`scripts/mcp-e2e-scenarios.py` isolates its own, on copies of the reviewer's own models
(`~/QucsWorkspace/rf25_check/`): every number the check gives came out the same.

The references are the reviewer's: Hammerstad and Jensen of 1980, and a 2D finite-difference solve of
the zero-thickness cross-section that needs no closed form. Microstrip throughout is a perfect ground,
100 µm of eps_r 9.8 and M1 of 3 µm copper.

## The reference itself

| Feedback | Now |
|---|---|
| The quasi-static write-up and its test hold W/h = 1 to "Hammerstad and Jensen: 6.620, 49.21", which is not H&J (eps_eff 0.7 % high); and call H&J "good to about a per cent", where they claim 0.01 % for Z0 in air and 0.2 % for eps_eff | **Corrected.** The test's `hammerstad()` was Hammerstad's form of 1975. It is now `hammerstadJensen()`, the 1980 form, which gives the check's eps_eff to the last digit (6.130, 6.579, 7.355) and its Z0 with the check's eta0 of 120 pi (79.64, 49.32, 25.75); with eta0 = mu0 c0, as the test takes it, Z0 is 0.07 % lower - 79.59, 49.29, 25.73 - and 0.02 % from the check's 2D solve at W/h 0.3 and 1. The test holds **three** widths to it, W/h 0.3, 1 and 3, with the solver's own per-metre values rather than the test's two-length difference: eps_eff within 0.5 % and Z0 within 1.5 %, measured at 0.06, 0.12, 0.06 % and 0.26, 0.37, 1.0 %. The old write-up's table and its claim are struck through where they stand, with a dated note giving the right values and pointing here. |

## Findings

| Feedback | Now |
|---|---|
| **F1** The line's per-metre values include the ends: the whole capacitance over the length, so a 50 Ohm line came out Z0 45.85 (-7.0 %), W/h 0.3 76.64 (-3.7 %), a coupled pair's even mode -8.6 %. "Solve L and 2L and difference, or recognise a uniform line and take the middle cells' charge. Either way, say in the answer which was done." | **Fixed, by neither, after trying both.** A uniform line's per-metre values are now of its **middle cross-section as if it ran on for ever**: each cell across the middle a strip the line's whole length, tiled by copies of itself end to end (summed out to a hundred times the stack's height or the cross-section's span, beyond which over a ground what is left is a part in ten thousand), and the charge per metre on those strips solved for. Its ends' fringing is not in it at all, and **the answer no longer depends on the line's length**: 500, 1000 and 2000 µm of the 50 Ohm line give the same Z0 to five digits. Both suggested ways were built and measured first. *The middle cells' charge* left C0 of the 1000 µm line 1.4 % high: in vacuum what an end does falls off as a power of the distance, not at once, and reaches the middle. *The difference of the line and its own first half* (cut from its own cells, the cut end graded as a real end is) was exact at 2000 µm - it is the reviewer's own method - but 0.46 % off at 1000 and 2.2 % at 500, for the same reason. The cross-section costs a few hundred thousand table lookups, nothing beside the solve. What a layout that is no uniform line gets - a bend, a taper, a step, a pad - is still the whole over its length, and said to be. The answer says which: "Per metre of the line's middle cross-section, as if it ran on for ever: its two open ends' fringing is not in it", in `what it did`, in `brief`'s `per metre`, and in the tab's notes. The S parameters and the Touchstone are built on these, so the ends' capacitance is no longer spread along the line. The reviewer's cases, now: see the table below. |
| **F2** The default mesh is far too coarse: 20 cells a wavelength and no largest cell put **3 cells** on a 1000 x 100 µm strip, one across, Z0 55.06 (+12 %); edge refinement does nothing when a cell is wider than the strip; "the cell size has to follow the cross-section too" | **Fixed.** The Mesh node has **Cells across a conductor** (10 by default) and **Stretch along a line** (4): each conductor is cut against a grid of its own, at least ten cells across its narrowest part - a rectangle's short side, else twice its area over its perimeter, which is a trace's width for a trace of any shape - and a straight strip's cells are up to four times as long along it, where the charge hardly changes. The outermost cells of every side are half as wide, as lines of the grid rather than cells divided again, so every two neighbours still share a whole edge (the inductance's runs follow shared edges). The default 50 Ohm line is now 324 cells and Z0 49.47, eps_eff 6.571 (+0.4 % and 0.06 % against the 2D 49.28 and 6.567), in 0.4 s. |
| **F3** The 4000-cell cap blocks convergence: 5 µm on a 1000 µm 50 Ohm line refused, W/h 0.3 below 5 µm refused, coupled lines at 10 µm hit it at 1000 µm. "A cross-section-aware mesh (fine across, coarse along) would help; the last is the cheapest" | **Fixed, three ways.** The cheapest first: the mesh is fine across and coarse along (F2), and since a uniform line's per-metre values are of its cross-section, they no longer need a long line or a fine one along it at all. Then the cap: **8000 cells**, measured at 1.4 s for 1648 cells and 6.4 s for 3768 (both solves), so 8000 is about a minute. The reviewer's refused cases solve: W/h 0.3 at 2.5 µm (5628 cells, 15 s, Z0 79.94), the 50 Ohm line at 5 µm (4444 cells, 9 s, Z0 49.49), the coupled pair at 10 µm. And a mesh too large for the solver is no longer refused outright: the cells across are taken down until it fits, and said ("10 cells across a conductor would be more than the 8000 this build solves: 3 across instead") - the heater example at ten across its narrow trace was 72 024 cells, and is now 5484. Where the largest cell sets the size and not the cells across, fewer across cannot help, and it says that instead. |
| **F4** R does not converge with the mesh: W 30 µm, 3 µm copper, 10 GHz, 1166 Ohm/m at 5 µm cells and 1288 (+10.5 %) at 2.5 µm. The crowding is of a zero-thickness strip's charge, whose square diverges at the edges; cut it off at the thickness, check against Wheeler's incremental inductance or the Pucel/H&J forms, with the ground's share | **Fixed, in the second commit of the round, and not by a cut-off.** R per metre is now **Wheeler's incremental-inductance rule, R = (Rs/mu0) dL/dn, on the line's own thick cross-section**: the middle cells across, each conductor a rectangle as thick as its metal - its bottom, its top and both walls in cells, the outermost halved at the corners until no wider than half the thickness - in a 2D method of moments in closed form over the ground's image. L is the vacuum's whatever the dielectrics, so that is all the rule asks of a geometry. Every face of a conductor recedes into it for its own share, by a central difference; the ground recedes below everything for its. Before any C++, the approach was checked in Python against an exact solve of the whole thick perimeter: Wheeler on H&J's thick air line is within 0.4 to 1.2 % of it, and two sheets without their walls - the cheaper model - 4 to 7 % low, so the walls are in. The answer converges: the reviewer's W 30 µm line is **615.0 Ohm/m at 5 µm cells and 615.1 at 2.5**, where it was 1166 and 1288 - the old figure put the whole current on one face and counted the edges' crowding without end. See the table below. **The ground's share**: a ground can now be "A metal plane" with a conductivity; copper adds 40.7, 38.7 and 30.4 Ohm/m to the three lines, against Wheeler on H&J's 40.5, 38.7 and 30.4. A perfect plane loses nothing, as its name says. At direct current R is no less than 1/(sigma w t), the current then through the whole metal; a metal's slab surface resistance is used, so a metal thin beside its skin depth loses as one. A coupled pair's R is a full matrix now, one conductor's faces receding moving the other's L too - the proximity the crowding of each alone could not give. A metal of no thickness, or a line with no ground, keeps the sheet's crowding, said. |
| **F5** The ground: the model check, `add()`, the help and the solver at odds over where it goes | **Done** by the phase A round (`b62b55d`), which came after this check: a ground is a plane under everything wherever the tree has it. |
| **F6** No ground: `ms100_none` solves to a strip's capacitance to infinity and then reports a "line" with Z0, eps_eff and S parameters | **Fixed.** One conductor and no ground is no line: its capacitance is still given (to infinity, which is an answer), and the line is refused in its own words - "its current has nothing to return through ... put a ground plane under the stack, or draw the conductor it returns through" - in `rf25_solve`'s `line not given`, and nothing of it in the tab's notes. Two conductors with no ground are a line between them, and are given as one, with what a capacitance per metre to infinity is not ("these are of 20 mm of it; between its conductors they hold"). And **a new model now has a ground**: "New Model" was a substrate, an oxide and a metal, so its first solve was exactly this case. |
| **F7** A new model's stack view says "No layer" | **Done** by the phase A round. |

### F1 and F2 in the reviewer's own cases

| Case | Before (`b62b55d`) | Now | 2D FD |
|---|---|---|---|
| W/h 1, 1000 µm, 10 µm cells | Z0 45.85, C 183.2 pF/m (1648 cells) | **Z0 49.69, C 172.0 pF/m** (+0.8 %, -0.8 %), eps_eff 6.566 (1224 cells) | 49.28, 173.4, 6.567 |
| the same, 2000 µm | Z0 47.67 | **Z0 49.69** | |
| W/h 1, 500 µm, 5 µm cells | Z0 42.45 | **Z0 49.49** | |
| W/h 1, 1000 µm, 5 µm cells | refused (5308 cells) | **Z0 49.49** (+0.4 %), eps_eff 6.570 (4444 cells) | |
| W/h 0.3, 1000 µm, 5 µm cells | Z0 75.85 | **Z0 79.99** (+0.5 %), eps_eff 6.124 | 79.57, 6.121 |
| W/h 0.3, 1000 µm, 2.5 µm cells | refused (7260 cells) | **Z0 79.94** (+0.5 %) | |
| W/h 3, 1000 µm, 10 µm cells | Z0 23.20 | **Z0 25.80** (+0.4 %), eps_eff 7.358 | 25.70, 7.348 |
| Coupled, W/h 1, s/h 0.5, 1000 µm, 10 µm | Ze 54.37 / Zo 36.02 | **Ze 59.94 / Zo 37.90** (+0.7 % / +1.3 %) | 59.5 / 37.4 |
| W/h 1, the default mesh | Z0 55.06 (3 cells) | **Z0 49.47** (+0.4 %), eps_eff 6.571 (324 cells, 0.4 s) | 49.28, 6.567 |
| No ground | "Z0 141.9" | **no line**, said | |

### F4: R in the reviewer's cases

3 µm copper, 10 GHz, a perfect ground (the strip's share alone):

| Line | Before (`dabc4ea`, the sheet's crowding) | Now | Exact (the whole perimeter, 2D) | Wheeler on H&J |
|---|---|---|---|---|
| W 30 µm, 5 µm cells | 1191 (the check: 1166) | **615.0** | 616.2 | 620.7 |
| W 30 µm, 2.5 µm cells | 1221 (the check: 1288) | **615.1** | | |
| W 100 µm, 10 µm cells | 347.3 | **226.0** | 226.4 | 227.4 |
| W 100 µm, 5 µm cells | 378.6 | **226.0** | | |
| W 300 µm, 10 µm cells | 114.7 | **80.4** | 80.5 | 81.5 |
| a copper ground's share, W 30 / 100 / 300 | none | **40.7 / 38.7 / 30.4** | | 40.5 / 38.7 / 30.4 |

The error left is the mesh's and first order in the cell across: 0.8 % at ten square cells across the
50 Ohm line, 0.4 % at twenty. eps_eff is within 0.2 % everywhere.

## Smaller issues

| Feedback | Now |
|---|---|
| **N1** The Touchstone's comment says "into 45.85 Ohm ... S11 -49.7 dB", while the data are into 50 Ohm | **Fixed.** The export's comment gives the S parameters into the data's own reference, saying so, with the line's own impedance beside it: "M1 as a two-port into 50 Ohm at 10 GHz: S11 ... dB, S21 ... dB at ...°; its own Zc is 49.25 Ohm". The tab, which is looking for the match, still gives them into the line's own, and says "into its own". |
| **N2** One mesh, two unknown counts under one word: "1648 cell(s) on 1 layer(s); 2758 unknowns" and "1648 cell(s) on 1 net(s) over M1, as 1648 unknowns"; the dense matrix's 116 MB is that of 2758 | **Fixed.** The charge's count and matrix are said as the charge's - "1224 cell(s) on 1 layer(s): 1224 to solve the charge on, a dense matrix of 11.4 MB" - and the full-wave edges as theirs, by name: "The 2334 edge(s) between them are a full-wave solve's unknowns: ... 83.1 MB". The solve's own line says "a charge on each, a dense matrix of 11.4 MB to solve", and the status line gives the charge's. |
| **N3** For a coupled pair the quasi-static limit says "the widest conductor is 250 µm across" for two 100 µm strips | **Fixed.** "The conductors span 250 µm across" for several, "the conductor is 100 µm across" for one. |
| **N4** The tab's notes for a pair give each net as a lone line - its Maxwell self-capacitance with the other grounded, neither mode | **Fixed, in the tab and in the tool.** A set of conductors is given as coupled lines: the tab's notes say "M1, M1 #2 as coupled lines over 500 µm: even Z 59.94 Ohm, eps_eff 7.136; odd Z 37.90 Ohm, eps_eff 5.713", and each mode's loss; its S parameters are the 2N-port's - through, near-end and far-end coupling into 50 Ohm - rather than each net's two-port. `rf25_solve` gives each net its capacitance and, for its line, "one of 2 coupled conductors, which have modes rather than a line each: see coupled lines", and the modes there carry eps_eff now. (`rf25_solve` gave the coupled block only with a frequency; it gives it always.) |
| **N5** `rf25_solve` answers are long (2.5 kB a net), and `max_chars` cuts the line first | **Added**: `brief` keeps each net's capacitance, its line's C, C0, eps_eff, Z0, L, R, G and alpha, a set's modes, the quasi-static limit's frequency and how the per-metre values were taken - 523 bytes for the 50 Ohm line, 824 for the pair. |

## Found on the way

- **A loop's "resistance at direct current" was not.** `rf25_inductance` minimised the magnetic energy for
  the current and gave that current's loss as R at direct current - and its description called that
  current the one at direct current. It is the other way round: at direct current the resistance divides
  the current; the least-energy current is where a current goes once its inductance outweighs its
  resistance. On the old meshes, one cell across a strip, the two were the same current. Ten across, a
  bar's R came out **29 % high** - the crowding counted as resistance. R at direct current now comes from
  the network's own nodal solve, exact on any mesh; `L_dc`, the inductance of that current, is given
  beside `L`, which is the least-energy current's and no more than it; the tool's description and note,
  the tab's line and the inductance write-up are corrected.
- **A metal's roughness was read and then never used** (second commit). The Metal node says "it raises the
  loss at high frequency", and the stack check refused a negative one, but nothing multiplied anything by
  it. It is Hammerstad and Bekkadal's factor now, 1 + (2/pi) atan(1.4 (rms/delta)^2), on the metal's
  surface resistance - 1.81 for 1 µm on copper at 10 GHz - in both R's, not on the ground's share or the
  resistance at direct current; `rf25_solve` gives the factor.
- **A Solve straight after an edit came to nothing, and said nothing** (second commit). An edit has its
  shapes and mesh made again 300 ms later, in the background; a Solve inside those 300 ms started on the
  mesh before, and when the new one was done it was thrown away, its reason cleared with it. Under CI's
  sanitizers that was every time in the Touchstone test, which solves right after adding its export node:
  CI's Linux job was red with it at `b62b55d`, and it reproduced here under the sanitizers. A Solve now
  waits for an edit's mesh and starts when it is made ("Solving once the shapes and the mesh of the last
  edit are made…"), and a solve the new mesh overtakes starts again on it rather than being dropped.
- **CI's Linux job at `b62b55d` also ran two suites past their 900 s**: test_rf25_sparams and
  test_mcp_wishlist, some 150 and 220 s here under the same sanitized Debug - the runner's two shared
  processors. The older tests of test_rf25_sparams and test_rf25_static mesh as they were written again
  (ten cells across each strip is not what they hold), which halves them, and those two and the wishlist
  have 900 s, three times the default, as the heavy multiphysics suites have.
- **`rf25_solve`'s note still said "No inductance of a loop, no ports"**, a round after both landed. It
  points at `rf25_inductance` now.
- **An L passed for a uniform line.** The first test of what is no line found it: its other arm is at
  none of the three places along the line its cross-section is compared at. A net is now held to be no
  broader anywhere than at its middle, and a step - as broad overall as its middle, of rectangles - is
  caught by the comparison along it, which a test now holds too.
- **A coupled pair's loss matrix per metre** is of the cross-section as C is; its coupling term comes out a
  little larger than the whole over the length, since at the ends the field goes to the ground rather than
  to the other line.
- **The capacitance matrix is symmetric to 1e-5, not 1e-6**, with the half-wide edge cells: each cell's
  potential is matched at its centre, which is not symmetric between cells of two sizes. The test says why.

## Tested

| Test | What it holds to |
|---|---|
| `aMicrostripsImpedanceIsThatOfTheClosedForm` | Three widths against H&J 1980: eps_eff within 0.5 %, Z0 within 1.5 %; the per-metre values the same from 400 and 800 µm of line, where the whole over the length is not and grows as the line shortens; Z0 falling as the strip widens; and twenty cells across nearer the closed form than ten |
| `aCoupledPairsModesAreOfItsCrossSection` | The pair's even and odd impedances within 2 % and 3 % of the reviewer's 2D values (0.36 % and 0.79 % measured), and the same from 500 and 1000 µm |
| `aLoneConductorWithNoGroundIsNoLine` | Its capacitance given and its line refused, saying why; no values per metre said; two with no ground said as such; the same strip over a ground a line |
| `whatIsNoUniformLineIsSaidToBe` | An L, a taper and a step: none taken as a line, each said to be the whole over its length, and its C exactly that |
| `aConductorHasCellsAcrossIt` | Ten across a 100 µm strip at a 750 µm wavelength cell, the outermost half as wide, four times as long along: 12 x 27 cells exactly; no stretch, 12 x 102; the largest cell still winning; a pad no strip; an L by twice its area over its perimeter; none across, the wavelength alone |
| `aMeshTooLargeForTheSolverHasFewerAcross` | Forty strips: fewer across, within the cap and said; one strip untouched; where the largest cell sets the size, too many said with what would help |
| `theCellsAtAnEdgeAreHalved` | Lines at 0, 0.5, 1 ... 5.5, 6: 64 cells, a corner's a quarter, and every edge whole - 2 x 8 x 7 shared, no T-junction |
| `theCellsAcrossAndTheStretchAreRead` | Both read from the node, a negative and a stretch below one refused, and a new model's defaults |
| `aBlankModelIsWrittenAndRead` | A new model's ground first, its stack whole |
| `theLossMatrixCouplesAsWellAsLoads` | G per metre of the cross-section, w times its loss matrix, near but not the whole over the length |
| `theQuasiStaticLimitIsSaidRatherThanAssumed` | "The conductor is 200" for one; "the conductors span 250" for a pair of 100 µm strips |
| `aPairIsShownByItsModes` | The tab's notes: the pair as coupled lines with each mode's Z and eps_eff, no net as a line of its own; its S parameters a 4-port's into 50 Ohm, near and far end |
| `theSolvedModelIsWrittenAsATouchstone` | The file's comment into 50 Ohm, with its own Zc; the tab's into its own |
| `theShapesAndTheMeshAreMadeOfTheLayoutRead` | The status line the charge's count and cost, and not the full-wave edges' |
| `aBarsResistanceAtDirectCurrentIsItsOwnOnAnyMesh` | A bar ten cells across: R at direct current exactly rho l/(w t), L_dc the textbook's within 2 %, L no more than L_dc, Q of L over that R |
| `anRf25ModelIsSolvedThroughItsTool` | The per-metre values said to be of the cross-section and below the whole over the length; `length` moving the two-port and not C per metre; `brief` the numbers alone and under half the size; a pair's nets pointing at the modes; `L_dc` no less than `L` |
| `aLinesResistanceIsWheelersOnItsThickness` | Three widths' R within 3 % of Wheeler on H&J (-0.9, -0.6, -1.2 % measured; within 0.2 % of the exact perimeter); twenty across within 1 % of ten (0.1 % measured); a copper ground's share within 3 % of the rule's (0.6 %, 0.0 %, 0.0 %) and added to the strip's; at 1 MHz exactly 1/(sigma w t); a metal of no thickness and a pair with no ground on the sheet's crowding, said |
| `aLinesResistanceAndConductanceAreThoseOfItsLosses` | R Wheeler's within 3 %, the perfect ground's share nothing, the resistance at direct current 1/(sigma w t) |
| `aMetalsRoughnessRaisesItsLoss` | The factor's own values (1 at none, 1 + (2/pi) atan(1.4) at one skin depth, 2 at the most); a line's strip share multiplied by it, its ground share and its resistance at direct current not |
| `theGroundIsReadForWhatItIsNotWhereItIs` | A metal plane a plane for the charge, read with its conductivity |
| `twoLinesAreCoupledLinesWithTheirModes` | A pair's R symmetric, with a term between the two smaller than either's own |
| `aSolveStraightAfterAnEditIsOfTheEdit` | A Solve inside an edit's 300 ms waiting for its mesh, said so, and solving it - the unknowns the new mesh's cells; a solve overtaken by an edit's mesh started again on it; a waiting solve stopped |
| `anRf25ModelIsFramedAndItsTreeMenuReached` | The menu offering a ground where there is none, and no second where there is one |

Every test of a heater layout says its mesh now - `cells_across` 0 and the largest cell it had - so that
the tests keep the cells they were written for, a few hundred, and not the thousands ten across its trace
would give: those are a solve of minutes under CI's sanitizers.

## Broken on purpose

`docs/feature_gaps/2026-10-25-claude-rf25-phase-b-check/breaks.py` holds twenty-six: the per-metre values the
whole over the length again, the strips not tiled, summed too short a way, no length of charge on each copy;
an L and a step taken as lines; G per metre not of the cross-section; a lone conductor with no ground a line,
every stack taken as grounded, the starter without its ground; no cells across, no stretch, a shape's
feature its bounds', the edges not halved, a mesh refused rather than fitted, the cap at 4000, the node's
value unread; the Touchstone's comment, the status line, a pair's span, the tab's and the tool's lone lines
and the tab's two-ports of a pair, `brief`; and a loop's R at direct current of the least-energy current,
and the tool without `L_dc`.

`breaks_f4.py` beside it holds eighteen for the second commit: R from the sheet's crowding again; the walls
left out, the ground's image left out, the corners unrefined, one side of the difference only, the bottom face
not receding; no floor at direct current; a slab of the wrong conductivity; no thickness unsaid; a metal
ground's resistance nothing, its conductivity unread; a pair's R diagonal; the roughness left out of R, its
factor misread; a Solve after an edit started on the mesh before, an overtaken solve dropped, a waiting solve
not waited for; and the tool without `R_dc`.

## What the run taught

- **One break went uncaught at first**: the cross-section not compared along the line. The step the test
  held then stepped at the middle itself, where it cut the middle's cells into shapes that are no
  rectangles - so the rectangle check said "no line" first, and the comparison along the line was never
  needed. The step is at 300 µm now, its middle's cells whole rectangles, and only its cross-section a
  quarter of the way along says it is no line; run again, the break is caught. 26 of 26.
- **The new default mesh made every heater test a solve of thousands of cells.** The doc suite went from
  5 s to 196; under CI's sanitizers, where a dense solve is two hundred times slower, that is hours. Each
  test now says its mesh. Every changed test function was then timed in the sanitized build: none over
  40 s.
- **The tests found a wrong idea in the inductance**, not just a number: the bar's resistance at direct
  current, exact by construction on one cell across, came out 29 % high on ten. The fix is to the physics
  (above), not to the test.
- The full ctest (146), the scenario script (97 checks) and both lints were green.

And of the second commit:

- **The first numbers were 2 to 4 % high, and that was a bug, not the method.** The same solve written in
  Python, at the same cells, came out on the exact value; the C++ did not. Grouping a net's middle cells
  into its rectangle compared a width in micrometres with one in metres, so every cell was a rectangle of its
  own, walls and all - and on some meshes those walls coincided, the matrix went singular, and the line was
  refused: three of the reviewer's models came back with no R at all. With the units mended, every one of
  their microstrips gives the same R at every mesh.
- **The corners wanted refining.** W 300 µm moved 1.4 % from ten cells across to twenty - its 15 µm edge
  cells coarse beside a 3 µm metal, where the corner's field is. Halving them down to half the thickness
  made it 0.1 %. A break undoing it went uncaught at first, because it undid one corner of two; undoing both,
  it is caught.
- **The first test took five minutes under the sanitizers**, nearly all of it the solve of 800 µm lines R
  does not need. Lines four times their width with long cells along them took it to a few seconds - and the
  first try at that turned a 300 µm wide, 200 µm long strip on its side: the longer side is the line's axis.
- **A wire half a micrometre wide** - the heater example's - gets its resistance at direct current at
  10 GHz, rightly: its skin depth is not small beside it.

## Not done

- **R of a layout that is no uniform line** is still the sheet's crowding over the whole of it: Wheeler's rule
  is taken on a cross-section, and a bend or a pad has none.
- **Where the skin depth is near the thickness**, R is the larger of the resistance at direct current and
  Wheeler's with a slab's surface resistance: right at both ends, an interpolation between.
- **The ground's metal is taken as deep** - its bulk 1/(sigma delta) - since a ground node has no thickness.
- **The per-metre error is first order in the cells across**: 0.8 % at ten, 0.4 % at twenty. Grading the
  cells across toward the edges more steeply than one halving would do better than more of them; not tried.
- **A loop's inductance on the new default mesh** can pass its own cap (3000 runs) on a large inductor:
  ten across a spiral's trace is many runs. It refuses, saying so; the mesh's fitting is to the charge
  solve's cap, not to it.
- **Between direct current and the inductive limit** a loop's current is neither of the two given, and
  `(R + jwL)i = v` at each frequency is still the next thing for it.
- **A metal lid and a surface-impedance ground** are still not solved, as the phase A round said.
