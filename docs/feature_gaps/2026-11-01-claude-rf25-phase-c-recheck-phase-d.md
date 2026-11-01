# RF 2.5D: the phase C recheck and phase D, 1 November (afternoon)

The reviewer's live check (`~/Desktop/qucs-reviews/RF2p5D/2026-11-01-claude-rf25-phase-c-recheck-phase-d.md`)
was made on the build of 1 November 11:24, which had everything up to `84c495b`: phase D (`45dc14b`, `d1c3b77`),
the phase B check's close (`ac6ce8f`) and the phase C check's fixes (`84c495b`). Every remaining item was
reproduced first, on `84c495b`, through `qucs-s --mcp-server` on copies of the reviewer's own models
(`~/QucsWorkspace/rf25_check/fw/`), and from the reviewer's own Touchstone files where they were the evidence.

The check's passes stand as it found them: the coplanar port's 1.5 % lost at 60 GHz, the two-line alpha with
coplanar ports, perfect metal's eps_eff and its dispersion, the merged tee, the tee's quasi-static "no line",
phase D's calibration and de-embedding of a microstrip, and Touchstone 2.0.

## Findings

| Feedback | Now |
|---|---|
| **Coplanar port still carries its own discontinuity** (calibration not done for coplanar feeds): 1 mm GCPW \|S11\| −15 dB at 60 GHz against −37..−40 for QFEM's ideal line; "this alone costs ~0.14 dB of the S21 gap. The next thing a CPW user needs" | **Done: a coplanar port is calibrated by strips of its coplanar feed.** Reproduced: −14.6 dB. L–2L on coplanar strips put the port's own discontinuity at 5.0 fF at its face and −13 pH at the port, two capacitive reflections that add to −15 dB where the line is a half wave. The full-wave solve now reports each coplanar port's feed as it is at the port: the gap to each ground and its width on the edge's line, and the vias down the grounds. The strips are that feed whole: signal, grounds and vias, each strip's first via as far in from each end as on the feed. The longer strip has twice the shorter's via periods, not twice its length. Then T₁T₂⁻¹T₁ is the port's own discontinuity up to the first via, and the line back from there to the face is taken off along the strips' own line. Without its vias a grounded CPW's grounds have modes of their own: a strip pair without them read −100 dB/m at 50 GHz. **On the check's 1 mm line, both ports calibrated, \|S11\| is −57 dB at 60 GHz and \|S21\| −0.074 dB, where QFEM's ideal line gives −0.077** (table below). A feed whose vias strips cannot copy alike at both ends is said, not calibrated: a lone via in a row, rows staggered by other than half a period, a via on the signal. The two ends of a line share one calibration. |
| **Copper: the CPW's eps_eff still rises with refinement** (5.55 → 5.61 at 4 → 8 across, QFEM Cu 5.46) **and its alpha is about half of QFEM's** (39–45 vs 77 dB/m at 60 GHz): "the metal-as-a-sheet item" | **Its loss done; its thickness's capacitance not.** Reproduced from the check's own files: 39.2 and 45.2 dB/m at 60 GHz, 17.4 and 19.9 at 10. The full wave's loss is exactly what a sheet loses when each cell carries its average current. A 2D solve of the same cross-section, cut into the solve's cells, gives the same numbers: a microstrip 5857 /m per ohm of Rs against the full wave's 5822. The current on a sheet crowds to its edges as 1/√s, and an edge cell 12.5 µm across smears that out. **The cells along a conductor's edge now lose as the crowding the metal's thickness stops does.** That is ln(e/Δ)/4 times their average current's loss, with Δ = t/(4πe^π), Holloway and Kuester's stopping distance (Ghione's coplanar formula has the same t). Where the metal is thin beside its skin depth, its own resistance spreads the current over Pearl's length as well, and at direct current the factor is one. QFEM's own loss is 13,400–13,850 /m per ohm of Rs from 5 to 60 GHz, set by the thickness and not the skin depth, which is what this assumes. **On the check's line, the calibration's 1.8 mm-apart strips read 70.5 dB/m at 60 GHz against QFEM's 76.9 (−8 %), and 7–8.5 % under from 20 to 60 GHz; four and eight across now agree to 1–2 %** (table below). A copper microstrip at 10 GHz is +10 % and +5.5 % of phase B's Wheeler R at four and eight across, where the sheet was 33 % under. Copper adds 0.078 to eps_eff at 10 GHz: QFEM's own loss implies 0.079 for its internal inductance. What is left of the eps_eff gap, 5.59 against QFEM's 5.46, is the copper's thickness: a 3 µm wall beside a 50 µm gap holds field in the air, which a sheet has none of (Not done). |
| **Small negative two-line alpha at 50–60 GHz with perfect metal** (−2 to −6 dB/m): leftover end coupling | **The reading is right, and a line read as gaining is no longer used as one.** Two lines 0.9 mm apart read −2.3 and −5.7 dB/m at 50 and 60 GHz. The calibration's strips, 1.8 mm apart, read −1.3 and −1.6: half the coupling, which falls with distance, so it is the ends' own. A line gives nothing back: a reference plane is now moved along a line the strips read as gaining as along one that loses nothing. `rf25_solve` gives "line dB/m" 0 and "line dB/m as read" −1.64, and the panel says why. |
| **The 750 µm thru reports "passive: false"** (most returned 1.0012), correctly flagged as the de-embedding's, with "passive as solved: true" | **The flag stands; behind it was a fault that is now fixed.** The reviewer's thru is a line de-embedded half way in from each end, its two reference planes meeting. The de-embedding went through Z, Z' = (A − ZC)⁻¹(ZD − B), and a thru has no Z. The check's thru came out as numbers only because the strips and the line differ a little. Meshed as the longer strip, a 2 mm line de-embedded to its middle came out 59 % active, \|S21\| +1.4 dB. **It is now done in waves**, S' = (1 + X E₂₂)⁻¹X with X = E₁₂⁻¹(S − E₁₁)E₂₁⁻¹, each port's discontinuity its own two-port in its reference. That 2 mm thru is now 1.0000000003, \|S11\| −180 dB. The check's own 1.0012 is unchanged: a residual reflection of −56 to −64 dB beside a whole transmission, which is the strips' accuracy. Cutting the strips into the line's own cells along did not change it. A 1.333 mm line whose cells are the strips' still reads −56.8 dB. The panel now gives its size, "as much as a reflection of −64 dB beside a whole transmission gives". |
| **Nit: a no-line quasi-static answer gives "first half-wave resonance": 0** | **Left out.** What is no line has no half-wave resonance: `rf25_solve` now omits the key, as the tab did. |

