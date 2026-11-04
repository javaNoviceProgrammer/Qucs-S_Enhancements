# RF 2.5D: full-wave meshes graded at narrow gaps, held to a 2D eddy-current solve

The follow-up the thin-layer round (db6da60) left: there a quasi-static study's mesh began to grade a conductor's side
that faces another across a narrow gap down to a quarter of it (`MeshSettings::gaps`), and a full-wave study's was kept
on the edges' one halving. The measured coplanar line of `test_rf25_measured` had lost 18 % under its measurement on
the halved edges and come out 5 % over it on graded ones, and the edge loss model (`crowding()`) had been fitted on
halved edges. Which was right - the graded mesh over-counting, or the halved one under-resolving - wanted a reference
that was neither the solver nor the measurement, whose metal is rough.

| Asked | Now |
|---|---|
| A 2D reference for the conductor loss | **A 2D eddy-current solve of the cross-section** (`eddy.py`): every conductor cut into rectangles of uniform current graded to a hundredth of a micrometre at its faces, the mean of ln r between them in closed form (`meanlog.py`, to 1e-10 of quadrature, a rectangle's own Gray's formula exactly), the chuck a perfect plane by images. The measured line's R: 731.4, 1267.3 and 1795.4 Ω/m at 10, 30 and 60 GHz, within 0.1 % from 1420 to 9450 filaments; held two independent ways (below) |
| Do graded meshes double-count the edge loss? | **No: the halved edges under-count.** Against the 2D solve's smooth line, the measured line's attenuation is 20, 17 and 14 % under on the halved edges and 1.6 % under, 5.5 and 9.7 % over on the graded ones - the grounds' outermost cells facing the gaps were 45 µm wide, where the current falls away from the gap within the cell. The measurement itself is 1.4 % under the smooth line at 10 GHz, 0.2 % over at 30 and 4.7 % over at 60 (the paper's 63.5 nm roughness is some 3 % there): "under the measurement" was never the smooth line's place |
| Re-fit the edge loss on graded meshes | **`crowding()` kept, after trying.** A band of a thick metal narrower than it is thick loses less than its current's average (0.78 of the sheet's at 4.2 µm in from 6.5 µm of metal, the 2D solve's bands), but the faces carry no current down their walls, and their narrow edge cells take the less of the current the lower the frequency: the factor as it is, over the band's, comes the nearer. On five lines, the factor faded to nought as a cell narrows left the attenuation 16 % from the 2D solves on average, the present one 8.6 %; unclamped, or without Pearl's fading, no better (below) |
| Turn `how.gaps` on for full-wave studies | **On for both**: the tab meshes every study through one `tabMeshSettings()`. On four coplanar lines the faces' attenuation comes within 8.6 % of the 2D solves on average, where the halved edges left 18 %; the full-wave solve itself on the measured line, -20 to -14 % before and -1.6 to +9.7 % now, and on a narrow-gap line -35 and -31 % before, -20 and -6 % now at 10 and 30 GHz |
| Reconcile `calibrate()`'s meshing | **The strips meshed as the structure is.** The solve job made its mesh settings afresh, without the gaps, and handed them to the calibration strips: graded, the structure's port would not have had its strips' cells. The job takes the tab's settings too, and the calibration says so: "The strips meshed as the structure is: 4 side(s) facing the feed's gaps, their outermost cells halved until they are no wider than a quarter of the gap." (`Mesh::facing`) |
| The dense limit | The measured line's strips, 4.1 to 8.2 mm, held compressed as before and converging at all three frequencies (the strips agree to 1e-4 to 3e-3): 166 s here where they took 38, 2.1 GB at the most; their function's limit on CI's macOS 1800 s (it took 145 s there). The narrow line's strips 13 s, 9 on the halved edges |

## The 2D reference

`eddy.py` is a cross-section's magneto-quasi-static solve: in each rectangle `I/(σA) + jω Σ L I = u` of its conductor,
`L` per metre `(μ0/2π)(⟨ln r⟩ to the source's image − ⟨ln r⟩ to the source)`, the mirror at x = 0 and the chuck's
images at y = 0; the signal carries +1, its two grounds −1/2 each (tied, as a coplanar port drives them - the strips
have no vias), the chuck none net. `alumina.py` holds it:

