# The hunt of 19 October, fixed

The user (2026-10-19), after the hunt's report (`docs/bug_hunts/2026-10-19-apd-round-and-ci.md`): *"yes, commit the fix and fix all of them."* The File Types tab the user reported during the hunt was committed first, on its own (`12de010`).

Everything is fixed: A1 and A2, B1 to B9, F1 and F2, C1 to C3, the notes N1 to N12 but N7, and T1 and T2. N7 is left as it is, with the reason below. Two more unchecked casts turned up in the Lumerical import while F1 was fixed (an FDE's trial modes and wavelength points), and are fixed with it.

## A1 and A2. Impact ionization near a junction, and Newton's method near breakdown

They were one fault. In the field driving, a triangle's current is fitted from its three edges' Scharfetter-Gummel currents. On the p⁺ side of a junction, between two nodes of some 10²⁴ holes per m³, an edge's hole current is the difference of drift and diffusion terms of some 10¹² A/m². Its rounding, some 10⁻¹³ of those terms, is ten thousand times the true current there. That rounding went into αp|Jp|:
- **A2:** `semi.Iion` and `semi.Gion` counted it, 99 times the pairs added without recombination;
- **A1:** Newton's method cycled at the minority electrons beside it near breakdown: their equation filled with the holes' rounding, they changed 2.7 % step after step. A sweep stopped at -50 V, 1.6 V below breakdown.

**Now** (`fem_semiconductor.cpp`, the impact ionization's assembly):
- **Each edge's rounding:** 10⁻¹⁰ of the two terms its current is the difference of.
- **A weighed fit:** in the field driving, each edge counts by how far its current stands above its rounding, 1/(1 + (f/ρ)²), ρ the triangle's largest edge current. A junction's triangle takes its current from its two edges across the junction. With fewer than two edges above their rounding, the fit is as it was.
- **What is no more than the rounding is smoothly nought:** J²/√(J² + J0²), J0 the fit's own projected rounding, its Jacobian exact in J (J0 is a rounding's: its own changes left out).
- **The quasi-Fermi and along-current drivings** are as they were. Neither had the fault: the first's gradient is flat where the majority's current is, the second's J0 is a millionth of the drift.

A first try took J0 from the triangle's mean density. That cut the noise but also the junction's genuine current, wherever a triangle has a p⁺ corner: the multiplication at -40 V was 1.431 on a mesh of 0.1 µm and 1.521 on 0.05, where it converges to 1.60. The per-edge fit converges:

| hmax | no change (noise) | mean density | per edge, weighed |
|---|---|---|---|
| 0.1 µm | 1.580 | 1.431 | 1.586 |
| 0.05 | 1.593 | 1.521 | 1.599 |
| 0.02 | (no solution) | 1.570 | 1.600 |
| 0.01 | (no solution) | (no solution) | 1.590 |

`semi.Iion` is now the pairs added: without recombination 3.571·10⁻¹⁴ A against 3.571·10⁻¹⁴ (the short diode's diffusion current times Mp - 1, 3.64·10⁻¹⁴, expected); with SRH 1.0000 of them on every mesh.

**One more convergence test** (`newton`): the equations met to 10⁻¹⁴ of their flows - their rounding - count as solved. On the 0.01 µm mesh at -32 V they were met to 10⁻¹⁶, the carriers wandering by up to 5·10⁻⁵ step after step: the linear solve's own rounding. A first try of 10⁻¹² took a current of 10⁻¹³ A among majority flows of 10⁻² A as it was, 15 % off, and a per-carrier test (a change below 10⁻⁸ of its own equation's flows) accepted states too early too; both were dropped.

**The near-breakdown sweep** (-49, -50 … -51.4 V) reaches every point on every mesh tried (0.03 to 0.1 µm, 0.098 among them), each after -50 V in one step.

**The imported Lumerical APD** is unchanged: its dark and lit sweeps' currents 1.0000 of the round's from -4 V to breakdown (-1 V 0.977, -2 V 0.998: at 10⁻¹⁰ A). There the p⁺⁺ layer's pairs come from currents well above their rounding.

`ionizationsDrivingField` read the field's pairs in a contact layer at a few nA, where the layer's hole current is 10⁻¹³ of its drift and diffusion - that rounding. It now checks two things. Dark, no driving makes pairs in the layer. Lit (some 10 µA), the field makes them and the other two a hundredth as many (on optimized builds: lit, its solves took a sanitized build past 300 s).

## B1. `semi.Mion` where no pairs are made otherwise

Not a number where Igen is nought or under 10⁻¹² of the current - no recombination model, a forward bias. It was infinite for a resistor of 10 A and a diode without recombination, 2.75·10¹⁸ for a diode at 0.7 V. At rest 1, as before; its help says so.

## B2. Stopping while the mesh smooths and while a step sets up

- **The mesher** asks whether to stop at each pass of its smoothing and of its Delaunay edges (`fem_mesh.cpp`).
- **A step's setup:** `setupStep` gives each problem the cancel and the telling before its `setup` (they came after), and tells "Setting up". The semiconductor's setup asks between its doping, its mobilities, its contacts and its unknowns; its equilibrium at each of Newton's steps.

The anneal example on a mesh of 0.002 µm, closed during its solve: 0.2 to 0.3 s at any moment (10.7 s during the smoothing before), but 2.1 s once, inside a Delaunay pass. Its progress said the mesh's "Done" while it set up; now "Setting up Stationary".

## B3. A sweep's points no longer keep their walk's line

Each point's line is let go once the next point has taken it (`runStudy`); the last keeps its own.

## B4. A sweep's second point starts on the first's line

A walk that took no step - its voltages there already, solved again - leaves the line it was handed. The first point's re-solve handed on a line of nought. The walk test: 223, 30, 15, 10 and none of Newton's steps (96 for the second point before); on a sanitized build's meshes of 0.09 to 0.11 µm 212 to 215, 25, 14 or 15, 8 to 10 and none.

## B5 and N8. The settings' lists

As the File Types tab's: the Locations tab's two path lists span a stretched row below their buttons, the library note kept to its lines; the Contents tab's categories box takes the rows' room. Between a dialog 700 and 1,100 high:

| list | grows by | before |
|---|---|---|
| Locations, Subcircuit Search Paths | 246 pixels | 74 |
| Locations, Library Search Paths | 154 | 0 |
| Contents, its categories | 275 | 82 |

The Settings and Appearance tabs keep their rows together at the top.

## B6. The gapped core inductor's gap

Its refinement selects the gap's boundaries by a box (it named `slot`, a tool of the difference that makes the core). L at 1 A 4.700 mH (4.687 before), its test passing. `everyExamplesSelectionsResolve` (test_fem_engine) resolves every selection of the 49 shipped examples at its own level: 316, none naming what the geometry no longer has.

## B7. The guided mode's check

The mode below the window's edge's index is not said to be unguided where the edge is the core's own material, doped or warmed (the same index or permittivity as written): the phase shifter's p⁺ and n⁺ slabs. The SOI wire in oxide is still warned of (`aChoiceItCannotMakeIsSaid`).

## B8. A sweep of the temperature

A point that finds no solution from the last point's starts again from rest, as a solve alone does; said ("from the last solution no solution found: solved from rest"), and where neither does, the error says both. A diode at 0.7 V swept from 300 to 400 K: at 400 K -1.0287 A, the single solve's.

## B9 and N12. Values not finite

A list's value of NaN or infinity - a sweep's, a transient's output times, a frequency's - is refused when the list is read: "0/0 is not a finite number".

## F1 and F2. The Lumerical import's whole numbers

One checked conversion, `lumerical::whole`, at every cast of a script's or project's number: twelve sites, and two more found (an FDE's trial modes, its wavelength points). The counts are bounded:
- a contact's range points to 10,000, as a sweep (its range start alone, said);
- trial modes to 1,000 (4 taken, said);
- wavelength points to 10,000 (the wavelength alone, said).

## C1, C2 and C3. CI

- **C1:** on macOS, the five tests over 1 GB (the laser, resonator, modulator, optoelectronics and the quantum test's first part) take a `RESOURCE_LOCK`: one at a time on the runner of 7 GB.
- **C2:** the walk test's bounds - the second point under 40 (sanitized) or 45 of Newton's steps, the last three under 60 - with room on every mesh tried.
- **C3:** the Linux job's limit 330 minutes, below the hosted runners' 360.

## Notes

- **N1:** a parameter error at a later sweep point now stops the sweep there, its points kept. Since N12 nothing reaches it (the one value-dependent parameter error was a NaN written back as `nan`): defensive.
- **N2:** `semi.In` and `semi.Ip`'s help, and the driving field's: α of |E| whatever the driving, their breakdown the field's.
- **N3:** a sweep stopped before, solved again with nothing changed, goes on from its next point, its points kept ("on from point 9 of 102, its 8 points before kept as they were").
- **N4:** a sweep's point named in its own unit where SI says it only with an exponent, or nought ("Nd = 1e+16 1/cm^3", "L = 0 mm"); one point kept is "its point before kept".
- **N5:** a whole number past an int's range and its schema's bounds is refused, for every tool ("timeout is at most 7200, not 10000000000").
- **N6:** the import's note names a driving field it does not know as such ("2, not one Qucs-S knows").
- **N7: not changed.** Closing during a solve waits for the step under way: a Newton's step, or a frequency step's factorization, which cannot stop midway. On the sanitized build that is seconds; on Release a tenth of one.
- **N9:** the general form's Γ is checked against u's unit over a length, not a fixed one: the annulus not warned, a flux in 1/m² still is.
- **N10, N11:** `si_inversion_valleys` asks for 10 states; `strained_laser_gain` takes 10 states, the hyperbolic secant line the help advises for gain, the bound continuum, and energies from 800 nm, past the barriers' gap and its fade. Neither warns now; their tests say so. The gain peaks stay at 986 nm.
- **T1:** `fuzz_tools.py` gives a call its own timeout and 30 s before calling it a hang.
- **T2:** `fuzz_qfem.py` keeps any call over 10 s beside its findings; the Lumerical script fuzzer's keys reach F1's casts.

## Broken on purpose

Twenty changes undone one at a time, the test rebuilt and run (a script restoring each file, its hash checked at the end). All caught:

| undone | caught by |
|---|---|
| A2: the weighed fit | `theFieldsPairsAreItsCurrents` (no solution at -40 V) |
| A2: the rounding's cut (J², s) | `ionizationsDrivingField`: the field's pairs in the dark contact layer 2.3·10⁻³ of all |
| B1: Mion infinite again | `aMultiplicationWithoutOtherPairs` |
| B2: the smoothing's asking | `aMeshStopsWhileItSmooths` (test_fem_engine) |
| B2: the setup's telling | `aSolveStopsWhileItSetsUp` |
| B3: the lines kept | `aWalkStartsOnTheLine` |
| B4: a walk of no step handing on its line | `aWalkStartsOnTheLine`: the second point's first step 100 mV |
| B5: the dialog's layout as it was | `theSettingsListsTakeTheRoom`: the categories 110 then 192 pixels |
| N8: the rows' stretch | `theSettingsListsTakeTheRoom`: the last row 564 then 944 |
| B7: the edge's material | `theExamples` (test_fem_semiconductor): "not guided" |
| B8: from rest | `aTemperatureSweep`: stopped at 400 K |
| B9, N12: values not finite | `aSweepsNamesAndValues` |
| N3: the resume | `aStoppedSweepGoesOn` |
| N4: the labels | `aSweepsNamesAndValues` |
| N5: the bounds | `whatTheHuntAfterRoundNineFound` (test_qucs_control) |
| N9: the fixed unit | `theExamples`, `aPdeNothingSets` (test_fem_pde) |
| N10: 6 states | `exampleInversionInValleys` |
| N11: the example as it was | `theExamples` (test_fem_quantum) |
| F2: the cap | `hostileNumbersAreTaken`: 2·10⁶ points taken |
| F1: the axis's cast | `hostileNumbersAreTaken` under the sanitizers: "1e+30 is outside the range of representable values of type 'int'" |

Three were missed at first and their tests sharpened: B4's (the second point's Newton's steps were no measure once A1 was fixed: its first step's length is), B5's (only the row's stretch undone - the spacer still gave the list its room: the whole change undone), B7's (its warning is in the solutions' logs, not the run's).

Not broken: the acceptance of equations met to 10⁻¹⁴ (A1), by hand - without it the avalanche diode at -40 V on 0.01 µm finds no solution, as above (the suite's meshes reach no such floor); N1 (unreachable since N12); N2 and N6 (text); C1 and C3 (CI's settings); T1 and T2 (the fuzzers).

## Runs

The first Release suite after the fixes: 127 of 130. The three failures came from the fixes and were resolved:
- **`test_fem_modulator semiconductorIn1D`:** a 1D sweep's second point now reaches 0.5 V from 0.3 V's in one step (B4), which says nothing; the test read "none said" as -1 and now counts it as one step (3 from equilibrium).
- **`test_multiphysics_doc claudeStopsASolve`:** it looked for "its 1 points before kept" (N4).
- **`test_qucs_control simulateSaysWhatIsWrong`:** -2147483648, an int's least, was taken for past an int's range (N5); the check now excludes it, so the tool's own message comes back.

- **The Release suite:** 130 of 130.
- **Under the sanitizers, with Eigen's LU (as CI's Linux), no report** - each changed or new function, its time:
  - test_fem_devices: theFieldsPairsAreItsCurrents 101 s, aSweepReachesBreakdownsEdge 48, aMultiplicationWithoutOtherPairs 62, aTemperatureSweep 13, aStoppedSweepGoesOn 9, aSweepsNamesAndValues 7, aSolveStopsWhileItSetsUp 0, aWalkStartsOnTheLine 66, ionizationsDrivingField 296 (dark alone: lit, it passed 300 s), anAvalancheGain 157, exampleInversionInValleys 78, theMostOfTheFieldLines 60;
  - test_fem_engine everyExamplesSelectionsResolve 2, aMeshStopsWhileItSmooths 1; test_fem_lumerical hostileNumbersAreTaken 7, aMqwScript 6; test_fem_pde theExamples 94, aPdeNothingSets 0; test_fem_quantum theExamples 120; test_fem_semiconductor theExamples 60; test_fem_magnetics theExamples 94; test_fem_modulator semiconductorIn1D 61; test_content_categories theSettingsListsTakeTheRoom 4, theFileTypesTableTakesTheTabsHeight 2; test_qucs_control whatTheHuntAfterRoundNineFound 5, simulateSaysWhatIsWrong 1; test_multiphysics_doc claudeStopsASolve 47.
  - ionizationsDrivingField's dark solves take the field driving 69 of Newton's steps, 95 before A2: its time is Eigen's LU's under the sanitizers, not the fix's.
- **The lints pass:** tool arguments, JSON nesting, third party.
