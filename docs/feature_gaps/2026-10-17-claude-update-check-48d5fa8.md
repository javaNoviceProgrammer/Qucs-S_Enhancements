# Update check 2026-10-17 (48d5fa8)

The reviewer's check of 59d76c1..48d5fa8 (`2026-10-17-claude-update-check-48d5fa8.md`) passed its ten items. It raised one new fault, four nits, and two items still open from earlier checks.

Each item was first reproduced. The reviewer's files in `~/QucsWorkspace/update_check_1017b` (mirror_purcell.qfem, scan_mirror.qfem, toy/), and the Liu 2013 model in `~/QucsWorkspace/liu2013_phase_shifter`, were copied to scratch and solved by this build's app through its tools, or by the tests. Each was then fixed, tested and broken on purpose.

Asked mid-round why selecting text in a PDF hangs, the cause was found and fixed in a commit of its own (2522d21; `2026-10-17-claude-pdf-selection-hang.md`).

## The items

| # | Feedback | Now |
|---|---|---|
| 1 | **New:** `show_layout` says `drawn: shapes 0, dots 0` when the layout viewer is on the GPU; the same view on the CPU, 74,075 shapes and 163,187 dots | On the GPU the counts were the canvas's paint's, filled when the window paints it - show_layout read them before (nought) - and they were every shape its buffers hold, not what is in sight. The view now counts what the CPU would draw in the same view (`countDrawn`: the renderer walking the cells, keeping nothing), once per view: on the GPU when asked, on the CPU from its own paint. The same counts whichever draws it. "Drawn in part" is the CPU's alone: the GPU draws every shape. |
| 2 | **Still open:** the toy laser's current at a fixed 1.7 V is not mesh-converged: 2.185, 2.013, 1.868 mA with its claddings' rows 4, 8, 16 | Converged. A heterojunction's band step was spread along the wider gap's first edge, a barrier one element wide, so the current through it was the mesh's. Its rows from 4 to 256 gave 2.185 mA to 77 µA, still falling. The step is now at the interface (below). At 1.7 V the toy now gives 10.3, 16.3, 32.6, 35.6, 36.6 µA at 4, 16, 64, 128, 256 rows (about 37 µA in the limit); at 1.4 V, 140, 164, 223, 230, 232 nA. The threshold is 7.11-7.12 µA (7.25 before), slope 0.1739 W/A (unchanged), now at 1.61-1.65 V. |
| 3 | **Nit:** the array scan asks for 64 / 256 points and logs 60 / 252 kF without saying why | The log line says why: "the array scan's 60 kF clustered about Rayleigh's anomalies at kF ±4.189e+06 1/m - of the 64 asked, tanh-sinh's rule between them (51 + 13 points, each interval's an odd number) less each interval's two ends, the anomalies themselves, where the integrand is singular and the rule's weight some 1e-8 of the rest". At 256: 205 + 51, 252 kF. |
| 4 | **Nit:** the commit says scan cells run on Eigen's LU, but a 42k-dof cell's log says Accelerate's LU (Automatic from 20k unknowns), 34 ms a cell | Under Automatic, a scan's cells take Eigen's LU, each cell on a thread of its own, below 50,000 unknowns (rows, a complex system's: half the log's degrees of freedom). Above that they take Accelerate's, one cell after another. The choice is made on the matrix each LU factorizes: the periodic ties' (fewer unknowns than the system's). For 32 cells on this machine, Accelerate against Eigen: 21k rows 1.02 s against 0.34; 33k 1.04 against 0.72; 44k 1.35 against 1.19; 56k 1.71 against 1.89; 82k 2.27 against 3.40; 196k 6.40 against 15.35; 504k 19.6 against 105.6. The reviewer's 42k-dof cells: 60 cells in 0.61 s, Eigen's LU. The commit's statement was true only below 20k. Choosing Accelerate or Eigen in Settings still decides. |
| 5 | **Nit:** fem_solve's "shown" stays Ez (plot group ewfd.Ez) after switching the polarization to hz; no warning that the plot shows a field not solved | Said, as a warning of the solve: "Ez: it shows ewfd.Ez, nought under the Hz polarization of Electromagnetic Waves, Frequency Domain (ewfd) - it solves ewfd.Hz (out of the plane), ewfd.Ex and ewfd.Ey". Each plot of a plot group (2D or 1D) is checked, its parts too (`_re`, `_im`, `_abs`): Ez, Hx, Hy under Hz; Hz, Ex, Ey under Ez. The plot itself is not changed: it is the user's. |
| 6 | **Nit:** an integral of `1.602e-19*semi.R` shows as "7.241 µHz"; a q constant with a unit would help | q0 (also `e_const`) has its unit, C: `q0*semi.R` integrates to A. What was missing is that it was said nowhere. The format text, and the Parameters node's help, now list each constant with its unit: "q0 or e_const (the elementary charge, C)", eps0 (F/m), mu0, c0, kB, h_const, sigma_const, N_A_const, R_const, g_const. fem_evaluate adds a hint where a number is a constant's value written bare: "1.602e-19 is q0's value without its unit (the elementary charge, C): q0 gives the result its unit". A number with its unit in brackets is not pointed out. |
| 7 | **Still open:** with incomplete ionization the small-signal C is asymmetric (the Liu 2013 benchmark) | Fixed, and a fault in time found with it. Reproduced on the reviewer's model at 1 MHz: C_aa 1.524 pF, C_kk 2.093 pF at 4 V (2.99 and 4.107 at 0 V). It is now 2.462 pF both (4.682 at 0 V), and G_aa = G_kk; complete ionization gives 2.458 (4.655). Below. |

