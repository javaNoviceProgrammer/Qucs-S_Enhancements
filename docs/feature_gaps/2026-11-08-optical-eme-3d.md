# Optical S-parameters in Multiphysics 3D: eigenmode expansion through a device's waveguides

*8 November 2026, on `worktree-photonic-sparams`, cut from `b18627cd`. Apple M2 Ultra (24 cores, 64 GB), macOS 27.2, Qt 6.12.0 from
Homebrew. None of it has run on Linux, Windows or CI's Qt 6.10.3 (see "What could not be verified").*

The request: "Now let's focus on adding more physics to the Multiphysics 3D. The most important for me at the moment is optical
mode propagation in integrated photonic devices such as waveguides, bends, MMIs, etc., where the transmissions and reflections
(optical s-parameters) are calculated at a given wavelength. Then I want to be able to sweep the wavelength and find the full
spectrum of transmission and reflections."

The plan agreed with the user: eigenmode expansion (EME) first, accuracy before speed, silicon and silicon nitride alike, every
family of device in turn - straight guides, tapers, MMIs and couplers first, bends, S-bends and rings next (bend modes), gratings
by a period's S-matrix repeated, crossings and Y-branches approximately in EME and properly later in a 3D solver. The methods
were weighed first (the table at the end).

**Where it stands:** the first three stages are here - optical ports and their modes, the S-parameters over wavelengths with
spectra and Touchstone files (straight guides, tapers, MMIs, couplers), and bends along a path with absorbing walls. Gratings,
crossings and Y-branches are the stages still to come.

## What you can do now

In a 3D model (`.qfem3d`) - its waveguides drawn as boxes or extrusions, or a GDS layout's layers in a Layer Stack, its materials
from the library (silicon, silica, silicon nitride: their indices follow the wavelength) or given:

