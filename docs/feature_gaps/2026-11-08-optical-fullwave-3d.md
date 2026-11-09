# Light through a 3D device by the finite element method in full (2026-11-08)

Optical S-parameters of a 3D device - a junction, a bend, a crossing, a Y-branch, an MMI, a grating's step - by solving
Maxwell's equations over the whole geometry at each wavelength: the "3D FEM" of the plan agreed after the eigenmode expansion
(2026-11-08-optical-eme-3d.md), stages F1 to F4. The time domain followed by the finite element method, not FDTD: 2026-11-09-optical-timedomain-3d.md.

## What it is

- **The physics.** *Electromagnetic Waves, Frequency Domain* in a 3D model (type `waves3`, tag `ewfd`;
  `fem3_wave.h`): ∇ × µr⁻¹ ∇ × E − k0² εr E = 0 over the whole geometry, εr = (n − jκ)² of each material at the wavelength
  (its dispersion too), metal on the geometry's outside except where an *Optical Port* is. Its features: *Optical Port*
  (faces of a waveguide's end; its modes, how many of its cross-section's modes it absorbs, fed or not) and *Perfectly Matched
  Layer* (domains; their reflection). Solved by a *Wavelength Domain* step - each wavelength a snapshot.
- **Elements.** Nédélec's of the first kind on the prism mesh, order 1 or 2, as the prism is its plan triangle times a layer:
  E across z the plan's edge functions times Lagrange's polynomials in z (shared by the layers at their heights, and at a
  layer's middle for order 2), E_z the plan's nodal functions times polynomials one order lower (a layer's own). 9 functions a
  prism at order 1, 36 at order 2 - some 15 unknowns a prism.
- **Ports.** A port's faces must lie in one vertical plane on the outside. Its cut is the same as the eigenmode expansion's
  (`cutAcross`, the full-vector mode solver, PEC walls), with as many modes as it absorbs (20 by default). Each mode is a modal
  boundary term u_m(W) = ∮ (ŝ × h_m)·W dS, and the absorbing condition is (K + jωµ0 Σ u_m u_mᵀ) E = −2jωµ0 u_p. That is solved
  by Woodbury's identity with one factorization of K: G = Uᵀ K⁻¹ U, S = (D⁻¹ + G)⁻¹ (G − D⁻¹), D = jωµ0. The modes are
  normalized ∫ e × h · ŝ = 1, as the expansion's, so the S-parameters are comparable. They are signed in one frame: the
  lateral axis toward +y (toward +x for a port facing ±y), so a straight guide's TE and TM modes both reach its far end as
  e^−jβL.
- **PMLs.** Stretched coordinates outward from the box of the non-PML prisms: s = 1 − jσ0ξ², σ0 = 1.5 ln(1/R)/(k d), the 2D
  mode solver's own profile; ε' = εΛ, µ'⁻¹ = Λ⁻¹, Λ = diag(sy sz/sx, sx sz/sy, sx sy/sz). By default a port's faces on a PML are
  left out (metal behind the PML there) and its modes are those of the window inside. With *Ports through the PMLs* they are
  kept, and the cut stretches the PML rectangles as the 3D field's (EmeSettings::pmlDomains) - see below for why that is not
  the default.
- **The solver.** K is complex symmetric, so it is factorized by Accelerate's complex-symmetric LDLᵀ (new:
  `SparseComplexLdlt`; macOS 26 and later, METIS ordering; elsewhere a SparseLu). The port columns are solved sixteen at a time
  (`SparseLu::solveMany`, the LDLᵀ's own).
- **What it gives.** The globals ewfd.S21, S21dB, S21_arg, S21_abs, neff and alpha of each port mode, ewfd.R/T/L (as the
  expansion's: `Solution3::optics`, so the Touchstone export, Plot Spectrum and fem_solve's answer take it as they take the
  expansion's), and the first fed port mode's field as plot variables: ewfd.normE, ewfd.Ex, ewfd.Ey, ewfd.Ez (|E| and E's real
  parts, rms, of 1 W fed - 4.3e7 V/m at a silicon wire's centre), averaged at the mesh's nodes.
- **What it costs, said first.** The study's estimate gives the unknowns and some 20 kB an unknown. A box of 973,000 unknowns
  took 19.8 GB and 68 s to factorize; one of 1.92 million took 43 GB and 249 s; a long guide's (1.5 million) took 15 GB and
  19 s. A study past the memory limit (Multiphysics 3D > Settings) is refused. Once analysed, the factor's size is known
  exactly (Accelerate's symbolic factorization), and a factorization past the limit is refused too.
- **The mesh against the wavelength.** Each domain's longest prism edge is checked against λ0/n/4 at order 2 (λ0/n/10 at order
  1), and a warning names the domain where it is coarser: a junction's |S21|² is 0.8999 with silicon's edges at a fifth of λ0/n,
  0.8995 at an eighth, and 0.914 at a half.

## Validation

| | What is known | What the full-wave solve gives |
|---|---|---|
| A hollow metal guide 0.9 x 0.4 µm, 2 µm, 1550 nm (TE10) | β = sqrt(k0² − (π/a)²): its port's index 0.5084168 | 0.5084167; quadratic, h 0.1 µm: phase error 9.4e-5 rad, \|S11\| 4.8e-5, h 0.05: 6.3e-6 (h⁴); linear: 0.23, 0.063, 0.016 rad at 0.2, 0.1, 0.05 µm (h²); \|S12 − S21\| 1e-14 |
| The same loaded with a core of index 2 (0.3 x 0.2 µm) | S21 = e^−jβL at its port's index | phase error 5.6e-3 rad at 0.1 µm, 1.1e-3 at 0.05 |
| A silicon wire (0.5 x 0.22 µm in oxide), 1 µm, PMLs round it | S21 = e^−jβL, \|S21\| = 1 | \|S21\| 0.99998, phase error 3.3e-4 rad (0.15 µm, the core 0.08); ports through the PMLs: S21 the same to 3.3e-5 |
| A slab's step, silicon 0.22 into 0.5 µm, absorbing oxide beyond 0.5 µm, TE (a slice with metal sides) | The 2D full-wave solver in the plane (x, z): \|S21\|² 0.946712, \|S11\|² 0.00250 | 0.946795, 0.00252 (slice 0.2 µm, silicon 0.06); 0.946943, 0.00245 (0.08, 0.08); the expansion: 0.946382, 0.00249 |
| A wire's junction 0.5 into 1 µm in the expansion's window (1.6 x 1.42 µm, metal) | What the expansion's two projections reach with 200 modes: 0.9006 | 0.89985, 0.89967, 0.89954 at 0.15, 0.12, 0.1 µm (ports 1.5 µm from it); 0.9012, 0.9013 with them 20 µm away; half the geometry (metal at y = 0) 0.89990; reciprocal to 1e-12, every propagating mode's power 1.000000 |
| A sidewall's step 0.49 into 0.51 µm (a grating's tooth), TE0 | The expansion: \|S11\| 0.0030 (0.00296 to 0.00302, converged in its mesh and modes) | 3.16e-3, 3.21e-3, 3.20e-3 at 0.15, 0.1, 0.08 µm; the guide without the step 1.2e-4, 5.4e-5. The expansion converges slowly at a step's corner: its 2D grating's κ was 4 to 8 % short of the full-wave solve's |

## F4: devices against the expansion

Each device is drawn once and solved by both methods on the same box: silicon wires 0.5 x 0.22 µm in oxide, 0.6 µm of it
below and above, metal walls. Where the device is symmetric, only half is drawn (y ≥ 0, metal at y = 0, where TE0's tangential
E vanishes). The full-wave mesh is 0.15 µm in the oxide and 0.08 in the silicon; the expansion uses 40 modes at 15 elements per
wavelength, and cells of 0.1 µm where the cross-section changes. |S|² is of the half's 1 W.

| Device | Expansion | Full-wave | Full-wave's cost |
|---|---|---|---|
| A 90° bend, 2 µm, straights of 1 µm (the whole of it, in PMLs) | −0.0774 dB (its own test: absorbing walls round the path, 60 modes) | −0.0735 dB (\|S21\|² 0.983222), \|S11\|² 3e-7; 0.95 % radiated into the PMLs | 1.37M unknowns, 104 s, 28.6 GB |
| A Y-branch (half: a taper 0.5 to 1 µm over 2 µm, arms from touching to 2 µm apart over 5 µm) | into each arm 0.9007 (cells 0.1 µm, 40 modes), 0.8939 (0.05 µm), 0.8924 (0.05 µm, 80 modes, 20 per wavelength) | 0.8917 (−0.50 dB), \|S11\|² 1.4e-3 (the expansion's 1.1e-3) | 1.03M, 42 s, 20.5 GB |
| A 1x2 MMI 3 µm wide (half), 0.9 x 3L_π/8 | 0.766, 0.775, 0.835, 0.842 with 40, 80, 120, 160 modes; \|S11\|² 0.017 to 0.021 | 0.856, \|S11\|² 0.020 | 1.62M, 77 s, 27.5 GB |
| A crossing at right angles (half: ports at both ends and up the second wire) | 0.805, \|S11\|² 4.0e-3 - but light up the second wire, across its axis, is beyond it | 0.736 (−1.33 dB), \|S11\|² 1.0e-2, up the arm 0.0835 (of the half's 1 W: 4.2 % of the whole's input in each arm); reciprocal to 1e-12, every mode's power 1.000000 | 631k, 36 s, 10.5 GB (a coarser one, 0.25 µm and 0.12 in the silicon: 0.7368 and 0.0835 - the test's) |

Where the expansion applies, it converges toward the full-wave solve, which holds more of the physics: a Y-branch's
staircase as its cells shorten, an MMI as its modes grow. The MMI wants 120 or more of the half's modes (240 of the whole's)
before it comes within 2.5 %. A crossing it cannot do: it cuts along one axis, and what goes up the other wire it carries as
part of that cut's modes, bounded by the window's walls. The full-wave solve shows 8 % of the half's light going there. The
bend agrees to 0.004 dB.

The timings were taken one device after another on an M2 Ultra (24 cores, 64 GB); the memory is the process's peak.

## Ports through the PMLs, and why that is not the default

Ports that keep their PML faces need their cut's modes in the same stretched medium. The cut's PML rectangles are stretched
(EmeSettings::pmlDomains), and its box is that of the non-PML rectangles, as the 3D solve's is. It works - the wire's S21 is the
same to 3.3e-5 either way - but the PMLs' own discrete modes (Re n 2 to 3.5, Im n −0.5 to −1 in a wire's oxide) crowd the mode
search. 19 of the 20 modes found about silicon's index were theirs. Searching about the index midway between the window's
least and largest (now done whenever a cut has full-wave PMLs) and asking again with twice as many (to 200) while too few are
left, a cut at 15 elements per wavelength needed 80 to keep 10: 22 s of a 24 s solve, against 2.6 s for the window inside.
No real search target avoids them, since they lie within some 3 of every real n² in the complex plane. A complex shift above the
real axis would, if the 2D solver took one. So by default the ports leave their PML faces out, and say so in the log.

## The factorization

Accelerate's complex LU (the SparseLu every complex 2D solve uses), with its default ordering, took 66 s and 9.1 GB of process
memory to factorize the wire in PMLs (97,000 unknowns) - 2.4 GB at 41,000, 7 GB at 75,000: some 5,900 factor entries a row.
METIS's ordering (`SparseLu::setMetisOrdering`, new) brought 75,000 unknowns from 20.6 s to 12.3 s and 7.0 to 5.6 GB, but the
wire still took 63 s. Accelerate's complex-symmetric LDLᵀ (`SparseFactorizationLDLT` of a `SparseSymmetric` complex matrix,
macOS 26) factorized it in 1.4 s, its process 1.9 GB, with the S-parameters the same to every digit printed. That made the
F2 to F4 checks possible at all: a 1.5-million-unknown junction in 34 s.

