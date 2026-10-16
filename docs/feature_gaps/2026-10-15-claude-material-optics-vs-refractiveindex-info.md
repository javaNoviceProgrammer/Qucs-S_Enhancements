# Feature gaps: the material library's optics against refractiveindex.info

*15 October 2026 — on `3bf5821` (with `133d1e2`, `c7b1bd6` and `889ddcf` between, other
work). The review: `2026-10-15-claude-material-optics-vs-refractiveindex-info.md`, 15
October. It compared the library's n and κ, read through `fem_describe what=material
vs=lambda`, with the refractiveindex.info database (cloned that day; CC0). Every entry with a
dispersion formula reproduced its source; the problems were silicon's pole and its visible
index, some fifteen compounds with a constant n and no κ, variants not named, silver in the
mid-infrared, and smaller points.*

Each item was first seen as the review says, then done. The database's measured spectra are
what the fits were made to and checked against (the datasets named below); the fits are
ours. Where two datasets disagree, it says so.

## The review's items

| Review | Now |
|---|---|
| **1.** Silicon: Salzberg and Villa's Sellmeier used below 1.2 µm has a pole at 1.13475 µm - n NaN, then 4.41 at 1.135 µm (true 3.54); a test that n is finite and monotonic over `nrange` | **Done.** Seen: NaN between about 1.1347 µm and the pole, 4.41 at 1.135 µm. Now one Sellmeier of two ultraviolet poles (0.262 and 0.349 µm) fitted to Green's (0.4-1.45 µm, 300 K) and Li's (1.2-14 µm, 293 K) together: within 0.007 of Green's, 0.001 of Li's, 2e-4 of Li's 3.4757 at 1550 nm; no pole in its range. Below 0.4 µm Green's measured points (the E1 peak at 0.37 µm). `nrange` 0.25-14 µm. Tested: finite and falling at 4000 points from 0.4 to 14 µm, and 3.534 ± 0.004 from 1.130 to 1.140 µm (Green's 3.535). |
| **2.** Silicon below 0.6 µm 0.5 low at 0.4 µm; κ (Rajkanan) 30-45 % off; Green's n and κ table, or `nrange` from 0.8 µm | **Done.** n at 0.4, 0.5, 0.6 and 0.8 µm now 5.613, 4.298, 3.938, 3.673 (Green's 5.613, 4.294, 3.940, 3.675; it was 5.070, 4.215, 3.908, 3.665). κ Green's (300 K, band to band) from 0.25 to 1.45 µm, straight between the logarithms of its 121 points (κ falls 5 decades in 0.4 µm), nought beyond: at its points exact, between them their geometric mean. |
| **3.** About 15 compounds a constant n and κ 0 (InGaAs a 1550 nm absorber made lossless); the alloys' n not following x; Adachi's model dielectric function, or at least κ from the gap, or Afromowitz extended | **Done, as Afromowitz extended and κ from the gap** (below). Every zinc blende binary and alloy: n Afromowitz's oscillator (E0, Ed) at the gap with a lattice term (εs - ε∞ at the TO phonon) for the infrared, each binary's fitted to the measured spectra; an alloy's oscillator its binaries' by their shares at its own gap (bowing), x, y and T; InGaAsP Adachi's 1982 index of y on InP's lattice. Held at 0.995 Eg and above. κ above the gap the library's direct-gap edge α = A √(E - Eg)/E - the form the Direct-gap absorption uses -, A fitted to measured κ from 0.05 to 0.45 eV above the gap; an alloy's A its binaries' A over µ^1.5 Ep (the edge's joint density of states) by their shares, times its own µ^1.5 Ep. Nought below the gap. Germanium Nunley's measured n and κ with Burnett's Sellmeier; CdTe, CdSe, CdS and GaN their sources' formulas. |
| **4.** Variants chosen silently (HfO2, Al2O3, SiN, TiO2, Cu, Au, water); film variants Al2O3 (ALD), HfO2 (ALD), SiN (PECVD); water's dispersion; SiO2 and Fused silica the same formula | **Done, but HfO2 (ALD)** (below). Each description names its variant and the others' values at 1550 nm (HfO2 Al-Kuhaili's e-beam 1.878, sputtered 1.96, others to 2.05; Al2O3 a crystal's 1.746, films 1.61-1.66; SiN Luke's LPCVD 1.996, PECVD 1.88-2.05, sputtered 2.46; TiO2 rutile's ordinary ray, films 0.2-0.6 lower in the visible; Cu Rakić's 0.47, Johnson and Christy's 0.72, Babar's 0.19). New: **Aluminium oxide (ALD)** (Rodriguez de Marcos et al.'s PEALD, a Sellmeier within 0.001 from 0.3 to 5 µm), **Hafnium oxide (sputtered)** (Bright et al.'s, within 0.0033 from 0.4 to 5 µm), **Silicon nitride (PECVD)** (Beliaev et al.'s, within 0.007 from 0.3 to 1.69 µm). Hafnium oxide's own n is now Al-Kuhaili's Cauchy (0.2-2 µm), not a constant. Water: Daimon and Masumura's Sellmeier (20 °C) to 1 µm, Hale and Querry's n and κ (25 °C) beyond, weighed between 1 and 1.13 µm; 0.2-200 µm. Silicon dioxide and Fused silica say they are the same Malitson formula (Fused silica has the glass's figures of heat and stiffness). |
| **5.** Silver's Drude in the mid-infrared (5.26 at 10 µm, Yang's 9.98); fit to Yang, or `nrange` to 2 µm | **Done.** A Drude-Lorentz fit to Yang et al.'s (ε∞ 2.45, ωp 8.85 eV, γ 37 meV, an oscillator at 8 eV for the interband absorption): n and κ within 3 % from 0.4 to 1 µm, 1 % from 1 to 25 µm (10.006 and 69.0 at 10 µm); Johnson and Christy's κ within 5 %. `nrange` 0.4-25 µm. |
| **6a.** No κ for any dielectric; SiO2's `nrange` to 6.7 µm where it absorbs | **Done.** Silicon dioxide and Fused silica: κ Franta et al.'s fused silica plate from 2.5 µm (5e-6 there, 0.0048 at 5 µm), nought below. Malitson's n agrees with Franta's within 0.003 to 6.7 µm. |
| **6b.** Lithium tantalate piecewise linear (its slope jumping, its group index with it) | **Done, as a Sellmeier fitted to Bond's points** (Abedin and Ito's coefficients are not in the database): within 0.0007 of each of Bond's 22 points, o and e. Tested smooth: its group index bends smoothly from 10 nm to 10 nm (the table's jumped 0.005 at each point). |
| **6c.** Outside `nrange` a formula is taken as it is; `fem_describe` gives no note | **Done.** A material's curves (fem_describe what=material, the material visualizer) say when n, κ or α are taken outside its `nrange` - swept past it ("taken at 1.2 to 2.731 µm") or held there ("taken at 1.55 µm"). A mode or wave solve already warned. The new formulas with poles near their ranges are held at their ends (CdTe, CdSe, CdS, GaN, HfO2): no pole is reached outside. InP's Pettit and Turner Sellmeier, whose pole at 0.626 µm gave NaN from 0.5 µm, gives way to InP's oscillator below 0.95 µm (weighed to 1 µm). |
| **6d.** The uniaxial crystals' descriptions could name o and e | **Done.** Lithium niobate, lithium tantalate, quartz and sapphire: no along x1 and x2, across the optic axis, ne along x3. |
| **Not checked:** YIG, Ce:YIG; GaInP and SiGe (the data below 1 µm); temperature | GaInP checked now against Ferrini et al.'s film (0.69-24.8 µm): within 0.011 from 1 to 1.7 µm, 0.03 at 0.8 µm (its gap here 1.96 eV, the films' 1.85-1.9). YIG, SiGe and temperature as they were. |

## The III-Vs

Adachi's model dielectric function was tried first. Its 1989 form could not be recovered
from the database's tables (they carry an E1 structure that neither a 2D M0 term nor an
extra oscillator reproduces), and Adachi's tables themselves run 0.07-0.2 low in n above
GaAs's gap against Aspnes and Studna, and 0.03-0.07 high in the infrared against Skauli. Its
broadening also leaves absorption below the gap: κ 0.0094 for GaAs at 0.8 eV (950 /cm in its
transparent window), AlSb 0.038 at 1550 nm, below its 1.6 eV indirect gap - the review's
table quotes that one. Reduced forms of it fitted to measured data were unstable (parameters
at their bounds). Afromowitz's oscillator - the review's second best - fitted each binary's
measured infrared index well once a lattice term was added:

| Binary | Fitted to | Within |
|---|---|---|
| GaAs | Skauli et al.'s Sellmeier, 0.97-17 µm (its oscillator 3.680, 36.29 eV; Afromowitz's own 3.65, 36.1) | 0.002 |
| InP | Pettit and Turner's, 0.95-10 µm (InP keeps Pettit and Turner's from 1 µm) | 0.008 |
| InAs | Lorimor and Spitzer's, 3.7-20 µm | 0.01 |
| AlAs | Fern and Onton's, 0.56-2.2 µm | 0.003 |
| GaP | Bond's, 0.5-4 µm | 0.008 from 1 µm (0.04 nearer its gap: its `nrange` from 0.8 µm) |
| AlSb | Zollner's (to 0.89 µm) and Adachi's; Djurišić's is 0.09-0.15 higher than both | 0.022 of Adachi's |
| GaSb | Adachi's and Djurišić's, 0.15 apart at 2 µm | between them |
| InSb | Adachi's (no measured index in the database) | 0.005 |
| AlP | none in the database: its oscillator at 1.65 its Γ gap (AlAs's and GaP's ratio), Ed to its ε∞ | - |

The edge's strength A, fitted from 0.05 to 0.45 eV above the gap: GaAs 6.58e4 /cm (Aspnes's
and Jellison's κ at 1.6 eV within 25 %), InP 1.16e5 (its Afk from the last round, Aspnes's
within 5 %), InAs 9381 (Adachi's κ at 1550 nm 0.110; ours 0.097 - its split-off band, below
1550 nm's photons, not counted), GaSb 2.8e4 (Djurišić's), InSb 3449 (Adachi's), AlSb, GaP and
AlAs from their Γ gap (κ nought between their indirect and direct gaps: their weak indirect
absorption is not counted).

The alloys: an alloy's oscillator is its binaries' E0 and Ed by their shares, its gap its own
(the bowings of the measured band set). Against Adachi's tables, the only near-infrared
reference the database has for them:

| Alloy | At 1550 nm | Against |
|---|---|---|
| In0.53Ga0.47As | n 3.514, κ 0.0927 (α 7500 /cm) | Adachi's 3.531 and 0.076; below its gap 0.03-0.07 under his (his 0.03-0.07 over the measured binaries there); measured α 6800-7000 /cm |
| InGaAsP on InP (Adachi's 1982, A 8.4 - 3.4y, B 6.6 + 3.4y) | the 1.3 µm quaternary 3.393, transparent | Adachi's 1989 table (y 0.24) within 0.004 from 1.55 to 5 µm, 0.01 at 1.3 µm; at y 0 Pettit and Turner's InP within 0.003. The oscillator mix had been 0.03-0.06 low |
| Al(x)Ga(1-x)As | Afromowitz's (as it was), κ the edge's | Aspnes's and Papatryfonos's near the edge: the datasets scatter ±40 % between themselves; ours within it |
| Ga0.51In0.49P | n 3.100 | Ferrini et al.'s 3.105 |

Linear interpolation of A between InAs and GaAs had made In0.53Ga0.47As 0.124 (α 1.0e4 /cm);
the µ^1.5 Ep scaling gives 0.0927.

The review's 1550 nm table, now:

| Entry | Was | Now | Database |
|---|---|---|---|
| Indium gallium arsenide (x 0.53) | 3.396, κ 0 | 3.514, κ 0.0927 | 3.531, 0.076 (Adachi) |
| Indium arsenide | 3.420, 0 | 3.534, 0.097 (outside its `nrange`, said) | 3.453, 0.110 (Adachi) |
| Gallium antimonide | 3.800, 0 | 3.993, 0.117 | 3.837 (Adachi), 3.980 (Djurišić), 0.09-0.11 |
| Indium antimonide | 4.000, 0 | 3.984, 0.042 (outside, said) | 3.965, 0.077 (Adachi) |
| Aluminium antimonide | 3.200, 0 | 3.253, 0 (below its gap) | 3.253 (Adachi; its 0.038 its broadening's tail) |
| Germanium | 4.275, 0 | 4.250, 0.0252 | 4.250, 0.025 (Nunley) |
| Cadmium selenide | 2.450 | 2.485 | 2.485 (Lisitsa) |
| Cadmium telluride | 2.720 | 2.736 | 2.736 (Marple) |
| Aluminium arsenide | 2.900 | 2.893 | 2.892 (Fern) |
| Gallium phosphide | 3.050 | 3.057 | 3.051 (Bond, at 1.6 µm) |
| Gallium nitride | 2.310 | 2.317 | 2.317 (Barker, o) |

## A material's κ and the options that add absorption

A Wave Equation's **Direct-gap absorption** (Franz-Keldysh) added its α to the material's κ;
with germanium's κ now its measured edge, and every III-V's, that counted the edge twice. It
now takes the place of a material's κ (a κ given in the feature is still added). Likewise the
**Quantum wells' absorption**: the wells' computed absorption in place of the well
material's bulk edge. Tested: a Ge-on-Si guide's mode loses 2212.55 /cm by its κ alone, the
same by the option at no field, the same with its κ given nought (it lost twice that); the
wells' mode loses as much with the well's κ given nought as with its bulk κ there, and without
the wells' feature the bulk GaAs's own loss.

**Germanium on silicon**: n bulk germanium's moved 27 meV down with its strained direct gap
(its peak, at bulk germanium's gap 1.55 µm, at 1.60 µm); κ its edge (Egd 0.773 eV, A 2e4 /cm:
4100 /cm at 1550 nm) or bulk germanium's, the larger. A different Egd given to it is the
Direct-gap absorption's to take; its κ stays the 0.773 eV edge's.

## What moved

- The **Ge photodiode example** (`ge_photodiode.qfem`): germanium's measured index (4.242 at
  1550 nm for germanium on silicon, the constant was 4.275) moves its mode from n_eff 3.84 to
  3.81, and the next mode up (3.94) came nearer the mode step's guess, 3.9: the guess is now
  3.82. Its loss (1.91 dB/µm), photocurrent (1.155 mA) and collection (0.925) are as they
  were; its spectrum's responsivity at 1580 and 1600 nm 1.09 and 0.88 A/W (1.12, 0.905). Its
  description says so. Its index along the sweep now rises to the strained gap's peak and
  falls after it; the sweep test checks the mode followed (steps under 0.03) and the one not
  followed jumping branch (0.14 at 1640 nm).
- The **GeSi modulator example**: its extinction at -3 V 5.62 dB (5.65).
- Silicon at 1550 nm 3.4756 (Li's formula 3.4764; Li's own table 3.4757).
- A mode at 1310 nm in germanium is no longer out of range; the test of the warning uses
  InAs (its range from 2.731 µm).

## Checked

test_fem_materials (`dispersion` rewritten, `semiconductorOptics` and
`everyIndexHoldsInItsRange` new): silicon against Green's and Li's points, finite and falling,
the pole region; InP finite from 0.4 µm; gold Vial's, silver its formula and Yang's at 1.55 and
10 µm; GaAs against Skauli's at eight wavelengths from 0.97 to 17 µm and the oscillator's
formula; GaAs's and InP's κ the edge's and Aspnes's; InAs, AlSb, GaP, AlAs, InSb against their
datasets; In0.53Ga0.47As's κ the µ^1.5 Ep scaling worked in the test, its gap 0.750 eV,
transparent past it; an alloy's n following x; GaInP against Ferrini's; InGaAsP against
Adachi's 1982 formula, his 1989 table and Pettit's InP; germanium against Nunley's and
Burnett's, germanium on silicon its edge and its index moved; the II-VIs and GaN their
formulas, held at their ends; water against Daimon's and Hale's, no step at the join; silica's
κ Franta's; the films against their datasets; LiTaO3 against Bond's and smooth. Every library
entry with an index: n finite and positive, κ finite and nought or more, at 600 points across
its `nrange`, no note there; outside it, the note. test_fem_optoelectronics
`theGapsEdgeCountedOnce` (new); test_fem_modulator `wellsAbsorptionOfTheField` (the bulk
well's κ, replaced); test_fem_anisotropy, test_fem_semiconductor and the examples' tests as
above.

## Broken on purpose

23 changes, each caught (one after the test was made to see it):

| Change | Caught by |
|---|---|
| Silicon: Salzberg and Villa's Sellmeier below 1.2 µm again | `dispersion` (Green's 4.691 at 0.45 µm: 4.512) |
| Silicon's κ straight between Green's points, not their logarithms | `dispersion` (between 0.60 and 0.61 µm) |
| Green's κ held past 1.45 µm (1.4e-13, not nought) | `dispersion` - missed while the test asked QCOMPARE(κ, 0.0), which takes 1e-13 for nought; the checks of nought are now exact |
| InP: Pettit and Turner's below 0.95 µm (its pole) | `dispersion` (14.7 at 0.4 µm) |
| The III-Vs without their lattice's term | `semiconductorOptics` |
| The oscillator not held at the gap | `everyIndexHoldsInItsRange` (InP NaN at 0.859 µm) |
| An alloy's edge strength linear in its binaries' A | `semiconductorOptics` (InGaAs κ 0.124) |
| The optics' gap Vurgaftman's, not the measured one | `semiconductorOptics` |
| AlSb fitted to Djurišić's again | `semiconductorOptics` |
| GaAs Afromowitz's again, without a lattice | `semiconductorOptics` |
| Germanium on silicon's κ bulk germanium's alone | `semiconductorOptics` |
| Germanium on silicon's index not moved with its gap | `semiconductorOptics` |
| Germanium: Burnett's Sellmeier below 2 µm (its pole at 1.18 µm) | `everyIndexHoldsInItsRange` (NaN at 1.169 µm) |
| Formulas not held at their ranges' ends | `semiconductorOptics` (CdTe) |
| Water: Hale's from 1 µm, not weighed | `semiconductorOptics` (a step of 0.0014) |
| Silver's old Drude | `dispersion` |
| LiTaO3's table again | `semiconductorOptics` (its group index bending 0.0051) |
| Silica without its infrared κ | `semiconductorOptics` |
| No note outside `nrange` | `everyIndexHoldsInItsRange` |
| A note at `nrange`'s own ends | `everyIndexHoldsInItsRange` (Copper at 0.6 µm) |
| InGaAsP the oscillator mix again | `semiconductorOptics` |
| The Direct-gap absorption added to the material's κ | `theGapsEdgeCountedOnce` (twice the loss) |
| The wells' absorption added to the material's κ | `wellsAbsorptionOfTheField` |

The Release suite 123 of 123. Under the sanitizers: test_fem_materials (12 functions, 59
s), test_fem_anisotropy crystalLibrary, test_fem_semiconductor plasmaDispersion and
indicesOfTheWavelength, test_fem_modulator wellsAbsorptionOfTheField (163 s),
test_fem_optoelectronics theGapsEdgeCountedOnce, theModulatorExample, thePhotodiodeExample
and aSweepFollowsItsMode (its slow build's three wavelengths) - all pass, no report.

## Not done, and limits

- **Hafnium oxide (ALD):** the database's only ALD hafnia (Siefke et al.'s PEALD) stops at
  0.62 µm; the variant added is sputtered (Bright et al.'s, to 500 µm).
- **The III-Vs' κ well above the gap**: the edge's √ form holds to some 0.45 eV above it; at
  E1 (visible light for GaAs, 2.9 eV) it is 2-4 times low. Their `nrange` starts 0.1 eV above
  the gap; the note says so beyond.
- **The III-Vs' n above the gap is held** at its value at 0.995 Eg (measured n rises in GaAs,
  falls in InAs): within the measured datasets' own spread 0.1 eV above the gap (GaAs's differ
  by 0.06 there).
- **Indirect absorption** (GaP, AlAs, AlSb, AlP, between their indirect and Γ gaps) is not
  counted; nor silicon's free-carrier absorption (the plasma option's).
- **AlP**'s index is an estimate (no data in the database).
- **SiGe** keeps its constant index near 1550 nm.
