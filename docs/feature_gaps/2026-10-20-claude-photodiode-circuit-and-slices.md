# A photodiode under strong light, 1: its load circuit and its length in slices

The question was whether Qucs-S supports all the physics of photodiodes, at low optical power and at the high power that saturates their bandwidth. The answer listed what was there and nine things missing. The user asked for all of them, starting with the load circuit and length slicing. This is that start.

**Before this round:**
- Contacts were held at their voltages. The load resistor only entered one small-signal formula (`semi.HopRL`), so the bias collapse that a large photocurrent causes was missed: no I·R drop at rest, and no load voltage swing in time.
- A waveguide photodiode's 2D cross-section took the mean of the light over its length, (1 − e^(−2αL))/2αL. Under strong light the input end saturates first, where the light is several times that mean (6.6× for the germanium example), so saturation was predicted at too high a power.

**Now:**
- A metal or Schottky contact can sit behind a circuit.
- An Optical Generation can cut the device into slices along its length.
- Both are solved together with the carriers, at rest, in time and small-signal.

## The contact's circuit

| Setting | Meaning |
|---|---|
| **Circuit** `none` | Held at V0, as before |
| **Circuit** `series` | V0, then Rs and the load RL in series: at rest and in time alike |
| **Circuit** `biastee` | A bias tee: V0 through Rs at rest; in time RL carries the current's change from rest (the tee ideal) |
| **Rs**, **RL** | The series (contact and access) resistance and the load |
| **Cp** | A capacitance across the device at the contact (a pad's), to ground |

**How it is solved:**
- The contact's own voltage Vc is an unknown, solved with the carriers in Newton's method.
- The contact's nodes follow it, V = Vc + their offset.
- Its row is V0 − Vc − R (I + Cp dVc/dt), with a bias tee's RL·I0 in time. R is the resistance in the current's path: Rs + RL in series, Rs for a tee at rest, Rs + RL for a tee in time.
- I is the current exactly as the solver reports it: the edges' currents weighed toward the contact, the carriers' change and the metal's charge's change in time. Its change with every unknown is in the Jacobian.
- In time Vc is kept with the solution, so a step's history covers it, and Cp's current comes from it.
- **Small-signal:** the device's admittance Y and the light's currents h are found with the contacts held, as before. Then the circuit is solved on them: (Y + jωCp + 1/Z) v = −h on the circuits' contacts, with Z = Rs + RL. The light's current through the load is −v/Z.

**Results:**

| Variable | What it is |
|---|---|
| `semi.Vc_<name>` | The contact's own voltage |
| `semi.V_<name>` | The source's V0 |
| `semi.IL_<name>`, `semi.VL_<name>`, `semi.PL_<name>` | The load's current toward the device, its voltage and its power. For a bias tee these are none at rest |
| `semi.Hc_<name>` | Small-signal: the light's response through the load, per pair |

## The walk to a solution

A stationary solve walks from the last solution to the contacts' voltages in steps. With the light switched on, or a resistance swept, there was no voltage to walk: Newton's method jumped all at once, and failed for a lit diode through 300 kΩ, whose bias goes forward.

Now a parameter λ from 0 to 1 is walked beside the voltages:
- the light goes from what the solution was solved with to all of it;
- each circuit's resistance goes from its value as solved to its own.

The full jump is still tried first, as before. λ is walked, halving on failures, only when that fails.

## Slices along the length

**Setting:** the Optical Generation's **Slices along the length** (1 by default).

**What it does:**
- Each slice is the cross-section at its place along the device. The slices absorb equal shares of the first source's light: z_k = −ln(1 − (k/S)(1 − e^(−2αL)))/2α, so they are finest at the input, where the light is strongest.
- Each source's light in a slice is the mean of e^(−2αz) over the slice, relative to the whole length's mean.
- Each slice's current is weighed by its share of the length.

**How it is solved:**
- All slices are in one Newton system. They share only the contacts, as one metal: a circuit's row takes all their currents.
- Each slice keeps its own saturated mobilities, since its own fields saturate them.
- In time each slice has its own history. Small-signal, each slice's admittance and light response are found at its own state, then summed by their shares.
- The fields shown are the first slice's, at the input end.
- `semi.H` is the slices' mean, by their shares: a 2D heat model's cross-section is the whole length's.

**Results:** `semi.Is<k>_<contact>` is slice k's share of the contact's current, `semi.zs<k>` is where it starts, and `semi.slices` is the count.

**Not combined with slices** (refused, with the reason): a laser, Schrödinger-Poisson, stimulated recombination, an axisymmetric model, a 1D solve.

## Found on the way, and fixed

- **Held mobilities.** Saturated mobilities are held once the potential settles, or after 15 steps, so Newton's method converges. The solution was then a solution of the mobilities as they were when held, not of its own. Two solves of the same device by different paths differed: a slice 0.2–0.8 % from itself solved alone.
  - Now a walk's last solve is polished when it held them before the potential settled: the mobilities are updated at the solution and the solve continues, while they move by more than 10⁻⁶, at most eight times. Held once settled, they are the solution's own already, so no polish (and no time) is spent there.
  - If the polish fails, the solution and its mobilities are restored together (once they were not, and the circuit row disagreed with the reported current by 1.6 mV).
- **The carriers' heat `semi.H`.** Its recombination part, q R (φp − φn), was read at a point, R between the nodes. ∫`semi.H` was I V to 10⁻³ only.
  - Now it is taken by the nodes as assembled, and each triangle gets the mean of its corners'. ∫`semi.H` equals I V exactly (`theCarriersHeat`: 1.36392 mW and 1.36392 mW).
  - It is stored with the solution, so sliced it is the slices' mean, not the first slice's carriers read at a point.
- **Noted, not changed:** a frequency step makes its semiconductor anew, so `semi.H` read at a frequency step's solution has no Joule heat. Read it at rest.
- **Franz-Keldysh and the bias.** A model's light is absorbed in its dark field: its mode is solved in a stationary step's field before the light. So a lit diode behind a circuit absorbs as at V0, not at its own voltage. That is item 6 of the nine (the light iterated with the carriers), next.

## Checks

The test is `test_fem_photodiode_power`:
- a silicon p-i-n under uniform generation, 10 µm long;
- a germanium waveguide photodiode on silicon, 15 µm long. In these checks its Franz-Keldysh absorption is off, so a device held at another voltage absorbs the same light.

| Check | Result |
|---|---|
| p-i-n, 10 µA of light, anode behind Rs = 0, 50, 100, 300 kΩ | Vc −2, −1.519, −1.039, +0.759 V (past nought at 300 kΩ, found by the walk); V0 − Vc − R I within 10⁻¹⁴ V; the device held at Vc carries the same current to 4·10⁻¹⁶ |
| Small-signal through 50 Ω, Cp none | `semi.Hc` equals `semi.HopRL` to 10⁻⁹ |
| 1 pF across 50 Ω at 20 GHz | 0.097086 against the RC's 0.097109 |
| A bias tee into 100 kΩ, the light modulated 5 % at 2 GHz, in time | The load current's amplitude is the small signal's to 1.0000; its mean is 9·10⁻¹⁶ A; the contact's mean is its source's, −2.0000000 V (RL I0 would be 0.096 V) |
| Dark, the source stepped −1 to −1.1 V through 1 MΩ, 10 fF across | Vc charges with τ = R (Cp + Cj) = 10.35 ns, Cj from the small signal, to 7·10⁻⁶ V |
| A Schottky diode (n-Si 1e16 cm⁻³), dark, stepped −1 to −1.1 V through 1 MΩ: its junction's charge on the metal | Vc charges with R Cj = 1.168 ns to 2.4·10⁻⁴ V; the source less R I to 5·10⁻¹³ V |
| 10 mA's worth of light switched on through 100 kΩ, dark first at −2 V | Vc +1.02 V, reached from the dark solution in 15 steps of the walk's λ (without it: from rest, again) |
| Germanium, 10 µW, 4 slices | Their starts 0, 0.713, 1.718, 3.429 µm, as the formula gives to 10⁻¹²; each carries a quarter of the current; together the length's mean light's current to 1.5·10⁻⁵; the small signal at 20 GHz to 10⁻³ (the first slice 5.3× denser); the capacitance 1.93314 fF against 1.93281 |
| The same: the slices' heat | Each slice solved alone with its own light, by its share: 0.916614 W/m; together 0.916614 W/m |
| 5 mW through 100 Ω | The input slice collects 0.928 of its pairs, the last 0.970; the device 0.947 against 0.964 with the mean light; Vc −0.409 V (from −1) |
| The same: slice 1 alone, held at the device's Vc with its own light | Its current, by its share, the device's slice 1's to 1.1·10⁻⁸ |
| Two slices through a bias tee into 1 kΩ, modulated 5 % at 10 GHz, in time | The load's amplitude is the sliced small signal's to 1.0000 |
| Dark-and-lit circuits against the device held at Vc, with velocity saturation, 1 and 4 slices | 8·10⁻¹⁰ and 10⁻⁷ |

## Sanitized builds

Under the sanitizers (Debug and ASan, with Eigen's sparse LU as on CI's Linux runners):

| Function | Time |
|---|---|
| `aSeriesResistanceDropsTheBias` | 88 s |
| `theLoadInFrequency` | 33 s |
| `slicesShareTheLight`: two slices, at rest only | 129 s |

The steps in time, the strong light's walks and the sliced small signal took 166 s to over QtTest's 300 s there. They run on Release builds alone, as the semiconductor's other benchmarks do, and are skipped on sanitized ones with the reason said.

## Break checks

Each break was applied, built and run, then the source restored:

| Break | Caught by |
|---|---|
| The circuit's row with R I's sign turned | `aSeriesResistanceDropsTheBias`: V0 − Vc − R I 0.96 V |
| The circuit's row without its current's Jacobian | `aSeriesResistanceDropsTheBias`: past QtTest's limit |
| Cp's current left out of the row | `aCapacitanceChargesThroughTheResistance` |
| A bias tee's rest current not taken off in time | `aBiasTeeInTime`: the contact's mean 0.096 V off |
| The small signal's Cp left out | `theLoadInFrequency`: 6.4 times the RC's |
| A bias tee's rest resistance with its load | `aBiasTeeInTime`: the contact at rest not at its source |
| No walk of the light and resistances | `strongLightThroughALoad`: solved from rest again |
| The metal's charge's change left out of the row's current | `aSchottkyChargesThroughTheResistance` |
| The carriers' change left out of the row's current | `aBiasTeeInTime` |
| The slices weighed alike | `slicesShareTheLight`: the heat 2.7 against 0.92 W/m |
| The slices all the mean light | `slicesShareTheLight`: the heat |
| The slices of equal lengths | `slicesShareTheLight`: a start 7.8 µm off |
| Each slice the first's history | `slicesThroughATeeInTime` |
| The slices sharing one set of mobilities | `theStartSaturatesFirst`: slice 1 alone 6 % off |
| The slices' admittances not weighed | `slicesShareTheLight`: the capacitance four times |
| The slices' heat not weighed | `slicesShareTheLight`: the heat 10.8 against 0.92 W/m |

**Not caught at first, five:**
- **A bias tee's rest current:** the load current's mean stayed nought. The contact's mean voltage is now checked.
- **No walk:** each case solved without it from rest. Its own case, light switched on with no voltage to walk, now checks that it is not solved from rest.
- **The metal's charge:** an ohmic contact's hardly changes. A Schottky diode's is now checked.
- **The slices' admittances:** the light's response does not read them. The capacitance is now checked.
- **The heat:** the break was of the Joule part alone, which `semi.H` no longer reads. The break is now of the whole.

## Next

Seven of the nine remain, in this order:
- the light's own heat, and the lattice temperature in the band gap, the saturation velocity and the impact ionization;
- the light iterated with the carriers (Franz-Keldysh, band filling, the temperature's edge shift);
- thermionic emission and tunnelling across heterointerfaces;
- hot electrons (energy balance, velocity overshoot, the transferred electrons' negative mobility);
- distortion (harmonics, compression);
- noise (shot, thermal, an APD's excess);
- the APD's convergence wall past −22.7 V.
