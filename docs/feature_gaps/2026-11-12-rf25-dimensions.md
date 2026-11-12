# RF 2.5D's Dimensions: the file's own shapes moved by a parameter

Phase 4 of the plan to give RF 2.5D a Parametric Sweep and an optimizer (phase 3, `2026-11-12-rf25-own-shapes.md`, gave
a model shapes of its own whose numbers are expressions). An imported layout's shapes are still fixed numbers. Phase 4
gives them a parameter without drawing them again, the way a dimension parameter works in a planar EM tool: a box
round some corners, a way, and how far, as an expression.

## What there is now

- **A Dimension node under Layout** (`rf_dimension`; several may be added, and they apply in order). Each has a layer
  of the stack, a box (from x and y to x and y, each an expression), a way (+x, −x, +y, −y, or an angle from +x), and
  a By (an expression: `w - 50[um]`).
  - Every corner of that layer's shapes of the layout file inside the box moves by By that way. The box's edges count
    as inside, so a box drawn along an edge, even one of no height, holds that edge.
  - A box round a line's one edge widens it, one round a stub's end lengthens it, and one round a whole shape moves it.
    The model's own Shapes are not moved: their numbers are already expressions.
- **Processing** (`dimensionsOf()` and `moveCorners()` in `rf25_layout.{h,cpp}`, applied in `processRead()`): the
  file's outlines are moved before they join their layers, so the processing, the mesh, the solve and an export see the
  moved shapes.
  - The report says what each Dimension moved ("d1 moved 2 corner(s) of M1 by 30 µm"), or that its box holds no corner.
  - A shape it would make cross itself, or leave without area, is left as it was, and the problems name the Dimension.
  - A Dimension that cannot be read (a number that is none, a layer the stack has not) is left out and said.
- **On the tab:**
  - The Draw menu has a **Dimension** tool. A drag draws the box round the corners on the layer drawn on, snapped as
    the drawing tools snap. A click then gives the way: along x or y from the box's centre, whichever is further. The
    Dimension is added with By nought, as one step of Undo, and its node is chosen in the panel so its By can be tied
    to a parameter. `placeDimension()` does the same from code.
  - Each Dimension is drawn as its box, with an arrow the way it moves labelled with its By.

## Checked

- **`test_rf25_layout`'s `aDimensionMovesTheFilesCorners` (new)**, on a GDS of a strip, a pad apart from it, and
  another layer's strip over the edge:
  - A box round the strip's top edge widens it from 50 µm to w = 80 µm. The pad and the other layer are untouched, and
    the report says so.
  - A box of no height along the edge does the same, and so does an angle of 90 degrees.
  - A box round the whole strip moves it, at the same width.
  - A corner alone pulled across its shape leaves it as it was, and says so.
  - A box of no corner says so.
  - A By that is no number, or a layer the stack has not, is left out and said.
- **`test_rf25_doc`'s `aDimensionWidensALineOnTheTab` (new):**
  - The tool's drag round the strip's top edge and click above it add a +y Dimension on M1, chosen in the panel.
  - Its By tied to w = 80 µm widens the strip, and it is drawn with its By.
  - Solved full-wave, it gives the same S within 1e-9 as an 80 µm strip read from a GDS.
  - Undone three times, the strip is as the file has it, with no Dimension.
- All the RF 2.5D suites and `test_qucs_control` pass under ctest, as do the four lints.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.

## Not done

- A Dimension moves corners, not edges' middles: an edge with no corner in the box stays where it is.
- No tool of Claude's adds a Dimension yet (phase 8).
