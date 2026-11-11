# RF 2.5D: an adaptive frequency sweep and a mesh convergence check

The request: F1 of the open items of 8 November
([2026-11-08-claude-rf25-open-items.md](../bug_hunts/2026-11-08-claude-rf25-open-items.md)), then "Build F1. We can make
both adaptive sweep and convergence check optional."

Both are optional. The study's "Add points where it changes" is off unless it is ticked, and the convergence check runs
only when asked for. A model that asks for neither solves as it did, with one line more: the mesh note.

## The adaptive sweep

The study's "Add points where it changes" was a checkbox that did nothing: the solve said "not done yet" and solved
every point. It now does what it says.

**How it goes** (`adaptiveSweep()`, `rf25/rf25_adaptive.cpp`):

1. The sweep's two ends and three points between, evenly, are solved first, side by side as a sweep is.
2. Then the middle of every gap: nine points, evenly spread.
3. Then, round by round, where the fit of everything solved differs most from the fit before the last round. Up to
   four points a round, and no two in one gap.
4. Before each round is solved, the fit says what it expects there. The sweep stops when it said S at the points solved
   last to within the tolerance (0.002 by default, the study's Tolerance), at two or more of them in a row. Those are
   the points it was least sure of.
5. The other frequencies are read from the fit.

**The fit** is set-valued AAA (Nakatsukasa, Sète and Trefethen, 2018; Lietaert et al., 2022): one rational function
in barycentric form for every term of the ports' Z, all of them sharing one denominator. Support points are added where
the fit is worst until it is within 1e-7 of every sample. Because the coefficients are the same for every quantity,
the ports' currents at a frequency not solved are carried by the same formula from the currents at the support
frequencies. They have the structure's poles, so the Currents view and the equivalent circuit work at every
frequency.

**Calibrated ports.** The calibration strips are solved only at the frequencies the structure was solved at. Their
discontinuity, the line's propagation constant and its impedance are then read at the other frequencies from a rational
fit of their own (`calibrationAt()`), and the de-embedding is done at every frequency as before. The strips were the
larger cost of a calibrated solve, so this matters as much as the structure's own points.

**A short sweep** of 12 points or fewer is solved whole and says so: adding points would save little.

**What it says.** The solve's lines say how many of the sweep's frequencies were solved, in how many rounds, the
fit's degree, and how close it was at the last check; then which frequencies were solved. `FullWave::solvedFrequencies`
holds them, the solution kept beside the model keeps them (its format 3; files of 1 and 2 still read), and
`rf25_solve` gives them as `solved frequencies`.

### On a plain coplanar line

A 500 µm GS line, a 50 µm signal in 6 µm gaps over 500 µm of silicon and no ground plane, its ports calibrated, swept
from 10 to 110 GHz at 101 points:

| | Frequencies solved | Solve | S against the whole sweep |
|---|---|---|---|
| Every point | 101 | 76.7 s | |
| Adding points | 13, in 3 rounds | 13.2 s | 0.0014 at most (at 42 GHz) |

The fit had 9 support points, and said S at the last 4 points to within 8e-5.

On the tests' microstrip (1000 µm on 100 µm of alumina, 41 points to 41 GHz, both ports calibrated) it solved 13 and
read the rest within 1.5e-5 of the whole sweep. On closed forms: a resonator's two-port, 13 of 201 points, exact to
rounding; a lossy line with 16 resonances in the band, 45 of 301 points, S within 1.5e-4 everywhere at a tolerance of
1e-3.

## The mesh convergence check

**RF 2.5D > Check Mesh Convergence…** (and the new tool `rf25_converge`) solves the model on several meshes, each finer
than the last, and says what each value did.

**The meshes.** Each one is named by its cells across the narrowest conductor. Left to itself, the check takes a
quarter, a half and all of the model's own (3, 5 and 10 for the default 10; 1, 2 and 3 for 3). The finest is the
model's own mesh, so the cost is about one solve of the model, and the answer is how far the model's own mesh is from
the limit.

Each mesh is the model's own **refined as a whole** (`refinedBy()`): the cells across a conductor and per wavelength,
the largest cell, and the share of a gap its facing cells are graded to, all scaled together. The first version changed
the cells across alone, and on the GS line above the three meshes came out the same, 1107 cells each: there the largest
cell and the gaps' grading set the size, not the cells across. The grading of a gap was a constant (a quarter of the
gap) and is now `MeshSettings::gapShare`, a setting the check scales. A full-wave check's calibration strips are
meshed with each mesh's settings too.

**What it watches.**

- Full wave, at the top of the study's sweep or the frequencies asked for: every term of S (S21 alone of S12 and S21),
  and of each calibrated port its feed's ε_eff and Z0 and its own discontinuity's C and L.
- Quasi-static: each net's capacitance and the mutual ones, and a uniform line's ε_eff and Z0.

**The verdict** of each value, from its last three readings (`convergenceOf()`):

- **Converging**: its changes shrink at least as fast as the square root of the cell size (a ratio under 0.71 where the
  cells halve). Its order is found from the cell sizes as they are, not assumed to be halving. It is extrapolated by
  Richardson to a limit, and the finest mesh's distance from it is given.
- **Oscillating**: it shrinks but turns back, so the last change bounds it.
- **Not converging yet**: its changes do not shrink fast enough. The ratio a converging value would have had is given
  beside it, and a finer mesh is wanted.
- **Settled**: it did not move.

