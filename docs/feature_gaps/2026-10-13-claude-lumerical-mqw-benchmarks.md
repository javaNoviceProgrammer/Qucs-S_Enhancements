# Feature gaps: Lumerical's MQW benchmarks, the review's items against now

*13 October 2026 - on `8f4ca6c` (the Newton residual check, after v27.0.0).*

The review rebuilt Lumerical's three MQW Standalone benchmarks in Qucs-S from the
example package's decoded inputs:

- **Benchmark 1** (Ahn, Chuang and Chang 1988, 4x4) was reproduced.
- **Benchmark 2** (Chuang and Chang 1997, wurtzite 6x6) was not possible: there was no
  wurtzite.
- **Benchmark 3** (De Franceschi, Jancu and Beltram 1999, 8x8) was compared with the 6x6,
  as there was no 8x8.

It found two small issues and listed three wishes. All five are done, in four commits:

| Commit | What |
|---|---|
| `e440e7a` | The small issues: a dataset's units, the Lorentzian tails under the gain |
| `bc6641a` | The 8x8 k·p (Kane's) |
| `2944d1d` | Wurtzite: the nitrides, the 6x6, strain, polarization and its screening |
| `4273d6a` | The Lumerical MQW importer (.ldev) and .ldf data files; the 8x8's and wurtzite's tests in binaries of their own |

All three benchmarks now run like for like. Lumerical's own three projects import in one
step and solve as the benchmarks below.

## The review's items against now

| Review | Now |
|---|---|
| **1. Mixed dataset units.** A 1D plot group's dataset had energies in eV but its `k` axis in 1/m. "Either write both in display units or both in SI, and say which in the dataset." | **Done: display units, said.** Each variable is written as its graph shows it: `k` in 1/nm (as `qw.k`), photon energies in eV, a swept field in V/m, a trace in its graph's unit. The dataset's import line carries a `"units"` map (none for a number). Its display's x axis is labelled "k (1/nm)". |
| **2. States and the line tail.** `states` moved a Lorentzian spectrum's peak by about 25 %. A note in the help, or a warning when the tail carries more than x % of the absorption into the gain region. | **Both.** `qw.alphaTEfar`, `qw.alphaTMfar` give, at each energy, what the transitions more than 5 half widths above it add by their tails alone. The log warns when they take more than 10 % off the gain at its peak, with how much. The review's case reproduces (TE peak 394 /cm with 2 states, 304 with 4): 18 % with 2 states, 36 % with 4, none with a Gaussian line. The help of `states` and of the line shape, the README and the agent reference say so. See "The line's tails". |
| **Wish 1. Wurtzite 6x6** ("unblocks benchmark 2"). | **Done**, with what the proposal asked: the nitride library, Chuang and Chang's 6x6, strain (D1-D6, a1, a2), the electron's mass across c, TM's Ep∥, and polarization in both routes (the wells' own field with the carriers' screening; a semiconductor interface charge). Benchmark 2: the first five subbands within 1.5 meV. See "Wurtzite". |
| **Wish 2. 8x8 zinc blende** ("benchmark 3 like for like; narrow gaps"). | **Done.** `"valence": "kp8"`: Kane's 8x8, the electrons non-parabolic, Foreman's remote terms. Benchmark 3 within 0.4, 0.4 and 1.3 meV of the tight binding. See "The 8x8". |
| **Wish 3. "Import Lumerical MQW".** | **Done.** *File → Import Lumerical MQW Project…* and Claude's `fem_model` `import` turn an `.ldev` into a `.qfem`, through an HDF5 reader of its own. `.ldf` data files import as datasets. See "The importer". |

## The benchmarks now

| # | Benchmark | Lumerical | Qucs-S now |
|---|---|---|---|
| 1 | Ahn 1988, 4x4, 50 kV/cm | bands, gain | As the review had it (bands to 0.45 meV rms of Lumerical's). Imported from `mqw_4x4.ldev`: at k = 0 0, −16.42, −26.92, −60.94 meV, the review's model's to the digit; gain peaks TE 393, TM 318 /cm on 801 energies (Lumerical's own 201 sample TM at 303) |
| 2 | Chuang and Chang 1997, wurtzite 6x6 | bands | **Now possible.** One 6 nm well as Lumerical's file has it: subbands 1-5 within 0.3, 0.4, 0.6, 0.8 and 1.5 meV along k (6 within 1.4, 7 within 2.8; 8 and 9, near the barriers' edge, about 5). Imported from `mqw_6x6.ldev` (three wells): the first five within 1.4 meV |
| 3 | De Franceschi 1999, 8x8 | bands | **Now like for like:** 0.4, 0.4 and 1.3 meV (the 6x6's 0.3, 0.3, 1.3). Imported from `mqw_8x8.ldev`: the same |

## The line's tails (item 2)

- **Measure.** At each energy, the transitions whose least energy lies more than 5 half
  widths above it reach it by their line's tail alone. A Lorentzian's 1/x² tail does; a
  Gaussian's or a sech's hardly does. Their part of α is `qw.alphaTEfar` and
  `qw.alphaTMfar`.
- **Warning.** With carriers and a Lorentzian line, at the gain's peak in either
  polarization: if those transitions take more than 10 % off the gain (of the gain without
  them), the log says so, the polarization, the peak's energy and how much, and advises
  a Gaussian or sech line or the states that matter (Lumerical keeps 2 conduction and 4
  valence subbands: `states` 2).
- **Why the peak moves.** From 2 states to 4, a GaAs well's peak falls for two reasons:
  - the carriers spread over more states (a Gaussian line's peak falls 7 % too);
  - the tails.
  The test shows the tails account for 77-89 % of the Lorentzian's extra fall. The rest is
  the tails of new transitions nearer than 5 half widths.

## The 8x8 (wish 2)

- **Model.** S↑ S↓ coupled to X, Y, Z by i P kα (P = √(Ep ħ²/2m0)). The remote bands
  leave γ1 − Ep/3Eg, γ2,3 − Ep/6Eg and A_c = 1/me − (Ep/3)(2/Eg + 1/(Eg + Δ)) (Foreman).
- **Spurious states.** With A_c below 0 they appear in the gap. With A_c at 0 (Foreman's
  usual choice) S has no stiffness: the first try found a spurious e1 at Ec, flat in k,
  because the elements' first derivatives leave S vectors coupled to nothing. Ep is now
  lowered where A_c would fall below a free electron's 1 (Foreman's other choice), said in
  the log: GaAs's 25.7 eV becomes 21.2.
- **Operator order.** The S-Z term is in Burt and Foreman's order, i P kz in S's row and
  −i kz P in Z's (the S band's part of N+). Symmetrized, De Franceschi's light-hole
  subband sat 3.1 meV off; in this order it is 0.4.
