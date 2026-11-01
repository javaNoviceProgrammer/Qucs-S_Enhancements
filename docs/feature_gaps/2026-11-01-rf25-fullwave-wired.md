# RF 2.5D, phase C: the full-wave solve wired in

`37cbe8f` built the full-wave solve as a library: points on faces, one reference impedance, nothing in
the tab calling it. The user said "Wire it in" (1 November). A model whose study's Solver is "Full wave
(MPIE)" is now solved full-wave by the tab's Solve:
- at each of the study's frequencies;
- driven at the model's own ports;
- with the ports' S in the Results node, the notes, the status line, the Touchstone export and the MCP
  tools;
- with the surface current density drawn over the layout.

The quasi-static extraction is unchanged and remains the study's default.

## What it does now

- **The study's Solver chooses.** Solve runs the extraction or the full-wave solve as the study says, and
  a tool may ask for the other for one solve without editing the model. One of the two answers is kept,
  never both; an edit throws it away, as it always threw the extraction away.
- **The ports are the model's own.**
  - **An edge port** keeps to the metal it names, faces the way it says (Outwards, +x, -x, +y, -y),
    drives the whole face, and is referred to its own reference impedance. The face is the edge whose
    outward normal points the way given, within 45 degrees, wherever the polygon is wound. A port at a
    corner then drives the face it says, and is the same port as one in the middle of that face, to
    1e-12.
  - **An internal port** drives the cell under its point on its "to" metal, against the ground or
    against the cell under it on its "from" metal.
  - **A de-embedding** is not done: calibration lines are the next phase's. A port that asks for one is
    solved at its face, and the Results say so.
- **Each port into its own reference.** S is Kurokawa's power waves, `R^-1/2 (Z - R)(Z + R)^-1 R^1/2`.
  With references that differ, the inverse must be on the right. Touchstone 1.1 has one reference, so
  the export renormalises every port to its own impedance and says so in the file.
- **The frequencies side by side.** Each frequency is solved on a thread of its own, sharing the static
  parts, as many at once as the machine has and the memory allows: 1.5 GB for the systems in flight. The
  answer is the same bit for bit on one thread as on three.
  - The tab counts them: "Solving full-wave: 12 of 41 frequencies…", in the notes, the status line and
    the Results node.
  - Stop stops the sweep at its next frequency, and so does closing the tab. Before, the solve ran on to
    its end and was thrown away.
- **The Results** give the ports and their references, every term of S at the top of the sweep (for
  more than four ports, the first column and the reflections), and two checks the proposal asks for and
  says are the first sign of a bad port: whether Z is symmetric, and whether S ever gives back more than
  it is given.
- **The currents** are drawn on the layout. Each cell is filled by its surface current density, in
  decibels over three decades below the largest, in 24 bands of viridis, with a scale in the corner.
  - The current density is each cell's runs averaged along each direction.
  - The Currents button turns the drawing on, and the Results node chooses the port and the frequency.
  - A port is driven as a circuit drives it: a source of its own reference impedance sends a wave of a
    watt in, and the other ports are ended in theirs. `currentDensity()` also gives the ampere-driven
    case, with the others open. On a line that shows the far end as an open end, with a standing wave no
    circuit puts on it.
- **The Touchstone** of a full-wave solve is its ports' S, an N-port in the order of the Ports. Its
  comments carry what each port drives and where its reference plane is. "Of" chooses among a
  quasi-static solve's lines.
- **The MCP tools.**
  - `rf25_solve` takes `solver`. A full-wave solve returns each port with what it drives and its
    reference, S (real, imaginary, dB, degrees) and Z at the top frequency or the one nearest
    `frequency`, every term of S in dB over the sweep, and the two checks. Given `frequency`, it solves
    that one frequency alone; `brief` cuts the answer down.
  - `rf25_export` writes the ports after a full-wave solve.
  - `rf25_inductance` asks for the quasi-static solve whatever the study's Solver: it drove the tab's own
    Solve, which in a full-wave study would have found no loop.
