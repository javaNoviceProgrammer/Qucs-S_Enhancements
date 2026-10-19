# Bug hunt: the APD round, its CI failures and the semiconductor near breakdown

*19 October 2026, Qucs-S 27.0.2, `91450f4` (the Release build and the ASan/UBSan build of it). 05:51 to 06:53 (three minutes of it paused, below), the findings written up as they came. Before the hunt, 04:50 to 05:50, CI's red runs on `91450f4` were read and their causes found; those findings (C1, C2, A1) are written up here too. Paused some three minutes at 06:22 for the user's report on the settings' File Types tab (its table stuck small at the bottom), fixed at their word: `qucssettingsdialog.cpp`, the table spanning a stretched row below the buttons, its note kept to its lines; `test_content_categories theFileTypesTableTakesTheTabsHeight` (before: in a tab 547 high the note from 9 to 417, the table from 424 to 537). B5 is that fault elsewhere. All the findings are fixed, the notes too but N7, in the commit that adds docs/feature_gaps/2026-10-19-claude-apd-hunt-fixes.md.*

This hunt covers what changed since the last one (`287bf23`):
- **The vertical photodetector round (`82669a9`):** the surface recombination's "majority" switch, the CHARGE export and import of it, the importer's damaged-input checks.
- **The APD round (`3afc7bf`):** the walk to the contacts' voltages (its line, its steps, a sweep's next point on its last step's line, "met" without a step), the progress told (the sweep's point, the walk, Newton's step), a stopped sweep's points kept, the ionization integrals from every corner, the driving fields (field, fermi, current), `semi.Iion`, `semi.Igen`, `semi.Mion`, `semi.Gion`, `semi.xion`, `semi.yion`, the Lumerical import's driving field, the quantum test's split.
- **CI on `91450f4`:** macOS and Linux red on main and on the tag v27.0.2 (Windows green; the release itself built and published).

It ran in the foreground on macOS arm64, each app run with its own HOME, settings and cache in a scratch folder. Methods:

1. **Reading CI's logs** and the code they point at; the runners' images, the tests' times and their order, compared with the last green macOS run (`28dab4c`).
2. **Probes**, throwaway test functions in `test_fem_devices` and `test_fem_lumerical` (kept beside this report as patches, the test files restored), on Release and, for the importer, on the ASan build. A Newton debug patch (also kept) for A1.
3. **The tools through `qucs-s --mcp-server`:** `fem_solve`'s timeout, `fem_describe`'s "at work" and "stopped", `fem_stop`, closing a model mid-solve.
4. **The previous commit built** (`28dab4c`, a scratch worktree) to tell what is new.
5. **Fuzzers on the ASan build:**
   - the 16 October hunt's `fuzz_qfem.py`, seed 1910 (40 mutants of the shipped examples, twice: the second keeping any call over 10 s) and seed 1912 (40 mutants of three avalanche diodes, one per driving field, their pairs in the equations);
   - `fuzz_tools.py`, seed 1911, 600 calls;
   - `test_fem_lumerical fuzz`, its keys extended by those F1 reads, seed 19, 120 s.
6. **A memory survey:** each multiphysics test's peak footprint (`/usr/bin/time -l`).
7. **Every shipped multiphysics example** opened and solved through the tools, its warnings read.
8. **The settings dialog's tabs** (after the user's report): each grabbed and its lists measured at two heights, offscreen (`test_content_categories` probes).

## Summary

