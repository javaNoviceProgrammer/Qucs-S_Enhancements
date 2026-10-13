# Feature gaps: the FEM's semiconductor capability

*12 October 2026 — Qucs-S 26.1.8, on `56a5abb`.*

The review ("FEM semiconductor capability", 12 October) asked how far the 2D
multiphysics FEM can go in simulating semiconductor devices, against dedicated TCAD. It
found a capable 2D drift-diffusion solver, strongest where it meets the mode solver, but
not yet general-purpose TCAD. It listed:

- ten gaps;
- seven next steps by payoff;
- three benchmarks;
- an addendum of six findings from running the germanium photodiode example.

All of it was done here except 3D, which gets a written proposal instead
(`docs/proposals/2026-10-12-multiphysics-3d.md`). The work is in fifteen commits,
`d9c4751` to `7196d35`.

| Gap | Review | Now | Commit |
|---|---|---|---|
| 1 | No semiconductor transient | It had one (adaptive BDF2). A mode step before a step in time now lights it: a photodiode's pulse and step response. Tested on reverse recovery. | `164ef3d` |
| 2 | No impact ionization | Chynoweth's α, the pairs in the currents, the ionization integrals: breakdown and avalanche gain | `18b5157` |
| 3 | No band-to-band or oxide tunnelling | Hurkx's band-to-band tunnelling; Fowler-Nordheim's through an oxide | `ed50888` |
| 4 | Boltzmann statistics only | Fermi-Dirac statistics, Slotboom's band gap narrowing, dopants ionized in part | `29b7e8e` |
| 5 | No interface traps | Interface traps: Shockley-Read-Hall occupancy, static and small-signal | `508f4ae` |
| 6 | Saturation by the whole field; no surface mobility | Lombardi's surface mobility; saturation driven along the current | `569b4fe` |
| 7 | Schrödinger reads the potential one way | Self-consistent Schrödinger-Poisson | `6ab2a98` |
| 8 | No self-heating | The carriers' heat for a heat physics, and a lattice temperature back for the mobilities | `49c5e3f` |
| 9 | 2D and axisymmetric only | A written proposal: three stages | `7196d35` |
| 10 | Analytic doping only | An implant profile, annealed; a diffusion in time read as the doping | `542074f` |

Each item was reproduced or measured through the solver's tests, with HOME, settings,
trash and cache isolated.

## 1. The photodiode example's findings (the addendum) — `d9c4751`

| Review | Now |
|---|---|
| 1. Generation exceeds the photon supply by 0.5–1.3 %. At 1550 nm G_op = 7.838e15 /s against an upper bound of 7.80e15 /s. | **Exactly the photons lost.** A node on germanium's edge took its light over the silicon's share too. Optical Generation now counts its pairs by triangle, each in its own medium, scaled to what the mode absorbs across the gap. `aGermaniumPhotodiode`: 7.7923e15 pairs/s against P(1 − e^−2αL)/hν = 7.7923e15. |
| 2. Mode hop in the wavelength sweep: from 1580 nm the solver follows another branch (n_eff 3.838 → 3.956). | **Follow the mode.** A mode step's *Follow the mode* (one mode) keeps the mode nearest the last one, from frequency to frequency and from one sweep point to the next. The example's spectrum now falls smoothly, 3.8678 → 3.7963 from 1500 to 1640 nm. Unfollowed, it still jumps to 3.9565 at 1580 nm. |
| 3. `R = semi.I_k/semi.Pin` shown in s³·A/(m²·kg). | **In A/W.** A quotient's unit is shown as written: 1.1494 A/W. |
| 4. "11 of 3772 edges not Delaunay (their currents left out)". | **None.** A semiconductor's mesh is kept Delaunay: edges are flipped after smoothing, and a segment with an obtuse angle across it is split. Both examples now have 0 edges weighed below 0 (3,776 and 3,648 edges). |
| 5. The optical generation is fed the last mode solved, not mode 1. The optical feature cannot choose a mode. | **A mode by number.** Optical Generation has a mode number, 1 by default. The mode step leaves that mode selected. A mismatch is an error that says which mode was solved last. |
| 6. Responsivity depends on the mode by about 6 %. A 2D cross-section cannot say which mix the waveguide launches. | **An assumption, stated.** The feature's description and the README say that the chosen mode is taken to carry all the light. Mode 1 gives 1.2084 A/W and mode 3 gives 1.1252 (`theModeThatLightsIt`). The launch mix itself is stage 0 of the 3D proposal: an eigenmode expansion across the junction. |

