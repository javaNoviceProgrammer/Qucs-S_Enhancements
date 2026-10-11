# Feature gaps: the semiconductor physics on the corrugated phase shifter

*11 October 2026 — Qucs-S 26.1.8.*

The pn-junction paper check's re-check on `8864128` confirmed that round's fixes. It
then named what was still open from the drift-diffusion check
(`2026-10-11-claude-fem-semiconductor-check.md`, written on `1ef149d`):

- the current at 0 V on a sweep's first point;
- the pA noise on the currents;
- a semiconductor solved inside a mode step being slow;
- `pn_diode`'s empty C–V and I–V graphs;
- the asymmetric G matrix.

It also added one more: a sweep's global graph over a mode step is empty too, and its
variable cannot be named.

Each item was first reproduced through `qucs-s --mcp-server`, with its own HOME,
settings, trash and cache. The models were copies of the reviewer's
`corrugated_ps_dd.qfem` and the bundled `pn_diode.qfem`. All of it is done here, on top
of `8864128`, except the slow mode step, which did not reproduce (section 3).

## 1. A current at equilibrium, and pA of noise on the currents

Reproduced on the reviewer's model, study "C-V (small signal)":

- **At 0 V:** −290.7 pA at the anode and exactly 0 at the cathode.
- **From 0.75 V:** the cathode's current wandered between −3 and −9.5 pA, while the
  anode's stayed at −1.6 fA.

The cause is one for both. A contact's current was its rows' reaction: the sum, over
its nodes, of the carriers' fluxes along the edges into the device. On 10¹⁹ cm⁻³
silicon each such flux is a difference of two terms of about 250 A per metre.

