# Mode overlap and 2D mode profiles for the mode solver

The request: a mode overlap feature for the eigenmode solver, and a good, easy way of exporting 2D mode profiles.

**What was added:**

- **Mode Overlap**, a new Derived Values node. It overlaps a mode with another mode of the study, a Gaussian beam, or a mode read from a file.
- **Mode Profile**, a new Export node. It writes a mode's six field components on a regular grid as MATLAB `.mat`, NumPy `.npz` or CSV.
- **One click:** right-click a 2D plot group of a mode analysis and choose **Export Mode Profile...** It writes the mode the plot shows. No node is needed.

The `.mat` the export writes is the file a Mode Overlap reads back. So a mode of one model, such as a fibre, can be overlapped with a mode of another, such as a waveguide facet.

## Mode Overlap (Results > Derived Values)

**The value.** It is the power coupled from the other field into the mode at a butt joint, reflections aside. This is Snyder and Love's overlap, the one Lumerical's MODE uses:

η = Re[(∫E1×H2\*·z)(∫E2×H1\*·z) / ∫E1×H1\*·z] / Re ∫E2×H2\*·z

It gives three values:

| Value | What it is |
|---|---|
| `overlap` | η, from 0 to 1 |
| `loss` | −10 log10 η, shown in dB |
| `overlapE` | The overlap of the transverse E alone, \|∫E1·E2\*\|² / (∫\|E1\|² ∫\|E2\|²) |

**What is integrated, and where:**
- The integrals run over the mode's domains with its perfectly matched layers left out, at the solution's own quadrature points.
- The other field's flux ∫E2×H2\* is its own: over its whole grid for a file, analytic for a Gaussian. Light of it that falls outside the cross-section counts as lost.

**The other field (`with`):**
- **mode:** another mode of the study (`other`, from 1), at the same frequency and sweep point.
- **gaussian:** a Gaussian beam at its waist. E = exp(−r²/w0²), along x or y (`polarization`). H is a plane wave's in its medium, (n/η0) z×E. Settings:
  - `waist`: w0, the 1/e field radius;
  - `centre`;
  - `index`: n; empty means the mode's neff.
- **file:** a mode profile `.mat`, its mode `filemode` (from 1). A relative path is taken from the model's folder.
- **`offset`:** moves the other field, which is evaluated at x − offset. This gives misalignment losses.

**Each mode, or the chosen one:** `solutions`, as for the other derived values.

## Mode Profile (Results > Export) and Export Mode Profile...

**The grid.**
- `from` and `to` set its corners, in the geometry's unit.
- When both are empty, the grid covers the mode's domains with the PML left out.
- `nx` and `ny` set the points along each side, 201 by default.
- Outside the mode's domains and in the PML the fields are 0.

**The fields.** All six components are complex:
- E in V/m, H in A/m.
- The mode is at 1 W, as the solver normalizes it.
- The phase is as found: E e^(j(ωt − βz)).

| Format | Layout | Variables |
|---|---|---|
| `.mat` (MATLAB 5, uncompressed) | `Ex(ix, iy)`: `[nx ny]`, or `[nx ny M]` for M modes | `x`, `y` (m, columns); `Ex` … `Hz`; `neff` (complex: β/k0 = neff − j·neff_im); `lambda0` (m); `mode`; `power` (W, ½ Re ∫E×H\*·z over the grid); `about` (text) |
| `.npz` | `Ex[iy, ix]`: `(ny, nx)`, or `(M, ny, nx)` | the same |
| `.csv` | one row per point and mode | `#` lines with each mode's neff, λ0 and power, then: mode, x, y, and the real and imaginary parts of each component |

**Reading them elsewhere:**
- **MATLAB or Octave:** `load wire_mode1.mat; imagesc(x, y, abs(Ex).')`.
- **Python:** `scipy.io.loadmat`, or `numpy.load` for the `.npz`.
- **Lumerical:** `matlabload("wire_mode1.mat")`. This was not tried in Lumerical itself.

