# Multiphysics 3D: solid mechanics, magnetics and semiconductors - the plan, 10 November

The request: add the three physics a 3D model can list but not solve - `solid`, `magnetic` and `semiconductor` -
each held to analytic or published results, addable from the 3D panel, in plot groups and derived values, estimated
before a solve, open to the `fem_*` tools and saved in `.qfem3d`. Today `unsolvedPhysics3()` refuses all three.

## Decided

Asked the plan's open choices, the user answered:

- **The order**: solid mechanics, then magnetics, then semiconductors.
- **The gauge**: a Lagrange multiplier.
- **The semiconductor's features**: all of the 2D solver's, in 3D where they mean something there.
- **Brought forward from later**: B-H curves, the piezoelectric effect in 3D, small-signal C-V and
  eigenfrequencies.

## What is there to build on

- **`Space3`**: Lagrange prisms of order 1 (lumped) and 2. A field is one scalar by degree of freedom (`Field3`).
  A vector field (u, v, w) needs `Field3` to carry components, as the 2D `Field` does (`components`, one after the
  other).
- **`EdgeSpace3`**: Nédélec prisms of order 1 (9 functions) and 2 (36), with their curls, made for `waves3`.
- **`System3`**: CG with AMGCL's smoothed aggregation (ILU(0)), Accelerate's or Eigen's Cholesky, and for a
  matrix not symmetric an LU or BiCGStab with multigrid. Newton's method and parallel assembly in parts.
- **The study**: Stationary and Time Dependent steps, physics solved in turn until they agree, the Parametric
  Sweep, snapshots, `Estimate3`.
- **The 2D physics**: `fem_elastic.cpp` (865 lines), `fem_magnetic.cpp` (1,602), `fem_semiconductor.cpp` (8,737).
  The semiconductor's Scharfetter-Gummel edges are weighed by the linear triangle's cotangents, its nodes' boxes
  the triangles' Voronoi shares.

## The order

1. **Solid mechanics first.** It is the smallest step from what is there: nodal prisms, a symmetric positive
   definite system, one-way coupling from heat. It also brings the vector field to `Field3`, which the others want.
2. **Magnetics second.** The edge elements exist; what is new is the gauge, the coils and the forces. Its coils
   read the currents physics.
3. **Semiconductors last.** The largest, and the one that reuses most of the 2D solver (its mobility,
   recombination and doping models), which wants those pulled out of `fem_semiconductor.cpp` into code both can
   call.

Each lands with its tests, its breaks-on-purpose script and its write-up, as the phases before did.

## Solid mechanics

- **The field**: the displacement (u, v, w) on `Space3`, order 1 or 2; order 2 by default, as in 2D. A linear prism
  locks in bending (a cantilever of one layer is far too stiff), which the write-up will show and measure.
- **The material**: isotropic (E, ν), or anisotropic by Voigt's 6 × 6 Cij in the model's axes - all 21 entries in
  3D, not 2D's nine. A crystal's stiffness from its material, turned by its orientation and then 90° about x into
  the 3D axes, as the scalar tensors are.
- **Conditions**: Fixed; Prescribed Displacement in x, y and now z; Roller; Boundary Load with Fz, or a pressure;
  Body Load with fz; thermal expansion α (T - Tref), α one value or a tensor, T read from heat. The 2D choice of
  plane strain or stress is not shown in 3D.
- **The solve**: CG with smoothed aggregation, given the six rigid-body modes as its near null space (elasticity's
  multigrid converges poorly without them); the Cholesky as for the scalar physics.
- **Results**: solid.u, v, w, disp; the strain's and the stress's six components; von Mises; the strain energy
  density. Globals: the greatest displacement, the strain energy, and each fixed or prescribed boundary's reaction
  force (solid.Fx_<name>...).
- **Eigenfrequencies**: an Eigenfrequency step, K x = ω² M x with the consistent mass ρ, by shift-and-invert
  Lanczos about a frequency asked (the 2D solver's own eigen solver, if it fits, else Spectra's); the modes as
  snapshots, each of its frequency.
- **The piezoelectric effect**: stress and charge in one, σ = C S - eᵀ E, D = εS E + e S, e by Voigt's 3 × 6 in
  the model's axes or the material's turned; solid and electrostatics in turn until they agree at rest, as 2D does
  it (and in one matrix if that is what converges).