At equilibrium the two are equal to the last digits only. Every anode node has the
same potential (the contact's), so the rounding has the same sign at all 48 nodes, and
it adds up to −291 pA. Reversed, the noise of Newton's last digits is about 10⁻¹⁴ of
the same fluxes: pA. The device's real dark current is a few pA, so the old figure was
noise rather than a measurement.

| Review | Now |
|---|---|
| I_a = −290.7 pA at equilibrium, the sweep's first point. | **Nought, exactly.** At equilibrium (contacts holding carriers at one voltage, nothing generating), no current flows. The solution there is Poisson's equation alone, exact, and its currents are set to 0 rather than the rounding of the contacts' fluxes. |
| Reverse leakage below the solver's resolution: I_a + I_k is ±pA noise. Suggested: a weighted flux integral over the region next to the contact; or the balance as a global; or a tighter Newton. | **The weighted integral.** Each contact that holds carriers (ohmic, Schottky) has a weight over the semiconductor's nodes: 1 on its own, 0 on the other contacts', and between them the solution of a Laplace equation weighed by each edge's carrier conductance. The weight is flat where the carriers are many and changes across a junction's depletion. The contact's current is the edges' currents times the weight's change along them, plus the carriers' change in time where it weighs. Where the equations hold this is the same as the reaction, an identity, but it has none of the large fluxes' digits. |
| | The weights cost one sparse Cholesky factorization per solve, its pattern analysed once. The reviewer's 15-point C–V study took 15.5 s, against 15.4. |
| | The currents' sum is 0 by construction: the weights add up to 1. So a balance global would say nothing, and was not added. Newton's tolerance is unchanged; it was not the cause. |
| Small-signal G not symmetric: G_aa 105.9, G_kk 107.9, G_ak −101.3 nS at 0 V. | **G_aa = G_kk = −G_ak = −G_ka**, to the sixth digit at every bias (101.293 nS at 0 V). The admittances are weighed the same way: the edges' linearized currents, and jω times the carriers' change. The diagonal's excess had been the same rounding: 4.6 nS against an admittance of 5.8 mS (ωC), a relative error of 8 × 10⁻⁷. |

On the reviewer's model, reversed:

| Vr | I_a before | I_k before | I_a now | I_k now | q ∫ R dA |
|---|---|---|---|---|---|
| 0 V | −290.7 pA | 0 | 0 | 0 | 3 × 10⁻²⁹ A |
| 0.05 V | −1.342 fA | 280 aA | −84.55 fA | 84.55 fA | −84.96 fA |
| 0.1 V | −1.536 fA | 321 aA | −143.4 fA | 143.4 fA | −143.5 fA |
| 0.5 V | −1.572 fA | 328 aA | −489.0 fA | 489.0 fA | −490.0 fA |
| 0.75 V | −1.574 fA | −5.443 pA | −666.7 fA | 666.7 fA | −668.0 fA |
| 1 V | −1.576 fA | −3.133 pA | −829.1 fA | 829.1 fA | −827.8 fA |
| 5 V | −1.593 fA | −9.466 pA | −2.399 pA | 2.399 pA | −2.398 pA |

The anode's current now is the depletion region's generation. It agrees with
q ∫ R dA over the silicon to 0.5 % or better; the integral interpolates the carriers in
each triangle, where the solver lumps R at the nodes. It rises with the bias as the
depletion widens.

The reviewer estimated q n_i/(2τ) · A ≈ 4 pA per metre. The 2.4 pA here is that
estimate with the depletion's actual area at 5 V.

## 2. The example's C–V and I–V graphs, and a sweep's graph over its mode

| Review | Now |
|---|---|
| `pn_diode`'s 1D plot groups wrote `<dep semi.Ckk frequency Va>`, with one frequency first. The data display drew over it: an empty 0–2 kHz frame. The same for `semi.I_a`. | **An axis of one value comes last:** `<dep semi.Ckk Va frequency>`. The data display draws over a variable's first dependency, so the C–V curve is drawn over Va. The values' order does not change: an axis of one value adds nothing to their index. The cut line's graphs over the same sweep are ordered the same way. |
| (Re-check) A global graph over a sweep with a mode step wrote `<dep emw.loss_100 mode Vr>`, its one mode first: empty. | The same fix: `<dep loss_dBcm Vr mode>`. On a copy of the reviewer's model the loss is drawn against the bias, 17.5 to 8.9 dB/cm. |
| (Re-check) The variable's name was the expression mangled (`emw.loss/100` → `emw.loss_100`), with no way to name it. | **`name = expression`**, in a global graph's list: `loss_dBcm = emw.loss/100; emw.neff`. The same syntax works in a Global Evaluation's list and in `fem_evaluate`'s, both as globals and at points. A lone `=` is not otherwise valid in an expression, so the syntax is unambiguous; `==`, `<=`, `>=` and `!=` stay comparisons. |

## 3. A semiconductor solved inside a mode step: not reproduced

The review timed the reviewer's model at Vr = 5 V, one point from equilibrium, six modes
about 2.7. A stationary `[semi]` step and then a mode `[emw]` step took 30.6 s, and a
mode `[semi, emw]` step took 138.4 s.

Here, the same study through `fem_solve` took the same time either way, on `1ef149d`
(rebuilt from that commit) and on `8864128`:

| Build | Study | Time |
|---|---|---|
| `1ef149d` | stationary `[semi]`, mode `[emw]` | 28.4 s |
| `1ef149d` | mode `[semi, emw]` (after the other, same server) | 28.2 s |
| `1ef149d` | mode `[semi, emw]`, first in a fresh server | 26.3 s |
| `1ef149d` | sweep (Vr = 5 V), mode `[semi, emw]` | 25.9 s |
| `1ef149d` | sweep (Vr = 5 V), stationary `[semi]`, mode `[emw]` | 25.5 s |
| `8864128` | stationary `[semi]`, mode `[emw]` | 29.6 s |
| `8864128` | mode `[semi, emw]` | 27.7 s |

Each log reads the same: the contacts' voltages reached in 8 steps, and 6 modes in 34
products with 2 restarts.

The mode step calls `initialize()`, the stationary solve from equilibrium, once. It then
calls `solve()`, which at the voltages already reached is a single Newton solve.
Nothing in it repeats the set-up or the factorization.

The 138 s may have come from something else running at the time. If it shows again, the
model, the study and how it was timed would find it.

## 4. Minor

| Review | Now |
|---|---|
| `fem_evaluate` on "C-V (small signal)" said "the model changed since it was solved" after an unrelated study was added and deleted. The note is study-blind. | **By study.** A change to the parameters, geometry, mesh, materials or physics makes every solution old, as before. A change to the studies makes old only the solution of each study that changed (or went). A study added and deleted, or one renamed, leaves the others' as they were. |
| G not symmetric (G_aa 105.9, G_kk 107.9, G_ak −101.3 nS). | Section 1: symmetric to six digits. |
| `semi.Q_a` and `semi.Q_k` are about nought for ohmic contacts on neutral silicon, though "charge on a's metal" invites reading them as Q = CV. | An ohmic contact's `semi.Q_` now says so: "about nought on neutral silicon (a junction's charge is balanced inside it); its capacitance is the small-signal semi.Caa". The README's semiconductor section says it too, and that the currents are weighed. |

## Tests

