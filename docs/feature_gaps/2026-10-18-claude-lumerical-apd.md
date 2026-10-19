# Lumerical's avalanche photodetector: the six issues against now

The reviewer's "Lumerical CHARGE Avalanche photodetector, reproduced in Qucs-S" (`2026-10-13-claude-lumerical-apd.md`, a build of 13 October) rebuilt Lumerical's Ge-on-Si waveguide APD (Huang et al., Optica 2016) by hand and compared its dark current:
- 0 to -4 V within 1.5 times of Lumerical's and on the measurement;
- -5 to -8 V up to 2.5 times low;
- breakdown: the current ran away between -9.25 and -9.5 V; Lumerical's and the device's near -9.9 V.

They listed six Qucs-S issues and left the photocurrent, gain and bandwidth undone. Issues 5 and 6 came the next day in `2e349c6`; the rest is done here, and the undone part with Lumerical's own project, imported.

| Issue | Then | Now |
|---|---|---|
| 1. Bias continuation slow, not adaptive | 80 steps to -0.5 V, 1270 (2519 s) from -9.25 to -9.5 V; 83 minutes for the sweep | **Done:** each step starts on the line of the last two solutions; their model's sweep in 39 s, 1401 s before on this machine |
| 2. Cancel loses the sweep | every point solved discarded | **Done:** the points before kept, where and why said |
| 3. No progress past the point | "Vc = -9.5 V: Solving Stationary" | **Done:** its point of the sweep, its contact's voltage and step, Newton's step and residual |
| 4. `semi.In` no breakdown flag | 0.91 at -9.5 V as the current ran away | **Done:** the most of every field line's, where; the field's own error found and a driving field to choose; a flag from the equations, `semi.Mion` |
| 5. TAT, surface recombination, Masetti, lifetimes of x and y | missing | **Done in `2e349c6`** (Hurkx's TAT, Surface Recombination, Masetti's mobility, lifetimes of x, y and doping) |
| 6. A 2D generation map | an SVD of 1D tables | **Done in `2e349c6`:** an interpolation of two arguments on a grid |

## 1. The way to the contacts' voltages

**Before.**
- Each step started from the last solution itself.
- The step doubled only after fewer than 8 of Newton's steps and was cut to a quarter where Newton failed.
- A step cut where it was capped (0.25 V left of a 1 V step) was tried again as it was.

**Now.**
- **Each step starts on the line of the last two solutions:** the potential on it, the carriers on their logarithms' (a junction's densities go as e^(V/Vt)), as far beyond the last as the step asks, the carriers' factor at most 10³.
- **The step adapts:** twice as long after at most 5 of Newton's steps, 1.4 times after 10, kept after 20, shortened after more; halved, as tried, where one fails.
- **A sweep's next point** starts on its last point's last step's line, its first step four times that step's length.
- **Its log** says how many steps it took again: "reached in 44 steps (365 of Newton's; 2 tried again)".

**The reviewer's model** (`apd.qfem`, its 34 points from 0 to -10.04 V, dark), on this machine: 1401 s with the build before this round, 39 s now (the reviewer's 83 minutes on theirs). The same currents, point by point, within 2e-4; 2e-3 at -9.25 V, where the current is steepest.

**A walk's end solved again.** A semiconductor's setup walks to its voltages, and its step's own solve then solves there again (the light's pairs may have been added). Where nothing had changed, Newton's method from that solution took its forty steps and failed: an avalanche diode without recombination at -40 V. Now, where the equations are met at its start, no step is taken.

**Not done: current-controlled or arc-length continuation.** The walk through breakdown, at voltage control, now takes seconds: their model past -9.5 V at some 0.5 s a step. A current-driven contact is a contact of its own, and no snapback device needs it yet.

## 2. A stopped sweep keeps its points

