# A photodiode under strong light, 5: hot carriers

The third of the nine missing pieces of a photodiode's physics (`2026-10-20-claude-photodiode-circuit-and-slices.md` lists them).

**Before:** a carrier's velocity was its local field's. Saturation was Caughey and Thomas's, driven by the field along its current or by the whole field.
- Carriers entering a high field cool could not run past vsat. A short transit layer's velocity overshoot, the reason a thin collector beats the saturated drift time, was missing.
- GaAs's and InP's electrons could not slow down past their peak velocity: no transferred electrons, no negative differential mobility.

**Now:** the Mobility Model has two more choices under High-field velocity saturation.

## The carriers' temperature (energy balance)

**Driving force: The carriers' temperature.** Each carrier's temperature comes from its energy balance, by the nodes' boxes:

Σ S + (3/2) k n V (T − TL)/τw = the field's gift

| Term | Meaning |
|---|---|
| S | The energy each edge carries: its particles' (5/2) kT each, and the carriers' conduction of heat, κ = (5/2)(k²/q) µ n T (Wiedemann and Franz). Scharfetter and Gummel's fitting covers convection and conduction together, D (B(−P) Ti − B(P) Tj) |
| The field's gift | Each triangle's particle flux (fitted to its edges' flows, each weighed by its face) times its band's slope, over its volume, a third to each corner |
| (3/2) k n (T − TL)/τw | The relaxation to the lattice, τw the **energy relaxation time** (electrons 0.3 ps, holes 0.25 ps by default) |

Contacts are at the lattice's temperature.

**The mobility** is the saturation model's at the field that heats the carriers so where everything is uniform: µ(F) F² = (3/2) k (T − TL)/(q τw). So:
- where the field is uniform, the result is the field-driven saturation's exactly;
- carriers entering a high field still cool are faster than saturated: overshoot.

**Results:** `semi.Tn`, `semi.Tp` (the physics' T where there is no energy balance).

**How it is solved:**
- **Along a stationary walk,** the mobilities are the field's (along the current).
- **At the walk's end,** the temperatures are solved in turn with Newton's method, until the mobilities move by less than 10⁻⁶.
- **Each temperature update** is a per-node secant step on its residual. In saturation, a velocity's temperature's mobility gives back about vsat²/v, so a plain update has slope −1 and swings without end.
- **A global factor** halves the steps whenever the updates grew, and recovers by half again when they fall. A space-charge-limited layer's current moves all its temperatures together.
- **Unsettled after 200 updates,** the solve fails and says so.
- **In time,** the temperatures are updated with each Newton step. **Small-signal,** they are held at the operating point's.

## Transferred electrons

**Electrons' velocity: Transferred electrons.** The mobility is (µ0 + (vsat/F)(F/E0)⁴)/(1 + (F/E0)⁴): past its peak near E0, the electrons move to the heavier valleys and their velocity falls toward vsat (Gunn's effect). **E0** is GaAs's 4 kV/cm by default. It combines with either driving force, the temperature's included.

## Found on the way

- **A bar near saturation is not uniform with hot electrons.**
  - At 10 V across 2 µm of n-silicon (1e17), the electrons enter cooler than the field and run a little past vsat. The current exceeds q Nd vsat A, the bar fills with 2.5 % more electrons than its donors, and its field rises along it from 0.8 to 8.8·10⁶ V/m.
  - The energy carried along that rise, (5/3) τw v F′/F, was 4 % of the field's gift. That is real: in a saturated bar the field relaxes along it only slowly.
  - The uniform check therefore runs at 1 V.
- **Heating by edge, half each end,** summed to the device's J·E exactly, but not node by node. Heating by triangle is exact where the field is uniform.
- **A triangle's flux fitted unweighed** (each edge's flow over its face) let a boundary edge's sliver of a face take one node to 10¹² K. It is now weighed by the faces.
- **The temperatures updated inside every Newton step** of the walk took 518 walk steps and 520 s. Walked with the field's mobilities and solved in turn at the end: 13 steps, 12 s, the same answer.
- **A time step's start** showed setup's values in its first snapshot: the low field's mobilities (0.14 m²/(V s) against the saturated 0.114) and, hot, 300 K. Its first Newton step also began from them. Now its mobilities, and hot carriers' temperatures iterated at that state, are made the state's own before the snapshot. This holds for every saturated model's time steps, not only hot ones.

