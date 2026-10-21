# A photodiode under strong light, 7: its noise

The ninth of the nine missing pieces of a photodiode's physics (`2026-10-20-claude-photodiode-circuit-and-slices.md` lists them): shot noise, thermal noise and an APD's excess noise, and from them a noise-equivalent power.

**Before:** a frequency step gave a photodiode's response, but no noise.

**Now:** the semiconductor physics has **Noise**. With it on, each frequency step also gives each contact's noise, by the impedance field method.

**The sources.** Each microscopic source is a Poisson process, carried to the contacts by its own Green's function:

| Source | Strength (one-sided) |
|---|---|
| Each edge's carriers' diffusion | 4q² D n per volume. Along the edge's constant-current profile n(s) = ni + (nj − ni)(e^(ds) − 1)/(e^d − 1), with the sources' weight 1/n there, this is 4q w n_h, n_h the profile's harmonic mean: d ni/((1 − e^(−d)) φ(r)), r = nj/(ni e^d), φ = ln r/(r − 1) |
| Each node's pairs made: the light's, a generation's | 2 × the rate |
| Each node's pairs recombined both ways: Shockley-Read-Hall (np + ni²)/D, Auger's, radiative | 2 × the rate |
| Each ionization, at its triangle's corners | 2 × the rate |

The diffusion term gives exactly 4kT G at equilibrium (Nyquist's), and n under uniform drift.

**The Green's functions.**
- They come from the transposed small-signal system, A^T λ = g, with g the contact's current's coefficients: one solve per contact, a second factorization per frequency.
- An edge's source is itself a current through the contact's weights, w_i − w_j (Ramo's). The carriers' answer to it comes on top.

**Results:**

| Result | What it is |
|---|---|
| `semi.Sn_<contact>` | The short-circuit noise current spectral density (A²/Hz) |
| `semi.in_<contact>` | Its square root (A/√Hz) |
| `semi.Fshot_<contact>` | Its ratio to the contact's full shot noise, 2q\|I\|: 1 for a photodiode, M·F for an APD |
| `semi.SnL_<contact>` | Through a circuit's load (the circuit's own resistors' noise not in it) |
| `semi.NEP_<contact>` | Lit by an Optical Generation, the noise-equivalent power (W/√Hz): the noise over the responsivity at the frequency |

**Where carriers ionize,** the diffusion sources are left out. In a local model the carriers' diffusion there changes no ionization: a carrier carried forward ionizes on the way what its shorter way ahead loses. At a junction's dense side it is thermal motion, which the field heats nothing. The discrete forms leaked it, multiplied. There the noise is the ionizations' and the primary's, McIntyre's picture.

## Found on the way

- **Diffusion noise as the two streams' hops.** Taking an edge's two Scharfetter-Gummel streams as independent Poisson hops, 2q(forward + backward), is exact at equilibrium. In a drift field it gives each edge the shot noise of its drift current, d/2 = ΔV/2Vt times too much. A bar at 3 V was 1.33 times its thermal noise. The harmonic mean replaces it.
- **The direct term.** Without an edge source's own current through the weights, a bar at equilibrium had no noise at all (10⁻⁴⁰ A²/Hz). Its λ there is all but nought: the weights are harmonic in the conductances.
- **The APD's diffusion noise multiplied.**
  - Coupled whole, it gave F = 5.1 against McIntyre's 2.1.
  - Letting a noise flux ionize as the edge's does still left 3 to 6, varying with the mesh: an edge with a sliver of a face dominated its triangle's fit, and a junction's dense thermal noise ionized.
  - Left out where the triangles ionize, F converges on McIntyre's: 4.03, 2.92, 2.39, 2.10 at 40, 20, 10 and 5 nm.

## Checks

The tests are in `test_fem_photodiode_power`:

| Check | Result |
|---|---|
| A bar of n-silicon (1e17, 2 µm) at equilibrium, 1 MHz | 3.71623319·10⁻¹⁸ A²/Hz, 4kT G the same to every digit printed |
| The same at 3 V (its mobility constant) | 4kT I/V to 0.4 %: its thermal noise whatever its current. The excess grows as the field squared, 3.8 % at 10 V, 2.5 to 5.3 % there as the mesh goes from 100 to 12.5 nm |
| The p-i-n, lit (9.6 µA) at −2 V | Its noise over 2q I 1.00000 |
| The same through 50 Ω in series | Its load's noise the same to 10⁻³ |
| Germanium on silicon at 1550 nm, 10 µW | NEP 1.626·10⁻¹² W/√Hz against Pin √(2q/I) = 1.629·10⁻¹² |
| A silicon APD: p⁺-i-n⁺ (2 µm of 1e13), lit in its p⁺ (electrons injected), van Overstraeten and de Man's ionization, M 4.66 | F = 2.099 against McIntyre's kM + (1 − k)(2 − 1/M) = 2.096, k = αp/αn = 0.108 at its field |

**Sanitized builds:** the APD's 5 nm mesh takes its walk past the budget there, so it runs on Release builds alone. The others are under a second each in Release.

## Break checks

| Break | Caught by |
|---|---|
| The edges' sources' own current (Ramo's) left out | `thermalNoiseOfABar`: 10⁻⁴⁰ against 3.7·10⁻¹⁸ A²/Hz |
| Diffusion noise as the two streams' hops | `thermalNoiseOfABar` at 3 V: 1.33 times |
| The pairs made not counted | `shotNoiseOfAPhotodiode`: 0.0002 of 2q I |
| The ionizations not counted | `excessNoiseOfAnApd`: F 1.00 |
| Diffusion noise where it ionizes | `excessNoiseOfAnApd`: F 21.8 |
| The load's noise not over its impedance | `shotNoiseOfAPhotodiode` |
| The NEP not over the responsivity | `shotNoiseOfAPhotodiode` |
| The adjoint of the untransposed matrix | `shotNoiseOfAPhotodiode`: 3.5·10⁵ |

The ionization fit weighed by the faces, tried on the way, was not caught once the diffusion was left out where carriers ionize: the noise no longer depends on it. It was taken out again, so the forward model is as before.

## Next

One of the nine remains: the APD's convergence wall past −22.7 V.