## 2. The semiconductor in time (gap 1, step 1) — `164ef3d`

| Review | Now |
|---|---|
| "Only stationary and frequency steps apply to `semi`." No switching, reverse recovery, pulse or eye response. | **It was there already.** `56a5abb` had a semiconductor in time (adaptive BDF2; `currentsInTime` and `carriersInTime` tested it). What was missing was light: a mode analysis before a step in time was refused. Now it lights the step. The mode's fields are kept, the step in time writes its own series of times, and Optical Generation's power can be a function of t. The step's warning names what changes in time. |
| | **Reverse recovery** (`aDiodesReverseRecovery`): a long diode switched from 0.65 V to −1 V in a picosecond. The charge out of the anode between two lifetimes is 0.8987 of the difference in stored charge. |
| | **A photodiode's step response** (`aPhotodiodesStepResponse`): it settles at the lit current at rest. Transformed, it gives the frequency step's response: 0.9648 against 0.9648 at 10 GHz, and 0.8767 against 0.8765 at 20 GHz. |

## 3. Impact ionization (gap 2, step 2) — `18b5157`

| Review | Now |
|---|---|
| No generation term for it. No breakdown voltage, APDs, SPADs or power-device blocking. | **Chynoweth's α = a e^(−(b/E)^β)**, with silicon's van Overstraeten and de Man coefficients or your own. Its pairs, (αn\|Jn\| + αp\|Jp\|)/q, come from each triangle's field and its currents fitted from its edges'. An edge's own field gave an APD's gain of 1.115 where the ionization integral says 3.14. |
| | **The ionization integrals**, along the field line through the highest field: `semi.In`, `semi.Ip`, `semi.Mn`, `semi.Mp`, `semi.Emax`. Breakdown is where they reach 1. An abrupt junction breaks down at 51.65 V at 1e16 cm⁻³ (depletion approximation 51.66) and at 13.86 V at 1e17 (13.91). |
| | **Avalanche gain**: 2.174 at −46 V against 1/(1 − Ip) = 2.202, and 3.071 at −48 V against 3.143. Past breakdown there is no solution, and the solver says so. |

**Found on the way, and fixed:**
- **A sweep's point started from equilibrium again.** The warm start compared the sizes of
  the values, and a solved field's seven arrays never matched a fresh one's room for its
  unknowns. Now −10 V to −11 V takes 4 steps, not 14 (`aSweepStartsFromTheLast`).
- **"Did not converge" at rest was passed on as a warning.** It is now an error.
- **An edge of nought weight** (a right angle across it) was counted as not Delaunay.
- **Newton crawling** at steps under 0.02 now stops after ten.

## 4. A MOSFET's mobility (gap 6, step 3) — `569b4fe`

