# Multiphysics 3D, 4 November: decision 8's devices held to what is published, and two solver faults they found

The task, handed over from the session that runs this work: *continue the Multiphysics 3D development where
phase C stopped* - CI first, then what phase C still lacks, each result checked against a published or
analytic one. Phase C (`2068aed`, its GCC fix `25a6d4f`) solved heat, electrostatics and currents in 3D and
held a TSV's liner capacitance to Katti et al.; it left open the `fem_*` tools on 3D models, heat in time,
results nodes, estimates before a solve and **the validation against publications of decision 8's devices**:
the heater over a ring, a VCSEL array, the photodiode with fingers, and a TSV's resistance and thermal
resistance. This round takes up those devices first: they test the solver everything later is built on.

| | What it found or holds |
|---|---|
| CI | Five reds on `25a6d4f` and `d7facd8`, none of phase C's: two races, a solve too long under the sanitizers, two timing ratios (`ac67eb6`) |
| A photodiode's fingers | One gap of the endless array of electrodes converges on Lim and Moore's capacitance - the error halving as the mesh is, as it must at a strip's edge, and the two finest within 0.3 % extrapolated (orders 1 and 2) |
| A VCSEL array | One VCSEL of a 3 x 3 array heated: its rise at its middle and over its disc, and its neighbours' (the cross-resistances), are Carslaw and Jaeger's disc on a half-space with the substrate's bottom and walls as images, order 2 within 0.005 % (its disc's mean 0.1 %) |
| A TSV | Its resistance Katti et al.'s ρh/(πa²), its copper's thermal resistance h/(kπa²) and its liner's ln(b/a)/(2πkh), order 2 within 0.002 % |
| Found by them | **An electrostatics in µm was not solved at all**: multigrid took its right-hand side, some 1e-17, for nought, and the capacitance came out 7 to 21 times too large. **Multigrid's ILU(0) smoother stalled** on order 2's stretched prisms (a VCSEL array's 1.2 million unknowns, the chip heater's 55,272): it is Gauss-Seidel now |
| The heater over a ring | C. L. Manganelli et al.'s double racetrack (IEEE Photon. Technol. Lett. 2018): 20.65 mW per FSR per ring at order 2, 20.86 at order 1 - their 3D simulation 21.2, their measurement 20 to 22 -, the ring's length from its FSR with the strip's group index by the 2D mode solver |
| Examples | A VCSEL array, an MSM photodiode's fingers, a TSV and the heater over a ring, each solved by a test beside its formula |

## CI first

Run 41729396446 (`25a6d4f`) was the first in which GCC compiled all of phase C: Linux and Windows built it.
Its macOS job and run 41746872589's Linux job (`d7facd8`) were red by five functions, none of Multiphysics
3D's. Fixed in `ac67eb6`:

- **`TestPptxDoc::theRawInputDrawsOnASlide`** (Linux, red since `c6d4dc8`): the pen's stroke began at 953
  points for the 930 asked. `send_input` mapped a slide's points to pixels and gave the mouse's events from the
  event loop after; between them Qt showed the vertical scroll bar, which it does by a queued call, and the
  slides, fitted to the width, were fitted again to the narrower viewport - 14 px narrower in Fusion, CI's
  style, a 2.5 % zoom: 930 × 1.025 = 953. Here the scroll bar was already up when the points were mapped.
  `send_input` now maps a view's points again as its events go. A zoom queued just before the call does the
  same on every platform, and the old mapping drew nothing.
- **`TestRf25Doc::theSolvedModelIsWrittenAsATouchstone`** (Linux): an export's Write said "Written to ..." and
  the form was then made again - the solve's end, deferred, came after the click on the slow runner - with an
  empty label. The panel keeps what Write said while its node is shown. A rebuild pending at the click now
  reproduces CI's failure word for word without it.
