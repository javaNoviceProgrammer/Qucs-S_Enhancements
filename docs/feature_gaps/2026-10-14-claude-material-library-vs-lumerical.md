# Feature gaps: the material library against Lumerical's

*14 October 2026 — on `2e349c6`. The review: `2026-10-14-claude-material-library-vs-lumerical.md`,
14 October, on `2e349c6`.*

The review set the library (47 entries) beside Lumerical's material database for CHARGE,
HEAT and MQW. By name it had about 80 % of Lumerical's electrical list. The bigger gap was
what each material carries: Lumerical's semiconductors bring their own models, of T, while
here every one ran with silicon's model parameters and its 300 K constants, and the optical
solvers took a 1550 nm index at every wavelength.

The library now has 64 entries. Each semiconductor carries its own models' parameters, its
properties are expressions of T read at the physics' temperature, the optical materials'
indices are expressions of the wavelength, and a property can be plotted against T, N, x,
λ or F.

## The review's recommendations

| Review | Now |
|---|---|
| 1. Per-material model sets: mobility, saturation, SRH, Auger, radiative, impact, BTBT, BGN; the features to use them; at least a warning where a non-silicon semiconductor runs with silicon's | **Done.** Each of Lumerical's 17 base semiconductors (Si, Ge, GaAs, AlAs, InAs, AlP, InP, GaP, AlN, InN, GaN, GaSb, AlSb, InSb, CdS, CdSe, CdTe) has its model set, read from Lumerical's database (its lowest valley's, its active models): Caughey and Thomas's mobility and its exponents of T, ζ, saturation (vsat, β), Slotboom's narrowing, Fossum's lifetimes, the tunnelling masses, Auger, radiative, Selberherr's impact ionization, Hurkx's band-to-band tunnelling. Silicon keeps the features' defaults, so a silicon model solves as before; Masetti's figures are silicon's alone. An alloy's are its binaries' weighed by their shares: mobilities harmonically, the rest and the exponents linearly, as Lumerical's alloys without bowings. **Each parameter:** as set on the feature (or its source "user"), else its material's, else the feature's default (silicon's). Where that default is taken on a material that is not silicon (not the library's Silicon, nor a gap within 0.03 eV of 1.12 eV), the log says so, once for each material: "Gallium arsenide (bar): its material gives no parameters for Masetti's mobility - silicon's (the features' defaults) taken". |
| 2. Temperature from the library: Varshni's Eg(T), Nc and Nv of T, the mobilities' exponents, κ(T) in heat | **Done.** A material's texts may read T. The semiconductor physics reads them at its temperature, the quantum wells at theirs, the heat physics at its field (T is ht.T there); the panel checks them at 300 K. The library's gaps are Varshni's anchored at their 300 K values (Eg300 - α (T²/(T + β) - 300²/(300 + β)), the same at 300 K to the last digit), Nc and Nv (T/300 K)^1.5, mobilities (T/300 K)^-ζ, conductivities of heat (T/300 K)^-η (silicon 1.65, germanium and GaAs 1.25, InP 1.4). An alloy's compound is taken at the physics' T. A lattice temperature TL scales the mobilities by (TL/T)^-ζ, from the physics' own T (it was from 300 K). |
| 3. The missing materials: SiGe; AlInP, AlGaInP, AlP; CdTe, CdS; HfO2, ZrO2, TiO2, Al2O3 as gate dielectrics; the metals' work functions, used by gates and Schottky contacts | **Done,** and CdSe, AlGaP, AlGaSb, AlAsSb, InAsSb, InAlSb and AlGaInN with them. **Silicon germanium** (x the germanium's share): its gap the lower of its X valley's (Lumerical's bowing of -0.4 eV) and its L valley's, the latter below from x 0.8; its mobilities and its conductivity of heat with the alloy's scattering (1/140 and 1/100 V s/cm², 1/2.8 m K/W of x(1 - x)). **AlP** with Vurgaftman's parameters (its k·p too); **AlInP, AlGaP, the antimonides** Vurgaftman's bowings; **AlGaInP** of its three ternaries, as AlGaInAs is; **AlGaInN** (wurtzite) of AlGaN, InGaN and InAlN, Vurgaftman and Meyer's. **CdS, CdSe, CdTe:** their 300 K gaps, χ from Lumerical's work functions, Nc and Nv of their masses, their model sets (CdSe's hole mobility an estimate). **HfO2, ZrO2, TiO2, Al2O3:** Robertson's permittivities (25, 24, 80, 9), Lumerical's heat. **The metals' work functions** φ (Ag 4.26, Al 4.28, Au 5.1, Cu 4.65 eV): a gate or a Schottky contact without its own takes the metal domain's beside it, said in the log - the gate's φ, the Schottky's barrier φ - χ. |
| 4. Optical dispersion: (n, k) of the wavelength for Si, Ge, III-Vs, SiO2, Si3N4, the metals | **Done, as formulas** checked against Palik's, Johnson and Christy's and the CRC's tables (the review's sources, in Lumerical's database): silicon Salzberg and Villa's Sellmeier below 1.2 µm and Li's above 1.4 µm (Palik's within 0.01 from 0.8 to 14 µm, 0.0001 at 1550 nm), its absorption Rajkanan, Singh and Shewchun's (κ within 30 % of Palik's from 0.45 to 1.1 µm, nought below the gap); SiO2 Malitson's; Si3N4 Luke's; GaAs and AlGaAs Afromowitz's (GaAs within 0.004 from 0.9 to 2 µm); InP Pettit and Turner's; sapphire Malitson and Dodge's; TiO2 DeVore's; ZrO2 Wood and Nassau's; gold Vial's Drude-Lorentz fit (κ within 3 % from 0.55 to 1.9 µm); silver a Drude fit (5 % from 0.4 to 1.9 µm); copper and aluminium Rakić's Lorentz-Drude (κ within 5 % from 0.6 to 2 µm). Each has `nrange`, where it holds: a mode or wave solved outside it says so ("Germanium's index holds from 1.5 to 1.6 µm ...; here it is taken at 1.31 µm"). Germanium, GaN, the II-VIs and HfO2 keep a constant index near 1550 nm, said so by their range. Two new functions make these: `neps(e1, e2)` and `keps(e1, e2)`, the index and extinction of a complex permittivity. |
| 5. A property visualiser | **Done.** A material's properties against T, N (the donors and acceptors), an alloy's x, λ or F, as its physics take them: Eg, χ, Nc, Nv, ni, εr, the mobilities (of T, by doping - Caughey and Thomas's or Masetti's, as its material gives them -, saturated by the field), vsat, the lifetimes (Fossum's by doping), the Auger and radiative coefficients, Slotboom's narrowing, impact ionization's rates, κ, ρ, Cp, n, κ and α. In the Multiphysics panel: a material's menu and the library's Plot Properties... (a dataset and a display). For Claude: `fem_describe what=material`. |
| §2: a stimulated recombination model (cstim, jthreshold, tgain) | **Done:** Stimulated Recombination, a semiconductor domain feature, Lumerical's laser model. Above threshold a share c of the recombination current is stimulated: R_stim = c (J_rec - J_th)/(q t), J_rec = q t ⟨R⟩ the domains' recombination per area. semi.Istim is its current (its photons' rate semi.Istim/q). The domains' mean is taken from the last iterate. |
| §2: the impact ionization feature's "silicon" model or user coefficients | Its model is **the material's** by default now: its own Selberherr coefficients, one range where it has one; silicon's has van Overstraeten and de Man's two. "Silicon" and "user" are as they were. |
| §3: importing materials from another project; an exchange file | **Done.** My Materials can import a library file (.json) or a Lumerical project's materials: an .ldev's electrical and thermal database (each semiconductor's gap Varshni's of T, Nc and Nv of its masses' T, χ from its work function, its models' parameters of T; a conductor's work function; κ, ρ and Cp, constant or of T), an .lms's or .fsp's optical materials (sampled data as `interp(lambda0, ...)` through its points, Sellmeier's as a formula). Its alloys are not taken: the library's compounds are those, said. My Materials exports to a .json file. In the panel: Import Materials... and Export My Materials...; for Claude, `fem_model action=materials`. |

The panel shows a semiconductor feature's parameter not set on it as its material's: the
field empty, its placeholder "40[cm^2/(V*s)] (Gallium arsenide's)".

`interp(x, x0, y0, x1, y1, ...)` is a new function as well: straight lines through its
points (constants, in any order), held beyond them. A sampled index is one, and so can be
any property of T.

A MODE project imported as a model keeps its materials' index of the wavelength too
(sampled data through `interp`, Sellmeier's as a formula), where it took them at the FDE's
wavelength: a sweep solves each wavelength at its own index.

`fem_describe what=format` lists the library with a long text (an index's formula, a
metal's permittivity, up to 3 kB) said by its length: `fem_describe what=material` gives its
values.

## Checked

- **The GaAs diode's ionization at -10 V** (its own coefficients, the new default): its
  ionization integral 4.25e-4, as with the coefficients typed, against 0.0100 with
  silicon's.
- **Stimulated recombination** in a GaAs pn diode at 1.25 V: semi.Istim 0.1199 A against q
  c (∫R - R0 A) = 0.1196 A from its recombination solved.
- **The gate's metal:** a MOS gate beside gold solves as with φ 5.1 eV typed; a Schottky
  contact on n-silicon beside gold as with its barrier 1.05 eV typed (5.1 - 4.05).
- **Lumerical's databases into My Materials:** the MQW project's .ldev gives its 33
  materials (the thermal records merged into the electrical ones of the same id); the
  MODE examples' .lms their Palik and CRC tables.

## The examples

All 46 examples were solved with `2e349c6` and with this round, every study:

- **Unchanged** to their digits: the semiconductor, quantum and other heat ones. Silicon
  at 300 K is as it was, and so are the compounds the quantum wells take.
- **The SOI wire** (`soi_waveguide_modes`): at 1550 nm TE0 2.4459 and TM0 1.7707 (Li's
  silicon 3.4764, Malitson's oxide). At 1500 nm 2.5018 and 1.8387: the review's hand model
  with Palik's indices at 1500 nm had 2.5018 and 1.8385, and this example with 1550 nm's
  had 2.497 and 1.835. Its description says 2.446 and 1.771 now.
- **Silicon's κ(T):** `self_heating_resistor` at 4 V is 333 K at its middle (330.4 K
  before) and draws 376.9 µA (380.8). Its description says so. `heater_crosssection`,
  `thermo_optic_modes` and `heatsink_import` move by 0.1 % or less, within their
  descriptions' digits.
- **The germanium photodiode and modulator, the phase shifter:** their mode indices move
  by 0.001 or less (the oxide's and silicon's at their wavelengths); their currents,
  losses and bandwidths as before to their descriptions' digits.

## Open

- **A compound's gap is its Γ gap.** AlAs's, AlP's, GaP's and the antimonides' indirect gaps
  are not taken: as the quantum wells' compounds have them, their entries say so.
- **Germanium's index is constant** (4.275 near 1550 nm), as are GaN's, the II-VIs' and HfO2's:
  `nrange` says where, and a solve outside it warns.
- **Copper's and aluminium's n** are 10-40 % off the CRC's (Rakić's fits are for κ).
- **Alloys from Lumerical's database** are not imported; the library's compounds stand for them.
- **Stimulated recombination** takes the domains' recombination from the last iterate (a
  lagged term, not in Newton's Jacobian).
- **CdSe's hole mobility** is an estimate.

## Tests

- **`test_fem_materials`** (new). Its figures are the sources' formulas worked in the
  test, not the library's texts read back:
  - **`expressionTables`:** `interp` between and beyond its points, in any order, its points
    constants; `neps` and `keps` against a complex square root.
  - **`siliconAsItWas`:** silicon at 300 K as before (its gap to the last digit), and each of
    its model parameters (more than 40) equal to the features' default.
  - **`ofTheTemperature`:** at 77, 250, 400 and 500 K, silicon's gap (Varshni's), Nc,
    mobility and conductivity of heat, germanium's and GaAs's gaps, GaAs's lifetime, InP's
    hole mobility; a binary's entry as its compound at T; an alloy at T.
  - **`modelSets`:** GaAs's Caughey and Thomas, Auger, radiative (and its exponent of T),
    impact and saturation; germanium's band-to-band; AlGaAs's weighed (mobilities
    harmonically, at 400 K their exponents weighed, Slotboom's GaAs's alone renormalized);
    AlGaN's.
  - **`newMaterials`:** each new entry; HfO2 an insulator; the metals' φ; CdTe's Nc; AlP's
    Vurgaftman parameters; the new ternaries' bowings; the quaternaries at their corners;
    SiGe's gap (X and L valleys), holes' mobility, conductivity of heat, χ.
  - **`dispersion`:** silicon Li's at 1550 nm, Salzberg and Villa's at 800 nm, the two weighed
    at 1.3 µm, Rajkanan's κ at 600 and 450 nm (the second gap's phonons there); Malitson's
    oxide, Luke's nitride, Pettit and Turner's InP, Afromowitz's GaAs (Palik's 3.3737 to
    0.005), Vial's gold at 800 nm, the Drude silver; the ranges.
  - **`curves`:** silicon's Eg and ni against T; GaAs's electrons by doping (Caughey and
    Thomas's, said in the notes); AlGaAs's gap against x; GaAs's electrons' ionization at a
    field; what is refused.
  - **`lumericalIntoTheLibrary`:** the DEVICE 7.17 project's silicon (Varshni's of T, Nc of its
    masses), aluminium's φ; the MODE project's sampled and Sellmeier materials; a DEVICE
    7.1 project's thermal database merged into its electrical records (the fixture's
    oxide); a library file written and read back; a file that is not a project.