| | Finding | Since |
|---|---|---|
| **A1** | Near an avalanche breakdown, Newton's method cycles between two states at voltages below breakdown: the walk cannot reach them, a sweep stops ("no solution found ... steps down to 10 µV"). An ordinary sweep of the test's avalanche diode stops at -50 V (breakdown 51.6 V) on a mesh of hmax 0.04; the shipped `avalanche_breakdown.qfem`, its pairs on, stops at 51 V (In 0.986, Ip 0.942) | before (`28dab4c` too) |
| **A2** | `semi.Iion` and `semi.Gion` under the field driving (the default, and the Lumerical import's choice) count the hole current's rounding in the p⁺ side of the junction: 99 times the pairs it adds without recombination; with it, about 1 % high | `3afc7bf` (the noise in the equations before) |
| **B1** | `semi.Mion` is infinite wherever no pairs are made otherwise - a resistor carrying 10 A, a diode without recombination at -1 V - and huge where they are a rounding: 2.75·10¹⁸ for a forward-biased diode at 0.7 V | `3afc7bf` |
| **B2** | Closing a model (or stopping its solve) while its mesh is smoothed waits for the smoothing: 10.7 s on Release, 45 to 67 s on the ASan build | before |
| **B3** | (reading) Every kept sweep solution holds its walk's last line, a vector of the unknowns: 43 % more memory a point, never used again | `3afc7bf` |
| **B4** | A sweep's second point starts as from nothing: the first point's walk solved again at its end hands on a line of length nought | `3afc7bf` |
| **B5** | The settings' Locations tab: the library path list stays 90 pixels high however tall the dialog, its note taking the room (the File Types tab's fault, fixed during the hunt); the subcircuit list takes a fifth of the room | before |
| **B6** | The shipped gapped core inductor's mesh refinement of its gap selects nothing ("Size: there is no object slot"): the gap meshed coarse, its peak field 6 to 13 % off | before |
| **B7** | The shipped phase shifter warns its mode is "not guided" (neff 2.567 in oxide): the check's rib exemption fails where the slab at the window's edge is doped | before |
| **B8** | A sweep of the temperature stops where each point solves alone: a forward-biased diode from 300 to 400 K, "no solution found ... steps down to 10 µV" (a sweep of the light or the doping solves) | before (`28dab4c` too) |
| **B9** | A transient's output time worth NaN ("0/0") is taken: a million steps, 68 s, then "stopped at t = NaN s"; the times sorted with a NaN among them | before |
| **F1** | Undefined behaviour in the Lumerical import: twelve numbers of a script or project cast to `int` unchecked; UBSan reports the two tried, each from a script of a few lines | before; one (the impact driving field) `3afc7bf` |
| **F2** | A CHARGE contact's "range num points" is taken as it is: 10⁶ makes a 14 MB model; 10³⁰ (cast to INT_MAX) hangs the import past 300 s, on its way to some 28 GB | before |
| **C1** | CI's macOS job: the VCSEL tests past their 300 s - the laser, resonator and modulator tests (2.4, 2.0 and 1.2 GB) at once on a runner of 7 GB, since the quantum test's split moved the tests' order | `3afc7bf` |
| **C2** | CI's Linux job: `aWalkStartsOnTheLine` over its bound (69 + 86 + 76 Newton's steps, 80 allowed): A1 on CI's mesh | `3afc7bf` (the test) |
| **C3** | CI's Linux job takes 222.6 of its 240 minutes | creeping |
| N1 | A parameter error at a later sweep point still loses the points before (only a failed solve or mesh keeps them) | `3afc7bf` |
| N2 | `semi.In` and `semi.Ip` are the field's integrals whatever the driving field: their "breakdown at 1" is the field driving's | `3afc7bf` |
| N3 | A stopped sweep solved again starts from its first point | `3afc7bf` |
| N4 | Labels: "L = 0 m" for nought in a sweep in mm, 1/m³ for one in 1/cm³; "its 1 points before kept" | before; `3afc7bf` |
| N5 | `fem_solve`'s `timeout` read as a whole number: 0.5 or 1e10 is 600 s, silently | before |
| N6 | The Lumerical import's note calls any driving field but 1 "E dot J": "(the Lumerical driving field 2, E dot J)" | `3afc7bf` |
| N7 | Closing mid-solve on the ASan build waits for the step under way (a Newton's step, a frequency's LU): 12 s where Release takes 0.2 | before |
| N8 | The settings' Settings and Appearance tabs spread their rows over a tall dialog's height | before |
| N9 | The shipped `annulus_laplace.qfem` warns "Conservative flux Γ, x of Δu = 0 is in 1/m, where 1 is expected": the general form's Γ checked against a fixed unit; its test does not read the log (the point source's does) | before |
| N10 | The shipped `si_inversion_valleys.qfem` warns "its highest e state found holds 0.1 % of its carriers: ask for more states" | before |
| N11 | The shipped `strained_laser_gain.qfem` warns its Kramers-Kronig window's top "still absorbs 100 % of its most" | before |
| N12 | A sweep's value worth NaN or infinity ("-1, 0/0, -2") is listed, then its point fails as "Va = NaN V: the parameters: Va: 'nan' is not a parameter ...", the points before lost (N1) | before |
| T1 | `fuzz_tools.py` calls a `wait_for` of 110 s (allowed: 1 to 3600) a hang: its own limit is 90 s | the fuzzer |
| T2 | `fuzz_qfem.py` keeps no slow call (B2 found by a patched copy); the Lumerical script fuzzer's keys never reach F1's casts | the fuzzers |

No crash or hang. One kind of sanitizer report, F1's. A2, B1, B3, B4 and C1's cause are the APD round's; A1 and B8, the most serious for a user, were there before it.

## A1. Newton's two-cycle near an avalanche breakdown