| Review | Now |
|---|---|
| Saturation uses the element's total \|E\|. With it on, the gate's field cuts Id about 20× at Vd 0.1 V. No perpendicular-field model. | **Lombardi's surface scattering.** Acoustic phonons and roughness act by the field across the nearest insulator's interface, faded with depth as e^(−d/lcrit). A 1 µm NMOS's channel has an effective mobility of 445.9 cm²/Vs at 0.333 MV/cm and 406.7 at 0.579, on the universal curve's scale. Its mobility as solved matches the formula to 0.3 % (478.4 against 477.1). |
| Suggested: saturation driven by the field along the current, E·J/\|J\|. | **Driven by each carrier's quasi-Fermi gradient** (capped at \|E\|), by default. The drain current at 0.1 V is 0.9987 of the unsaturated one (the channel's own saturation 0.9989), where the whole field cut it to 0.0909. E·J/\|J\| fitted on a triangle was tried first. It took an inversion layer's cancelling drift and diffusion for current and halved the channel's mobility, so it was not kept. *Driving force: field* keeps the old behaviour. |
| | **Held once settled.** Lagged a step, the mobilities crawled where generation sets the current and cycled where the carriers carry nothing. They are now held once the potential settles, or after 15 iterations. |
| | **Found on the way, and fixed:** `semi.mun` and the current densities were the low-field mobility's. A saturated bar's current density was 14 times its current. They are now as solved (`mobilityModels`: 159.81 A, q n v 159.81). |

The photodiode example is again 1.15 A/W and 29 GHz at 1 mW. Its doped germanium's
carriers are no longer slowed by the heterojunction's field across them.

## 5. Heavy doping (gap 4, step 4) — `29b7e8e`

| Review | Now |
|---|---|
| Carriers are Boltzmann's: n = ni exp((V + θn − φn)/Vt). Degenerate regions (1e20 contacts, HEMT 2DEGs, heavily doped emitters) are approximate. | **Fermi-Dirac statistics.** n = Nc ℱ1/2(η), tabulated once to 2e-9. It enters through each carrier's band offset, Vt ln γ, so the flux keeps Scharfetter and Gummel's form with the generalized Einstein relation's diffusion. This holds at rest, at equilibrium, in time and small-signal, the carriers' change read back as n ℱ'/ℱ δη. Silicon's 1e20 cm⁻³ is 0.30 of Boltzmann's np. |
| No band gap narrowing. | **Slotboom's narrowing** (Klaassen's values): ni e^(ΔEg/2kT), the bands closing on the intrinsic level. |
| No incomplete ionization. | **Dopants ionized in part**, filled by the Fermi level at once: `semi.Ndi`, `semi.Nai`, and `semi.rho` with them. 1e18 cm⁻³ of phosphorus is 76.3 % ionized, of boron 48.5 %. |

**Checks:**
- **An n⁺n step** (`aDegenerateStep`): its potential step and minority carriers match the
  analytic ones under all four statistics (Boltzmann or Fermi-Dirac, narrowed or not), to
  the sixth digit. At ±1 mV its currents are opposite, and its 1 kHz conductance is the dc
  slope (307.07127 S against 307.07127).
- **A degenerate accumulation layer** (`aDegenerateAccumulation`): its 1 kHz capacitance
  is the quasi-static one (0.98828 of Cox both), and the substrate's is −C_gg.
- **A gated film** (`aDegenerateChannel`): its transconductance at 1 kHz is the dc one,
  220.1177 S both.

## 6. Interface traps (gap 5, step 5) — `508f4ae`

| Review | Now |
|---|---|
| Only a fixed `charge` on boundaries. No C-V stretch-out or hysteresis, no GaN current collapse. | **An Interface Traps boundary feature.** Acceptor- or donor-like, one level or a uniform Dit over a range (in levels), each filled as Shockley, Read and Hall have it. Their charge goes into Poisson's equation and the pairs they recombine into the carriers'. At rest their changes are exact in the Jacobian. Small-signal, each level follows with its own time, 1/(cn(n + n1) + cp(p + p1)), and its charge is in the contacts' currents. |
| | **Stretch-out** (`interfaceTrapsStretchOut`): a MOS capacitor's gate voltage matches Vfb + ψs − (Qs + Qit)/Cox, with Qit = −q²Dit(ψs − φB), to 1 mV. From −0.9 to 0 V the surface potential moves 0.504 V with 5e11 cm⁻²eV⁻¹, against 0.565 without. The quasi-static capacitance is Cox in series with Cs + q²Dit to 0.3 %. |
| | **The conductance method** (`interfaceTrapsConductance`): Gp/ω matches a level's Cit ωτ/(1 + ω²τ²) to 0.2 % from a decade below its peak to a decade above. The substrate's admittance is the gate's. |
| | **Surface generation** (`interfaceTrapsGenerate`): a reversed junction under oxide carries the traps' generation, 4.4424e-11 A against q∫U 4.446e-11. |
| | **Not done:** in time the occupancy follows at once. A slow trap's own state is not kept, so there is still no hysteresis and no GaN current collapse. That needs the occupancy as an unknown of its own. |