## 2: heterojunctions abrupt

**Why.** A node on a heterojunction holds the narrower gap's carriers. The band step to the other semiconductor was taken along each edge into it, in the Scharfetter-Gummel step, so the step was smeared over that edge's length. That is a graded barrier one element wide: refined, the barrier sharpens, and the current through it changes.

- An n-n isotype bar (Al0.3Ga0.7As / GaAs, 1e18 cm⁻³, 0.1 V): 92.5, 80.7, 54.9, 46.5, 40.8 A from 20 nm to 1.25 nm elements.
- The VCSEL example's current at 1.9 V: 2.09 mA with 5 cavity rows, 1.63 with 10, 1.42 with 20.

**Now (the default):**

- **The step at the node.** Each edge's carriers are its triangles' semiconductor's at both ends. An end on a node of another semiconductor's bands converts to this one's at the node: n e^(sN/Vt), p e^(-sP/Vt), with s the step from that node's bands to these. Quasi-Fermi levels stay continuous across the interface: drift-diffusion's heterojunction.
- **Each node's box filled by its parts' carriers.** The part of an interface node's box in the wider gap holds that side's carriers, in Poisson's charge and in the carriers' change in time and at a frequency. As it was, the narrow gap's carriers filled the whole box: a sheet of charge on the wide side, a width of the mesh's, raising the barrier.
- **An edge's medium is its triangles' bands.** An edge between two domains of one material is that material's: from a node of a heterojunction, the band step is at that node. It had been taken as an edge of two media, with no step. In the reviewer's toy, from the GaAs disk's corner, the edges along the boundary between the layer beside the disk and the cladding under it (two domains of Al0.6Ga0.4As) took the GaAs's electrons into the AlGaAs. With the step at the node and the boxes split, the toy at 1.4 V still gave 452, 353, 295, 264 nA at 64, 128, 256, 512 rows; now 223, 230, 232.
- **An edge along a heterojunction is two,** one for each side's triangles, each carrying its own side's carriers. As one edge, the narrow gap's carriers conducted over the wide side's half too. Its share is small: a lateral GaAs channel under undoped AlGaAs (an unstructured mesh, 10 mV along it) gives 27.17, 27.20, 27.22 A at 10, 5, 2.5 nm elements; with the AlGaAs's half on the GaAs's carriers, 27.42, 27.32, 27.27. A layered mesh gives such edges nearly no weight (right angles across them).
- **No saturation over a step.** A triangle with a corner of other bands holds the heterojunction's step, and its carriers are emitted over it, not drifting. Its mobility is not saturated there. Saturated on a coarse mesh, the holes' quasi-Fermi fall across germanium's 0.48 eV valence step into p⁺ silicon (one element's, its depletion unresolved) lowered their mobility, and that lower mobility held the fall: a Ge-on-Si bar collected 223 µA where it collects 946.
- **What is read at a point** (semi.n, semi.Jn, the quasi-Fermi levels) takes a triangle's corner of the other semiconductor on the triangle's bands.