- **Validation**:
  - Patch tests: a uniform strain exact at order 1, a quadratic field at order 2.
  - A cantilever's tip deflection, Timoshenko's beam (bending and shear) against the refined mesh, order 2.
  - Lamé's thick-walled cylinder under internal pressure: u(r), the hoop and radial stress, on its arcs.
  - A free bar's thermal expansion: α ΔT L, and no stress; a bimetal strip's curvature (Timoshenko, 1925) as heat
    and solid coupled.
  - Silicon's anisotropy: a cube pulled along [100] and along [110] gives E = 130 and 169 GPa.
  - Uniform in z: a 3D slab between rollers equals the 2D model in plane strain, to 1e-9.
  - Eigenfrequencies: a cantilever's first bending frequencies, 1.8751² √(EI/ρA)/(2π L²) and on (Euler-Bernoulli,
    then Timoshenko's), and a free-free bar's axial ones.
  - Piezoelectric: a disc's thickness strain d33 V and its clamped and free capacitance; uniform in z against 2D's.

## Magnetics

- **The field**: the vector potential A on `EdgeSpace3`, order 1 or 2, ∇ × (µ⁻¹ ∇ × A) = J + ∇ × (Br/µ0 µr).
- **The gauge**: the curl-curl matrix is singular, its null space the gradients. **A Lagrange multiplier**, a nodal
  scalar ψ of the same order, holds ∇·A = 0 weakly: exact at both orders, no parameter to tune, and a source that
  is not divergence-free is taken care of by ψ. The system is a saddle point, solved by an LU, so its size is the
  direct solver's. (Not chosen: a small mass term with the source projected, for CG and multigrid; a tree-cotree
  gauge, order 1 alone.)
- **Materials**: µr isotropic, diagonal or symmetric (now 3 × 3), or a B-H curve by Newton's method at rest, as in
  2D (the saddle point's Newton matrix an LU's too).
- **Sources**:
  - An External Current Density of three components.
  - A Coil read from the currents physics: J = (N I0 / I_ec) ec.J, its direction and spread the currents' solution
    between the coil's terminals. One turn of a solid conductor and a homogenized multi-turn coil are then one
    formula.
  - A Magnetization: Br along a direction of three components.
- **Conditions**: Magnetic Insulation (n × A = 0); Magnetic Potential (A0 of three components: a uniform field B0
  is A0 = B0 × r / 2); Perfect Magnetic Conductor; Surface Current Density of three components.
- **Results**: mf.Ax, Ay, Az; B and H and their norms; the energy density. Globals: the energy mf.Wm; each coil's
  inductance from the energy and from its flux linkage ∫ A·J dV / I², and the coils' matrix by each in turn; the
  force on a domain by Maxwell's stress tensor over its surface (mf.Fx_<domain>...).
- **Validation**:
  - A finite solenoid's inductance by Nagaoka's coefficient.
  - A coaxial line's inductance at rest, µ0/2π (ln(b/a) + its conductors' internal terms), its currents from the
    currents physics.
  - A sphere of µr in a uniform field: B inside 3µr/(µr + 2) B0, uniform; a uniformly magnetized sphere, 2/3 Br.
  - Two parallel wires' force, µ0 I² / (2π d) per length.
  - Uniform in z: A along z equals the 2D model's Az, B and its inductance to 1e-9.
  - A B-H curve: an iron ring's flux against the curve's own (a toroid of one material, H = N I / 2π r), and uniform
    in z against 2D's B-H solve.

## Semiconductors

- **The discretization**: order 1 prisms alone, the box method on them. A prism's lumped order-1 Laplacian is the
  2D one times the layer along z: a horizontal edge weighed by its triangle's cotangent weight times half the layer
  heights about it, a vertical edge by its node's Voronoi share over the layer's height; a node's box its triangles'
  Voronoi shares times half its layers. Scharfetter-Gummel's current along every edge, as in 2D, so the 2D
  equations carry over edge for edge.
- **The unknowns**: V, n and p as the 2D solver has them, by Newton's method from equilibrium with the contacts'
  voltages raised in steps, as in 2D; BiCGStab with multigrid, or an LU.
