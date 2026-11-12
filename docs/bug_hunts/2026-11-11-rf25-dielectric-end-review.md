# RF 2.5D: the dielectric that ends, after its test against the 2D engine

The request: the test report of 11 November on F2, the dielectric that ends
([2026-11-11-rf25-dielectric-end-and-inductance.md](../feature_gaps/2026-11-11-rf25-dielectric-end-and-inductance.md)).
It compares the uniform line's C per metre with the 2D multiphysics engine on the same cross-sections:

- **T1:** a 300 µm microstrip, 3 mm long, on 254 µm of εr 6.15 over a ground. The substrate is everywhere, a mesa 1500,
  600 or 300 µm wide (300 flush with the strip), ending 50 µm past one edge, or absent.
- **T2:** a coplanar line with no ground plane, 8/20/8 µm with 100 µm grounds. It sits on 0.6 µm of εr 28 (isotropic) over
  4.7 µm of oxide over 500 µm of silicon. The film is everywhere, a band 300 µm wide, a ridge 28 µm wide under the
  signal, or absent.

Its findings:

- **E1.** The flush mesa read 2.7–4 % high and did not converge. Past about 28 cells across, every ended case jumped by
  2–12 %, the 1.5 mm mesa to more than the substrate everywhere.
- **E2.** The thin film could not be solved at a real line's size: 6362 panels for the ridge at 5 cells across.
- **E3.** "A charge at 505.5 µm is outside the stack."
- **E4.** The one film case that solved, the ridge on a 200 µm line, read 241 pF/m. That is 2.2 times the 2D engine's,
  and more than the film everywhere.

## What was wrong

**E1: the coarsening, and the walls' rows.** Past 4000 panels the grading was coarsened as a whole: the smallest panel
grew by half at each try, those next to the conductors with the rest. The panels under a metal on the face are its
cells, so a finer mesh meant more panels, and from 28 cells across the rest were coarsened.
- The 3 mm flush mesa with room for every panel read 85.75 pF/m at 28 cells across. Coarsened, it read 91.67.
- Where it converged, the walls' eight rows were too coarse at the corner. The top row was 10 µm under a strip whose
  edge cells are a few µm wide, and the line read 2.3 % high. With sixteen rows it read 1.4 % high, and with 32, 1.2 %.

**E2: an edge refined wherever it ran.** Each panel was no larger than 0.4 of its distance from the conductors *or*
from the region's edges. An edge far from every conductor was cut as finely as one beside them:
- the box's own boundary, millimetres off;
- a band's or ridge's edges along a whole line.

A plate 20 µm square on the film, with the film's region far wider than the box, took 16,000 panels.

**E3: the step for the field.** A face's normal field was read off the potential at its middle and two points a step
further in. That step was a twentieth of the smallest panel. Coarsened, the smallest panel grew past the film's
thickness, so the bottom face's probes stepped through the 0.6 µm film and out of the stack.

**E4: the face's field, three ways.**
- **A difference of three expansions.** The normal field was a difference of potentials at three heights. Each
  potential's Green's function is an expansion of its own, its images binned and pruned for its heights, and the
  three agree to some parts in 10⁷ of the potential. Over a step of some tenths of a micrometre, a field from a
  millimetre off was then good to about a part in a thousand of itself.
  - A thin film's two faces carry nearly opposite charges. What reaches the conductors from far off is their
    difference, about t/D of either: a part in a thousand, a millimetre off.
  - The film's band, its region whole across the line, read the signal's C 7 % above the film everywhere. Its line per
    metre came out negative.
- **The field averaged at points.** A metal on a face of 28 has a bound charge under it that cancels all but 1/28 of
  its own. What is left, and so the capacitance, is what the fields between the film's two faces' panels make of it,
  amplified some 27 times. Against the film's own Green's function, a plate on a free film read:
  - 8 % high, averaged over four points a panel, at 1.25 µm cells;
  - 10 % low, at one point, at 2.5 µm cells;
  - 2.8 % high, over up to 144 points, at 1.25 µm cells.
