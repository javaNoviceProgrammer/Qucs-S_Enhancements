# RF 2.5D: the phase C check of 1 November

The reviewer's live check of phase C (`~/Desktop/qucs-reviews/RF2p5D/2026-11-01-claude-rf25-phase-c-check.md`)
was made on the build of 1 November 07:53, which had everything up to `2e31d40`: the retarded Green's
functions, the full-wave solve, its wiring and vias. Phase D (`45dc14b`, `d1c3b77`) and the phase B check's
close (`ac6ce8f`) came after it. Every finding was reproduced first, on `ac6ce8f`, through `qucs-s
--mcp-server`, isolated as `scripts/mcp-e2e-scenarios.py` isolates its own, on copies of the reviewer's own
models (`~/QucsWorkspace/rf25_check/fw/`).

The check's passes - the static limit, dispersion, the stub's notch, the via's inductance and resistance, the
ports' references, reciprocity, the currents' drawing, the speed - stand as it found them.

## Findings

| Feedback | Now |
|---|---|
| **F1** A non-rectangular polygon loses the stretch: a merged tee (3 mm line, 1.8 mm stub) meshes to 1120 cells and 2035 runs at four across and hits the 3000 limit, where the same two rectangles unmerged give some 370 cells. "Splitting a merged polygon into its strips before meshing would make ordinary layouts (tees, bends, stubs) fit" | **Fixed, and reproducing it found worse.** Every conductor whose sides all run along x or y is now cut into the rectangles it is made of before it is meshed: slabs between its corners' heights, across y or across x, whichever gives fewer. So a tee is its line and its stub, an L its two arms. Each rectangle is meshed as a strip of its own, stretched along its long side. **The unmerged pair, the check's "join correctly", met through one run of current**: two grids laid independently share an edge only where two of their lines happen to coincide, and of 360 runs, one crossed from the line into the stub. The notch came out near enough because one cell's edge carried all the current. Now, where two rectangles meet - of one conductor, or two shapes left unmerged - each takes the other's grid lines along the stretch they share, so every cell of the stub's foot shares a whole edge with a cell of the line: five runs across the joint at three across, six at four. A side wholly met by another rectangle is no edge, and is not graded as one. Merged or not, the tee is now the same 342 cells and 621 runs at four across, where it was 1120 and 2035 and refused; at three across 230 and 409, where the check's merged tee was 694 and 1233. |
| **F2** Quasi-static on a tee reports "the line" anyway: Z0 31 Ohm, a quasi-static limit from "the conductor is 1900 µm across", and "narrowest conductor 97.96 µm" for 100 µm strips. "A shape that is not a uniform strip should get no per-metre line, or a warning" | **Fixed: no line.** A layout that is no uniform line now has no values per metre at all: no Z0, eps_eff or RLGC, no coupled modes, and no quasi-static S parameters or Touchstone, which were a line of its length. Its capacitance stands whole, and the full-wave solve is named for its ports' S. It was labelled "Not a uniform line ... the whole over its length", which is how the check read past it: the tee's Z0 was still given, and its S as a 3 mm line. `rf25_solve` gives each net "line not given" with why; the tab says "No uniform line, so no S parameters as one"; the export refuses with the same reason. What is no line has no width, so its quasi-static limit is set by its longest extent: "the layout spans 3000 µm at its longest, and is no line", 3.2 GHz, where the stub's 1900 µm said 5.0. The narrowest is now 100 µm, F1's rectangles' short sides; 97.96 was twice the tee's area over its perimeter. |
| **F3** The full-wave wiring's write-up says the export "renormalises every port to its own impedance"; it renormalises all to the one reference, as the file's comment says | **Corrected** where it stands, struck through and dated. Touchstone 2.0, from `45dc14b`, writes each port into its own. |
| **F4** (understanding) The two-line alpha of a matched line falls from 31 to 15 dB/m between 20 and 30 GHz; the port ends radiate, and two lengths do not take the ends' fields out. At 10 GHz the full wave's conductor loss is 15 % below phase B's Wheeler R | **The reading is right**, and F6 is its cure where the line allows: on the check's coplanar line, with coplanar ports, the two-line alpha is 0.06 to 5.6 dB/m all the way to 60 GHz, where it was 6 to 1650. A microstrip's edge port's own discontinuity is taken out by phase D's calibration strips (`45dc14b`). The conductor loss's 15 % is the sheet's: the full wave carries a line's current on one sheet with the surface impedance of its thickness, phase B on the thick cross-section's every face by Wheeler's rule. Known, and not changed here. |
| **F5** (known) The solution is not kept with the tab: a tool solve of a closed model, or a disk reload, leaves the tab unsolved | **Parked**, as the user parked it. Within a session a tool solve does leave the tab solved: `rf25_export` straight after `rf25_solve` writes it, from the tab. A reopen, or a reload of a file changed on disk, solves again. |
| The open end of a plain line reads some 22 µm short of port plane plus Kirschning-Jansen-Koster; "probably the port's half-cell reference plane ... worth a look in phase D's de-embedding" | **It was.** Phase D (`45dc14b`) takes the port's own discontinuity out: de-embedded, an open line's end extension is 30.96 µm against Kirschning, Jansen and Koster's 31.73 (−2.4 %), on 100 µm of 9.8 at six cells across (29.6, 31.0, 31.5 µm at four, six and eight). |
| **F6** (major for CPW users) The edge port's return through the backside ground radiates: 1 mm of grounded CPW, |S21| −2.35 dB at 60 GHz where QFEM's mode gives −0.08; the 1.9 mm line loses less than the 1 mm one, so the loss is at the ends. "A GSG port ... is what a CPW needs" | **Added: an edge port's Return.** "Through the conductors beside it (ground-signal-ground)" drives the port's face against the faces beside it on the line of its edge, as a probe's grounds touch them: a column of the drive against each, the columns side by side - one voltage, their currents added - so the two grounds are tied together and the solve shares the current out between them. The ports' Z is `(Bᵀ Z⁻¹ B)⁻¹` of the columns' Z, B which port each column is of. On the check's own 1 and 1.9 mm lines, perfect metal and four across: 1 mm loses **0.5 %** of the power at 60 GHz where it lost 35 % (|S21| −0.18 dB, was −2.27), 1.9 mm 0.4 %, and both are passive and reciprocal. A ground on one side only is a ground-signal probe's. A port with none beside it is refused, saying so, rather than solved as something else. The model, the tab, `rf25_solve` and its description carry it. Calibrating a coplanar port is not done: one that asks keeps its own discontinuity, and says why. |
| **F7** CPW eps_eff about 2 % high: 5.647 at four across, 5.687 at eight, heading to some 5.73, against QFEM's zero-thickness 5.60-5.64 and Ghione-Naldi's 5.577. "Part may be the end radiation contaminating the two-line phase" | **It was all of it.** With coplanar ports the check's line, discrete fences and perfect metal, converges from below: see the table. Ground-plane ports converge to the check's 5.69; coplanar ones to about 5.556, 0.4 % under Ghione-Naldi and 0.8 to 1.4 % under QFEM's zero-thickness range. The ground-plane ports also put a false dispersion into the two-line phase: +0.045 from 2 to 10 GHz, against +0.004 with coplanar ports. Copper's further rise with refinement is the metal as a sheet, the known item. |

