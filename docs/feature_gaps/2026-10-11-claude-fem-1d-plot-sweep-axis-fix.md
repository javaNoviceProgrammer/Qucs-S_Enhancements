# Feature gaps: what a 1D plot group draws its graphs over

*11 October 2026 — Qucs-S 26.1.8.*

The review (`2026-10-11-claude-fem-1d-plot-sweep-axis-fix.md`) was written on
`8864128`. It read the source and proposed three fixes:

- Fix 1: no axis of one value.
- Fix 2: the sweep on x when an inner axis has several values.
- Fix 3: a variable named after its graph's label.

It also raised a unit of dB/cm for `emw.loss`.

`45d857e` (the semiconductor check) had already moved an axis of one value last, so
`pn_diode`'s graphs drew. That was half of Fix 1, and none of the rest. Each item was
reproduced on `45d857e` through `qucs-s --mcp-server`, with its own HOME, settings,
trash and cache:

- a waveguide's two modes over three widths wrote `<dep emw.neff mode a>`: x the mode
  number, a curve per width;
- a graph labelled "Index" was named `emw.neff`;
- `fem_evaluate` of `emw.loss` with unit `dB/cm` ignored it and showed the dB/m number
  as "0.1908 1/m". A graph did the same.

All of it is done here, on top of `45d857e`.

## Fix 1: no axis of one value

| Review | Now |
|---|---|
| A mode step with one mode, or a step with one output time or frequency, gave an axis of one value. With it first, the data display drew nothing: `<dep semi.Ckk frequency Va>`. Leave it out; the values stored do not move. | **Left out**, as proposed. `pn_diode`'s graphs are `<dep semi.I_a Va>` and `<dep semi.Ckk Va>`, with no `frequency` variable. The reviewer's loss against bias is `<dep emw.loss Vr>`. `45d857e`'s "an axis of one value last" is replaced by this. With nothing left (one solution, one output), the index of one is there as before. |

## Fix 2: what goes along x

| Review | Now |
|---|---|
| With several modes over a sweep, x was the mode number, a curve per bias. Put the sweep first and transpose the values. | **Done:** `<dep emw.neff a mode>`, a curve per mode over the width. The values are reordered with their axes, for any number of axes, the real and imaginary parts alike. |
| Should a mode analysis's frequency also go before the mode (a dispersion plot)? That would change the WR90 test's `{mode, frequency}`, deliberately. | **Yes.** `<dep emw.neff frequency mode>`, a curve per mode over the frequency, and the WR90 test now asserts that. Its TE20 at 20 GHz is still value 3: with two of each, the index is the same either way. |
| Fix 2's code moved the sweep first for any inner axis. | Except a transient's times and a spectrum's photon energies (or wavelengths). Those are read over their own axis: temperature against time, a curve per power; absorption against energy, a curve per field. Moving the sweep first would have drawn the temperature against the power, a curve per output time. The modes and a quantum well's states go last in every case. A frequency step's frequencies go after the sweep, as proposed: C against the voltage, a curve per frequency. |
| | Neither order suits every study (C–V or C–f), so a 1D plot group has a **Plot against** choice: Automatic (the order above), The sweep's parameter, or The times, frequencies or modes (the step's own axes first, the sweep last). |

A line graph keeps its own axis (the distance, x or y) first, as before.

## Fix 3: names

| Review | Now |
|---|---|
| A graph's variable is its expression through `safeName` (`emw.loss/100` → `emw.loss_100`). A graph with a label could give it its name, when it has one expression. | **Done, for all three kinds.** A global graph of one expression with a label is named by the label (`Index`). So is a point graph (`Tmid`, and `Tmid_1`, `Tmid_2`… for several points) and a line graph (`Tline`). `45d857e`'s `name = expression` is first: it names each of several expressions. None of the examples' graphs has a label, so their datasets keep their names. |
| A unit on the graph (`dB/cm` for `emw.loss`) would be better than `/100`. Not tried. | **It did not work:** dB is not a unit, so `dB/cm` was refused and the value shown in SI under "1/m". Now a global with a unit of its own (`emw.loss`, dB/m) takes it over another length: `dB/cm`, in a graph, a Global Evaluation and `fem_evaluate` (as a global or at a point). A unit of its dimension still converts it (`1/cm`). A unit it cannot take is said, and the value shown in its own unit: "dB/furlong is not a unit of emw.loss (dB/m)", "0.19083 dB/m". The bundled `pn_phase_shifter` graphed `emw.loss` with unit `1/m` (so dB/m, labelled 1/m); it now uses `dB/cm`. |

## Test to add, as proposed

- **`aDiodeSwept`**: `semi.I_a` and `semi.Ckk` over `{Va}` alone, three values each, and
  no `frequency` variable.
- **`aWaveguidesModes`**: `{frequency, mode}`.
- **`graphsOverASweepOfModes`**: two modes over three widths. Its checks:
  - `{a, mode}`, TE10 at each width, then TE20 (`re.at(1)` is TE10 at the second width);
  - `name = expression`, the label's name, and `emw.loss` in dB/cm (the second width's
    TE10, its global over 100);
  - Plot against the modes: `{mode, a}`;
  - one mode over the sweep: `{a}`, with no `mode` variable;
  - `fem_evaluate` in dB/cm as a global and at a point, and `dB/furlong` said, the value
    in dB/m.
- **`graphsInTimeOverASweep`** (new): a slab heated in time, swept over its power. Its
  checks:
  - `ht.Tmax` over `{time, P}`, twice as fast at twice the power;
  - a labelled point graph the same way;
  - Plot against the sweep: `{P, time}`, each value moved with its axes;
  - a labelled line graph over `{x, time, P}`.

Each of the 15 changes was broken in turn, and its test failed.

The full ctest, the MCP scenario script and the DMG's checks (now with this scenario)
pass.

## The workaround's second half

The review suggested editing the `.dat` by hand until then. That is no longer needed;
showing the plot group writes it as above.

Separately, CI's run of `138aba0` stopped `test_multiphysics_doc` at ctest's 300 s, under
the sanitizers. It now has 900 s, as the `test_fem_` tests do (`4c33044`).
