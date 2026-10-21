# A photodiode under strong light, 2: the light's heat and the lattice's temperature

The question was whether Qucs-S supports all the physics of photodiodes, at low optical power and at the high power that saturates their bandwidth. Nine things were missing; the user asked for all of them. The first round (`2026-10-20-claude-photodiode-circuit-and-slices.md`) added the load circuit and the slices along the length. This round is the fifth of the nine: the light's own heat, and the lattice's temperature in the band gap, the saturation velocity and impact ionization.

**Before this round:**
- `semi.H`, a heat physics' source, was the carriers' Joule heat and q R (φp − φn). The light's own heat was not in it: each photon's energy above what its pair carries, and the photons absorbed without making a pair.
- A lattice temperature TL (a heat physics' `ht.T`) scaled the mobilities alone.
- The saturation velocities and the impact ionization coefficients were the same at any temperature, the physics' T or TL.
- Franz-Keldysh absorption took the direct gap as given, at any temperature.

**Now:** all of these follow the temperature. A strongly lit photodiode coupled to a heat physics warms by its I V and its light together, and its intrinsic density, saturation velocities, ionization and absorption edge follow the lattice there.

## The light's heat

`semi.H` now holds the light's heat too:
- **What it adds:** the energy each pair is made with, less what the pair carries, q G (φp − φn). The pair's excess over its quasi-Fermi split cools to the lattice at once.
- **A mode's light:** ħω/η for each pair. The photons absorbed across the gap that make no pair (η < 1) heat too.
- **A User-Defined Generation:** a new **Photon energy** (`Eph`). Empty, the pairs are made at the gap, with no excess.
- **At a contact's nodes:** pairs made there go to the contact at once, so all of their energy is heat.

So the device's ∫ `semi.H` is its contacts' I V and the power absorbed together, exactly.

| Result | What it is |
|---|---|
| `semi.H` | The heat the carriers and the light leave (W/m³) |
| `semi.Hlight` | The light's part alone |
| `semi.Pabs` | The power the pairs are made with (W): every photon absorbed across the gap, ħω/η a pair |

## The lattice's temperature

Given TL (`ht.T`, say), each part of the device takes its material's values there:

| What | At TL |
|---|---|
| Intrinsic density | Its material's own ni(TL) = √(Nc Nv) e^(−Eg/2kTL), from its Eg(T), Nc(T) and Nv(T) (the library's Varshni gaps and T^1.5 densities, or a user material's own expressions). They are tabulated from 150 to 1200 K by 5 K and interpolated cubically. |
| Band gap shown | `semi.Ec`, `semi.Ev`: the material's own gap at TL |
| Saturation velocity | Quay's vsat300/((1 − A) + A TL/300 K) |
| Impact ionization | a and b times γ = tanh(ħω/2k·300 K)/tanh(ħω/2kTL) (van Overstraeten and de Man; the carriers' mean free path between optical phonons, Crowell and Sze's) |
| Mobility | (TL/T)^−ζ, as before |

**Quay's A** (Quay et al. 2000, as Palankovski's table 3.31 has them; the Si and Ge entries of the Lumerical™ material database carry the same as their temperature alternative):

| Material | Electrons | Holes |
|---|---|---|
| Si | 0.26 | 0.63 |
| Ge | 0.55 | 0.61 |
| GaAs | 0.56 | 0.41 |
| AlAs | 0.55 | 0.3 |
| InAs | 0.57 | 0.3 |
| InP | 0.68 | 0.3 |
| GaP | 0.30 | 0.3 |
| GaN, AlN | 0.15, 0.135 | none |

Every other material in the library has A of nought, not silicon's. The Mobility Model's new **Avn** and **Avp** give A; their defaults are silicon's, as the feature's other defaults are.

**The phonon** for impact ionization (the Impact Ionization feature's new **Optical phonon energy**, `Eph`, or the material's `iiEph`): silicon's 63 meV is van Overstraeten and de Man's own. The others are their LO phonons at Γ: Ge 37 meV, GaAs 36, AlAs 50, InAs 30, InP 43, GaP 50, GaN 92, AlN 110, InN 73, GaSb 29. With 0, the coefficients are the same at any temperature.

