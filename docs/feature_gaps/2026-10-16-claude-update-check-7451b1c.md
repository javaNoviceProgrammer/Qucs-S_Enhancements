# Update check 2026-10-16 (7451b1c, fbd3bc3), with the open items of 8bd5709 and 110e8b9

The reviewer's check of 7451b1c (`2026-10-16-claude-update-check-7451b1c.md`) passed every item
of the GUI check and auto-compact, and raised two nits. It also listed what was still open from
two earlier checks:

- `2026-10-16-claude-update-check-8bd5709.md`;
- the laser L-I items of `2026-10-16-claude-update-check-110e8b9.md`, with its MOSFET re-check.

Each item was first reproduced. The reviewer's models in `~/QucsWorkspace/update_check_1016{b,c,d}`
were copied to scratch and solved by this build's app through its tools, or by the tests. Each was
then fixed, tested and broken on purpose. Asked mid-round whether images scale correctly in the
Claude chat: they do. Rendered at 340 and 760 px wide at a Retina ratio, each image fitted its
width and was drawn smoothly (SmoothImages, as in the viewer).

## 7451b1c: the nits

| Feedback | Now |
|---|---|
| A long axis label (diagram 1's `phase(rlgc:sqrt(...))`) is taller than the diagram and runs well above and below its frame | A y, right or z label longer than the diagram is high is cut in its middle, its ends kept (`db(1/(1+j*2…ac1.c1))) <invalid>`). One that fits is whole. The legend names a trace whole. |
| A blockquote in the Markdown preview is only an indent: no bar | Each quote level has a bar beside it, in the viewer and in Claude's chat: a 3 px bar in the text's colour at 70/255, through every paragraph of the quote, nested quotes a bar each. Qt's Markdown sets a quote in by its margin and nothing else. The bars are drawn after the view paints itself; drawn first, the document's background covered them. |

## 110e8b9: the laser L-I items

| # | Feedback | Now |
|---|---|---|
| 1 | `keep` overwrites the last mode's solution when a step follows the Eigenfrequency step (count 4, keep 0: mode 4 read as mode 2, 53.93 THz) | The step after wrote its solution over the last snapshot, whatever mode the fields held. A solution now knows which snapshot its fields belong to: the kept mode's (`leaveAt`). A stationary step after it replaces that snapshot. Mode 4 reads as mode 4 again; the kept one is marked as chosen. |
| 2 | The example's threshold and current not mesh-converged: threshold 129 → 104 µA with the cavity's rows doubled (the reviewer's cause, found later in the check: a well node recombined over its whole box, half of each barrier element too; the budget I / (q∫R + Istim) 1.458) | Two faults, both fixed. (a) A node's box (`a_box`) is now its Voronoi share, the box the edges' weights and the doping use. It was a third of each triangle. (b) A node recombines over its parts in domains with its own bands (Eg, χ, ni), each part at its own domain's mechanisms and lifetimes. The other bands' parts are left out: a well's node no longer recombines over the barrier. The budget closes: q∫R + Istim against I is 4.61513e-7 against 4.61511e-7 A at 1.45 V, and 1.90589e-4 against 1.90587e-4 A at 1.6 V. The threshold with the cavity's rows ×1, ×2, ×4 is 90, 86 and 83 µA; it was 130, 104 and 92. The current at 1.9 V still moves with the cavity's rows (2.12 to 1.62 mA from ×1 to ×2; the wells' rows doubled change nothing). So the example now has ten cavity rows: its current at 1.9 V is within 11 % of twice as many. Its description gives its new numbers: 980.29 nm, threshold 49 µA at 1.34 V, 0.60 W/A, 0.95 mW at 1.63 mA. |
| 3 | No spontaneous emission (β 0, no Radiative, B 0): S = 0 with positive net gain, silently | Refused, saying which: "no spontaneous emission into the mode (its β is 0)" or "no Radiative recombination on its gain region". |
| 4 | A reverse-bias point anywhere in a Laser L-I list fails; a stationary laser step at 1.6 V from equilibrium fails | Below threshold, where Newton's step for S goes past 5 % of S, S is now set from its balance, S = τp β Rsp / (1 − τp G). Reverse bias converges: its current is about nought, its light none. A laser whose mode becomes ready on a state of no photons walks to its voltages again from equilibrium. Before, it started from a state walked without the light. The stationary 1.6 V step converges. Tested: `aReverseBiasAndAStepAboveThreshold`. |
| 5 | The partial-curve fallback is dead code ("did not converge" checked, "failed to converge" said) | Both are checked: the points before a failure are kept, and the stop is said. |
| 6 | GUI: plot title and colour bar stuck after the solve (vcsel_laser.qfem, pg2, pg3) | **Not reproduced.** Tried offscreen, in a hidden Cocoa window on the GPU path, and with the 1D data display in front: the title and the colour bar followed each plot. Not changed. |
| 7 | The threshold log says "below threshold at every point" when fewer than 3 points lase; NaN V when every point lases; "-58 mA" at 4 V with leakage; a sweep starting above threshold not warned | One or two points above threshold: "lasing at its last N points only - too few above threshold to draw its line: more voltages above V". Every point lasing: a warning, "it lases from its first point, V: its threshold is below it - start the voltages lower", and the threshold at "a voltage below the first point's". A light line that reaches none at a positive current: "its light's line reaches none at … - no threshold (a current that leaks past the gain region, or points above too far from it)". |
| 8 | Quantum gain in a stationary step only logged, not refused; the refusals not in the format text; gmean and Isp not in the help | Refused once the mode is solved. Before the mode is solved, it is said once. The format text lists every refusal. The help names semi.gmean, semi.Isp and semi.tauph, and the log gain's floor. |
| 9 | Silent inputs: keep −1/−3/"abc"/1.5 → 0; ng 0 → unset; gain "parabolic" as logarithmic, carrier "photons" as mean; unknown keys kept; a 1D plot's x unknown or a field → against V, x = semi.I_anode*1000 labelled "A"; voltages as a JSON list → "no values"; carriers beyond Nmax held silently | A keep below 0 is refused ("not -1"). At the study's start every feature and step is checked: a Choice must be one of its choices (one that takes a value in its place too, as Absorption per takes a length, is its step's to check) ("its Gain is "parabolic" - one of logarithmic, linear, quantum"); an Integer must be whole ("is 2.5 - a whole number", "abc" too). A key no property of its kind is warned: "it has no property gainn - not read". ng 0 is refused: given, it must be 1 or more. A 1D plot's x that is not a global, or not one axis, is refused. A plot group 1D has "xunit" (x = ec.I_1 in mA reads ×1000, labelled mA). A JSON list is read as the list's text. Carriers past the gain table's Nmax are warned, once: "its carriers reach … per cm³ … past its wells' gain table's … (Nmax): its gain held at the table's last there - raise Nmax". |
| 10 | semi.Rst and semi.gain leak into the neighbouring elements (∫Rst over all domains 9× Istim/q; 1090 per cm inside the barriers) | Nought outside the laser's gain domains: semi.gain, semi.Rst read per element by domain. Tested: their integral over every domain equals the gain region's. |
| 11 | The example's log warnings: InGaAs index outside 0.999-17 µm; quantum wells "ask for more states" | A material with its own n (and no nrange) is not range-checked: the warning was about the library's table it does not use. The example asks for twelve states: with five, transparency was 1.00e18 and the warning stood; with twelve, 1.094e18 and no warning. |
| 12 | Units "4.591e-11 aJ", "2.132e-11 aA", emr.we in "Pa"; a 1-point L-I 113 s against 93 s for 31; "Since your last call" glued to the error text | Beyond the prefixes (below 1e-18, above 1e18) a quantity is written in plain SI: "4.591e-29 J". Energy densities are in J/m³. The one-point L-I: most of its time was its walk from equilibrium, 20 steps to 1.9 V. Nine of its tries at the threshold (1.33 V) took the photons to none, cut to a twentieth at every Newton iteration, and each ran all 40 iterations: 72 of the walk's 84 s. Such a try is now given up after eight iterations. One point at 1.9 V takes 61 s (it was 110; the mode and the set-up are 26 s of it). The example's 36 points take 74 s (103), with the same results. The walk's log line gives its Newton iterations too: "reached in 20 steps (175 of Newton's)". Also, a laser's `initialize()` walked to the voltages once without the mode before its step walked again; it is skipped for a laser now. The note is a text of its own starting on a new line, and the MCP reply reads it trimmed into "since_last_call". |
| - | Root cause of item 2: an interface node's box | Item 2 (a), (b). |
| - | The example's SRH lifetimes (5 ns, no "source": "user") taken as the material's 1 ns | A lifetime set on the feature is the feature's, as its other parameters are; a lifetime it does not set is the material's. The example's 5 ns now count: threshold 49 µA. |
| - | semi.taup (the photon lifetime) shadows the holes' SRH lifetime | The photon lifetime is semi.tauph. |
| - | fem_evaluate ignores "point" on an L-I solution | A point is read as that point. |
| - | High Q (1.76e7) does not converge; the log gain's floor (−10 g0) not documented | The floor is in the help and the format text. High Q: **not fixed.** At Q about 2e6 (κ 1e-6, the test's) the curve stops past threshold at 1.45 V. Newton's linearization there sets dS = −S. The points before it are kept, and the stop is said. A coarser mesh fails earlier; a finer one converged in the one case tried. |
| - | MOSFET small signal with velocity saturation: G_drain_drain 0.604 mS AC against 0.197 mS DC slope (3.07×), G_drain_gate 7.00 against 6.40 mS | Newton holds the mobilities a step (lagged), and the small-signal matrix was that Jacobian: the mobility's own change with the field was missing. The small-signal matrix now has each triangle's mobilities' change with its nine unknowns (V, n, p at its corners; central differences) times its edges' currents, in the matrix and in the terminals' currents. A saturated bar at 1e7 V/m: G 0.081122 S, its DC slope 0.081122, q N dv/dE 0.081122; with the mobility held it was 15.981 (197×). The reviewer's MOSFET at Vd 5 V, Vg 2 V on this build: G_drain_drain 0.1928 mS against the DC slope 0.1952 mS (±10 mV); G_drain_gate 6.378 mS against the DC slope 6.449 mS (−1.1 %; it was +9 %). (Its drain current is 7.483 mA here, 7.446 on the reviewer's build: the recombination's box of item 2.) A surface's (Lombardi's) mobility, of the field across, is included the same way. Each derivative is a central difference of 1e-7 Vt. At first it was 1e-4 Vt, which moved a weak-field triangle's potential as much as its corners differ, so the differences measured the curvature of |∇φ|; a junction's Gak came out 7 % from −Gaa, and the photodiode's matrix carried a current with both contacts raised together. The potential's three derivatives are made to sum to nought, as they must: the mobility depends on the potential through its gradient alone. A saturated junction (vsat 1e5 cm/s) is now a two-terminal's to 1e-6 (Gaa = −Gak = −Gka = Gkk). Its Gaa is 0.362167 S against the DC slope 0.362097 at 0.6 V, and 3.70e-14 against 3.57e-14 at −2 V; with the mobility held it was 7.95e-13 there, 22× off. Also changed: the Ge photodiode example's response is faster. Its 3 dB is now 33 GHz at 1 mW and 36 GHz through 50 Ω at 25 µW; held, the field's feedback through its own carriers was overstated: 29 and 35. Its description and test now say so (0.82 at 24 GHz, was 0.77). Its AC conductance is no check against its DC curve: the DC points re-solve the light at each bias, so the generation follows the field, while the small signal holds it. |

## 8bd5709: the open items

| Feedback | Now |
|---|---|
| Sweep labels round to 4 figures: 21.542937, 21.543037, 21.543137 all "theta = 21.54" | A sweep's values are named with the fewest figures, from 4, that tell them apart: "L = 21.5429 mm", "21.5430 mm", "21.5431 mm". |
| "Solved … in X s" is the last point's time | A line of the log after the sweep: "The sweep: N points in X s (the lines above the last point's)". |
| Seconds carry float noise (0.4460000000000003) | Rounded to the millisecond. |
| 1D slower than the 2D layered solve: 0.74 s against 0.45 s for 8 points (a single 2D point 0.74 s, against 0.50 earlier) | Each 1D point rebuilt its strip and walked from equilibrium; a 2D sweep starts each point from the one before. The strip's solution now goes with the field to the next point: it starts there if its strip is meshed alike. The reviewer's 8 points take 0.26 s in 1D (0.74 s) and 0.45 s in 2D. One point takes 0.37 s in 1D and 0.61 s in 2D (the reviewer's 0.74), 5 steps from equilibrium. The earlier 0.50 s was not rerun: that build is not at hand. |
| Layered rows of two heights: the walls pull V off level (2.6 mV across `pin_200_40_200`; `a_box` a third of each triangle while the doping is Voronoi) | `a_box` is the Voronoi share (110e8b9 item 2): rows of 5 and 25 nm level to 3.9e-16 V. |
| The mesa warning once a point; "some %"; the missing "at" and coarse mesh only in the log | A sweep's warnings are each said once. The mesa's says its numbers: "4 to 11 % more forward, 35 % through narrow-wide-narrow". The "at" and coarse-mesh notes are warnings. |
| "bars's" | "bars'". |
| InP: 1.48e-4 per K as a compound, 2.0e-4 in the library | A compound's dn/dT is calibrated by its binaries' measured values at 1550 nm: GaAs 2.247e-4 (Franta et al.), InP 2.0e-4. An alloy takes its binaries' factors by their shares, the same factor in n(T). GaAs as a compound now 2.243e-4 at 1.55 µm, InP within 1 % of the library; In0.53Ga0.47As 2.505e-4, the 1.3Q 2.305e-4. The library's GaAs dn/dT is now Franta's 2.247e-4 (it was 2.35e-4). |
| InGaAs κ rises with T, but the thermo-optic mode loss stays at 3.266e6 dB/m | The material's κ at Tref ± 0.5 K gives dκ/dT, used with dn/dT: the loss of In0.53Ga0.47As goes from 3.266e6 to 3.837e6 dB/m from 300 to 350 K. |
| Near the gap: AlGaAs x 0.315 +0.0137 against a claimed 0.008; In0.53Ga0.47As +0.040 above Adachi at 1550 nm, +0.141 at 1.3 µm (InGaAsP y 1 3.5389 against InGaAs 3.5704); GaSb, InSb, InP regressions; AlGaAs NaN above 25 µm | InGaAs near InP's lattice (In 0.532 ± 0.1) is Adachi's InGaAsP at y 1, weighed out between 2 and 4 µm: In0.52Ga0.48As 3.5438 at 1.5277 µm and 3.4786 at 1.9566 µm (Adachi's 3.5276 and 3.4688), and InGaAsP at y 1 equals InGaAs 0.532 to 1e-4. The 68c6656 write-up's AlGaAs 0.008 was the rms; its worst is 0.0137, at 0.652 µm. GaSb, InSb, InP: **not changed**, the price of the refit nearer the gap; said here. AlGaAs above 25 µm: outside its range (0.633-5 µm), warned; NaN past 1.5 times the range, as designed. |
| `te` silently takes a mode below the cladding (8 modes about 1.4: 1.4239, Γ 0.0025); 2 modes about 2 lack TE0 and `te` takes 1.493 (TE 0.70) | Two warnings. A chosen mode below the cladding's index (the largest index at the window's edge) is "not guided: no guided mode in its set; search about a higher index", with the mode and the cladding's index in the log. Not said where the window's edge is as high as anything, a slab's or a rib's. A te (tm) mode chosen under 90 % TE (TM) is "a hybrid - its set may lack the TE mode: ask for more modes". None TE-like: "the most TE chosen". The reviewer's SOI wire: 8 about 2, TE0 2.4454 (TE 0.983), nothing said; 4 about 1.2, 1.2331 and the cladding warning; 2 about 2, 1.4938 (TE 0.673) and the hybrid warning. |
| Resonator feature and step choices taken silently | The checks of 110e8b9 item 9, at every study's start: "choose": "bogus", "polarization": "xy", "count": 2.5 refused. |
| Figures text "VCSEL" for a PEC cylinder | Generic: "seven: the solve's on this mesh - its error is the mesh's …". |
| Globals "at the last" while the log says "mode 1 kept" | fem_solve's globals are the kept mode's, said so: "mode 1, the one kept for the steps after". |

## Breaks

Fifty-two changes broken on purpose; each was built, its tests run, and the file restored. The script is `breaks.py`, kept in scratch.

- **45 caught at once.**
- **4 caught after a test was added or made stricter:**
  - **Recombination over the node's own domain alone:** no test had two domains of one material. Now `aWallOfOneMaterialRecombinesWhole` has a diode's n side split in two; the break moves its current by 1.4 %.
  - **A fresh laser not walked again:** with `initialize()` skipped for a laser, only a stationary step solved before the mode reaches it. The test now runs that sequence, and the break fails "on the way to the contacts' voltages".
  - **Quantum gain at rest logged, not refused:** `theVcselLaserExample` now puts a stationary step in the Laser L-I step's place.
  - **Quote bars without the view's own painting:** the tests now see the quote's text beside the bar.
- **3 not caught:**
  - **The stimulated recombination's mean weighed by the whole box:** a mean of R with other weights. It differs only where R differs at a wall of two materials, which no test's geometry shows.
  - **Seconds unrounded:** the test's three points summed exactly. Its check is now exact (the double nearest a whole number of ms), and it sees noise when there is some.
  - **The small signal's steps of 1e-4 Vt, alone:** with the potential's changes summed to nought they stay inside the tolerances. The two breaks together were caught (Gak 7 % from −Gaa), and the sum alone was caught.

## Not done

- **High Q past threshold** (Q about 2e6 and above): Newton's linearization of S there; the
  partial curve is kept and the stop said.
- **The VCSEL's LU factorization** cannot be stopped mid-way (8 s): as the fuzz round said.
- **The GUI's stuck title and colour bar**: not reproduced.
- **GaSb, InSb, InP near-gap** against Adachi: left as the refit made them.
- **A 1D profile no finer than the mesh where it is read** (off by up to 40 mV): said, as before.
- **The example's current at 1.9 V** still moves 11 % with the cavity's rows doubled from ten.

## Tests

New:

- **test_fem_laser**:
  - `aModeKeptAndAStepAfter`: mode 4 kept, a stationary step after it, each mode read as itself;
  - `aReverseBiasAndAStepAboveThreshold`: a point below 0 V in the list; a stationary step at 1.6 V from equilibrium;
  - `theThresholdSaid`: each case of the log; the high-Q curve cut at 1.45 V, kept and said;
  - `itsCurrentWhatItRecombines`: the budget to 1e-5; gain and Rst nought outside; an L-I's point read;
  - `onePointAboveThreshold`: under 350 Newton iterations at 1.6, 1.8 and 2.0 V (271; 484 before).
- **test_fem_semiconductor**:
  - `smallSignalSaturated`: G22 against the DC slope within 2 % and against q N dv/dE within 10 %;
  - `aStacksRowsLevel`: rows of two heights level.
- **test_fem_waves**:
  - `theThermoOpticAndTheChoices`: κ(T) in the loss; an own index unwarned; choices refused;
  - `aChoiceItCannotMakeIsSaid`: the SOI wire's three sets; a slab not warned.
- **test_multiphysics_doc** `theUpdateChecksInTheTools`: the kept mode's globals, the figures text, J/m³, the seconds and the sweep's line, a 1D plot's x refused, xunit.
- **test_fem_semiconductor**:
  - `aJunctionsSmallSignalSaturated`: a two-terminal's matrix to 1e-6; Gaa against the DC slope within 1 % forward and 10 % reversed (its DC current noisy to 1e-16 A);
  - `aWallOfOneMaterialRecombinesWhole`: the split diode's current as the whole one's to 1e-6.
- **test_diagram_legend** `longAxisLabelsFitTheDiagram`.
- **test_markdown_doc** and **test_claude_code** `quotesHaveTheirBar`: the bars where the quote is, drawn over the view.

Extended:

- **test_fem_laser**:
  - `refusals`: β 0, no Radiative, ng 0, keep −1, gain "parabolic", carrier "photons", count 2.5, an unknown key warned, voltages as a JSON list;
  - `theVcselLaserExample`: its new numbers; Nmax 1.5e18 warned once.
- **test_fem_modulator** `semiconductorIn1D`: the second point from the first's strip (2 steps of its walk against 3); the mesa's warning once, with its numbers.
- **test_fem_semiconductor** `srhLifetimes`: a lifetime set on the feature.
- **test_fem_materials**: GaAs and InP dn/dT within 1 % of the measured; InGaAs against Adachi's.
- **test_fem_engine**: plain SI beyond the prefixes; "array rows'".
- **test_fem_physics** `sweeps`: names that tell 21.5429 from 21.5431.
- **test_qucs_control**: the note on a line of its own.
- **test_fem_optoelectronics** `thePhotodiodeExample`: 0.78 to 0.86 at 24 GHz (0.70 to 0.80).
- **test_multiphysics_doc**: the seconds exactly a whole number of ms.

Also fixed on the way:

- **The fuzz round's commit (bd1371f)** added `tests/data/lumerical_damaged_dataspace.ldev` without listing it in `scripts/ci/check-third-party.py`, so CI's lint failed. Its suite had run before the file was tracked. The test data's generator now writes the file from `lumerical_heat.ldev` with the fuzzer's five edits (byte for byte) and it is listed; that is a commit of its own.
- **A new test wrote `QJsonArray{QJsonArray{5, 10}}`**, a copy, not a list of one point; `check-json-nesting.py` found it.

## Runs

- **The Release suite:** 126 of 126.
- **The touched tests under the sanitizers (ASan/UBSan):** no report.
  - **Whole:** test_fem_laser, test_fem_waves, test_fem_semiconductor (813, 1,087 and 1,671 s, three at once), test_diagram_legend, test_fem_materials, test_fem_engine, test_mcp_server.
  - **Their functions:** test_fem_modulator, test_markdown_doc, test_claude_code, test_multiphysics_doc, test_qucs_control, test_fem_physics, test_fem_quantum, test_fem_optoelectronics; the trimmed ones again after the trimming.
- **Each new function timed there.** Those over 200 s got a lighter sanitized mode:
  - the thermo-optic test's InAs checks in a 1 µm box (its absorbing mode in the 6 µm one took 12 s in Release, 400 s sanitized);
  - the SOI wire's set of eight left out;
  - the reverse-bias test without its first stationary step, the L-I's reverse points alone;
  - one voltage, not three, above threshold;
  - the saturated bar on a coarser mesh.
  Each is now 40 to 130 s.
- **CI's sanitized Debug job** (its time limits 3 times each test's) took 2,056 s for test_fem_waves at 3bf5821, with a limit of 2,700. test_fem_waves and test_fem_laser now have 1,800 s limits (5,400 there), as test_fem_semiconductor has.
- **test_fem_quantum's `spectrumAsLumerical`** takes about 300 s sanitized here, at QtTest's own 300 s limit locally (CI's is 900). It was not changed this round.
- **The lints** (check-json-nesting, check-tool-arguments, check-third-party) pass.