- **`test_fem_semiconductor`:**
  - **`materialModelSets`:** a GaAs bar's mobility its own Caughey and Thomas's (to 1e-9), one
    set on the feature winning, Masetti's on GaAs silicon's and said.
  - **`atItsTemperature`:** silicon at 400 K: semi.ni and semi.mun to 1e-6 and 1e-9; a TL of
    400 K changing nothing there, 500 K (5/4)^-2.5.
  - **`metalWorkFunction`:** a gate beside gold as with 5.1 eV typed, beside nothing 4.05 eV;
    a Schottky contact beside gold as with 1.05 eV typed; the log.
  - **`stimulatedRecombination`:** semi.Istim against q c (∫R - R0 A) to 2 %; below threshold 0.
  - **`impactFromItsMaterial`:** GaAs's ionization integral as typed, at 1e16 and 1e17 cm⁻³
    on its n side (the field above 4e5 V/cm there); silicon's another; E0 set on the
    feature, GaAs's one range still.
  - **`indicesOfTheWavelength`:** a mode at 1310 nm with the library's indices as with them
    typed; germanium's range said; its index given on the feature, nothing said.
  - **`plasmaDispersion`:** its reference the library's silicon at 1550 nm.
- **`test_fem_devices` `selfHeating`, `latticeTemperatureOneWay`:** silicon's κ(T) in the
  references by Kirchhoff's transform (U = ∫k/k300 dT); the currents to 2e-4 (they agree to
  1e-6), the middle's warming to 0.2 %.