A sweep cancelled (the tab's Cancel, Claude's `fem_stop`), or a point of it without a solution (Newton's method failed, its geometry or mesh not made), keeps the points before. It says "Parametric Sweep stopped at Va = -3 V (point 3 of 3): cancelled - its 2 points before kept":
- in the model tab, in red, its points shown;
- in `fem_solve`'s result as `stopped`;
- in `fem_describe`'s study as `stopped`.

At its first point, nothing to keep: an error, as it was.

## 3. How far

While a study runs, its words say:
- its point of the sweep, "Va = -48 V (point 3 of 5)";
- a semiconductor's way there, "a at -47.6 V of -48 V (step 4, 120 mV)";
- Newton's step and its residual, "Newton's step 3, its residual 2.1e-05".

The tab's bar shows them; `fem_solve`'s "Still solving after 600 s, 61 % - …" and `fem_describe`'s `at work` give them to Claude.

## 4. The breakdown flags

**The most of the field lines.** `semi.In` and `semi.Ip` were the integrals along the one field line through the highest field. Now they are the most over the field lines from every corner whose α is a thousandth of its most or more, the most ionizing first, each covered corner skipped (2000 lines at most). Two new globals, `semi.xion` and `semi.yion`, give In's line's highest field: where it breaks down first.

**In the reviewer's model the line was not the cause.** On this build:
- The most over all lines at -9.5 V is 0.918 (their single line's 0.91). It lies where it should: the i-Si's n⁺ edge, near the axis (y 0.82 µm).
- No field line reaches 1 as its current runs away.
- Half the mesh in the multiplication and charge layers changes neither: runaway at -9.5 V, In 0.922.

**The cause: the field's ionization at rest.** The equations drove α by each triangle's |E|. At -9 V a fifth of the pairs are made in the germanium: 9 % in its absorber, 11 % in its p⁺⁺ top. The p⁺⁺ Ge's Gaussian (σ 6.6 nm) holds a field of 2.6e5 V/cm at 0 V, where drift and diffusion balance and nothing is heated. That feedback, off every field line, ran away first.

**A driving field to choose**, Impact Ionization's Driving field:
- **the field |E|**, as it was (the default);
- **each carrier's quasi-Fermi gradient**, |∇φn| and |∇φp**|**: the force that drives its current, the field where it drifts, nought where drift and diffusion balance (the Lumerical grad phi);
- **the field along each carrier's current**, E·J/s with s = √(|J|² + J0²) and J0 a millionth of its drift q n µ |E| (the Lumerical E dot J). As E·J/|J| it took the noise's direction where the current is nought: a steep contact layer at rest found no solution. With J0 a thousandth, it cut a saturated germanium's F by 0.25 %.

Each has its derivatives in Newton's Jacobian.

**The reviewer's model, dark, device current (2 × the half's):**

| V | \|E\| | quasi-Fermi | E·J | Lumerical | measured |
|---|---|---|---|---|---|
| -1 | 0.135 nA | 0.123 | 0.123 | 0.306 | 0.081 |
| -4 | 9.43 | 7.92 | 8.00 | 11.2 | 14.2 |
| -8 | 79.4 | 48.1 | 49.0 | 199 | 244 |
| -9 | 337 | 109 | 108 | 469 | 451 |
| -9.5 | 3.2 mA: run away | 252 nA | 243 | 1020 | 775 |
| -9.9 | 17 mA | 785 nA | 675 | 5451 | 4282 |
| In = 1 at | (0.92 at -9.5) | -9.98 V | -9.98 V | | |

With either physical driving field there is no runaway, and In and Ip reach 1 together, at -9.98 V: where Lumerical's own run diverged, past -9.98. Below breakdown this model is 2 to 4 times low. It lacks Lumerical's trap-assisted tunnelling and surface recombination; Lumerical's own project, imported, has them (below).

**From the equations themselves:**
- `semi.Iion` is impact ionization's pairs' current, q ∫ G.
- `semi.Igen` is the current of the pairs made otherwise: thermal, tunnelling, light, surfaces.
- `semi.Mion` = 1 + Iion/Igen is the multiplication of all of them. It is finite and monotone, and it rises without bound toward breakdown.
- `semi.Gion` is the impact pairs per volume and time, at a point: where an avalanche makes them.

I/(I - Iion) was tried first. Recombination of a hundredth of the impact pairs made it negative before breakdown.

Mion is a photocurrent's gain where all the light's pairs reach the field. In `anAvalancheGain`'s diode only 42 % of the light's holes reach it, and Mion is 1 + (M - 1) × 0.42.

## The reviewer's "not done yet": Lumerical's own project

`avalanche_photodetector_electrical.ldev` was imported with this build. It carries:
- each material's models: Hurkx's TAT, Selberherr's ionization, the high-field mobility;
- the five surface recombinations, the two at the metals with their switch off (the vertical photodetector's round);
- its imported generation, off for the dark sweep as Lumerical's.

**Impact ionization's driving field, imported.** Lumerical's impact ionization has two: grad phi and E dot J. The APD's Driving Field is 0, and its own bandwidth script sets E dot J, so 0 is E dot J (inferred). Run both ways, |E| reproduces Lumerical's results, so the importer takes Lumerical's E dot J as the field |E|, said; its grad phi (1) as the quasi-Fermi gradient. The importer had taken |E| for both, the driving field not read.

**Dark** (device current):

| V | Imported, \|E\| | Imported, E·J | Lumerical | measured |
|---|---|---|---|---|
| -2 | 1.40 nA | 1.18 | 1.20 | 0.81 |
| -4 | 15.6 | 13.0 | 11.2 | 14.2 |
| -6 | 59.6 | 46.1 | 60.7 | 72.0 |
| -8 | 205 | 126 | 199 | 244 |
| -9 | 500 | 216 | 469 | 451 |
| -9.5 | 1180 | 307 | 1020 | 775 |
| -9.9 | 40.4 µA, broken down | | 5.45 µA (33.7 µA at -9.98, then no solution) | 4.28 µA |

**With |E|:**
- 0.95 to 1.16 of Lumerical's from -4.5 to -9.5 V.
- It breaks down at -9.9 V: `semi.Mion` 80, 185 and 3910 at -9.8, -9.85 and -9.9 V.
- `semi.In` there is 0.90: the field's ionization off the lines again.

**With E·J:** 0.3 to 0.7 of Lumerical's near breakdown, and it breaks down past -10.5 V.

**Lit** (3.42e-7 W, Lumerical's own voltages; gain as Lumerical's FOMs script has it, I_photo/I_photo at its unity voltage, -3 V):

| V | Photocurrent | Lumerical | Responsivity | Lumerical | Gain | Lumerical |
|---|---|---|---|---|---|---|
| -2 | 114 nA | 89.7 | 0.329 A/W | 0.259 | 0.92 | 0.82 |
| -4 | 145 | 129 | 0.377 | 0.344 | 1.17 | 1.18 |
| -6 | 235 | 223 | 0.512 | 0.475 | 1.91 | 2.05 |
| -8 | 532 | 503 | 0.958 | 0.888 | 4.32 | 4.62 |
| -9 | 1155 | 1079 | 1.92 | 1.78 | 9.37 | 9.91 |
| -9.5 | 2570 | 2216 | 4.06 | 3.50 | 20.9 | 20.4 |

From -2.5 to -9.5 V the photocurrent is 1.04 to 1.16 of Lumerical's. At 0 to -1.5 V it is 1.6 to 3.9 times, the light's carriers collected before the depletion reaches the germanium. Past -9.75 V it breaks down a tenth of a volt before Lumerical's.

**Bandwidth against gain** (the light modulated, `semi.Hop_collector` from 100 MHz to 100 GHz, 10 a decade; gain the response's at 100 MHz against -3 V's):

| Driving field | At gain | 3 dB | Gain × bandwidth |
|---|---|---|---|
| \|E\| | 11.9 (-9.5 V) | 9.4 GHz | 112 GHz |
| \|E\| | 43.7 (-9.8 V) | 3.0 GHz | 130 GHz |
| E·J | 12.9 (-10.8 V) | 30.8 GHz | 396 GHz |
| quasi-Fermi | 13.5 (-10.4 V) | 30.8 GHz | 416 GHz |
| Lumerical (its page) | 12 | about 30 GHz | |
| measured (Huang et al.) | 12 | 25 GHz | |

So one model does not match both here:
- **|E|** gives Lumerical's DC: dark and photocurrent, gain by voltage, breakdown. Its bandwidth is a third of Lumerical's, its germanium's ionization at rest feeding back slowly.
- **The quasi-Fermi gradient and E·J** give Lumerical's bandwidth at its gain. Their breakdown is 0.5 to 0.9 V past Lumerical's.

Without Lumerical its own E dot J cannot be read further.

## Found on the way

- **A small signal's first frequency** differed from its next: an APD's response 4.44 at 100 MHz, 6.25 at 100 MHz again. A step's semiconductor is made fresh, and its Fermi-Dirac degeneracy is set by its first assembly. The mobilities saturated along the current, which read it, were updated before that, with Boltzmann's. Now the degeneracy at x comes first. It was older than this round (the build before gave the same 4.44) and touched Fermi-Dirac models with saturation alone (the photodetector's Boltzmann response was right).
- **The importer's impact driving field**, above.
- **Not fixed: a wall.** A 1e16 diode with velocity saturation and impact ionization, and with Fermi-Dirac statistics or light, finds no solution past -22.7 V (its breakdown 51.7 V), on this build and the one before. Newton's method holds the mobilities, its potential still to 1e-13 Vt. One equation stays short of its 1e-6: a hole's at the device's insulated edge in the depletion, its holes 4e7 m⁻³, changing a few percent each step. Its cause was not found. Lumerical's APD, which has all of these, solved to -10.5 V.
- **CI's Linux at `28dab4c` failed:** `test_fem_quantum` ran past its 5400 s. It is 1660 s sanitized here (its note said 697), and its additions since the last pass are 28 s. Its functions now run in two ctest entries, each with its own limit and the two at once: `QUCS_TEST_PART` 1 runs those before `aBoundContinuum`, 2 runs that one and those after, unset all of them (its `init()` skipping the other part's). Sanitized here, some 820 and 840 s.

## Not done

- A current-driven contact, or arc-length continuation (above).
- The wall (above).
- A generation read straight from a file (the vertical photodetector's round).

## Tests

- **test_fem_devices `aWalkStartsOnTheLine` (new).**
  - An avalanche diode toward breakdown, -40 to -51 V: 250, 96, 31, 50 and none of Newton's steps. From the solution alone it took 1367 to -40 V; from the last point's solution, 74, 45 and 163 to -48, -50 and -51 V.
  - The point, the contact's voltage and step, Newton's step and residual told.
  - Without recombination, solved at -40 V.
  - A sanitized build's coarser mesh has its own limits.
- **`aSweepStoppedKeepsItsPoints` (new):** cancelled at point 3 of 3, its 2 kept, said; at its first, an error; a point's geometry not made (a length of -1), the points before kept.
- **`theMostOfTheFieldLines` (new).** A 1e16 junction beside a punched-through 1e17 one of 0.1 µm, which has the highest field and α but a short line (In 0.43): In is the 1e16's, as it alone has it (0.85316 against 0.85310), and where.
- **`ionizationsDrivingField` (new).** A p⁺⁺ Gaussian contact layer of 6.6 nm, germanium's α's:
  - by the field it makes pairs; by the quasi-Fermi gradient or along the current, a millionth as many or fewer;
  - outside it the field makes the most, the gradient the fewest, along the current between.
- **`aSmallSignalsFirstFrequency` (new):** Fermi-Dirac, saturation along the current, impact ionization, light; the same frequency twice gives the same response, to the bit.
- **`anAvalancheGain`:** ∫ q semi.Gion is semi.Iion to 1e-9, and semi.Mion is 1 + (M - 1) × the share of the light's holes that reach the field, to 3 %.
- **`newtonStopsNotShortOfTheSolution`:** the fault at work is now seen in its first step's Newton's steps (6 against 4), its mesh the same on every build.
- **test_fem_laser `onePointAboveThreshold`:** the log's "tried again" read; 166 of Newton's at 1.6 V (271 before).
- **test_fem_lumerical `aDeviceProject`:** impact ionization's driving field the field, its Lumerical one not given, said.
- **test_multiphysics_doc `claudeStopsASolve`:**
  - a two-point sweep stopped in its second keeps its first, said cancelled, not an error;
  - `fem_solve`'s "still solving" gives the progress;
  - `fem_describe`'s `at work` gives point 3 of 3, the contact's voltage, its step and Newton's;
  - stopped, its 2 points kept, said in `fem_stop` and `fem_describe`.

## Breaks

Nineteen changes broken on purpose, all caught:
- **The walk:**
  - no start on the line;
  - a sweep's point not on its last line;
  - its end solved without checking the equations first.
- **Told:** the problem's words not passed; no point of the sweep.
- **Stopped:** the points before not kept; not on the solution set; `fem_describe` silent of it, and of `at work`.
- **The lines:** one line, the most ionizing; the starts a third of the highest field (the old choice).
- **The globals:**
  - Iion a third of each triangle's;
  - Igen without the light;
  - semi.Gion not stored.
- **The driving fields:**
  - the quasi-Fermi potentials without the carriers;
  - the quasi-Fermi option not read;
  - E·J's direction not regularized.
- **The mobilities' degeneracy not first.**
- **The import's grad phi for E dot J.**

## Runs

- **Release:** the suite 130 of 130.
- **Under the sanitizers, with Eigen's LU (as on CI's Linux):** no report.
  - `aWalkStartsOnTheLine` 77 s, `aSweepStoppedKeepsItsPoints` 7 s, `theMostOfTheFieldLines` 62 s, `ionizationsDrivingField` 219 s (coarser, at -1 V there);
  - `aSmallSignalsFirstFrequency` 23 s, `anAvalancheGain` 160 s, `newtonStopsNotShortOfTheSolution` 138 s;
  - `onePointAboveThreshold` 70 s, `aDeviceProject` 1 s, `claudeStopsASolve` 50 s;
  - `test_fem_quantum`'s 36 functions 1660 s in all.
- **The lints pass.**
