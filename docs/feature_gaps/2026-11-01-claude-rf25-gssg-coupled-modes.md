# RF 2.5D: a GSSG's even and odd modes, 1 November (night)

The reviewer's GSSG round (`~/Desktop/qucs-reviews/RF2p5D/2026-11-01-claude-rf25-gssg-coupled-modes.md`) was made on
the build of 1 November 14:37, to `75ff6f9`. The task was a GSSG line's even and odd n_eff and Zc. It could be done
only by hand, outside the tool, from the quasi-static solve's partial capacitances. The full wave could not be used
at all. The case is the rf_benchmark GSSG: 150 µm InP (12.4, tan δ 1e-4), nothing under it, 2 µm Au;
G 300 | 35 | S 50 | 50 | S 50 | 35 | G 300 µm.

Every item was reproduced on `83b0caa`'s code through `qucs-s --mcp-server`, on copies of the reviewer's models
(`~/QucsWorkspace/rf25_check/fw/gssg_L*.qrf25`), against QFEM's curves of the same line
(`~/QucsWorkspace/rf_benchmark/results/gssg_{even,odd}_curves.dat`).

## Findings

| Feedback | Now |
|---|---|
| **G1. Coupled lines referred to infinity; the grounds float** (major) | **Done: the lines return through named return conductors.** A net is a return when a **Return Conductor** (a new node under Ports, named G1, G2…) is on it, or a via joins it to the ground plane. With no ground plane and none marked, the two outermost of three or more nets across a straight run are returns: a coplanar line's grounds. The quasi-static answer is then of the signals alone, held at a volt against the returns at nought: C, C0, L, R and G per metre, and their modes. A symmetric pair's are **even and odd by name, each with n_eff and Zc**. On the GSSG it is the reviewer's own hand reduction, to the last digit of the solve. With no plane, infinity floats, M − (M1)(M1)ᵀ/(1ᵀM1). Over a plane the signals' block of the Maxwell matrix is taken as it is. A via to the ground, refused before ("solve it full-wave"), now holds what it lands on at nought as a return. |
| **G2. "symmetric pair": false** | **Done.** The two signals against the grounds are a symmetric pair, and the answer says so. |
| **G3. No per-metre coupled matrices** | **Done.** The answer's "coupled lines" carry "per metre": C, C0, L, R and G, of the middle cross-section, in Maxwell form. They also name the conductors and what the lines return through. The tab's summary has a "Per metre" line of C, C0 and L. |
| **G4. The full wave refuses a stack with no ground plane** (major) | **Done.** With no ground plane, a port's current comes back through the layout's conductors. A coplanar edge port's returns through those beside it; an internal port's comes back from the metal it is driven from. What goes in comes out, and the potential's nought, at infinity, drops out. A port that would return through the ground is refused with its name and what to do: an edge port not coplanar, or an internal one from the ground. So is a via down to a ground that is not there. The retarded Green's functions already had an open half-space below. The GSSG on InP with nothing under it, as its two faces at four cells across, is within **0.2 % of QFEM's full-wave odd n_eff and 0.7 % of its even from 10 to 50 GHz** (tables below). |
| **G5. A coplanar port cannot be told its grounds** (major) | **Done: a coplanar port returns through the nearest return conductors either side, past any other**, a GSSG's other signal among them. Its "drives" names them and the one it passes. Two ports can be a **differential pair** ("Pair with" on a port: this one the plus, that one the minus). The answer then has their **mixed-mode S**: each pair's differential mode, then each common mode, then the ports of no pair, into twice, half and their own reference. The sweep carries SDD21, SCC21, SDC21 and the rest. **rf25_export's "modes" mixed** (and the Touchstone node's Modes) writes them in Touchstone 2.0 with its **[Mixed-Mode Order]** (D1,2 D3,4 C1,2 C3,4). A coplanar port whose nearest conductor is a signal another port drives is refused, with the advice to mark the returns. Over a plane nothing else makes a GSSG's grounds returns. |
| **G6. No loss without a ground plane** | **Done.** Wheeler's rule recedes the returns' faces with the signals'. At direct current a signal's R is never less than its own and the returns' resistance together. The odd mode's loss at 50 GHz is 144 dB/m at six cells across and 146 at sixteen; QFEM's is 148 quasi-TEM and 150 full-wave. The crowding the reviewer saw gave 192 and 228. |
| **G7. The answer hard to read** (minor) | **Done.** Brief now keeps the coupled lines with their per-metre matrices, and the modes with n_eff. The 2N-port S comes only when 'length' asks for it; without it, the answer says so in its place. |