- **`test_fem_lumerical`:** a MODE project's materials of the wavelength (sampled data through
  `interp`, read at 1550 nm; Sellmeier's); the oxide's index at 1500 nm the library's.
- **`test_multiphysics_doc` `claudeAndTheLibrary`:** `fem_describe what=material` for GaAs's
  mobility by doping, an alloy refused, a material not there; the format's long texts
  shortened; a Lumerical project's materials and a library file into My Materials, its
  silicon's gap of T; the panel's placeholder (GaAs's 40[cm^2/(V*s)]) and a feature's own
  value; a material plotted (a dataset with T, Eg and mun).

The fixtures: `lumerical_charge.ldev` has a thermal database beside its electrical one
(DEVICE 7.1's layout), its oxide's record under the same id.

The full ctest (120 tests, `test_fem_materials` new) passes; no file was written outside
the scratch directories.

## Breaks

Each change was broken in turn and its tests run: 55 breaks.

| Of | Breaks |
|---|---|
| the texts of T | Varshni not anchored at 300 K; the exponents negated; silicon's heat exponent -1.5 (two tests); T always 300 K; a compound at 300 K whatever T; the texts rounded to 6 digits |
| model sets and alloys | mobilities weighed linearly; the shares not renormalized; the exponents not weighed; the Auger keys swapped; an impact key unmapped; SiGe's bowing the other way, without its L valley, its heat and its holes without the alloy's scattering; AlInP's bowing nought |
| dispersion | Rajkanan's emission as absorption, without the second gap; Li's from 1.2 µm; Malitson's third term off; Afromowitz without the gap's term; gold's oscillator weight 1; the Lorentz terms' sign; `keps` the real part's; `interp` extrapolating, its points unsorted; the range not said, taken of a user's index |
| the semiconductor | the material's parameters not taken; a feature's own not first; silicon's defaults not said; the physics' texts at 300 K; a TL scaled from 300 K; a gate's metal not read; a Schottky barrier φ - 4 eV; stimulated recombination without its threshold, its share, R0 per area; a second ionization range not the first's; that and E0 reached |
| the quantum wells | their materials without T; a gap of T moved again |
| Lumerical's materials | a gap's β dropped; a conductor's work function not taken; sampled data not interpolated; the thermal records not merged |
| the visualizer and the app | ni at 300 K's gap; N not in 1/cm³; ionization without β; x not swept; the placeholder not the material's; Claude's points ignored; a project's materials not kept; the format's long texts whole |

Nine first passed:

- **Silicon's heat exponent -1.5** was aimed at the self-heating bar, whose tolerance (0.5 %)
  hid it. `ofTheTemperature` catches it; the bar's checks are now 2e-4 and 0.2 % (it agrees
  to 1e-6), and catch it too.
- **The shares not renormalized, the exponents not weighed:** AlGaAs was checked at 300 K and
  on parameters both its binaries have. Its µmin at 400 K and Slotboom's narrowing (GaAs's
  alone) are now checked.
