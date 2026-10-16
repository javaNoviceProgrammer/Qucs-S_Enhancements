# A VCSEL's threshold and light-current curve: the cavity's mode with the carriers and the wells' gain

The request (2026-10-15, again 2026-10-16): after the full-vector axisymmetric solver (stage 1,
68c6656, `2026-10-15-claude-vcsel-bor-solver.md`), "coupling it to the existing semiconductor
and quantum-well physics, as Lumerical does with CHARGE and MQW, to get threshold and
light-current curves". This is that step.

## What it is

**A semiconductor's Lasing (Cavity Mode)** (`"type": "laser"`, a domain feature on the gain
region, its wells) lases in the mode a resonator's Eigenfrequency step kept. The photons in
that mode, S, are one more unknown of the semiconductor's Newton's method, solved with the
potential and the carriers:

    (G - 1/τp) S + β Rsp = 0        the photons' balance
    G = Σ_i v g(N_i) Π_i            the modal gain rate (1/s)
    R_stim,i = v g(N_i) Π_i S       taken from node i's electrons and holes (pairs/s)

- τp = Q/ω, the cold cavity's photon lifetime, from the mode's complex eigenfrequency;
- Π_i, node i's share of the mode's stored energy: over the gain region's triangles a
  third of each one's ∫ ½ ε0 Re ε |E|² dV to each corner, over the mode's U (the resonator's
  new emr.U, emr.we); Σ Π = Γ, the confinement - the light's lateral and axial profile
  in it, so spatial hole burning is the solution's own;