- **The layout view** fills an overlay where it is given a fill, and draws a scale where it is given
  one, in the top right corner where the scale bar is not.

## What it agrees with

| Case | Reference | Measured |
|---|---|---|
| An edge port at a strip's corner, facing -x and facing -y | A port in the middle of the end, and of the side | Z the same to 1e-12; 6 cells and 14 |
| An edge port at a point on the ends of two strips, one on M1 and a wider one on M2 | The strip it names | 6 cells on M1, 8 on M2 |
| An internal port to the ground in the middle of a strip, at 100 MHz | Phase B's C of the strip | −3.1e-4 |
| An internal port between two plates on M1 and M2 | (Caa Cbb − Cab²)/(Caa + Cbb + 2 Cab) of phase B's Maxwell matrix: two conductors with no charge between them | 5.5e-5 |
| S into 50 and 25 Ohm | The two-port's closed form of the power waves | 1e-14 |
| A sweep on one thread and on three | Itself | bit for bit |
| The current density across the middle of a line at low frequency, the far port open | Half the ampere, by symmetry | 0.50000 A |
| The same line driven by a 50 Ohm source, the far port ended in 50 | A short wire at 100 MHz: 2 sqrt(50) / (50 + 50) all along it | 0.141421 A, to six digits |
| A non-reciprocal two-port's Touchstone | S11 S21 S12 S22, Touchstone's order | exact |
| A microstrip of 1 mm, 50 µm wide on 100 µm of 9.8, through the tab at 1, 3 and 5 GHz | Reciprocal and passive; it passes most of a wave | both checks pass; \|S21\| above 0.8 |

## What building it found

- **`rf25_inductance` would have found no loop.** It drove the tab's own Solve, which now follows the
  study's Solver: in a full-wave study the loop was never solved. It asks for the quasi-static solve.
- **A face's outward normal is not the right-hand one.** A layout's polygons are wound either way. The
  normal is now the one whose side of the edge's middle is outside the shape.
- **A port at a corner was a tie** between the two edges that meet there, broken by the polygon's order.
  The facing is what breaks it now, and the test pins it by Z.
- **The first picture of the currents drove each port by an ampere with the others open.** That is
  what Z is made of. But it showed a line's far end open, with a standing wave that no circuit puts on
  it. The drawing now drives as a circuit does, a source of the port's own reference with the other
  ports ended in theirs: across the middle of a short line, the 0.141421 A that 2 sqrt(50)/100 says.
- **The first picture of the currents was in steps, and its scale hid the scale bar.** Eight bands over
  60 dB looked like steps; there are 24 now. The scale was in the bottom left corner, where the view's
  scale bar is; it is in the top right.
- **The MCP suite's RF tests each waited a minute.** `test_qucs_control` passed ctest's 300 s in the full
  run. Its three RF functions took a minute each. That was not their work, which is under a second each,
  but `closeAllFiles()`: it asked whether to save an earlier test's untitled schematic, and waited a
  minute for an answer that never came. They close their own documents now, and the suite went from 320
  s to 135 s.
- **`rf25_solve`'s description passed the tool list's cap** once it described both solvers: 2752
  characters against 2048 (`theToolsAreListed`). It is 1979 now. The timeout had hidden the failure
  until the suite was run alone.
- **The frequencies' answers were first gathered in a QList.** Its implicit sharing is no place for
  several threads to write side by side, so the answers go into a `std::vector`.
- Two slips in the tests themselves:
  - a point on the ends of two strips that met there in plan, where both strips' edges tied;
  - a column of cells sought across the middle of a line, where the cells meet exactly. The mean of the
    two columns either side is the half by symmetry.

## Tested