## 7. Self-consistent Schrödinger-Poisson (gap 7, step 6) — `6ab2a98`

| Review | Now |
|---|---|
| `qw` reads another physics' potential one way. No quantum confinement in inversion layers or 2DEG charge. | **The states' carriers.** `qw.n` and `qw.p` are each state's \|ψ\|² times the density of states of its free directions: a well's in the plane, a wire's along z, a dot's none. They are filled to a Fermi level given from the band (`semi.Efn − semi.Ec`), so the two physics' bands need not share a reference. A flat well matches the sum of its subbands to 1e-6 (`quantumCarriers`). |
| | **Solved in turn.** A semiconductor's *Schrödinger-Poisson* region, and *Self-consistent* on the Quantum States step. Each node's carriers are moved to the states' by a quantum potential in its band, Vt ln(n_states/n), using Trellakis's predictor and corrector, with the contacts and currents. |
| | **Anderson mixing** of the last five turns. Plain turns crawled at 0.96 a turn on a 2 nm mesh, past 60; mixed, it rests in 26. |
| | **A MOS inversion layer** (`schrodingerPoisson`) rests in 9 turns. Its electrons match the states' at every node to 1e-3, with none at the oxide (0.00108 of their most, classically 1.03). Their centroid is at 1.963 nm (classically 0.968; Fang and Howard's 1.777), and there are 2 % fewer of them. Holes behave likewise (`schrodingerPoissonHoles`). |

## 8. Electrothermal coupling (gap 8, step 7) — `49c5e3f`