Its convergence:

| Model | Graded, as it was | Abrupt |
|---|---|---|
| The reviewer's toy, 1.7 V, rows 4 → 256 | 2.185 mA → 77 µA, still falling | 10.3 → 36.6 µA (limit about 37) |
| The isotype bar, 20 → 1.25 nm | 92.5 → 40.8 A | 17.9, 26.7, 31.5, 34.5, 35.7 A (about 36.5) |
| A strip double heterostructure (test), 1.6 V, rows 32, 128, 512 | 7.65 A, 822 mA, 309 mA | 176, 223, 228 mA |
| The VCSEL example, 1.9 V, cavity rows 5, 10, 20, 40 | 2.09 (5), 1.63 (10), 1.42 (20) mA | 1.456, 1.418, 1.408, 1.406 mA |

**The VCSEL example** (vcsel_laser.qfem): threshold 47 µA at 1.37 V (was 49 µA at 1.34 V), 0.60 W/A, 0.82 mW at 1.42 mA at 1.9 V (was 0.95 mW at 1.63 mA). Its ten cavity rows are now within 1 % of forty (they were 11 % from twenty). Its 36 points solve in 15 s. Its description says so.

**The other way kept:** the Semiconductor physics' "Heterojunctions", Abrupt (default) or "Graded over an element", as it was. It matters for one case: a carrier crossing a large step at high density.

- The lateral Ge-on-Si photodiode benchmark (Virot 2017, `aPublishedLateralPhotodiode`) measures 1.16 A/W at 25 µW. Abrupt, its holes must cross germanium's 0.48 eV valence step into the p⁺⁺ silicon at 5e6 A/m². Drift-diffusion (or emission over the step) would want some 1e21 cm⁻³ of holes at the interface for that: 66 nA of the 31 µA (0.003 A/W).
- The real device passes it by tunnelling through the silicon's few nanometres of depletion and by the interface's intermixing, which no abrupt drift-diffusion has. Graded over an element takes the step as an intermixed interface the element's width: 1.248 A/W, the same on 1 nm interface elements.
- The benchmark's test asks for it, saying why. The examples ge_photodiode and gesi_eam give the same currents abrupt as graded (8.8 nA, 1.043 mA): their current densities are far lower. They now take 23 and 37 Newton steps where they took 102 and 101.
- In 1D (a bar lit at 3e3 A/m²) both ways collect everything.

**What to mesh:** abrupt, a step's depletion and accumulation want elements of a few Debye lengths at the interface (1 nm at 1e19 cm⁻³) where a current crosses it at high density with saturation on. A tall Ge-on-Si bar (saturation, lit) collects 838 µA on 30 nm elements, and 945 on 3 nm, as without saturation (946). The property's tooltip says so.

**The laser tests' pillbox toy** (claddings of four rows) declares "Graded over an element". Its voltages were found that way, and its tests are of the laser's rate equations, which do not depend on the transport. Abrupt, the same toy reaches threshold at 1.78 V. Its current at 1.9 V converges with its rows: 23.5, 26.9, 49.1, 54.4 µA at 4, 16, 64, 128 rows. Graded, at 1.6 V: 191, 143, 36, 5.9 µA.

## 7: incomplete ionization's charge

**Why, at a frequency.** With dopants ionized in part, the ionized donors follow the electrons (Nd⁺ = Nd / (1 + g n/n1)). Poisson's equation had them, but each continuity's store, the carriers' change at jω, was the carriers' alone. A carrier a dopant takes leaves its band through no current, so the device's charge was not kept, and the terminals disagreed: C_aa ≠ C_kk.

**Now.** Each continuity's store is the carriers' less their dopants' ions: n − Nd⁺ and p − Na⁻, in the matrix and in the terminals' currents.

**In time, the same, and a second fault.**

