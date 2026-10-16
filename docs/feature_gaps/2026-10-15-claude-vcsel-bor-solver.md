# A full-vector axisymmetric eigenfrequency solver (a VCSEL's modes)

The request (2026-10-15): "a full-vector axisymmetric solver that handles one azimuthal
order at a time, the way Lumerical's VCSEL solver does. It would give the vector
splittings, field profiles directly comparable to Lumerical's, and Q from complex
frequencies without my hand-written absorbing layers." Then, as a second step, coupling it
to the semiconductor and quantum-well physics for threshold and light-current curves.

This is the first step: the solver, checked against what is known and against the user's
VCSEL bench. The coupling is not done.

## What it is

A physics *Electromagnetic Waves, Eigenfrequency* (`"type": "resonator"`, tag `emr`) on an
axisymmetric component (x = r, y = z, the axis x = 0, nothing at x < 0), and a study step
*Eigenfrequency* (`"eigenfrequency"`: `"around"` a free-space wavelength or a frequency,
`"count"` the modes nearest it).

**The fields** are those of one azimuthal order m (0, 1, 2...; -m alike), E(r, z) e^(-jmφ),
full-vector: E_r and E_z on Nédélec's edge elements, E_φ through χ = -j r E_φ on the nodes
(Lagrange, the same order: 1 or 2). With μ and ε diagonal along (r, z, φ):

    K = ∫ [ (m E_r + ∂r χ)² / (μ_z r) + (m E_z + ∂z χ)² / (μ_r r) + r (curl_t E_t)² / μ_φ ] dr dz
    M = ∫ [ ε_r r E_r² + ε_z r E_z² + ε_φ χ² / r ] dr dz

symmetric (complex with an absorbing layer or a lossy material), K x = k0² M x. On the
axis χ = 0 for every m and E_z = 0 for m ≠ 0 - the field single-valued there; nothing else
(no boundary condition). r is the element's own affine map's (exact on straight sides),
so the 1/r of a triangle touching the axis integrates to the end.

**Open cavities:** a pml feature is a cylindrical perfectly matched layer - r stretched as
r̃ = r - jσ0 d ξ³/3 where the layer is along r, the stretch's factors
((r̃/r) s_z/s_r, (r̃/r) s_r/s_z, (r/r̃) s_r s_z) along (r, z, φ) - so a mode leaking out is
taken away and its eigenfrequency is complex: **Q = Re ω / 2 Im ω**, no layers of the
user's own. The modes mostly in the layers (their own) are left out, said in the log; so is
the curl's null space (k0 nought). pec (the outside by default) and pmc too.

**The solve:** shift-and-invert about (2π/λ_around)², a sparse LU of K - σM and Arnoldi's
largest eigenvalues of (K - σM)⁻¹M, k² = σ + 1/θ; the `count` nearest, by frequency.

