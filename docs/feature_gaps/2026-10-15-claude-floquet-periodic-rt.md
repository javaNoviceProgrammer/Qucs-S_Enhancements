# Feature gaps: Floquet periodicity, reflection and transmission in the full-wave optics

*15 October 2026 — on `1aff299`. The gap, as reported: no Floquet (periodic) boundary and no
reflection/transmission outputs in the full-wave optics.*

The frequency-domain waves (`waves`, tag `ewfd`: Ez or Hz in the plane) had conductors, a
scattering boundary that a plane wave could come in by, perfectly matched layers and ports
across a waveguide (its first mode, the S-parameters). Nothing in the engine was periodic,
so a grating, a film or a metasurface could only be modelled as a finite piece between
absorbers, and no physics said how much light was reflected or transmitted.

## Now

| Gap | Now |
|---|---|
| A periodic boundary | **Periodic Condition** (`"periodic"`), a waves boundary feature on a unit cell's two opposite walls in one selection (`{"of": [its layers], "side": ["left", "right"]}` - a selection's `"side"` may now be a list). The selection is split into its two sides, each a connected run of boundaries; one must be the other moved (as long, each one's points on the other's), the source the side lower in x, then y. u(r + d) = u(r) e^-jkF·d: kF the fed periodic port's plane wave's along the cell (`"floquet": "port"`, the default; continuity without one), nought (`"continuity"`) or given (`"given"`, kx and ky). |
| Floquet ports | **Periodic Port** (`"periodicport"`), on the cell's top and bottom: one fed, a plane wave at `"angle"` degrees from its normal into the cell, toward +x (+y for a port along y), of amplitude E0; each diffraction order m - the plane wave e^-jk_m·r, k_m = (kF + 2πm/L) along the port - taken out without reflection, those that die away included (all that propagate in any port's medium and ten more each way, as many as the port's edges resolve). A port is one straight period long, along the cell's d, in one medium (its axes along it and across). |
| Reflection and transmission | ewfd.R, ewfd.T, ewfd.A (1 - R - T); each order that propagates, ewfd.R_m at the fed port and ewfd.T_m at the other (m written n1 for -1; T_<port>_m and T_<port> with more ports); the zeroth orders' fields over the incident's at the ports' lines, ewfd.r_0, r_0_im, t_0, t_0_im (their phase along the lines from one origin); ewfd.kF. The log lists each port's propagating orders and their angles. A waveguide's ports: ewfd.R, T, A of the first port fed, from the S-matrix (|S11|², Σ|Sj1|²). |

Ez is TE (s), Hz TM (p). The example `grating_orders.qfem`: a lamellar glass grating on glass,
lit from air at 632.8 nm, swept from 0° to 40°.

**The mesh.** A periodic condition's sides are meshed alike: the mesher gives a pair of
boundaries, one the other moved, the nodes of both, and once it has triangulated (its
refinement splits boundary edges where triangles encroach) it moves each node of one side
onto the other and splits the edge there where that side has none. The solver ties each
destination node to its source node (u = T v) and tests the equations with T's conjugate - the
other side's phase - so the walls' fluxes cancel: Tᴴ A T v = Tᴴ F. Two conditions (a cell
periodic both ways) tie a corner through both; a node tied twice takes the first's.

