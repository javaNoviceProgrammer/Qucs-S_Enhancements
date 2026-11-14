# RF 2.5D's port de-embedding from 30 to 150 GHz

The user saw de-embedding go wrong above 30 GHz and asked for its accuracy to be checked from 30 to 150 GHz, then
said "Yes, fix it".

**The answer:**
- De-embedding was accurate to about 50 GHz.
- Above that, on a lossless microstrip it gave back up to 2 % of the power, and from 100 GHz it added 70–260 dB/m
  that isn't there.
- There were two causes, both in how the calibration strips are read, and both are fixed.
- The gold CPW keeps a smaller per-port error, which is not fixed.

## How it was checked

`make_lines.py` makes two plain lines, each at two lengths, with their edge ports calibrated and not:
- **A microstrip:** 95 µm wide on 100 µm of alumina over a ground plane, perfect metal, 1000 and 2000 µm long. It
  loses nothing.
- **The benchmark's coplanar line:** 2 µm of gold solved as Automatic, on 500 µm of quartz open below, 500 and 1000 µm
  long.

What de-embedding should leave is a uniform line: the same γ and Z at either length, and on the microstrip no loss.
`lines.py` reads both from each de-embedded two-port. The references:
- **L-2L of the uncalibrated pair**, in which the ports cancel.
- **Palace 2D** (`make_cs_ms.py`): the microstrip's mode is bound and loses nothing, with εeff 6.662, 6.788, 6.927,
  7.073, 7.218, 7.361 and 7.499 at 30–150 GHz. RF 2.5D's L-2L matches it within 0.3 % throughout.
- **eddy.py and QFEM** for the gold line's loss.

`lines_before.txt` and `lines_after.txt` hold the full tables.

## What was wrong

**1. Strips whose ports' fields meet.** Where the strips disagree on one line, the calibration solves shorter ones
beside them, halving up to three times: 990 → 495 → 248 → 124 µm on the microstrip. It then takes each frequency from
the longest set that agrees within a percent, and otherwise from the set that agrees best.
- The 124 µm strips' ports sit about a substrate height apart, so their near fields meet.
- All three of them are wrong alike, so they agree with one another as well as any set does.
- From 102 GHz they agreed best and were taken, reading the lossless line as losing 247–1464 dB/m. The longest strips
  read −12 to +69 there, noisy around the true 0.
- De-embedded, the line lost 3–7 % of the power.
- The halving came in on 2 November for a feed over 305 µm of silicon.

**2. A line read as gaining, lent to the box as loss.** The strips fix the port's box only to within a piece of the
line itself: E·L_x fits every strip as E does, for any x. So whatever loss or gain the strips read into the line, the
box takes the opposite.
- Where nothing in the feed can lose, the line's read loss or gain is its two ports reaching each other past it,
  through the substrate.
- On the microstrip the longest strips read the line as gaining 7–22 dB/m from 40 to 90 GHz, so the box lost the
  same.
- Taking that lossy box out gave back up to 2 %, which the solve flagged.

## The fix (`rf25_deembed.cpp`)

1. **No strips shorter than the ports' own fields reach.** That is half the strips' first length: four heights and the
   width, or on a coplanar feed with no plane, four of its signal and gaps and its width. **Where no set agrees within
   a percent, the longest is taken**, not the one that agrees best.
2. **Where nothing in the feed can lose, the line is taken lossless.** "Nothing can lose" means the metal has no
   conductivity or no thickness, the dielectrics no loss tangent or conductivity, and the ground is a perfect plane.
   The piece x = (l − wave/γ)/2 that makes the line lossless is taken out of the box. What the ports radiate then
   stays in the box, and de-embedding removes it with the box.

## Results

