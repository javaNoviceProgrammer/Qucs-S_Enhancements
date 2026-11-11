# Multiphysics 3D: semiconductors - 11 November

The third of the three physics the plan (2026-11-10-multiphysics-3d-more-physics-plan) brings to 3D models, after
solid mechanics and magnetic fields. The user asked for all of the 2D solver's features. All of them are here but
four that are the plane's alone (at the end). A model uniform in z is its 2D model to rounding, feature by feature.

**A solver of its own.** The 2D solver (`fem_semiconductor.cpp`, 8,700 lines) is built on triangles throughout:
- three corners and three edges, each edge weighed by its cotangents;
- per-triangle mobilities and gradients;
- quantum slices, lasers, a 1D line.

Generalizing it in place would put its validated results at risk. So 3D has a solver of its own,
`fem3_semiconductor.cpp`, with the 2D solver's equations, property names and results. The models both use (the
mobility models, Fermi-Dirac's integral, saturation velocities, Hurkx's tunnelling, impact ionization's rates...)
moved out of `fem_semiconductor.cpp` into `fem_semiconductor_models.h`, word for word, both solvers calling them.

## The discretization

The box method on order-1 prisms:

- **Horizontal edges:** coupled by half the cotangent of the angle opposite them in each plan triangle (the linear
  triangle's stiffness, as 2D's), times half the layer's height for each of the prism's two levels.
- **Vertical edges:** coupled by their corner's share of the plan triangle over the layer's height.
- **A node's box:** that share times half the height, summed over its semiconductor prisms.
- **The share** is 2D's rule (`cornerShare`): the circumcentric (Voronoi) area, or in an obtuse triangle half to
  the obtuse corner and a quarter to each other.
- **A face's charge, traps and recombination** go to its nodes by the same shares (a floor's) or a quarter each (a
  wall's): over each box's own face.
- **A node on a junction** takes each domain's doping over its share of its box, as 2D does. It recombines over the
  parts of its box on its own bands alone.
- **The currents:** Scharfetter and Gummel's along every edge. Poisson's charge is lumped in the boxes.
- **By prism, as 2D by triangle:** the mobilities (their fields and forces at the prism's middle), impact
  ionization, the turned currents of a magnetic field, the hot carriers' heating.
- **An abrupt heterojunction:** an edge along it is two, one each side's bands; an end on the other side's node sees
  its carriers on these bands (the quasi-Fermi levels continuous); a node's box's other-side part holds its carriers
  on that side's bands.

## The physics

- **Unknowns:** V, n and p at every node, n and p scaled by the largest doping; behind a circuit, its contact's own
  voltage.
- **Media:** a domain whose material model gives a band gap is a semiconductor; one without is an insulator, its
  potential alone.
- **Mobility:**
  - Low field: constant, Caughey and Thomas, Masetti, Arora, Dorkel and Leturcq, Klaassen, Yamaguchi, or an
    expression of N, Nd, Na, T, n, p, Epar, Eperp, dsurf.
  - At a surface: Lombardi's or Shirahata's scattering, the field across the nearest insulator's interface, faded as
    e^(-d/lcrit) from it.
  - Saturation: Caughey and Thomas's, driven by the force along each current (2D's default), the whole field, or
    the carriers' temperature from an energy balance (hot carriers); the electrons' velocity the transferred
    electrons'.
  - The lattice's temperature: (TL/T)^-ζ; Hall factors.
- **Statistics and bands:** Fermi-Dirac's; Slotboom's band gap narrowing; incomplete ionization.
- **Heterojunctions:** abrupt (each side's carriers on its own bands to the interface), thermionic (emission over the
  step in series with the edges into it, Crowell and Sze's), tunnelling through its spike too, or graded over an
  element.
- **Doping:** uniform, a box (its 3D zmin and zmax), an implant (its surface at z, the ions downward), or an
  expression of x, y and z.
- **Recombination and generation:** SRH (Fossum's lifetimes, Hurkx's trap-assisted tunnelling), Auger, direct;
  generation of x, y, z and t, its photons' energy; band-to-band tunnelling; impact ionization (driven by the
  field, the quasi-Fermi gradients or the field along each current; its pairs in the equations or its integrals
  alone); stimulated recombination above threshold.
- **Optical generation from a full-wave solve:** a Wavelength Domain step's light (an Electromagnetic Waves, Frequency
  Domain physics) solved before the semiconductor's step, its |E| of 1 W fed - P0 of it, each photon the media's κ
  absorb a pair at η: G = η P0 ω ε0 Im εr |E|²/ħω. (The wave's modes are normalized ∫ E×H·dA = 1: the power lost is
  ω ε0 Im εr |E|², no half.)