- Add **Electromagnetic Waves, Eigenmode Expansion** under the component. Its light runs along **+x**, **+y**, or a **path**:
  its points, from its start to its end, each corner rounded by an arc of the **Bend radius** tangent to its two pieces (a 90°
  bend, an S-bend, a ring's arc). Along +x or +y the device runs from where the geometry begins to where it ends (or **Device
  from**/**to**).
- Each cut across it takes the geometry's whole cross-section, or a **box** of it (across the axis, and in z; across a path, from
  its centre line, positive to its left). Its **walls** are metal (default), magnetic, or **absorbing**: a layer outside the
  window, of the material at its edge, its extinction rising in steps as the square of the depth to κ at its back (1 µm, κ 1,
  four steps by default), metal behind it - what a junction radiates dies out there instead of coming back off a wall.
- Add **Optical Port** features: at the **start** or the **end** of the axis, over the whole cross-section or, two or more at an
  end, a **box** round each one's waveguide (an MMI's outputs, a coupler's arms). Each takes the modes listed: **TE0, TM0, TE1**
  (TE where most of the transverse field lies across the axis in the plane) or by index (1, 2). Each port mode is one
  S-parameter port.
- Add a **Wavelength Domain** step to the study: `1550[nm]`, or `linspace(1500[nm], 1600[nm], 101)`. **Compute**.
- Each wavelength is a solution, as an output time is: the tab's **Wavelength** box chooses it. The messages show each port
  mode's index and, row by row, |S|² in dB of each fed port mode.
- Every value is a global at each wavelength: `eme.S21` (real), `eme.S21_im`, `eme.S21dB` (10 log |S|²), `eme.S21_abs`,
  `eme.S21_arg` (degrees), `eme.neff_1_TE0`, `eme.alpha_1_TE0`, `eme.TEfraction_1_TE0`, `eme.T`, `eme.R`, `eme.L` (the first fed
  port mode's transmittance, reflectance and what is lost), `eme.lambda0`. A Derived Values node's Global Evaluation lists them
  over the wavelengths.
- **Results > Export > Touchstone File** writes the whole S-matrix at every wavelength as an `.sNp` (Touchstone 1.1, `# Hz S RI R
  50`, frequencies rising, a comment line naming each port: "Port 2: optical port 2's TE0, at the end of the device"), which a
  schematic's S-parameter file part reads.
- Solved over two wavelengths or more, the **spectra** are written beside the model as a Qucs dataset (`model_spectrum.dat`:
  every S-parameter's |S|² in dB and phase, each port mode's index, against `lambda`); the physics node's **Plot Spectrum**
  shows the first fed port mode's |S|² in dB against the wavelength in a display (`model_spectrum.dpl`), where any other of
  them can be added.
- A 2D model's **Mode Analysis** has a **Bend radius** too (and the bend's axis): a bend's modes, their index the arc's, their
  loss its radiation.
- Claude does the same with the `fem_*` tools: `fem_model` makes the model, `fem_edit` adds the physics, ports and step,
  `fem_solve` solves it and answers with an **`optics`** section - the S-parameter ports, and at each wavelength (21 at most,
  evenly) every fed column's |S|² in dB and phase and what leaves by the ports - `fem_evaluate` reads any global at a wavelength
  (`"frequency": "1550[nm]"`) or at all of them (`"all": true`), `fem_plot` with `"spectrum": true` shows the spectrum, and
  `fem_export` writes the Touchstone file.

## The method

**Cuts.** A 3D model's geometry is layered: every solid is a region of the plan from a bottom z up through a height. So every
vertical cut through it is a set of rectangles - which the 2D mode solver meshes as it is. `cutAcross` intersects the plan's
segments (lines, arcs of circles and ellipses, polylines, exactly) with the cut's line, classifies each interval by the plan's
domain at its middle, and stacks the slabs.

**Cells.** The axis is divided where the cut can change: at the plan's vertices inside the window, where its boundaries cross
the window's sides, and at an arc's turning points. A piece whose cut is the same at its two ends and its middle is one cell; one
that changes all along (a taper, a curve) is cut into cells no longer than **Longest cell where the cut changes** (0.5 µm by
default), each its middle's cut - a staircase. Cells of the same cut one after another are one; the same cut found twice is
solved once (a grating's period, a symmetric device's two ends).

**Modes.** Each distinct cut becomes a 2D model of its rectangles with the 3D domains' materials (copied, the model's
definitions with them), meshed to **Elements per wavelength** in each material (λ0/15n by default, four across each layer at
least) and solved by the 2D full-vector mode solver (Nédélec's edges for E_t, Lagrange's nodes for E_s, order 2) about the
largest index. The modes are made **forward** (Im β ≤ 0; a lossless mode's imaginary part, the eigensolver's rounding of either
sign, taken for none - see the first fault below), normalized to ∫(e × h)·ŝ = 1 without a conjugate - the bi-orthogonality mode
matching stands on (a degenerate pair made orthogonal first) - and each port mode's sign fixed by its main field's integral over
the window (by its first moment when that is nought), so the same mode at both ends of a guide reads alike. A mode whose
self-overlap is nought or not finite is dropped. The cuts are solved side by side on the machine's cores, and **kept**: a cut's
modes depend on its 2D model and the parameters that model reads, so a length swept through a Parametric Sweep solves each cut
once.

**Junctions.** Between two cuts, the overlaps O_ab(m, n) = ⟨e_m of a, h_n of b⟩ and O_ba are integrated **exactly on the two
meshes' common refinement**: each triangle of one cut clipped by every triangle of the other it overlaps, a degree-5 rule on
each piece. The interface's S-matrix from E's continuity projected on the right's h and H's on the left's e is
R = (I + OOᵀ)⁻¹(I - OOᵀ), T = 2Oᵀ(I + OOᵀ)⁻¹ - symmetric, so reciprocal, whatever the modes kept. The other projection (E on the
left's h, H on the right's e) gives another. **Both converge to the interface's S-matrix as the modes kept do, but slowly where a
cut has corners**, a wire's; **their mean converges fast**, and is what is used:

| a 0.5 µm into 1 µm silicon wire, TE0's \|T\|² (window 1.6 x 1.42 µm) | 40 modes | 100 modes | 200 modes |
|---|---|---|---|
| E on the right's h, H on the left's e | 0.9119 | 0.9062 | 0.9004 |
| E on the left's h, H on the right's e | 0.8924 | 0.8953 | 0.9007 |
| their mean | 0.9021 | 0.9008 | 0.9006 |

With every propagating mode of the window kept, either projection carries the power out exactly (1.000000 of 1). In a slab
(the cut's corners gone) the two agree within 0.15 % from 20 modes.

**Cascade.** Each cell carries its modes along as e^-jβL; the cells and junctions are cascaded by Redheffer's star product -
stable with evanescent modes, as a transfer matrix is not.

**Ports.** A port over the whole window takes the end cut's own modes. A port in a box has its own cut (the box, metal round it),
and its modes are projections of the end cut's: what of each end mode an outgoing field leaves in the port's mode, and what of
each end mode the port's mode feeds - ½(⟨e_p, h_n⟩ + ⟨e_n, h_p⟩), the same both ways, so S between the ports stays reciprocal.
The end cut's other modes leave as through a semi-infinite guide: nothing reflects from a port but what reflects from the
device.

**Bends.** Along a path the axis is its centre line: straight pieces, and arcs of the bend radius tangent to them. On an arc the
cut is along the radius, and its modes are the bend's. In the bend's own frame - u across (positive to the left), z up, s = Rφ
along the arc - Maxwell's equations are a straight guide's whose media are ε′ = f ε c c and µ′ = f µ c c, c = (1, 1, 1/f),
f = ρ/R the distance from the bend's axis over the radius: the exact transformation, no conformal approximation. The 2D mode
solver takes these media (its new **Bend radius** option, `fem_waves.cpp`) and gives β per length of the centre line, its
imaginary part the bend's radiation into the walls. u and z are lengths in both frames, so a bend's transverse fields are the
physical ones and its junction with a straight piece is the same mode matching; the overlaps evaluate h of a bend's mode with
its µ′. Where the geometry is concentric with the arc its cut is the same all along it: one cell for the arc, its modes carried
as e^-jβRθ. The window and its walls' layer must stay within the radius on the bend's inner side (f > 0): refused and said
otherwise.

**Absorbing walls.** The window's sides or floors, chosen absorbing, get layers outside: the edge rectangle's material extended
by the layer's thickness in steps, the i-th of N with κ_max((i + ½)/N)², corners with the larger of the two, metal behind. They
are passive, isotropic media like any other, so the modes and their bi-orthogonality are unchanged in kind: what a junction
radiates into the window's continuum is carried by modes that die in the layers, and the expansion keeps reciprocity and
loses power, never gains it (every run below: power out ≤ power in).

## Validation

Each against something known independently of the expansion, by `test_fem3_eme` unless said.

| | What is known | What the expansion gives |
|---|---|---|
| A silicon wire's modes (0.5 x 0.22 µm in oxide, 1550 nm) | The 2D solver's own on the cut, h 0.02 µm: 2.445409 | TE0 2.445427, TM0 1.770397; their overlaps the identity to 6.9e-10 |
| A silicon slab's mode, 0.22 µm in oxide, TE (the slab step's input) | Its transcendental equation: 2.8477822 | 2.8477783 (absorbing oxide 0.39 µm from the core, as either layer or wall) |
| Its group index, the library's dispersive silicon and silica | ~4.2 (published, e.g. Chrostowski and Hochberg, *Silicon Photonics Design*) | n_eff 2.44501, n_g 4.1954 (a central difference of 1540, 1550, 1560 nm; not a test) |
| A straight wire, 10 µm | S21 = e^-jβL, \|S21\| = 1, S11 = 0 | to 1e-9, S11 0 |
| A slab's step, silicon 0.22 into 0.5 µm, absorbing oxide beyond 0.5 µm, TE | The 2D full-wave solver in the plane (x, z), ports 9 µm either side: \|S21\|² 0.946712, \|S11\|² 0.00250 | 0.946382, 0.00249; the same with the absorber as the cut's absorbing walls: 0.946382, 0.00249 |
| A junction of 0.5 and 1 µm wires | Reciprocity, power | S21 - S12 1e-16; power over every propagating mode 1.000000; the same with twice the modes within 0.4 % |
| The same junction, 40 modes, window 1.6 x 1.42 µm | What both projections reach with 200 modes: 0.9006 | each alone 0.91189 and 0.89239, their mean 0.90211 (within 0.17 %) |
| A directional coupler, wires 0.2 µm apart | Coupled-mode theory's supermodes: even 2.456880, odd 2.436351, L_c = λ/2Δn = 37.7516 µm; sin²(πL/2L_c) | into guide 2: 0.99941 at L_c, 0.54937 at L_c/2 (0.5), 0.96596 at 0.9 L_c (0.9755), 0.97405 at 1.1 L_c, 0.91893 at 1.2 L_c (0.9045), 0.49619 at 1.5 L_c (0.5); back ≤ 9e-3 |
| A 1x2 MMI, 3 µm wide, its outputs at ±W/4 | Soldano and Pennings: two images at 3L_π/8, L_π = π/(β0 - β1) = 22.183 µm (its own modes): 8.319 µm; the split even by symmetry | even to 1e-5; most light out a little short of 3L_π/8 - see below |
| A 1x2 MMI uniform in z (2D, E along z, core index 2, 3 µm wide) | Mode matching with every mode of a finite-difference box, no truncation (written apart, in Python; the appendix): light out 0.88043 at 7.185 µm, 0.77602 at 6 µm; back 0.00303, 0.00291 | 0.88091, 0.77571 (40 modes); back 0.00306, 0.00294 - see below |
| A bend's mode: the wire at 2 µm radius | The ring's whispering-gallery mode of order 19 by the axisymmetric eigenfrequency solver: f 188.153116 THz, Q 3.998e5, so Re β R = 19 and Im n_eff = n_g/2Q = 5.102e-6 | the 2D solver's bend at f: Re n_eff k0 R = 18.99996, Im n_eff 5.016e-6 (1.7 % low) |
| A 90° bend uniform in z (2D, E along z), absorbing walls | The 2D full-wave solver in the plan, the same strips round the path, ports at both ends | silicon core 0.2 µm wide: \|S21\|² 0.994434 against 0.994564 at 2 µm, 0.991140 against 0.991336 at 1.5 µm, 0.999350 against 0.999378 at 5 µm; core index 2, 0.5 µm wide (radiating): 0.910109 against 0.911348 at 5 µm, 0.986313 against 0.986529 at 10 µm - see below |
| A 90° bend of the silicon wire along a path, absorbing walls | Reciprocity; nothing back; the loss rising as the radius falls | S21 - S12 under 1e-9 (2e-16 in a run); back under 1e-7; -0.0113, -0.0774, -0.1642 dB at 5, 2, 1.5 µm with 60 modes (40: -0.0112, -0.0763, -0.1639; 80 at 1.5 µm: -0.1644) |
| Port modes over wavelengths | The oxide's Sellmeier (Malitson) in the model | TE0 2.490331 at 1500 nm, 2.385413 at 1600 nm; S31 e^-jβL at each |
| The model and the tools | - | `test_multiphysics3d_doc`: a wire solved through `fem_model`, `fem_solve` (its `optics`), `fem_evaluate` at a wavelength and at all, `fem_export`'s `.s2p` of three frequencies rising, its ports named |

**The slab's step and the 2D solver.** The 2D solver's ports take their first mode alone; what the step radiates reflects from
them back into the step unless it is absorbed first. With the oxide beyond |z| = 0.7 µm absorbing at κ 0.1 its S21 swung with
the distance to its ports - 0.967, 0.961, 0.946, 0.944, 0.959 at 2, 3, 4, 6, 9 µm, round the expansion's 0.9524 (each of its
projections within 0.0007 of that) - and with κ 0.3 beyond 0.5 µm it settles: 0.955, 0.943, 0.945, 0.948, 0.947. The test
takes 9 µm.

**Bends against two references.** The bend's mode is checked against a ring: the axisymmetric solver finds the ring's resonance
of order m and its Q with nothing of the bend's transformation in it; the bend's mode at that frequency must have β R = m and
lose Im n_eff = n_g/2Q. Both hold (the index to 4e-5 in m, the radiation within 2 %, the rest the two meshes' PMLs). The bend in
a device - straight into arc into straight, its junctions' mismatch, its radiation, its walls - is checked against the 2D
full-wave solver on the same structure made uniform in z: the 3D model's core extruded through the window's whole height between
metal floors, so its cuts' guided modes have E along z alone and the problem is the plan's. The 2D model draws the same strips
round the path (the core, 1.5 µm of clear oxide outside it, 0.8 µm inside, 1 µm of absorber in four steps beyond each, the arcs
as polygons of 0.01 µm chords) with ports at both ends. They agree to 2e-4 in |S21|² where the loss is the junctions' mismatch
(silicon, 1.5 to 5 µm), and to 1.3e-3 where it is radiation (index 2, 5 µm: -0.41 dB) - where the 2D solve's ports reflect
some of the radiation that reaches them (its |S11|² 7.7e-4, the expansion's 8e-9). Neither solve moves with a finer mesh (2D h
0.06 to 0.04 µm: 0.994564 to 0.994564, 0.911348 to 0.911345) or more modes (30 to 60: 0.994434 to 0.994460, 0.910109 to
0.910184). The guide must carry one mode: a 0.5 µm silicon core carries three in 2D, and the 2D solve's single-mode ports
reflect the second, which the bend converts light into - |S11|² 0.26 at 2 µm, nothing to compare with.

**The MMI.** The split is even and the light out peaks a little short of 3L_π/8 - the imaging length of a high-contrast MMI is,
its higher modes' β not quite quadratic in their order. But **how much light** depends on the modes kept and the window,
because what the input junction radiates comes back off the window's metal walls:

| out (\|S21\|² + \|S31\|²) at L / (3L_π/8) | 0.70 | 0.75 | 0.80 | 0.85 | 0.90 | 0.95 | 1.00 | 1.05 | 1.10 |
|---|---|---|---|---|---|---|---|---|---|
| window 4.5 µm, metal, 40 modes (fewer than its ~70 propagating: warned) | | | | 0.767 | 0.811 | 0.837 | 0.763 | 0.586 | 0.411 |
| window 4.5 µm, metal, 90 modes | | | | | 0.825 | 0.788 | 0.786 | 0.662 | 0.416 |
| window 6 µm (1.5 µm beside the MMI), metal, 130 modes | 0.243 | 0.316 | 0.621 | 0.742 | 0.835 | 0.795 | 0.750 | 0.644 | 0.485 |
| window 4.5 µm, absorbing walls (1 µm, κ 1 in four steps), 80 modes | | | | 0.772 | 0.812 | 0.833 | 0.751 | | |

In every window the light out peaks between 0.90 and 0.95 x 3L_π/8 at 0.83 to 0.84 of it - an excess loss of some 0.8 dB, split
50/50 to 1e-5. Its own 3L_π/8 is the paraxial estimate; the peak 5 to 10 % short of it is the higher modes' β falling faster than
quadratically, as in a high-contrast MMI it does. Away from the peak the walls move the answer by up to 0.07 with metal and 0.04
with absorbing walls against the widest metal-walled window. The test checks what holds in all of them - the even split,
reciprocity, little back, 0.75 to 0.9 out at 0.9 x 3L_π/8, 0.15 less at 1.1 x - with 40 modes.

**The MMI against mode matching with every mode.** That the expansion converges to the right MMI is checked where a reference
without truncation can be had: the MMI made uniform in z (as the bend above) is a 2D problem whose cross-section is a line, and
on a line of finite differences (h 0.005 µm, 1,600 points, metal at the box's edges, the same absorber) mode matching can keep
**every** mode of the box - exact for that discretization, the error the grid's alone (h 0.01 µm gave 0.879323, 0.005 µm
0.880152, extrapolated 0.88043). Its light out - the power in the output section's guided supermodes - against the expansion's
with 40 modes: 0.88043 and 0.88091 at 3L_π/8 (7.185 µm), 0.77602 and 0.77571 at 6 µm, back 0.00303 and 0.00306: within 0.05 %.
The expansion moves 1e-4 from 40 to 120 modes. (The 2D full-wave solver is no reference here: an MMI sends a tenth of its light
or more off its outputs, the solver's single-mode ports reflect it, and with the same box ports its light out read 0.9067
against the expansion's 0.8655 at 7.185 µm and 0.7213 against 0.7400 at 6 µm, its |S11|² 0.034 there.) Its ports in boxes
read 0.8655 where the supermodes read 0.8809: a box's metal wall 0.5 µm from a weakly guided output (index 2) cuts its mode -
a box's walls belong where its guide's mode has died away.

## Breaks the tests catch

Each put in, the tests run, taken out:

| Break | Caught by |
|---|---|
| (a) a lossless mode's 1e-16 imaginary part taken at its word (the first fault below, put back) | the junction's power (out more than in), the wavelength step (the index rising with the wavelength), the coupler (no TE0 found) |
| (b) the junction's mean replaced by one projection | the mean's convergence (0.9119 for 0.9006), the slab's step against the 2D solve (0.2 % off), the MMI (back 0.05 or more) |
| (c) the cut found in no solid at a rectangle's middle (the second fault, put back) | the cut of a layered geometry (cuts through 36 cores' middles: "outside the solids"), the coupler (a fatal error) |
| (d) the bend's transformation left out of the mode solver | the ring (m 18.930 for 19, no radiation), the bend along a path (back 9.9e-6), the bend against the 2D solve (0.997318 for 0.994564) |
| (e) a bend's µ′ left out of the overlaps' h | the bend along a path (back 9.2e-5), the bend against the 2D solve (0.989341 for 0.994564) |
| (f) the absorbing walls' extinction nought | the slab's step (its absorbing walls 0.4 % off its absorbing layers), the MMI against mode matching (0.88539 for 0.88043), the bend against the 2D solve (0.07 % off) |

## Tests run

- Release: `test_fem3_eme`, 15 tests, 93 s alone; the whole suite 182 of 182 (556 s, six at once).
- Debug with ASan and UBSan: `test_fem3_eme` (the comparisons that take minutes there skipped; the cuts, the modes, the
  junction, the wavelength step and a 6-mode bend along a path run; 430 s of its 900 with the machine shared at a load of 13 to
  17, the junction the longest at 163 s of a function's 300 - trimmed since to a smaller window, 105 s at a load of 189),
  `test_fem3_geometry`, `test_fem3_solve`, `test_fem_engine` (the shared `domainAt`), `test_fem_waves` (the mode solver's bend),
  `test_multiphysics3d_doc` (218 s of its 300, run beside other suites) and `test_multiphysics_doc` (the 2D Mode Analysis's new
  bend radius; 886 s of its 1800) pass.
- The four lints in `scripts/ci/` (third-party, tool arguments, JSON nesting, shaders) are green.

## What the expansion warns about

A cut whose modes kept are **all propagating** cannot carry what a junction scatters into the window's other propagating modes:
that light goes back into the modes kept and the S-parameters come out too large (a wire's junction in a 4 x 3.2 µm window,
some 70 propagating modes: TE0 |T|² 0.918 with 20 modes, 0.902 with 40). The solve says so, with the window's count by Weyl's law
(k0² Σ A n²/2π over its rectangles): "the cut at s = 5.243 keeps 40 modes, all of them propagating ... ask for more modes (its
window has some 80 propagating), or a smaller window". It is in the log, the tab's messages and the tools' warnings.

## Four faults found on the way

1. **Half the propagating modes went backward.** A lossless mode's β comes from the eigensolver with an imaginary part of 1e-16,
   of either sign; "forward" was Im β < 0, so half the propagating modes were turned round, and a junction gave 4 % more power out
   than in. An imaginary part under 1e-10 of β is none now.
2. **The middle of a long rectangle was in no domain.** `Topology::domainAt` (2D, shared) tests a point against the triangles a
   domain is drawn and picked with; the middle of a thin rectangle lies on the diagonal two of them share, and rounding made each
   say "outside" by 1e-17: a cut at a cell's middle found "outside the solids" (a third of 180 wires cut through their cores'
   middles). A point within 1e-12 of a triangle's twice-area of its edges is in it now. (Every 2D suite passes with it.)
3. **A cell index past int's range.** UBSan stopped the overlaps' triangle grid: a point's cell was `int(floor(v))` of a value
   far outside the grid, undefined beyond int's range. The grid's box is the triangles' own now, and the cell clamped as a double
   before the cast.
4. **A bend's absorber across its axis gave NaN.** An absorbing wall on a bend's inner side reached past its axis (f ≤ 0, µ′
   infinite) and the S-parameters came out NaN without a word. The window and its layer must stay within the radius now (said,
   with what to change), and a mode not finite is dropped.

## Perfectly matched layers: tried, not offered

PMLs let what a junction radiates leave instead of coming back off a wall. The expansion supports them - layers outside the
window of the edge material, stretched exactly as the mode solver's own PMLs (`fem_waves.cpp`: s = 1 - jσ0ξ², σ0 = 1.5 ln(1/R)/(k
d)), µ evaluated stretched at every point of the overlaps, and the mode solver's new **Keep the PMLs' own modes** option (a 2D
Mode Analysis step's `keeplayers`) giving their modes as the basis - and with the layers' fast-dying modes left out (above the
core's index, |Im n| near half Re n, all in a layer: they took 7 of 30 modes of a wire's cut) a single junction agreed with
metal walls (0.894 against 0.892). But a 1x2 MMI's field then carried **more power out than in**: 0.54 W through its window of
0.5 W fed, with either projection alone. The layers' modes are not orthogonal in power, and truncated, mode matching keeps
neither power nor passivity. So the model offers metal, magnetic and **absorbing** walls - a lossy medium, which keeps both, as
the comparisons above show - and the PML code stays, with this written beside it.

## Timings (this machine, Release)

| | |
|---|---|
| A wire's cut, 1 mode (44k unknowns, h 0.02 µm in the core) | 0.4 s |
| ... 30 modes | 2.6 s (13k unknowns) to 7 s (44k) |
| An MMI's cut (3 µm wide), 60 modes | 16 s |
| A junction, two cuts of 40 modes | 4 s |
| A coupler, three cuts of 40 modes, at one length | 7.5 s |
| A 1x2 MMI, five cuts of 40 (90) modes, at one length | 15 s (46 s) |
| The same, absorbing walls, 80 modes | 90 to 230 s |
| The same at another length | the overlaps alone: the cuts are kept |
| A 90° bend of the wire, three cells, 60 (80) modes | 60 to 81 s (115 s) |
| A 2D-like bend or MMI (uniform in z), 30 to 40 modes | 0.5 to 1.8 s |
| `test_fem3_eme`, 15 tests | 93 s |

Wavelengths are solved side by side, as many at once as half the machine's cores (a step's **Wavelengths at once**).

## What could not be verified

- **Bends in 3D** against a 3D full-wave solve or a measurement: the bend's mode is checked against the ring solver, and a whole
  bend against the 2D full-wave solver only where it is a 2D problem (uniform in z). A wire's 90° bend here loses 0.16 dB at
  1.5 µm, mostly the junctions' mismatch; published numbers for such bends were not compared. (The test runs it with 20 modes,
  -0.137 dB at 1.5 µm, and checks only what holds at any count: reciprocity, little back, the loss's rise.)
- **S-bends and rings.** An S-bend is a path with two corners turning opposite ways, solved the same way; none was compared with
  anything. A ring resonator - a closed loop and its bus coupler - is not an expansion along one path and is not built.
- **Silicon nitride** devices: the method is the same, and the radiating bend above (core index 2) is close to one in 2D, but no
  SiN device in 3D was compared with anything.
- **PMLs**: see above.
- **The 3D field** of the expansion is not drawn: the solve keeps the S-parameters and the ports' modes, not every cell's modes
  at every wavelength.
- **Gratings, crossings, Y-branches**: the stages to come.
- **Linux, Windows, Qt 6.10.3**: not run.

## The methods weighed (before building)

| | 3D FEM (edge elements, PML, mode ports) | EME | 3D FDTD |
|---|---|---|---|
| Devices | any shape | piecewise uniform along a path; bends by bend modes | any shape |
| 10 µm straight wire | ~0.6 M unknowns (order 2), ~40 GB LU, ~10 min a wavelength (estimated from phase C's fits) | one cut: seconds (measured) | ~15 M cells, ~1 GB, minutes for the spectrum (estimated) |
| 3 x 15 µm MMI | ~2 M unknowns: past 64 GB | 3 to 5 cuts: tens of seconds a wavelength (measured) | ~45 M cells, 10 to 30 min (estimated) |
| A sweep | a factorization a wavelength | new modes a wavelength; a length free | one run |
| Reuse here | partial | most | little |

## Appendix: the MMI's reference

Mode matching with every mode of a box of finite differences, for the 2D MMI of `anMmiAgreesWithModeMatchingOfEveryMode`
(scalar E_z; numpy), its arguments the MMI's length, the core's index and the points across: `7.185 2.0 1600` gives light out
0.880152, back 0.00303; with 800 points 0.879323 (this text of it run so; the 1,600 from the same code before it was shortened).

```python
import sys
import numpy as np

lam = 1.55; k0 = 2 * np.pi / lam
L = float(sys.argv[1]); nc = float(sys.argv[2]); N = int(sys.argv[3])
W, w, a = 3.0, 0.5, 2.0
U, d, Nab = W / 2 + 1.5, 1.0, 4
half = U + d; h = 2 * half / (N + 1); u = -half + h * np.arange(1, N + 1)

def eps(section):
    n = np.full(N, 1.444, dtype=complex)
    if section == "in": n[np.abs(u) < w / 2] = nc
    elif section == "mmi": n[np.abs(u) < W / 2] = nc
    else: n[np.abs(u - W / 4) < w / 2] = nc; n[np.abs(u + W / 4) < w / 2] = nc
    depth = np.abs(u) - U
    for i in range(Nab):
        n[(depth > i * d / Nab) & (depth <= (i + 1) * d / Nab)] = 1.444 - 1j * ((i + 0.5) / Nab) ** 2
    return n * n

def modes(section):   # every mode of the box, metal at its edges
    A = (np.diag(np.ones(N - 1), -1) + np.diag(np.ones(N - 1), 1) - 2 * np.eye(N)) / h**2 + np.diag(k0**2 * eps(section))
    b2, V = np.linalg.eig(A)
    beta = np.sqrt(b2.astype(complex)); beta = np.where(beta.imag > 0, -beta, beta)
    o = np.argsort(-beta.real); beta, V = beta[o], V[:, o]
    V = V / np.sqrt(np.sum(V * V, axis=0) * h)          # unconjugated: sum phi^2 h = 1
    return beta, V

def interface(A, B):   # psi and its x-derivative continuous, both bases complete
    (bA, VA), (bB, VB) = A, B
    O = VA.T @ VB * h; I = np.eye(N)
    t = np.linalg.solve(np.diag(bA) @ O + O @ np.diag(bB), 2 * np.diag(bA))
    tb = np.linalg.solve(np.diag(bB) @ O.T + O.T @ np.diag(bA), 2 * np.diag(bB))
    return [O @ t - I, tb, t, O.T @ tb - I]

def prop(beta, length):
    P = np.diag(np.exp(-1j * beta * length)); Z = np.zeros((N, N), dtype=complex)
    return [Z, P, P, Z]

def star(a, b):   # Redheffer
    a11, a12, a21, a22 = a; b11, b12, b21, b22 = b; I = np.eye(N)
    X = np.linalg.inv(I - b11 @ a22); Y = np.linalg.inv(I - a22 @ b11)
    return [a11 + a12 @ X @ b11 @ a21, a12 @ X @ b12, b21 @ Y @ a21, b22 + b21 @ a22 @ X @ b12]

mi, mm, mo = modes("in"), modes("mmi"), modes("out")
S = star(star(star(star(prop(mi[0], a), interface(mi, mm)), prop(mm[0], L)), interface(mm, mo)), prop(mo[0], a))
out = sum(abs(S[2][k, 0]) ** 2 * mo[0][k].real / mi[0][0].real for k in (0, 1))   # the two guided supermodes
print("light out %.6f, back %.5f" % (out, abs(S[0][0, 0]) ** 2))
```