### F6 and F7 in the check's own lines

The check's 1 mm and 1.9 mm grounded CPW: 100 µm signal, 50 µm slots, 250 µm grounds on 200 µm of 9.8, perfect
metal, two-line between the two lengths.

| | Ground-plane ports | Coplanar ports |
|---|---|---|
| 1 mm at 60 GHz, power lost (walls, four across) | 34.9 % | **0.5 %** |
| 1.9 mm at 60 GHz | 18.1 % | **0.4 %** |
| two-line alpha, 10 to 40 GHz | 6 to 195 dB/m | **0.06 to 2 dB/m** |
| eps_eff at 10 GHz, fences, four across | 5.645 | 5.514 |
| six across | 5.685 | 5.548 |
| eight across | 5.691 | **5.553** |
| eps_eff from 2 to 10 GHz, eight across | +0.045 | +0.004 |

## What reproducing found

- **Touching shapes left unmerged were two nets.** The quasi-static solve gave the unmerged tee "two coupled
  conductors" with modes of their own, the line and its stub. Shapes of one metal that touch or overlap are one
  piece of it, merged or not: they are one net now, and the unmerged tee's numbers are the merged tee's.
- **A continuous via wall at eight cells across** gives back up to 3 % with coplanar ports, in a dielectric, and
  the solve's own passivity check says so. Discrete fences at eight across are passive, walls at six are, walls
  in vacuum are, and walls with ground-plane ports are, where the ports' radiation would hide it. The port's own
  share of the vias' correction is not it: taken out for a coplanar port, it was still not passive. A wall is
  many touching via cells, whose correction to each other is taken at their centres and no nearer than their
  own mean distance: enough for vias apart, not for a wall of them. Said, and not done here.
- **The mesher's lines** for a layout of a strip, a tee and an L are in `test_rf25_mesh`; the cut into
  rectangles is said in the mesh's own lines ("cut into the 2 rectangle(s) they are made of, each meshed as a
  strip of its own").

## Tested