- **Contacts:** metal (ohmic), its carriers neutral; Schottky, its carriers emitted over the barrier; a gate on an
  insulator's face. Behind a circuit: Rs and RL in series, or a bias tee, a capacitance Cp across.
- **Faces:** surface charge; interface traps (a level or a range, acceptor- or donor-like, each filled as SRH has it,
  its charge and its pairs); wurtzite polarization charge (c along x, y or z, the metal face's or the nitrogen's, its
  layers strained to a substrate's lattice); surface recombination, of both carriers or an ohmic contact's minority
  carriers alone; Fowler-Nordheim tunnelling through an insulator (its current after the solve).
- **A magnetic field** Bx, By, Bz: each carrier's current turned, J = (J0 + J0×β + (J0·β) β)/(1 + β²), β = ∓ r µ B.
- **A lattice temperature TL** (a heat physics' ht.T, its source semi.H): self-heating, solved in turn.

## The solve

- **Newton's method from equilibrium:** first Poisson's equation alone, the carriers Boltzmann's of a Fermi level
  at nought; then all three together; then the contacts' voltages raised in steps, longer after an easy one, halved
  where one fails.
- **Each Newton step:** the Jacobian by an LU; each carrier's column scaled by its value; rows by their largest
  entry; one step of iterative refinement; a line search; the potential's step at most 10 Vt, a falling carrier's
  in its logarithm.
- **At equilibrium all the way** (every contact but the gates at one voltage, nothing generated): Poisson's equation
  alone. A MOS capacitor's inversion layer spans 16 decades of electrons; Newton's method on all three stalled there.
- **Lagged a step:** mobilities of the state, the trap-assisted tunnelling's field, a heterojunction's emission.
- **Hot carriers:** walked with the field's mobilities, their temperatures anew at the end - theirs and the
  mobilities each other's, a temperature some of the way each step (2D's secant).
- **A contact's current, as reported:** its weight's drop along the edges times their currents - a weight 1 on it, 0
  on the other contacts, between them by the carriers' conductance (2D's). Its rows' reaction, the difference of a
  heavily doped contact's large fluxes, was rounding-limited: a p⁺ anode's 1e-15 A off its cathode's by 5e-4.

## Studies

- **At rest** (a Stationary step, a Parametric Sweep).
- **In time** (BDF), from the solution at rest; a contact's current with its metal's charge's change; Cp's in a
  circuit.
- **Small-signal** in a Frequency Domain step: the contacts' admittance, semi.G_k_l and semi.C_k_l; traps following
  with their time; lit, each contact's photocurrent per pair (Hop), through RL (HopRL), through its circuit (Hc).