## Checks

The tests are in `test_fem_photodiode_power`:

| Check | Result |
|---|---|
| n-silicon (1e17), 2 µm at 1 V (0.57 vsat): the electrons' heating in its middle | 367.67 K against TL + (2/3) q τw v F/k = 367.67 K (v and F its own there), to 1 % of the heating |
| The same: warming in at the cathode | Over 70.5 nm (its rise from 40 to 160 nm in, fitted) against 1/\|λ\| = 68.9 nm, from κλ² − (5/2)k n v λ − c′ = 0 linearized about the middle. c′ is the relaxation's (3/2)k n/τw less the gift's own rise with T: with the current held, a hotter electron's lower mobility steepens its field, so c′ = c·2(1 + s)/(2 + s), s = d ln µ/d ln F = −(v/vsat)². Conducted alone, 47.6 nm |
| The same: the current | 185.0 A against 183.8 A driven by the field, to 1 % (the contacts' cool electrons) |
| n⁺-n-n⁺ silicon (1e19, 1e16; 100, 200, 100 nm) at 3 V | The fastest electrons inside the n layer 2.07·10⁵ m/s, past vsat (1·10⁵); driven by the field 1.003·10⁵. Current 68.7 A against 41.6 A |
| The same bar in time, stepped on from rest 3 ps | The current its rest's to 10⁻⁹; its start's temperature and mobility shown as at rest (367.726 K, 0.114294) |
| n-GaAs (1e16), 10 µm, transferred electrons: at 2, 4, 6, 8 kV/cm | q n v A with µ0 its own (0.85 m²/(V s), at 10 mV) and vsat 7.2·10⁴ m/s, to every digit printed; past the peak, 6 and 8 kV/cm, less current |

**Sanitized builds:** the uniform bar takes 122 s, the bar in time 62 s and the transferred electrons 6 s. The n⁺-n-n⁺'s 500 Newton steps took past QtTest's 300 s there, so it runs on Release builds alone.

**Corrected after CI's run of 21 October.** The first check took the rise from two points, 30 and 60 nm in, on a 20 nm mesh:
- this machine's mesh gave 55.5 nm, against 57.5 nm from a balance without the gift's rise;
- CI's Linux mesh gave 75.9 nm, and meshes of 17.5 to 25 nm here gave 55 to 85.

Fitted from 40 to 160 nm across four lines along the bar, the solution's length is 70.0 to 71.6 nm on meshes of 5 to 25 nm (70.4 at 5 and 10). The balance with the gift's rise gives 68.9; the check holds to 5 %.

## Break checks

| Break | Caught by |
|---|---|
| The relaxation without its 3/2 | `hotElectronsWhereUniform`: 395 against 363 K |
| No energy carried with the current | `hotElectronsWhereUniform`: warming in over 49.1 nm against 68.9 (conducted alone, 47.6) |
| The temperature's field the local field's | `velocityOvershoot`: no overshoot, 1.003·10⁵ m/s |
| The transferred electrons' square, not fourth power | `transferredElectrons`: 241 against 263 A at 2 kV/cm |
| The flux fit unweighed by the faces | `velocityOvershoot`: 2.6 m/s, the temperatures run away |
| No final polish of the temperatures | `hotElectronsWhereUniform`: 333 against 373 K |
| The gift a hole's sign for electrons | `hotElectronsWhereUniform`: 219 against 381 K |
| A time step's start from setup's mobilities | `hotElectronsInTime`: 0.14 against 0.114 |

**Not caught at first, one:** no energy carried with the current. The overshoot remained, since conduction alone also delays the heating. The warming length at the cathode is now checked.

## Next

Three of the nine remain:
- distortion (harmonics, compression);
- noise (shot, thermal, an APD's excess);
- the APD's convergence wall past −22.7 V.