**Export Mode Profile...** is on a 2D plot group's menu when the model has a mode analysis.
- It suggests `<model>_mode<N>.mat`. Choosing `.npz` or `.csv` in the dialog gives those formats.
- It writes the mode the plot group shows: its study, sweep point, frequency and mode.
- The message line says what was written. For example: modes 2 (neff 1.77039, 0.99 W within) of wg on a 201 by 201 grid from (−1.5e−06, −1.5e−06) to (1.5e−06, 1.5e−06) m: x, y, Ex... Hz complex of dimensions [201 201] (x the first) ...

**Claude's tools:**
- `fem_export` runs a Mode Profile node.
- For a `.mat` or `.npz`, its `begins` field returns that summary rather than the file's binary bytes.

**Reading a `.mat` back** (Mode Overlap, `file`):
- MATLAB 5 files without compression are read. That covers what the export writes, SciPy's `savemat`, and MATLAB's `save -v6`.
  - Any numeric storage type is accepted, since MATLAB compacts small integers.
  - The fields may be real.
- A file needs `x`, `y`, `Ex` and `Ey` at least.
- When H is missing, it is made a plane wave's of the file's `neff` (1 when absent).
- Each field must be `[nx ny]`. On a grid that is not square, `[ny nx]` is taken and turned round; that is how SciPy saves a NumPy `(ny, nx)` array.
- MATLAB's default `save` compresses. Such a file is refused with a message to save it with `-v6`. HDF5 files (`-v7.3`) are not read.

## Also fixed

**Confinement and Perturbation**, the older mode derived values, took their physics as "emw" when none was named. A model whose mode analysis had another tag failed with "there is no mode analysis emw solved".

They now share one rule with the two new nodes:
- If the named physics is a mode analysis of the solution, it is used.
- If none was named, the solution's first mode analysis is used.

## Checks

The tests are `test_fem_modeoverlap` (7 functions, 2 s) and `test_multiphysics_doc`'s `aModesProfileExportedAndOverlapped` (0.5 s).

**Under the sanitizers** (Debug and ASan, with Eigen's sparse LU as on CI's Linux runners), the tests were first far too slow:
- `test_fem_modeoverlap` took 430 s;
- the interface test went past QtTest's 300 s limit for one function, on a single solve of its wire with PML.

Nearly all of the time went to the mode solves' LU factorizations. Now:
- The wire and the fibre are each solved once and shared by the functions that use them.
- On the sanitizers' builds the elements are first order. The 2 µm splice's bound there is 2 % rather than 1 %, and the profile self-overlap's 0.97 rather than 0.99.
- The interface test's wire is coarse and first order on every build.

The two tests now take 38 s (no function over 12.4 s) and 18 s, with no sanitizer reports.

