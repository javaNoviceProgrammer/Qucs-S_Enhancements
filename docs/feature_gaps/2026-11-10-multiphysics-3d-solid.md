# Multiphysics 3D: solid mechanics, 10 November

The first of the three physics the plan (2026-11-10-multiphysics-3d-more-physics-plan) brings to 3D models. Asked
its open choices, the user answered: solid mechanics first, then magnetics, then semiconductors; and the piezoelectric
effect and eigenfrequencies in this round, not later.

## The field

- **A vector field in 3D.** `Field3` carries components now (`components`, u, v, w), one after the other in `values`
  as the 2D `Field` has them. A variable reads its component by `VariableDef::part`. The point evaluator, the
  nodes' values the view colours by, the snapshots and the coupling's relative change all take it.
- **The displacement** (u, v, w) on `Space3`'s Lagrange prisms, order 1 or 2. **A 3D solid is made of order 2**, and
  read as order 2 where its file gives none (the scalar physics stay order 1). A linear prism is stiff in bending:
  on the cantilever below, with four layers, order 1's tip is 1.8 % short of order 2's. With fewer layers it is worse.
- **The equations** are each degree of freedom's three components together, so multigrid can aggregate a node as one
  point.

## The physics

`fem3_solid.cpp`:

- **Hooke's law**, σ = C (ε - ε0) - eᵀE, -∇·σ = f:
  - C is isotropic (E, ν), or Voigt's 6 × 6 in the model's axes. In 3D all 21 entries may be named.
  - A material's crystal stiffness is turned by its orientation, then into a 3D model's axes: x the same, y the
    plane's -z, z the plane's y (the scalar tensors' turn, as a rotation).
- **The thermal strain** α (T - Tref), T any expression (ht.T: heat's field, solved in turn). α is one value, or a
  tensor for an anisotropic solid.
- **Constraints on faces**, later features over earlier ones:
  - Fixed.
  - Prescribed Displacement in x, y and now z (w0).
  - Roller: a plane face across an axis holds that component exactly. An oblique or curved one is held by a stiff
    spring along its normal, 1e5 times the stiffest modulus over the face's size, as 2D's oblique roller is.
- **Loads:**
  - A Boundary Load of Fx, Fy and now Fz per area, or a pressure along the inward normal. The normal is the
    gradient of the reference coordinate that is nought on the face, in the prism beside it, so it is exact on an
    order 2 arc.
  - A Body Load of fx, fy and now fz.
- **The reactions:** the fixed degrees of freedom's rows less their loads, and an oblique roller's springs, by
  component.
- **Results:**
  - Variables: solid.u, v, w (and plain u, v, w) and disp; the strain's six components (ex, ey, ez; eyz, exz and
    exy the tensor's, half the engineering shears); the stress's six; Ws, mises, p; and E, nu, rho, dT.
  - Globals: solid.dmax, solid.W (∫ Ws dV) and solid.Rx, Ry, Rz.
- **The solve:**
  - Conjugate gradients with smoothed aggregation, given the six rigid-body modes as its near null space, and
    blocks of three where no component is held alone.
  - Or the Cholesky, asked or chosen, as the scalar physics' are.
  - `AmgSolver::setNearNullspace` and `System3::setNearNullspace` carry the modes.

## Eigenfrequencies

An **Eigenfrequency step** in a 3D model:

- **What it solves:** a solid's natural frequencies, K x = ω² M x, M the consistent mass ∫ ρ φi φj. Its physics at
  rest are solved first.
- **How:** shift and invert about σ = (2π f)² - 1 s⁻², f its new 3D property "Search about" (fshift, 0 Hz by
  default; the plane's wavelength is the 2D resonator's). The 1 s⁻² keeps a free solid's rigid motions from making
  K - σM singular. The 2D solver's Krylov-Schur method (`largestEigenvalues`) is run on (K - σM)⁻¹ M, factorized by
  an LU.
- **Its modes:** each a snapshot of its frequency, lowest first, the field scaled to a largest component of 1. The
  globals solid.freq, solid.omega and solid.mode are each mode's. The tab's selector reads "Eigenfrequency: 1:
  8.101 kHz". `Solution3::eigen` marks such a solution.
- **ρ** comes from the material, or (the user's) a new 3D property Density of the Linear Elastic Material.

## The piezoelectric effect

- **The solid's side:** a Piezoelectric Effect names its electrostatics. The solid's stress takes -eᵀE, e by Voigt's
  3 × 6: the feature's own in the model's axes, or the material's e (or d C) turned into 3D's axes.
  `piezoMatrix3` is shared by the two physics.
- **The electrostatics' side:** in 3D it now takes the polarization P = e S where such a feature names it. That is
  P·∇φ on the right of each equation, and P in es.Dx, Dy, Dz.
- **Solving:** the two are solved in turn until they agree.
- **What each charge is:** a terminal's charge is the free one; the terminal sweep's matrix (no sources) the
  clamped one, as in 2D.

## The window and the tools

- **The panel:** its Add menu offers Solid Mechanics in 3D, with its z components shown and the plane's 2D
  approximation not. A step may be an Eigenfrequency one.
  - The schema's `inSpaces()` marks a property as one space's.
  - `solvedIn3D` takes solid/* and eigenfrequency.
- **Plot** offers solid.disp, mises, u, v, w and Ws.
- **The tools' description** for Claude (fem_model's 3D model) names the solid's 3D properties and variables.
  fem_solve and fem_evaluate need nothing new.
- **The estimate** counts three unknowns at each degree of freedom, nine times the nonzeros
  (`estimateOn(..., components)`).

## Validated

| Check | What it holds |
|---|---|
| Patch tests | A uniform strain held on every face of two boxes: order 1's at every degree of freedom to 1e-9, its stress C ε; u = a x² held with its body load -(λ + 2µ) 2a: order 2's |
| A cantilever | 10 × 1 × 1 mm, fixed at its root, 10 N at its tip, ν 0: order 2's tip 201.19 µm against Timoshenko's 201.20 µm (bending and shear, κ 5/6), 0.005 % off; order 1's 197.54 µm. Its reaction 10 N, its strain energy P δ / 2, its root's stress M c / I |
| Pressure on every wall | A quarter of a solid cylinder pressed all round, on rollers: u_r -p (1 + ν)(1 - 2ν) r / E within 0.2 % at five angles; a cube pressed on three faces: -p (1 - 2ν)/E each way to 1e-9 |
| Lamé's cylinder | 1 mm to 2 mm, 100 MPa inside, in plane strain (a quarter, on rollers): u_r at 1.02, 1.5 and 1.98 mm within 0.006 %; σθ 92.37 MPa against 92.59 MPa (0.24 %), σr -26.06 against -25.93 MPa (0.5 %); σz ν (σr + σθ) |
| A free bar warmed | 100 K, on three planes of symmetry: its corner α ΔT (L, W, H) to 1e-12 m, no stress, solid.dmax √18 α ΔT mm, its nodes' u and v |
| Its weight | ρ g along -z on a fixed base: the reaction ρ g V to 1e-9 |
| A bimetal strip | Two layers 0.5 mm alike but for α (1e-5, 2e-5 per K), 50 K from a heat physics, clamped: its tip -150.04 µm against Timoshenko's (1925) -150.00 µm, 0.03 % |
| Silicon | C11 165.7, C12 63.9, C44 79.6 GPa from its material, pulled along x: E 130.1 GPa along [100] and, the crystal turned 45° about the wafer's normal, 169.1 GPa along [110], to 1e-6; its sides' strains S12 σ along [001] and (S12 + (S11 - S12 - S44/2)/2) σ along [1-10]; no shear |
| Uniform in z | Two materials, fixed, pulled and sheared, warmed along x, its top and bottom on rollers: the 2D model in plane strain on the 3D plan mesh, every u and v to 1e-9 and w nought, solid.W, Rx, Ry, dmax, and σz at a point - at order 1 and 2 |
| Eigenfrequencies | The cantilever, steel: 8100.9 Hz twice against Euler and Bernoulli's 8153.8 Hz (0.65 % under, Timoshenko's shear and rotary inertia); 48920.5 Hz twice against 51099 Hz (4.3 % under); its torsion 82101.8 Hz against Saint-Venant's square's 81954.6 Hz (0.18 %); its axial c/(4L) 126189 Hz, exact to six figures |
| Piezoelectric | A block of e33 15 C/m², E 100 GPa, ν 0, εr 1000, at 10 V: its top -e33 V / C33 to 1e-6, its charge the free capacitance (ε + e33²/C33) A/h, the sweep's es.C_top_top the clamped ε A/h, Dz |
| Multigrid | The rigid-body modes and blocks: a 6 mm cantilever of 20,493 order-2 unknowns, as a direct solve has it to 1e-6, in 94 iterations (113 without the blocks; ILU(0) 72, SPAI-0 109) |

**Multigrid on a thin beam:** 94 iterations is many for smoothed aggregation. A bending beam's slow modes are hard
for it, and order 2's matrices on long prisms are not what it is made for. The smoothers measured are within a third
of each other, so Gauss-Seidel, the scalar physics' smoother, stays.

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_solid` (new, 14 functions) | The checks above, and what is refused: a solid held nowhere, a domain without its elastic properties; a 3D solid of order 2, w0 a 3D model's alone, the plane's approximation not; the estimate's three unknowns a dof |
| `test_multiphysics3d_doc` `aSolidThroughTheTools` (new) | A cantilever made by fem_model, saved as a .qfem3d and read back (order 2, Fz), solved by fem_solve, solid.Rz 10 N and solid.w at the tip by fem_evaluate, Plot offering solid.mises and solid.disp |
| `test_multiphysics3d_doc` `physicsThroughThePanel` (changed) | Solid Mechanics offered on the component: order 2, no 2D approximation, Prescribed Displacement's w0 and z shown |
| `test_fem3_geometry` `schema` (changed) | A 3D model's physics now include solid; magnetics is refused |
| `test_fem3_solve` `refusals` (changed) | A magnetic physics in a 3D file is "not solved in 3D yet" (it was solid mechanics) |

## Broken on purpose

`docs/feature_gaps/2026-11-10-multiphysics-3d-solid/breaks.py` holds 18 breaks, each built and its suite run through
ctest:

- the strain's yz column; the isotropic λ without its (1 + ν); the thermal strain's sign;
- a wall's normal inward on its long edge; a roller across an axis holding nothing; the reactions with the loads
  added;
- a crystal's turn the plane's, not 3D's; a vector field's components all its first, at a point and at the nodes;
- the body load's z; a held value's components all its x's;
- the eigenvalue's shift the wrong way; the mass without its density;
- the piezoelectric stress's sign; the polarization's sign;
- multigrid without the rigid-body modes;
- a 3D solid made of order 1; a displacement's w0 shown in 2D too.

All 18 are caught. One needed a second look:

- **A wall's normal inward on its long edge** was not caught at first. A wall lies on one of its triangle's three
  edges, and the normal is found differently on each. On Lamé's inner arc no wall happened to lie on a triangle's
  edge from its second corner to its third. `pressureOnEveryWall` presses a quarter cylinder all round, and a cube
  on three faces: the cube's faces caught it, εx 1.4e-4 against -4e-4.

## Not yet

- **The view** colours the solid by any of its variables, but does not draw it deformed.
- **A harmonic (frequency) response**, prestress in the eigenfrequencies, large deformation and contact: later,
  as the plan has them.
- **The sanitizers:** not yet run on this change.