## The tests

`test_fem3_wave` (ten functions, 155 s in Release; most skipped in a sanitized build, where the hollow guide, the PML and the
model's way run coarse): the hollow guide's phase by order; a PML taking a guide's wave (|S11| 9.9e-4, R^(β/k0) = 8.9e-4
predicted); the wire in PMLs, both port set-ups; the slab step against the 2D numbers; the junction and the sidewall step
against the expansion's; the crossing's three ports (reciprocity, power); the model's way (a Wavelength Domain step of two
wavelengths, its globals, |E| at the core, its Touchstone file, a memory limit refusing it, the estimate in the log, the
coarse-mesh warning).

## Breaks the tests catch

| Break | Caught by |
|---|---|
| (a) Ports not turned to the common frame (orientModes left out) | the wire's phase error π; the model's S21 phase |
| (b) The PML's ε not stretched (µ alone) | the PML's \|S11\| 1.43 |
| (c) The balance counting lossless modes alone (the slab's guides' tails in the absorber make every mode lossy) | the slab's balance 0 |

## Not done

- **FDTD (T1 to T3)**: the Gaussian pulse and its spectrum - the plan's second engine - is not begun.
- A complex shift for the 2D mode solver, which would make ports through PMLs cheap.
- Iterative solution of the 3D system (memory is the limit: 64 GB holds some 3 million unknowns of a compact box).
- The wavelengths are solved one after another, each factorization on every core. The LDLᵀ's analysis (the same pattern at
  every wavelength) is made again each time.