- **Electrons.** In a spectrum the electrons are the 8x8's: their energies, in-plane
  masses and densities (the Fermi level over their own bands), and their transitions from
  the spinors, <c|p|v> with both S parts. Excitons are left out, said.

| Check | Result |
|---|---|
| A 50 nm box of a material whose remote terms are known (A_c = 1, γ1' = 1, γ2' = γ3' = 0), its electron along k against Kane's bulk cubic at k² + kz² | 9 µeV at 0.6 /nm, where a parabola is 26 meV off |
| Its heavy holes at k = 0 against the 6x6's | 1e-6 eV |
| Its electron's in-plane mass against me | 0.5 % |
| Benchmark 3 | 0.4, 0.4, 1.3 meV |
| Gain, unbroadened | crosses 0 at the quasi-Fermi levels' separation (1.6146 eV both) |
| Electron density at 3e18 | Fc 5.3 meV below parabolas' of the same masses (the bands heavier as they rise) |
| The e1-hh1 step against the 6x6's | 1.086 times, its reduced masses' 1.114: the S part's 0.974 |

## Wurtzite (wish 1)

- **Materials.**
  - GaN, AlN, InN, AlGaN, InGaN and InAlN are compounds, with Vurgaftman and Meyer's
    (2003) parameters. Lumerical's stock GaN, AlN and InN, read from the example's
    project file, carry the same A1-A6, D1-D6, a1, a2, Δ's and lattices.
  - The gaps bow by 0.8, 1.4 and 2.5 eV; Psp is bowed (Bernardini and Fiorentini); e31 and
    e33 are Bernardini, Fiorentini and Vanderbilt's.
  - The library's "Gallium nitride" now has its compound, and five nitride entries are
    added.
  - A user material with `"crystal": "wurtzite"` takes all of these as given; its Ev is
    the k·p reference (Chuang and Chang's, as Lumerical's layer table).
- **The 6x6.** Chuang and Chang's, in the X, Y, Z basis (the proposal's coefficients); the
  spin-orbit term is Δso/3 σ·L.
  - N2 is split for Burt and Foreman's order: N2− is the geometric mean of what the E1
    remote bands give M2 and M3, which is zinc blende's M − ħ²/2m0 when the two are one.
    Symmetrized, a GaN well between AlN barriers grows a spurious band, 72 meV above its
    top by 4 /nm; in this order its top band only falls.
  - Strain: λε, θε on the bands and a1 εzz + 2 a2 εxx on the conduction band; εzz is
    −2 C13/C33 εxx.
  - The electron's mass across c and TM's Ep∥ are their own. One crystal per stack.
