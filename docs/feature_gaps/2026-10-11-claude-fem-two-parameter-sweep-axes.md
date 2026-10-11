# Feature gaps: a 1D plot group over a sweep of two parameters

*11 October 2026 — Qucs-S 26.1.8.*

Reported in conversation; there is no review file for it. A sweep of two parameters
wrote its graphs against a plain index of the points, not against `V_CE`. It did so even
when one of the two had a single value. The reviewer drew their display from exported
values instead.

This is the same family as the axis fix in `fc28dc0` (no axis of one value). That fix
dropped an output time, frequency or mode of one value, but not a parameter of one
value. On `94d32e7` a 1D plot group named its sweep axis only when the sweep had one
parameter. Any other sweep got `sweep` = 1, 2, 3…, even one of two parameters where only
one changed. Reproduced by `aSweepOfTwoParameters` against `94d32e7`'s
`multiphysicsdoc.cpp`: `<dep ht.Tmax sweep>`, not `<dep ht.Tmax P>`.

## Now

| Sweep | Before | Now |
|---|---|---|
| Two parameters, one of a single value (`kt` = 2, `P` = 1e5, 2e5, 3e5) | `<dep ht.Tmax sweep>` over 1, 2, 3 | `<dep ht.Tmax P>` over the powers. The parameter of one value is no axis, as `fc28dc0` has it for the steps. |
| All combinations of two parameters of several values each (`P` × `kt`) | `<dep ht.Tmax sweep>` over 1…6 | `<dep ht.Tmax kt P>`: an axis each. The last parameter listed is the faster one (the sweep's own order), so it goes along x, with a curve for each value of the other. Listing V_BE, then V_CE gives I_C against V_CE, a curve per V_BE. The values are not moved; the axes say where each one is. |
| Specified combinations with one parameter changing (`kt` 2, 2, 2; `P` 3e5, 1e5, 3e5) | `sweep` | `P`, with its values as given, one per point. A single swept parameter did this already. |
| Specified combinations with two changing together (`kt` 1, 2; `P` 1e5, 2e5) | `sweep` | `sweep`, as before: no one parameter says where a point is. The same goes for specified combinations that miss a combination of the grid, or that hold all of them but in another order (the first parameter faster). |

The axes are found from the solutions' own parameter values. A parameter's values,
each taken once, make an axis. The combinations must then be exactly the grid of those
axes, in the sweep's order, with the last parameter the faster. "All combinations" always
is that grid. Specified combinations are when the user wrote them so.

The sweep's axes take their place among the others, as before. A transient's times and a
spectrum come first, then the sweep's parameters, then a frequency step's frequencies and
the modes. **Plot against** chooses as before. Its choice "The sweep's parameter" is now
"The sweep's parameters".

## Test

**`aSweepOfTwoParameters`** (in `test_multiphysics_doc`) runs a heated slab, stationary,
with `T − 300 K` proportional to `P/kt`, through six sweeps:

- `kt` of one value, `P` of three:
  - `{P}`, and the axis is 1e5, 2e5, 3e5;
  - there is no `sweep` or `kt` variable;
  - `T − 300` at 3e5 is three times that at 1e5.
- `P` of three values, then `kt` of two, all combinations:
  - `{kt, P}`, the axes 1, 2 and 1e5, 2e5, 3e5;
  - each of the six values is where its axes say: `re[j + 2i]` is `P_i/kt_j` times the
    first sweep's.
- Specified, with `P` alone changing (3e5, 1e5, 3e5): `{P}`, the axis as given, and the
  first value is that of 3e5.
- Specified, both changing together: `{sweep}` = 1, 2.
- Specified, three of the four combinations: `{sweep}`, of three.
- Specified, all four combinations with `kt` the faster: `{sweep}`, of four.

It passes in Release and under ASan/UBSan (with `graphsInTimeOverASweep`,
`aPlotGroupShownAgain` and `aDiodeSwept`, 28.6 s, no sanitizer report).

## Breaks

Each break was built and the test run against it. All of them failed it:

| Break | Fails at |
|---|---|
| `94d32e7`'s code (the index) | `dependencies` `sweep`, not `P` |
| The axes in the listed order (the first parameter the faster) | `{P, kt}`, not `{kt, P}` |
| No check that the combinations number the grid's | Three of four combinations given two axes |
| No check of each point against the grid | All four with `kt` the faster given two axes |
| One parameter changing: its values each once, not as given | The `P` axis's size |
| A parameter of one value kept as an axis | `{kt, P}`, not `{P}` |