- **`test_fem_semiconductor`:**
  - `contactCurrents`: a P+ P N N+ diode (10¹⁹ and 10¹⁷ cm⁻³, contacts on the 10¹⁹)
    swept 0, −0.45, −0.5 and −0.55 V, with a small-signal step at 1 kHz and 1 MHz.
    Its checks:
    - at 0 V, the first point, both currents exactly 0;
    - reversed, I_a + I_k below 10⁻⁶ of I_a, and I_a equal to q ∫ R to 1 % (to 0.3 %
      here), growing with the bias;
    - G_aa = G_kk = −G_ak = −G_ka to 10⁻⁴;
    - at 1 kHz and −0.5 V, G_aa equal to the dc current's slope to 1 % (0.05 % here);
    - C_aa the depletion's ε/W to 3 %, and C_ak = −C_aa;
    - `semi.Q_a`'s description.
  - `currentsInTime`: a junction reversed by a ramp of −2 V in 1 µs. At −1 V the
    current is C·dV/dt, with C from a small-signal step at −1 V (to 2 × 10⁻⁴ here), and
    the cathode's current equals it, out.
- **`test_multiphysics_doc`:**
  - `aDiodeSwept`: `semi.I_a` and `semi.Ckk` over Va first.
  - `graphsOverASweepOfModes`: a guide's one mode swept over its width. Its checks:
    - the global graph over the width, the one mode last, named as asked;
    - TE10's index;
    - `fem_evaluate`'s names, as globals and at a point, with comparisons no names
      (`a == a`, `emw.neff >= 0`);
    - a study added and deleted leaving no solution old;
    - a study changed making its own solution old alone, and a renamed one neither.

Each of the 14 changes was broken in turn, and its test failed.

The full ctest, the MCP scenario script and the DMG's checks pass.

Separately, CI's Linux job (Debug, ASan and UBSan) now has 150 minutes, not 90
(`138aba0`). The quantum wells' tests took it past 90 minutes on a busy runner.

## Re-check on 45d857e and fc28dc0

The reviewer re-checked those two commits on the reviewer's model, the bundled
`pn_diode` and test copies.

**Confirmed:**

- the current at 0 V (0 A exactly);
- the currents (I_a + I_k about 10⁻²² A, I_a within 1 % of q ∫ R);
- G (symmetric to 8 digits);
- `pn_diode`'s graphs (`<dep semi.Ckk Va>`, drawn);
- the 1D plot groups' three fixes and dB/cm.

**Withdrawn:** the slow mode step (25.6 s against 26.2 s, the 138 s most likely the
machine busy). The per-study staleness and the help text were not re-tested.

Two new notes, both done here:

| Review | Now |
|---|---|
| Showing a plot group again keeps the old diagram in its `.dpl` and adds a new one under it. `corrugated_ps_dd_pg1d2.dpl` had the stale `emw.loss_100` diagram (traces `<invalid>`) above the working `Loss` one. Replace the stale one, or rename its traces with the variable. | **Replaced.** A plot group's display is its own: shown again, its diagrams of its title other than the one showing its traces now are deleted. A new one takes the first one's place and size, so one moved or resized by hand stays so. A diagram of the user's (another title) is kept. On a copy of the reviewer's display, shown again: one diagram, `Loss`, where it was (80, 510). Reproduced on fc28dc0 first: a graph relabelled and a second added left three diagrams there. |
| A label's text in brackets is dropped from the variable's name ("Loss (dB/cm)" → `Loss`). Graphs have no unit on their y axis: `pn_diode`'s C–V shows `semi.Ckk` from 0 to 1, no "pF". Using the graph's unit in the axis label (`semi.Ckk (pF)`) would cover both. | **The y axis in the traces' units.** Each trace is named with its unit: the graph's own (`semi.Ckk (pF)`), a global's own (`emw.loss (dB/m)`), else its SI unit (`emw.normE (V/m)`), and none for a number (`emw.neff`). An expression of a global with a unit of its own has none (`emw.loss/100` is dB/cm, not the 1/m its dimension would say). A label that says its unit says it once (`Loss (dB/cm)`). With several traces the label names them all and a legend is on, since the axis no longer names them in their colours. Shown again with a unit changed and the traces not, the axis is said anew. The variable keeps its bracket-free name: a unit is the axis's, not the dataset's. |

**Tests (`test_multiphysics_doc`):**

- `aPlotGroupShownAgain`. Its checks:
  - a guide's loss graph (`emw.loss/100`, its axis saying no unit) shown, then moved
    and resized by hand;
  - relabelled "Loss (dB/cm)" in dB/cm: one diagram, `Loss`, where it was and as
    large, its axis "Loss (dB/cm)";
  - another diagram of the title beside it, as the reviewer's display had: gone;
  - a user's diagram kept;
  - a second graph: two traces, "Loss (dB/cm), emw.neff", a legend;
  - a unit changed: the axis said anew;
  - a line graph without a unit: "emw.normE (V/m)".
- `graphsGoToAQucsDataDisplay`: the heater's axis is "ht.Tmax (degC), T_1 (degC),
  T_2 (degC)".

Each of the 12 changes was broken in turn, and its test failed. Deleting the current
diagram too crashes the test, since the display touches the diagram gone: the break
script counts a crash as a failure now, where it read only "FAIL!".
