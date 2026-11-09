# Light through a 3D device in the time domain by the finite element method (2026-11-09)

The time domain of the photonic plan (2026-11-08-optical-eme-3d.md, 2026-11-08-optical-fullwave-3d.md). The plan said FDTD
(T1 to T3: a Yee grid, mode sources and Fourier monitors, Lorentz poles). The user asked "Does it have to be FDTD? what not
time domain wave propagation with FEM?" - it does not, and this is the finite element method stepped in time, on the same
mesh, elements, materials, ports and PMLs as the frequency domain.

## Why the finite element method and not FDTD

- **FDTD**: explicit, no factorization, memory tiny, so it reaches devices past any direct solve; the field's standard (MEEP,
  Lumerical). But a new staircased grid (subpixel smoothing at every sidewall), new PMLs, ports and fields, little of F1-F4
  reused.
- **This**: the frequency domain's own discretization stepped by the trapezoidal rule. Its steps' Fourier transform at ω is
  the frequency domain's solution at ω' = (2/Δt) tan(ωΔt/2) *exactly* - the steps' error is a known shift of frequency, undone
  by sampling the run where ω' is each wavelength asked. So the time domain is checked against the frequency domain to many
  digits, and a step can be large (six a period by default). What it gives up: a factorization, so its memory ceiling is the
  frequency domain's (half of it: the step's matrix is real).

## What it is

- **The physics.** *Electromagnetic Waves, Time Domain* in a 3D model (type `waves3t`, tag `ewtd`; `fem3_wavetd.cpp`), with its
  own *Optical Port* and *Perfectly Matched Layer* features. Solved by a *Wavelength Domain* step: one run for every wavelength
  of the step, each a snapshot with the same globals as the frequency domain's (ewtd.S21, S21dB, S21_arg, neff_…, T, R, L) and
  the first fed port mode's field (ewtd.normE, Ex, Ey, Ez, of 1 W fed). Touchstone export and Plot Spectrum take it as they
  take the others. Properties: order, ports' elements per wavelength, steps a period (6), settled at (1e-5), most steps.
- **The stepping.** The frequency domain's matrix A(p) = ∫ µ⁻¹ Λ⁻¹ ∇×W·∇×V + p² ε Λ W·V (p = jk0 there; the PMLs' s = 1 + σ/p
  with σ the same at every frequency - the frequency domain's s = 1 − jσ0ξ² rewritten). p is replaced by p0 (1 − q)/(1 + q),
  q a step's delay, p0 = 2/(cΔt), and the whole equation multiplied by (1 + q)². The polynomial parts (the bulk's curl and its
  materials' p² ε) are Newmark's average acceleration - two history matrices H1, H2 of E one and two steps back. The rational
  parts - a PML's Λ and Λ⁻¹ at each quadrature point, a dispersive material's pole - are recursions of their own (transposed
  direct form), at each PML quadrature point (their plan functions cached by triangle) and, for a dispersive material's bulk,
  as vectors of its mass matrix. Each step solves A(p0): the frequency domain's matrix at a *real* p - symmetric, positive
  definite - by one Cholesky factorization (Accelerate's, METIS's order) for the whole run, all fed port modes as columns of
  one block solve.
- **Ports.** A port must have a PML behind it: its waveguide goes on into a PML, the port on the face between. Each fed port
  mode is a sheet of current J = ŝ × h_m on its faces (its profile the band's middle's mode), with a Gaussian pulse in time; it
  sends its mode both ways and the PML takes what goes back. The monitor is every port's field on the device's side,
  transformed at each frequency asked, split into each mode's incoming and outgoing waves: a = ½(P + Q), b = ½(P − Q), P = ∮ (E
  × h)·ŝ, Q = ∮ (e × H)·ŝ with the modes' own normalization ∮ (e × h)·ŝ = 1. P is the frequency domain's port column. Q is the
  device side's own equation's residual against e projected on the port's face (H's flux - not ∇×E at the face, which gave a
  phase error of 1e-3 and |S11| 6e-4 against 9e-5 and 2e-6 this way). S = B A⁻¹ over the fed port modes. The frequency domain
  takes such ports too (all of a solve's ports one kind or the other), so the same model runs in both.
- **The pulse.** A sine at the run's band's middle under a Gaussian, its spectrum a tenth of its peak at the band's edges (the
  band 10 % of its middle at least), from e⁻¹⁵ to e⁻¹⁵. The S-parameters are ratios of transforms, its spectrum's shape cancels.
  The run stops when the ports' fields have fallen to 1e-5 of their peak over two periods (or at most steps, warned).
- **Materials.** Each material's ε is fitted over the frequencies asked: ε∞ and a conductivity where they fit to 2e-6 in the
  index, else with the one Lorentz pole (its frequency and damping on a grid, outside the band; ε∞, Δε and σ ≥ 0 by least
  squares) that fits best. Since the transforms are exactly the frequency domain's solution of the fitted medium at each
  wavelength asked, the fit's error there is all the dispersion's error; it is said in the log, and warned past 1e-4.

## What it was checked against