| Check | Result |
|---|---|
| A silicon wire's TE0 with itself | 1.000000000000, and E alone 1.000000000000 |
| TE0 with TM0, and with mode 3 | 7e-31 and 4e-24 |
| TM0 with TE0 (the other way round) | the same 7e-31 |
| TE0 against itself moved 100 nm right, then left | 0.836023 and 0.836383 |
| A lossy wire's TE0 (core κ 0.1, 3.8e4 dB/cm; complex fields) with itself | 1.000000000000 |
| A step-index fibre (a = 4.1 µm, V 2.135) against Marcuse's Gaussian (w = 4.917 µm) | x-polarized, into the HE11 pair: 0.60208 + 0.39179 = 0.99388; y-polarized: the same 0.99388; `overlapE` 0.993909 (weakly guiding, so nearly η) |
| The fibre against itself moved 2 µm and 3 µm, against Marcuse's exp(−d²/w²) | 0.84389 (0.84754) and 0.68551 (0.68922) |
| A Gaussian centred 2 µm right and moved 2 µm left | the unmoved value to 8 digits |
| The wire's three modes exported to `.mat` (121 by 101) and read back | every component equal to the solution's at the grid points; neff and λ0 exact |
| Power within the grid by the trapezoid rule | 0.999, 1.017 and 1.007 W (E jumps at the core's sides) |
| The wire's TE0 against its own profile read from that file | 0.9939 (the grid's interpolation of E's jump) |
| The fibre against a Gaussian from a file with E and H | 0.992910 (built-in: 0.993909, 161 by 141 grid) |
| The fibre against the same Gaussian from a hand-written MATLAB-style file: E alone, real, `[ny nx]` | 0.992910 (the same to 12 digits) |
| The interface test, on a wire with PML | the menu action is there; the plot group's mode 2 is written and read back with neff equal to `fem_evaluate`'s; the grid spans the window, not the PML; `fem_export`'s `begins` is the summary; the Gaussian overlap is 0.463 for TE0 and 4e-7 for the cross-polarized TM0 (a coarse first-order mesh; 2e-13 on a fine second-order one) |

**Outside Qucs-S** (SciPy 1.18.1, NumPy 2.4.6). The wire's two modes were exported over `qucs-s --mcp-server` on a 61 by 41 grid, in all three formats:
- `scipy.io.loadmat` reads every variable. `Ex` is (61, 41, 2), that is [nx ny M]; `x` is (61, 1); `neff`, `lambda0`, `mode` and `power` are (1, 2).
- `numpy.load` reads every array. `Ex` is (2, 41, 61).
- The `.mat`'s `Ex[:, :, m]` equals the `.npz`'s `Ex[m].T` exactly, for all six components.
- The CSV's rows equal both to 3e-12, which is its 12 printed digits.
- NumPy's own trapezoid ½ Re ∫E×H\*·z gives 1.0056580089 and 0.9874026599 W, the file's `power` to every digit.

## Break checks

Each break was applied, built and run, then the source restored:

| Break | Caught by |
|---|---|
| The overlap normalized by the mode's flux, not the other's | `aFibreAgainstMarcuse`: η 7e-14 |
| A y-polarized Gaussian's H with the x's sign | `aFibreAgainstMarcuse`: η −0.99 |
| The Gaussian's offset added, not taken | `aFibreAgainstMarcuse`: centre and offset 48 % apart |
| `overlapE` normalized by the mode's E twice | `aFibreAgainstMarcuse`: 7e-14 |
| The other mode's snapshot not restored | `modesWithEachOther`: the selection |
| The mode's H not conjugated in ∫E2×H1\* | `aLossyModeWithItself`: 0.9937 (a lossless mode's fields are real: no test without loss could see it) |
| The `.mat` dimensions written [ny nx] | `aProfileExportedAndReadBack`: read back turned, 6.8e7 off |
| The `.npz` shape (nx, ny) | `aProfileExportedAndReadBack` |
| The reader not turning a [ny nx] file | `aGaussianFromAFile` |
| The reader without a plane wave's H | `aGaussianFromAFile` |
| The reader's mode its place in the file | `aModesProfileExportedAndOverlapped`: mode 1 for 2 |
| The export's grid over the PML | `aModesProfileExportedAndOverlapped`: from −2 µm |
| No fallback to the first mode analysis | `aModesProfileExportedAndOverlapped`: "no mode analysis emw" |
| `fem_export`'s `begins` the file's bytes | `aModesProfileExportedAndOverlapped` |
| The menu action unnamed | `aModesProfileExportedAndOverlapped` |

At first `overlapE` and the conjugate were not caught. The two checks that catch them were then added: `overlapE` against η for the fibre, and the lossy wire.

## Not done

- **The offset's direction for a mode is not checked by itself.** The test guides are symmetric, so moving right and left give the same value. The Gaussian's offset is checked against its centre, and the file and mode branches use the same x − offset.
- **The PML's exclusion from the overlap integrals is not tested by itself.** The fields there are small. The export's grid leaving the PML out is tested.
- **Fresnel reflection is left out at the joint**, as in Lumerical's overlap.
- **Only `.mat` profiles are read back**, not `.npz` or CSV.