**The test's diode.** `aWalkStartsOnTheLine` uses a one-sided silicon diode (p⁺ 10¹⁹ cm⁻³, n 10¹⁶ cm⁻³ over 4 µm) with impact ionization and SRH (1 µs). Its breakdown is 51.6 V. On the sanitized build's mesh (hmax 0.1 µm) one meshed a little differently, hmax 0.098 (`QUCS_TEST_SLOW=1`, the probe's `WALK_HMAX`), gives:
- the sweep -40, -45, -48, -50, -51 V stops at -51 V: "no solution found on the way to the contacts' voltages (steps down to 10 µV)";
- from -50.6 V, -50.7 and -50.8 V cannot be reached, while -50.79, -50.81 and -50.85 V can. In is 0.975 at -50.6 V.

The walk closes on -50.8 V by halves. Each half step succeeds in 5 to 11 of Newton's steps. Each step to -50.8 V itself takes its 40 and fails, down to an 11 µV step.

**The failing solve** (the debug patch). Newton's method has met the equations: their imbalance is 4.4·10⁻⁹ of the flows, and the potential moves 1.1·10⁻¹¹ Vt a step. But one carrier changes by 2.65 % and 2.72 % on alternate steps for all 40. The convergence test (`fem_semiconductor.cpp`, the end of `newton`) asks for the carriers still to 10⁻⁶, or the potential to 10⁻¹² Vt. Neither holds, so the solve fails.

The carrier is the electrons at the junction's corner on the p⁺ side, node 2 at (0, 0). There n alternates between 5.218·10⁸ and 5.36·10⁸ m⁻³, against p = 2.9·10²⁴. Its triangles show why:
- triangle 33 (the junction, αp 8.6·10⁵ /m, |E| 3.9·10⁷ V/m) has a hole current alternating between (-0.00832, 0.00063) and (-0.00664, -0.00236) A/m²;
- the true current density there is some 5·10⁻⁷ A/m².

On the p⁺ side the hole current is the difference of drift and diffusion fluxes of some 10¹² A/m² each, so its rounding is ten thousand times the current. The field-driven generation αp|Jp| takes that rounding into the electrons' equation at the corner, where electrons are 10⁻¹⁶ of the holes. Whether a voltage falls on such a cycle depends on the mesh and the voltage.

**On ordinary meshes.** A sweep of -49, -50, -50.1 … -51.4 V:
- at hmax 0.05 µm (the walk test's Release mesh) reaches every point (-50 V "2 tried again", -51.4 V "1");
- at hmax 0.04 µm stops at -50 V, 1.6 V below breakdown (In 0.96 there).

**The shipped example.** `avalanche_breakdown.qfem` solves its ionization integrals alone (`currents` off). With its pairs put in the equations, as its help offers ("avalanche multiplication ... up to breakdown"), its sweep of 10 … 54 V stops at 51 V: "no solution found on the way to the contacts' voltages (steps down to 10 µV) ... its 7 points before kept" (`probe_aval.py`, Release). Its own integrals at 51 V are In 0.986 and Ip 0.942; they pass 1 only at 52 V. So it stops below its breakdown, as the test's diode does.

The build before the APD round (`28dab4c`, `old28dab4c_probe.patch`) stops at -50 V on hmax 0.05 (then the whole sweep an error, nothing kept) and at -50.6 V on hmax 0.04. So the walk of `3afc7bf` moved where it fails but did not cause it.

**Proposed fix.** At the source: the |J| that drives the field's generation should not be the noise of a majority current. Either:
- take the triangle's |J| as `current` does, against J0 (a millionth of q n µ |E|); A2 shows it is clean then;
- or weigh each edge's current by its own terms' size.

At the check: a carrier whose change is below its equation's own rounding (its row's gross flows over its diagonal, times some 10⁻¹²) should count as converged. Test: the hmax 0.098 sweep to -50.8 V, and hmax 0.04 to -51.4 V.

## A2. `semi.Iion` and `semi.Gion` count a majority current's rounding

The same diode at -40 V, its pairs in the equations or not (`currents`), on hmax 0.05:

| | I, pairs out | I, pairs in | the rise | `semi.Iion` |
|---|---|---|---|---|
| no recombination, field | 9.88·10⁻¹⁴ A | 1.309·10⁻¹³ | 3.20·10⁻¹⁴ | 3.18·10⁻¹² (99 times) |
| no recombination, fermi | 9.88·10⁻¹⁴ | 1.280·10⁻¹³ | 2.92·10⁻¹⁴ | 2.92·10⁻¹⁴ (1.0) |
| no recombination, current | 9.88·10⁻¹⁴ | 1.299·10⁻¹³ | 3.11·10⁻¹⁴ | 3.11·10⁻¹⁴ (1.0) |
| SRH 1 µs, field | 2.225·10⁻¹⁰ | 3.545·10⁻¹⁰ | 1.320·10⁻¹⁰ | 1.332·10⁻¹⁰ (1.008) |

The terminal current is right: without recombination it is the short diode's diffusion current, some 10⁻¹³ A, times Mp = 1.37. It is `semi.Iion` that counts what A1 shows:
- each triangle's rate is (αn|Jn| + αp|Jp|)/q from its edges' Scharfetter-Gummel currents;
- on the p⁺ side at the junction, |Jp| is the rounding of currents of 10¹² A/m².

At -20 V without recombination it is 190 times (1.26·10⁻¹³ A against 6.7·10⁻¹⁶). `semi.Gion` is the same rate, so a plot of it shows a line along the junction's p⁺ edge some 10⁴ times the true rate there. Through the tools at -3.5 V with SRH (`probe_gion.py`, hmax 0.02): `fem_plot` of `semi.Gion` ranges to 4.23·10¹⁷ /(m³ s) under the field driving, to 3.86·10¹⁶ under fermi; `semi.Iion` 261 aA against 141 aA.

Where the dark current is small, the gain read from `semi.Mion` = 1 + Iion/Igen is wrong too. The quasi-Fermi and current drivings have none of it. Fix as A1's.

## B1. `semi.Mion` infinite where nothing else makes pairs

`setGlobals` writes `Mion = still ? 1 : Igen > 0 ? 1 + Iion/Igen : Inf`. Without recombination or light, Igen is 0 whenever a current flows:
- a uniform n bar (10¹⁶ cm⁻³) with impact ionization at 1 V carries 9.97 A, with `semi.Iion` 8.3·10⁻²³⁸ A, and reads `semi.Mion` = inf;
- the diode without recombination reads inf at -1 V (Iion 1.6·10⁻²² A) and at -40 V;
- the shipped `avalanche_breakdown.qfem`, its pairs in the equations, reads inf at every point from 10 V.

The help says "rising without bound toward breakdown". Proposed: Mion where Igen is nought should be 1 when Iion is nought too (or below the currents' rounding), else said ("no pairs made otherwise: no multiplication to tell"). A forward-biased diode, whose net recombination is positive everywhere, is the same case, worse: Igen is then a rounding, not nought. The avalanche diode with SRH (`probe_fwd.py`) reads Mion 2.2·10¹³ at 0.3 V and 2.75·10¹⁸ at 0.7 V (22.7 mA), Igen 1.5·10⁻³⁴ A at both.

## B2. Closing during the mesh's smoothing waits for it

`boron_implant_anneal.qfem` on hmax 0.002 µm (Release, through the tools):
- `fem_solve` with a timeout of 1 s, then `close_document`: "21 % - Triangles", closed in 0.1 s;
- a timeout of 3 s: "27 % - Smoothing", the close took **10.7 s**;
- on hmax 0.003: "27 % - Smoothing", 3.5 s.

On the ASan build the example as shipped took 67.1 s to close (its progress "30 % - Done"). The model fuzzer's 45.4 s close (a mutant differing only in a plot's label) is this, and so is its first run's 47.2 s; its 12 s close of a `mos_capacitor` mutant is N7's (0.2 s on Release).

`fem_stop` waits the same way (`probe_stopmesh.py`): "27 % - Smoothing", stopped after **10.5 s**. The 16 October hunt's F5 made the mode searches stop on a cancel; the mesher's smoothing does not look. Proposed: the smoothing passes look for the cancel between passes.

## B3. Every kept sweep solution keeps its walk's last line

(Reading.) `stationary()` ends with `f.walkedFrom = std::make_shared<...>(before ...)`: the solution before the walk's last step, a vector of the unknowns (three per node), for the next point's first step. Each point makes its own. The kept `Solution` holds it after the next point has used it, beside its values (seven per node): 43 % more a point. At 311,665 unknowns that is 2.5 MB a point, 250 MB for a 100-point sweep. Proposed: release it once the next point's field has taken it, or keep it out of the stored field.

## B4. A sweep's second point starts as from nothing

The first point of a sweep is walked to its voltages by the step's setup, then solved again by the step at the same voltages. That solve finds its equations met (no step: `met`) and, as a success, sets `before = x` and `beforeAt = at`. So it hands on `walkedStep = apart(at, at) = 0`. The second point then starts with the default 0.1 V step and no prediction (`last` nought).

Through the tools (`probe_stop.py`):
- -1 V took 4 steps, -1.5 V 3 steps (13 of Newton's), -2 V and every later 0.5 V point one;
- what was told: "a at -1.1 V of -1.5 V (step 1, 100 mV)" at point 2, "a at -2 V of -2 V (step 1, 500 mV)" at point 3.

The probe `huntHandoffWithFrequency` gives -1, -3, -5 … -11 V: point 2 in 5 steps (24 of Newton's) from a 100 mV step, each later 2 V in 2. A frequency step after the stationary changes nothing. Proposed: a walk that took no step leaves the line it was handed (or the setup's) as it is.

## B5. The Locations tab's path lists do not take the room

Found after the user's report on the File Types tab (fixed during the hunt, as said at the top): the same fault on the Locations tab. Each tab's item views measured at a dialog 700 and 1,100 high (`huntViewsGrow`; the tab 608 and 1,008):

| tab, view | at 700 | at 1,100 |
|---|---|---|
| File Types, its table (fixed) | 539 | 939 |
| Locations, Library Search Path List | 90 | **90** |
| Locations, Subcircuit Search Path List | 112 | 186 |
| Contents, its categories | 110 | 192 |

- In the Library Search Paths group (`qucssettingsdialog.cpp` 845-866), the list spans the three button rows, and the word-wrapped note in row 3 takes the room: File Types' fault exactly.
- In the Subcircuit Search Paths group (818-841), the room goes between its buttons.
- The Contents tab gives its categories table a fifth of the room, the rest left empty below Scratch.

Proposed: as File Types, a stretched row below the buttons that the list spans, its note kept to its lines. At 900 × 1,000 the Settings and Appearance tabs spread their rows over the whole height, wide gaps between them (a note, N8).

## B6. The gapped core inductor's gap not refined

Every shipped multiphysics example was opened and solved through the tools on Release (`probe_allex.py`: 49, each 30 s at most, its export files left out). All 49 solve; five warn.

`gapped_core_inductor.qfem` warns "Size: there is no object slot":
- its mesh refines "the boundaries of slot" to 0.25 mm - the 1 mm air gap, the region its own text says rules its inductance at small currents ("At small currents the gap rules");
- but `slot` is a tool of the difference that makes `core`, so it is consumed, and the size feature selects nothing.

With the refinement on the gap's edges by a box instead (`probe_ind.py`, its sweep 1 and 25 A):

| | triangles | L at 1 A | L at 25 A | `mf.normB` at 1 A | at 25 A |
|---|---|---|---|---|---|
| as shipped | 8,239 | 4.6873 mH | 2.3106 mH | 0.835 T | 2.274 T |
| the gap refined | 10,076 | 4.7004 mH | 2.3108 mH | 0.783 T | 2.562 T |

The inductance moves 0.3 %, its peak field 6 to 13 %. Since the example's first commit (`a940f9c`), or a later change making a difference's tools no objects. Proposed: select the gap's boundaries (a box, or the core's edges there), and let a selection that names a consumed tool say so at the example's test.

## B7. The phase shifter example warned "not guided"

`pn_phase_shifter.qfem` warns at each point: "the mode chosen is below its cladding's index (at its window's edge) - not guided: no guided mode in its set; search about a higher index". Its mode is a silicon rib's in oxide, neff 2.56664 to 2.56679 (`test_fem_semiconductor theExamples`): guided.

The check (`fem_waves.cpp` 1570-1580) is skipped where the window's edge is as high as anything (a slab or rib reaching it), compared within 10⁻⁹. Here the slab at the edge is the contact's: `pplus` and `nplus` (`Ncontact`) run to x = ±3 µm, the window's edges, and their free carriers lower its index a little below the rib's (inferred from the warning; the indices not printed). So the check runs and compares the rib's mode with silicon's index. `theExamples` does not read the log. Proposed: compare with the edge's guided slab, or skip where the edge's index is above the mode's by more than the core-cladding contrast; let the example tests fail on any "Warning:".

## B8. A temperature sweep stops where each point solves alone

The test's diode with SRH (1 µs) at 0.7 V forward, its temperature swept (`huntTemperatureSweep`, `test_fem_devices_probes2.patch`):

| sweep | at 0.7 V | the last point alone |
|---|---|---|
| 300, 400 K | stopped at 400 K: "no solution found on the way to the contacts' voltages (steps down to 10 µV)" | -1.0205 A |
| 300, 600 K | stopped at 600 K, the same | -10.534 A |
| 300, 200 K | -9.9457·10⁻⁷ A at 200 K | the same |
| 300, 100 K | -2.1074·10⁻¹⁵ A at 100 K | the same |

At -5 V every one of them is the single solve's. Other parameters do not show it (`probe_light.py`, `probe_dop.py`, through the tools): the same diode at 0 V lit from nothing to 10³⁰ /(m³ s), each point the single solve's (to 79.53 mA); at 0.7 V its n side's doping 10¹⁶, 10¹⁷, 10¹⁸ cm⁻³, the same.

`stationary()` starts each point from the last point's solution (warm). Its voltages are already there (`left` nought), so the one try is Newton's method from the old temperature's state. When that fails, the halving makes `step = min(step, frac·left)/2` nought, under 10 µV at once: the point fails after one try, and the message speaks of steps in voltage that were never taken. Nothing tries the point from rest, as the single solve does, or walks the temperature.

The build before the APD round (`28dab4c`) fails the same two, and there the whole sweep was an error, its first point lost.

Proposed: a warm start that fails with no voltage to walk is solved again from equilibrium (the cold walk), and the message says which.

## B9. A transient's output time of 0/0 taken

The diode's transient with `"times": "0, 1e-9, 0/0"` (`huntNanTimes`, `test_fem_devices_probes5.patch`, Release): 68 s, then "Time Dependent: more than a million steps; stopped at t = NaN s". `"0/0, 1e-9"` the same. `nan` and `inf` typed are refused as unknown names, but an expression worth NaN passes `valueList`. Then `std::sort` orders the times with a NaN among them (undefined: no strict weak order), the range is NaN, and the stepper runs its million steps. On the ASan build that is some half an hour. Since before (`fem_solver.cpp` 3440-3456). Proposed: refuse a time that is not finite, said. A sweep's values are not checked either (N12).

## F1. The Lumerical import's casts to `int` unchecked

A seven-line script - `addpoly`, `set("vertices", [0, 0; 1e-6, 0; 0, 1e-6])`, `set("first axis", 1e30)`, `set("rotation 1", 30)`, an FDE region - imported on the ASan build:

    fem_lumerical_import.cpp:132:24: runtime error: 1e+30 is outside the range of representable values of type 'int'

A rectangle with `override mesh order from material database` 1 and `mesh order` 1e30: the same at line 603. Every such cast of a number read from a script or project, by reading:
- `fem_lumerical.cpp`: 727 (a valley's "active"), 1077 ("sweep type");
- `fem_lumerical_import.cpp`: 132 (rotation axis), 603 and 1161 and 2006 (mesh order), 1036 (the impact driving field, the APD round's), 1073 and 1940 (the solver's dimension), 1425 (source face), 1590 (range num points, F2), 2054 (surface type).

The 16 October hunt's F4 checked the script reader's own casts, not these. Proposed: one checked conversion (`toIntChecked(v, lo, hi, fallback)`), as the reader's.

## F2. A CHARGE contact's "range num points" taken as it is

The probe exports the graded diode of `aGradedDiodeRoundTrip` and sets its anode's sweep to a range:

| range num points | import | the model |
|---|---|---|
| 10⁴ | 16 ms | 143 kB |
| 10⁵ | 30 ms | 1.4 MB |
| 10⁶ | 170 ms | 14 MB |

Linear, so 2·10⁹ (an int still) would take some 340 s and 28 GB: not run. The model it makes is refused when solved - a sweep has 10,000 combinations at most (`huntHugeSweep`, `test_fem_devices_probes4.patch`: "Parametric Sweep: 1000001 combinations; 10000 at most", in 3 ms) - but the import has spent the time and memory first. With 10³⁰ (on the ASan build, the probe's values changed): UBSan reports the cast at line 1590 (F1), which on arm64 gives INT_MAX, and the import was still building its 2.1·10⁹ values when QtTest's 300 s ran out - a hang from one line of a script. Proposed: the import refuses past 10,000 points, as the sweep does, said.

## C1. CI's macOS job: memory, not time

Both runs of `91450f4` failed on macOS. `theVcselExamplesOtherOrders` (in `test_fem_resonator`) and `theVcselLaserExample` (in `test_fem_laser`) passed Qt's 300 s for one function. Here they take 6 and 15 s run together.

The runner image is the same as on `28dab4c`, green (`macos-15-arm64` 20260907.0337.1, three processors, 7 GB). What changed is the order:
- the new `test_fem_quantum_2` entry moved each later test's number;
- so the resonator (03:02-03:08), laser (03:03-03:08) and modulator (03:01-03:09) tests ran at once;
- at `28dab4c` the laser test started the second after the resonator test ended.

Peak footprints here (Release):

| test | peak |
|---|---|
| `test_fem_laser` | 2.38 GB |
| `test_fem_resonator` | 2.02 GB |
| `test_fem_quantum` part 1 | 1.28 GB |
| `test_fem_modulator` | 1.18 GB |
| `test_fem_optoelectronics` | 1.12 GB |
| `test_fem_semiconductor` | 0.66 GB |
| the rest | under 0.6 GB |

The three together are 5.6 GB beside the system's. Every test of the run was slower: the modulator 218 to 477 and 501 s, optoelectronics 200 to 305 and 386 s. Proposed: a ctest `RESOURCE_LOCK` (or resource groups of memory) for the tests over 1 GB, so no two run at once.

## C2. CI's Linux job: the walk test's bound

`aWalkStartsOnTheLine` on CI's sanitized mesh: 227, 101, 69, 86, 76 of Newton's steps, against 228, 48, 20, 21 and none here. Its last three must stay under 80.

Each point after the first is some 40 steps over: one try of 40 failing, then halved. That is A1 on CI's mesh, whose breakdown sits nearer (`aJunctionsBreakdown` there 51.58 V, here 51.63 V). The test's bound holds only on meshes that miss A1. Fixing A1 is the fix; else the test should not sweep within a volt of breakdown on the sanitized mesh.

## C3. CI's Linux job near its limit

The job ran 222.6 minutes of its 240 (02:32:21 to 06:14:58), and 221.9 at `28dab4c`. Its longest entries:

| entry | time |
|---|---|
| `test_fem_devices` | 4,676 s |
| `test_fem_semiconductor` | 4,264 s |
| `test_fem_waves` | 3,289 s |
| `test_fem_quantum_2` | 2,896 s |

The next round's tests will likely pass 240 minutes. Proposed: a longer limit, or the long entries split or coarsened as `QUCS_TEST_PART` did.

## Notes

- **N1.** In `runStudy`'s sequential and batched loops, a point whose parameters do not evaluate returns `run.error`: the points before are lost. Only a failed solve or mesh goes through `stopAt`.
- **N2.** `semi.In`/`Ip` follow the field's lines with α(|E|) whatever `driving` is: at -40 V the diode's In is 0.68093 under field, fermi and current alike. Here the three multiply alike (3.20, 2.92 and 3.11·10⁻¹⁴ A of pairs, A2's table), but where they differ - the Lumerical APD of the round, whose |E| equations ran away at -9.5 V with In 0.918 - In's "breakdown where it reaches 1" is the field driving's only. The help could say so.
- **N3.** `fem_stop` at point 8 of 102 kept 7 points; `fem_solve` again solved from point 1.
- **N4.** A sweep `"L", "10, 15, 0, ..."` in mm labels its third point "L = 0 m"; one in 1/cm³ labels its points in 1/m³ ("Nd = 1e+22 1/m³"); one point kept is "its 1 points before kept".
- **N5.** `fem_solve` reads `timeout` by `toInt(600)`: not a whole number, or past an int, is 600 s (the 1 October hunt's C1, made whole unchecked, for this tool).
- **N6.** The import's note names the driving field by code: "(the Lumerical driving field %2, E dot J)" for any code but 1 (`fem_lumerical_import.cpp:1044`).
- **N7.** Closing a model mid-solve on the ASan build can wait some seconds where Release takes 0.1 to 0.2 s (the `mos_capacitor` mutant 12 s): the cancel is looked for between Newton's steps.
- **N12.** `huntNanSweep` (`test_fem_devices_probes6.patch`): the sweep "-1, 0/0, -2" V solves -1 V, then fails at "Va = NaN V" - its value written back as the text `nan`, which the parameters do not read - and the study is an error; "-1, 1/0" the same with `inf`. Refused when the list is read, said, it would cost nothing.
- **T1.** `fuzz_tools.py` seed 1911, call 157: "HANG wait_for", its arguments `{"event": "document_changed", "timeout": 110}` - a wait the tool allows (1 to 3600 s), past the fuzzer's 90 s.
- **T2.** `fuzz_qfem.py` records only crashes, hangs and sanitizer reports, so B2 needed a copy that keeps calls over 10 s (`fuzz_qfem_slow_and_seeds.patch`). The Lumerical script fuzzer's `set` keys never include those of F1.

## Checked and found right

- **The "met" shortcut on a sweep of the light** (`huntWeakLightSweep`): G0 0, 10¹⁴ … 10²² /(m³ s) at -1 V. Each point's current is the single solve's at that G0 to all printed digits, its photocurrent collected 18 % alike.
- **The driving field's choices** (`huntDrivingChoice`): "Fermi", "grad phi" and "1" refused, saying the three; empty is the field.
- **A stopped sweep** (`huntParallelStop`): a rectangle's length -5 or 0 mm, a conductivity 0 or -10⁶ S/m at point 3, one at a time and four at a time - stopped there, its 2 points kept, the same. A bad first point is an error, nothing kept.
- **The tools** (`probe_stop.py`, `probe_idle.py`):
  - `fem_solve` at 3 s: "Still solving after 3 s, 7 % - Va = -4.5 V (point 8 of 102): Solving Stationary - a at -4.5 V of -4.5 V (step 1, 500 mV); Newton's step 2, its residual 0.27";
  - `fem_describe`'s "at work" the same; `fem_stop` kept 7 points; `fem_evaluate` gives the 7; `fem_describe`'s "stopped";
  - `fem_stop` with nothing running says so; twice is the same;
  - `fem_solve` with a timeout of 0 or -5 waits 1 s.
- **`jobProgress`** reads the stage under the job's mutex; the timeout's message is made on the main thread before the job's end is processed.
- **The walk's handoff** is copied only from a solution on the same mesh (`start->meshPtr() == mesh`) and of the same size.
- **The ionization integrals' cost:** at hmax 0.005 µm (311,665 unknowns) the solve with them 26.7 s, without 27.3 s.
- **The integrals with nothing ionizing:** a uniform bar at rest has In 0, Mn 1 (its field's rounding 1.9·10⁻⁹ V/m: the NaN path is not reached).
- **The quantum test's split:** `QUCS_TEST_PART` unset runs every function.
- **A contact's minority carrier** for the "majority" switch is the one whose equilibrium density is lower; at an intrinsic contact the holes, consistently.
- **The MQW layer count:** a negative count reads no layers.
- **Every shipped multiphysics example solves** (`probe_allex.py`, 49 on Release, each 30 s at most): no error, no stopped sweep; `heater_crosssection` and `heatsink_import` with their layout and SVG beside them. Five warn: B6, B7, N9, N10, N11. The infinite values are `avalanche_breakdown`'s Mn and Mp past breakdown (52 to 54 V), as they should be.
- **The driving fields in time** (`huntTransientDriving`, `test_fem_devices_probes3.patch`): the diode at -45 V with SRH, its light switched on at 0, 40 ns in steps of 2 ns, under each driving field - each ends at its own lit stationary current to five digits (field 1.3562·10⁻⁷ A, fermi 1.1019·10⁻⁷, current 1.0608·10⁻⁷).
- **A model's own sweep is capped:** `range(10, 1e-5, 20)` mm (10⁶ points) refused in 3 ms, "10000 at most", 30 MB at its peak.
- **A stopped sweep plotted** (`probe_plotstop.py`): `fem_plot` of `semi.Gion` and `semi.I_k` shows its last kept point.
- **Lists elsewhere:** no other dialog puts a list or table in a grid spanning button rows (a search of every `addWidget` of one with a row span); the sweep dialog's stretch is on an empty row, as it should be.
- **The File Types fix** (during the hunt): its test, and the Contents tab's, pass on Release and under the sanitizers, no report; `test_content_categories` whole on Release, 11 of 11.
- **The CI lints** (`check-json-nesting`, `check-tool-arguments`, `check-third-party`) pass at `91450f4`.
- **Fuzzing, ASan/UBSan:**
  - `fuzz_qfem.py` seed 1910, 40 mutants of the shipped examples, twice, and seed 1913, 20 more: no crash, hang or report;
  - seed 1912, 40 mutants of the three avalanche diodes: none (their solves all past the fuzzer's 20 s on this build: open, build, describe and close fuzzed, not the solutions);
  - `fuzz_tools.py` seed 1911, 600 calls: only T1;
  - `test_fem_lumerical fuzz` with the extended keys, seed 19, 120 s: 1,083 scripts (70 built), 295 damaged projects, no report.

## Reproducing

The probes are patches against the test files at `91450f4`: `git apply docs/bug_hunts/2026-10-19-apd-round-and-ci/<file>_probes.patch`, build the test, then run the function. The tool probes (`python3 -B -I <probe> <folder of mcp_client.py> <scratch folder>`) take `mcp_client.py`, beside this report; `mk_apd.py OUT POINTS HMAX` writes the avalanche diode they use.

| Finding | Probe | How |
|---|---|---|
| A1 | `newton_cycle_debug.patch` (`aWalkStartsOnTheLine`, `WALK_HMAX`, `WALK_POINTS`, `WALK_TOLD`; `DBG_NEWTON`, `DBG_TRI` in `fem_semiconductor.cpp`) | `QUCS_TEST_SLOW=1 WALK_HMAX=0.098 WALK_POINTS="-50, -50.6, -50.7"`, `-maxwarnings 0` |
| A1 | `test_fem_devices huntNearBreakdownSweep`; at `28dab4c` `old28dab4c_probe.patch` | Release |
| A1, B1 | `probe_aval.py`, `probe_aval2.py` | `avalanche_breakdown.qfem` with `currents` true (aval_on.qfem); `probe_aval2.py` takes the file's name |
| A2 | `probe_gion.py`, `probe_plotstop.py` | the Release app |
| B1 | `probe_fwd.py` | `mk_apd.py fwd.qfem "0.3, 0.7" 0.05` |
| A2, B1 | `huntIionAgainstCurrent`, `huntIionByDriving`, `huntMionWithoutGeneration`, `huntUniformBarIntegrals` | Release |
| B2 | `probe_close2.py` | `boron_implant_anneal.qfem` with `hmax` 0.002, the Release app; the ASan app as third argument |
| B2 | `probe_stopmesh.py` | as `probe_close2.py`, `fem_stop` in place of the close |
| B4 | `probe_stop.py`; `test_fem_devices huntHandoffWithFrequency` | Release |
| B6 | `probe_ind.py` | the example and a copy refined by a box on the gap |
| B7, N9-N11 | `probe_allex.py`; `test_fem_semiconductor theExamples` | Release |
| B8 | `test_fem_devices huntTemperatureSweep` (`test_fem_devices_probes2.patch`) | Release; at `28dab4c` the same function |
| B9 | `test_fem_devices huntNanTimes` (`test_fem_devices_probes5.patch`) | Release |
| B5, N8 | `test_content_categories huntViewsGrow`, `huntEveryTabTall` (`test_content_categories_probes.patch`) | offscreen; `SHOT=<prefix>` for the pictures |
| F1 | `test_fem_lumerical huntAxisCast`, `huntMoreCasts` | the ASan build |
| F2 | `test_fem_lumerical huntRangePoints` | Release |
| C1 | `/usr/bin/time -l` on each `test_fem_*` | Release; CI runs 39190433145 and 39190433866 against 39167480344 |
| C2, C3 | CI's logs | `gh run view 39190433145 --log-failed` |
| B3, N1, N5, N6 | — | by reading |