| Review | Now |
|---|---|
| `semi` has T as a setting; no Joule heat fed back. No self-heating. | **The carriers' heat, `semi.H`.** Their Joule heat as solved: each edge's currents through their quasi-Fermi potentials' drop, shared among its triangles as their conductances are. Plus the pairs' recombination heat, qR(φp − φn). A forward diode's heat is the power it takes, 1.36384 mW against IV 1.36385 (`theCarriersHeat`). J²/(qnµ) at a point was tried first and gave 65 W for 0.5 mW: it took a depletion region's cancelling drift and diffusion for current. |
| | **A lattice temperature**, `TL` (a heat physics' `ht.T`), for the mobilities: (T/300 K)^−ζ, with ζ 2.5 and 2.2 by default. The statistics stay at the physics' T. The two physics are solved in turn, as physics reading each other are. |
| | **A self-heated bar** (`selfHeating`): 380.77 A against the one-dimensional problem's 380.77, 15 % below its current at 300 K (448.6). Its middle is at 330.45 K, also the 1D problem's. |
| | **One way** (`latticeTemperatureOneWay`): a bar warmed by a source of its own gives 9.11501 A, against V/∫ρ = 9.11505. |

## 9. Tunnelling (gap 3) — `ed50888`

| Review | Now |
|---|---|
| No band-to-band or oxide tunnelling. No TFETs, GIDL, Zener diodes or gate leakage. | **Band to band, Hurkx's:** A E^γ e^(−B/E) (np − ni²)/((n + ni)(p + ni)) at each triangle's field, its Jacobian complete (silicon's Klaassen values), its pairs in `semi.R`. Or **generation alone**, for a reversed tunnel junction. Hurkx's factor is small wherever either carrier is above ni, and a junction's carriers spill some 10 nm into its field. A p⁺-i-n⁺ diode's current with Hurkx's is 0.51 of generation alone's at −2 V, and 0.61 at −2.3 V. |
| | **The diode** (`bandToBandTunnelling`): its current with generation alone is q Σ A E^γ e^(−B/E) over its triangles, 1.847e-12 A against 1.8472e-12. With Hurkx's it is q∫G (9.4917e-13 against 9.4896e-13). Without the feature it is 6.9e-17 A. |
| | **Fowler-Nordheim** through an oxide: J = A E² e^(−B/E) at the oxide's field across a boundary, with Lenzlinger and Snow's A and B (1.184e-6 A/V², 241.6 MV/cm for SiO₂ on silicon). It is reported as `semi.Ifn` after the solve and not taken from the carriers. A MOS capacitor at 9.94 MV/cm gives 3.25 mA/cm² (`fowlerNordheim`: within 1 % of J W). |
| | **Not done:** both models are local (the field at a point stands for the tunnelling path's). There is no non-local path integration. |

## 10. Process simulation (gap 10) — `542074f`

| Review | Now |
|---|---|
| Doping is analytic. No implant, diffusion or anneal profiles; they have to be typed in as expressions. | **An Implant doping profile.** Its dose sits about its range below the surface (Rp, ΔRp: a range table's), and a mask's window's edges are spread by the lateral straggle. Annealed, intrinsic diffusion widens it: σ² = ΔRp² + 2Dt, with D = D0 e^(−Ea/kT), boron's by default. The surface reflects (its image), so the dose is kept. |
| | **Boron**, 1e14 cm⁻² at 0.1 µm, annealed 30 minutes at 1000 °C (`implantAndAnneal`): its dose to 1e-5, its peak (5.2046e24 against 5.2047e24 m⁻³) and junction (0.4302 µm) the formula's, its window's edge 0.5063 of its middle. |
| | **A general diffusion in time** as a PDE physics' step, its u read as the doping. Diffusing the implant those 30 minutes gives the formula to 2e-4. Read as the semiconductor's doping, it gives the Implant profile's to 1e-3 (`annealedDopingRead`). |
| | **Not done:** oxidation, segregation, concentration-dependent or coupled diffusion of several species, and etching and deposition. A PDE's D can be an expression of u, but nothing here tests that. |

## 11. 3D (gap 9) — `7196d35`

A written proposal, `docs/proposals/2026-10-12-multiphysics-3d.md`. Every layer of the
engine is 2D. But most of what the review names (FinFETs, nanosheets, corner effects,
interconnects) is 2D shapes stacked in z. Prisms extruded from one 2D Delaunay mesh keep
the box method's weights positive; a conforming Delaunay tetrahedral mesh is the hard part
of 3D TCAD. It proposes three stages:

- **Stage 0**: an eigenmode expansion across a junction of cross-sections. This gives the
  photodiode's launch mix (finding 6), with no 3D.
- **Stage 1**: extruded, layered 3D. The scalar physics and the semiconductor, AMGCL beyond
  2e5 unknowns, cut planes and a 3D view. Its main test: a model uniform in z equals the
  2D model.
- **Stage 2**: general solids, only for a concrete need.

It ends with three decisions for the user.

## 12. Benchmarks — `f10f86e`, and `aJunctionsBreakdown`

Both device benchmarks run in Release builds only, and skip in sanitized ones. They take
about 10 s and 25 s.

**Ge photodiode against a published device** (`aPublishedLateralPhotodiode`). Virot et
al.'s lateral Si/Ge/Si photodiode: germanium 260 nm thick on a 60 nm seed, between p⁺⁺
and n⁺⁺ silicon. Measured at their −16 dBm (25 µW).

| | Measured | Here |
|---|---|---|
| 40 µm long, w 1 µm, −1 V | 1.16 A/W | 1.248 A/W (ideal qλ/hc 1.25) |
| 10 µm long | 0.5 A/W (their FDTD 0.63: butt coupling, weaker absorption) | 1.231 A/W. The cross-section's mode is all absorbed within 10 µm; theirs, coupled from a silicon waveguide, is not. |
| 3 dB, w 1 µm, −4 V | about 18 GHz | 35 GHz (vsat 6e6 cm/s here) |
| 3 dB, w 0.3 µm, −2 V | over 50 GHz (the setup's limit) | 96 GHz |

At 1 mW the 1 µm device's germanium filled with its own carriers (4.5e16 cm⁻³), the
field was screened to 2 kV/cm, and the bandwidth fell to 2.5 GHz. That is why the paper's
power is used. The photodiode example's "29 GHz" is now said to be at 1 mW; it gives 33
GHz through 50 Ω at 25 µW.

**An EAM against a paper** (`aPublishedElectroabsorptionModulator`). Liu et al.'s GeSi
modulator, as Bernardis's thesis gives it: 0.75 % silicon, 0.4 µm × 0.6 µm × 50 µm. The
measurements:
- 6.5 dB of extinction at 1550 nm at an effective −2.5 V;
- 2.2 dB/V up to 5.5 dB;
- 5.2 dB or more from 1510 to 1555 nm.

The model is the modulator example 0.4 µm high. Its direct gap is the one thing not
given:

| Direct gap | At 0 V | Extinction at −2.5 V, 1550 nm | dB/V to −2 V | 1530 nm | 1555 nm |
|---|---|---|---|---|---|
| 0.80 eV (tensile Ge's 0.773 + 3.6 eV per unit Si, the example's law) | 18.5 dB lost | 2.46 dB | | | |
| 0.81 eV | 4.07 dB lost | **6.51 dB** (6.5 measured) | 2.81 (2.2) | 2.02 dB | 6.32 dB |

At the composition law's gap, 1550 nm's photons sit on the edge, and there is no
modulator. 10 meV higher, the measured extinction at 1550 nm comes out, with a slope near
the measured one. But the window is narrower than the device's. Above the gap the field
lowers the loss, and below 1540 nm there is little left to modulate. The device's broader
window suggests an edge broader than the model's 8 meV (strain and composition varying
over the stripe), or the indirect gap's absorption. The insertion loss could not be
checked: no source at hand gives it reliably.

**BV(N)** (`aJunctionsBreakdown`, from section 3). An abrupt one-sided silicon junction
against Sze and Gibbons' universal BV ≈ 60 (Eg/1.1 eV)^1.5 (N/1e16)^−0.75 V:

| N | Here | Depletion approximation | Sze and Gibbons |
|---|---|---|---|
| 1e16 cm⁻³ | 51.65 V | 51.66 V | 61.6 V (−16 %) |
| 1e17 cm⁻³ | 13.86 V | 13.91 V | 11.0 V (+26 %) |

The solver matches the integrals of the analytic field to 0.4 %. The difference from Sze
and Gibbons comes from the coefficients: theirs were fitted to older ones (Lee's), and van
Overstraeten and de Man's give these.

## 13. Where it stands now

The review's "not yet" list was: power devices, breakdown, anything in the time domain,
short-channel CMOS, HEMTs, and heavily doped or quantum-confined channels.

- **Breakdown, the time domain, heavy doping and quantum-confined channels:** now in.
- **Power devices:** breakdown and self-heating are in, but 2D only.
- **Short-channel CMOS:** surface mobility and saturation along the current are in. There
  is no energy balance (no velocity overshoot), and tunnelling is local.
- **HEMTs:** Fermi-Dirac statistics, Schrödinger-Poisson, heterojunctions and a fixed
  polarization charge (as a surface charge) are in. Trap dynamics (current collapse) are
  not.
- **Still missing:** 3D (proposed), slow traps' own state, non-local tunnelling, energy
  balance or hydrodynamic transport, and process steps beyond implant, anneal and
  diffusion.

The physics' description in the model editor and the agent's reference (`39c1f97`) now
name all of these features.

## Tests

| Item | Tests | File |
|---|---|---|
| The addendum | `theModeThatLightsIt`, `aSweepFollowsItsMode`, `aSemiconductorsMeshIsDelaunay`; `aGermaniumPhotodiode` and `thePhotodiodeExample` tightened (pairs = photons lost to 1e-6, the example's spectrum followed, its R in A/W) | `test_fem_optoelectronics` |
| | `aWaveguidesModes` (as it was): it caught several modes being followed | `test_multiphysics_doc` |
| In time | `aDiodesReverseRecovery` | `test_fem_semiconductor` |
| | `aPhotodiodesStepResponse` | `test_fem_optoelectronics` |
| Impact ionization | `aSweepStartsFromTheLast`, `aJunctionsBreakdown`, `anAvalancheGain` | `test_fem_devices` (new) |
| Mobility | `aMosfetsMobility` | `test_fem_devices` |
| | `mobilityModels`: the current density as solved | `test_fem_semiconductor` |
| Heavy doping | `aDegenerateStep`, `aDegenerateAccumulation`, `aDegenerateChannel`, `dopantsIonizedInPart` | `test_fem_devices` |
| Traps | `interfaceTrapsStretchOut`, `interfaceTrapsConductance`, `interfaceTrapsGenerate` | `test_fem_devices` |
| Schrödinger-Poisson | `schrodingerPoisson`, `schrodingerPoissonHoles`, `quantumCarriers` | `test_fem_devices` |
| Electrothermal | `theCarriersHeat`, `selfHeating`, `latticeTemperatureOneWay` | `test_fem_devices` |
| Tunnelling | `bandToBandTunnelling`, `fowlerNordheim` | `test_fem_devices` |
| Process | `implantAndAnneal`, `annealedDopingRead` | `test_fem_devices` |
| Benchmarks | `aPublishedLateralPhotodiode`, `aPublishedElectroabsorptionModulator` | `test_fem_optoelectronics` |

The figures in sections 1–12 are these tests' output on a Release build.

**Sanitized builds.** Each test function has a smaller or coarser form, kept under
QtTest's 300 s per function (CI's runner is about 1.45 times slower than this Mac's
sanitized build). Three needed it:
- `aMosfetsMobility`: 0.5 µm at 1.5 V alone;
- `aDegenerateAccumulation`: a thinner fine layer;
- `schrodingerPoisson`: a 0.2 nm line.

`test_fem_devices` takes about 17 minutes sanitized here, and about 25 on CI's runner. It
has an hour in ctest, and CI's Linux job 210 minutes, not 150 (`a757c78`).

On `7196d35`:
- the full ctest passes: 116 tests;
- the sanitized `test_fem_devices` passes: 23 functions in 16 minutes;
- the MCP scenario script passes: 97 checks.

## Breaks

Each change was broken in turn, and its tests run. A break that passed got a test, except
the three below.

| Item | Breaks | Caught |
|---|---|---|
| The addendum | 12: pairs not scaled to what the mode absorbs; the last mode left selected; the number not checked; no following; the centre not carried to a sweep's next point; several modes followed; a quotient's unit not named; a quotient in a product named; the mesh not kept Delaunay; no flips; no splits; segments' triangles not found again | 12 |
| In time | 4: a mode step before a step in time refused; the time step not a series of its own (caught once a check was added); the power not of t; the carriers' change left out of the contacts' currents | 4 |
| Impact ionization | 10: no pairs in the currents; the holes' rate the electrons'; the holes' range above E0 not taken; Ip without its exponential; the line along a triangle's own field; the field recovered across domains; the integrals' choice of currents ignored; a sweep compared by its values; a failure at rest a warning; an edge of nought weight not Delaunay | 9 |
| Mobility | 7: no fading with depth; the tangent for the normal; the whole field always; no write-back; C without T; no hold; no cap at \|E\| (the last two caught by the photodiode's tests) | 7 |
| Heavy doping | 13: narrowing not in ni; nor in the bands; contacts by Boltzmann on Fermi-Dirac; the flux without ln γ; equilibrium by Boltzmann; small-signal by Boltzmann; the Jacobian without ln γ; donors without gd; donors filled by n, not n/γ; the shown donors by n; recombination without γ; the terminals read back by Boltzmann; the terminals' edge currents without ln γ (caught once a check was added) | 12 |
| Traps | 8: no emission to the valence band; donor-like charged when filled; static occupancy without its change (caught once C0 was checked against Cox in series with Cs + q²Dit); the small-signal occupancy wrong; the traps' charge not in the contacts; their recombination reversed; levels mirrored about Ei; levels at their steps' edges | 8 |
| Schrödinger-Poisson | 9: equilibrium without the quantum potentials; the bands not taking them; no Anderson mixing; the density of states without kT; holes filled from the wrong side; a line state read at its own node; the holes' potential reversed; the semiconductor not solved again; the carriers not kept with the field (two caught once a coarse wide run and a 0.2 nm line were added) | 9 |
| Electrothermal | 6: mobility not at the lattice temperature; the temperature not read at each solve; the heat shared by geometry alone; the Joule heat reversed; no recombination heat; the temperature read without the coupling (caught once the one-way test was added) | 5 |
| Tunnelling | 7: A not scaled to SI; Hurkx's sign reversed; generation alone not taken; no field derivative in the Jacobian; tunnelling not in `semi.R`; FN without the oxide mass; FN's field along the boundary | 7 |
| Process | 4: no image at the surface; the anneal not widening it; the window not annealed; depth measured upward | 4 |
| Benchmarks | 2: Franz-Keldysh's θ with twice the reduced mass; the light not falling along the photodiode | 2 |

**Not caught (3 of 82):**

- **Impact ionization's field line along a triangle's own field.** The tests' junctions are
  planar, so the line is straight either way. A triangle's constant field integrates to
  within the tests' bounds of the interpolated one's.
- **Recombination without γ under Fermi-Dirac statistics.** The degenerate regions are
  where minority carriers are fewest. In the tests' devices it moves the current by about
  1e-20 A.
- **The Joule heat shared among an edge's triangles by geometry, not conductance.** Any
  sharing keeps the totals, and the tests check totals (∫H = IV, a 1D bar). Checking it
  would need a reference for the heat inside a single triangle.

**A note from the tunnelling breaks.** Without the field derivative in the Jacobian,
Newton converged to a current of 2.36e-8 A where it is 1.85e-12. Newton stops on its
step's size, and with a wrong Jacobian the steps became small short of the solution. The
test caught it, because the current did not match q∫G. A residual check beside the step's
size would have refused it. That is not done here.

## Sources

- J. Virot, D. Benedikovic et al., "Integrated waveguide PIN photodiodes exploiting
  lateral Si/Ge/Si heterojunction", Opt. Express 25, 19487 (2017).
- J. Liu, M. Beals, A. Pomerene, S. Bernardis et al., "Waveguide-integrated,
  ultralow-energy GeSi electro-absorption modulators", Nat. Photonics 2, 433 (2008).
- S. Bernardis, "SiGe electro-absorption modulators for applications at 1550nm", S.M.
  thesis, MIT (2008), hdl.handle.net/1721.1/44385.
- S. M. Sze and G. Gibbons, Appl. Phys. Lett. 8, 111 (1966).
- R. van Overstraeten and H. de Man, Solid-State Electron. 13, 583 (1970).
- C. Lombardi et al., IEEE Trans. CAD 7, 1164 (1988).
- J. W. Slotboom and H. C. de Graaff, Solid-State Electron. 19, 857 (1976); D. B. M.
  Klaassen et al., Solid-State Electron. 35, 125 (1992).
- G. A. M. Hurkx, D. B. M. Klaassen and M. P. G. Knuvers, IEEE Trans. Electron Devices
  39, 331 (1992).
- M. Lenzlinger and E. H. Snow, J. Appl. Phys. 40, 278 (1969).
- A. Trellakis et al., J. Appl. Phys. 81, 7880 (1997); D. G. Anderson, J. ACM 12, 547
  (1965); F. F. Fang and W. E. Howard, Phys. Rev. Lett. 16, 797 (1966).