- **Features**: all of the 2D solver's, in parts:
  1. *The core*: the material model, analytic doping in x, y and z, SRH, Auger and direct recombination,
     user-defined generation, insulation, metal (ohmic) and Schottky contacts, gate contacts through an oxide,
     surface charge and surface recombination; the mobility models of 2D (by doping, Lombardi's at a surface,
     saturation); the lattice temperature from heat and the carriers' heat semi.H for it (self-heating).
  2. *Statistics and junctions*: Fermi-Dirac's statistics, band gap narrowing, incomplete ionization;
     heterojunctions (abrupt, thermionic, tunnelling through the spike, graded).
  3. *Generation*: impact ionization, band-to-band and trap-assisted tunnelling, Fowler-Nordheim tunnelling,
     interface traps, optical generation (as given, or from a `waves3` solve's absorption), polarization charge.
  4. *Studies*: in time (BDF), small-signal in a frequency step (the contacts' admittance: high-frequency C-V),
     noise by the impedance field.
  5. *The rest*: Schrödinger-Poisson (a quantum wells physics' states in 3D, where its 2D one has a meaning),
     stimulated recombination and lasing, a magnetic field's Lorentz force (B now of three components). 2D's
     "solved along a line" is the plane's alone, not offered in 3D.
- **Results**: semi.V, n, p, the quasi-Fermi levels, Ec, Ev, the currents' densities, the net recombination.
  Globals: each contact's current and voltage; a Parametric Sweep of a contact's voltage gives an I-V.
- **Validation**:
  - Equilibrium: the built-in potential kT/q ln(Na Nd / ni²).
  - A pn diode's I-V against Shockley's, a short diode's (D/W) where the regions are short.
  - A MOS capacitor's quasi-static C-V, dQ/dV, against the exact 1D surface-charge solution (Sze).
  - A resistor's q µ N, with each mobility model's µ at its doping.
  - Uniform in z: the 3D model equals the 2D one, every node's V, n and p and every contact's current - and
    with each part's features, in time and small-signal, so 2D's validations carry over.
  - A MOS capacitor's high-frequency C-V against the depletion approximation's minimum (Sze), and its low-frequency
    limit against the quasi-static one.

## Every physics, too

- The 3D panel's Add menu, each feature's 3D properties (z components, 3D tensors) shown in 3D alone, as kzz is.
- Its variables in plot groups and derived values; the slice and the solution view.
- `Estimate3`: its unknowns and its solver's memory, said before the solve and held to the limit.
- `fem_*` tools for Claude: adding, setting, solving and evaluating, held in `test_multiphysics3d_doc`.
- Saved and read in `.qfem3d`; an example model for each.

## Later

- Solid: a frequency response (harmonic), large deformation, contact.
- Magnetics: eddy currents (A-V in the frequency domain), torque, magnetics in time.

## Where it stands, 11 November

- **Solid mechanics:** done, with eigenfrequencies and the piezoelectric effect
  (2026-11-10-multiphysics-3d-solid).
- **Magnetic fields:** done at rest, with B-H curves, coils from a currents physics and the Lorentz force
  (2026-11-10-multiphysics-3d-magnetic).
- **Semiconductors:** done (2026-11-11-multiphysics-3d-semiconductor) - all of the 2D solver's features but four
  that are the plane's alone: its 1D line, the process flow's doping, Schrödinger-Poisson and lasing (the last two
  need a quantum wells physics' states and a resonator's mode, which no 3D model has). A solver of its own, equal to
  2D's to rounding in a model uniform in z, feature by feature.
- **Found on the way, in 3D's code:** a floor face's charge lumped by thirds, not by its boxes' shares (the potential
  uneven across it); an interface node recombining over the other side's part of its box; a contact's current read
  from its rows, rounding-limited at a heavily doped contact - now weighed over the device, as 2D does.
- **Two deviations from the plan:**
  - A sphere in a uniform field is not a swept mesh's layered solid, so the magnetics' check is a cylinder across
    a field, exact for its bounded domain.
  - The magnetics' gauge system is factorized by an LDLᵀ, not an LU (18.6 s against 0.8 s).