| Check | Reference | Result |
|---|---|---|
| Hollow metal guide, PMLs behind its ports, frequency domain (order 2, h 0.1 µm) | e^−jβL, nothing back | phase error 9.0e-5 rad, \|S11\| 1.8e-6, \|S21\| 1.00003, balance 1.00006 (the absorbing port: 9.4e-5, 4.8e-5) |
| Hollow guide, time domain against the frequency domain (same mesh, ports, sources), 1500/1550/1600 nm | the frequency domain | \|ΔS\| ≤ 1.7e-9 settled at 1e-8 (1,478 steps); ≤ 1.8e-6 at the default 1e-5 (878 steps) |
| Silicon wire in PMLs (0.6 µm, 108,674 unknowns, 69 % of its prisms in PMLs) | the frequency domain at 1550 nm; e^−jβL | \|ΔS\| 5e-8; phase error 2.7e-4 to 7.9e-4 rad (a coarse mesh, warned); balance within 2.4e-4 |
| test_fem3_wave's junction, 0.5 into 1 µm (coarser: 0.2/0.1 µm, 96,640 unknowns) | the frequency domain 0.8999 (0.15/0.08 µm), the expansion 0.9006 | \|S21\|² 0.900085, \|S11\|² 4.9e-5, \|S22\|² 3.88e-3 (the frequency domain's 6.0e-5, 3.83e-3), \|S21 − S12\| 2.3e-4, balance within 1e-3 |
| Silica's Sellmeier filling the guide, 1500-1600 nm | the frequency domain with the material's own ε at each wavelength | fit: ε∞ 2.11201 and a pole at 46.45 µm, its index within 3e-6; \|ΔS\| ≤ 3.1e-5 |
| A lossy medium (n 1.444, κ 1e-3 at every wavelength) | the same | fit: a pole and a conductivity, within 2.2e-6; \|ΔS\| ≤ 2.2e-5 |
| The model's way: a Wavelength Domain step, order 2, h 0.12 µm | e^−jβL | phase error 1.9e-4 and 1.7e-4 rad; the port mode's index to 1e-6; ewtd.normE 6e7 V/m of 1 W; Touchstone; a port on the outside refused, the memory limit kept |

**What a step costs.** Almost all of it is the solve with the factors - Accelerate's supernodal triangular solves, which ran
on about two of the 24 cores (2.2 cores of CPU): 59,000 unknowns 7.2 ms for one column, 9.4 for two, 16.7 for eight (Eigen's
simplicial LLᵀ 17.6 ms a column); 108,674 unknowns 46 ms for two; 346,146 (a factor of 1.9 GB) 237 ms for two. The PMLs'
points, the bulk's history and the transforms are 5 to 25 % (a persistent pool of threads; the solve's scratch kept between
steps changed nothing). A run is some 250 to 1,500 steps - the pulse some 35 periods, then what rings: a metal window's modes
near their cutoff ring long (the junction's 1,423 steps, 56 s). So the time domain pays where a sweep has many wavelengths:
one frequency-domain wavelength of the same mesh is one complex factorization; a run is one real one (4x fewer flops, half
the memory) and its steps. For devices past a factorization's memory FDTD stays the tool, and is not built.

**Found on the way.**
- *A PML that grew without end.* σ was scaled by each PML domain's own index, so where a core goes on into an end PML σ jumped
  across the layer - not a stretching of a coordinate any more. In the frequency domain that is a small reflection; in the time
  domain, in a dielectric-loaded metal box, a mode grew 0.2 % a step from some 1,000 steps on (the junction never settled). Now
  every PML's σ is of one index, the lowest of the PMLs' materials (a cladding's: the frequency domain's PMLs as before where
  they are of one material; test_fem3_wave's sidewall step 3.231e-3 for 3.259e-3, its PML holding silicon too).
- *A field at rest.* The curl's null space (static fields) integrates the source twice; the pulse's samples' sum and first
  moment are now taken off (its envelope's shape and its moment's), and the field falls to nothing after it.
- *The ports' H.* ∇×E at the port's face gave a phase error of 1e-3 and |S11| 6e-4 on the hollow guide; the device side's
  residual gives 9e-5 and 2e-6.

## Tests

- New `tests/test_fem3_wavetd.cpp` (seven functions, some 2 minutes in Release): a port with a PML behind it in the frequency
  domain; the steps the frequency domain (hollow guide, to 1e-7); a wire in PMLs; the model's own way; a dispersive and a lossy
  material fitted; the junction. The wire and the junction skip under ASan.
- **Deliberate breaks**, each caught: a dispersive material's bulk without its own recursion (|ΔS| ≈ 1); the PMLs' points
  without their recursions (the hollow guide 6e-7 against 1e-7 - such a PML, damped hard, is nearly as absorbing -, the wire
  3.1e-4 against 1e-6).
- test_fem3_geometry (the physics lists), test_fem3_wave (the refactor: every number as before but the sidewall's, above),
  test_qucs_control (fem_solve's description, shortened to fit its 2,048 characters).

## Not done

- **The pulse in time** is not kept - only the spectra and the transformed field at each wavelength (as many wavelengths as
  2 GB of it hold). An animation of the pulse would want time snapshots.
- **Speed for large devices**: the triangular solves are the limit, and Accelerate's run on few cores. Not tried: a solver of
  more parallel solves, single precision, an iterative solve of the (well-conditioned) step matrix.
- **The sources' profiles** are the band's middle's modes; at the band's edges the profile's mismatch sends a little into other
  modes, untaken ones not corrected (B A⁻¹ is over the taken modes).
- **A 3D model's physics are made of order 1** (as the frequency domain's): the time domain at linear order is as inaccurate as
  the frequency domain there; the model test sets 2.
- **FDTD** (the plan's T1-T3): not built.
- Not looked at in the running app; Linux and Windows not run.

## Checks run

- Release, through ctest: test_fem3_wavetd 7 of 7 (114 s), test_fem3_wave 10 of 10, test_fem3_geometry, test_qucs_control;
  the whole suite 181 of 184 side by side with other sessions' runs - test_large_schematics and test_qucs_control slow under
  that load, test_build_commit a stale stamp - each passing on its own after.
- Debug ASan + UBSan, through ctest: test_fem3_wavetd (6 passed, the wire and the junction skipped; 489 s) and test_fem3_wave.
- The four lints (check-third-party, check-tool-arguments, check-shaders, check-json-nesting): green.