- **The line per metre with the layer twice.** A line's per metre is the cross-section with the layer everywhere, plus
  the end's own difference: the correction as drawn less the correction with the layer drawn everywhere. Where the
  second did not solve, the first was added whole: the layer counted twice. The 500 µm ridge read 201 pF/m where the
  film everywhere is 134.

## What it does now

**A face's normal field from its own Green's functions** (`rf25_green.*`, Green's fourth constructor). The image
series is built as before. Each path's length moves with the point's height at a slope of one, up or down, and its
coefficient carries that sign:

    E_z(ρ) = Σ a z / (4π ε (ρ² + z²)^(3/2))

That is exact, one expansion for one pair of heights. The table over ρ holds (ρ² + z₀²)^(3/2) E_z, z₀ the nearest
image's distance. The field is held to the spectral integral's slope to 2e-4. The table is good to 6e-6 on the film
and 4e-7 under an oxide, where ρ³ E_z alone was 3e-6 a nanometre off.

- **Of a rectangle near a face's rectangular panel:** the field averaged over the panel exactly, for each of the 40
  strongest images: the integral of w/R³ over the two rectangles, a closed form summed over their corners
  (`rectanglePairField()`). The images beyond them are taken from the series.
- **Of another shape:** the solid angle it subtends (`solidAngle()`), averaged over points of the panel.
- **Of a wall:** its images as vertical rectangles, as their potential was: each field the line integrals along its two
  horizontal edges.
- **A wall reads its field as before,** across a step at its own height. All three of its potentials are of one Green's
  function, so their difference is of one expansion.

**The panels** (`dielectricCorrection()`):
- **Rectangles graded from the conductors' edges:** across an edge by its distance, along a straight one up to four
  times that, as the mesh stretches a line's cells. Along a metal's edge they may be as long as its cells there.
- **An edge of the region** refines the panels about it no finer than a quarter of the conductors' distance (times the
  grading).
- **The smallest panel:** on a face, the finest cell's longer side or half the layer's thickness. A wall's strips go by
  its shorter side.
- **A wall's rows,** within four of the finest cells' widths of a metal, begin a quarter of the finest cell from a face
  a metal is near, and grow by half. Elsewhere there are four.
- **Coarsening:** past the most panels, only how fast they grow away from the conductors is coarsened, from 0.4 of the
  distance to 0.6 and 0.9, and how far they stretch along an edge. It is never applied within two of the smallest
  panels of a metal's edge, and it is said.
  Past that, the solve is refused, saying why. The most is now 8000, a dense system of half a gigabyte, and
  `setDielectricMostPanels()` sets another.
- **E3's probes are gone:** a face reads no potential off it.

**A line's per metre** (`rf25_static.cpp`): where the correction with the layer drawn everywhere does not solve, there
is no per metre. The extraction says why, and that a shorter piece of the line gives it. The totals are as solved.

## Checks

Against the 2D engine's cross-sections, the report's own models solved again, C′ in pF/m:

**T1**, the microstrip, by cells across the strip. The report's readings are in brackets.

| Case | 2D engine | 10 across | 20 | 30 | 40 |
|---|---|---|---|---|---|
| No substrate | 28.43 | 28.29 | 28.40 | 28.44 | 28.45 |
| Substrate everywhere | 123.39 | 122.28 (−0.9 %) | 122.89 (−0.4 %) | 123.07 (−0.3 %) | 123.15 (−0.2 %) |
| Mesa 1500 µm | 123.20 | 122.17 (−0.8 %) [122.38] | 122.78 (−0.3 %) [122.39] | 122.96 (−0.2 %) [126.99] | 123.04 (−0.1 %) |
| Mesa 600 µm | 114.49 | 114.14 (−0.3 %) [114.81] | 114.68 (+0.2 %) [115.22] | 114.85 (+0.3 %) [118.14] | 114.93 (+0.4 %) |
| Mesa 300 µm, flush | 83.30 | 84.89 (+1.9 %) [86.36] | 84.74 (+1.7 %) [85.58] | 84.54 (+1.5 %) [93.74] | refused |
| Ends 50 µm past one edge | 111.25 | 110.91 (−0.3 %) [111.48] | 111.49 (+0.2 %) [111.51] | 111.61 (+0.3 %) [113.38] | 111.67 (+0.4 %) |