### The check's 1 mm GCPW, both coplanar ports calibrated

100 µm signal, 50 µm gaps, 250 µm grounds with vias every 150 µm, on 200 µm of 9.8, copper 3 µm, four across.
The strips are 1900 and 3700 µm.

| GHz | \|S11\| as solved | calibrated | QFEM line | \|S21\| as solved | calibrated | QFEM line |
|---|---|---|---|---|---|---|
| 10 | −36.5 | −36.5 | −73 / −27 | −0.018 | −0.029 | −0.031 |
| 30 | −38.1 | −29.9 | −26 / −25 | −0.034 | −0.052 | −0.065 |
| 50 | −17.7 | −33.3 | −29 / −27 | −0.125 | −0.067 | −0.076 |
| 60 | −14.6 | **−57.2** | −40 / −37 | −0.218 | **−0.074** | −0.077 |

(QFEM's \|S11\| with its two impedance definitions, Z_PI and Z_PV. "As solved" is the check's own, before the
loss correction.)

### Copper's loss on the check's line

| GHz | two-line, 4 across, was | now | 8 across, was | now | calibration strips, now | QFEM |
|---|---|---|---|---|---|---|
| 1 | 7.88 | 7.96 | 8.85 | 8.85 | 7.96 | 11.97 |
| 10 | 17.40 | 27.50 | 19.89 | 27.56 | 27.53 | 30.88 |
| 30 | 33.34 | 52.27 | 38.08 | 53.06 | 49.56 | 54.14 |
| 60 | 39.15 | 66.79 | 45.21 | 67.38 | **70.51** | 76.86 |

