# RF 2.5D's own shapes: geometry its parameters drive

Phase 3 of the plan to give RF 2.5D a Parametric Sweep and an optimizer (phases 1 and 2:
`2026-11-12-optimizer-driver-shared.md`, `2026-11-12-rf25-solve-untabbed.md`). The usual RF knobs - a line's width, a
stub's length - are geometry, and until now an RF 2.5D model's geometry came only from its layout file as fixed
numbers. That included what the Draw tools drew, which they wrote into the file. Parameters drove the stack, the
ports, the mesh, the cutout and the sweep, but no shape.

## What there is now

- **A Shapes node under Layout** (`rf_shapes`, at most one), holding Shape nodes (`rf_shape`). Each Shape sits on a
  metal or a via of the stack, or on a dielectric or the substrate where that layer ends. It is one of these, and every
  number in it is an expression of the parameters (`w`, `L/2 + 5[um]`):
  - **a rectangle**, by its lower left corner or its centre, its width and its height;
  - **a polygon**, by its corners;
  - **a path**, by its points along the spine, its width and its ends (flush, round, or extended by half its width).
    It is outlined by gdstk, as a path the Draw tools write to the file, so its joins and ends are the same;
  - **a square via**, by its centre and its side.
- **Processing** (`ownShapesOf()` in `rf25_layout.{h,cpp}`, called by `processRead()`): each Shape is evaluated in the
  model's parameters and added to its layer with the file's shapes, before the processing's steps. So it is merged,
  cut out, meshed, solved and exported (HFSS 3D Layout from the processed shapes) like a shape of the file. The
  processing's report counts them.
  - A Shape that cannot be read is left out and the problems say why. That covers a length that is no number, a
    polygon of fewer than three corners, a path of one point or no width, no area, a layer the stack has not, and a
    ground plane, which has no shapes.
- **A model of its own shapes alone needs no layout file.** The model no longer reports that the layout names no file,
  and the tab processes the shapes over an empty layout of one cell. That empty layout goes away when the last Shape
  does.
  - The layout line says "No layout file: the model's own shapes alone".
  - Zoom Extents fits the shapes.
- **On the tab:**
  - The Shapes are drawn filled in their layer's colour, under the processed outlines, as the layout's shapes are.
  - In Select, a click on one chooses its node, so the panel shows its numbers.
  - A parameter edited moves them at once, and Undo puts them back.
- **Drawing them:**
  - The Draw menu's **Keep as Parametric Shapes**, which is remembered for the next model, makes the Rectangle, Polygon
    and Via tools add a Shape node instead of writing to the file. Its numbers are plain lengths (`120[um]`) for the
    user to replace with parameters.
  - `drawShapes()` (and Claude's `rf25_draw`) draws one where an edit says `"parametric": true`, including paths. A call
    is all such shapes or none, and a Shape is not deleted the way a file's shape is: it is deleted under Layout >
    Shapes like any node.

## Checked

- **`test_rf25_layout`'s `theModelsOwnShapesAreItsParameters` (new):**
  - Each kind's outline: a rectangle from L = 2w + 100 µm, one by its centre, a polygon of area 1250 µm², a via, and a
    path whose area is 1000 µm² flush, 1100 extended and 1000 + 25π round.
  - A parameter changed moves the rectangle.
  - The shapes are processed over an empty layout on their layers, and counted.
  - The model says nothing of a missing file.
  - Each unreadable Shape is left out and said.
- **`test_rf25_doc`'s `aModelsOwnShapesAreDrawnSolvedAndVaried` (new):**
  - A strip that is a Shape, L by w, with no layout file: the tab says so, draws it filled, and fits it.
  - It solves full-wave to the same S, within 1e-9, as the same strip read from a GDS.
  - w changed widens it, and Undo narrows it again.
  - A click in Select chooses its node.
  - The Draw menu's choice draws a node, not into a file, as one step of Undo.
  - A call draws a round-ended path of the model's own; a mixed call is refused, and so is a delete.
- All the RF 2.5D suites and `test_qucs_control` pass under ctest, as do the four lints.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.

## For Claude

`rf25_draw`'s description says how to draw a Shape (`"parametric": true`). No tool yet edits an RF 2.5D model's nodes,
so writing a Shape's numbers as expressions is done in the panel or in the file. Phase 8 is to add one.

## Not done

- A Shape is not selected in the view, as a file's shape is: the click chooses its node in the panel.
- Phase 4 moves the edges of the file's own shapes by parameters; Shapes do not.
