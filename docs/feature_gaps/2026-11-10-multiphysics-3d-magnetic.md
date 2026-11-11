# Multiphysics 3D: magnetic fields, 10 November

The second of the three physics the plan (2026-11-10-multiphysics-3d-more-physics-plan) brings to 3D models, after
solid mechanics (2026-11-10-multiphysics-3d-solid). Asked the gauge, the user chose a Lagrange multiplier, and B-H
curves in this round.

## The formulation

`fem3_magnetic.cpp`, at rest:

- **The unknowns:**
  - The vector potential A on `EdgeSpace3`, the full-wave solve's Nédélec prisms of order 1 (9 functions) or 2
    (36).
  - A multiplier ψ on `Space3` of the same order.
- **The equations:**
  - ∫ H(B)·∇×W + ∫ ∇ψ·W = ∫ J·W (+ ∫ Js·W on faces), with B = ∇ × A.
  - ∫ A·∇q = 0: the gauge, ∇·A = 0, held weakly (Kikuchi's mixed form).
- **Why the multiplier works:** the prisms' gradients lie in the edge space. ∇ of order k's nodal prisms is in
  order k's Nédélec prisms: the plan's triangles' gradients are in its edge functions, and d/dz of a degree-k
  polynomial is of degree k-1, E_z's own. So the multiplier takes exactly the curl-curl matrix's null space. The
  gradients are taken by the plan triangle's straight map, as the edge functions are. ψ is nought where A is held,
  and is held at one node where nothing holds it.
- **A source J that is not free of divergence** (a current ending at a face) is projected by ψ, not refused.
- **The saddle point** is solved by an LU, Accelerate's on macOS. The gauge's rows are scaled to the curl-curl's,
  ν0 / h² (h the mesh's typical prism), for the pivots.
- **Newton's method:** every solve is a step of it. A step solves the tangent matrix dH/dB, with the gauge's rows
  and columns, for δA and ψ from the residual ∫ J·W - ∫ H(A)·∇×W.
  - A linear model takes one step.
  - A B-H curve iterates until the relative change is under the step's tolerance. dH/dB = (H/|B|) I + (dH/dB -
    H/|B|) b̂ b̂ᵀ, the 2D curve's monotone cubic.

## Materials and sources

- **Ampère's Law:**
  - µr isotropic, diagonal or symmetric: in 3D its zz and its xz and yz too, ν = (µ0 µr)⁻¹ inverted at each point.
  - Or a material's µr, a tensor where it is one.
  - Or a B-H curve, the feature's or the material's. `bhCurveOf`, 2D's parser, is now shared.
- **External Current Density:** Jex, Jey and Jez, any expressions: a currents physics' ec.Jx is one.
- **Coil:** its current density is the currents physics' J between its terminal and the rest, scaled to N I0
  through it: J = N I0 ec.J / I_ec. The new 3D properties "ec" (which physics) and "terminal" choose them. A single
  conductor (N 1) and a homogenized multi-turn coil (N turns) are one formula. Its role and voltage are 2D's alone.
- **Magnetization:** Br along (dx, dy, dz), H = ν (B - Br), its µr the recoil one, Ampère's Law's.
- **Conditions:**
  - Magnetic Insulation, n × A = 0, on the physics' outer faces alone. Its default selection, every face, does not
    hold the faces between its domains.
  - Magnetic Potential: A0x, A0y, A0z, its n × A held by projecting n × A0 onto the faces' edge functions'
    tangential traces. A uniform B0 along x is A0 = (0, 0, B0 y).
  - Perfect Magnetic Conductor: natural, n × H = 0.
  - Surface Current Density: Jsx, Jsy, Jsz.

## Results

- **Variables:** mf.Ax, Ay, Az; mf.Bx, By, Bz, normB; mf.Hx, Hy, Hz, normH; mf.Wm (½ H·(B - Br), or ∫ H dB on a
  curve); mf.mur; and mf.Jx, Jy, Jz, the sources' current density.
- **Globals:**
  - mf.W, the energy ∫ Wm.
  - For each coil: mf.I_<name>; mf.Phi_<name>, its flux linkage ∫ A·J / I0; mf.L_<name>, Phi / I0; and the Lorentz
    force ∫ J × B on it, mf.Fx_<name>, Fy, Fz.
  - With the step's sweep, in a linear model, the coils' inductance matrix mf.L_<i>_<j> (mf.Lij for one-letter
    names): each coil alone at 1 A, by the same factors.
- **The estimate:** A's and ψ's unknowns, and a real LU's memory, some 10 kB an unknown (`estimateMagnetic3`).

## The window and the tools

- **The panel:** Magnetic Fields is offered in 3D, its 3D components shown and the plane's (Je, A0, Js, the
  magnet's direction, the coil's role, voltage and reference) not.
- **Plot:** mf.normB, Bx, By, Bz, normH, Wm and Az.
- **The tools' description** names magnetics' 3D properties and variables.

## The solve's speed

**The factorization is an LDLᵀ, not an LU.** The first version factorized the saddle point by Accelerate's LU:
53,705 unknowns (the solenoid at 0.25 mm, order 2) took 18.6 s, and the suite ran past QtTest's five minutes.
Accelerate's LDLᵀ with threshold pivoting is what `SparseComplexLdlt` offers. It is used here on the real matrix as
a complex one, its imaginary parts nought.

| Solve | Unknowns | Factorized |
|---|---|---|
| LU | 53,705 | 18.6 s |
| LDLᵀ | 234,345 | 7.9 s |
| LDLᵀ, one layer | 74,134 | 0.78 s |

