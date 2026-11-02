# RF 2.5D, G9's cause, 2 November: a port's own path up from the ground

The user's "Yes, dig into G9", after the phase E check (`2026-11-02-claude-rf25-phase-e-check.md`). G9, from the
reviewer: the raw two-port of a MOM capacitor gave back more than it was given at 100 GHz, and the reviewer put it
down to the ports' path through the ground, which the solve gave no field. Last round answered that it was not the
ports but the stack's second surface wave, found TE1's cutoff exactly, and flagged it rather than curing it. That
answer was wrong and the reviewer was right. Each port's path up from the ground now has the field a via's current
has, and the solve is passive where it gave back.

| Feedback | Now |
|---|---|
| **G9**: the raw MOM two-port gives back 1.61 times what it is given at 100 GHz | **Passive.** A port's own path up from the ground has a field (`PortPath`, `addPortPaths()`). The raw MOM's most returned 1; calibrated, passive as solved, its strips agree to 58.3 GHz (48.8), and the de-embedded two-port gives back 1.04 at most, at 40.7 GHz (1.40 at 23.8), said to be the de-embedding's |
| Last round's diagnosis, "past the stack's second surface wave this solve does not hold power to account" | **Withdrawn.** A strip over vacuum, with no surface wave of any kind, gave back 49 times at 64 GHz. The TE1 line stays, as information: the top of a microstrip's band on the stack, no warning; the passivity line's TE1 variant is gone |
| The phase C check's F6 (1 November): a grounded coplanar line with ports returning through the ground lost a third of the power at 60 GHz | **The same fault.** With their paths those ports lose 0.63 % there, the coplanar return 0.75 % |
| Last round: strips on a thick substrate stop agreeing, "their ports reach one another through the surface wave" | **Mostly the same fault.** On 254 µm of alumina the third strip fits 5 to 10 times better and the line reads -5 dB/m where it read -103 to +235; they still part at 26 GHz, and on 635 µm at 20 |

## Where it was not

The Green's functions first. On 635 µm of alumina, at 38 GHz (below TE1's 39.79) and 42 (past it):

- **The path.** The Sommerfeld remainders taken on half-ellipses 200 and 1000 1/m high and on the default height
  agree to 4e-11 out to 7 mm. The 3000 1/m ellipse parts at 15 mm only because J0 grows by e^45 along that path.
- **The table** agrees with the direct integral to 2e-9.
- **The spectral kernels**, checked by hand:
  - With e^jwt the spectral field of a horizontal current is -[cos² a V_e + sin² a V_h]. Matched to -jw A - grad
    phi, that gives G_A = V_h/(jw) and K_phi = (jw/kr²)(V_e - V_h), which is what `spectral()` has.
  - `lineVoltage()` is Z_up parallel with Z_down at the source, and even in the slab's kz.
  - The ellipse keeps to the proper sheet over the whole first quadrant.

Then two experiments, where the second surface wave cannot be the cause:

| A strip 5 mm long, 600 µm wide, two edge ports | 10 GHz | 28 | 34 | 46 | 58 | 64 |
|---|---|---|---|---|---|---|
| over 635 µm of vacuum: most given back | 1.003 | 1.264 | 1.496 | 1.088 | 1.864 | 3.869 |
| over 2 mm of vacuum | 1.014 | 2.632 | 21.82 | 1.371 | 6.792 | 49.39 |

A conductor-backed coplanar line on the same 635 µm of alumina, its ports returning beside it, so that no port
has a path up from the ground: 0.995 to 0.872 from 34 to 46 GHz, passive past TE1. The kernels were sound. The
ports' paths, which coplanar ports do not have, were not.

## What it was

A ground-referenced port drives its cells with a current that comes up from the ground by a path the solve gave
no field. The header said as much: "a port's own path up from the ground, which carries no field of its own", a
part in 1e4 in vacuum, measured where k h is small. So:

- **The charge** the port brings up had its scalar potential.
- **The current** that brings it had no vector potential.
- **For a vertical current** the vector potential gives 3/2 of its radiation resistance and the charge's scalar
  potential takes away 1/2. Alone, the scalar potential is -R/2: a negative resistance at every port, growing as
  (k h)².

In vacuum each old gain is minus a half of the new loss, as that says:

| Structure | Old gain | New loss | Ratio |
|---|---|---|---|
| A line over 100 µm at 40 GHz | 3.45e-4 | 6.85e-4 | -1.99 |
| A line over 200 µm at 40 GHz | 9.74e-4 | 2.13e-3 | -2.19 |