**At the physics' T too:** the saturation velocities and the ionization coefficients are the library's 300 K values, so they now follow T as well as TL. A model at T other than 300 K, with velocity saturation or impact ionization, solves differently than before.

**What stays at the physics' T:** the carriers' kT. Their diffusion and the built-in potential are T's; only their bands are TL's.
- A device at 300 K with its lattice at 350 K has the intrinsic density and gap of one at 350 K, exactly. Its generation current is 2 % apart: where n or p is near ni at a depleted layer's edges depends on kT.
- At −25 V the field across the p-i-n's i layer was 1.7 % weaker with TL 400 K than with T 400 K: the built-in potential is kT ln(Na Nd/ni²).
- The gap's change is taken half from each band, about the intrinsic level.

**Not of TL:** the lifetimes, the Auger and radiative coefficients, the interface traps' levels, the electron affinities and the Schottky barriers.

## The absorption edge at temperature

The Wave Equation's Direct-gap absorption has a new **Absorber's temperature** (`Tea`), a heat physics' `ht.T` for instance. The direct gap there is Varshni's, Eg − α (T²/(T + β) − (300 K)²/(300 K + β)), about its 300 K value.

The library's Germanium and Germanium on silicon now have the Ioffe Institute's α 5.82·10⁻⁴ eV/K and β 296 K (`Egdalpha`, `Egdbeta`); a user-defined gap takes them on the feature. Germanium on silicon's mode at 1550 nm absorbs 2114 /cm at 300 K and 2653 /cm with its absorber at 350 K.

## Lumerical files

The importer reads a "Temperature Dependent" saturation velocity's Av into Avn and Avp. A constant one gets nought, as the Lumerical file has it.

## Found on the way, and fixed

- **A heterojunction's heat.** `semi.H`'s recombination part was spread over each node's recombining parts' volume, not the volume the integral takes. In germanium on silicon, dark, ∫ `semi.H` was 3 % off I V. Now it is spread over the corners' volume, and the identity is exact.
- **The Joule heat under saturation.** Each triangle's share of an edge's heat was its conductance at the solution over the edge's conductance with its mobilities held, so the shares did not add up to one: 2·10⁻⁶ of I V. Now the shares are over their own sum.
- **The sliced time run under the sanitizers.** `slicesInTime` (round 1) took past QtTest's 300 s there. It now runs on Release builds alone, like the other runs in time.
- **CI's Linux run on bc35507** timed out in `spectrumAsLumerical` on a slower runner. Its normalized wells now take a line of 0.25 nm elements under the sanitizers: 236 s became 87 (2f95a05).

## Checks

The tests are in `test_fem_photodiode_power`:

