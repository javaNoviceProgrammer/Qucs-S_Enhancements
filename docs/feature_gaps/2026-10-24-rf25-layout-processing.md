# RF 2.5D: the layer map applied and the layout processed

The user, after phase A's document and panel landed: "let's move to implementing the remaining parts of
RF 2.5D." The next piece in the proposal's own order is the one phase A left as settings with nothing
behind them - `2026-10-24-rf25-document-and-panel.md` says it plainly: "the layer map and the processing
steps are held and checked, but nothing turns shapes into a mesh". This round is that: the shapes of a
file become the conductors of the stack's layers, and every step says what it did.

## What it is now

`rf25/rf25_layout.{h,cpp}`:

- **`layerMapOf()`** reads the map's rows - layer, datatype, the stack layer, and an optional fourth cell
  marking the row as dummy fill. A datatype left empty is 0. A layer sent to two layers of the stack, and
  a row that names a layer the stack has not, are said rather than taken.
- **`processingOf()`** reads the `rf_processing` node: its six flags, and its three lengths as expressions
  of the model's parameters (`2[um]`, or a parameter's name), in metres like everything else in a model.
  A length that is no number is said and the default kept.
- **`processShapes()`** is the whole of the processing and knows nothing of files, so a test hands it
  polygons it wrote itself. In order, per layer: **heal**, **snap**, then either **merge vias** (on a via
  layer) or **merge**, then **drop slivers**; dummy fill is dropped before any of it when that is asked
  for. Each step counts what it changed and adds a line to the report ("M1: merged 2 shapes into 1",
  "V1: merged 9 vias into 1 shape, keeping 100% of their area").
- **`processLayout()`** puts it together: reads the file with `layout::read()`, takes the named cell or
  the one a view would show, walks it with `layout::search()` so that placed cells and repetitions are
  resolved, sends each shape where the map says, and processes what it gathered. A file that is not
  there, a cell the file has not, and shapes on layers the map does not name are each said.

## Two things the tests taught me

- **A union does not merge a via farm.** The first attempt clustered the vias by distance and united each
  cluster, which did nothing: a union joins what *overlaps*, and the vias of a farm do not touch. Merging
  by a distance is a morphological closing - grow every shape by half the distance, unite (which now
  bridges every gap below it), shrink back by the same half - and that is what it does now, which also
  did away with the clustering code entirely. Vias further apart than the distance stay apart, and a test
  holds that.
- **A rectangle is not damaged.** `QPolygonF` of a rectangle carries five points, the last closing the
  ring, so the first heal step counted every shape in a layout as healed. The closing point is how a
  polygon is written and is taken off without a word; a point repeated *along* the way is damage and is
  counted. The break that puts the closing point back fails two tests, which is the right shape for a
  mistake that quiet.

## Keeping a merged farm's area

The proposal asks for the choice, and names its cost: "the resistance of a merged farm is right to a few
per cent, the current distribution within it is not". The closing leaves a shape far larger than the vias
it stands for - nine 0.2 µm squares on a 0.4 µm pitch become one 1 µm square, 1.0 µm² against their own
0.36 µm² - so with *Keep the merged area* on it is shrunk back: each step takes off (extra area) /
(perimeter), repeated up to five times until it is within a per cent, never below the area the vias had
and never to nothing. On that farm it lands on 0.36 µm² exactly, and the report says what fraction it
kept so a reader can see when it did not converge. With the choice off the union is left as it is, which
is the honest alternative rather than a silent one.

## Tested

| Test | What it holds to |
|---|---|
| `theLayerMapSendsShapesWhereItSays` | Rows to layers, an empty datatype as 0, a row left out, the fill note, a layer sent twice, a layer the stack has not |
| `theProcessingIsReadFromItsNode` | The six flags and the three lengths, a length given as a parameter, and a length that is no number said with the default kept |
| `touchingShapesAreMergedIntoOne` | Touching rectangles joined and the area kept, shapes apart left apart, nothing said when merging is off |
| `aViaFarmIsMergedAndItsAreaKept` | Nine vias into one, their own area within a few per cent, the report's words, the union left large when the choice is off, and vias beyond the distance left apart |
| `sliversAreDroppedAndSaid` | A thin shape and a small one dropped and counted, all three kept when the step is off |
| `snappingHealingAndFill` | Points moved to the grid (and only then), a repeated point healed, fill dropped or kept as floating metal and marked as such |
| `whatCannotBeProcessedIsSaid` | A layer the stack has not, and a layout whose every shape was dropped |
| `aLayoutFileIsReadAndProcessed` | A real GDSII read end to end: nothing mapped means nothing solved and the count said, its layers sent to M1 give conductors with area, a file that is not there and a cell that is not there both said |

## Broken on purpose

`docs/feature_gaps/2026-10-24-rf25-layout-processing/breaks.py` holds nineteen, one or two for each
behaviour: merging that ignores its flag, no merging at all, a farm united without being closed, a
closing never undone, the area never kept, a closing four times too wide, a sliver's width unread, the
grid not rounded to, a repeated point not counted, a ring's closing point counted, fill dropped
unasked, fill not marked as floating metal, the three things the layer map must say, a length that is no
number passed over, a cell that is not there passed over, shapes on unnamed layers uncounted, and a
layout whose every shape was dropped reported as a success.

## What the breaks taught

Two of the nineteen were not caught on the first pass, and both were the test's fault rather than the
code's:

- **Undoing the closing was invisible** while *Keep the merged area* was on, because the shrink that keeps
  the area hid a growth that was never undone. The test now also says how large the shape may be with that
  choice off - about the 1 µm² the farm spans, not the 4 µm² of a growth left standing.
- **The ring's closing point** is only observable where the shapes come from `QPolygonF(QRectF)`, which
  is one test of the two the break named; the break asks that one alone now.

## Not done

- **The mesh.** The conductors are polygons; nothing divides them into cells yet, and the `rf_mesh` node
  is still read by nobody. That is the next piece, and the last of phase A.
- **The view does not draw the processed shapes.** The proposal asks for them over the originals, so that
  what the solver sees is what the user checks. The panel can report the lines; drawing them is the
  view's own round.
- **A sliver is told by its bounding box.** The smaller side of the axis-aligned box is exact for a
  rectangle and an over-estimate for a shape turned at an angle, which can keep a thin diagonal shape a
  true minimum-width test would drop. Real layouts are mostly axis-aligned, and the alternative (a
  minimum-width measure over every edge pair) costs more than it is worth here.
- **Vias are clustered by their bounding boxes**, not by their outlines, inside the closing's grow step -
  which is exact for the rectangles a via farm is drawn with.
- **Nothing is cached.** Every call reads the file again; a model with a large GDS will want the read kept
  beside the document, as the multiphysics model keeps its geometry.
