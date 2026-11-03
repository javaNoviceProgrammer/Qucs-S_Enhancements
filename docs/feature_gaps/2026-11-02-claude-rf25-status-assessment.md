# RF 2.5D's assessment of 2 November: its workflow items, and the solver held to a published measurement

The reviewer's overall assessment after the checks of 24 October to 2 November, on the build of 9603502:
`~/Desktop/qucs-reviews/RF2p5D/2026-11-02-claude-rf25-status-assessment.md`. It finds the field solve sound wherever
it was held to an independent reference, and names five priorities: the metal model, a GSG calibration, a
validation suite against measured or commercial data, the workflow, and a coarse-level preconditioner. Asked which
to take, the user chose the workflow's four - a progress row with its Stop and a time estimate, the solution kept
with the model, a one-click equivalent circuit, the pen reader's coarse labels - and a suite of published
measurements, their papers downloaded and each cited. The metal model, the GSG calibration, the preconditioner and
in-tool drawing of shapes and vias were not taken this round.

| Feedback | Now |
|---|---|
| "No progress bar, Cancel or time estimate in the tab (a text counter only; the multiphysics tab has a progress row to reuse)" | **A bar, its Stop, and the time left.** The bar at the end of the status line, from branch claude/quizzical-lumiere-65bd6f, merged here as e0a88ff: busy for the quasi-static extraction and a solve waiting for its mesh, then the sweep's frequencies and each calibration's strips counted. Beside it a Stop, as the multiphysics row has its Cancel. Once one is counted and a second has gone, the time left at the rate it has gone - in the status line, the notes and the bar's tool tip, each feed's strips timed from their own start |
| "The solution is not kept with the model" | **Kept beside it**, `<model>.solution`: written when a solve ends on a model saved as it is, else when it is saved; read when the model is opened, and shown with no solve where the model, its layout file and its mesh are those it was solved from - each stamped - with when and by which build. Otherwise why not, and solved again as before |
| "No bridge to circuit simulation: the Touchstone -> lumped RLC fit done by hand for the MOM would be a natural built-in, as would a schematic part from a solve"; "one-click equivalent-circuit export (Touchstone -> RLC subcircuit, its fit error said)" | **Exports > Equivalent circuit**: a pi of R, L and C fitted to the full-wave solve of one or two ports, each branch the form that fits it with the fewest values, written as a Qucs-S part (`.sch`, which a schematic's Subcircuit places) or a SPICE subcircuit (`.cir`, `.sp`, `.lib`), its values and how far its S is from the solve's said in the file, the panel and the new `rf25_circuit` tool |
| "the pen reader's labels are coarse (an L read as a ring, one-stroke arrows as marks)" | **Corners, and arrows drawn at one go.** An L, a V or a caret is a corner, with its point where the arms meet and what that point is on. Both sides of a V drawn out and back from the point, or a closed triangle, is an arrow, its tip at the point |
| "No measured validation"; "A validation suite against measured or commercial data: about six structures (line, stub, via, spiral, MOM, coupled pair), run automatically as the rf_benchmark is" | **A suite of published measurements in ctest, `test_rf25_measured`, with one structure**: G. Gold, K. Lomakin, K. Helmreich and U. Arz's coplanar line on alumina (Adv. Radio Sci. 17, 2019, CC BY 4.0), measured on a metal chuck to 120 GHz. eps_eff within 1.1-1.6 % of the measured at 10, 30 and 60 GHz; the attenuation 17-19 % under it on the test's mesh, its growth with frequency the measured one's to 2 %. For the other five, no open publication with both measured data and the whole geometry was found (below) |

## The bar's Stop and the time left

The progress bar was its own branch (cdb7671, ea9dc40), merged here first: at the end of the status line, busy - no
count - for the quasi-static extraction and for a solve waiting for its mesh, then counting the sweep's frequencies,
and each calibration's strips at their frequencies as they are solved ("feed 1 of 2" in its tool tip). The Solve
button already turned into Stop while a solve runs.

Beside the bar now a **Stop**, as the multiphysics tab's progress row has its Cancel - a flat tool button, no taller
than the status line, shown and hidden with the bar - which stops the solve as the Solve button's Stop does.

The **time left**: once one of a count is done and a second has gone since that count began, what is left at the
rate it has gone - each done one having taken as long as those before it on the whole. Said roundly:

| Seconds left | Said |
|---|---|
| under 10 | a few seconds left |
| under 90 | about 40 s left (to five seconds) |
| under 90 minutes | about 7 min left |
| more | about 1 h 30 min left |

in the status line ("; solving 1 of 3 frequencies, about 40 s left"), the notes ("Solving full-wave: 1 of 3
frequencies… (about 40 s left)") and the bar's tool tip. The sweep and each feed's strips are counts of their own,
each timed from its start: a calibration's estimate is "for this feed", not the solve's - the strips of a second
feed, or shorter strips solved where the first disagree, are not foreseeable from the first.

## The solution kept beside the model

A model solved once was solved again each time it was opened: a minute or more for a large one. Its solution is now
kept in a file beside it, `<model>.solution` - which sorts with it and goes where it is copied:

- **What:** a full-wave solve's whole answer - each port's S and Z, the currents the drawing takes, what the
  de-embedding did, the checks and the account - or a quasi-static solve's extraction and its loop's inductance;
  the build that solved it, its version and commit (`27.0.5 (934b0de)`), and when (UTC).
- **Against what:** the SHA-1 of the model's JSON as the solver is given it - without its exports, which change
  nothing solved -, the SHA-1 of the layout file's bytes, and the cells of the mesh it was solved on.
- **The file:** "QRF25SOL", its format (1), the stamps, then the answer compressed (QDataStream, Qt 6.0's), written
  whole or not at all (QSaveFile).
- **When written:** as a solve ends, where the model on disk is the one solved; otherwise when the model is saved.
  An unsaved edit's solve is not kept until then, so the file never holds a solve of a model that is not on disk.
- **When taken:** as the model is opened or reloaded it is read; once its mesh is made again it is shown, with no
  solve, where all three stamps match - and the notes say "The solution kept beside the model is shown: solved by
  Qucs-S <build> on <date> <time> UTC. Solve to solve it again." Where they do not, nothing is shown and why
  is said: another version of the model, another version of its layout file, a mesh of other cells (a newer
  mesher's), a solution of another format, or a damaged file. A solve then solves as before.
- **What an edit does:** one that changes nothing the solver is given - an export added, its file named - leaves
  the solve standing, and once saved it is still the one kept. (Before, any edit cleared what was solved.)
- **Claude:** `rf25_solve`'s answer - full-wave, brief or whole, and quasi-static - says `kept beside the model`
  and the file's name.

## The equivalent circuit

**Exports > Equivalent circuit** (`rf_circuit`, its tag `ec…`; File, and Name for a SPICE subcircuit) fits a lumped
circuit to the full-wave solve of the model's ports and writes it. One port is one branch to the reference; two are
a pi - a branch between them, and one from each to the reference.

- **From S to the branches:** at each frequency Y = F (1 - S)(1 + S)^-1 F, F the inverse roots of the ports'
  references; the branch between the ports is -Y12, those to the reference Y11 + Y12 and Y22 + Y12. Where 1 + S has
  no inverse - a port shorted through, two ports joined by a wire, a line a whole number of half waves long - that
  frequency is left out of the fit, and said; only where every one is so is nothing fitted.
- **The forms:** between the ports R, C, R+L, R+C, R||C, R+L+C and (R+L)||C; to the reference C, R, R||C, R+C and
  C+(R||C) (an oxide over a lossy substrate). Each fitted by Levenberg-Marquardt on the values' logarithms, from
  every start of each value at its guess - from how the admittance goes at the band's ends -, a tenth of it or ten
  times it; its misfit the rms of the fitted admittance's relative difference from the solve's.
  The branch taken: the fewest values within a quarter of the best misfit or 2e-3 of it - so a capacitor is not
  written as R+L+C with a vanishing R and a vast L.
- **Held to the solve:** the circuit's S beside the solve's at every frequency, the most it differs, where, and
  its rms - in the file's comment, the panel and the tool's answer. A lumped circuit holds while what it stands for
  is short beside the wavelength; the note says to solve a lower band or use the Touchstone where S differs by more
  than a few hundredths.
- **Written as** a Qucs-S part - a schematic with Port P1 and P2, wires labelled p1 and p2, its R, L and C between
  them and a ground, and a symbol, which a schematic's Subcircuit places - by default `<model>_<tag>.sch` beside the
  model; or as a SPICE subcircuit, `.subckt <name> p1 p2 ref`, where the file is `.cir`, `.sp` or `.lib`.
- **Where:** the node's Write in the panel (the file, then the fit's lines), and `rf25_circuit` (new): the
  branches with their forms, values (SI and as written) and own misfits, `S differs by at most`, `most at`, `S
  differs by rms`, the band, and the file where one is asked - its `node`'s, or `file`. `rf25_export` given such a
  node says that `rf25_circuit` writes it.

The test line - a millimetre of 50 µm strip, solved at 1, 3 and 5 GHz - fitted:

    A pi fitted to the two-port's S from 1 to 5 GHz (3 frequencies): between P1 and P2 R+L: R 1.045 Ohm,
    L 615.9 pH (fitting its own share within 1.4 %); from P1 to the reference C: C 64.35 fF (fitting its own
    share within 0.27 %); from P2 to the reference C: C 64.35 fF (fitting its own share within 0.27 %).
    Its S differs from the solve's by 0.0034 at most (at 5 GHz), 0.002 rms.

written as a part that Qucs-S loads and netlists: between the netlist's p1 and p2 its Y is the circuit's own to a
part in a thousand at each frequency.

## Corners and arrows drawn at one go

The stroke reader (`markup::gestureOf`) knew dots, rings, lines, arrows and arrowheads, and called anything else a
mark. Now:

- **An arrow drawn at one go** is out to the point furthest from where it began, then its head: what comes back
  from the point must stay near it - no further from it than 0.45 of the shaft - and may be as long as twice the
  shaft. So both sides of a V drawn out and back from the point, and a closed triangle, are arrows with their tip at
  the point. (Held before to three fifths of the shaft in length, a V of both sides read as a mark.)
- **A corner** is two straight arms - the stroke reduced to three points within 6 % of its size - at 45 to 135
  degrees, the shorter at least two fifths of the longer: an L, a V, a caret. Its point is where the arms meet; on a
  page `at` says what that point is on, and `point` where; on a layout the same, of the shapes and cells there. A
  tick (its short arm under two fifths), a hairpin (arms 10 degrees apart) and a bend of 150 degrees stay marks.
- **A head drawn after its line** may be a corner now, as a V is: it is still taken for the arrow's head.

## Held to a measurement

The suite asked for six structures - a line, a stub, a via, a spiral, a MOM capacitor and a coupled pair - from
publications that give measured data and the whole geometry openly. Downloaded and read: Gold et al. 2019 (Adv.
Radio Sci., CC BY 4.0); Ponchak, Papapolymerou and Tentzeris, finite ground coplanar lines on silicon (NASA, a
preprint); Janezic and Williams, permittivity from transmission lines (NIST); Hoffmann and Siegl 1982, a
microstrip-slot coupler (IEEE MTT); and Eroglu 2010, a spiral inductor (ACES). One had what a test needs.

**The coplanar line on alumina.** G. Gold, K. Lomakin, K. Helmreich and U. Arz, "High-Frequency Modeling of
Coplanar Waveguides Including Surface Roughness", Adv. Radio Sci. 17, 51-57 (2019), doi:10.5194/ars-17-51-2019.
Its Table 1 and section 2: a 50 µm signal, 25 µm gaps and 270 µm grounds of electroplated metal, taken as 6.5 µm
thick (it varies across the wafer) and 36 MS/m, on 500 µm of Al2O3 of 9.7 and tan δ 1.25e-4 (a split-cylinder
resonator's, 8 to 22 GHz); its propagation constant measured by a multiline calibration on a metal chuck - a ground
under the substrate - and on a ceramic one. The measured curves of its Figs. 3 and 2 (metal chuck) were read from
the vector figures' own paths, mapped through their axes' ticks, each value the mean within 0.5 GHz.

Solved as it was measured: the line's own coplanar feed calibrated, its strips - 4.1, 8.2 and 6.6 mm - giving its
propagation constant as a multiline calibration does, three cells across the signal and none over 100 µm:

| GHz | eps_eff solved | measured | | α solved, Np/m | measured | |
|---|---|---|---|---|---|---|
| 10 | 4.854 | 4.934 | -1.6 % | 6.23 | 7.66 | -18.7 % |
| 30 | 4.804 | 4.874 | -1.4 % | 11.30 | 13.63 | -17.1 % |
| 60 | 4.815 | 4.868 | -1.1 % | 16.61 | 20.33 | -18.3 % |

The strips agree on one line to 2.6e-3 or better at each. The shape is the measured one: eps_eff falls from 10 to
30 GHz as the metal's internal inductance does, then turns up. α at 30 and 60 GHz is 1.81 and 2.67 times that at 10
- measured 1.78 and 2.65 - where a metal's loss alone, going as the root of the frequency, gives 1.73 and 2.45: the
bend the paper's quasi-TEM model missed and its model of radiation and dispersion found (its Figs. 2 and 4).

Both differences come mostly from the mesh, which a test's time keeps coarse. At 10 GHz:

| Mesh | eps_eff | α, Np/m | Its calibration at 10 GHz |
|---|---|---|---|
| 2 cells across, 100 µm, no edge cells | 4.798 | 4.96 | 5.9 s |
| 3 cells across, 100 µm (the test's) | 4.854 | 6.23 | 9.9 s |
| 4 cells across, 100 µm | 4.869 | 6.56 | 11.6 s |
| 4 cells across, 50 µm | 4.880 | 6.94 | 27.0 s |
| 6 cells across, 100 µm | 4.890 | 6.93 | 51.3 s |
| measured | 4.934 | 7.66 | |

A coplanar line's conductor loss by the incremental inductance rule, its grounds taken as wide, is 7.35 Np/m at
10 GHz (the substrate's 0.026 more); the roughness the paper measured, 63.5 nm rms, adds 0.5 % there and 3 % at 60
GHz by Hammerstad's factor. The loss is in the current crowding at the conductors' edges, which coarse cells
average away; α rises toward it as they are made finer. eps_eff's remaining percent the paper puts down, for its own
models, to the substrate's permittivity, the metal's thickness and its roughness.

The test holds eps_eff within 2 %, α under the measured and within a quarter of it, the strips agreeing within
kStripsAgree, and α's growth within 5 % of the measured. It takes 38 s in Release. Under the sanitizers it is
skipped, as test_fem_exciton's dense solves are: the coarsest mesh at one frequency, 5.9 s in Release, was still
solving when QtTest's watchdog stopped it at 300 s, and its code paths are the calibration's, which
test_rf25_deembed runs there. CI's macOS job, a Release build, runs it.

A first try at four cells across and 50 µm, its cells at most twice as long as wide (27804 unknowns on the 8.2 mm
strip), did not converge at 60 GHz: GMRES left a residual of 2.2e-6 after its 3000 iterations. The coarser meshes
converged there in 568 and 755. Long coplanar lines over a ground at tens of GHz are the slow case the coarse-level
preconditioner would help.

**What was not found, or would not do:**

- **A via, a stub, a MOM capacitor, a coupled pair:** no open publication found giving both the measured S and the
  whole geometry - stack, metal, ports and calibration.
- **A spiral:** Eroglu's (ACES 2010) - 6.375 turns of 80 mil copper on 100 mil alumina - measured 594.91 nH at
  13.56 MHz, but its Figs. 2 and 3 put that well up the rise to its self-resonance, where what the paper leaves out
  (the corners' radius, the bridge's path, the ports) moves it by more than a comparison could tell apart.
- **Finite ground coplanar lines on silicon** (Ponchak et al.): their measurements are raster figures in a scanned
  preprint that asks not to be reproduced without the authors' permission, and the wafer sat on a quartz plate of
  unstated thickness over the chuck.
- **The microstrip-slot coupler** (Hoffmann and Siegl): its slot is in the ground plane, which the solver's
  infinite ground cannot hold.
- **Permittivity from transmission lines** (Janezic and Williams): its measured data are the substrates'
  permittivity, which its lines were measured to find - a check of the method, not of a structure's S.
- **Spiral inductances** (Mohan et al. 1999): its download failed certificate verification, and was left.

## Tests

| Test | What it holds |
|---|---|
| `aSolveShowsHowFarItHasGot` (`test_rf25_doc`, extended) | The Stop beside the bar shown with it, no taller than the status line; once a second has gone, the time left in the status line, the bar's tool tip and the notes, said roundly; the bar's Stop stopping the solve, and gone with the bar |
| `theTimeLeftIsSaidRoundly` (new) | A few seconds under ten; to five seconds under 90; minutes under an hour and a half; hours and minutes past it |
| `theSolutionIsKeptBesideTheModel` (new) | A solve written beside the model, its stamps the model's, the layout file's, the cells' and the build's; written back, the same bytes. Opened again: shown with no solve and said, its account, Touchstone, S, Z, currents and checks the solve's to the last bit; a solve again says nothing of it. An export added leaves the solve standing, and kept. An edit saved with no solve, the layout written again, a mesh of other cells, another format, a damaged file: not taken, and why said. A quasi-static solve kept and shown alike; an unsaved model's solve kept once it is saved, not before |
| `anEquivalentCircuitIsWrittenFromTheSolve` (new) | Nothing before a full-wave solve, and why; the line fitted as a pi within 0.05 of its S; written as a part Qucs-S loads and netlists, its Y between p1 and p2 the circuit's to a part in a thousand at 1, 3 and 5 GHz; an export's file named leaving the solve standing; a SPICE subcircuit of the name asked, a space made an underscore, its Y the circuit's; the panel's Write saying where and how well |
| `test_rf25_sparams`: `aPiOfKnownBranchesIsFoundAgain`, `aOnePortIsOneBranch`, `aSlightlyLossyCapacitorIsACapacitor`, `aLongLineIsSaidToBeNoLumpedCircuit`, `whatIsNoCircuitIsSaid`, `theSpiceSubcircuitIsTheCircuit` (new) | A pi of R+L+C, C+(R\|\|C) and R\|\|C found again from its S over 0.5 to 64 GHz, each form and value to 1e-3, its S to 1e-6; a series resonator as a one-port; a capacitor of Q in the thousands a capacitor; a half-wave line still fitted and said far, its 10 GHz left out and said; three ports, nothing solved, a thru and ten micro-ohms between two ports refused; the SPICE subcircuit's lines read back as the circuit's Y to 1e-9; values as a part writes them |
| `anRf25ModelIsSolvedFullWaveThroughItsTool` (`test_qucs_control`, extended) | `kept beside the model` said, and the file there; `rf25_circuit`: three branches, between what, its fit error, its part and SPICE files where asked, its node's file, a node that is no circuit refused; `rf25_export` given a circuit node pointing to `rf25_circuit`, before any solve and after; the model closed and opened again by `rf25_circuit` and by `rf25_export`, each using the solution kept beside it |
| `anRf25ModelIsSolvedThroughItsTool` (extended) | A quasi-static solve's answer says where it is kept |
| `strokesAreReadAsDrawn` (`test_pptx_doc`, extended) | Both sides of a V and a closed triangle drawn at one go are arrows, their tip at the point; a long-sided V and an L corners, their point the bend; a hairpin and a bend of 150 degrees marks; a caret drawn after a line a corner; a corner on a word: at that word, its point given, in its sentence |
| `theRawInputDrawsOnALayout` (`test_layout_doc`, extended) | An L dragged on a layout through `send_input` read as a corner at the shapes and the cell under its bend, its point to 0.1 µm |
| `aCoplanarLineOnAluminaIsTheMeasuredOne` (`test_rf25_measured`, new) | The measured line, as above |

## Broken on purpose

`docs/feature_gaps/2026-11-02-claude-rf25-status-assessment/breaks.py` holds forty-eight:

- **The time left and the Stop:** the time left left out of the notes, the status line and the bar's tool tip, in
  turn; seconds not rounded to five; hours said as minutes; the Stop beside the bar never shown, and stopping
  nothing.
- **The kept solution:** a solve not kept; a save not keeping the solve of the model saved; what was kept not
  shown; one of another version of the model taken, of another layout file, of a mesh of other cells; an export
  added clearing the solve; an edit that changes nothing solved clearing it all the same; a file of another format
  read; the cells' stamp not kept; the full-wave and the quasi-static answers not saying where it is kept.
- **The equivalent circuit:** its Y of the wrong sign; the least misfit taken whatever its values; a frequency of
  no Y fitted all the same; two ports joined by a wire fitted; the SPICE subcircuit without its reference; the part
  without its ground; a subcircuit's name taken as given, a space and all; a circuit node's file by default a SPICE
  one; no Equivalent circuit among the exports; the panel's Write not for a circuit; the tool not writing the file
  asked, not saying how far its S is; `rf25_circuit` and `rf25_export` looking for a solve before the model's mesh
  is made; `rf25_export` taking a circuit node for a Touchstone one, and pointing it to `rf25_circuit` only once
  the model is solved.
- **The pen:** both sides of a V drawn at one go not an arrow; an arrow's head as far from the point as its shaft;
  no corners; a tick a corner; a hairpin a corner; a bend of 150 degrees a corner; a corner not taken for an arrow's
  head; a corner's point not said on a page, what it is at not in its sentence; a corner's point and what it is at
  not said on a layout.
- **The physics the measured line checks:** the substrate's permittivity taken 6 % higher; a two-faced metal's own
  impedance halved.

All forty-eight are caught. Of the first run's forty-three, four got past - each a check no test made, now made:

- **The quasi-static answer's `kept beside the model`**: the tool's test read only the full-wave answer's. The
  quasi-static test now holds it too, and the two are broken apart.
- **The least misfit taken whatever its values**: on exact data the true forms fit best anyway. A capacitor of Q
  in the thousands - which R\|\|C fits to the last digit and C within 2e-3 - is now held to be a capacitor.
- **A frequency of no Y fitted all the same**: the half-wave line was still said far, its inverse's garbage at 10
  GHz notwithstanding. The test now holds that 10 GHz is said left out.
- **Two ports joined by a wire fitted**: a thru has no Y at any frequency, and was refused earlier, for that. Ten
  micro-ohms between the ports - a Y, but too much of it - is now refused as a wire too.

Rebuilt after the breaks: the full ctest (153 tests in 308 s, `test_rf25_measured` 42 s beside the others), the
scenario script (97 checks) and the four lints green, nothing written to the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan, as CI's Linux job is, and solved with Eigen's sparse LU, each function alone:

| Function | Time |
|---|---|
| test_rf25_doc `theSolutionIsKeptBesideTheModel` | 14 s |
| test_rf25_doc `anEquivalentCircuitIsWrittenFromTheSolve` | 15 s |
| test_rf25_doc `theTimeLeftIsSaidRoundly` | 1 s |
| test_rf25_doc `aSolveShowsHowFarItHasGot` | 17 s |
| test_rf25_sparams, the fitter's functions | 0 to 7 s each |
| test_qucs_control `anRf25ModelIsSolvedFullWaveThroughItsTool` | 30 s, with the model reopened by the tools (31 s at 934b0de, before the circuit's calls) |
| test_qucs_control `theToolsAreListed` | 1 s |
| test_pptx_doc `strokesAreReadAsDrawn` | 4 s |
| test_layout_doc `theRawInputDrawsOnALayout` | 10 s |
| test_rf25_measured | skipped there |

Whole, test_rf25_doc took 159 s of its 900 (145 before this round) and test_rf25_sparams 44 s of its 900; on CI's
Linux runner, about twice as slow, both have room.

## Found by the DMG's check

Driving the tools as the DMG's check does, before the DMG was built: a full-wave model solved through `rf25_solve`,
closed, then opened again by `rf25_circuit` or `rf25_export`, was refused - "Nothing is solved full-wave yet:
rf25_solve it first" - though its solution was kept beside it. The two looked for a solve as soon as the model was
open, and a kept solution is taken only once the model's mesh is made, a moment later. Both now wait for the
model's shapes and mesh first, as `rf25_solve` and `rf25_inductance` already did. And `rf25_export` given an
Equivalent circuit node now points to `rf25_circuit` whether the model is solved or not; it said nothing was solved
first. The tool's test holds both - the model closed and opened again by each tool, the node refused before the
solve - and three breaks more (each tool's wait, and the refusal's order) are caught. Its whole suite, 214
functions, green in Release.

## Not done

- **The rest of the assessment's priorities**, not taken this round: the thick-metal model, a GSG calibration with
  pad de-embedding and a multiline option, the coarse-level preconditioner, and drawing shapes and vias in the tool.
- **Five of the six measured structures**: no open source found with the measurement and the whole geometry. A
  structure measured for this suite, or the same layouts in a commercial planar solver, would fill it.
- **The one-second wait before the time left is said** is not held by a test: whether a second has gone when the
  first frequency is done depends on the machine. Nor is each feed's estimate timed from its own start, rather than
  the solve's.
- **The kept solution's size** is the whole answer's, the currents included: a large compressed solve's file is as
  large as what it found.
- **`test_rf25_measured` on CI's macOS runner** has not run yet: its time there is a guess, its function given 900 s
  as the heavy multiphysics ones are.
