# RF 2.5D held to references that are not itself

Phase 1 of the plan for RF 2.5D's accuracy limits (the user: "implement all Accuracy limits items", then the plan
approved). The four limits are those the open items of 8 November left
(`docs/bug_hunts/2026-11-08-claude-rf25-open-items.md`):
- **A1,** the walls' current;
- **A2,** the metal's loss;
- **A3 and A4,** the pads' loss and a filter's frequency;
- **a quasi-static Z0's convergence.**

This phase records where RF 2.5D stands against references made outside it, so that each later phase shows what it
gains.

## The new suite: `test_rf25_accuracy`

| Check | Reference | Now (12 November) |
|---|---|---|
| `aThinCoplanarLinesZ0ConvergesOnPalaces`: the reviewers' CPW on quartz (S 50 / G 20 / grounds 160 µm), thin perfect metal, quasi-static at 6, 12, 24 across | Palace's thin-metal Z0 73.72 Ω, eps_eff 2.4005 | Z0 77.67, 75.68, 74.70 Ω (+5.4, +2.7, +1.3 %); each step 0.49 of the last (first order); Richardson's limit 73.77 Ω (+0.07 %); eps_eff −0.03 % |
| `aPerfectMetalsInductanceIsTheBoundaryElementSolves`: the alumina line, perfect metal as its two faces, full wave at 10 GHz | wheeler.py's boundary-element L, 3.38279e-7 H/m | +5.5 % |
| `theFiveLinesAreTheir2DSolves`: lines.py's five lines, full wave as the tab meshes them | eddy.py's R and L of each cross-section | see below |
| `theBenchmarksCoplanarLineIsPalacesAndQfems`: the same CPW, 2 µm gold as its two faces, 12 across | Palace 2D's and QFEM's mode solves (the Palace benchmark's comparison 1) | Z0 +2.73, +2.71, +2.76 % at 10, 30, 50 GHz; eps_eff +0.66, +0.62, +0.46 % |

**The five lines,** the worst of each one's three frequencies:

| Line | R | L | α |
|---|---|---|---|
| alumina (50/25/270 µm, 6.5 µm) | +14.8 % | +6.3 % | +8.0 % |
| copper (100/50/250 µm, 3 µm) | −9.3 % | +3.7 % | −12.3 % |
| film (10/5/100 µm, 1 µm gold) | −14.9 % | +5.4 % | −18.7 % |
| narrow (20/6/100 µm, 3 µm gold) | −12.6 % | +9.2 % | −20.0 % |
| microstrip (100 µm over 100 µm, 3 µm) | +32.2 % (−5.0 at 2 GHz) | +2.8 % | +28.6 % |

**What the numbers say:**
- **L is high on every line,** by 2 to 9 %, and a perfect metal's too. This is A1: the faces' walls carry no current.
  It is also the benchmark CPW's +2.7 % in Z0, with eps_eff within 0.7 %.
- **The loss** is under on the thinner metals at their lower frequencies and over on the alumina line's higher ones,
  the 4 November write-up's pattern for `crowding()` (A2).
- **The microstrip's +32 % in R at 30 GHz** is more than its metal. The full wave has radiation and a substrate
  wave, which a 2D solve of the metal has not. Phase 4 looks at it.
- **The quasi-static thin line** is right in the limit and converges at the first order (the convergence item).

Each check holds RF 2.5D to what it reads now with a little room; the later phases tighten them.

## The references

- **eddy.py's R and L** come from `lines.py`'s `reference()`, run on 12 November, and are written into the test as
  numbers. The microstrip's are halved. `lines.py` took its half-model's drop twice, though its mirror carries the
  other half's current: 8.36e-7 H/m for a strip as wide as it is high in air, where Hammerstad and Jensen's Z0/c is
  4.2e-7. The 4 November comparison was of ratios, which the doubling left alone.
- **wheeler.py's L**, Palace's thin-metal line, and Palace 2D's and QFEM's mode solves are from the 4 and 8 November
  write-ups and the Palace benchmark's comparison 1. No Palace run was needed for this phase.
- **The setups:**
  - the alumina line on its own alumina, as `test_rf25_measured` has it;
  - the rest on εr 3.8, which R and L do not see. On 3.8 the alumina line's calibration read R 25 % high at
    30 GHz.
  - The film's cells along it are 50 µm, not `lines.py`'s 20: its 8 mm calibration strip was otherwise past the
    dense limit.

## Checked

- `test_rf25_accuracy` passes, its seven functions in 3.5 minutes in Release. It skips its full-wave checks under the
  sanitizers, as `test_rf25_measured` does.
- No `breaks.py`: this phase adds tests and no code. Each of its checks is broken by the code each later phase
  changes, and those phases' break checks undo that code.
