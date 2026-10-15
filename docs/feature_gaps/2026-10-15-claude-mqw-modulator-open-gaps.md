# Feature gaps: the MQW modulator's open gaps

*15 October 2026 — on `22b2153`. The review: `MQW_modulator_open_gaps.md`, 15 October, read
from the source at `22b2153`: what was still open for the design flow of an MQW electro-optic
modulator (an InP depletion modulator, its wells' field from a semiconductor, its index and
loss from the mode).*

The review listed nine gaps in three groups - the local Δn and Δα of the physics, the coupling
of the electrical and optical solves, the mode analysis and its output - in a priority order.
Each is done; each was first seen as the review says.

## The review's items

| Review | Now |
|---|---|
| **A1.** III-V plasma dispersion: the wave feature's `plasma` silicon's Soref-Bennett fit alone, wrong for InP by 2-5; a `model` choice, Drude's of the material's masses with an override, at the mode step's wavelength | **Done.** `plasmamodel`: `soref` (as before), `drude` - Δn = -(q²λ²/8π²c²ε0 n)(N/me + P/mh), me the material's (its compound's at its composition), mh the heavy and light holes' conductivity mass (mhh^1.5 + mlh^1.5)/(mhh^0.5 + mlh^0.5) of its γ1, γ2 (InP: me 0.0795, mh 0.399), or `mepl`, `mhpl` - or `user` (∂n/∂N, ∂n/∂P). At the wavelength solved. InP's Drude per electron at 1550 nm 4.1e-21 cm³ (n 3.3), 4.7 times silicon's 8.8e-22. emw.dncarriers. |
| **A2.** Band filling (Burstein-Moss), the largest term of a depletion MQW modulator; P_bf from a table or computed (Bennett, Soref and del Alamo) | **Done.** `bandfilling`: computed of the material's gap (Egd, else Eg, at the carriers' temperature `Tbf`), me, γ1, γ2 and its edge's strength - a measured one (Afk: the library's InP 1.16e5 per cm, Aspnes and Studna's), else Kane's Ep (α_j = q² Ep (2m_rj/ħ²)^{3/2} √(E - Eg)/(12π n c ε0 m0 ω), heavy and light holes each) times Sommerfeld's excitonic enhancement of the material's εr (`bfexciton`) - Fermi-Dirac occupancy of the states the photon joins, Δα = -Σ α_j (f_c + f_v), and Δn its Kramers-Kronig transform (the principal value with the pole taken out); electrons, and holes (`bfholes`). Tabulated at the mode's photon energy from 10¹⁴ to 10²⁰ cm⁻³, as N below, held above. Or `bfmodel`: `user`, Δn per electron and per hole (`Pbfn` may be an Interpolation of lambda0: a table against wavelength). N and P are Ncarriers and Pcarriers: semi.n, semi.p by default, qw.n where the wells give it. Above the gap the edge is bleached no more than the medium absorbs (its κ and a direct gap's). |
| **A3.** Free-carrier absorption per material, σn N + σp P as λ², the doped claddings' static loss too | **Done.** `fca`: Δα = (σn N + σp P)(λ/1550 nm)², the material's `sigmaFCn`, `sigmaFCp` (the library's InP, InGaAs, InAlAs, InGaAsP, AlGaInAs: 1e-18 and 2e-17 cm²; silicon 8.5e-18 and 6.0e-18) or the feature's. Of all the carriers there - a doped cladding's loss with the change. emw.alphacarriers. With silicon's fit, which has its own absorption, refused: it would count twice. |
| **A4.** QW electroabsorption and electrorefraction from the device field; a quadratic (Kerr, QCSE) coefficient as the simple alternative | **Done.** `qwabsorption` ("Quantum wells' absorption (QCSE)"): a quantum wells physics' absorption and Kramers-Kronig index at the mode's photon energy, of the field at each point (`Fqw`, semi.Ey by default). The mode step solves the wells again at `Fpoints` fields from -`Fmax` to `Fmax`, each its spectrum as a Spectrum step says (`spectrumstep`, one disabled too: its options alone - energies the Kramers-Kronig window round the photon's, broadening, excitons, absorption per), and reads it there; a point's α (TE in the wells' plane, TM along their axis) adds to κ, n(F) - n(F0) to n. emw.alphaqwTE, alphaqwTM, dnqwTE, dnqwTM. `kerr`: Δn = ½ n³ R (\|E\|² - E0²) along x, y, z (R one value or three), E the static field. |
| **A5.** Alloy composition from a target gap, a "1.3Q" solved for (Vurgaftman's 0.993 eV at the composition meant for 0.954) | **Done.** A material's `composition`: `gap` solves its compound for hc/λg at 300 K in its band set - a ternary's x; a quaternary's x and y on a binary's `lattice` (InP by default): InGaAsP along As and P, AlGaInAs and AlGaInP along Al and Ga, Vegard's lattice matched. Every property is then the compound's at that composition: the gap at any temperature, masses, offsets, deformation potentials. The model's log says it. |
| **B1.** Perturbative Δn_eff and Δα_mode per mode, swept against bias, Δφ and VπL | **Done.** A derived value **Perturbation**: each mode's dneff = (c ε0/2) ∫ n Δn \|E\|² dA / ∫ Sz dA and the power's dalpha = (c ε0/2) ∫ n Δα \|E\|² / ∫ Sz over the domains chosen, `dn` and `dalpha` any expressions (emw.dncarriers, emw.alphacarriers by default); dloss in dB/cm; with a `length`, dphi = k0 dneff L. Along a sweep with a `bias` (the swept parameter): from its first point, dphi_sweep and VpiL = λ\|V - V0\|/2\|Δneff - Δneff0\| (V·cm). |
| **B2.** Layer stacks: anisotropic or structured meshing, or a 1D semiconductor | **Done, both.** A mesh feature **Layered Region**: rectangles of a stack (along y or x) as a grid of stretched triangles - `count` across each layer, `hlateral` along them, each cell split the one way - not refined, its sides never split (a change to the vendored triangulator, below). Forty 6-8 nm layers under a 2 µm mesa: 6400 triangles in the stack at 50 nm and two across each layer, 12 126 in the model against 88 253 isotropic. The semiconductor's box method takes them: a right triangle's diagonal has its two opposite angles 90°. **A 1D semiconductor**: `"space": "1d"` solves it along a line across its layers (`axis` y or x, `at`, `h1d`) - the same physics on a strip of its own (a Layered Region of 8 columns, its sides insulating, read along its middle), every node of its domains then taking the line's values at its coordinate, so whatever reads semi.n, semi.Ey... (a mode's carriers) reads them; its contacts where the line ends or meets an interface, its currents the line's density through the narrowest of its domains across it (a mesa's), every contact alike. At rest (stationary steps). |
| **C1.** Mode tracking criteria: guided, polarization fraction, most in domains, highest n_eff; a per-mode TE fraction; a selected mode plots and derived values use by default | **Done.** The mode step keeps those above `minneff` and losing less than `maxloss` (dB/cm), of a polarization at a `tefraction` (TE at it or more, TM at 1 less it or less - it was 1/2), and the most in `track`'s domains (as before), combined. It `choose`s one of those kept at each frequency and point - the first (the highest index), the most in the domains tracked, the most TE, the most TM: a plot group's mode 0 (now its default), a 1D plot group's `modes` and a derived value's `solutions` "auto" (their default: the chosen one where the step chose one, each mode where it did not - "all" and "chosen" too; a guided TE0 against the bias, one curve), and the steps after (an Optical Generation not naming one). A 1D graph of a Perturbation has its VpiL along the sweep. emw.TEfraction was there already. |
| **C2.** Field export on a grid or at the mesh's nodes, CSV, NumPy, .dat, a chosen mode, time or sweep point | **Done.** An export **Field on a Grid**: expressions (each named if wanted) on a regular grid (its corners in the geometry's unit, empty the model's extent; nx, ny) or at the mesh's nodes, as CSV (x, y, then each value), NumPy .npy (a grid: [solution,] expression, y, x) or columns with # comments and a blank line after each row (gnuplot); one solution (its point, time or frequency, mode), each, or each point's chosen mode. NaN outside the domains. fem_export says a .npy's header. |

## Checked

**A1, A3.** A 2 by 0.5 µm core of InP (its masses; n 3.3) in n 3.17, N 1e18 and P 5e17 cm⁻³:
emw.dncarriers Drude's to 1e-9 at 1550 and 1300 nm, the masses given, silicon's as before and
coefficients given to 1e-9; the TE0 slower by less than the core's Δn. Free-carrier
absorption 11 per cm at 1550 nm (1 + 10), as λ² at 1300; the mode losing between 0.3 and 1 of
it. A material without masses or cross-sections, and silicon's index with absorption: said.

**A2** against its integrals done in the test, independently: degenerate electrons (4 K,
10¹⁸ cm⁻³) bleach the edge up to their Fermi level, and Δn at 1550, 1300 and 1000 nm is the
closed integral of that block to 2e-3; above the gap Δα is -α exactly; the index is continuous
across the gap; few carriers at 300 K (10¹⁵, 10¹⁶) as Boltzmann's integral to 5e-3; holes a
third or so of the electrons', and degenerate (4 K) the closed integral of both bands filled to
one Fermi level to 2e-3; the table between its points to 3e-3. Sommerfeld's enhancement exact
at the gap's Rydberg and far above it, finite at the edge; degenerate electrons with it as the
test's own integral to 2e-3, above the gap -Σ α_j S_j exactly. In a mode, the library InP's
measured edge, InP of its compound alone of Ep with and without the enhancement; given per
carrier exact; above InP's gap (900 nm) the bleaching no more than the medium's κ 1e-4 absorbs,
all of it with κ 0.2.

**InP's edge**, Aspnes and Studna's k (1983) at the longest wavelengths they give:

| E (eV) | measured α (per cm) | A √(E - Eg)/E, A 1.16e5 | Kane's Ep alone | with Sommerfeld's |
|---|---|---|---|---|
| 1.500 | 30 860 | 30 540 | 11 290 (0.37) | 19 160 (0.62) |
| 1.600 | 35 350 | 36 680 | 13 570 (0.38) | 20 680 (0.59) |
| 1.700 | 41 700 | 40 710 | 15 060 (0.36) | 21 630 (0.52) |

The fitted edge is within 4 % of each. Of 10¹⁸ electrons per cm³ at 1550 nm (300 K) band
filling makes Δn -3.92e-3 of the measured edge, -4.05e-3 of Ep with the enhancement - the two
alike, the edge's first tens of meV what counts - and -1.52e-3 of Ep alone: band filling as
large as Drude's -4.1e-3, as the review said.

**B1** against the solve: the carriers' Δneff -0.0027508 on the field with them, the mode
solved again -0.0027708 (0.7 %, the second order: the core's Δn is 4 % of its contrast); the
first order on the field without them on the other side, the two's mean within 1e-3. The
mode's loss 669.12 per m, the perturbation's 670.04: their ratio (n0 + Δn)/n0 to 1e-5 - the
solve's absorption 2(n0 + Δn)κ, the first order's 2 n0 κ. Along a sweep of V (N 1e17(1 + V)
cm⁻³): VπL from the first point as λ|V - V0|/2|Δneff - Δneff0| exactly, the same at 1 and 3 V
to 0.3 % (the field's own change), and Drude's Δn per volt times the core's Γ (index) to 3e-3.

**C1**, the review's case reproduced: a rib (6 µm window; an MQW slab of n 3.35, 0.2 µm, under a
2 µm ridge of n 3.26 on InP). Kept by the most |E|² in the MQW, one mode: n_eff 3.1667, a hybrid
of TE fraction 0.846 (Γ 0.190) - not the TE0 at 3.2267 (Γ 0.169) - and TE at 1/2 keeps it too.
TE at 0.9: the TE0. Above 3.2: the TE0 and TM0 (2.6e-4 apart), the most in the MQW of them the
TE0. Of four, the first, the most TE, the most TM, the most in the ridge chosen - and shown by
a plot's mode 0, the fields left so. A lossy substrate (κ 2e-5): the rib's two at 0.78 and 0.81
dB/cm kept below 1 dB/cm, the others (1.7, 1.9, ...) left out, two found of all it searched.
Along a sweep, each point's chosen mode alone.

**C2**: the expression x on a 10 by 7 grid is each point's x (in m, the coordinates in µm),
emw.normE the solution's there, NaN beyond the model; the .npy's header a multiple of 64, its
shape (3, 7, 10), its numbers the CSV's; the columns' blank lines; the mesh's nodes each; a
sweep's three points as (3, 1, 3, 3), the core's Δn at each Drude's.

**A5**: the 1.3Q of the 300 K fit solved to In0.722 Ga0.278 As0.605 P0.395 - on InP's lattice by
Vegard's law to 1e-9, its gap 0.95372 eV to 1e-6 (Nahory's y 0.613: within the fit's 7 meV), its
me, γ1, VBO, Ep, ac the compound's there; at 77 K its gap Varshni's. AlGaInAs 1.3 µm:
Al0.1657 Ga0.3032 In0.531 As (the fit's Al0.165 Ga0.305). Vurgaftman's at Al0.165 Ga0.305: 0.993
eV, the review's; solved in Vurgaftman's: Al0.139 Ga0.329, 0.954 eV. The fit's In0.53Ga0.47As at
0.750 eV. Above InP's gap on its lattice: said, with the gaps there (0.75 to 1.344 eV).

**A4**: a GaAs well 8 nm between Al0.3GaAs barriers 10 nm in a guide's core (the three a
Layered Region), the mode at 1.46 eV, below the edge. The wells' own spectrum there: α TE 4.59
per cm at 0, 30.4 at 50 kV/cm, Δn TE 7.44e-4 between. In the mode at 50 kV/cm (a table field),
emw.alphaqwTE is that α to 1e-6,
emw.dnqwTE that Δn to 1e-6; at 25 kV/cm the mean of 0 and 50 (linear between); -50 as 50; beyond
the table held. The mode loses 8.37 per m at 0, 55.60 at 50 kV/cm, and the perturbation's Δα
of emw.alphaqwTE is 55.59 (the well's TE absorption on its TE mode, to 2e-3) - the same, to 1e-5,
as with the stack meshed isotropically. Kerr's: the TE0 moved by ½ n³ R E² (3.59e-4) times the
core's Γ to 5e-3; none at E0.

**B2**: forty layers - every triangle in the stack half of a 50 nm cell, on the grid's lines,
6400 of them. Four layers' TE0: 3.152854501 isotropic (17 972 triangles), 3.152854685 layered
at 50 nm and two rows (4591), 3.152854520 at 25 nm and four (8107). A silicon pn junction
across two 0.2 µm layers at 0.5 V: 1.10462 mA on 1600 layered triangles (20 rows, 0.1 µm along),
1.10955 mA on 6694 isotropic ones (0.02 µm); 40, 80 and 160 rows 1.10149, 1.10071 and 1.10051 mA
(as h², toward 1.10045), isotropic at 0.01 µm (27 153 triangles) 1.10270 - the layered mesh the
nearer with a quarter of the triangles. Every cell split lower left to upper right. A circle
among them: said, meshed as any other.

**The 1D semiconductor**: the junction solved in 1D (5 nm along its line) against the same
junction solved in 2D on a layered mesh of the same 5 nm rows - uniform across, as it should
be: the currents 1.1014893 and 1.1014921 mA; at every node half a µm and more from the 2D one's
sides V within 0.8 µV, n and p within 3e-5. The same y anywhere across, the same values. On an
isotropic mesh of its own (0.07 µm) the same current, its nodes between the line's interpolated
- V linearly, the carriers in their logarithm - as the 2D solution reads at those points (V to
0.1 mV, n and p to 1e-3). Under a mesa - the n layer 4 µm wide, the p mesa 2 -: the anode's
current and the cathode's alike, the mesa's (the 2 µm stack's to 1e-9), beside the mesa its
y's values. A bias sweep (0.3, 0.5 V). Refused, and said: a step in frequency, a contact on a
side the line never reaches, a line leaving its domains.

**The 1D plot groups** (test_multiphysics_doc): a slab swept in thickness, its step choosing
the more TM of two modes (at one point at least not the first). A 1D plot group by default: one
curve against the thickness, the chosen mode's n_eff and TE fraction at each point, no axis of
modes; "all": an axis of them, each value as solved. A derived value by default: the chosen
one's at each point (two values), "all" four. A Perturbation's VpiL in a 1D graph: none at the
first point, one at the second.

## Found on the way

- **The Soref fit was already at the mode's wavelength**, as λ² from 1550 nm: the review's
  "fixed at 1550 nm" is right of its coefficients, not of its wavelength.
- **emw.n is the index as given**, without the carriers' or a field's change: the perturbation
  weighs by it (the review's n0), Γ (index) too.
- **A mode filtered to few searches far.** Kept below a loss, the search doubled till 96 modes;
  it now stops when twice as many found keep none more.
- **A structured grid's diagonals matter to the semiconductor.** Its box is a third of each
  triangle about a node, and a node where two layers meet takes their dopings as those
  triangles share it. The triangulator splits a grid's cells either way (their corners on a
  circle), so the same stack came out 11 mV different from one side to the other where its
  layers met. The Layered Region now splits every cell the one way: interior nodes balanced
  (three triangles each side), the sides still leaning, oppositely - so the 1D strip is read
  along its middle, eight columns wide.

## Open

- **The wells' table is one well's**, the qw physics' line: each point takes its response at
  the field there (a local-field approximation), not its own well's states.
- **The 1D semiconductor** is solved at rest (no small-signal or time steps), takes no Optical
  Generation, Schrödinger-Poisson or lattice temperature; what is read on the model's mesh -
  its heat, its currents at a point - is of that mesh's interpolation.
- **Kane's Ep with Sommerfeld's enhancement** is some 0.6 of InP's measured edge 0.15 to 0.35 eV
  above it (non-parabolic bands, the split-off band): the alloys without a measured edge take
  it; give `Afk` where one is known.
- **The holes' absorption** is a cross-section (σp): no intervalence band model.
- **A composition** is solved at 300 K and lattice-matched only (a strained quaternary given).
- The review's own "outside the FEM": a travelling-wave electrode.

## The triangulator

`third_party/cdt` (CDT 2.0.0, MPL 2.0): `Triangulation::unsplitEdges`, fixed edges its
refinement never splits for being encroached - a triangle whose circumcenter would encroach
one is given up on, as one by a too short edge is (`Unrefined::unsplitEdges`). Marked "Qucs-S"
in it; `README.qucs.md` says it.

## Tests

`test_fem_modulator` (new): drudeOfTheMaterialsMasses, freeCarrierAbsorption,
bandFillingAgainstItsIntegral, inpEdgeAsMeasured, bandFillingInTheMode,
perturbationAgainstTheSolve, perturbationAlongABiasSweep, modesKeptAndChosen, fieldOnAGrid,
compositionOfAGap, wellsAbsorptionOfTheField, kerrAlongEachAxis, layeredStacks,
semiconductorIn1D. `test_multiphysics_doc`: graphsOfTheChosenMode.

## Breaks

Each made alone, the new tests run:

| Break | Caught by |
|---|---|
| Drude's hole mass the heavy holes' alone | Drude |
| Drude's at 1550 nm whatever the mode's wavelength | Drude (1300 nm) |
| Drude's without the index | Drude |
| Free-carrier absorption not as λ² | absorption (1300 nm) |
| Silicon's index with absorption allowed | absorption (refused) |
| Band filling's carriers Boltzmann's | band filling's integral (degenerate) |
| Holes filled as electrons are | band filling's integral (degenerate holes)* |
| The pole's principal value left out | band filling's integral (across the gap) |
| Bleaching not held to what is absorbed | band filling in the mode (κ 1e-4) |
| Band filling's tables not over n | band filling in the mode |
| The perturbation without n | perturbation |
| VπL without its 2 | sweep |
| A sweep's phase not from its first point | sweep |
| The TE fraction at 1/2 whatever asked | modes |
| The index above not kept to | modes |
| The loss below in nepers, not dB | modes |
| The chosen mode not marked | modes |
| Mode 0 the first, not the chosen | modes |
| NumPy's header not padded | grid |
| NumPy's shape x before y | grid |
| The grid's rows short of the corner | grid |
| A lattice a part in a thousand off | composition |
| A solved alloy's gap hc/λg at every temperature | composition (77 K) |
| A solved alloy's properties of the x, y given | composition |
| The wells' Δn not from F0 | wells |
| The wells' TE and TM axes swapped | wells (the mode's loss) |
| The table's field not held beyond it | wells |
| Kerr's without ½ | Kerr |
| Kerr's not from E0 | Kerr |
| Layered triangles refined for size | layered (the grid's cells) |
| A layered region's sides split | layered |
| Layered triangles refined for angles | layered (the junction)* |
| No grid inside | layered |
| The grid smoothed | layered |
| Narrow layers sized as any boundary | layered (the triangles' count) |
| Automatic: each mode, whatever the step chose | 1D plot groups |
| A 1D plot group's modes not read | 1D plot groups |
| A 1D graph's Perturbation without its sweep | 1D plot groups (VpiL) |
| The step's choice not kept with the solution | 1D plot groups |
| Sommerfeld's enhancement left out | band filling's integral |
| The exciton's Rydberg of m0, not m_r | band filling's integral |
| bfexciton not read | band filling in the mode |
| InP without its measured edge | InP's edge |
| 1D: currents not scaled to the stack | 1D |
| 1D: read along the strip's side | 1D (its nodes) |
| 1D: the carriers linear between the line's nodes | 1D (an isotropic mesh) |
| 1D: a contact off the line allowed | 1D (refused) |
| 1D: a step in frequency allowed | 1D (refused) |
| 1D: domain features not moved onto the strip | 1D |
| Layered cells split either way | layered (the diagonals) |

50 of 50; the two marked * after the tests were made to see them - the holes' integral at 4 K
(the holes were checked against the function itself before), a junction 0.1 µm along (a
circumcentre in a thin layer encroaches on its side, so the grid's sides alone had kept it).

## Builds

The Release suite, 123 of 123 (226 s, eight at a time). Under the sanitizers (Debug, ASan,
UBSan) the tests the round touches pass, no report: test_fem_waves, anisotropy,
optoelectronics, pde, physics, engine, layout, quantum; then, its second part's, five at a time:
test_fem_materials (9 s), lumerical (205 s), test_multiphysics_doc (498 s), test_fem_modulator
(745 s), semiconductor (1284 s). On a slow build test_fem_modulator's modes are linear
(Whitney's) and its meshes coarser, and the rib, the wells and the layers are solved fewer
times (each such check marked in it). The vendored triangulator's change is in
`third_party/cdt/README.qucs.md`.