It looked like TE1 for three reasons:

- **Two near conditions.** On 9.8 and 11.9 the TE1 cutoff, k0 h (eps - 1)^1/2 = pi/2, is within 5 % of k1 h = pi/2,
  where the negative resistance outgrew a line's own losses on both stacks.
- **Loss made it worse,** 16.6 with a loss tangent of 0.01 against 13.1 lossless at 44.5 GHz. A missing loss term
  alone would not do that.
- **The control kept its ports.** Last round's "strip with nothing on it" still had its two, so it never took the
  path out.

The apparent losses below the onset were the same fault. A line on 100 µm of 9.8 under 100 µm of 4 lost 2.5e-3 at
40 GHz. With its ports' paths, meshed finely enough, it loses 3e-4.

## The fix

`PortPath`. Each port column has a path to every cell it drives:

- **From** the ground, or from the cells of the metal an internal port is driven from, shared out as the port
  shares its current.
- **Where it stands:** at the cell's centre, where its current enters the metal, as a via to the ground does.
- **How wide it is:** an edge port's path is a ribbon as wide as the face the cell has. An internal port's is the
  cell's box, clipped to its rectangle.
- **Its current** is the port's, not an unknown of the solve. Its span joins the vias', so `ViaGreen` gives every
  coupling.

`addPortPaths()` adds what it needs to the drive and to the columns' own impedances. The system matrix is not
touched. Both the dense and the compressed solve take Z as C - G^T M^-1 G, so they share the one function.

- **Along the runs:** each path's potential at every run's two ends, which by the formulation's reciprocity is also
  the run's G_zx along the path.
- **Along a via:** the via's G_zz along the path, beside its potential at the path's ends, which the drive already
  had.
- **Between paths:** each pair's G_zz and their potentials at each other's ends, the two ways round averaged.

One thing was found while testing. G_zz goes as 1/eps of its source's slab, so between two vertical currents in
different dielectrics it is the same both ways only with the other's potential added. Half of the G_zz each way
with the potential one way round left a climbing line, a via between 100 µm of 4 and 100 µm of 9.8, giving back
1.3e-6 at 5 GHz. The coupling between a via and a path is now taken the one way round whole.

What changes for whoever reads the answer:

- **Raw ports.** A port's raw impedance now has its path's inductance, part of the port's own discontinuity, and
  Calibrate takes it out. The reviewer's MOM's ports climb 305 µm of silicon. Its raw two-port at 100 GHz passes
  -9.8 dB where it passed -0.25, and its ports' series term is 242 pH where it was -12.6. De-embedded it passes
  -1.18 dB at 100 GHz (-2.49) with |S11| -23.7 dB (-8.2).
- **Internal ports.** One shorted over a via's square is that via.
- **The panel** says how many paths there are.

## Against physics

- **A port's path as a monopole.** A patch 200 µm square, 1 mm over the ground in vacuum, driven by an internal
  port, is a short monopole with a uniform current. Its radiation resistance is 160 pi² (h/lambda)²:
  - 0.157950 Ohm at 3 GHz against 0.158132;
  - 0.631320 at 6 GHz against 0.632530.

  Without the path's field it would have read -0.079.
- **A via's inductance through a port's path.**
  - An internal port shorted over the via's own square reads as the via to 2e-10 nH.
  - Over the whole cell, 50 by 200 µm, the line shorted by the via less by it is 0.033463 nH. That is the closed
    forms' difference to the digit, and 0.5 % more through 200 µm of 9.8.
  - Between two metals, over a square 20 µm across, the difference is within 5.7e-4 of the closed forms, and within
    5e-3 from a thick M1's top face.
  - A joint driven the other way round is the same short to 1e-9.
- **Past TE1.** The strip on 635 µm of alumina returns at most 0.62 of a wave at 42 GHz, and 0.54 with a loss
  tangent of 0.01. Before, it gave back 2.2 at 41.5 GHz, and more again with the loss.
- **Over 2 mm of vacuum** the same strip returns at most 0.946 from 16 to 64 GHz, where it gave back up to 49.
- **The mesh's own limit.** The line on 100 µm of 9.8 under 4 gives back 6.6e-6 at 40 GHz with one cell across.
  Two and four cells across lose 2.4e-4 and 3.2e-4, converging.

Calibration strips of eight heights and twice the width on alumina, with and without the paths:

