# RF 2.5D: calibration strips sized by a coplanar feed's gaps where there is no ground plane

The question: "Looks like enabling port de-embedding significantly increases the simulation time of a simple GS
transmission line. Why?" Then: "Yes, fix it".

## What it was

Calibrating a port, "Take out its own discontinuity", first solves three straight strips of the port's feed, l, 2l
and 1.618 l long, at every frequency of the sweep. The shorter strip was eight times the metal's height over the
ground, plus twice the feed's width. That height keeps the near fields of the ports at a strip's two ends apart.

With the ground "open", there is no plane at all, and the height read was the metal's over the bottom of the stack:
the whole substrate. A coplanar line's field is across its gaps, between the signal and the conductors beside it, and
reaches nowhere near that far.

On a 500 µm line, a 50 µm signal in 6 µm gaps over 500 µm of silicon, swept from 10 to 110 GHz:

- The strips were 4144, 8288 and 6705 µm long, some 19 mm of line for a 0.5 mm structure, plus a low frequency to
  follow their phase up from.
- The solve went from 1.1 s to 102 s.
- A segmented T-rail line on quartz hardly showed it: its own solve, 16,000 unknowns held compressed, took minutes,
  and the strips added some 14 s.

## How long the strips need to be

The same line, calibrated with the shorter strip's length given:

| Shorter strip | Solve | At 110 GHz: discontinuity | Line ε_eff | Line Z |
|---|---|---|---|---|
| 150 µm | 1.6 s | 1.46 fF, −0.66 pH | 4.407 | 43.10 Ω |
| 250 µm | 2.6 s | 1.52 fF, 1.78 pH | 4.156 | 42.83 Ω |
| 400 µm | 5.6 s | 1.44 fF, 2.44 pH | 4.126 | 43.26 Ω |
| 700 µm | 13.8 s | 1.45 fF, 2.45 pH | 4.127 | 43.53 Ω |
| 1000 µm | 47 s | 1.44 fF, 2.19 pH | 4.134 | 43.42 Ω |
| 2000 µm | 130 s | 1.44 fF, 2.13 pH | 4.134 | 43.28 Ω |
| 4144 µm (the old choice) | 102 s | 1.46 fF, 2.2 pH | 4.134 | 43.47 Ω |

From 400 µm up, what the strips read stays within their own scatter. That is about six and a half times the signal
and its two gaps (62 µm).

## The fix

`calibrate()` (`rf25/rf25_deembed.cpp`):

- With no ground plane under the stack, a coplanar feed's shorter strip is eight times its signal and gaps, plus twice
  its width. This is the old rule with the gap span in place of the height.
- It applies whatever the substrate's thickness. With no plane, a thin substrate's height bounds nothing either.
- Over a ground plane the strips stay eight heights long, as before. So does a feed that is not coplanar. A given
  "Calibration strips" length is used as given.
- The calibration says why: "There is no ground plane under the stack: the strips as long as eight of the feed's
  signal and gaps, … µm, and twice its width, not as eight of the metal's … µm over the bottom of the stack."
- The "Calibration strips" property's help says so too.

On the two lines (both solves of the second are minutes long, and swing by minutes from run to run):

| Line | Not calibrated | Calibrated before | Calibrated now | Strips now |
|---|---|---|---|---|
| Plain, on silicon, to 110 GHz | 1.9 s | 102 s | 19.9 s | 596, 1192, 964 µm |
| Segmented T-rail, on quartz, to 50 GHz | minutes | +14 s | no slower | 1336, 2672, 2162 µm |

Both read what the long strips read. The plain line at 110 GHz: 43.4 Ω, ε_eff 4.128, 1.44 fF and 2.48 pH. The T-rail
at 50 GHz: 56.98 Ω, ε_eff 3.032, 2.24 fF. The three strips agree within 0.34 % and 0.08 %.

## Tests

`test_rf25_deembed`'s `aCoplanarFeedWithNoGroundPlaneIsCalibratedByItsGaps`:

- With no plane under 500 µm of silicon, the strips are 596 µm long, and the calibration says why.
- They agree on one line.
- Strips of 1500 µm read the same line, within 0.5 % in ε_eff and 1 % in Z, and the same de-embedded S, within 0.01.
- Over a ground plane 100 µm down, the strips are 900 µm: eight heights.
- With no plane under 20 µm of substrate, the strips are 596 µm still, by the gaps.

[breaks.py](2026-11-11-rf25-strips-open-ground/breaks.py) undoes each piece on purpose, builds and runs the test. All 5 breaks
are caught: [results.txt](2026-11-11-rf25-strips-open-ground/results.txt).