**Outputs**, each mode a solution: emr.freq, emr.freq_im, emr.lambda0, emr.Q, emr.m; the
fields at φ = 0 emr.Er, Ez, Ephi (each _im too), Hr, Hz, Hphi, normE, normH and the power
flow Sr, Sz - the largest |E| outside the layers 1 V/m. Default plots: the electric field
(|E| and E_r, E_z's arrows) and E_φ. fem_solve lists each mode's lambda0 and Q to seven
figures (a VCSEL's vector modes can be split by thousandths of a nanometre: 7.4 pm on the
user's bench).

## Checked

`test_fem_resonator` (new):

- **A metal cylinder** (radius 10 mm, 20 mm high): for m 0, 1 and 2 the eight modes
  nearest 15 GHz, each TM_mnp and TE_mnp of Bessel's zeros j_mn and j'_mn, none missing,
  none more, within 4.4e-5 (quadratic, hmax 1 mm; worst TE_1 at 26.52 GHz); Q infinite;
  m 0's TE011 E_φ alone, its TM010 none.
- **A dielectric sphere** (n 3.5, radius 1 µm, a 2 µm layer round a 4 µm box): Mie's
  resonances - the poles of b_n (TE) and a_n (TM), worked in the test with the spherical
  Bessel functions in closed form for a complex argument and Newton's steps - TE1 to TE4,
  TM1 to TM3 and the second TE2, k0 R within 8e-4 (the sphere's polygon, as h²; 2.5e-4 at
  half the size) and **Q within 0.2 %** (4.98, 46.99, 26.38, 206.2, 13.53, 161.5, 979.9,
  25.33 against 4.98 ... 980.4); m 2 the same resonances of order 2 and up, none of order 1
  (m ≤ n); a deeper layer, farther out, the same.
- **The axis:** order 1's E_φ = -j E_r there (one turning field; 3 % off at 2 nm from it,
  the mesh), E_z nought, H finite on it.
- Refused, and said: a component not axisymmetric, a geometry reaching x < 0, an
  Eigenfrequency step without a resonator, "around" neither a length nor a frequency.
- **The metal cylinder's magnetic field:** TM010's H_φ / E_z at r = a/2 J1(x) / (η0 J0(x)),
  x = j01 / 2 (0.744871 against 0.744713, the series in the test - libc++ has no
  cyl_bessel_j), H_r nought.
- **The example** (below), each number its description gives and which mode is which; each
  mode by its number; one of the absorbing layers' own modes left out, said.

Seventeen changes broken on purpose; fifteen caught (two after the tests were made to see
them - the absorbing layers' own modes, a stack's layer two rows from the split). Two not,
and why: χ left free at a vertex on the axis (the edges' loop fixes the same nodes where a
domain meets the axis along an edge - the vertices' loop is for a domain touching it at a
point alone), and E_z left free on the axis for m ≠ 0 (the 1/r weight of (m E_z + ∂z χ)²
drives it to nought - the condition natural as well as set).

## The user's VCSEL bench

The geometry of `~/QucsWorkspace/lumerical_vcsel/vcsel_cavity.qfem` (read, not changed): its
89 layers, oxide aperture, mesa and contact, with the resonator physics and absorbing
layers where the user's scalar model had its own. Half its mesh, quadratic: 524 296
unknowns (10 triangles more than before the Layered Region's columns below were made one
set - its stack has split layers), 1 to 3 minutes a run.

Each mode told by its field at r = 1 um in the quantum well (E_φ alone: TE0n; E_r alone:
TM0n; both alike: HE/EH), against the user's scalar LP modes and Lumerical's eigenvalues
(its plots' precision):

| mode            | m | Qucs-S, vector        | the user's scalar (LP)   | Lumerical |
|-----------------|---|-----------------------|--------------------------|-----------|
| HE11            | 1 | 980.9454 nm, Q 1931   | LP01 980.955 nm, Q 1948  | 980.9     |
| TE01 (E_φ)      | 0 | 979.2148 nm, Q 1815   | LP11 979.23 nm, Q 1847   | 979.17    |
| HE21            | 2 | 979.2075 nm, Q 1785   |                          |           |
| TM01 (E_r)      | 0 | 979.2074 nm, Q 1781   |                          |           |
| EH11            | 1 | 976.9715 nm, Q 1727   | LP21 976.98 nm, Q 1854   | 977.0     |
| HE12            | 1 | 976.2503 nm, Q 1663   | LP02 976.26 nm, Q 1834   | 976.2     |

The vector splitting the scalar model cannot give: the LP11 group is three modes, TE01,
HE21 and TM01, 7.4 pm apart here (Lumerical's FI 0 pair "degenerate to the plot's
precision", as the user wrote); their Q 1781 to 1815. The higher modes' Q comes out 7 to 9 %
below the scalar model's - the absorbing layers are the solver's own now, a cylindrical
PML, not the user's; which is nearer is for a finer mesh (or Lumerical's Q) to say. Leaky
modes of low Q (150 to 620) lie near 978 nm in every order (m 0: 978.28 nm Q 616, 977.93
nm Q 226; m 1: 979.09, 978.34, 975.73; m 2: 978.70 nm Q 571, 978.56 nm Q 225) - where
Lumerical lists other eigenvalues (FI 0 978.5, 978.2; FI 1 979.84), which the user's scalar
runs found only as low-Q modes moving with the mesh; not followed further here.

## The example

`examples/multiphysics/vcsel_modes.qfem` - an own design (not the bench's): GaAs and
Al0.9Ga0.1As quarter-wave mirrors (25 pairs below, 20 above), a one-wave Al0.3Ga0.7As
cavity, a 30 nm oxide layer with a 2 µm aperture, absorbing layers along r, below and
above.

Order 1, the three modes nearest 977 nm (127 462 unknowns, 20 s): HE11 978.5249 nm, Q 15 991
(the aperture's confinement 1.5 nm below the mirrors' 980 nm), a ring at 974.2370 nm (EH11's,
with some of HE12's on the axis - not nought there, as a pure EH11 would be) and HE12 at
973.4269 nm (highest on the axis, nought near r = 1 um). Order 0: TM01 976.8342 nm and TE01
976.4568 nm; order 2: HE21 976.6570 nm - the LP11 group split by 0.38 nm by the strong oxide.
Q 16 000: the mirrors lossless, the cavity cold. The test
(`theVcselExample`) checks each of these numbers and which is which by its field.

Mesh: the oxide's corners make the lateral size count - 0.05 um along r instead of 0.1
moves HE11 0.13 nm (978.391 nm), five rows a mirror layer instead of three 0.05 nm, the
cavity's and substrate's rows 0.02 nm. The fields within an element of the axis are its
slope's: E_φ = j χ / r there is first-order - on the axis itself 5 % above E_r at 0.1 um
elements (exact at 0.03 um from it); the eigenvalues are not affected.

## Found on the way

- **A Layered Region's columns** (`fem_mesh.cpp`, layerColumns): each layer divided its
  own width by the lateral size, each boundary its own length; where a layer was split - an
  aperture's edge at 2 um in a 5 um stack - and the size did not divide both parts and the
  whole alike (0.06 um: 34 + 50 columns beside 84), the layers' nodes disagreed, the
  triangulation made slivers, and the solve stopped at a singular matrix ("ZERO COLUMN").
  Now a stack - layered domains sharing a boundary along their layers - has one set of
  columns: its breakpoints (its domains' and its boundaries' ends) and between each two
  ceil(gap / size) alike, in every layer and on every boundary. Stacks of whole layers are
  meshed as before, node for node (test_fem_modulator's numbers unchanged); test
  (layeredStacks): the split stack at 0.06 um and a base two layers from the split, every
  node on the columns, no sliver, 1344 triangles.
- **An array's name selects its copies** (`{"objects": ["bottom_h"]}` for bottom_h_1 ...
  bottom_h_25) when no object has that name: the example's mirrors change their pairs
  without their selections listing each. Test (test_fem_engine).
- **A resonator's modes by number:** the evaluation (fem_evaluate's 'mode', a plot group's
  mode) picked the mode among those at one frequency - a resonator's each at its own, so
  always the last; now the mode asked (Solution::eigenfrequencies).
- fem_solve lists a resonator's lambda0 and Q to seven figures (globalText's digits).

## Not done

- **Coupling to the semiconductor and the quantum wells** (threshold, light-current): the
  next step. The pieces there are: the wells' gain spectrum (Quantum Wells), the
  semiconductor's carriers; what is missing is the modal gain's overlap with the mode and
  the rate equations - or a self-consistent solve with the gain as an imaginary index.
- **Memory:** the LU of the bench's 524 296 unknowns took 14.7 GB (Eigen's SparseLU, its
  COLAMD ordering). AMD on the symmetric pattern was tried and was far worse (a small
  sphere's LU 7 GB, minutes per solve); a symmetric LDLᵀ for complex symmetric matrices
  would halve it.
- Anisotropic materials are taken along r, z, φ (a uniaxial crystal along the axis is
  exact; one tilted is not).