- **`TestRf25Doc::aCoplanarPortIsReadAndItsFeedCalibrated`** (Linux, red since `c6d4dc8`): past the 300 s it
  waits for its solve. Its time is the calibration's strips - eight heights over the ground and twice the
  feed long, and shorter ones tried -, dense solves some 200 times slower under ASan: 60 s here, 154 s of
  processor. Under the sanitizers it gives the strips a length (the port's own `strips`): 6 s.
- **`TestMarkdownDoc::longDocumentsRenderAtOnce`** (Linux): a ratio of times, 7.08 under ASan for a bound of 7.
  Judged in Release alone now, as its sibling in the same file already was.
- **`TestCanvasDrawing::aLargeSchematicIsQuickToWorkOn`** (macOS): a corner's paint took 12.2 ms on the runner
  (1.5 here) beside a whole one of 31.2, timed back to back. They are timed in turn over three rounds now.

## The photodiode's fingers

**The formula.** For an endless array of coplanar fingers w wide at a period p, alternately charged, on a
half-space of εr under air, each gap's capacitance per unit length is ε0(1 + εr)K(k)/K(k'), k = tan²(πw/4p) -
C. Lim and R. Moore, *Properties of alternately charged coplanar parallel strips by conformal mappings*, IEEE
Trans. Electron Devices 15, 173 (1968); J. B. D. Soole and H. Schumacher give it for MSM photodetectors as
C = A/(w + s)·ε0(1 + εr)K(k)/K(k') (IEEE J. Quantum Electron. 27, 737, 1991). I derived it again before using
it: the cell between two fingers' middles, its walls the array's symmetry, maps onto two coplanar strips by
ζ = -cos(πz/p), and their K(q')/(2K(q)), q = sin(πw/2p), is the formula by Landen's transformation, equal to
twelve digits for w/p from 0.1 to 0.9 (and ½ at w = s, where K(k) = K(k')).

**The model.** One cell of the array in 3D, on GaAs (εr 12.9) 4 µm deep under 4 µm of air, fingers 1 µm wide
at a period of 2. The electrodes have no thickness, as the formula's: a thin box of air over each finger
draws its outline on the GaAs, and its bottom face - between two solved domains - is the terminal.

**What it gave.** Refined in proportion (the edges' size h, 2.5 h elsewhere):

| h at the edges | order 1 | order 2 |
|---|---|---|
| 0.2 µm | +10.33 % | +1.67 % |
| 0.1 µm | +5.61 % | +0.89 % |
| 0.05 µm | +2.88 % | +0.46 % |
| 0.025 µm | +1.46 % | |

Every one above the formula, as a Galerkin solve's energy is, and the error halving with h: a zero-thickness
strip's edge makes the potential go as r^½, so h, not h², is all any order can do there. Extrapolated at that
rate the two finest are +0.03 % (order 1) and +0.04 % (order 2) from the formula.

**First tried with the fingers' ends**, two finger lengths and their difference: 1.5 % under the formula
whatever the mesh. Under a bus bar the substrate's potential is the bar's, under the fingers their mean; it
passes from one to the other over some 2H/π along the fingers, 4 µm under a 6 µm substrate - so 8 µm
fingers' two ends were not independent. The endless cell is the formula's own idealization; the ends are
the example's.

## The VCSEL array

**The reference.** A disc of radius a heated by q on a half-space of k, the rest of its face insulated, rises
(2qa/πk)E(r/a) within it and (2qr/πk)[E(a/r) - (1 - a²/r²)K(a/r)] beyond: H. S. Carslaw and J. C. Jaeger,
*Conduction of Heat in Solids* (2nd ed., 1959), §8.2. Its middle P/(πka), its mean 8P/(3π²ka), and P/(2πkr)
far off. Checked here by integrating the point source over the disc: ten digits at six radii. A substrate of
finite depth and width, its bottom held and its walls insulated, is the same plus images - in the bottom,
every other one negative (an alternating series, its partial sums' last two averaged), and in the walls -,
those far enough to be points. A neighbour's rise per watt of the one heated, R_AB = ΔT_A/P_B, is the
array's cross-resistance: by linearity the whole array's temperatures are these sums.

**The model.** 3 x 3 discs 10 µm across at a pitch of 25 µm on GaAs 300 x 300 x 150 µm (k 46 W/(m·K)), the
middle one's top heated by 10 mW, the bottom at 300 K:

| | order 2 | order 1 | discs and images | a lone disc, half-space |
|---|---|---|---|---|
| its middle | 13.74962 K | 13.60255 K | 13.75025 K | 13.8396 K |
| over its disc | 11.64643 K | 11.37065 K | 11.65814 K | 11.7474 K |
| a neighbour at d | 1.30365 K | 1.29124 K | 1.30362 K | |
| a neighbour at d√2 | 0.89564 K | 0.88638 K | 0.89561 K | |

Order 2 within 0.005 % at the middles, 0.1 % over the disc (its rim's kink in the flux; 0.02 % on a mesh
twice as fine). Order 1 is 1 to 2.5 % cool: its heated disc is a polygon 0.7 % short of the circle, and its
lumped prisms - up to 60 times wider than tall here, where the swept mesh carries the discs' fine layers
across the whole plan - smooth what varies in the plane; refining the discs alone closes it slowly (-1.07,
-0.84, -0.55 % at the middle as their size halves twice). Order 1 is the default for its speed and its
M-matrix; this is what order 2 is for.

## The TSV

A copper via 5 µm across and 25 µm deep in a 0.2 µm oxide liner:

| | order 2 | order 1 | formula |
|---|---|---|---|
| resistance, ρh/(πa²) (Katti et al. 2010) | 0.0213635 Ω | 0.0216915 Ω | 0.0213631 Ω |
| its copper's thermal resistance, h/(kπa²) | 3183.16 K/W | 3232.04 K/W | 3183.10 K/W |
| its axis over the liner's held outer wall, the via heated within: P[ln(b/a)/(2πk_ox h) + 1/(4πk_Cu h)] | 0.357921 K | 0.357985 K | 0.357921 K |

Order 1's polygon is 1.5 % short of the circle's area.

## Found by the validations

### An electrostatics in µm was not solved

The photodiode's cell, in µm, gave 4.6e-10, 7.1e-10 and 1.3e-9 F/m on three meshes - 7 to 21 times the
formula, the more as the mesh was refined. The same cell in mm gave the formula's neighbourhood. Its log said
"conjugate gradients with algebraic multigrid: 0 iterations, residual 7.8e-17".

A 3D electrostatics' entries are ε0 times a length: some 1e-17 in µm. AMGCL's conjugate gradients take a
right-hand side whose norm is under the machine's epsilon for nought: they clear x and return 0 iterations
and that norm as the residual - which our relative test passed as converged. Every potential not held was
nought. Heat escaped it only because k·h is eleven orders larger; a model in nm would have met it sooner.

`AmgSolver` now scales each right-hand side to a norm of one (the guess with it), and refuses one not
finite. Nothing else in a solve depends on the matrix's scale - its aggregates, its smoother, its coarse
level, the fallback's incomplete factor -, so the system itself is left as it is (a scaling of it, written
at first, was caught by no break and taken out). `solvesAtAnyScale` (`test_fem_amg`) solves the Laplacian
scaled by 1e-20 and 1e20, and a right-hand side of 1e-20 alone, to the same answer in the same iterations;
the photodiode's cell in µm is the device's.

### Multigrid's smoother

Order 2 on the VCSEL array - 1,209,654 unknowns, prisms 30 µm wide and 0.5 µm tall away from the discs -
did not converge: 500 iterations, then the incomplete Cholesky's 1186, 243 s. The chip heater of phase C's
example at order 2 (55,272 unknowns) took 421, 283 and 83 iterations over its three Picard solves. Phase C
chose ILU(0) on the chip at one mesh, where it took 52.

The system written out and solved fourteen ways (`2026-11-04-multiphysics-3d-devices/amg_smoothers.cpp`,
`amg_smoothers.tsv`): every ILU smoother - ILU(0) with one sweep or two, a strength threshold of 0 or 0.25,
ILU(1), ILUT, under smoothed aggregation, Ruge-Stüben, its energy-minimising variant or plain aggregation -
stalled, at residuals of 3e-4 to 0.3 after 400 iterations. AMGCL's ILU(0) applies the same factor before the
coarse level and after it, a cycle that is not symmetric; conjugate gradients tolerate that on an M-matrix,
and order 2's matrices on stretched prisms are not one. Gauss-Seidel sweeps forward before and backward
after:

| system | ILU(0) | Gauss-Seidel | two sweeps | Ruge-Stüben, Gauss-Seidel |
|---|---|---|---|---|
| the VCSEL array, order 2 (1,209,654) | stalled, 3e-4 | 156 it, 33.3 s | 111 it, 34.7 s | 85 it, 27.4 s (+7.7 s setup) |
| the chip heater, order 2 (55,272) | 400 it, 9.6e-9 | 102 it, 0.86 s | 70 it, 0.87 s | 51 it, 0.82 s |
| the Laplacian (270,400) | 12 it, 0.30 s | 14 it, 0.27 s | 13 it, 0.37 s | 11 it, 0.39 s |
| copper in oxide (270,400) | 13 it, 0.33 s | 15 it, 0.30 s | 13 it, 0.37 s | 15 it, 0.55 s |

The smoother is Gauss-Seidel, one sweep, under smoothed aggregation: as quick as ILU(0) where ILU(0) works,
with 27 % less memory, and converging where it does not. In the solver: the VCSEL array's order 2 in 134
iterations (34 s, where it was 236 s), the chip heater's 97, 55 and 16. `QUCS_FEM_AMG_RELAX` still picks
ILU(0) or SPAI-0 for measuring.

## The heater over a ring

**The publication.** C. L. Manganelli, P. Velha, P. Pintus, F. Gambini, O. Lemonnier, L. Adelmini, C. Kopp, S.
Faralli, F. Di Pasquale, C. Wenger and C. J. Oton, *Low-power-consumption integrated tunable filters for WDM
switching applications in silicon photonics*, IEEE Photon. Technol. Lett. 30, 1601-1604 (2018) - its open
manuscript at Sant'Anna Pisa's repository, downloaded at the user's word (`ptl18.pdf`, 1,065,847 bytes,
SHA-256 a46cc566...dcc2). Two coupled racetracks with Bezier bends, a strip 410 x 220 nm on 2 µm of buried
oxide, an FSR of 2.4 THz (19.2 nm); over each a Ti/TiN heater loop 120 nm thick (110 nm TiN on 10 nm Ti),
434 nm wide, 600 nm of silica above the waveguide and 320 nm over the metal. Its 3D thermo-electrical
simulation, with a thermo-optic coefficient of 1.8e-4 /K, took **21.2 mW per FSR per ring**; measured, **22
and 20 mW** (vias 7 and 6 µm from the rings), and 0.95 nm/mW.

**What one FSR takes.** Δλ = λ ΔT dn/dT / n_g and FSR = λ²/(n_g L), so one FSR is ΔT = λ/(L dn/dT) whatever
the group index. L is the racetrack's length, c/(n_g FSR): the strip's TE0 by Qucs-S's own 2D mode solver,
the library's dispersive silicon and silica, has n_eff 2.2552 and n_g 4.3717, so L = 28.573 µm and one FSR
is 301.4 K - 14.2 K per mW at the paper's 21.2.

**The model.** Two stadium racetracks of 3 µm bends at that length, 250 nm apart, both heaters at 1 mW; a
silicon substrate 100 µm deep held at 300 K below, 80 µm across, its top insulated; silicon 148, silica
1.38, Ti/TiN 29 W/(m·K). Each racetrack's mean rise:

| | order 1 | order 2 |
|---|---|---|
| edges of 0.3 µm (150,000 prisms) | 14.444 K/mW, 20.86 mW/FSR | 14.597 K/mW, 20.65 mW/FSR |
| edges of 0.15 µm | 14.498, 20.79 | 14.603, 20.64 |
| edges of 0.075 µm | 14.536, 20.73 | |

Converged, 20.64 mW per FSR per ring: 2.6 % under the paper's simulation, within its measurements.

**What the paper does not say**, each tried (order 1, edges of 0.3 µm, 20.86 mW/FSR as drawn):

| | | mW/FSR |
|---|---|---|
| the bends' radius, the racetrack's length kept | 2.5 / 3.5 µm | 19.77 / 21.79 |
| the gap between the racetracks | 0.2 / 0.35 µm | 20.78 / 21.03 |
| the substrate's depth | 50 / 300 µm | 21.02 / 20.27 |
| the chip's width | 60 / 160 µm | 20.68 / 21.02 |
| a whole chip: 725 µm deep, 1 mm across | | 21.01 |
| the heater 434 nm wide, as the paper's (3 million prisms) | | 20.77 |

The bends move it most, 5 % either way; nothing else 3 %. The loops are drawn as wide as their waveguides:
the paper's 434 nm against 410 leaves 12 nm slivers between their edges in the one plan every layer is swept
from, a mesh twenty times the size for 0.1 %. The leads and pads, which the paper draws and this model leaves
out, are why its Fig. 2 has the heaters some 60 K over the racetracks where this model's loops peak at twice
their racetracks' rise: they draw heat from the loops' tips. With one heater alone, the other racetrack rises
36 % of the heated one's (the example's break below).

## Examples

`examples/multiphysics3d` has four more, each solved by `theExamples` (`test_fem3_devices`) beside its
formula:

- **`vcsel_array.qfem3d`**: the array of the test, all nine heated, GaAs of the library (its k falling as it
  warms), order 1: 18.85 K in the middle, 17.60 at an edge, 16.57 at a corner - 11.57 for a lone disc on a
  half-space. 146,000 prisms, 2.2 s.
- **`msm_photodiode.qfem3d`**: two combs of four fingers, 12 µm long, on GaAs, the electrodes their outlines:
  7.0 fF, the seven gaps' share by the formula 4.7 fF - the bus bars, the fingers' tips and the outer
  fingers' sides add the rest, and order 1 on this mesh some 5 %. 1.07 million prisms, 4.4 s.
- **`ring_heater.qfem3d`**: the double racetrack of the test, the library's silicon and silica (k of their
  temperature), 1 mW in each heater: its racetracks 14.32 K per mW, 21.04 mW per FSR per ring. 150,000
  prisms, 2.5 s.
- **`tsv.qfem3d`**: the TSV in a silicon die, currents on the via alone and heat from a pad on its top, order
  2: 21.23 mΩ (Katti's for the library's copper, to six digits), and 1332 K/W where its copper alone would be
  3183: the silicon beside it carries more than half the heat down.

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_devices` (new, 5 functions) | `photodiodeFingers`, `vcselArray`, `tsv`, `heaterOverARing` as above (order 2's VCSEL also that multigrid converged, not its fallback; the heat made the flux over its disc: order 1's polygon 0.7 % short); `theExamples` |
| `test_fem_amg` | `solvesAtAnyScale` (new); `fallsBack` refuses a right-hand side not finite |
| `test_multiphysics3d_doc` | Six examples on the start page |
| `test_pptx_doc`, `test_rf25_doc` | A zoom queued before `send_input`; a rebuild pending at Write's click |

## Broken on purpose

`2026-11-04-multiphysics-3d-devices/breaks.py` holds twelve, each built and its tests run, the sources and the
examples put back:

- **CI's fixes:** `send_input`'s points as mapped before its events go (the ring on slide 2: nothing
  drawn); an export's Write's words lost when its form is made again (CI's failure, word for word).
- **Multigrid:** the right-hand side not brought to a norm of one (`solvesAtAnyScale`: "a right-hand side
  of 1e-20: 0 iterations"; the photodiode's cell); its smoother ILU(0) again (the VCSEL array's order 2:
  "multigrid did not converge" in its log, four minutes on); a right-hand side not finite taken
  (`fallsBack`: a residual of nan, not why).
- **What the devices lean on:** a terminal on a face between two solved domains not held (the photodiode's
  zero-thickness electrodes: "holds the potential nowhere"); the 2D group index without the media's own
  dispersion (the strip's n_g 4.2276, the racetrack 3 % longer); order 2's floors weighed as straight
  triangles (the VCSEL's middle 0.31 % low: its heated disc a polygon).
- **The examples:** the TSV's currents on every domain (the die's silicon has no conductivity: refused);
  the photodiode's electrode on its comb's top, 0.25 µm over the GaAs (2.8 fF, under its gaps' share); the
  ring heater's power in one heater (its racetracks 21.1 and 7.5 K); the VCSEL array's heat on its middle
  VCSEL alone (9.9 mW made).

All twelve caught. A thirteenth was written and caught by nothing: the system itself scaled to a mean diagonal
of one, beside its right-hand side. With the right-hand side's normalisation every solve is of the matrix's
scale alone, so the system's was taken out, and its break with it. Until then the right-hand side's own
break was caught by `solvesAtAnyScale` alone - the system's scaling had covered the photodiode's cell - and
run again after, it was caught by both.

## Under the sanitizers, and the whole

Built Debug with ASan and UBSan, as CI's Linux job is, each function alone (the process's start included):

| Function | Time |
|---|---|
| test_fem3_devices `photodiodeFingers` (order 1 on three meshes, order 2 on the coarsest) | 29 s |
| test_fem3_devices `vcselArray` (order 1 alone: order 2's 585,000 unknowns are Release's) | 29 s |
| test_fem3_devices `tsv` | 43 s |
| test_fem3_devices `heaterOverARing` (the 2D mode on a coarser core, order 1 alone) | 30 s |
| test_fem3_devices `theExamples` (the TSV alone: the array's and the photodiode's ran past 300 s) | 69 s |
| test_fem_amg (`solvesAtAnyScale` among it) | 24 s |
| test_fem3_solve | 28 s |
| test_multiphysics3d_doc | 39 s |
| test_pptx_doc, test_rf25_doc | 17 s, 104 s (its coplanar feed 6 s, where it was 60) |

No sanitizer said anything. In Release `test_fem3_devices` takes 44 s.

The full ctest, 160 tests at eight at once, 6 minutes: 158 passed. The two that did not,
`test_rf25_green` (`whatTheTruncationDroppedIsCounted`) and `test_rf25_doc` (`aSolveShowsHowFarItHasGot`'s
"; solving"), are of the RF 2.5D Green's function's review, uncommitted in the same checkout while this ran
(`rf25_green`, `rf25_mesh`, `rf25_static`, `rf25doc` and their tests); both passed before it. The four
lints green; nothing written to the real cache.

## What is left

- **The heater over a ring's chain**: the proposal's own test, the 3D cut handed to a 2D mode analysis for
  n_eff(T) - phase D's bridge; here the paper's conversion (dn/dT 1.8e-4) stands in for it.
- **Of phase C**: the `fem_*` tools on 3D models; heat in time and sweeps; results nodes (3D derived values,
  plot groups with slices, isosurfaces and clips, VTU, Field on a Grid in 3D, Interpolation of three
  arguments); estimates and a limit before a solve; a user's anisotropic tensor; Newton's method; parallel
  assembly. Order 1 runs some per cent off on the swept mesh's most stretched prisms (the VCSEL array): an
  estimate could say so, and offer order 2.
- **Phase B's leftovers** (B3) and decisions 4 and 7, as before.