- Fields are kept for the first fed port mode only, as |E| and real parts at the nodes (their normal components averaged
  across material faces).
- Not looked at in the running app: the physics in the 3D panel's menu, its features' selection of faces, Plot Spectrum and the
  field's plots were reached through the tests and the model's own code, not by hand.
- fem_solve's description was at its 2,048-character cap: the expansion's part was shortened to make room for the full-wave
  solve's (its cells and its too-few-modes warning are no longer named there; fem_describe's schema still has them).

## Checks run

Through ctest: the whole Release suite (183 of 183), `test_fem3_wave` and `test_fem3_eme` in the Debug ASan + UBSan tree (85 s
and 520 s), and the four lints.

## Since (2026-11-09, with the time domain)

- **Ports with a PML behind them** in the frequency domain too: a port's faces may lie inside the geometry, between the device
  and a PML its waveguide goes on into. Such a port is a source (a sheet of current, its mode both ways) and a monitor (the
  device side's field split into each mode's incoming and outgoing waves, H's flux from the device side's own residual), S = B
  A⁻¹ over the fed port modes - the hollow guide's phase error 9.0e-5 rad, |S11| 1.8e-6. All of a solve's ports one kind or
  the other.
- **One index for every PML's stretching**: the lowest of the PMLs' materials', not each domain's own (σ jumped across a layer
  a core goes on into - a mode grew without end in the time domain). The sidewall step's |S11| 3.231e-3 for 3.259e-3; the other
  tests' numbers as before.
- The setup shared with the time domain (`fem3_wave_setup.h`); `SparseCholesky` gained a block solve and its factors' size.

