# RF 2.5D phase E check, 2 November: a feed's calibration on a thick substrate, and the second surface wave

The reviewer's live check of phase E and of the 1 and 2 November fixes,
`~/Desktop/qucs-reviews/RF2p5D/2026-11-02-claude-rf25-phase-e-check.md`, with its addendum on a MOM capacitor's
two-port, written into `2026-11-01-claude-rf25-gssg-coupled-modes.md` the same day. Phase E passed: dense and
compressed alike to 6e-7, a patch of 78 000 unknowns in 53 s, the GSSG's even and odd modes and the two-face
metal as the reviewer's own reductions have them. What was left open:

> **Corrected the same day** (`2026-11-02-rf25-port-paths.md`): G9 was the ports' own paths up from the ground, as
> the reviewer said, and not the second surface wave. What follows of G9 below is withdrawn.

| Feedback | Now |
|---|---|
| **G8 (major)**: on a feed over 305 µm, the calibration line reads eps_eff 8.38, 211 Ohm and 1982 dB/m at 100 GHz, the port's discontinuity +197 pH, and the de-embedded two-port gives back 16.8 times what it is given - the raw one is passive; and the answer says nothing | The strips are checked by a **third**: where they are not one line between two ends alike it is said, from where, and by how much. Where they are not, **shorter strips** are solved beside them and each frequency taken from the longest that agree. A port's **feed shorter than its own field reaches** is said, and a de-embedding that **gives back where the structure does not** is said to be the de-embedding's. All in the answer's new `warnings`, the brief one too, and on each port. On the reviewer's MOM: the strips agree to 48.8 GHz (the chosen ones alone to 23.8), the de-embedded two-port gives back 1.40 at most (16.8), and six warnings say why it is still not to be trusted - first that its 50 µm feeds are far shorter than the 1230 µm their ports' field reaches |
| G8: "a multiline / short-strip option for thick substrates" | Both: the third strip and the shorter strips beside the first, each frequency from the strips that agree best; and **Calibration strips**, a port's own length for them |
| **G9**: the raw two-port non-passive at 100 GHz, put down to the ports' path through the ground having no field | It is not the ports: **past the stack's second surface wave this solve does not hold power to account**. A strip with nothing on it, 4 mm on the MOM's stack, is passive to 75.8 GHz and gives back 6 times what it is given at 80.9; the stack's TE1 is cut off at 75.3. On 635 µm of alumina, passive to 39.6 GHz and 2 times at 40.8, its TE1 from 39.8. Said now - the stack's TE1 found exactly, a warning where the sweep passes it, the passivity line naming it - and not yet cured |
| Note: iterations grow fast on via-stitched structures (573 at three cells per finger) | Not done here: the coarse-level preconditioner phase E's write-up leaves open |
| Note: `matrix` is not in that session's rf25_solve schema | The session's tool list predated the build: rf25_solve lists `matrix` (the DMG's own tool list, checked) |
| Note: a lumped L from the MOM on lossy silicon depends on the frequencies fitted | Not the solver's, as the reviewer says |

## G8, reproduced

`mom_100_2p_cal_hiR.qrf25`, copied: a 100 µm MOM of two metals over 4 µm of oxide on 300 µm of insulating silicon,
a 10 by 50 µm feed of M2 off each bus, an edge port at each feed's end returning through the ground, `calibrate`,
`deembed: 50[um]`. The answer read exactly as the reviewer has it: the line 211.3 Ohm, eps_eff 8.385, 1.98e3 dB/m,
the discontinuity 1.97 fF and 197 pH at 100 GHz, `most returned` 16.78, and of its trouble only "Not passive - look
at the ports and the mesh".

Two strips cannot say whether they are right: `T_2l = E L(l)^2 E'` holds by how `E` and `L(l)` are found from them.
A third, its line phi = 1.618 times the shorter's, can: de-embedded with the `E` the two found it should be the line
over its own length, and how far it is from that is a residual at each frequency. A probe solved one beside the
feed's two:

| Feed | Strips | Residual |
|---|---|---|
| The MOM's, 10 µm over 305 µm of silicon | 2460 / 4920 µm, the chosen | 1e-4 to 13 GHz, 1e-2 by 20, 0.16 at 26, 0.3 to 2 above 45 |
| | 1230 / 2460 | 5e-4 to 20 GHz, 3e-2 at 32, 1 at 50 |
| | 800 / 1600 | 1e-3 to 32 GHz, 0.18 at 50 |
| | 400 / 800 | 4e-3 to 32 GHz, 0.016 at 50, 0.08 at 79 |
| 100 µm wide over 100 µm of alumina | 1000 / 2000 | 1e-4 to 20 GHz, 1e-3 at 26, 2e-2 at 35 |
| 250 µm wide over 254 µm of alumina | 2532 / 5064 | 2e-4 to 7 GHz, 1e-2 at 15, 0.04 at 20 |

(The probe's residual was the first measure, its chain matrices' terms in ohms and siemens side by side; the
solve's own takes the line's `B` in its impedance and `C` in its admittance, below.) Two things are in it. A floor,
the higher the shorter the strips - their ports' near fields reaching each other. And a frequency from which
it climbs, the lower the longer the strips - the ports at a strip's two ends reaching one another past the line,
by the substrate's surface wave, at about a twentieth of a wavelength of substrate on all three stacks. Past it the
line two strips read is not the line: on the MOM's feed 330 to 660 Ohm and up to 3500 dB/m above 45 GHz. The
reviewer's de-embedded two-port first gave back at 12.7 GHz, where the longer strip is its first half wave.

## What changed

**A third strip** (`rf25_deembed`). Each calibration solves three strips: the shorter, the longer of twice its line,
and the third of phi times it - with rows of vias, a whole number of their periods, never the shorter's or the
longer's count. `E` comes from the shorter and the longer as before. The third's `E^-1 T_3 E'^-1`, and the
shorter's, are held to the line the two read, each term of the difference over the largest of the line's chain
matrix, `B` taken in the line's impedance and `C` in its admittance: `Calibration::residual`, at each frequency. A
calibration agrees to the highest frequency below which every residual is within a percent (`kStripsAgree`).

**The impedance by a half wave.** Where the shorter strip is a whole number of half waves long its `L(l)` is all
but the identity, and gave the impedance poorly: on the MOM's feed 124 Ohm at 26 GHz among 145s. Within a quarter
turn of that the third's impedance is taken, where the third is the further from one; elsewhere the shorter's, as
before. (Taking the third's wherever it was the better conditioned - every low frequency, where the longer strip
always is - moved a de-embedded thru's |S11| from 1e-9 to 1.6e-5: the shorter's is the line its `E` was found at.)

**Shorter strips, where they do not agree.** Where the chosen strips disagree by more than a percent at any
frequency, strips half, a quarter and an eighth as long are solved too, until every frequency has strips that agree.
Each frequency is taken from the longest that agree within a percent there - their ports the furthest apart, the
floor the lowest - and else from those that agree best. On 254 µm of alumina strips of 2532 µm agree to 12 GHz and
with 1266 µm strips beside them to 20 GHz (6e-3 there against 1.3e-2); on the MOM's feed 2460 µm strips to 24 GHz,
with 1230, 615 and 308 µm strips to 49. A length given is solved as given: a port's **Calibration strips**.

**A feed shorter than its port's field.** A structure's own feed must run straight as far as its port's own field
reaches - the strips use eight heights and twice the width for two ports, so four and once for one - for the port
there to be what its strips find. The full-wave solve now measures each edge port's feed (`Feed::length`): in from
its face, as wide as the face, until its metal widens or ends or another conductor lies over or under it - a
coplanar feed's grounds beside it no widening. Where a calibrated port's feed is shorter than `4h + w` it is said,
for every frequency: the MOM's feeds run 50 µm, their ports' field 1230. No strip length cures that.

**A de-embedding that gives back.** Each frequency's de-embedded two-port is held beside the structure as solved
there; where the de-embedded gives back the most and the solve gives back less than half of it, it is said to be the
de-embedding's: the MOM's 39.9 % at 23.8 GHz, where as solved it gives back nothing.

**Said, and kept apart.** Everything that makes an answer not to be trusted goes into the full-wave solve's
`warnings`, and among its lines: rf25_solve's answer has them as `warnings`, the brief answer too, and each port's
`de-embedded` the third strip, up to where the strips agree, the most they differ and where, the shorter strips
solved, how far the feed runs straight and how far its field reaches. `checks` says where it gives back the most.

On the reviewer's validated lines nothing that was right moved: the 1 mm GCPW calibrated at eight across, |S11|
-57.65 dB and |S21| -0.07 dB at 60 GHz as before, its whole sweep's S21 within 0.0017 dB (S11 0.28 dB at -37 dB, at
35 GHz where its shorter strip is a half wave and the third's impedance is taken); the 1.5 mm microstrip the same to
the digit. The GCPW's three strips agree within 0.3 % over its sweep. A calibration costs a third strip more: the
GCPW 117 s, 103 before.

## G9: past the stack's second surface wave (withdrawn: `2026-11-02-rf25-port-paths.md`)

The reviewer put the raw two-port's giving back at 100 GHz down to the ports' path through the ground, which the
solve gives no field. A strip with nothing on it says otherwise. The calibration's own third strip on the MOM's
stack, 4 mm of the feed and its two ports, swept across:

| GHz | 70 | 74.8 | 75.8 | 76.8 | 77.8 | 78.8 | 79.9 | 80.9 | 82 |
|---|---|---|---|---|---|---|---|---|---|
| Most given back | 0.66 | 0.73 | 0.75 | 0.83 | 1.00 | 1.41 | 2.93 | 5.96 | 1.52 |

300 µm of silicon over a ground carries its second surface wave, TE1, from c / (4 h (eps - 1)^1/2) = 75.67 GHz; the
whole stack from 75.27. On 635 µm of alumina, TE1 from 39.79 GHz, a strip 10 mm long: 0.57 at 39.6 GHz, 2.08 at
40.8, 14.6 at 46. Both passive below, both active just past it. Sixty-four arcs of the integration's ellipse in
place of sixteen moved the numbers and not the onset (2.69 at 79.8 GHz): not the integral's steps. Something in how
the solve takes a second surface wave - a pole that has just left the branch point at k0, its field reaching far
into the air - does not hold power to account, and it is not found yet.

What is done: `secondSurfaceWave()` finds where a stack begins to carry its second surface wave, by transverse
resonance - the TE chain matrix of the stack at grazing incidence, its `A` nought over a ground (a quarter wave in
one slab), its `C` nought without one (a half wave) - exactly: silicon under 20 µm of oxide 74.3508 GHz, the two
slabs' own equation's root. A full-wave sweep that reaches it says, among its lines and warnings, that what it gives
from there is not to be trusted; the passivity line names it where the most given back is past it; the answer
gives `second surface wave from`.

## Tested

| Test | What it holds to |
|---|---|
| `aThirdStripSaysWhereTheStripsAreOneLine` (`test_rf25_deembed`) | On 100 µm of alumina the three agree within 3e-3 to 20 GHz (2e-5 to 4e-4), the third phi times the shorter, only the strips asked solved, said. On 254 µm the chosen strips disagree past a percent at 20 GHz; shorter ones are solved, agree within half as much there and as far up at least, the lowest frequency kept on the longest; a length given solved as given |
| `aHalfWaveTakesItsImpedanceFromTheThird` | Strips of 3 mm on 100 µm of alumina, a half wave at 19.47 GHz: the impedance there within a percent of either side's, said |
| `aFeedShorterThanItsPortsFieldIsSaid` | A feed into a pad measured 50 and 600 µm to the step; the reach 500 µm; said for the 50 µm feed alone; a straight line's feed as long as looked |
| `aDeembeddingThatGivesBackIsSaid` | A line taken out through a discontinuity that loses, which it does not: gives back, where, said as the de-embedding's, among the lines too; through none, nothing said |
| `aCoplanarPortsFeedIsItsGroundsAndItsVias` (added to) | A coplanar feed's grounds and their vias no widening: its feed the line's whole 1300 µm |
| `theSecondSurfaceWaveIsTE1sCutoff` (`test_rf25_fullwave`) | 635 µm of alumina's TE1 to 1e-9 of its quarter wave; none up to 30 GHz; silicon under oxide the two slabs' own root to 1e-6; with no ground the half wave |
| `pastTheSecondSurfaceWaveItIsSaid` | A strip on 635 µm of alumina at 36 GHz: nothing said; at 36 and 42: one warning, from 39.79 GHz, among the lines too |
| `aFullWaveStudySolvesThePortsAndDrawsTheCurrents` (`test_rf25_doc`, added to) | Calibration strips of 300 µm read and solved as asked; a length that is none refused |
| `anRf25ModelIsSolvedFullWaveThroughItsTool` (`test_qucs_control`, added to) | A calibrated port's third strip, agreement, feed and reach in the answer, no warnings; on 635 µm at 42 GHz the brief answer's one warning and the second surface wave's frequency |

## Broken on purpose

`docs/feature_gaps/2026-11-02-claude-rf25-phase-e-check/breaks.py` holds twenty-one:

- **The third strip:** not looked at, the residual the shorter's alone; the residual in ohms beside the cosh and
  the sinh; the third as long as the longer.
- **The shorter strips:** none solved; each frequency from those that agree best rather than the longest that agree.
- **The impedance:** always the shorter's; the third's wherever it is the longer.
- **A feed's straight run:** not looked at; never stopped; a coplanar feed's grounds taken for its widening; a
  feed shorter than its port's field not said.
- **A de-embedding that gives back:** not said; where it gives back the most not kept.
- **The second surface wave:** never found; over a ground TE1 taken for the slab's half wave; past it nothing said.
- **The answer and the tab:** the warnings left out; what the strips said left out; their agreement not kept at the
  port; a port's Calibration strips not read; handed over in metres.

All twenty-one were caught, after the strips' test was made to hold to two things more. Three got past it at first:

- **The third strip not looked at** still passed: where the shorter's own impedance is taken it fits its own line
  exactly, and the residual fell to 1e-16 - but nothing held it above nought, and the thick alumina's top frequency
  still read as disagreeing, through the half wave's impedance. The thin strips' residuals are now held between
  1e-7 and 3e-3 (1.6e-5 to 3.5e-4 measured), and the thick ones' lowest above 1e-6.
- **Each frequency from the strips that agree best** passed where the longest that agree were compared at the
  lowest frequency alone. Every frequency at which they agree is now held to them.
- **The residual in ohms** was caught by the long strips' test and not by the strips' own, which had been named for
  it; the script names the one that catches it.

Rebuilt after the breaks: the full ctest (151), the scenario script (97 checks) and the four lints green.

## Under the sanitizers

Built Debug with ASan, as CI's Linux job is, and solved with Eigen's sparse LU:

| Suite or function | Time |
|---|---|
| test_rf25_deembed, all | 396 s; its heaviest function, an open line's own end, some 60 s |
| test_rf25_fullwave, all | 487 s (456 last round) |
| test_rf25_fullwave `theSecondSurfaceWaveIsTE1sCutoff` | 7 s |
| test_rf25_fullwave `pastTheSecondSurfaceWaveItIsSaid` | 9 s |
| test_rf25_doc `aFullWaveStudySolvesThePortsAndDrawsTheCurrents` | 35 s |
| test_qucs_control `anRf25ModelIsSolvedFullWaveThroughItsTool` | 25 s |

Every calibration solves a third strip now. The de-embedding's suite, at CI's runner's twice this machine, would be
past three quarters of its 300 s times three: it has 900 s, as the other RF suites of dense solves do.

## Not done

- **G9's cause.** Found the same day, and not where this said to look: the ports' own paths up from the ground
  (`2026-11-02-rf25-port-paths.md`).
- **The coarse-level preconditioner** for via-stitched structures, from phase E's own list; the reviewer's MOM took
  573 iterations at three cells per finger.
- **A coplanar port's reach.** The feed-length check holds a ground-referenced port to four heights and its
  width; a coplanar port's own field is the gaps' and grounds', which this does not judge.
- **NIST's weighting.** Each frequency takes one set of strips whole; a multiline calibration weighs every pair of
  lines by how well it is conditioned there.