- **Polarization.**
  - `"polarization"`: periodic, or no field at the ends; metal or nitrogen face. Each
    layer's field is (D − P)/ε0εr from the interfaces' bound charges.
  - A spectrum's carriers screen it in a loop of the states and their charge (the charge
    mixed, to 0.1 meV).
  - The semiconductor's **Polarization Charge** puts −ΔP·n at interfaces between nitrides.

| Check | Result |
|---|---|
| A strained GaN box at k = 0 against Chuang and Chang's closed forms (heavy holes alone; light and crystal-field holes the 2x2), and its electron | 1e-6 eV |
| Benchmark 2 | 1.5 meV (the first five subbands; the test's three points each 1.3) |
| The c-plane's isotropy: along [110] against [100] | 1e-9 eV |
| Al0.3Ga0.7N, In0.2Ga0.8N gaps at 300 K | 4.086, 2.677 eV (measured near 4.05-4.10, 2.7) |
| A relaxed GaN box's top against the library's Ev | 1e-5 eV |
| GaN's edge, AlN's (its crystal field below 0) | TE 27204 against TM 517 /cm; TM 7601 against TE 11 |
| TM with Ep∥ twice Ep; the step with the electron's mass across c 0.4 | TM ×2.000000, TE unchanged; the step as the reduced masses to 1e-3 |
| A periodic GaN/Al0.3Ga0.7N stack's well field, and with no field at the ends | 2.12 and 2.63 MV/cm, the closed forms (Pb − Pw) Lb/(εw Lb + εb Lw) and (Pb − Pw)/εw to 1e-6 |
| Screened: 1e19 and 4e19 cm⁻³ | 1.79 and 1.03 MV/cm, e1-hh1 3.231 → 3.298 → 3.435 eV, in 13-15 turns (parabolic holes and the 6x6 alike) |
| An Al0.3Ga0.7N barrier on GaN (the semiconductor) | σ 0.024 C/m² (Ambacher et al.'s 1.5-1.7e13 q/cm²), the same solution as a Surface Charge of it, a 2DEG of 2.6e19 cm⁻³ under it; the nitrogen face's none |

## The importer (wish 3)

- **Reading.** The `.ldev` is HDF5. Its reader (`fem_hdf5`) is new, Qt Core only, and
  reads what Lumerical's files use:
  - a version 2 superblock and version 2 object headers;
  - dense links and attributes: fractal heaps with direct and indirect blocks, v2
    B-trees with internal nodes;
  - contiguous datasets.
  The MQW object's "Properties" blob and its layer table (a Qt QDataStream dictionary) are
  parsed as the review's decoders found them. Materials are found by name; their models'
  GUID groups by their "Name".
- **The model.**
  - Its layer table becomes a stack, bottom first.
  - Each layer is a Quantum Well Material of its own: its material's gap at the
    temperature (Varshni's when that model is active) over the layer's valence band
    offset; me; the γ's (zinc blende) or A1-A6, Δ's, D's, a1, a2 (wurtzite); Ep; and its
    strain. A strained zinc-blende layer takes its ac, av, b, C11, C12: user materials of
    zinc blende now take a strain too.
  - The physics takes the k·p order (4x4, 6x6, 8x8), the temperature, the field and the
    grid.
  - The studies are valence bands (k to the Brillouin zone ratio of π/a, the well's a; as
    many states as Lumerical's eigenvalues) and a spectrum: its frequencies as energies,
    Lumerical's γ as a full width (half of it here, Lorentzian), its carrier density over
    the stack per the wells. Plot groups of the bands and the gain are added.
  - The notes say what was read and how it was taken.
- **Data files.** `.ldf` files import as datasets: each variable's curves (name_x1, name1,
  ...), an n x m matrix's columns on its first.

| Check | Result |
|---|---|
| A synthetic project (`tests/data/lumerical_sample.py`, h5py's newest layout: 100 links, 70 attributes - two-level B-trees, indirect heap blocks) | every value read, the model's fields as computed |
| Lumerical's `mqw_4x4/6x6/8x8.ldev` (not in the repository) | imported and solved: the benchmarks above |
| Not a project; a model there already | said; written over only with `replace` |

## Tests and breaks

| Part | New tests | Broken on purpose | Caught |
|---|---|---|---|
| Small issues | the wells' dataset units (`test_multiphysics_doc`); `lorentzianTailsUnderTheGain` (`test_fem_quantum`) | 16 | 16 |
| The 8x8 | `test_fem_kp8`: `kp8AgainstKane`, `kp8ElectronsMass`, `kp8AgainstTightBinding`, `kp8Gain`, `kp8EdgeStep` | 13 | 11 (one not caught; one's code removed) |
| Wurtzite | `test_fem_nitrides`: `wurtziteAtTheZone`, `chuangAndChang97`, `wurtziteInterfaces`, `nitrides`, `wurtziteOptics`, `polarizationField`, `polarizationScreened`; `wurtzitePolarizationCharge` (`test_fem_semiconductor`) | 19 | 19 |
| The importer | `test_fem_nitrides`: `hdf5`, `lumericalProjects`; `lumericalDataIsRead`, `claudeImportsLumerical` | 15 | 14 (one's code removed) |

The 8x8's and wurtzite's tests have binaries of their own (`test_fem_kp8`,
`test_fem_nitrides`; the helpers shared in `fem_quantum_testing.h`). Under the
sanitizers they would have added some 25 minutes to `test_fem_quantum`, past its 1800 s.
(CI's Linux job runs each binary as one ctest entry; at 1381057 `test_fem_quantum` took
1119 s there, 1252 s on this machine's sanitized build.) There, the
heaviest are lightened: coarser lines, fewer energies and values of k. Three are left
out, because Eigen's complex solves run unoptimized there: the 8x8 spectrum's mass, the
edge step's two spectra, and the [110] rerun. The macOS job's Release ctest runs them.

Not caught:

- **The valence state's S-part term** in the 8x8's momentum element (kept). It is an
  O(θ²) correction: 2 % at 0.3 /nm in GaAs, below these tests' reach. It matters in
  narrow gaps, where no reference was at hand.
- **A tail past the 8x8's k grid**, for the electrons' density. It is never reached: the
  grid's reach is the holes', at 0.5 m0, far past any electron level. Removed.
- **The fractal heap's tiny objects.** Lumerical's files have none (0 of more than 2000).
  Removed.

Two wurtzite breaks first passed: N2 symmetrized, and N1 dropped (zero along [100]). They
led to `wurtziteInterfaces` and the isotropy check, which catch them.

## Under the sanitizers, ctest, the scenarios

| Binary | Sanitized here | Its functions |
|---|---|---|
| `test_fem_kp8` | 619 s (1800 s allowed) | Kane 44 s, the tight binding 174, the gain 202; the mass and the edge's spectra left out |
| `test_fem_nitrides` | 582 s (1800) | the zone 12, Chuang and Chang 165, the interfaces 33, the library 210, the optics 136, the fields 7, the screening 21, the HDF5 0, the projects 75 |
| `test_fem_quantum` | 1252 s (1800) | as at 1381057 (1119 s on CI's runner then), with the tails' 26 s |

The others: `wurtzitePolarizationCharge` 86 s, `lumericalDataIsRead` 3 s,
`claudeImportsLumerical` 30 s, `claudesToolsShowTheWellsInEV` 108 s. Every function is
under QtTest's 300 s, and the sanitizers report nothing.

The full ctest (Release):
- With the importer: 116 of 116 in 228 s.
- With the split binaries: 117 of 118. `test_python_tools`'s debugger check ("total 6",
  its output's timing under `-j6`) failed once; it passed alone three times and in the
  run before.

The end-to-end scenarios (`scripts/mcp-e2e-scenarios.py`, the build's app): 97 checks,
none failed.

The DMG, packaged from 4273d6a, passed these checks:
- `--version` names the commit; `hdiutil verify`; no Homebrew links (`otool -L`);
  `codesign --verify`;
- the smoke test (a schematic to an ngspice netlist);
- the twelve MCP checks of earlier rounds (`axis`, `fem`, `fem2`, `image`, `mcp`, `opto`,
  `pde`, `qw`, `review`, `semi`, `sweep2`, `tcad`): all OK;
- this round's check: the packaged app imports both sample projects through
  `fem_model`, solves their bands, and screens a GaN well's polarization field (1.03 MV/cm
  at 4e19 cm⁻³, in 13 turns).

## Also found: CI's Linux job was red before this round

CI's Linux job (Debug, ASan) failed at 1381057 and at v27.0.0 (408f07e). These failures
are on Linux only: macOS's Release and this machine's sanitized build pass them all.
- `test_fem_optoelectronics`: `aPhotodiodesStepResponse`, S against its rest 0.9876 (1e-3
  allowed).
- `test_fem_devices`:
  - `aSweepStartsFromTheLast`: "1 of 615 edges not Delaunay (their currents left out)";
  - `fowlerNordheim`: 4 % off;
  - `exampleFowlerNordheim`: 7 % off;
  - `anAvalancheGain`: its broken solve's error not "no solution found".

The last green run on main was 1b71490 (12 October). Linux's mesh (one edge not Delaunay)
looks like the common thread. Reproducing it needs a Linux run (a ci/ branch). It is
left for the next round, not changed here.

## The Newton task's check

8f4ca6c's per-function timings under the sanitizers finished: 89 functions of the
semiconductor, device, optoelectronics and quantum tests, all passing with no reports,
the slowest 198 s (under QtTest's 300 s).
