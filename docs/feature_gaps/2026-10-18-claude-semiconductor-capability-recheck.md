# The semiconductor capability review, rechecked

The review "FEM semiconductor capability" (`2026-10-12-claude-fem-semiconductor-capability.md`, on `56a5abb`) was answered on 12 October in fifteen commits, `d9c4751` to `7196d35`. It was written up then in `2026-10-12-claude-fem-semiconductor-capability.md`:
- **Its ten gaps:** all ten done, but 3D, which got a written proposal.
- **Its addendum:** the six findings from the germanium photodiode example, all done.

On 18 October it was checked again on `9e7e8d4`.

## What still stands

- **The suite:** every test of that round passes (the Release suite 129 of 129). These are `test_fem_devices`' 23 functions, the photodiode's and the benchmarks' in `test_fem_optoelectronics`, and the others listed there.
- **The one open note:** "a residual check beside the step's size" was added the next day, `8f4ca6c`. Newton now stops on its equations met, not on its steps' size alone.
- **Still not done, as written then:**
  - 3D (the proposal's three decisions are the user's);
  - slow traps' own state (hysteresis, GaN current collapse);
  - non-local tunnelling;
  - energy balance;
  - process steps beyond implant, anneal and diffusion.

## The photodiode example again (the addendum)

`examples/multiphysics/ge_photodiode.qfem`, its three studies through the tools (a scratch copy):

| | The review, 12 Oct | Now | The example's text |
|---|---|---|---|
| Dark current at -1 V | 7.19 nA | 7.135 nA | said 7.4 nA: **now 7.1** |
| neff at 1550 nm | 3.843 | 3.809 | 3.81 (germanium's measured index, Nunley et al.) |
| Responsivity | 1.150 A/W | 1.158 A/W | 1.16 A/W |
| Capacitance at 1 GHz, 1 mW | 6.57 fF | 2.287 fF | said 6.6 fF: **now 1.9 fF, 2.3 under the light** |
| 3 dB at 1 mW | 27 GHz | 33.3 GHz | about 33 GHz |
| 3 dB through 50 Ω at 25 µW | | 35.4 GHz | said 36: **now 35** |
| R at 1580 / 1600 / 1620 / 1640 nm | | 1.099 / 0.884 / 0.414 / 0.157 A/W | 0.88, 0.41, 0.16 |

**The capacitance moved, and nothing checked it.**
- **Dark, the diode is 1.926 fF,** flat from 1 to 200 GHz, and 1.93 fF at 25 µW. A plate's estimate of the junction (germanium 0.45 µm by 15 µm, 0.7 µm apart, εr 16) is 1.4 fF before its fringes.
- **Under 1 mW** its low frequencies hold more: 2.287 fF to some 25 GHz, falling to 1.90 fF at 200 GHz. That is the carriers the light makes, stored as the bias changes.
- **The review's 6.57 fF** was that excess under the light, three times the diode's own. Somewhere between 12 and 18 October it fell to 0.36 fF. The commits were not bisected; those that changed the semiconductor's carriers in that span include the heterojunction's band step and the nodes' boxes.
- **The example's "6.6 fF" was never a test's figure,** so it was not caught.

**Now:**
- The example says 1.9 fF (2.3 fF below some 25 GHz at its 1 mW), a dark current of 7.1 nA and 35 GHz at 25 µW.
- `thePhotodiodeExample` checks the capacitance at 1 GHz under the light, 2.29 fF within 0.1 fF (0.3 on a sanitized build's mesh). Its dark current's centre is now 7.15 nA, within 0.5 nA as before.

## Break

The small-signal capacitance reported three times over (6.86 fF, about the review's figure): caught by `thePhotodiodeExample`.

## Runs

- **Release:** `thePhotodiodeExample` 2.29 fF, 7.135 nA; the suite 129 of 129.
- **Sanitized:** 2.301 fF, 6.827 nA, in 70 s; no report.