| Strips on alumina | Third strip's misfit, now | before | Line's loss, now | before |
|---|---|---|---|---|
| 254 µm at 26 GHz | 1.2e-2 | 7.5e-2 | -4.9 dB/m | -103 dB/m |
| 254 µm at 50 GHz | 2.0e-2 | 0.19 | -4.4 dB/m | +235 dB/m |
| 635 µm at 30 GHz | 6.4e-2 | 0.95 | +9.8 dB/m | -127 dB/m |

The reviewer's models through `rf25_solve`, against ab18dfe's own DMG:

| Model | ab18dfe | Now |
|---|---|---|
| MOM, raw | gives back 1.61 at 100 GHz | passive |
| MOM, calibrated | gives back 1.40 at 23.8 GHz; not passive as solved; strips agree to 48.8 GHz | 1.04 at 40.7 GHz, said as the de-embedding's; passive as solved; to 58.3 GHz |
| Microstrip, 1500 µm, calibrated | strips within 0.0031; port -19.8 pH; at 30 GHz S21 -0.121 dB, S11 -33.1 dB | within 0.0011; +13.6 pH; -0.059 dB, -40.5 dB |
| GCPW, coplanar ports | | the same: S21 within 2.4e-13 dB |

The microstrip's de-embedded line is the strips' own line, so a perfect calibration reads it lossless and matched.
It reads nearer that now.

## Tested

| Test | What it holds to |
|---|---|
| `aPortsPathRadiatesAsAMonopole` (`test_rf25_fullwave`, new) | An internal port's path in vacuum: 160 pi² (h/lambda)² within 2 % at 3 and 6 GHz (0.12 and 0.19 % measured) |
| `aStripOverVacuumGivesNothingBack` (new) | Over 2 mm of vacuum, 16 to 64 GHz: passive and reciprocal; the panel names as many paths as the ports drive cells |
| `pastTheSecondSurfaceWaveAStripIsPassive` (in place of `pastTheSecondSurfaceWaveItIsSaid`) | On 635 µm of alumina at 42 GHz: passive, no warnings, the TE1 line among the lines; the loss tangent's loss the more; below TE1, nothing said |
| `aViaToTheGroundIsItsOwnInductance` (rebuilt) | Over the via's square a port's path is the via (1e-4 of it); over the whole cell the closed forms within 1e-3; through 9.8 within 1 %; a pair side by side; an ampere down the via |
| `aViaBetweenTwoMetalsJoinsThem` (rebuilt) | The joint over the via's square is the via; over 20 µm the closed forms within 2e-3, through the dielectrics 5e-3; driven down, the same short; passive in vacuum too; from a thick M1's top face within 2 % |
| `anInternalPortOverARectangleDrivesItsCells` (changed) | A rectangle drives the edge port's cells, within a few thousandths of it and nearer than a point; a 2 µm sliver four times the nearer, the edge port's path a ribbon |
| `aCoplanarPortReturnsThroughItsGrounds` (changed) | Both returns lose under 2 % at 60 GHz |
| `aThirdStripSaysWhereTheStripsAreOneLine`, `aLongStripsPhaseIsFollowedFromBelow` (`test_rf25_deembed`, changed) | The strips of eight heights on 254 µm part at 26 GHz, where shorter ones agree to a third of their misfit; 635 µm at 30 GHz said not one line |
| `anRf25ModelIsSolvedFullWaveThroughItsTool` (`test_qucs_control`, changed) | On 635 µm at 42 GHz the brief answer passive, no warnings, the second surface wave's frequency given |

## Broken on purpose

`2026-11-02-rf25-port-paths/breaks.py` holds twenty:

- **The paths' field:** none at all; their potential along the runs; the vias' G_zz along them; that G_zz halved
  each way round; the paths' own G_zz; their potentials at each other's ends; their own in the dense Z; none in the
  compressed solve.
- **How a path is made:** from the ground whatever metal it is driven from; its current up whichever way; no bottom
  end on the metal it comes from; an edge port's path its cells' boxes; a rectangle's the whole cell's; standing
  10 µm off its cell's centre; its mean distance a square's of its side; not said.
- **The second surface wave:** a warning again; nothing said; its frequency left out of the answer.
- **The strips:** those that disagree said to agree.

All twenty were caught the first time through. Three of them only because tests were added for them while the list
was made:

- **A path's current up whichever way its port drives it.** No test drove a port downward. The climbing line's
  joint is now driven the other way round too, and must be the same short.