| Test | What it holds to |
|---|---|
| `aTeeIsItsStripsMeetingEdgeToEdge` (`test_rf25_mesh`) | A tee merged and unmerged: the same cells, under 400 at four across; its narrowest 100 µm; every edge along the stub's foot shared by a cell of each side, the whole 100 µm of it; the foot not graded and the open end graded |
| `aConductorHasCellsAcrossIt` | An L cut into its two 20 µm arms, stretched along them; a shape with a slanted side by twice its area over its perimeter, as before |
| `touchingShapesAreOneNet` (`test_rf25_static`) | Two touching rectangles one net, their capacitance the merged tee's; a hair apart, two |
| `whatIsNoUniformLineIsSaidToBe` | An L, a taper, a step and a tee: no line, no values per metre, said; the capacitance whole |
| `theQuasiStaticLimitIsSaidRatherThanAssumed` (`test_rf25_sparams`) | What is no line by its longest extent, "is no line", no resonance |
| `aCoplanarPortReturnsThroughItsGrounds` (`test_rf25_fullwave`) | The check's CPW at 60 GHz: 36 % lost through the ground, under 3 % (0.75 % measured) through the conductors beside it; passive, reciprocal; said; an ampere in at 10 GHz going out of the signal's face and back alike into each ground's, to a part in a thousand; a ground on one side a ground-signal probe's; none beside refused |
| `aCoplanarPortIsReadAndSaysWhatItCannotHave` (`test_rf25_doc`) | The edge port's Return read by the tab; a coplanar port that asks to be calibrated says why it is not; a lone strip's refused |
| `whatIsNoLineHasNoSParameters` | The heater's two pads: "No uniform line" with the limit of what is no line; the Touchstone refused, and no modes per metre |
| `anRf25ModelIsSolvedFullWaveThroughItsTool` (`test_qucs_control`) | A coplanar port on a lone strip refused through `rf25_solve` |
| `anRf25ModelIsSolvedThroughItsTool` | Two nets as coupled lines, now of the heater's two strips: its pads made one of them no uniform line, which was given modes |

## Broken on purpose

`docs/feature_gaps/2026-11-01-claude-rf25-phase-c-check/breaks.py` holds sixteen:

- **F1:** no conductor cut into its rectangles; the slabs that give more of them; the lines not shared where two
  meet; a joint graded as an edge.
- **Nets:** touching shapes of one metal left two nets.
- **F2:** what is no uniform line said to have values per metre; its limit set by its width; modes per metre of
  it; the tab's S of it unsaid.
- **F6:** the Return not read; the conductor beside a port on one side not found; a coplanar column without its
  ground's share; a coplanar port's currents left in its columns; its grounds not tied, its first column alone;
  the solve's lines silent on it; a coplanar port calibrated as though its feed were a strip.

## What the breaks taught

- **Fifteen of the sixteen were caught at once.** The one that was not chose the slabs that give more
  rectangles: across x a tee is three, its line in two halves and a column through it. That meshes as well,
  edge to edge, and by chance to as many cells, so the test could not tell. What the choice decides is how
  many rectangles there are, and so how many lines are forced where they meet. The test now holds a tee to
  its line and its stub, two, and run again the break is caught: sixteen of sixteen.
- **The current's share between the grounds** wants a test of its own, since a port's S does not show where
  its current went. Its first form was wrong, and the breaks hid it. It summed the current across a fixed line
  80 µm in from the port, and on the correct solve that read 0.92 A along the signal and nothing on either
  ground: the grounds' first cells are longer than 80 µm, so no run of theirs crosses the line. It had never
  run in the Release build before the breaks - that build had stopped at another test's compile error, and the
  test that "passed" was the one before it - so a coplanar port's currents left in its columns failed it with
  the same 0.92 and nothing, and was counted caught. The sanitized build failed it on the correct solve, which
  is how it came out. It now sums what leaves each face's own cells: 0.924 A out of the signal's, 0.368 A into
  each ground's, the two alike to the digit - the rest of the ampere charges the faces' cells, and on a ground
  goes down the fence's first via, which stands on its face. The currents left in the columns now give 0.634
  and 0.101 into the two grounds, and are caught by that. The grounds not tied, the port its first column alone,
  is caught by the power lost: 4.4 % at 60 GHz, against the 3 % allowed and 0.75 % measured.

The full ctest (149) and the scenario script (97 checks) were green on the code as committed, both lints too;
the tests changed after that run were run again on their own. Under the sanitizers, with Eigen's sparse LU as
CI's Linux has it, the new functions take 0.01 to 19 s - the coplanar port's 19 - and the suites they are in
6 s (mesh), 101 s (static), 41 s (S parameters), 174 s (full wave) and 77 s (the tab).

## Not done

- **Calibrating a coplanar port.** Its strips would have to be of the coplanar feed, its grounds and their vias
  with it. A plane can be moved along a given line.
- **A continuous via wall at fine meshes** (above): many touching via cells.
- **A shape with a slanted side** meets another where their grid lines happen to coincide: rectangles are cut
  and share their lines, slanted shapes are meshed whole, as before.
- **Keeping the solution with the model**, parked by the user.
- **The metal as a sheet**: copper's rise in a CPW's eps_eff with refinement, and F4's conductor loss.
