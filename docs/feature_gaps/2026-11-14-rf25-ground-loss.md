# A metal ground's loss in RF 2.5D's full-wave solve

The user asked whether RF 2.5D always takes the ground plane as perfect. It does, for the fields, in both solvers.
- **The quasi-static extraction** counts a metal or impedance ground's loss, its share of the line's R.
- **The full-wave solve** ignored that loss and didn't say so: on a copper microstrip it missed 14–22 % of the
  conductor loss.

The user asked for it to be added "as an option that the user can enable or not".

## The option

The ground node has a new property, **Its loss in the full-wave solve** (`fullwave_loss`).
- It is shown only for a metal plane or a surface impedance, and is off by default.
- On, the full wave counts the plane's loss. The fields stay a perfect plane's, as in the quasi-static extraction.
- **The notes say which:** with the option on, "The ground plane's own loss, a metal of … S/m, counted on the current
  each run's mirror image carries … N pair(s) of runs". With a lossy ground and the option off, "The ground plane is a
  metal of … S/m, and its loss is not counted here - its "Its loss in the full-wave solve" counts it; the quasi-static
  extraction always does."
- The control tools' description of the format names the property.

## How it is counted (`rf25_fullwave.cpp`: `groundPairsOf()`, `groundImpedances()`)

Over a perfect plane, the plane carries each run's mirror-image current, and that current is what loses power in a
real metal. A line's wave is quasi-TEM: at each cross-section the plane carries what the strip's own current there
makes it carry, spread across as a 2D solve spreads it, and nothing along the line.

Two planar runs a and b therefore share

  Zs × d_a·d_b × (how far they run side by side) / (w_a w_b) × ∬ (H/π)/(H² + s²) dy dy′, with H = h_a + h_b.

- The double integral over their widths is closed form: Φ(s) = (s·atan(s/H) − ½H·ln(s² + H²))/π at four corners.
- For a strip much wider than its height, the result is its own number of squares, as it should be.
- **Zs** is (1 + j)·√(πfμ0/σ) for a metal plane, its internal inductance included, or the impedance given.
- **Which pairs:** those within ten H across the line. Beyond that the kernel holds 6 % of the ground current but only
  0.04 % of its loss, which goes as the square.
- **The geometric part is computed once per structure.** Each frequency adds Zs times it through the same path as the
  metals' own impedances.
- **In the compressed solve,** the ground's entries go whole into the operator, but into the near field's LU only among
  the runs it already holds near. Otherwise ten heights of pairs would fill the LU.

**A first kernel was dropped.** The 3D static Poisson kernel H/(2π(ρ² + H²)^{3/2}), taken along the line too, counts
the coupling of runs along it. A wave's phase along the line makes that coupling cancel, e^{−kh} at k = β. At 50 GHz
it kept only 78 % of the ground's loss on 100 µm of alumina (5.83 against 7.50 dB/m), and 65 % on 203 µm of
laminate (2.06 against 3.15). The 2D kernel doesn't lose it.

## Checked

Copper microstrips over a copper plane, the ground's loss per metre in the full wave (from the power lost, option on
less option off, 4 mm line) against the quasi-static extraction's ground share:

| dB/m | 10 GHz | 20 GHz | 30 GHz | 50 GHz |
|---|---|---|---|---|
| 95 µm on 100 µm alumina, full wave | 3.31 | 4.74 | 5.81 | 7.50 |
| the quasi-static extraction | 3.34 | 4.73 | 5.79 | 7.48 |
| 450 µm on 203 µm RO4003, full wave | 1.43 | 1.99 | 2.47 | 3.15 |
| the quasi-static extraction | 1.44 | 2.03 | 2.49 | 3.21 |

Within 1 % on alumina and 2 % on the laminate, at both 2 and 4 mm, passive and reciprocal. On alumina that is 16 %
added to the conductor loss a perfect ground gives; on the laminate the quasi-static extraction has 29 %. The full
wave's L-2L there is too noisy to say: the 450 µm strip's ends radiate.

- **`test_rf25_fullwave`** `aMetalGroundsLossIsCountedWhereAsked` (new), 0.6 s:
  - option off: the perfect plane's answer exactly, and said;
  - option on: said, passive and reciprocal;
  - the ground's loss within 3 % of the extraction's at 10 and 30 GHz (3.312 against 3.343, 5.815 against 5.791);
  - the compressed solve within 1e-5 of the dense one.
- **`test_rf25_model`** `aGroundsLossInTheFullWaveIsReadWhereItCanLose` (new): read for a metal or impedance ground, not
  for a perfect or open one, nor where not asked; shown only for metal and impedance.
- **`breaks.py`** undoes each piece; all six breaks are caught. Results are in `results.txt` beside it.
- All RF 2.5D suites and `test_qucs_control` pass under ctest at `-j4`, on main as merged (8aa38cc6), and the four
  lints pass. The two new tests pass under ASan and UBSan (12 s and 17 ms).

## Not done

- **A via's current into the plane:** the plane's spreading loss around a via's foot is not counted.
- **Off by default:** a model saved before keeps its answer.
- **The fields stay a perfect plane's:** a plane thin beside its skin depth, or one whose loss changes the line's
  dispersion, is not modelled.
