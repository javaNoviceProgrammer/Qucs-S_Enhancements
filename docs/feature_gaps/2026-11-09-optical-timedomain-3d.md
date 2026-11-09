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

## The Y-junction check, and what came of it

The first check in the app (`qucs-reviews/Multiphysics3D/FullWave3D-TimeDomain/2026-11-09-claude-timedomain-ybranch-check.md`,
Zhang's Y-junction, half of it: 956,014 unknowns at order 2) found it right at the band's middle (|S21|² within 1.7e-5 of the
frequency domain on the same model and ports), and nine things. What came of each:

| # | Found | Now |
|---|---|---|
| 2 | An error growing linearly from the band's middle, antisymmetric: T 4e-4 off at ±40 nm | **The sources' profiles.** The frequency domain with its sources the band's middle's mode (1540 nm) at 1500 nm gives 0.9525912 against 0.9513165 with its own (TE0 fed): +1.3e-3, the review's sign (its twenty fed modes, B A⁻¹ over them, took some of it back). Not the materials' fit: silica's to 1.5e-6, silicon's to 3.2e-7 in index. Each fed mode's source is now its profile at the band's ends and middle, each sent through a filter whose spectrum is its Lagrange polynomial in σ = sin²(ωΔt/2) (σ ↔ −δ²/4: a symmetric filter of five steps, the pulse's sum and moment kept nought) - the mode's own profile at every frequency to the third order. Against the frequency domain with each wavelength's own sources: 1500 nm −5.1e-6, 1540 −1.9e-6, 1580 −2.9e-6 in \|S21\|²; R within 1e-7. |
| 2 | The help said "ε at the band's middle, its loss a conductivity" | It says the fit (ε∞, a conductivity, a Lorentz pole; its error in the log). |
| 3 | PML-backed ports look the more accurate (0.3 % in T at 1540 nm) | The frequency domain's port help says so: a PML takes what a port on the outside reflects - radiation and the modes past those it absorbs. Not made the default (the ports' kind is where the faces are). |
| 4 | Memory not half: 19 GB against 18 | Each fed port mode is a column of every step's solve, history and PML states: the review fed twenty to have the total reflection. **Rall and Tall** (new globals, both domains): R and T over every mode carrying power at the ports, taken or not - TE0 alone fed gives the total reflection, 0.0037806 at 1500 nm (the review's twenty: 0.003782). TE0 fed: 12.5 GB, 495 s for five wavelengths (the review 19 GB, 902 s). R and T themselves are as they were, over the modes taken: over every mode T would count a port's window's own modes, radiation reaching its plane (0.964 against \|S21\|² 0.951 here). The help says what the memory depends on. |
| 5, 8 | The ports' modes at each wavelength one after another (≈ 19 s each, 15 of 40 minutes for fifty) | Side by side, as many as half the memory (or the limit) holds at some 1.5 GB a cut at resolution 30 - the factors, the history and the PMLs' states let go first: fifty wavelengths' ports 65-72 s, 22 at once. |
| 9 | The whole field transformed at every wavelength at every step | **Field kept at** (new property): five wavelengths evenly by default, the middle, every one, or none; the S-parameters at every one. The monitors' transforms on the steps' pool, a frequency a task. |
| 6 | fem_solve's full-wave ports listed `"at": "start"` | A full wave's port has no axis: its `selection`, as the model gives it. |
| 7 | The run's log not kept where fem_describe reads | fem_describe's `solved` (a 3D model's) and each study (a 2D model's) carry the solve's log, its last 60 lines. |
| - | Run 2: point-to-point noise of ±2e-4 in T above 1545 nm | **The ports' cuts re-meshed at each wavelength** (λ0/n over the resolution): R smooth, T and the balance jumping together. Every wavelength's cuts now meshed at the band's shortest (EmeSettings::meshWavelength; the frequency domain's sweeps alike): the largest second difference of fifty T's 5.7e-4 → 3.5e-5, the curve's own. |

**Fifty wavelengths** (`linspace(1500[nm],1580[nm],50)`, TE0 fed, the field at five): 406-468 s, 17 GB - the review's 2,434 s and
25 GB; five wavelengths 495 s (the steps' time varies with the machine's load: 535 to 750 ms a step, 590 steps). So the run's
cost hardly moves with the wavelengths' number now; the frequency domain's is some 60 s a wavelength here, the break-even near
eight. T peaks at 0.96742 near 1562 nm (48.37 % an arm), flat within 1e-4 from 1555 to 1570 nm.

**Found on the way: the pulse's cut ends.** The sources' filters are polynomials, large far from the band (some 4,000 times
at the steps' Nyquist frequency, 300 at σ = 0). The pulse was cut at e⁻¹⁵ of its peak: that step's broadband 3e-7, so
magnified, set the mesh's fastest waves ringing, and the hollow guide (settled at 1e-8) never settled. Its envelope's ends are
now brought to nought over four periods (sin²), and each filtered signal's sum and moment taken off again: settled in 1,921
steps (1,478 before, a tail at some 3e-8 - only a decay this tight sees it), \|ΔS\| 4-5e-9 at 1500, 1550 and 1600 nm.

**Tests.** test_fem3_wavetd's junction now runs 1500-1600 nm and checks 1500 nm against the frequency domain with its own sources:
\|ΔS\| 4.8e-6; with the band's middle's profile alone (the break) 4.1e-3. Its other comparisons now take the frequency
domain's own sources too, its ports' cuts meshed at 1500 nm as the time domain's (meshed at 1550 nm its own, the hollow
guide's 1550 nm was 1.1e-5 apart: the cut's discretization). The model test reads Rall and Tall. test_fem3_wavetd 8 of 8 in
118 s (the junction 56 s, the most).

**Checks run** (Release, through ctest): test_fem3_wavetd 8 of 8, test_fem3_wave, test_fem3_eme, test_fem3_geometry,
test_qucs_control; the whole suite 185 of 186, test_claude_code's prompt dates failing in the suite's shared HOME and passing
alone in a fresh one; the four lints green. The Y-junction runs by a scratch driver (not kept) calling solveWave3Time and
solveWave3 on the review's model. Not looked at in the running app; the Debug ASan tree not rebuilt for this.

## Not done

- **The pulse in time** is not kept - only the spectra and the transformed field at each wavelength (as many wavelengths as
  2 GB of it hold). An animation of the pulse would want time snapshots.
- **Speed for large devices**: the triangular solves are the limit, and Accelerate's run on few cores. Not tried: a solver of
  more parallel solves, single precision, an iterative solve of the (well-conditioned) step matrix.
- **The sources' profiles** are interpolated between the band's ends and middle (below, "The Y-junction check"): a band wide
  enough for the fed mode to change its shape much between them (its end's mode under 0.9 alike) is warned, under 0.5 refused.
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