**Refused, and why said:** two periodic ports fed; none fed; periodic ports beside waveguide
ports; a periodic port with no Periodic Condition one period along it apart (or one shorter
than the period); a port across two media; a medium turned in the plane at a port; walls that
are not one the other moved, or one run (a wall and the top); a condition's kF that is not
the fed port's plane wave's (continuity with a wave at 30°: its orders would not be the
field's).

## Checked

Air over glass (n 1.5), a cell 0.4 µm wide at λ 1 µm, the interface 1 µm from each port, hmax
0.02 µm, quadratic elements:

| | R | Fresnel | \|r_0 - r e^-2jβ1d\| | \|t_0 - t e^-j(β1+β2)d\| |
|---|---|---|---|---|
| TE 0° | 0.0400000 | 0.0400000 | 1.4e-7 | 2.5e-6 |
| TE 30° | 0.0577961 | 0.0577961 | 1.3e-7 | 2.5e-6 |
| TE 60° | 0.1765715 | 0.1765715 | 5.9e-7 | 2.3e-6 |
| TM 0° | 0.0400000 | 0.0400000 | 1.4e-7 | 3.8e-6 |
| TM 30° | 0.0252491 | 0.0252491 | 2.0e-7 | 3.8e-6 |
| TM 60° | 0.0018019 | 0.0018019 | 7.0e-8 | 3.8e-6 |

R + T is 1 to 8 digits; T as Fresnel's to 1e-7. TE at 30°, r_0's error 2.5e-6, 1.3e-7 and
7.7e-9 at hmax 0.04, 0.02 and 0.01 µm (quadratic elements' order).

A film 0.3 µm of n 2 on glass, and the same absorbing (n 2 - 0.1j), against Macleod's
characteristic matrices (written in the test): R, T and A to 2e-6, TE and TM at 0° and 45° -
the absorbing film's A 0.29142 (0.29142) TE at 0°, 0.32970 (0.32970) TM at 45°.

A lamellar grating - ridges of n 1.5 half of a 1 µm period, 0.3 µm deep, on glass - from air
at 10°, λ 632.8 nm, TE, against rigorous coupled waves (Moharam et al. 1995, 81 orders, written
in the test; its homogeneous limit the characteristic matrices' to 1e-9):

| Order | R | coupled waves | T | coupled waves |
|---|---|---|---|---|
| -2 | - | - | 0.045069 | 0.045068 |
| -1 | 0.005655 | 0.005655 | 0.178287 | 0.178287 |
| 0 | 0.021350 | 0.021350 | 0.552516 | 0.552515 |
| +1 | 0.002196 | 0.002196 | 0.185740 | 0.185741 |
| +2 | - | - | 0.009185 | 0.009185 |

Nothing lost (Σ = 1 to 1e-9). With the ports 50 nm from the grating, in its near field, each
order the same to 5e-4 - the evanescent orders taken out by the ports (none kept, or their β
on the wrong branch, and this fails).

A cell whose left wall has points of its own (a block against it): its walls meshed node for
node, R as Fresnel's.

## Found on the way

The first cell reflected about 1e-3 of the field where nothing should (air on air) at oblique
incidence, growing as sin²θ and not shrinking with the mesh - linear and quadratic elements
alike. The fed port's trace was the incident wave times a sawtooth across the period (each
order's amplitude about 1/|m|), and the reduced equations' residual for the exact plane wave
sat on the source wall: a destination node's flux was four times its source node's, a
quadratic element's middle node's against a corner's. The two walls had been given the same
nodes, but the triangulation's refinement had split one wall's edges, so a corner node on one
side faced a middle node on the other, and tying the two equated different traces. The mesher
now matches the walls after triangulating; the tie by interpolation stays for walls that still
differ, said as a warning (less exact), and no mesh made here leaves them so.

## Open

- **TM gratings** against an independent method: the TM checks here are planar (Fresnel, the
  films); TM coupled waves (Li's factorization) are not written.
- **A cell periodic both ways** (two conditions): its corners' ties chain, untested - there is
  no room for periodic ports in it, and no source in `waves` to drive it.
- **Bloch modes** - periodicity in the mode solver, a photonic crystal's bands - and
  periodicity in the other physics: not done.
- **Conical incidence** (out of the plane): `waves` is TE or TM in the plane.
- A **lossy port medium**: the angle taken of its index's real part (said in the log).
- A cell whose d is not along its ports (a skewed cell): refused.

## Tests

`test_fem_waves`: periodicFresnel, periodicThinFilms, gratingOrders (with the ports in the
near field), periodicSidesMeshedAlike, periodicRefusals; irisKeepsPower (R, T, A of the
waveguide ports); theExamples (`grating_orders.qfem`: at 10° each order as the coupled waves,
R + T 1 at each angle, the +1st reflected order gone past grazing at 40°).

## Breaks

Each made alone, the new tests run:

| Break | Caught by |
|---|---|
| T transposed, not conjugated | Fresnel, films, grating, sides |
| The Floquet phase's sign | Fresnel, films, grating, sides |
| The walls not matched after triangulation | Fresnel, films, sides |
| No evanescent orders at the ports | grating (near field) |
| β on the growing branch | grating (near field) |
| An order's power without its medium's p | Fresnel, films (TM) |
| The dual orders not conjugated | Fresnel, films, grating, sides |
| A condition's kF unchecked | refusals |
| A list of sides: its first only | all five |
| A waveguide's R as \|S11\| | iris |
| The incident wave's drive halved | Fresnel, films, grating, sides |
| Negative orders named m1 | grating |
| A port's length unchecked | refusals |

13 of 13.

## Builds

The Release suite, 122 of 122 (192 s, eight at a time). `test_fem_waves` under the sanitizers
(Debug, ASan, UBSan), all 23, no report: 589 s alone (482 s before, four at a time) - on a slow
build the new tests' meshes are twice as coarse (0.04 µm; t_0 then 6.4e-5 off, a film's T
2.1e-5) and the example solves three of its angles (64 s), its first run of all of them 886 s.
The slow path run in Release too (QUCS_TEST_SLOW). Meshes of models without a periodic
condition are made as before, node for node.