- The ionized dopants are now kept with the field (two values a node), so a step's BDF history has theirs exactly, as it has the carriers'.
- The contacts' displacement current took their charge's history as the charge at the history's state. That holds only for a charge affine in the unknowns, and the ionized dopants are not: Nd⁺ was evaluated at the history's combination of n.
- A 1e18 cm⁻³ junction ramped −2 V in 1 µs gave I_a −81.8 mA and I_k +39.6 mA at −1 V, against C dV/dt −2.95 mA, on 21ec256 too. The dopants' ions are now taken out of that charge, and their change is added from their own history: −2.951 and +2.951 mA. Complete ionization was right before and is unchanged (2.930).
- Two more found on the way, with the ramp in 10 ps:
  - **The dopants' values were nought at a step in time's start.** Each step sets its problem up again, and the field was cut to its old size and grown again, so a solution kept from the step before lost them. Now the field is resized in one go, and a step in time's start takes them from its carriers. Before, the 1 µs ramp took 73 steps; now 48.
  - **Newton never met in 10 ps.** Its test is each row's residual against the flows through the row, estimated as |Jacobian × unknowns|. The dopants' change in time is large, Q A Nd⁺/Δt, but its derivative times the carriers is small where they are depleted, so its rounding exceeded 1e-6 of that estimate. Its own size is now among the flows. The 10 ps ramp: 103 steps, I_a −364.7 A = −I_k at 2 ps. (21ec256 met it, with I_a −13.2 kA against I_k +6.3 kA.)