- v = c/ng in the gain region (ng given, else √Re ε there);
- g(N), the material gain at the mode's wavelength of the carriers there (N = √(n p), or n,
  or p): logarithmic g0 ln((N + Ns)/(Ntr + Ns)), linear a (N - Ntr), or **the quantum wells'
  own** - a Quantum Wells physics' TE (or TM) gain at the mode's photon energy, solved over
  a table of densities (`Nmin` to `Nmax`, by their logarithm) with a Spectrum step's options
  (states, broadening, lineshape; that step may be disabled - it is only read), a monotone
  cubic in ln N between (Fritsch and Carlson: Newton's method sees a smooth derivative);
- Rsp, the radiative recombination on the gain region (B (n p - ni²), its Radiative feature's
  B); β of it into the mode.

S is scaled and kept positive as the carriers are; from nothing it starts at the
spontaneous steady state τp β Rsp/(1 - τp G). Below threshold S is spontaneous; above it the
carriers clamp - the modal gain at 1/τp - and the current goes to light.

**A study step Laser L-I** (`"type": "laser"`: `"contact"`, `"voltages"`) holds that contact at
each voltage and solves the physics at rest there (the semiconductor - not the modes, not the
wells), each point from the last, each a solution (the solution's axis its voltages, V); it
makes the wells' table first; afterwards its log gives the threshold - the light's line above
it extrapolated back to none - the voltage there, the slope (W/A) and photons out per
electron.

**Outputs**: semi.S, semi.Pout (η ħω S/τp, η the share of the loss that is light out),
semi.G, semi.Gnet = G - 1/τp (nought at threshold, negative below), semi.Gamma, semi.gth =
1/(τp v Γ), semi.gmean (the mode's mean gain), semi.Istim (q S G), semi.Isp, semi.Nact,
semi.taup; at each point semi.gain and semi.Rst.

**Around it**:
- the resonator's mode energy emr.U = ½ ε0 ∫ Re ε |E|² dV (the absorbing layers left out),
  emr.Ploss = ω U/Q, emr.we its density, emr.epsr;
- the Eigenfrequency step's `keep` - the mode the steps after see (its number; 0 the one of
  highest Q);
- a 1D plot group's `x`: a global along x in place of its one axis (`"x": "semi.I_anode"`:
  the light against the current);
- Quantum Wells along a line about an axis: a 1D line along the axis (y) - planar wells
  across it, a VCSEL's - was refused ("about an axis it is 2D"); now solved, the same states
  as in a plane; a line along r is still refused.

Refused, and said: a Laser L-I step without a semiconductor that lases, before its mode is
solved, with a contact the semiconductor has not, in a cavity that loses nothing (Q
infinite), with the mode's energy not on the gain region; lasing in a step in time or
frequency, or in a semiconductor solved in 1D; a quantum gain outside a Laser L-I step.

## Checked

`test_fem_laser` (new):

- **A toy laser against its rate equations**: a metal pillbox (1 µm radius and height) of
  n 3.5 - 0.001j, its TM010 mode E_z = J0(kr), a 20 nm GaAs gain disk of radius 0.2 µm at its
  middle between Al0.6Ga0.4As claddings, linear gain:
  - Γ against Bessel's (h_a/h) b²(J0(kb)² + J1(kb)²)/(a² J1(ka)²): 0.002801497 / 0.002801527;
  - the stored energy against ½ ε0 Re ε h π a² J1(ka)²: 2.7e-4;
  - above threshold the carriers at Ntr + 1/(τp v Γ a) (1e-3), the mode's gain at 1/(τp v Γ)
    (1e-4), the disk's spontaneous current q B N² V (3e-3);
  - below it S = τp β Rsp/(1 - τp G) (1e-6), the net gain negative;
  - at every point the photons' balance S/τp = G S + β Rsp and the light ħω S/τp;
  - the slope 0.13519 W/A against ħω/q (0.13558 V) times the differential efficiency
    0.99707 the run reports (2e-3);
  - ∫ semi.Rst dV over the disk the stimulated photons Istim/q (2.5e-4), semi.gain's mean
    there the mode's mean gain (7.6e-4).
- **Logarithmic gain of the electrons**, the disk p-doped (p 1.26e19, n 9.27e18 per cm³ - the
  reason to take n): semi.gain at every point of the disk g0 ln((n + Ns)/(Ntr + Ns)) (6e-4 of
  gth at most, the gain linear between nodes and n not), the mode's mean gain at gth (2e-6);
  without ng the photons' speed c/√Re ε (gth to 1e-9).
- **The wells' table**: a gain linear in ln N interpolated exactly (1e-12), its derivative
  too; a saturating table and one with plateaus monotone between their points and within
  their values, the derivative the values' (1e-5), held beyond the ends with derivative
  nought.
- **The kept mode**, each of three; **planar wells about an axis** - their states (e1, hh1,
  e2) those in a plane to the last digit, a line along r refused; **refusals**.
- **The example** (below), at six of its voltages: the numbers its description gives.

`test_multiphysics_doc` lightAgainstCurrent: a 1D plot group's dataset against
semi.I_anode (each point's value), and without x against the voltages.

## The example

`examples/multiphysics/vcsel_laser.qfem` - the 980 nm oxide VCSEL of `vcsel_modes.qfem` made
a laser:

- **The structure**: three 8 nm In0.2Ga0.8As wells in GaAs barriers at the cavity's middle
  (Al0.3Ga0.7As spacers, the cavity one wave), mirrors doped 2e18 per cm³ (n below, p above;
  their layers one graded medium electrically - abrupt GaAs/Al0.9GaAs steps would make the
  mirrors' series resistance the model's subject), a gold ring contact on top from r 2.5 µm,
  the oxide's 2 µm aperture taking the current to the wells.
- **The mode**: HE11 at 980.30 nm, Q 15 800. The wells' material is given κ 0: their
  absorption is the gain's (−4323 per cm at the table's lowest density), not the cold
  cavity's too. With the library's κ the cold cavity's Q was 400 - counted twice.
- **The wells' TE gain** at 1.2648 eV: parabolic bands, 3 states, a sech line of 6.6 meV;
  transparent at 8.6e17 per cm³, 5039 per cm at 2e19.
- **Threshold** 0.13 mA at 1.40 V (the mode's gain at threshold 409 per cm, Γ 0.042), then
  0.59 W/A - 0.47 photons out the top an electron -, 1.1 mW at 2.1 mA (1.90 V); 31 points in
  93 s.
- **η 0.484**, the top's share of the cavity's loss, by the mode's Poynting flux through a
  line in the air above (48.4 % up, 41.0 % into the substrate, 10.6 % sideways and into the
  gold): its bottom mirror's 25 pairs leak almost as much as the top's 20. A design meant to
  emit from the top would add pairs below.

## Breaks

Thirty-eight changes broken on purpose (each built, the tests run, the file restored):
29 caught at once, 7 after the tests were made to see them, 2 not.

Caught at once: the stimulated rate not taken from the carriers, or its sign in the holes'
row; a node's share of the mode not over U, a triangle's energy all to each corner, the
share by area (no 2πr), Γ over the whole semiconductor; ng ignored; β left out; τp = Q/f;
the spontaneous rate without the nodes' volume (in the globals, in the photons' row); Pout
without η; semi.Rst not per volume; the logarithmic gain without Ns; U without 2πr or
without ε, emr.we without ε; the contact not held at each voltage, the held voltage not the
contact's; the wells' table TM for TE, at another photon energy; the threshold's fit over
every point; the step's readiness check left out; keep 0 the last mode, not the highest Q;
the bias axis named time; a plot group's x ignored; the line about an axis refused again;
semi.Rst not declared in a solution's own variables; lasing in time not refused.

Caught after the tests were made to see them:
- **v from Re ε** (no ng) - every test gave ng; the logarithmic test now gives none.
- **semi.gain left nought** - nothing read it; its mean on the disk now.
- **N √(n p) when the electrons are asked** - the check compared semi.Nact, which reads the
  same choice, and in the undoped disk n and p were 0.07 % apart; the disk is now p-doped
  and the gain checked against n at every point.
- **The table linear in N**, **its slopes the differences' mean** (it overshoots beside a
  plateau), **its derivative beyond the end not nought** - the example's 25 densities hid
  the first; a test of the table itself now (the last at first passed: QCOMPARE takes
  1e-12 for nought, the derivative 7e-23 - the comparison made exact).
- **keep ignored** - the pillbox's modes have one Q (n/2κ: the loss a material's), so the
  highest-Q mode was by chance the one asked; each of three is kept now.

Not caught, and why:
- **S from nought** at the first point, not the spontaneous steady state: the photons' row
  is linear in S at given carriers, so Newton's first step from nought lands where the
  start puts it - redundant, as it is.
- **The gain's derivative left out of the electrons' rows of the Jacobian**: Newton's
  method converges to the same residual's root, more slowly (the example 83 s against 77).

## Not done

- **Self-heating** (a laser's roll-over) and the temperature of the gain - a heat physics
  coupled to the semiconductor's heat exists; its coupling to the gain table (its T) and the
  cavity's dn/dT is the next step.
- **Dynamics** (modulation, turn-on): the photons' equation is solved at rest alone - a step
  in time or frequency with lasing is refused.
- **Several modes** (HE11 and the next competing): one mode lases.
- **η by the solver**: given, here from the mode's Poynting flux worked outside.
- **A Layered Region over a domain that is not a rectangle** (the example's air, notched by
  its gold ring): the mesher says so and meshes that domain freely, but the cavity's solve
  then found a singular matrix (a zero column) - the example keeps its air out of the
  Layered Region; not followed further here.