The two-line pair is only 0.9 mm apart, and its 50–60 GHz is held down by the ends' coupling. That is the
same −6 dB/m perfect metal reads there. Between 1 and 5 GHz the copper is 1.4 to 3 skin depths thick. There
the stopping distance, made for a metal many skin depths thick, gives way to the sheet's own spreading, and
the loss stays under QFEM's, by 33 % at 1 GHz and 16 % at 5.

## What reproducing found

- **The de-embedding through Z broke where it should be simplest.** It failed wherever the reference planes
  met: a line de-embedded half way from each end. The check's thru lived because the strips and its line differ.
  Fixed in waves, above.
- **The two ends of a line made two calibrations of one feed:** its vias were met in a different order from
  each end, and the feeds' texts differed. A feed is now keyed with its vias sorted, and level ones are found in
  order across. Its mirror image, which the far end sees, shares it too.
- **A micrometre of copper is not three.** The first form of the edge factor faded by the edge cell's own L/R
  and put a 1 µm sheet at 2 GHz at 1.75 times its direct-current resistance. The current in a metal thinner
  than its skin depth spreads over Pearl's length, 1.5 µm here, past which a 12.5 µm cell holds little crowding.
  The factor now stops the crowding at the thickness's stopping distance plus that spreading: where the sheet
  loses no more than a thick metal's two faces, that is the thickness alone; at direct current it is all of it.