| Test | What it holds to |
|---|---|
| `itsPortsAreTheModelsOwn` | Facing at a corner, by cells and by Z; the metal named; an internal port to the ground and between two plates, against phase B; refusals of an internal port on no cell and against its own |
| `eachPortIsReferredToItsOwnImpedance` | The power waves against the closed form; one reference the old formula; a solve's S of its ports' own; a reference of nothing refused by the port's name |
| `aSweepIsSolvedSideBySideAndCanBeStopped` | One thread and three alike; the frequencies counted; the checks; stopped before it began |
| `theCurrentDensityAndTheTouchstoneAreOfTheSolve` | The half ampere across the middle, the far port open; the matched drive's 2 sqrt(50)/100; the driven end brighter than the open; the Touchstone's header, order, values and wrapping |
| `aFullWaveStudySolvesThePortsAndDrawsTheCurrents` (`test_rf25_doc`) | The tab's Solve of a full-wave study: S, the summary, the notes, the status line and the Results node; the currents drawn of a port and frequency chosen, with their scale; the export; the other solver asked for once; one frequency alone; a stop; an edit throwing it away; a de-embedding said; facing +y and a metal that is not there; no ports |
| `anRf25ModelIsSolvedFullWaveThroughItsTool` (`test_qucs_control`) | `solver` full_wave at one frequency: ports, S, Z, sweep, checks; the model left as it was; `brief`; `rf25_export` of the ports; a solver that is none; a full-wave study followed; `rf25_inductance`'s loop |
| `overlaysAreDrawnOverTheLayoutWithinTheirBudget` (`test_layout_doc`) | A fill painted where its shape is; the scale's colours in the top right corner |

## Broken on purpose

`docs/feature_gaps/2026-11-01-rf25-fullwave-wired/breaks.py` holds twenty-six breaks:

- a face's inward normal taken for its outward one;
- an edge port's metal not kept to;
- an internal port between metals driven against the ground;
- the references left out of the power waves, and their inverse taken on the left;
- every thread writing one slot;
- the stop not heeded;
- the frequencies not counted;
- a cell's current density summed rather than averaged;
- a two-port written S11 S12 S21 S22;
- rows wrapped three to a line;
- the matched drive's source and ends left out of its loop, and its source an EMF of a volt;
- the tab drawing the ampere-driven currents;
- the document never full-wave;
- a solve kept past an edit;
- the export writing lines;
- the currents not drawn;
- a de-embedding passed over in silence;
- an edge port's metal, and its facing, not read;
- `rf25_solve`'s `solver` not read;
- `rf25_inductance` solving by the study;
- `rf25_export` refusing a full-wave solve;
- the view's fill not painted, and its scale not painted.

## What the breaks taught

- **All twenty-three were caught at once.** So were the three of the matched drive, added after it:
  twenty-six of twenty-six.
- The one written and left out is a Stop that does not tell the sweep to stop. Its result is dropped
  either way, so nothing the tab shows can tell, and the solver's own test holds the stop instead.
- The full ctest (148), the scenario script (97 checks) and both lints were green. The MCP suite and the
  scenarios were run again after the two fixes above. Under the sanitizers the solver's suite takes some
  85 s, no function more than 31; the tab's new test takes 13 s and the tools' 6.

## Not done

- **De-embedding.** Calibration lines, or a given Z0 and γ, are phase D's. Until then the reference
  plane is at the centres of the cells a port drives, and the Results say so where a port asks for more.
- **Touchstone 2.0** and its per-port references: the export writes Touchstone 1.1, every port into one.
- **Adaptive frequency sampling**, which is phase F's. The study's points are solved, and the Results say
  so when the study asks for more.
- **Vias**, the ground's loss, and the metal as more than a sheet, all as before.
- **Keeping the solution with the model.** Reopened, it is solved again; this is the item already parked.
- **What a Stop does to the work behind it** cannot be seen through the tab: its result is dropped either
  way. That the sweep stops is the solver's test, `aSweepIsSolvedSideBySideAndCanBeStopped`.