- **Rajkanan without the second gap:** at 600 nm only the first gap's phonons count. 450 nm
  is checked too.
- **Li's from 1.2 µm:** nothing was checked between 1.2 and 1.4 µm, where the two fits are
  weighed (they differ by 0.0035 at 1.3 µm). 1.3 µm is checked.
- **A user's index said out of its material's range:** the user's index was silicon's, in
  range anyway. A germanium core with its index given at 1310 nm is checked.
- **The second ionization range not the first's, and with it E0 reached:** the diode's
  field stayed below 4e5 V/cm. Its n side at 1e17 cm⁻³ reaches 5e5, and the feature with
  E0 set is checked there.
- **The thermal records not merged:** no fixture had DEVICE 7.1's two databases. The CHARGE
  fixture's oxide now has a thermal record beside its electrical one.

With these the 54 in the table are caught. A 55th, keeping E0 out of reach for a material of
one range, could not be: the second range's falling back to the first did the same. That
guard was taken out.

## Builds

The full ctest (120 tests) passes. Under the sanitizers the changed tests pass, with no
reports:

| Test | Time |
|---|---|
| `test_fem_materials` | 0.1 s |
| `test_fem_semiconductor`, its new and changed functions | 512 s (`impactFromItsMaterial` 223 s, now 32 s) |
| `test_fem_devices` `selfHeating`, `latticeTemperatureOneWay` | 34 s |
| `test_fem_quantum` `oneMaterialOneGap`, `at77K` | 11 s |
| `test_fem_lumerical` | 183 s |
| `test_multiphysics_doc` `claudeAndTheLibrary`, `claudeImportsLumerical` | 26 s |

`impactFromItsMaterial` solves six diodes, on a sanitized build on a coarser mesh (the
integrals compared on one mesh): 223 s there was too near QtTest's 300 s on CI's slower
runners.

The round's lambdas of 15 lines or more name their captures (GCC 12 and 13, as
`d7ae967`'s).