The model, its mesh and its solution are left as they were. The check's report is shown in the tab's notes until the
model or its layout changes. It runs as a solve does, with the bar counting each mesh, Stop and `rf25_stop`, and joins
and waits like `rf25_solve`.

### On the same coplanar line

At 110 GHz, on 1, 2 and 3 cells across (the model's own is 3):

| Mesh | Cells | Unknowns | Time |
|---|---|---|---|
| 1 across | 286, 3.1 to 56 µm | 799 | 0.2 s |
| 2 across | 646, 1.6 to 29 µm | 1847 | 1.0 s |
| 3 across | 1107, 1.0 to 20 µm | 3199 | 4.9 s |

- ε_eff of the feed: 3.887, 4.039, 4.128. Not converging yet: it moved by the same amount per refinement on both steps
  (0.59 times the change before, where a converging value would have had 0.44 or less for these sizes). The line's
  ε_eff at the model's own mesh is still moving by about 2 %.
- Z0: 45.16, 44.01, 43.40 Ω, the same.
- The port's own discontinuity C: 1.349, 1.428, 1.441 fF. Converging as h^2.2, toward 1.450 fF, the finest within 0.6 %.

So the check says something the solve alone could not: on this line at 3 cells across, the discontinuity is right
to a fraction of a per cent, but the line's ε_eff and Z0 want a finer mesh.

On the tests' microstrip, 2, 4 and 8 cells across at 5 GHz: S converges at order 0.9 to 1.1, the finest within 0.0035
of the limit. First order is what piecewise-constant charge against the edge singularity gives (A1 of the open items).

## The mesh note

Every solve's lines now start with what the mesh is against what it cuts, for example: "Solved on 1107 cell(s) from
1.04 to 20 µm; 3 across the narrowest conductor, 50 µm, each 16.7 µm there; the narrowest gap 6 µm, 5.8 times the
smallest cell. How far that is from the limit: RF 2.5D > Check Mesh Convergence." The mesher records the numbers in
`Mesh` (`narrowest`, `across`, `acrossCell`, `narrowestGap`, `smallest`, `largest`).

## Where it is

- `rf25/rf25_adaptive.h`, `.cpp` (new): the rational fit, the adaptive sweep, the calibration read between its
  frequencies, the convergence verdicts, the check's values and report, and the default meshes.
- `rf25/rf25_mesh.*`: `refinedBy()`, `MeshSettings::gapShare`, and the mesh's sizes for the note.
- `rf25doc.*`: the full-wave solve and its calibrations split out of the solve's thread (`solveWaveOn()`), so the
  sweep and the check share them; the check as a kind of solve (`SolveRequest::levels`, `convergeOn()`); the bar's
  count; the notes; `convergence()`.
- `rf25/rf25_model.cpp`: the study's "Add points where it changes" shown for a full-wave study, with its help, and its
  Tolerance.
- `rf25/rf25_kept.cpp`: format 3, the frequencies solved.
- `qucs_rf25.cpp`: RF 2.5D > Check Mesh Convergence…, asking for the meshes.
- `qucscontrol_rf25.cpp`, `qucscontrol.cpp`, `qucscontrol_jobs.cpp`: `rf25_converge` (listed as a solve among the
  background jobs), and `rf25_solve`'s `adaptive`.

## Tests

`test_rf25_adaptive` (new, under a second):

- `aRationalFunctionIsFoundAgain`: a resonator's Z from 30 samples, within 1e-6 between them.
- `anAdaptiveSweepSolvesFewAndReadsTheRest`: 13 of 201 points, S within 2e-3 everywhere, the ends solved, the lines.
- `aLineOfManyResonancesIsReadWithinTheTolerance`: 16 resonances, fewer than half the points, within 3e-3.
- `theCurrentsAreCarriedByTheFit`: currents at frequencies not solved within 1e-4.
- `aShortSweepIsSolvedWhole`, `aStoppedOrFailedSweepSaysSo`.
- `aCalibrationIsReadBetweenItsFrequencies`: within 1e-4 between every fourth frequency.
- `reciprocityAndPassivityAreTakenOverEveryFrequency`.
- `aValueConvergingIsExtrapolated`: order 2 and 1.5, sizes halving and not, complex values, four meshes, settled.
- `aValueNotConvergingIsSaid`: growing, too slow, turning back, two meshes.
- `aMeshIsRefinedAsAWhole`.

`test_rf25_doc`:

- `anAdaptiveSweepSolvesFewOfItsFrequencies`: calibrated ports, against the whole sweep, its Tolerance shown with it,
  kept and opened again, 12 points solved whole, a Tolerance of 2 refused.
- `aMeshConvergenceCheckSaysHowFarTheMeshIsFromTheLimit`: full wave and quasi-static, the meshes refined as a whole,
  the solution shown left alone, the bar, gone with an edit.

`test_qucs_control`'s `anRf25ModelsSweepAndMeshAreCheckedThroughTheTools`: `rf25_solve`'s `adaptive` and what it
refuses; `rf25_converge` full wave and quasi-static, and what it refuses.

[breaks.py](2026-11-11-rf25-convergence/breaks.py) undoes each piece on purpose, builds and runs its tests:
[results.txt](2026-11-11-rf25-convergence/results.txt).

## Left

- The convergence check is the uniform one. An adaptive refinement (where the error is, as Multiphysics' does) is (4)
  of F1 and is not built.
- The check is kept in memory, not in the model or beside it.
- A Touchstone file written from an adaptive sweep does not say which of its frequencies were solved.