**Checked against the DC solutions.** dQ/dV of the depletion charge, ∫min(ρ, 0) at ±10 mV (the reviewer's workaround): 4.846 pF at 0 V, 2.493 at 4 V, ionized in part. Fully ionized, 4.812 and 2.485 against the small signal's 4.655 and 2.458. The same 3.4 % and 1.1 % above, both ways: the measure's (ρ between nodes), not the fix's.

## Tests

- **test_layout_doc:**
  - `show_layoutShowsACellRegionAndLayers`: the drawn counts the same after the view is switched between the GPU and the CPU, and not nought.
  - `aMillionShapesAreReadAndDrawnInTime`: the GPU view's counts the CPU's for the same view; the canvas's buffers hold the million.
- **test_fem_waves:**
  - `anArrayScanAgainstAWindow`: the log's kF are its intervals' points less two each, and "of the 16 asked".
  - `aPlotOfAFieldNotSolvedIsSaid` (new): Ez and `abs(ewfd.Hy_im)` under Hz said; Hz, normE, Ex, `ewfd.Ezz` not; Hz under Ez said.
- **test_fem_sparse `manyAtOnceFromFiftyThousand` (new):**
  - 50,000 by default;
  - a 30,258-unknown matrix on Accelerate's LU alone and on Eigen's at once;
  - the environment's size and the choice of Accelerate over it.
- **test_fem_engine `constantsWithTheirUnits` (new):** each constant's unit; the text; bare values pointed out (q0, eps0, kB, c0), and not with a unit, as a name's part, or for an unrelated number.
- **test_multiphysics_doc `claudesToolsBuildAndSolveAModel`:** the format text lists q0 with its unit; fem_evaluate's hint for `8.854e-12`, none for `eps0`.
- **test_fem_semiconductor:**
  - `anAbruptHeterojunctionWithItsMesh` (new): the strip at 1.6 V converges abrupt (within 5 % from 128 to 512 rows) and does not graded. At rest, the cladding's electrons beside the interface are fewer by e^(−ΔEc/kT), times Nc's ratio 2.13 (graded: 0.75 of the GaAs's).
  - `edgesBetweenDomainsOfOneMaterial` (new): the reviewer's toy without its laser, a GaAs disk in Al0.6Ga0.4As about an axis at 1.4 V: 223.0 and 229.9 nA at 64 and 128 rows, within 6 %.
  - `notSaturatedOverAStep` (new): the lit Ge-on-Si bar (0.1 µm tall) on 30 nm elements collects within 15 % of its unsaturated current: 276 against 314 µA (82 with the step saturated).
  - `aJunctionIonizedInPart` (new): a 1e18 junction ionized in part. C_aa = C_kk = −C_ak = −C_ka to 1e-6; a 1 µs ramp's I_a = C dV/dt to 1e-3 and = −I_k to 1e-4; a 10 ps ramp met, I_a = −I_k to 1e-4 (Release builds: some minutes sanitized).
  - `aHeterojunction` (unchanged) passes: the Si/Ge band steps to 1 meV, a p-Ge/n-Si diode's current to 3 %.
  - Under the sanitizers the new ones are lighter: the strip at 32 and 128 rows; the toy at 16 rows alone, its 164 nA below the limit, some 232 nA, as the rows have it (the two-media edges give 542, above it); the junction on 0.1 µm elements without the 10 ps ramp (its consistency, not its C: 0.79 nF there).
- **test_fem_laser:** the pillbox toy declared graded; `theVcselLaserExample` at the new numbers (six points: threshold 45 µA at 1.37 V, 0.598 W/A, 0.82 mW at 1.42 mA).
- **test_fem_optoelectronics `aPublishedLateralPhotodiode`:** graded, said why.

## Breaks

Twenty-nine changes broken on purpose; each was built, its tests run, and the file restored (the scripts are `breaks_uc48*.py`, kept in scratch).

- **24 caught:**
  - the layout counts not counted, or counting no polygons;
  - the scan's intervals not said, or the number said not the asked;
  - the at-once size 20,000, or ignored;
  - the plots not checked, a field's part (`_im`) not read, the polarizations swapped;
  - the constants' tolerance 2e-6, a number with its unit pointed out, the format text without the constants, fem_evaluate without the hint;
  - the step along the edge again;
  - the boxes the nodes' own (the strip and the VCSEL example);
  - an edge of two domains of one material two media (the toy's 452, 353 nA again);
  - the corners not on their triangle's bands;
  - saturated over a step;
  - graded never (the strip, and the photodiode benchmark: abrupt, one of its cases finds no solution on the way to its voltages);
  - the small signal's stores the carriers' alone;
  - the contacts' charge less the doping again;
  - the dopants not kept;
  - the dopants not among Newton's flows;
  - the field cut and grown again together with the dopants not taken at a step's start.
- **5 not caught:**
  - **The field cut and grown again, alone, and the dopants not taken at a step's start, alone:** each covers the other. Kept both: one for a step after another, the other for a field from elsewhere.
  - **In time, the continuity's store the carriers' alone:** a ramp in 1 µs has each voltage's state at rest, whatever the rows' stores, and the contacts' currents come from the states' charges: the same to 1e-4. In 10 ps it differs (−380 A against −313 at 1 ps), but no independent value for it was made. A 100 GHz sine in time against the small signal's Y was tried as one, and took over ten minutes. It is kept: the small signal's matrix is the same equations'.
  - **The split along a heterojunction, in two forms** (an edge of two sides folded into one side's; both sides on the narrow gap's carriers): the toy's layered mesh gives such edges no weight. Its share is the channel's 0.9 % above, too small to tell from another platform's mesh.

## Runs

- **The Release suite (offscreen):** 127 of 127, 256 s. test_fem_semiconductor whole after the last changes: 37 of 37.
- **On the display:** test_multiphysics_doc 57 of 57. test_layout_doc: its counts' checks pass; three picture tests of the canvas (`theCanvasIsLightOrDarkWhateverTheTheme`, `theLayersStandOutFromTheCanvas`, `aGridIsBehindTheLayout`) fail on the display as they do at 2522d21, with this round's layout files taken out. Offscreen they pass.
- **Under the sanitizers,** with Eigen's solvers as CI's Linux has them: the fourteen multiphysics test files this round touches or the abrupt default reaches, whole (semiconductor, devices, quantum, nitrides, laser, optoelectronics, modulator, resonator, Lumerical, waves, sparse, engine, layout, the multiphysics document): all pass, no report, 1966 s six at a time. The new functions each within a minute there: the strip 27 s, the bar 32, the junction 59, the toy at 16 rows 56.
- **The lints pass;** the tools' end-to-end scenarios: 97 checks, none failed.

## Not done

- **A heterojunction's own transport** (thermionic emission, tunnelling through a spike) is not modelled: abrupt is drift-diffusion's, continuous quasi-Fermi levels. Where a device relies on it, as the Ge-on-Si lateral photodiode at 25 µW, "Graded over an element" stands in, mesh-dependent by its nature.
- **The mesh is not refined at heterojunctions** by the physics: the user's, as the tooltip says.
- **A scan's choice of threads** (by the tied cell's size) is tested through the decision it takes (`manyAtOnceFromFiftyThousand`), not by a scan of 20k-50k-unknown cells: the measurements above are its evidence.
- **The Lumerical export** does not carry "Heterojunctions".