- **Coplanar strips at eight across are more than a dense solve takes** on this build (3000 runs and cells):
  three conductors 3.7 mm long. The strips are now cut to a half, then a quarter, and the cut is said ("their
  ports the nearer and the line they read the less sure. A coarser mesh takes them whole"). The check's line at
  eight across calibrates on 1000 and 1900 µm strips, to −46.7 dB at 60 GHz.
- **Phase B's Wheeler R takes each face's surface resistance as a whole slab's**, Re Zs(σ, t). Where the metal
  is thinner than about two skin depths and both faces carry current, as a coplanar line's do, each face's own is
  the half slab's, up to twice as much. Its floor at the direct-current resistance holds the low end. Noted, not
  changed: the full wave takes the half slab.

## Tested

| Test | What it holds to |
|---|---|
| `aCoplanarPortsFeedIsItsGroundsAndItsVias` (`test_rf25_deembed`) | A 1.3 mm grounded CPW with vias every 150 µm from 25 µm in. The feed found: gaps 50, grounds 150, two rows, pitch 150, first via 50 µm in. The far end's feed keyed as the near's, uneven grounds and their rows too, mirrored. Strips of 1000 and 1900 µm, six periods between, said. De-embedded at 10 and 40 GHz, the strips' own line to 0.1° and 2e−3, which was 0.024 and 0.095 off as solved (measured 0.0015° and 2.4e−5); half way from each end, a thru to 2e−3 |
| `whatACoplanarFeedsStripsCannotCopyIsSaid` | A lone via in a row, rows staggered by a third, a via on the signal: each said, and refused with it; rows staggered by half a period are a feed |
| `stripsTooLargeAreCutAndSaid` | With the limit lowered, strips cut to a half and said; past a quarter, refused with the size; a length asked for is not cut |
| `aThruWhosePlanesMeetIsAThru` | A line meshed as the longer strip, de-embedded to its middle: \|S11\| under 1e−6 and S21 within 1e−6 of one, passive and reciprocal |
| `aLineReadAsGainingIsMovedAlongAsLosingNothing` | A line read as gaining 1 Np/m moves a plane as a lossless one does, to 1e−12; what was read is kept and said |
| `aMetalsLossIsItsThickEdgesWhereTheCurrentCrowds` (`test_rf25_fullwave`) | 3 µm copper microstrip at 10 GHz: within 13 % of Wheeler's R at four and eight across, the two within 7 % (measured +10.4 %, +5.5 %); the runs along an edge a tenth to four tenths of them, said |
| `aSubstratesAndAMetalsLossesAreTheirs` | 1 µm copper: 10 MHz its direct-current resistance, 2 GHz the sheet's crowding as before (2.7 %): its own resistance spreads the current |
| `aCoplanarPortIsReadAndItsFeedCalibrated` (`test_rf25_doc`) | The tab calibrates a coplanar port that asks, strips of its coplanar feed with no vias, said |
| `anRf25ModelIsSolvedThroughItsTool` (`test_qucs_control`) | The heater's pads, no line: "is no line" and no "first half-wave resonance" |

## Broken on purpose

`docs/feature_gaps/2026-11-01-claude-rf25-phase-c-recheck-phase-d/breaks.py` holds twenty:

- **Coplanar calibration:** strips without their grounds; without their vias; the longer twice the shorter's
  length rather than its periods; the discontinuity left at the first via; the vias found nowhere; rows
  staggered by a third taken; a via on the signal not seen; a feed keyed apart from its mirror image; its vias
  keyed in the order found; the tab's coplanar port not calibrated; strips too large not cut.
- **The edge loss:** no crowding; no side of a run taken as an edge; every side taken as one; a thin metal's own
  resistance not spreading the current; the crowding stopped at the thickness itself.
- **De-embedding:** through Z again; each port's two-port with its two ends swapped.
- **The gaining line** moved along as read. **The resonance of what is no line** said at nought.

## What the breaks taught

- **Seventeen of the first nineteen were caught at once.** Two were not, and both showed a test that could not
  tell.
- **Every side of every run taken as an edge** came out +8.8 % of Wheeler's R, inside the test's bound. The
  extra crowding moved from the edge cells (1.44 where 1.78) to the inside ones (1.58 where 1), and the total
  hardly changed. The test now also holds the share of runs said to be along an edge: 32 of 126 at four across,
  60 of 408 at eight, and all of them under the break.
- **A feed keyed apart from its mirror image** passed because the test's feed is symmetric: its mirror is
  itself. The two calibrations the check's line made came from the vias' order, not from mirroring. The test
  now also has uneven grounds, 150 and 250 µm, each with a row of vias, which the two ends see the other way
  round. The mirror break and a twentieth, the vias keyed in the order found, are caught by it: twenty of
  twenty.

The full ctest (149) and the scenario script (97 checks) were green on the code as committed, both lints too.
Under the sanitizers, with Eigen's own LU as CI's Linux has it, the new and changed functions take 7 to 44 s - the
tab's coplanar calibration 44, the coplanar feed's strips 32, the edge loss 24 - and the de-embedding's suite
241 s, with no sanitizer report.

## Not done

- **The metal's thickness beyond its loss.** A thick conductor's walls hold field in the air beside them: on
  the check's line, copper's eps_eff is 5.59 at four across against QFEM's 5.46, 2 to 3 %. Wanted: each edge's
  wall, a strip of charge of the metal's height tied to its edge cell - charge cells at a height of their own, a
  short run up to each. That is more cells and runs, and so a dense solve of more unknowns.
- **A wide strip's current favours its lower face**, which a sheet's two faces alike do not see. At w/h 1 that
  is 1.3 % of the loss, at 3 it is 7 % and at 10 it is 28 %. It wants the field of the currents' image at each
  cell.
- **Where the metal is one to three skin depths thick**, the loss stays under: 3 µm of copper reads 33 % under
  QFEM at 1 GHz, 16 % at 5.
- **Phase B's Wheeler R for a metal thinner than two skin depths** (above).
- **Coplanar strips at fine meshes** are cut to fit, and their line read is the less sure.
- **A continuous via wall at fine meshes**, from the phase C check: many touching via cells.
