# What RF 2.5D's sweeps and optimizations read of a solution

Phase 5 of the plan to give RF 2.5D a Parametric Sweep and an optimizer (phases 1 to 4: the shared driver, the solve
without the tab, the model's own shapes, the Dimensions). A sweep's quantity and an optimization's objective are
expressions of what a solve found, and of the parameters, over some of its frequencies. Phase 5 gives them names to
read and a way to judge a row of frequencies. It adds no user interface: phases 6 and 7 use it.

## What there is now: `rf25/rf25_optimize.{h,cpp}`

- **`solutionValues(solution, model)`** gives a solution's values by name, at each of its frequencies:
  - **Full wave**, for each pair of ports i, j:
    - `S21_dB`, `S21_mag`, `S21_deg`, `S21_re`, `S21_im`;
    - `Z21_re`, `Z21_im`, `Z21_mag` (Ohm);
    - `GD21`, the group delay −dφ/dω (s), from the phase unwrapped along the sweep: central between neighbours,
      one-sided at the ends, absent with one frequency;
    - `VSWR1` for each port.

    Each pair is written `S2_1` as well, which is the only form past nine ports.
  - **Quasi-static**, at each of the study's frequencies:
    - `C11`, the capacitance matrix (F);
    - for a uniform line, `Z0`, `eps_eff`, `R`, `L`, `G`, `C` per metre and `alpha_dB` (dB/m) of its first net, and
      `Z0_2`… of the others. These are what `asLine()` says at that frequency;
    - for the loop two ports make, `Lloop` and `Rloop`.
  - `f`, in either kind.

  Every value carries its unit, so an expression's units are checked (`Z0 - 50[Ohm]`).
- **`seriesOf()`** gives an expression's value at each frequency within a window, both ends included. An expression it
  cannot read is refused with the names the solution has.
- **`readingValue()`** judges a `Reading` (the expression, its window, its Over) as it says:
  - **worst**: by its goal, as an optimization's goals judge a sweep (`optimization::measure`): a MIN's largest, a
    MAX's least, a target's furthest from it;
  - **last**, **min** or **max**;
  - **at** one frequency, read between the two about it. A frequency outside the sweep is refused.
- **`overOf()` and `frequencyOf()`** read a row's words and frequencies (`2.4[GHz]`, an expression). A frequency in
  another unit is refused: the expression code only warns of a unit that disagrees, and here that warning refuses it.

## Checked

- **`test_rf25_optimize` (new suite)**, on a two-port made up in closed form (S11 0.1, S22 0.2, S21 = 0.9 e^(−jωτ)):
  - every name, the decibels, the degrees, both ports' VSWR, and Z's parts with their unit;
  - the group delay exactly τ, also at a delay whose phase wraps between the frequencies;
  - no group delay at one frequency;
  - a window both of whose ends are in it;
  - the worst for MIN and for MAX, the last, the least and most, and between two frequencies at one;
  - refused: no frequency for at, one outside, a name it has not, a window with no frequency;
  - the words, and a frequency's unit.
- **`test_rf25_doc`'s `aQuasiStaticSolutionsValuesAreReadByName` (new):** a strip solved quasi-static without its tab.
  At each of the study's frequencies, C11 is the extraction's, Z0, ε_eff, R, C and α are its line's, and Lloop is its
  loop's, each with its unit. `Z0 - 50[Ohm]`, read last, is the top frequency's.
- All the RF 2.5D suites and `test_qucs_control` pass under ctest, as do the four lints.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.