The fields are the same to every digit printed. One step of iterative refinement follows each solve, and the
relative residual is said: 4.3e-14 on the solenoid.

## Validated

| Check | What it holds |
|---|---|
| Uniform in z | An iron block (µr 100) beside a bar of 1e8 A/m² in insulated air, two layers: the 2D model on the 3D model's plan mesh, the energy to 12 digits (2.21967778451e-7 J at order 1, 2.2672255976e-7 J at order 2) and Bx, By and Az at four points to 1e-6; Bz and Ax nought |
| The gauge across layers | A loop of current in the x-z plane, J = ∇ × (0, s, 0) for a bump s of x and z, in a box of four to five layers: ∇·A by central differences 3 to 6 % of the size of its terms at four points (the weak gauge does not make it nought at a point); order 1's energy 10 % under order 2's, as a conforming method's is |
| An infinite solenoid | A shell 1 to 1.1 mm of 1e8 A/m² round z, every face natural: Bz in the bore 12.571 mT against µ0 J t's 12.566 mT (0.04 %), 6e-7 T outside |
| A cylinder of µr 50 in a field | 1 T across it, held at a wall 6 mm out: B inside 1.9098 T at three points against the bounded domain's exact 2 B0 / ((1 + 1/µr) + (a/R)² (1 - 1/µr)) = 1.9098 T, within 0.07 %; By under 1e-3 T |
| A magnetized cylinder | Br 1 T along x: B inside 0.4859 T against Br/2 (1 - (a/R)²) = 0.4861 T (0.04 %); H inside -(Br - B)/µ0 |
| A coaxial line | Its energy's L 0.28234 nH per mm against µ0/2π (ln(b/a) + 1/4 + the shell's term) = 0.28301 nH, 0.24 % (its arcs chords) |
| A coil from currents | A bar's current from a currents physics' terminal, 2 A, in 0.5 T: its Lorentz force 1.0000 mN along y against I l B0, its x 2e-9 N; its J 2 A over the bar's section; two coils' inductance matrix symmetric to 1e-6 and positive definite |
| An iron ring by its B-H curve | Round a wire of 50 A: \|B\| at 0.7, 0.9, 1.1 mm 1.7177, 1.6744, 1.6397 T against the curve's at I/(2πr), 1.7172, 1.6768, 1.6412 T (within 0.15 %), by Newton's method |

**The solenoid at 0.1 mm:** its shell, 0.1 mm thick, is then one element across. The edge elements take its arcs
as chords, and its two polygons left its thickness, and Bz, 0.47 % off. Five meshes from 0.3 to 0.07 mm gave
-0.015 %, +0.06 %, +0.003 %, +0.47 % and +0.03 %. The test meshes it at 0.07 mm.

**A sphere** cannot be made of a swept mesh's layered solids. The plan's sphere in a uniform field is the cylinder
across one here, whose bounded field is exact.

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_magnetic` (new, 8 functions, 12 s) | The checks above, and what is refused: a coil whose currents physics is not solved; Jez a 3D model's alone, Je the plane's |
| `test_multiphysics3d_doc` `aMagneticFieldThroughTheTools` (new) | A bar in air made by fem_model, solved by fem_solve, mf.W and mf.By either side of it by fem_evaluate, Plot offering mf.normB |
| `test_multiphysics3d_doc` `physicsThroughThePanel` (changed) | Magnetic Fields offered on a 3D component |
| `test_fem3_geometry` `schema`, `test_fem3_solve` `refusals` (changed) | A 3D model's physics now include magnetics; the semiconductor is the one refused |

## Broken on purpose

`docs/feature_gaps/2026-11-10-multiphysics-3d-magnetic/breaks.py` holds 12 breaks, each built and its suite run
through ctest:

- the curl-curl tangent the identity's; the gauge's z gradient; the source's z component;
- a magnet's Br added; the B-H curve's tangent its secant alone;
- insulation on inner faces; a potential's projection's sign;
- a coil's scale without its current; the inductance sweep's coils all the first;
- the Lorentz force's y component; the energy density without its half;
- Jez shown in 2D.

All 12 are caught. Two needed a second look, and a thirteenth was dropped:

- **The gauge's z gradient left out** was not caught at first. In every model uniform in z, ∂Az/∂z is nought, so
  a gauge of x and y alone gives the same A. B is the same either way, since B does not depend on the gauge. Two
  checks were added. `uniformInZ` now compares Az with 2D's, over two layers. `aLoopAcrossTheLayers` holds ∇·A
  small in a truly 3D field: 3 to 6 % of its terms against 76 % under the break.
- **A coil's scale without its current** was not caught at first. The Lorentz force multiplies the coil's current
  in by itself, and its own field's force on it is nought. `coilsFromCurrents` now checks its J, N I0 over the
  bar's section.
- **One refinement step's correction thrown away** was dropped from the list, uncaught. The solves it was added
  for were never short of their accuracy: the LDLᵀ's first solve is already at a residual near 1e-14. It is a
  safeguard, not a correction any test needs.

## Not yet

- **Eddy currents** (A-V in the frequency domain), magnetics in time, torque, and the force on iron by Maxwell's
  stress tensor: later, as the plan has them. The Lorentz force is the coils'.
- **A coil's role** (voltage, floating) is the plane's alone. In 3D a coil is fed its current.
- **The sanitizers:** not yet run on this change.