- **Noise** (the physics' option) by the impedance field: every edge's diffusion noise (Nyquist's at equilibrium),
  every node's pairs made and recombined, every ionization, each carried to the contacts by the transposed system's
  Green's function: semi.Sn_, in_, Fshot_, SnL_ through a circuit's load, NEP_ lit.

## Results

- **Variables:** semi.V, n, p, Nd, Na, Ndop, ni; Ex, Ey, Ez, normE; Ec, Ev, Efn, Efp; R; Jn, Jp, J; H; Tn, Tp.
- **Globals:** semi.I_, V_, Q_ by contact; behind a circuit Vc_, IL_, VL_, PL_; semi.Ifn; semi.Istim; semi.In, Ip,
  Mn, Mp, Emax, Lion, xion, yion, zion, Iion, Igen, Mion; semi.Gop, Iop, Pabs, Pin; small-signal and noise above.
- **The estimate:** three unknowns a node, order 1.

## The plane's alone

- **The 1D line** (a 2D model solved along a line): a 3D model is solved whole.
- **The process flow's doping:** the 2D process simulator's profiles.
- **Schrödinger-Poisson:** it reads a quantum wells physics' states; 3D has no quantum wells physics.
- **Lasing** (a cavity mode, stimulated by its photons): it reads an electromagnetic eigenfrequency step's resonator
  mode; 3D's waves have no eigenfrequency step. Stimulated recombination above threshold, the simpler model, is here.
- **Optical generation's slices and mode number:** the 2D feature feeds one mode of a cross-section's Mode Analysis
  and can cut the device along its length; in 3D the full wave is solved, its light everywhere.

## Found on the way

Each found by a check against 2D or an analytic result, and fixed in 3D's code:

- **A floor face's charge lumped by thirds**, not by its boxes' shares: the potential uneven across the face, an
  interface trap's capacitance 3.3 % low (the conductance method's peak 4.5 %).
- **An interface node recombining over the other side's part of its box:** a graded heterojunction 2e-4 off 2D's.
- **A contact's current from its rows:** rounding-limited at a heavily doped contact (above).
- **Impact ionization by edge,** α of the field along each edge: a slanted edge's projected field, α half 2D's.
  Now by prism, its currents fitted to its edges (2D's rounding weights).
- **The heat by the potential's drop**, not each carrier's quasi-Fermi drop: a photodiode's ∫ semi.H 3 % over the
  light's power.
- **Trap-assisted tunnelling's trap depth** half the gap, not Ec - Et of the bands; a junction's node's field of both
  sides' prisms; equal gaps' node the first prism's domain, not the lower domain's.
- **An obtuse triangle's edge of no coupling** left out of the weighed current, though its prisms turn current
  through it in a magnetic field: 2e-5.

## Validated

| Check | What it holds |
|---|---|
| A junction at rest | Na 1e17, Nd 1e16 per cm³: quasi-Fermi levels flat to 1e-6 eV; the neutral potentials to 1e-6 V; the peak field 4.548e6 V/m against the depletion approximation's 4.571e6 (0.5 %) |
| A short diode | 1.00224e-12, 6.83215e-12, 4.65333e-11 A at 0.35, 0.4, 0.45 V against Shockley's 1.00243e-12, 6.83436e-12, 4.6559e-11 (0.06 %); anode and cathode to 1e-26 A |
| A MOS capacitor | quasi-static C 3.3053, 1.2354, 3.3561 fF against Sze's exact 3.2687, 1.2354, 3.3234 (1.1 %, 0.002 %, 1.0 %); a fixed charge shifting it by -Qf/Cox |
| A resistor | q µ N A V/L to 1e-6, µ constant and by doping |
| A photodiode | -1.8591e-10 A against -q G A (W + (L - W)/2) = -1.8626e-10 (0.2 %) |
| The light's heat | ∫ semi.H the light's power absorbed, G Eph V, 6.40871e-10 W both; of the gap's energy without Eph |
| Recombination | semi.R the sum of SRH, Auger and direct, to 1e-9 |
| A Schottky diode | 0.911 and 35.0 nA at 0.1 and 0.2 V against thermionic emission and diffusion's 0.927 and 38.4 nA (1.7 %, 8.8 %; its vD an estimate) |
| Uniform in z | the 2D solver on the plan mesh: V to 8e-15 V, n, p to 3e-13, the current to its last digit |
| Each model against 2D | Masetti, Arora, Dorkel, Klaassen, band gap narrowing, incomplete ionization, Fermi-Dirac, Fossum's lifetimes, band-to-band tunnelling, a lattice at 350 K, saturation by the current and by the field, transferred electrons, an expression of the state, stimulated recombination (Istim to 1e-9), a magnetic field, trap-assisted tunnelling: V to 1e-10 V, the carriers to 1e-8, the currents to 1e-9; hot carriers to 4e-8 (their iteration's stop) |
| Heterojunctions against 2D | graded, abrupt, thermionic, tunnelling: V to 8e-15 V, the carriers to 3e-13, the current to its last digit |
| Impact ionization against 2D | its current to 3e-10, Mion to its last digit; In 1.1 %, Ip 1.7 % (each solver's field lines); driven by the quasi-Fermi gradients, along the current, its integrals alone: the current to 1e-10 |
| Self-heating | 20 µm of n silicon, 4 V: its current the 2D model's within 5e-4, its middle's warming 0.3 %; 15 % below the cold bar's |
| Velocity saturation | q n A µE/(1 + (µE/vsat)²)^½ to its last digit |
| Doping profiles | a box's N0 and Gaussian fall, an implant's peak and window edge, to 1e-6 |
| Decay in time | excess electrons e^(-t/τ) within 0.5 % at τ and 2τ |
| A MOS capacitor at 1 MHz | depletion its quasi-static C; inversion 0.749 fF against the depletion approximation's minimum 0.767 (2.3 %) |
| Interface traps | the gate's charge with the traps' to 2e-5 against the exact 1D; the conductance method's peak q² Nit A/(8 kT) to its last digit |
| Polarization | an AlGaN barrier on GaN, c along z: the same solution as a surface charge of σ 24.2 mC/m², a 2DEG of 1.4e19 per cm³; the nitrogen face none |
| Fowler-Nordheim | a gate oxide at 10 V and at 2 V (tunnelling direct): A E² e^(-B/E) at the oxide's field, to its last digit |
| Contact circuits | V0/(R + Rs + RL) and its load's VL and PL to 1e-6; a bias tee's Rs alone at rest; Cp charged through Rs within 0.2 % of V∞(1 - e^(-t/τ)) |
| Surface scattering against 2D | Lombardi's, Shirahata's, Yamaguchi's: V to 5e-9 V, the current to 3e-9 |
| A Hall bar | B along x: its Hall field r I B/(q n t W) within 3e-4 (between y 0.3 and 0.7 µm: at the walls it falls to nought over a Debye length), its sign the electrons' |
| Optical generation | a lossy silicon wire fed 1 µW: the power absorbed 0.152 µW against the wave's own loss 0.155 µW (its ends radiate the rest); each pair a photon's; its small-signal collection at 1 kHz its DC one to 4e-5 |
| Noise | a resistor at rest Nyquist's 4kT/R to its last digit, through its circuit's load (R/(R + Z))² of it; a photodiode's shot noise, F 0.9998 |

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_semiconductor` (new, 29 functions, 3 minutes) | The checks above, and what is refused: a semiconductor with no contact; the 1D line and lasing the plane's |
| `test_multiphysics3d_doc` `physicsThroughThePanel` (changed) | Semiconductor offered on a 3D component, its contacts, doping and impact ionization, not lasing; its study's steps Stationary, Time Dependent, Eigenfrequency and Frequency Domain |
| `test_fem3_geometry` `schema`, `test_fem3_solve` `refusals` (changed) | A 3D model's physics include semiconductors; a PDE is the one refused; a heat model's Frequency Domain step refused, it being a semiconductor's small signal in 3D |

## Broken on purpose

`docs/feature_gaps/2026-11-11-multiphysics-3d-semiconductor/breaks.py` holds 45 breaks, each run against the test
functions that should catch it (the suite's binary, ctest's environment): the fluxes, Poisson's charge, the shares,
the contacts, SRH, mobility by doping, generation, a Schottky barrier, the equilibrium path, a surface charge, the
reported current, a floor's lumping, a trap's charge and lag, a heterojunction's recombining box, abrupt carriers,
emission and tunnelling, the node-domain rule, trap-assisted tunnelling's depth, polarization, Fowler-Nordheim's B
and barrier, a circuit's drop and Cp, the Hall sign and slowing, Lombardi's and Yamaguchi's, saturation's force,
transferred electrons, hot carriers' relaxation, impact ionization's fit and integrals, stimulated recombination's
threshold, optical generation, noise's rate and direct current, the heat, the lattice's mobility, an obtuse edge's
turned current.

44 of the 45 are caught. Missed: impact ionization's currents fitted without 2D's rounding weights - the weights
guard a heavily doped region's currents, the difference of two terms ten thousand times their size; in the test's
diode (1e17 and 1e16 per cm³) no edge's current is near its rounding, so they change nothing there. Kept as 2D has
them.
