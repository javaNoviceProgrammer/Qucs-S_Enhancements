# Feature gaps: anisotropic materials

*14 October 2026 — on `ce8d2b0`. The review: `anisotropic_support.md`, 12 October, on
`29b7e8e`, found while modelling a thin-film lithium niobate (TFLN) modulator and a graphene
electro-absorption modulator (`~/QucsWorkspace/tfln_modulator`, `graphene_modulator`).*

The review found anisotropy in two places only: the mode solver's diagonal n or ε, and the
custom PDE's 2×2 diffusion. Electrostatics, currents, heat, solid mechanics and magnetics
took scalars, so the TFLN modulator's electrostatics went through a `pde`, its RF capacitance
was corrected by hand, and a graphene layer absorbed through its normal field.

Every physics now takes a tensor where its material has one, and a crystal's own tensors are
turned into the model's axes by the material's orientation. The library has the crystals the
review named.

## The review's recommendations

| Review | Now |
|---|---|
| 1. Electrostatics tensor (diagonal + xy); `lineparams` G following it | **Done.** A material's εr, σ, k may be a tensor: `"44; 44; 28"` along x, y, z, six items (xx; yy; zz; yz; xz; xy) or nine (row by row) - a crystal's in its own axes, turned by its orientation. Electrostatics, currents and heat take its in-plane 2×2 part (a feature's own too: "anisotropy" diagonal or symmetric, epsxx, epsyy, epsxy; sigmaxx...; kxx...). es.Dx, es.Dy, es.We, ec.Jx, ec.Qrh, ht.qx follow the tensor; es.epsrxx... show it. A line's G is ω ε0 E·(εr tan δ)E + E·σE with both tensors, tan δ along the axes where the material gives three. |
| 2. Per-axis loss in `modes`; a surface conductivity boundary | **Done.** kappa, tand, sigma one value or three along x, y, z (a sheet as a thin layer: σ; 0; σ). **Conductive Sheet**, a boundary feature of `modes`: -n × (H1 - H2) = σs Et on an interior boundary, graphene's σs (Kubo's, Falkovsky's form: the Drude part and the interband one, of µc, τ, T at the mode's frequency) or one given (sigmas, sigmas_im). emw.Psheet, emw.loss_sheet: what the sheets absorb. |
| 3. Material orientation (Euler, cut presets); crystal library entries with ε, n (Sellmeier), r_ij, d_ij and stiffness | **Done.** A material's "orientation": none, cut ("cut" the crystal's axis up - y, a wafer's normal -, "along" its axis along z) or euler (euler_phi, euler_theta, euler_psi, z-x-z, degrees). Every physics turns the material's tensors: rank 2 as R T Rᵀ, stiffness, photoelastic and the piezoelectric and electro-optic ones by Auld's Bond matrix. **The library:** lithium niobate (replaced: a crystal now) and lithium tantalate, x-cut and y-propagating; KTP; quartz; sapphire, c-plane; YIG and Ce:YIG (their gyration); silicon (its cubic stiffness and photoelastic constants) and GaAs and InP (r41), a (100) wafer's axes. d_ij are the piezoelectric strain constants, e = d C^E taken from them. |
| 4. Full ε tensor in `modes`: xz, yz terms, the Pockels tensor, magneto-optic | **Done.** ε's xz and yz couple Et and Ez: the modes are then found by β - (K0 + β K1 + β² K2) x = 0 in the uncoupled problem's variables (β Et, -j Ez): K0 its A, K2 its B, K1 the coupling, j C_tz - j C_zt (ε's tz and zt). So it is free of spurious modes as the uncoupled A x = -β² B x is - its extra eigenvalues at β = 0, none at infinity. Linearized, shifted and inverted in t = β/σ (its halves alike in size); the forward modes kept. The TFLN arm's coupled solve 8.5 s, its uncoupled one 8.1. **Electro-optic:** Δ(1/ε) = r E in the crystal's axes, every r_ij and E's three components (es.Ex, es.Ey by default), ε = (ε⁻¹ + Δη)⁻¹. **Photoelastic:** Δ(1/ε) = p S, S a solid mechanics' strain. **Magneto-optic:** ε_ij + j e_ijk g_k, g along the magnetization (x, y or z). The PML stretches the full tensor. Waves in the plane (`waves`, Hz) take ε's in-plane tensor. |
| 5. Heat, solid and magnetic tensors | **Done.** Heat: k's tensor. Solid mechanics: a material's stiffness C (or "anisotropy": anisotropic with C on the feature) in plane strain and about an axis, plane stress condensed (D = Cii - Cio Coo⁻¹ Coi); thermal expansion along the crystal's axes. Magnetics: µr's in-plane tensor (ν its inverse), a laminated core's. |
| 6. Piezoelectric (solid ↔ es) and photoelastic coupling | **Done, at rest.** "Piezoelectric Effect", a solid feature naming an electrostatics: σ = C S - eᵀE, D = εS E + e S, the two physics solved in turn till they agree; plane strain or about an axis (plane stress refused: its condensation would have to take e's in too, not done). Photoelastic: item 4. **Not:** the solid has no inertia - no frequency domain, no eigenfrequencies - so SAW and BAW resonators, acousto-optics and a modulator's piezoelectric resonances are still out of reach. |

The review's `waves` row ("presumably accepted, not tested") was right to doubt: a waves
port and a scattering boundary took a medium as isotropic (its εzz), so a port in a diagonal
crystal solved the wrong mode. A port's line problem now takes the medium along it (tᵀPt)
and across it (nᵀPn), and the scattering boundary across it. A port in a crystal turned in
the plane (nᵀPt not nought) is refused: its mode there is a tilted one, which a port's single
profile cannot be.

## The review's benchmarks

**The TFLN arm** (`tfln_mzm_arm.qfem`, copied, at G = 5 µm):

| Model | Δneff/V |
|---|---|
| The review's `pde` (εxx 28, εyy 44) and n_x = ne - ½ ne³ r33 Ex | 1.7225e-5 |
| `es` with the library's lithium niobate (x-cut: εxx 28, εyy 44), the same n_x | **1.7225e-5**, the same to ten digits |
| `es` and the wave feature's material Pockels effect, r33 alone | 1.71596e-5 = 1.7225e-5 × 30.8/30.9 (the library's r33) × (2.1376/2.138)³ (its ne) |
| the same with every r_ij | 1.7703e-5: r13 and r51 add 3.2 % - r51 with the field's vertical part in the rib |

**The RF line** (`tfln_cpw_rf.qfem`, at 10 GHz): `lineparams` gives C = **140.83 pF/m** with
the library's lithium niobate, the review's 140.8 by hand; with its εr 28 as before, 139.50.
G 313.4 µS/m (308.0), Z0 53.86 Ω (54.11), εeff 5.170 (5.121).

**The graphene modulator** (`graphene_eam_modes.qfem`, Ef = 0):

| Graphene as | TE0 loss (dB/m) | TM0 |
|---|---|---|
| a 1 nm layer, σ every way (the review's) | 44178.8 | 106706.5 |
| the layer, σ in its plane alone | 44121.9 (-0.129 %) | 103534.5 (-2.97 %) |
| a sheet, the model's own Kubo terms, mid-layer | 44112.3 | 103532.8 |
| a sheet, the built-in graphene (µc 0, τ 50 fs, 300 K) | 44128.4 | 103568.9 |

The review expected -0.13 % and -2.6 % (its TM figure the normal field's share of |E|² in
the sheet, not the loss). The sheet and the in-plane layer agree to 2e-4 and 2e-5.

**z-cut against x-cut** (the review's new benchmark; `zCutAgainstXCut`): a lithium niobate
core (the library's) between plates 4 µm apart - z-cut, the plates above and below, its TM
mode - and the same turned 90° in the plane, x-cut, its TE mode: -6.4794e-6 and -6.4797e-6
per volt, the same VπL to 5e-5.

## Checked

- **A tilted uniaxial slab** (no 2.2, ne 2.6, its axis 45° from y toward z): TM0 1.7322417,
  its layers' 4×4 transfer matrix 1.7322417 (without the coupling 1.7366608).
- **A garnet on a slab** (Ce:YIG's g, magnetized along x): the non-reciprocal phase shift
  -3.292e-4 against the layers' -3.309e-4 (the oxide's 3 µm truncate the weakly guided mode
  alike, so the shifts from g = 0 are compared).
- **Pockels and photoelastic shifts** of a slab's TE mode: 1.9942127 at 10 V/µm (the transfer
  matrix's 1.9942128), strained 1.9505412 (1.9505412).
- **A sheet across WR-90** at 10 GHz: neff 0.732089 and α 21.5732 Np/m, its transverse
  resonance's to the digits shown; emw.Psheet its whole loss.
- **Graphene's conductivity:** undoped e²/4ħ at 1550 nm; Pauli-blocked at µc 0.5 eV; the
  interband reactance's T → 0 log; Drude's at 1 THz.
- **Silicon's stiffness:** E 130.13 GPa along [100], 169.10 along [110] (1/S11).
- **An AlN plate between electrodes:** its top moved -e33 V/C33 to 1e-6, its free and
  clamped capacitances; its strain constants given instead (d33 5.53 pC/N), the same.
- **The library's e = d C:** lithium niobate's e15 3.778, e22 2.497, e31 0.232, e33 1.343
  C/m² (Weis and Gaylord's measured 3.7, 2.5, 0.2, 1.3); quartz's e11 0.1712, e14 -0.0406
  (Bechmann's 0.171, -0.0406).
- **A sheared crystal:** a uniform simple shear (γ 1e-3) of a cubic crystal of strong
  anisotropy turned 22.5°: TE10 1.9510152 against 1/ny² = 1/n² + p'26 γ's 1.9510150 (p'26
  0.15, its p turned by Bond's matrix).
- **Ez in a guide filled with µr 2, 1, 1,** its ports in it: β 355.4639 /m against µyy (k0²
  εzz - (π/w)²/µxx)'s 355.4640, |S11| 2e-7.
- **A crystal turned 30° between plates, Hz:** β 237.353 /m and its front's tilt κ 50.464
  /m, against k0 √εyy 237.349 and (ηxy/ηxx) β 50.486 - the plates hold Ex nought, so the
  wave is e^{-jβx - jκy}.

## The library's crystals

| Crystal | Its tensors and their sources |
|---|---|
| Lithium niobate | εr 44, 44, 28 (clamped); no, ne Zelmon et al.'s Sellmeier (0.4-5 µm); r13 8.6, r22 3.4, r33 30.8, r51 28 pm/V (clamped); d15 69.2, d22 20.8, d31 -0.85, d33 6.0 pC/N and stiffness (Warner et al.'s); photoelastic constants and expansion along its axes (Weis and Gaylord's summary); heat 4.19 W/(m·K) across its optic axis, 4.6 along it (a supplier's) |
| Lithium tantalate | εr 41, 41, 43; no, ne Bond's measured indices (0.45-4 µm, through `interp`); r13 7.5, r22 1, r33 33, r51 20 pm/V (its sources differ: r33 30 to 36); d15 26, d22 8.5, d31 -3.0, d33 9.2 pC/N and stiffness (Warner et al.'s) |
| KTP | nx, ny, nz Kato and Takaoka's (0.43-3.54 µm); r13 9.5, r23 15.7, r33 36.3, r42 9.3, r51 7.3 pm/V; heat and expansion along its axes; εr a datasheet's effective 13 |
| Quartz | εr 4.52, 4.52, 4.68; no, ne Ghosh's (0.2-2 µm); d11 2.31, d14 0.727 pC/N and stiffness (Bechmann's) |
| Sapphire | εr 9.4, 9.4, 11.6; no, ne Malitson and Dodge's (0.2-5 µm); stiffness; heat and expansion along its axes; a c-plane wafer's (its optic axis up) |
| YIG, Ce:YIG | n 2.21, 2.22 near 1550 nm; g -3.3e-4 and 0.0086 (their Faraday rotations, about 175 and -4500 °/cm, θF = -π g/(λ n) about the magnetization); YIG's stiffness |
| Silicon | its cubic stiffness (Hall's 165.7, 63.9, 79.6 GPa) and photoelastic constants (-0.094, 0.017, -0.051, as used near 1550 nm), a (100) wafer's axes: x [1-10], y [001], z a ⟨110⟩ |
| GaAs, InP | r41 1.5 and 1.45 pm/V, a (100) wafer's axes |
| Silicon dioxide, fused silica | photoelastic 0.121, 0.270 (an isotropic solid's) |

Silicon's orientation keeps a silicon solid as it was: 169 GPa along x where its isotropic E
was 170 (the bimetal strip's tip 0.58410 mm, 0.58379 before). Lithium niobate's x-cut keeps
what its old scalar entry gave a TE mode: ne and ε33 along x.

## Around them

- **The Euler angles' keys** are euler_phi, euler_theta, euler_psi: "phi" is a metal's work
  function, which a material kind's Euler default ("0") had made every material's - a gate
  beside gold took 0 eV (`metalWorkFunction` caught it).
- **A material's orientation** is read from its library entry where the node has none of its
  own (a file's `{"material": "Silicon"}`), as its properties are.
- **Physics of isotropic media** (semiconductor, rays, quantum wells) take a tensor's mean and
  say so in the log.
- **Lumerical:** the importer takes κ and dn/dT along x, y, z (it took x's), and a MODE
  project's anisotropic material into My Materials as "nx; ny; nz". The MODE export writes a
  crystal's n and κ turned into the model's axes as Lumerical's diagonal anisotropy - with a
  note where it is turned off them (its ε's off-diagonal left out) -, and κ, tan δ and σ
  along the axes. A material whose index is a tensor is written as its own material even
  when Lumerical's are asked for (the oxide given an index along its axes would have become
  Lumerical's isotropic glass).
- **The material plot and `fem_describe what=material`:** a crystal's εr, k, n, κ along each
  of its axes (n_x1, n_x2, n_x3; "axis" for Claude).
- **The panel:** a tensor's items said ("along x, y, z ... : 44, 44, 28"), a count no tensor
  has marked.
- **Pockels' r** in pm/V is checked as m/V (it was warned of as not dimensionless).
- **The coupled modes' first form,** in Et and Ez as they are (K0 = A + C + B_zz, K2 = B_tt),
  had spurious modes near the physical ones on some meshes: a garnet slab narrowed to 0.1 µm
  gave its backward TM0 1.4709 - 0.014j, in a lossless guide. In the uncoupled problem's
  variables there are none, and no refinement is needed: the indices are the same to
  thirteen digits with one, and real to 1e-30.
- **`fem_describe what=format`** describes tensors, orientations, the crystals and the new
  features.

## The examples

All 46 examples were solved with `ce8d2b0` and with this round, every study: the same to
1e-9 but two. The bimetal strip's tip is 0.58410 mm (0.58379: silicon's cubic stiffness on
its wafer, 169 GPa along the strip where its E was 170), its description's 0.58 mm still. The
SOI wire's PML losses move by 1e-6 of themselves (0.010245 dB/m), its indices the same to ten
digits.

## Open

- **Elastodynamics:** no inertia in solid mechanics, so no resonators (above).
- **KTP's stiffness and permittivity, quartz's and sapphire's photoelastic constants, quartz's
  r:** not carried - no source checked here. KTP's εr is a datasheet's effective 13.
- **Figures not checked against one source here:** lithium tantalate's r (its sources' r33
  30 to 36 pm/V), KTP's r, silicon's photoelastic constants, YIG's and Ce:YIG's gyration
  (their Faraday rotations' sign conventions differ - set g's sign from your film's data).
- **Lithium niobate's dn/dT** is the extraordinary ray's for every axis.
- **YIG's and Ce:YIG's indices** are constants near 1550 nm (`nrange` 1.2-1.7 µm).
- **A waves port in a crystal turned in the plane** is refused, not solved.

## Tests

- **`test_fem_anisotropy`** (new, 22 functions):
  - `tensorTexts`, `orientations`, `rotations`: texts of 1, 3, 6, 9 items; cut presets
    right-handed; Euler's z-x-z; R T Rᵀ and Bond's matrix against numbers.
  - `capacitorAlongEachAxis`, `turnedCrystalAgainstItsPde`, `lossesAlongTheAxes`: es, ec and
    ht with a crystal - along each axis, turned against the PDE of its components, a coax's
    C and G of its in-plane εr and tan δ.
  - `laminatedCore`, `cubicSilicon`, `thermalExpansionAlongAxes`, `piezoelectricPlate`:
    magnetics, solid mechanics, the piezoelectric plate (by e and by d).
  - `turnedCrystalModes`, `tiltedCrystalSlab`, `nonReciprocalPhaseShift`,
    `pockelsAndPhotoelasticShifts`: modes against the layers' transfer matrices; a turned
    crystal sheared.
  - `conductiveSheet`, `grapheneConductivity`, `grapheneLayerAgainstSheet`: the sheet.
  - `crystalLibrary`: each crystal's indices at 1550 nm its source's formula's (to 1e-9),
    every tensor text read, e = d C, silicon's and lithium niobate's orientations from a
    panel's node and a file's.
  - `turnedCrystalInWaves`: a crystal slab and a filled guide (ports in it) against their
    closed forms; Ez's TE10 in a guide of µr 2, 1, 1; the turned crystal's tilted wave; a
    port in it refused.
  - `zCutAgainstXCut`: the review's benchmark.
- **`test_fem_lumerical` `aCrystalBothWays`:** lithium niobate (κ along two of its axes)
  exported and imported again; asked for Lumerical's materials, still its own, and the oxide
  given an index along its axes its own too; turned, the note.
- **`test_fem_materials` `curves`:** lithium niobate's n along its axes against λ.
- **`test_multiphysics_doc` `aCrystalInThePanel`:** a crystal from the library in the panel,
  its tensor said and a wrong count marked; `fem_describe` along its axes.
- **`test_fem_semiconductor` `metalWorkFunction`:** caught the "phi" key.

## Breaks

Each change was broken in turn and its tests run: 36 breaks.

| Of | Breaks |
|---|---|
| tensors and orientation | R T Rᵀ with a wrong index; a cut not right-handed; Euler's θ the other way; Bond's shears without their second term; Cji not filled from Cij; an orientation not read from the library's entry; the Euler angle's key "phi" again; d taken for e; a tensor's mean not taken for an isotropic physics |
| es, ec, ht, magnetics, solid | the plane's xy dropped; the piezoelectric polarization's and stress's signs; G's tensor without its xy; ν not inverted; plane stress not condensed |
| modes | the coupled K1's sign; the coupling left out of K1; the gyration the other way; Pockels' Δη added to ε; a strain's shears not halved; the sheet's tangential term left out; graphene's interband reactance's sign; an in-plane σ taken along every axis; r's unit checked as 1 |
| waves | the Hz tensor's off-diagonal sign; a port's line problem along it with its across; a port in a turned crystal not refused |
| the library | silicon's wafer not turned 45°; lithium niobate z-cut; its no and ne swapped; a panel's node without the orientation |
| Lumerical, plot, panel | the export not turning a crystal; a crystal mapped to Lumerical's material; κ imported along x alone; a crystal's curves its mean; the panel's tensor not said |

Three first passed:

- **A strain's shears not halved:** the strained glass had no shear, and an isotropic p is the
  same turned. A cubic crystal turned 22.5° and sheared is checked now.
- **A port's line problem along it with its across:** the filled guide's TEM mode does not
  vary along its port, so the term along it is nought. Ez's TE10 in a guide of µr 2, 1, 1 is
  checked.
- **A crystal mapped to Lumerical's material:** Lumerical's table has no optical lithium
  niobate, so its guard did nothing there. The oxide with an index along its axes is checked.

With these all 36 are caught, and were again on the final code (the coupled solver's
refinement gone, its coupling left out broken instead).

## Builds

The full ctest passes. Under the sanitizers (Debug, ASan and UBSan) the changed tests pass,
with no reports:

| Test | Time |
|---|---|
| `test_fem_anisotropy` | 330 s (12 s in Release) |
| `test_fem_waves` | 472 s |
| `test_fem_lumerical` | 190 s |
| `test_fem_semiconductor` `metalWorkFunction` | 93 s |
| `test_fem_physics` | 23 s |
| `test_fem_materials` | 2 s |
| `test_multiphysics_doc` `aCrystalInThePanel`, `claudeAndTheLibrary` | 4 s |

Its slabs are as narrow as their sides allow (uniform across them: 0.1 µm, graphene's 0.02),
and on a sanitized build its graphene and z-cut models meshed coarse - the graphene's six
solves 365 s there before, the z-cut's four 384 s; the z-cut's there its two biased devices
alone, the same to 5e-6.

CI's jobs have run out of time since `faaf93d`, before this round: macOS's unit tests past
its 60 minutes, Linux's sanitized ones past 210 (`test_fem_quantum` past its 1800 s), both
running ctest one test at a time.

The round's lambdas of 15 lines or more name their captures (GCC 12 and 13).
