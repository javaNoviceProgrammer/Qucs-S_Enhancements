# Hiding a trace in a diagram

The request: in a 1D diagram's dialog, a "hide" option in the drop-down, so a curve can be hidden and shown again quickly instead of deleted.

## What it does

In a Cartesian or a stacked diagram, the Data tab's **Shows** box has a last item, **hide**.

**Hidden, a trace:**
- keeps everything it was: its part (dB, phase...), colour, style, marker, axis and pane;
- is not drawn: no curve, no row in the legend, no name on an axis, no markers, and no new marker can be set on it;
- is left out of the axes' automatic limits, so the axes fit the traces shown.

In the dialog's list it is shown in italics, faint, with a tooltip.

**Shown again:** choose what it shows (auto, dB...) in the Shows box. It comes back as that, the diagram laid out again even though its data did not change.

**Saved** as a thirteenth field of the graph's line (`... 0 0 1>`), which versions before it do not read: there it is shown.

Not offered for a table, a 3D, histogram or contour diagram (each lays out its graphs its own way), nor a Smith or polar chart (no Shows box).

**Claude's tools:**
- `add_trace` and `edit_trace` take `"hidden": true` or `false`, for a Cartesian or a stacked diagram's trace;
- a diagram's description says `"hidden": true` for a hidden trace.

Spec limits still check a hidden trace: hiding is what is drawn, not what is measured.

## Checks

| Check | Result |
|---|---|
| A Cartesian diagram's Shows box | 7 items, the last "hide"; a table's 6 |
| A magnitude trace hidden in the dialog, Apply | Hidden, its part still magnitude; its row italic, the tooltip "Hidden..."; the other's not |
| Selected again | The box shows hide; magnitude chosen: shown, as magnitude |
| The y axis's top, two traces (1 and up to 6) | 6 shown, 1 with the larger one hidden |
| The legend's height | 34 and 21 |
| The line saved and loaded | Hidden, its part, no ghost; shown: not hidden |
| Drawn, the y axis 0 to 25 | The hidden trace's red: no pixel; the other's blue drawn; shown, both |
| Autoscaled: hidden, then shown, its data unchanged | The y axis 0 to 1, then to 21 or more, the red drawn |

## Break checks

| Break | Caught by |
|---|---|
| A hidden graph painted | `aHiddenGraphNotDrawn` |
| A hidden graph in the axes' limits | `aTraceHiddenInTheDialog`: the axis's top 6 |
| A hidden graph in the legend | `aTraceHiddenInTheDialog`: the legend's height |
| Hidden not saved | `aTraceHiddenInTheDialog`: the line read back |
| Not laid out again when one is shown | `aHiddenGraphNotDrawn`: the axis's top 1 |
| The dialog's hide item taken as a part | `aTraceHiddenInTheDialog` |
| A hidden graph's part lost | `aTraceHiddenInTheDialog` |

At first a painted hidden graph was not caught: hidden, it was off the autoscaled axis anyway. The drawing check now uses a fixed axis, and the autoscaled case checks showing again.