- **Direct current**: R 93.383982 Ω/m, `1/(σA)` of the signal and its grounds in parallel to every digit printed.
- **Refinement**: 1420, 3744 and 9450 filaments give 732.50, 731.96 and 731.44 Ω/m at 10 GHz, 1268.7 to 1267.3 at 30
  and 1796.3 to 1795.4 at 60: within 0.15 %. L (all of it, the metal's own too) 3.4995, 3.4506 and 3.4309e-7 H/m.
- **Conductivity**: at four times σ, R is 0.5005, 0.5020 and 0.5023 times as much - a thick metal's σ^-1/2, its
  current where a perfect metal's would be already at 10 GHz.
- **Wheeler's incremental inductance rule**, independently (`wheeler.py`): a boundary-element solve of the same
  cross-section in vacuum - panels of constant charge graded to 2 nm at the corners, the mirror and the chuck by images
  - gives C0 32.8915 pF/m and the external inductance `1/(c² C0)` = 3.38279e-7 H/m, which the eddy solve's `L − R/ω`
  is to 0.01 % (3.3831e-7 at 10 GHz, 3.3833e-7 at 60). Its `R = (Rs/μ0) dL/dn` is 758.7, 1314.1 and 1858.4 Ω/m: the
  eddy solve 3.6, 3.6 and 3.4 % under it. A right-angled corner is where Wheeler's rule is not exact, its share of
  the loss within a skin depth of it: as the skin depth goes, the gap closes - 2.75 % at 360 GHz, 2.13 at 2.16 THz,
  1.61 at 13 THz (δ 0.023 µm, filaments from 1 nm), about as δ^1/3, a 270° corner's field r^-1/3. Wide plates 10 µm
  apart, whose corners carry little, agree to 0.2 % (`wheeler.py plates`).

The smooth line's attenuation is then `R sqrt(εeff) / (2 c L)` (β is `ω sqrt(L C)`, so the metal's own inductance is in
it), and the substrate's `ω/(2c) εr/sqrt(εeff) (εeff − 1)/(εr − 1) tan δ` beside it: at the measured εeff, 7.77, 13.60
and 19.41 Np/m against the measured 7.66, 13.63 and 20.33.

## The measured line, full wave

The line's strips as `test_rf25_measured` calibrates them, three cells across the signal and none over 100 µm, the
substrate lossless (`probe.cpp`, `probe.txt`):

| | halved edges | graded at the gaps | the 2D solve |
|---|---|---|---|
| outermost cells, signal / grounds at the gaps | 8.33 / 45 µm | 4.17 / 5.63 µm | |
| εeff at 10, 30, 60 GHz | 4.854, 4.804, 4.815 | 4.914, 4.845, 4.843 | (measured 4.934, 4.874, 4.868) |
| α, Np/m | 6.20, 11.22, 16.45 | 7.62, 14.27, 21.14 | 7.73, 13.48, 19.21 at the graded εeff |
| R = Re(γZ), Ω/m | 682.9, 1206.8, 1740.3 | 763.7, 1401.1, 2024.6 | 731.4, 1267.3, 1795.4 |
| L = Im(γZ)/ω, H/m | 4.048e-7 | 3.710e-7 | 3.4995e-7 |
| Z, Ω | 55.1 | 50.2 | 47.2 (its L and C = β²/(ω² L)) |
| a perfect metal's α (dense) | 0, 0.003, 0.235 | 0, 0.001, 0.196 | |

So the halved edges left R 6.6 % under and L 16 % over - Z0 17 % over - and α 19 % under. Graded, εeff comes within
0.6 % of the measurement (1.6 % under on the halved edges), Z0 6 % over, and α 1.4 % under at 10 GHz but 5.8 and
10 % over at 30 and 60. What a perfect metal loses - the ports reaching one another past the line, a substrate wave -
is none at 10 and 30 GHz and 1 % at 60; the substrate's lowest wave is slower than the line up to 60 GHz (its index
2.11 there against the line's 2.2), so nothing leaks. The rest is the metal.

## Why the faces lose more as the frequency rises

`faces.py` is the full-wave's conductor model in the cross-section: a metal solved as its two faces is two rows of
ribbons, cut as the mesh cuts them (`gridLines()` as `rf25_mesh.cpp` has it), each with `Zs coth(γt)/w` of its own,
`Zs csch(γt)/w` to its twin and, on an edge cell, `(crowd − 1) Re(sheet)(1 + j)/w` on both faces' current
(`ownImpedances()`). It is the full-wave solve's R and L to 0.1 % at 10 GHz on both meshes (1.1 to 2.1 % at 30 and 60),
and on a second line, 20/6/100 µm of 3 µm gold, to 0.1 % at 10 and 30 GHz - the solve's conductor loss is its cells,
and what follows is theirs:

- **The faces' walls carry no current.** The walls of a metal solved as its two faces are vias between them, which
  carry the current between the faces and none along the line. A perfect metal's faces have L 3.0 % over the
  boundary-element solve's; given walls that carry it, 0.01 % (`walls.py`). On the graded mesh the line's L is 6 % over,
  and its C as far under (147.3 pF/m where the line's is 156.9): εeff comes out right, Z0 6 % over.
- **A narrow edge cell takes the crowding in late.** The 2D solve's outermost 4.17 µm of the signal, its wall in it,
  carry 0.491 of the half signal's current at 10 GHz and 0.504 at 60, a perfect metal's already. The faces' cell
  there carries 0.429 at 10 GHz, 0.467 at 60 and 0.499 only as the frequency goes to infinity: its `Zs/2w`, where
  the band's current runs over `2w + t` of metal, holds the current out of it at a finite frequency. Its R climbs
  with the frequency - 84 % of its high-frequency limit at 10 GHz - where the line's goes as the root of it.
- **The loss of such a cell is over the band's.** The band's own factor - its loss over `Re(sheet) |I|²/w` - is
  0.78 at 4.17 µm and 0.90 at 5.63 (`alumina.py bands`): under one, its current spread down the wall. `crowding()`
  says 1.036 and 1.110. At a perfect metal's distribution the graded faces lose 20 % over the line even with every
  factor one.

The two go opposite ways and cross near 10 GHz. Each band given its own factor, the graded faces lose 9.6, 7.2 and 6.1
% under the 2D solve (the narrow cell's current short); given their walls' current as well, 0.8 % under, 2.4 and 3.8
% over, L within 1 % (`walls.py`). That is what the faces would need: their edge cells' current and charge down half
the wall each, in the kernels of the cells' coupling - more than a loss factor.

## The edge factor

What `lines.py` scores, the attenuation of five lines by the faces against their 2D solves (the faces' L and C off
alike, their εeff right, so the error in α is `(1 + dR)/(1 + dL) − 1`):

| | a 50/25/270 µm line, 6.5 µm (the measured) | 100/50/250, 3 µm Cu (the recheck's) | 10/5/100, 1 µm Au | 20/6/100, 3 µm Au | 100 µm microstrip, 3 µm Cu |
|---|---|---|---|---|---|
| GHz | 10, 30, 60 | 2, 10, 30 | 10, 40, 100 | 10, 30, 60 | 2, 10, 30 |
| halved edges | −19.0, −16.4, −15.3 | −16.6, −10.8, −9.3 | −22.3, −28.8, −24.4 | −34.9, −31.3, −29.9 | −6.8, +0.6, +2.9 |
| graded at the gaps (g/4) | −1.4, +5.6, +8.9 | −12.3, −5.1, −2.3 | −9.6, −18.8, −11.6 | −19.9, −5.7, +1.4 | (no gaps) |
| graded to g/2 | −4.4, 0.0, +1.9 | −13.6, −7.2, −5.2 | −12.4, −18.2, −11.9 | −21.8, −10.8, −5.8 | |
| graded, but no finer than 2t | −4.4, 0.0, +1.9 | −12.3, −5.1, −2.3 | −10.6, −17.0, −10.1 | −22.2, −14.4, −11.0 | |

| edge factor | halved: mean, most | graded: mean, most |
|---|---|---|
| `crowding()` as it is | 18.0, 34.9 % | **8.6, 19.9 %** |
| without Pearl's fading | 17.9, 34.9 | 8.5, 19.9 |
| not held at one at least | 18.0, 34.9 | 9.3, 21.8 |
| faded to nought as the cell narrows (the bands' own, 1 − e^(−2.2w/t)) | 19.2, 37.1 | 16.0, 32.6 |
| the same without Pearl's fading | 19.2, 37.1 | 16.8, 37.0 |

A quarter of the gap is the best rule of these and the quasi-static solve's own; nothing in the factor did better than
the factor. Its worst are the thin film's 40 GHz, its metal 2.5 skin depths thick, and the narrow line's 10 GHz, its
outermost cells 0.83 µm, a skin depth wide: where the faces' current is held back the most.

## The tab and the calibration

- `tabMeshSettings()` (rf25doc.cpp): the Mesh node's settings, held compressed as the study is, the gaps graded -
  for the prepared mesh and for the solve job's, which its calibration strips are meshed with. `studyFullWave()`, which
  only decided the gaps, is gone.
- `Mesh::facing`: how many sides were graded at a gap; the calibration's strips report theirs, a line in the solve's
  account, so that a structure and its strips meshed unalike would show.

## Tests

| Test | What it holds |
|---|---|
| `aCoplanarLineOnAluminaIsTheMeasuredOne` (`test_rf25_measured`) | Meshed as the tab meshes it, its gaps graded. εeff within 2 % of the measurement (−0.4, −0.6, −0.5 %); α within 12 % of the 2D eddy solve's smooth line (−1.6, +5.5, +9.7 %; on the halved edges −20, −17, −14) and within 8 % of the measurement (−0.2, +5.2, +4.7 %; −19, −17, −18); the growth faster than the root of the frequency and within 8 % of the measured (+5.5, +5.0 %). Before: α under the measurement by no more than a quarter, the growth within 5 % |
| `aQuasiStaticSolveSaysWhatItDoesAndStops` (`test_rf25_doc`) | A full-wave study's mesh graded at the gaps as a quasi-static one's is, the same cells (it held it was not) |
| `aCoplanarPortIsReadAndItsFeedCalibrated` (`test_rf25_doc`) | The structure's four sides facing its 50 µm gaps graded (`Mesh::facing`), and its calibration's strips said to be meshed so: four sides |

## Timings

In Release here, the machine shared with two other sessions' builds and tests:

| | before | now |
|---|---|---|
| `test_rf25_measured` alone | 38 s | 166 s, 2.1 GB at the most (197 s in the full ctest beside the others) |
| `test_rf25_doc` | 18 s | 18 s |
| the narrow line's calibration (`probe.txt`) | 9 s | 13 s |

Every rf25 suite whole in Release (`test_rf25_mesh` 14, `_doc` 42, `_measured`, `_deembed` 19, `_fullwave` 31,
`_static` 26, `_sparams` 21, `_green` 20, `_inductance` 15, `_wave` 18, `_fast` 7, `_model` 13, `_layout` 11),
`test_qucs_control` 216/216, `test_mcp_server` 19/19, and the full ctest 160 of 161: `test_build_commit` had the
commit before another session's, made while it ran, and passes rebuilt at it. Nothing written to the user's cache.
Under the sanitizers (build-rf25-asan): `test_rf25_doc` 42 in 83 s (79 before), `_mesh` 14, `_deembed` 19 in 185 s,
`_measured` skipped as before, `_fullwave` in its two parts 17 in 286 s and 16 in 396 s, and `test_qucs_control`'s four
rf25 functions in 53 s, with no report. The four lints green.

## Breaks

`breaks.py` beside this file holds seven, each built and its tests run; all seven caught. They were run on a copy of
the source with a build of its own (`BRK_SOURCE`, `BRK_BUILD`): two other sessions were building from the checkout
meanwhile, and a break edited there would have been in their builds.

| Undone | Caught by |
|---|---|
| The tab grading a quasi-static study's gaps alone, as before | `aQuasiStaticSolveSaysWhatItDoesAndStops`, `aCoplanarPortIsReadAndItsFeedCalibrated` |
| The calibration strips meshed without the gaps, the structure with them | `aCoplanarPortIsReadAndItsFeedCalibrated` |
| The structure meshed without the gaps, its strips with them | both of the tab's |
| The tab's settings not held compressed for a compressed study (the helper's other half) | `aFullWaveStudysMatrixIsItsOwn` |
| The strips' grading not said | `aCoplanarPortIsReadAndItsFeedCalibrated` |
| The mesh not keeping how many sides it graded | `aCoplanarPortIsReadAndItsFeedCalibrated` |
| No gap graded at all | `aCoplanarLineOnAluminaIsTheMeasuredOne`, `aSideFacingANarrowGapIsGradedToAQuarterOfIt`, `aCoplanarPortIsReadAndItsFeedCalibrated` |

## Left

- **The faces' walls.** A metal solved as its two faces carries its current and its charge on the faces alone; its
  walls only join them. On fine edges its L is 3 to 9 % over and C as far under (εeff right, Z0 over), and its loss
  climbs with the frequency faster than the root of it - 5.8 % over the measured line's smooth loss at 30 GHz, 10 % at
  60. The cross-section says what would do: the edge cells' current and charge down half the wall each (walls.py:
  R within 4 %, L within 1 %), which is the partial inductances' and the potentials' kernels for those cells, not
  their loss.
- **A metal solved as a sheet** (the default) is a zero-thickness strip in the field: on these lines its L is 7 to 45 %
  over the thick metal's, which grading does not change (21.1 % from the 2D solves on average, 20.4 graded). Thick metal
  in narrow gaps wants its two faces.
- The narrow line (6 µm gaps, 3 µm gold) is still 20 % under at 10 GHz graded, its cells under a skin depth wide.