### The quasi-static GSSG against its grounds

The tool's own answer on the reviewer's 1 mm models, as a sheet, the reviewer's hand reduction in brackets
(infinity floating), and the odd mode's loss at 50 GHz:

| cells across | even n_eff | even Zc (Ω) | odd n_eff | odd Zc (Ω) | odd alpha, dB/m | even alpha |
|---|---|---|---|---|---|---|
| 4 | 2.5165 | 86.32 | 2.5820 | 43.51 | 140.3 | 109.6 |
| 6 | 2.5181 (2.518) | 84.39 (84.4) | 2.5821 (2.5821) | 42.86 (42.86) | 143.6 (the sheet's crowding: 192) | 113.1 |
| 10 | 2.5199 (2.520) | 82.81 (82.8) | 2.5823 (2.5823) | 42.31 (42.32) | 145.3 | 115.1 |
| 16 | 2.5209 (2.521) | 81.93 (81.9) | 2.5823 (2.5823) | 42.02 (42.02) | 146.3 (228) | 116.3 |
| conformal mapping, zero thickness | 2.500 | 79.9 | 2.596 | 41.4 | | |
| QFEM 2D, 2 µm Au, quasi-TEM | 2.502 | 79.2 | 2.556 | 40.8 | 148.1 | 117.0 |

The answer now names its two signals, "returning through" the two grounds, a symmetric pair. As its two faces at
sixteen across, the odd mode is within −0.35 % of QFEM's n_eff, +1.8 % of its Zc and 1 % of its alpha at 50 GHz.

### The full wave with no ground plane

Four coplanar edge ports, S1 and S2 at each end, paired (P1 with P2, P3 with P4), each returning through both
grounds past the other signal. The even and odd modes come from the two-line method on the mixed-mode Touchstone:
the common modes' two-port and the differential modes' two-port of two lengths. Against QFEM's full-wave curves:

| GHz | odd n_eff, faces 4 across | QFEM | even n_eff, faces 4 across | QFEM | odd, sheet | even, sheet |
|---|---|---|---|---|---|---|
| 10 | 2.5532 (−0.18 %) | 2.5577 | 2.4898 (−0.67 %) | 2.5065 | 2.5958 (+1.5 %) | 2.5133 (+0.3 %) |
| 30 | 2.5460 (−0.07 %) | 2.5478 | 2.4893 (−0.55 %) | 2.5031 | 2.5901 (+1.7 %) | 2.5139 (+0.4 %) |
| 50 | 2.5446 (−0.07 %) | 2.5463 | 2.4963 (−0.56 %) | 2.5103 | 2.5888 (+1.7 %) | 2.5210 (+0.4 %) |

(The faces from lines of 1 and 2 mm, the sheet's from 1 and 2 mm too.) As a sheet the odd mode is the zero-thickness
line's, 2.596, plus its gold's own inductance: the static sheet gives 2.582, and R/ω of the odd mode adds 2.6 % to
its L at 10 GHz. As its two faces, the walls take it to QFEM's.

The loss, from lines of 2 and 4 mm as a sheet (the differences of shorter lines are a few hundredths of a decibel,
under what the ends leave):

| GHz | odd alpha (dB/cm) | QFEM | even alpha | QFEM |
|---|---|---|---|---|
| 10 | 0.597 (−11 %) | 0.668 | 0.436 (−16 %) | 0.520 |
| 30 | 1.039 (−9 %) | 1.141 | 0.801 (−11 %) | 0.898 |
| 50 | 1.522 (+1.5 %) | 1.499 | 1.111 (−7 %) | 1.192 |

That is the sheet's edge loss as the last round left it: 10.8 dB/m against QFEM's 12 at 1 GHz on the check's GCPW.

## What reproducing found

- **The answer was right but referred to infinity.** With no plane the four conductors' four modes were each
  against infinity: mode 2 (Z 61 Ω) was signals and grounds together, not the even mode. The reviewer's reduction
  is the tool's now. The grounds are held at nought. Infinity floats, since the charge to infinity per metre falls
  as the log of the length: C − (C1)(C1)ᵀ/(1ᵀC1), of the per-metre Maxwell matrix. The loss matrix is that one's
  derivative in the permittivities. L is C0⁻¹/c² of the signals' block.
- **The loss had no return to recede.** Wheeler's rule moves every face of every conductor in, the returns' with
  the signals'. Without a plane it was skipped, and the sheet's charge crowding was all there was. Taken with the
  grounds, it is settled at six cells across.
- **The full wave never needed the plane for a coplanar port.** Its columns go in at the signal and out at the
  grounds, so nothing is left at infinity. The Green's functions already had an open half-space; the partial
  inductances have no image there. Only the refusal stood in the way.
- **"Beside" was the nearest conductor, signal or not.** For S1 of a GSSG that was S2. The returns are now known
  to the full wave as to the static solve (the same returnNetsOf()), and the search passes over what is no
  return. A port whose nearest conductor is a signal another port drives ties the two signals through each
  other's faces, and the tied columns came out singular ("could not be tied"). That is now refused with the cause
  and its remedy.
- **Two of the tool's own rules took a line's direction from its bounding box.** Both the outermost-grounds rule
  and the per-metre cross-section did. A GSSG half a millimetre long is 820 µm across, and was solved across: no
  grounds, no line. Both now take the direction every net runs the whole length of, where the nets lie no
  further apart across it than it is long. Two pads a millimetre apart stay what they were, no line. The tab's
  default length for a uniform line is its own, not its longer side.
- **What counted lines counted nets.** The tab's S summary, the coupled Touchstone's port count and its comment,
  rf25_export's answer and its default file name took one line a net. Against returns they are the signals': the
  GSSG is a 4-port, and a coplanar line against its grounds is one line, not net 0 - which may be a ground.

## What changed

- **rf25_static**: returnNetsOf() - marks, a via to the ground, the outermost two in either direction - and
  Extraction's returns, signalNets() and signalsOf(): the signals' C, C0 and loss, infinity floating with no
  plane. Wheeler's 2D solve of the signals against the returns with no plane, with the free-space potential, the
  charge held to none. The DC floor with the returns' own resistance (dcReturn). lineParameters() of one signal
  against its returns, and none of a return. The per-metre direction of a short line.
- **rf25_sparams**: asCoupledLines() of the signals alone, L of their C0, R of Wheeler's or the crowding.
- **rf25_fullwave**: the returns found as the static solve finds them; besideFaces() keeps to them and says what
  it passes (Beside::passed), which makes the feed "unlike" for calibration. No ground plane allowed where every
  port returns through the conductors. Differential pairs (FullWavePort::pair, FullWave::pairs) checked as given;
  mixedModeOf(); touchstone2Of(..., mixed) with [Mixed-Mode Order].
- **rf25_model**: the Return Conductor node (G1, G2…), a port's "Pair with", the Touchstone node's "Modes".
- **rf25doc**: the marks handed to both solvers; the pair read; a mixed-mode export; a coplanar port's grounds
  drawn as the solve's returns (besideEdges() given which shapes are returns); the return conductors drawn by
  their names; the summary's per-metre line; a uniform line's own length; the S summary and the coupled
  Touchstone of the signals, a one-line export of the first signal.
- **qucscontrol_rf25 / qucscontrol**: the quasi-static answer's nets ("return"), coupled lines (conductors,
  returning through, symmetric pair, per metre, the S only with 'length'), brief with both; the full-wave answer's
  "mixed-mode" (modes, of, reference impedances, the matrix, its sweep); rf25_export's 'modes'. Both tools'
  descriptions say so, within 2048 characters (2014 and 1948); rf25_export counts the signals' ports.

## Tests

- test_rf25_static `aGssgsSignalsAreCoupledLinesAgainstItsGrounds`: the returns; the signals' C against the hand
  reduction to 1e-9; even and odd by name against the closed form; Wheeler's loss settled from four to six cells
  across and within 10 % of QFEM's; the DC floor with the returns on 10 µm of gold; a 500 µm piece the same line;
  marks; a return no line; one signal's line; a via-fenced GSSG over a plane, its signals' block as it is.
- test_rf25_fullwave `aGssgReturnsThroughItsGroundsWithNoGroundPlane`: no plane; each port past the other signal
  through both grounds, named; the feed unlike; the pairs; no mode conversion (under 1e-6); each mode by two
  lengths within 1.5 % of the static extraction of the same mesh (0.8 and 1.2 % at three across, falling with
  the mesh); a port through the ground, an internal one from it and a via to it refused; metal-to-metal internal
  ports solved, reciprocal and passive.
- test_rf25_fullwave `aDifferentialPairsModesAreItsMixedModeS`: T S Tᵀ by hand to 1e-12; a pair named from both
  ends one pair; a port of no pair after the modes; the Touchstone's [Mixed-Mode Order] and data; a pair checked
  as given; over a plane, unmarked refused and marked solved.
- test_rf25_doc `aCoplanarPortsGroundsAreDrawnAsItsReturns`: the drawn grounds the two grounds' edges past S2;
  G1 and G2 drawn; the node's Modes read; the tab's solve with the marks.
- test_qucs_control `aGssgIsSolvedThroughTheToolsAsItsModes`: the whole through the tools - its own 600 µm length,
  the returns, the coupled lines per metre, the S only with 'length', brief; the signals' 4-port written and
  summed up by the tab; the full wave's ports, mixed-mode S and its sweep, brief; the mixed Touchstone, and what
  cannot be written so.
- Changed: whatItCannotSolveIsSaid (no plane: refused only where a port returns through the ground),
  aViaToTheGroundIsSolvedFullWaveAndDrawn (quasi-static, the via's net a return), anRf25ModelIsSolvedThroughItsTool
  (the 4-port asked for with 'length').

## Broken on purpose

`docs/feature_gaps/2026-11-01-claude-rf25-gssg-coupled-modes/breaks.py` holds thirty-nine:

- **The returns:** a Return Conductor not taken; what a via joins to the ground no return; the outermost two
  looked for along the longer side alone; the lines of every net, the returns' too.
- **The signals' lines:** infinity held at nought where it floats; Wheeler's rule wanting a ground plane; the
  returns' own resistance out of a signal's at direct current; a return given a line of its own.
- **A short line:** the per-metre line along its longer side alone; two pads far apart a line.
- **The full wave's coplanar port:** its returns the nearest conductors, whatever they are; a signal passed over
  not said of the feed; a port returning through what another drives, solved.
- **No ground plane:** every port refused, as before; a port through the ground driven against infinity; a via
  down to a ground that is none.
- **The pairs:** a pair named from both ends two pairs; its two references not held alike; the common mode a second
  difference; the modes by pair rather than each differential, then each common; [Mixed-Mode Order] the common
  modes first; the mixed-mode Touchstone's data the ports' own.
- **The tab:** the Return Conductors not handed to the full wave; a port's Pair with not read; the export node's
  Modes not read; a mixed-mode export written port by port; a coplanar port's grounds drawn as the nearest
  conductors; a uniform line's length its longer side; the S summary of every net, the returns' too; the coupled
  Touchstone's ports named of every net; a Return Conductor named as no ground.
- **The tools:** the matrices per metre left out; what the lines return through left out; the coupled lines' S of
  a length unasked; the brief's modes without their n_eff; the full-wave answer without the pairs' modes; the
  modes' references the ports' own; mixed modes let into Touchstone 1.1; the coupled export's ports counted of
  every net.

**All thirty-nine are caught**, thirty-eight on the first run. The thirty-ninth, a return given a line of its own,
was not: the test asked it of a GSSG ground, which two signals refuse anyway ("one of 2 signals"). On a coplanar
line against its grounds there is one signal, and without the check a ground was given the signal's line. The test
now asks it there too, and the break is caught.

## Under the sanitizers

The new and changed functions, each alone in the sanitized build, with no report:

| Function | Time | Peak memory |
|---|---|---|
| test_rf25_static `aGssgsSignalsAreCoupledLinesAgainstItsGrounds` | 41 s | 919 MB |
| test_rf25_static `aCoupledPairsModesAreOfItsCrossSection` | 12 s | 600 MB |
| test_rf25_static `aLoneConductorWithNoGroundIsNoLine` | 10 s | 571 MB |
| test_rf25_static `whatItCannotSolveIsRefused` | 1 s | 238 MB |
| test_rf25_sparams `twoLinesAreCoupledLinesWithTheirModes` | 11 s | 453 MB |
| test_rf25_sparams `theLossMatrixCouplesAsWellAsLoads` | 7 s | 451 MB |
| test_rf25_fullwave `aGssgReturnsThroughItsGroundsWithNoGroundPlane` | 25 s | 443 MB |
| test_rf25_fullwave `aDifferentialPairsModesAreItsMixedModeS` | 9 s | 260 MB |
| test_rf25_fullwave `whatItCannotSolveIsSaid` | 28 s | 320 MB |
| test_rf25_fullwave `aCoplanarPortReturnsThroughItsGrounds` | 44 s | 260 MB |
| test_rf25_doc `aCoplanarPortsGroundsAreDrawnAsItsReturns` | 11 s | 671 MB |
| test_rf25_doc `aViaToTheGroundIsSolvedFullWaveAndDrawn` | 6 s | 638 MB |
| test_rf25_doc `whatIsNoLineHasNoSParameters` | 4 s | 799 MB |
| test_qucs_control `aGssgIsSolvedThroughTheToolsAsItsModes` | 16 s | 901 MB |
| test_qucs_control `anRf25ModelIsSolvedThroughItsTool` | 42 s | 1141 MB |

The whole full-wave suite took 375 s sanitized (357 last round), the static suite 142 s. CI's runner is about
twice this machine there, which puts the full wave near 750 s against its 900 s limit times the Linux job's
scale of three; QtTest's 900 s for one function is the nearer limit, and the heaviest new one is 41 s here.

Rebuilt after the breaks, the full ctest (149), the scenario script (97 checks) and the three lints (tool
arguments, JSON nesting, third-party names) were green.

## Left open

- **The full wave's sheet loss** stays where the last round left it: 7 to 16 % under QFEM's on the GSSG at 10–30 GHz,
  the even mode the more. The two faces' loss needs lines longer than the 6000 unknowns allow to read by two
  lengths: a millimetre's difference is a few hundredths of a decibel.
- **Wheeler's R below a few hundred MHz.** On a metal thin beside its width, Wheeler's rule with the slab's
  resistance stands above the true one at low frequency, the current spread evenly there and not as the field has
  it: 315 Ω/m for the GSSG's 2 µm of gold at 1 kHz against 264 at direct current, and its mutual R 68 against the
  shared return's 4. The floor bounds a signal's own R from below only, where it binds on a thick metal (10 µm here).
- **A coupled feed is not calibrated.** A port past a signal says why ("no return, lies between it and its
  return"): strips of the one signal and its grounds are not the coupled line it is fed by. Its reference plane
  stays at its cells, or moves along a given line.
- **A pair of two references** is refused rather than given generalized mixed modes.
- **"The conductors no port is on" as the default returns**, the reviewer's other suggestion, was not taken: the
  outermost two cover a coplanar line, and marks name any others.