**The microstrip, de-embedded 1000 µm line** (true α 0, εeff Palace's above):

| GHz | α before (dB/m) | α after | εeff before | εeff after | Power lost before | after |
|---|---|---|---|---|---|---|
| 30 | 3.4 | 0.1 | 6.6458 | 6.6458 | +0.0008 | +0.00001 |
| 60 | 13.4 | 0.2 | 6.8583 | 6.8583 | −0.0030 | +0.00004 |
| 90 | 22.3 | 0.6 | 7.0715 | 7.0715 | −0.0051 | −0.0001 |
| 100 | 72.9 | 0.1 | 6.9872 | 7.1404 | +0.0173 | −0.0004 |
| 110 | 118.3 | 0.2 | 7.2127 | 7.2043 | +0.0273 | −0.0001 |
| 130 | 228.4 | 1.0 | 7.3100 | 7.3558 | +0.0491 | −0.0009 |
| 150 | 258.0 | 8.8 | 7.3839 | 7.5188 | +0.0599 | −0.0003 |

- After the fix the 1 mm line loses at most 0.26 % (66 GHz) and gives back at most 0.19 % (80 GHz) at every 2 GHz
  step, apart from 0.61 % given back at 112 GHz. Before, it gave back up to 0.8 % and lost up to 6 %.
- **The 2 mm line** gives back 0.25–0.5 % between 50 and 96 GHz, and loses up to 1.3 % at 140–150. Its own two ends
  radiate to each other more, which belongs to the line as measured. A port's box found from strips of other lengths
  can't hold that exactly.

**The gold CPW** is not covered by the second part, since gold loses:
- To 60 GHz its de-embedded α at either length is within 2 % of L-2L and of eddy.py.
- Above that, the 500 µm line reads below L-2L at 60–90 GHz and above it from 100 GHz: 224 against 176 dB/m at 120.
  That is about −0.005 to +0.012 dB per port. It was the same before.
- **Tried and not kept:** subtracting what a perfect-metal twin of the strips reads. Tested on L-2L of the
  uncalibrated lines, it brought gold within 1.5 % of eddy.py to 90 GHz, but was 15 % off above. The twin is a sheet
  where the gold is two faces, and their ports couple differently.

**Reflective structures:** the bandstop filter of phase 7 and its single stub pair now give back 0.19–0.20 % at
12–13 GHz, against 0.12–0.14 % before. Each port's box now carries its radiation as its own loss, which is right for a
matched line. Where the structure reflects nearly everything, that box gives back a little. The filter's stopband
edges are unchanged (7.4444 and 13.7069 GHz at 6 across).

## Checked

- **`test_rf25_deembed`** `aMicrostripDeembeddedToMmWavesLosesNothing` (new):
  - the microstrip's 1000 µm line, de-embedded at 60, 90, 102, 110 and 130 GHz;
  - within 1e-3 of lossless, with α under 3 dB/m (measured 0.18–1.16);
  - no strips shorter than half the first tried.
- **`aThruWhosePlanesMeetIsAThru`:** a strip de-embedded half-way from each end is no longer an exact thru, since the
  strips' ports' coupling past the line now stays in it. Its |S21| bound is 1e-3 (0.99998 at 10 GHz, 0.99979 at 20)
  and |S11| 1e-5. It still gives nothing back.
- **`breaks.py`** undoes each piece; all four breaks are caught. At first the "agrees best" break wasn't caught, so
  102 GHz was added to the test. Results are in `results.txt` beside it.
- All 26 RF 2.5D suites and `test_qucs_control` pass under ctest at `-j4`, and the four lints pass.
- **`test_rf25_deembed` under ASan and UBSan** passes through ctest with CI's timeout scale, in 586 s, with no errors.
  Run directly, without the scale, `aCoplanarFeedWithNoGroundPlaneIsCalibratedByItsGaps` runs past QtTest's 300 s.

## Not done

- **The gold CPW's per-port error** above 60 GHz, up to 0.012 dB per port.
- **The solve's passivity check** takes the worst case over all excitations. On a near-lossless two-port an S11 of
  −36 dB in the wrong phase already reads as 3 %, so at some frequencies it still says the de-embedding gives back.