- Every case now converges smoothly with the mesh, and none reads more than the substrate everywhere.
- The ended cases follow the uniform substrate's own convergence (−0.9 to −0.2 %) to within 0.6 %. The flush mesa is
  the exception, 1.5–1.9 % high.
- The flush mesa at 40 across takes 9668 panels, and is refused, saying so.
- At 30 across each solve takes 18–32 s; at 40, 67–78 s.

**T2**, the coplanar line on the film, 200 µm long, at 10 cells across: the signal's C′ in pF/m, and the signal's
whole C11 in fF.

| Film | 2D engine | C′ | Ratio to the film everywhere, against the engine's | C11 | Before |
|---|---|---|---|---|---|
| everywhere | 139.17 | 134.36 | | 28.77 | 134.28 |
| absent | 80.97 | 79.03 | | 16.78 | 78.94 |
| a band 300 µm wide | 139.05 | 134.06 | 0.9978 against 0.9991 | 29.18 | "A charge at 505.9 µm is outside the stack" |
| a ridge 28 µm wide | 110.73 | 110.14 | 0.8197 against 0.7957 (+3.0 %) | 24.43 | 241.19; C11 40.9 |

- The ridge's C11 is now less than the film everywhere's, as it must be.
- Each of these takes 35–65 s, its two corrections each near 6000 panels with the growth coarsened, and said so.
- On the report's longer lines:
  - the 500 µm ridge solves, C11 59.06 fF against the film everywhere's 69.16. The correction with the film everywhere
    would take 10,032 panels, so there is no per metre, and the extraction says so.
  - the 500 µm band takes 11,266 panels;
  - the 2 mm ridge and band take 30,390 and 24,224.
  
  Those three are refused, saying why.

On the stack's own Green's function (`test_rf25_dielectric::aThinFilmIsItsStacksOwn`), a 20 µm plate in 2.5 µm cells on
the film, its region far wider than the box:

| Film | Region | Film everywhere | Air |
|---|---|---|---|
| on the oxide and silicon | 3.778 fF (+1.3 %) | 3.728 fF | 2.185 fF |
| alone in air | 1.361 fF (+2.1 %) | 1.332 fF | 0.784 fF |

Refined, the two converge on each other.
- In air, the film everywhere goes 1.332, 1.362, 1.375, 1.381 fF from cells of 2.5 µm to 0.3125, toward 1.387. The
  region read 1.367 at 2.5 µm, 1.404 at 1.25 and 1.426 at 0.625, as its panels were first graded; with finer panels at
  1.25 µm cells, 1.394.
- Before, the region read 1.385, 1.477 and 1.501, the error growing with the mesh.

## Found on the way

- **The coarsening, made only of the growth, still moved a plate by 5.5 %.** Along an edge, a panel's length was
  limited by the growth times its distance from the edge's end, and that is next to the metal too. The build's grading
  now holds within two of the smallest panels of a metal's edge. `panelsNextToTheConductorsAreNotCoarsened` caught it.
- **The field under a face that reflects 93 % changes by 3 % in 4 nm.** The first slope test read the field at the face
  against the slope 2 nm below it, and blamed the Green's function. Both are now read at one point.
- **A wall's rows fine wherever a metal is within the layer's thickness** took the 600 µm mesa from 1300 panels to
  3600, and changed its line by 0.03 %. They are fine now only within four of the finest cells' widths.
- **A face's panels as fine as the finest cell is wide** changed nothing beside the 1.5 mm mesa, but took it from 5000
  panels to 7600 and its solve from 17 s to 44. Only the walls' strips go by the cell's width now.
- **Stretch held at four everywhere** took the film's 200 µm lines' correction with the film drawn everywhere to 8112
  panels: no per metre. The stretch coarsens again beyond the panels next to a metal.