| Check | Result |
|---|---|
| p-i-n, a User-Defined Generation, reversed 2 V and at 0 V | ∫ `semi.H` = I V + `semi.Pabs` to 10⁻⁹; 1.120000 eV a pair (the gap); at 0 V ∫ `semi.H` = Pabs |
| The same, photons 2.5 eV | Pabs = q Gop · 2.5 eV to 10⁻¹²; ∫ `semi.H` = I V + Pabs |
| Germanium on silicon at 1550 nm, 1 mW: one slice, two, η 0.8 | Pabs = Gop ħω/η to 10⁻¹²; ∫ `semi.H` = I V + Pabs to 2·10⁻⁹ (Newton's tolerance with saturated mobilities held) |
| The same, dark | ∫ `semi.H` = I V to 3·10⁻¹⁵ (3 % off before) |
| p-i-n dark at −5 V: 300 K, its lattice at 350 K, 350 K | ni 6.6759·10¹⁵, 2.31481·10¹⁷, 2.31481·10¹⁷ /m³; the gap 1.120000, 1.106716, 1.106716 eV, Varshni's to 10⁻⁹ eV; the current 35 times, 2 % from the device at 350 K |
| A silicon bar (1e17) at 20 V, saturated: 300 K, its lattice at 400 K, 400 K | µE/√(1 + (µE/vsat)²) with Quay's vsat: 1303.74, 877.877 and 877.877 A against 1303.74 and 877.877, to 10⁻⁶ |
| p-i-n at −25 V, van Overstraeten and de Man's: its lattice at 400 K, and the physics at 400 K | The ionization integrals the same as user coefficients times γ(400 K) = 1.160760 at 300 K, to every digit; In 0.0129 at 300 K, 0.00308 with its lattice at 400 K |
| Germanium on silicon's mode at 1550 nm, Franz-Keldysh in its own field, its absorber at 350 K | α the same as with a gap of 0.750522 eV given, to 10⁻⁹; 2653 against 2114 /cm at 300 K |
| The p-i-n lit (3 mA's worth, photons 1.5 eV) at −5 V, its heat physics fed `semi.H`, TL `ht.T` | Solved in turn to rest; ∫ `semi.H` = I V + Pabs to 10⁻⁶; its middle at 310.1 K |

## Sanitized builds

Under the sanitizers (Debug and ASan, with Eigen's sparse LU as on CI's Linux runners):

| Function | Time |
|---|---|
| `theLightsHeat`: the p-i-n's three, germanium's η 0.8 and dark | 118 s |
| `theLatticeInTheBands` | 31 s |
| `saturationOfTheLattice` | 19 s |
| `ionizationOfTheLattice`: without the 300 K reference | 90 s |
| `theEdgeOfTheLattice`: without the 300 K reference | 95 s |

Each germanium run took some 50 s there: those whose checks are not exact identities (one slice and two at η 1, the 300 K references) run on Release builds alone. So does the self-heating loop, like the semiconductor's other turns with a heat physics.

## Break checks

Each break was applied, built and run, then the source restored:

| Break | Caught by |
|---|---|
| The pairs' energy left out of the light's heat | `theLightsHeat`: ∫ `semi.H` I V alone |
| The recombination heat by the recombining parts' volume | `theLightsHeat`: germanium 6·10⁻⁵ off |
| An edge's Joule shares over its held conductance | `theLightsHeat`, dark: 2·10⁻⁶ off |
| η left out of a pair's energy | `theLightsHeat`: Pabs a quarter short |
| A User-Defined Generation's photon energy ignored | `theLightsHeat`: Pabs at the gap's 1.12 eV |
| No thermal shift of the bands | `theLatticeInTheBands`: ni 300 K's |
| The bands not made again when TL changes | `theLatticeInTheBands` |
| The band extras not refreshed | `theLatticeInTheBands`: ni shown 300 K's |
| The gap shown the carriers' (its whole shift) | `theLatticeInTheBands`: 0.937 against 1.107 eV |
| vsat at TL not applied | `saturationOfTheLattice`: 902.7 against 877.9 A |
| Quay's at the physics' T not applied | `saturationOfTheLattice` |
| γ at TL not applied | `ionizationOfTheLattice`: In 0.0116 against 0.0031 |
| γ at the physics' T not applied | `ionizationOfTheLattice` |
| γ on a alone, not b | `ionizationOfTheLattice` |
| The direct gap's Varshni shift turned | `theEdgeOfTheLattice`: α 568 against 2653 /cm |
| The absorber's temperature ignored | `theEdgeOfTheLattice`: 2114 against 2653 /cm |

**Not caught at first, one:** the Joule shares over the held conductance. Lit, its error was under the check's 10⁻⁸; dark it is 2·10⁻⁶. The dark germanium is now checked to 10⁻⁹.

## Next

Six of the nine remain, in this order:
- the light iterated with the carriers (Franz-Keldysh in the device's own field as it is lit, band filling, the temperature's edge shift);
- thermionic emission and tunnelling across heterointerfaces;
- hot electrons (energy balance, velocity overshoot, the transferred electrons' negative mobility);
- distortion (harmonics, compression);
- noise (shot, thermal, an APD's excess);
- the APD's convergence wall past −22.7 V.