- **An edge port's path its cells' boxes.** Nothing told a ribbon from a box. A sliver 2 µm deep along the face now
  has to be the nearer to the edge port.
- **The paths not said.** No test read the panel's line. The vacuum strip reads it, its count the cells its ports
  drive.

The halved G_zz, the fault found while testing, is caught by the climbing line's passivity through its dielectrics
alone.

Rebuilt after the breaks: the full ctest (152), the scenario script (97 checks, 0 failed) and the four lints
green.

## Under the sanitizers

Built Debug with ASan, as CI's Linux job is, and solved with Eigen's sparse LU. Every solve whose ports return
through the ground now builds its ports' paths' vertical kernels at every frequency, beside the horizontal ones,
and under the sanitizers those tables are most of a solve. A strip with no via had no `ViaGreen` before; a metal
solved as its two faces has three spans of them now where it had one. The suites grew accordingly:

| Suite or function | Time |
|---|---|
| test_rf25_fullwave, its functions one by one | 1147 s (487 last round) |
| test_rf25_fullwave through ctest, part 1 and part 2 | 562 s and 603 s |
| `aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent` | 263 s (77 last round): its ports' paths go up to both faces |
| `aViaBetweenTwoMetalsJoinsThem` | 195 s, five solves where Release has eleven |
| `aCompressedSolveIsTheDenseOne` | 194 s |
| `aCoplanarPortReturnsThroughItsGrounds` | 46 s |
| `aViaToTheGroundIsItsOwnInductance` | 42 s |
| `pastTheSecondSurfaceWaveAStripIsPassive` | 28 s |
| `anInternalPortOverARectangleDrivesItsCells` | 22 s |
| `aStripOverVacuumGivesNothingBack`, `aPortsPathRadiatesAsAMonopole` | 8 s, 2 s |
| test_rf25_deembed, all | 731 s (396 last round) |
| test_rf25_deembed `aLongStripsPhaseIsFollowedFromBelow` | 176 s: four sets of strips on 635 µm at 30 GHz |
| test_rf25_deembed `aThirdStripSaysWhereTheStripsAreOneLine` | 95 s |
| test_rf25_doc, all | 273 s, past nine tenths of the 300 a test has unasked: given 900, as the other RF suites have |
| test_qucs_control `anRf25ModelIsSolvedFullWaveThroughItsTool` | 48 s |

What was done about it:

- **The climbing line.** Its eleven solves through two dielectrics ran 872 s, each frequency some 38 s with three
  spans' tables. Sanitized it now makes five: the line at its low frequency alone, the joint over the via's own
  square in vacuum alone, never driven the other way round, and no thick metal. Release runs all eleven, and so
  does macOS CI's job.
- **The full-wave suite in two.** At CI's runner's twice this machine, 1147 s is past three quarters of the one
  entry's 900 times 3. Its functions are split before `aViaBetweenTwoMetalsJoinsThem` into `test_rf25_fullwave`
  and `test_rf25_fullwave_2`, each with its own limit, as `test_fem_quantum` is. The two parts partition the suite:
  15 functions and 12 in Release, every one run once.
- **The de-embedding suite** stays whole: 731 s is half its 900 times 3 at CI's rate.

The heaviest function, the two faces at 263 s, is some 530 s at CI's rate, under QtTest's 900 there. The cure for
all of it is in the solver, not the tests: the Green's functions of one stack at one frequency are built afresh for
every solve, and a calibration's strips and a two-line test's pair build the same ones.

## Not done

- **The Green's functions kept between solves.** Every solve builds its stack's tables at each frequency afresh,
  and since the ports' paths have a field those tables are twice what they were. A calibration's three strips, the
  shorter ones beside them, and a two-line test's pair all build the same ones. Kept by stack, heights, spans and
  frequency, they would win back what the paths cost, here and under the sanitizers.
- **The coarse-level preconditioner** for via-stitched structures, still from phase E's own list.
- **A broad path in a dielectric.** Its G_zz's dielectric part cancels to a part in a thousand of the whole, its
  potential taken across its mean distance: 0.17 pH of a 72 pH path 50 by 200 µm through 200 µm of 9.8. It is in
  the port's own discontinuity, which a calibration takes out.
- **One cell across a strip** leaves a line on a dielectric interface giving back 6.6e-6 at 40 GHz. That is the
  mesh's limit, and two cells across do not.
- From last round, still: **a coplanar port's reach**, and **NIST's weighting** of a multiline calibration.