- **A line per metre on a short line.** At 1.5 mm, the flush mesa's per metre is 1 % above the 3 mm line's: the end's
  difference summed along a line of six substrate heights is short of the endless line's. That is F2's, not this
  change's; the test's line is 3 mm.

## What it does not do

- **A long line on a thin film is still past the most panels.** The panels under a metal on each of the film's two faces
  are its cells, and the panels beside them go by half the film's thickness. The report's 2 mm coplanar line takes
  24,000–30,000 panels at ten cells across, and is refused, saying so; the 500 µm band takes 11,300. A 200 µm piece
  solves in about a minute, and its per metre is the line's. Held compressed, as the full-wave solve holds its matrices, with an
  iterative solve, the panels could go far past this; that is a change of its own.
- **The flush corner is 1.5–1.9 % high,** against the 2D engine, whose strip is 0.2 µm thick, and past 30 cells across it
  is past the most panels.
- **The ridge's per metre is 3 % above the 2D engine's ratio** to the film everywhere, at ten cells across: 0.820 against
  0.796. The plain film cases are themselves 3.5 % below the engine at that mesh (the sheet against 0.1 µm of metal).
- **The solve takes longer.** Two corrections are solved for a line's per metre, each a dense system of up to 8000
  panels. The 3 mm flush mesa takes 6 s at twenty cells across and 32 s at thirty; the film's 200 µm ridge, about a minute.

## Where it is

- `rf25/rf25_green.*`: the normal field's Green's functions, their table, `normalField()`, `normalFieldExact()`,
  `normalFieldBeyond()`.
- `rf25/rf25_dielectric.*`:
  - the face's field: `horizontalField()`, `wallField()`, `rectanglePairField()`, `solidAngle()`;
  - the panels' grading, rows and coarsening;
  - `dielectricMostPanels()` and `setDielectricMostPanels()`.
- `rf25/rf25_static.cpp`: no per metre where the layer drawn everywhere does not solve.

## Tests

- `test_rf25_dielectric`:
  - `aNormalFieldIsItsPotentialsSlope` (new);
  - `aRectanglePairsFieldIsItsClosedForm` (new);
  - `aThinFilmIsItsStacksOwn` (new);
  - `panelsNextToTheConductorsAreNotCoarsened` (new);
  - `anOxideOverTheSubstratesFace`, within 2 % where it was 1.5 %. The stack's own answer moves 2 % from cells of 12.5 µm
    to 6.25, and the two meet as they are refined: 1.8, 1.1 and 0.7 % apart at 12.5, 6.25 and 3.125 µm.
- `test_rf25_doc`:
  - `aStripFlushWithItsMesaIsThe2DEnginesLine` (new): the report's T1 flush mesa at ten and twenty cells across;
  - `aSubstrateThatEndsIsSolvedByBothSolvers`: a block of substrate beyond the stub's end with room for its own panels
    alone, so no per metre, said.

[breaks.py](2026-11-11-rf25-dielectric-end-review/breaks.py) undoes each piece on purpose, builds and runs its tests:
[results.txt](2026-11-11-rf25-dielectric-end-review/results.txt). 12 of the 13 are caught.

- **Two were not caught at first.** The field's table scaled by ρ³ alone was 3e-6 out a nanometre off, under its 1e-5
  bound. The test now holds the table within 1e-6 over seven decades. The panels next to a metal coarsened did not
  move a mesa 60 µm wider than its plate. The test now also squeezes a plate flush all round, which must be refused.
- **One is still not caught:** two face panels' mean field sampled at points rather than in closed form, where the
  metal's cells' stays exact. At 2.5 µm cells, the thin film then reads −0.4 and +1.0 % of the film everywhere, rather
  than +1.3 and +2.1 %. The film everywhere is itself 5 % short of its limit there, so no bound at this mesh tells
  the two apart. With refinement the exact pairs converge on the limit; sampled ones settle by the number of points.

The suites of RF 2.5D, the tools and the layouts pass. `test_qucs_control` failed three tests, all with file dialogs,
when run four suites at a time, and passes alone. The four lints pass.
