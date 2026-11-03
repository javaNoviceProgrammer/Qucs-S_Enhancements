# The layout viewer's tools for Claude, 2 November: the selection, rulers, where a cell is placed, texts by what they say, the layers' colours, the view written to a file

The user asked what the GDS/OASIS layout viewer lacks and whether Claude's tools control an open layout fully. The
review found a sound read-only viewer whose tools covered reading and framing - `get_layout`, `find_shapes`,
`show_layout` with its pen, `read_drawings`, `send_input` - but not several of the tab's own features, and the
gaps it named first as the cheap ones were closed here (the user's "Yes, start"). Editing, the viewer's largest
gap, is a round of its own and was not begun.

| Gap | Now |
|---|---|
| What the user selected could not be read, and Claude could not select a shape for the user | **The selection, read and set.** `show_layout`'s answer and `get_layout`'s tab give what is selected, as `find_shapes` gives a shape. `show_layout` `select` [x, y] selects what a click there selects - the smallest shape or text, of the layers shown; at the same point again, the next one under it - brought into view at the zoom shown; `none` clears it |
| The user's rulers could not be read; Claude could not show a dimension, or measure snapped to the shapes | **Rulers, read and added.** Each ruler's ends, length, run across and up, and angle, in the same two answers. `show_layout` `rulers` [[x1, y1, x2, y2], ...] adds rulers, each end snapped as a ruler's drag snaps it - to a shape's corner, else an edge, within a few pixels at the zoom shown; `clear` takes them away |
| Nothing said where a cell is placed, only how often | **`find_placements`** (new): every placement of a cell in another and in the cells it places, to a depth - each copy of an array -, with the cells it is placed through, its origin, rotation, mirror and magnification as they come together in the cell searched, its bounds there, which copy; in a region; counted to 100,000 |
| Texts could not be found by what they say | **`find_shapes` `text`**: the texts that say it - a part of it, or a pattern with `*` and `?`, in any case, as the cell panel's find matches names |
| The layers' colours, a `.lyp`, the palette, the canvas's theme, the colours' contrast and the grid's style: none set by a tool | **In `show_layout`**: `colours` {layer: colour}, `layer_properties` (a `.lyp` beside the layout or anywhere, or `palette`), `theme`, `contrast`, `grid_style`, `reload`. `get_layout` gives each layer's colour, to name it by |
| The view could not be written to a file: `screenshot` hands Claude a picture, and `export_image` took schematics and slides | **`export_image` of a layout**: its tab's view as it shows it - the cell, the region, the layers, the selection, the rulers, what the pen drew - as a picture of `scale` times its pixels (2 by default), an SVG or a PDF, as vectors |

## What is selected

The view selected on a click: the smallest shape or text under the pointer, of the layers shown and to the depth
drawn, the next one under it on a second click (`LayoutView::selectAt`). Claude now does the same at a point in µm
of the cell shown - the point taken to the view's pixels so that the click's own tolerance, four pixels, is the
one used - and the shape selected is brought into view when it is out of sight, the zoom kept. A point with nothing
under it says so (`not selected`) and leaves nothing selected.

What is selected, the user's or Claude's, comes in `show_layout`'s answer and in `get_layout`'s `tab` as
`find_shapes` gives a shape: its kind, layer and the layer's name, the cell it is in and the cells it is placed
through, its points and bounds (a path's width, spine and ends; a text's text and place), which copy of a
repetition, its properties. `find_shapes`' own builder of that object is the one both use now.

## Rulers

A ruler's drag snaps each end to the nearest corner, else edge, of a shape shown within six pixels - one and a half
times the click's tolerance - and on the database grid where there is none (`snap()`). The view's own snapping is
`LayoutView::snapped()` now, which the two drag handlers and `show_layout` share, so Claude's rulers snap as the
user's do. Given ends from `find_shapes` they stay where they are; given ends read off a picture, they find the
corners meant. Each ruler, Claude's or the user's, is said with its ends, its length, how far across and up it runs,
and its angle from the x axis, counterclockwise. The ruler drag's own Shift lock (across or up only) needs no tool:
Claude gives the ends it wants.

## Where a cell is placed

`find_placements` walks down from the cell searched as `search()` does - each placement's transform the product of
every level's, a repetition's copies those that meet the region - but looks only into cells that place the target
somewhere below them. Those are found once, up from the target through each cell's parents, so an array of a million
cells that place it nowhere costs nothing, and a cycle cannot hide one. (A layout's own cycles are cut where the
reader cuts them: "PING places PONG, which places it: that placement is left out.") The copy-numbering `search()`
already did for shapes is `copyAt()` now, which both use.

Each placement: the cell it is in, the cells it is placed through, its origin in µm in the cell searched, its
rotation in degrees counterclockwise after the mirror about x - read from the composed transform, so a turned
placement inside a mirrored one is said as it is -, `mirrored` and `magnification` where they are, its bounds there,
and which copy of an array. In the sample: INV at (10, 0); at (20, 0) turned 90°, its bounds (19, 0)-(20, 2); at
(30, 0) mirrored, (30, -1)-(32, 0); VIA's 3-by-2 array as six placements, the one at (1, 1) its fifth copy. Down a
chain of cells, LEAF in C2 is found at (1.5, 1.5) through C2 > C1 > C0, at depth 2 and not at depth 1. An OASIS
array of 65535 by 65535 is counted to 100,000 within the test's 0.4 s. (GDSII's array counts are 16 bits: the same array read from
GDSII is one copy.)

## Texts by what they say

`find_shapes` `text` keeps the texts that say it - a part of it, or a pattern with `*` and `?`, in any case, as the
cell panel's find matches cells' names. `OUT`, `o?t` and `U` each find the sample's three texts; `IN` none. With
`texts` false it is refused, since it would find nothing.

## How the layers look

- `colours` {layer: colour} - each layer as `layers` names them (`"1/0"`, `"1"`, a name), each colour a name or
  `#rrggbb` - sets the layer's frame and fill, as the layer list's Colour… does (`LayoutDoc::setLayerColour()`, which
  that menu calls now too). Every colour is checked before the tab is touched; a layer that is none is refused.
- `layer_properties` takes a KLayout `.lyp`: named alone, the one beside the layout first, else as a path from the
  project or workspace; `palette` gives the palette's colours again.
- `theme` (application, light or dark), `contrast` (the layers' colours darkened or lightened till they stand out
  from the canvas, or as they are) and `grid_style` (dots, lines or crosses) are set as the tab's menu sets them: in
  every layout tab, kept for the next.
- `reload` reads the file again first, the cell, view and layers kept; the read goes on in the background and the
  rest of the call waits for it, as a tool on a tab still reading always did.
- The answer says the layer properties (the file, or the palette), and the theme, contrast and grid style where they
  are not the defaults; `get_layout` gives each layer's colour, so Claude can say "the orange layer".

## The view written to a file

`export_image` given a layout - its tab, its file, or the tab in front - writes its view as the tab shows it. The
view's painting was split in two for it: the layout's picture, then what goes over it (the scale, the selection,
the overlays and their legend, the rulers, the pen's strokes) in `LayoutView::paintOver()`. `paintOnto()` draws the
grid, the layout afresh with `render()` and then `paintOver()` onto any device, in the view's own pixels at the
device's resolution:

- a picture (png, jpeg, bmp, tiff, webp) of `scale` times the view's pixels, 2 by default, within the schematics'
  largest picture (100 megapixels, 32000 a side), its resolution set (96 dpi times the scale); at 1, the view's own
  picture, pixel for pixel within a percent;
- an SVG or a PDF of the view's size, as vectors - the shapes drawn as paths (those under two pixels as points, as
  on screen), not as a picture;
- `transparent` leaves the canvas out where the format can.

EPS and PDF+LaTeX, grayscale and monochrome, `diagram`, `selection` and `slides` are a schematic's or a
presentation's, and refused for a layout with that said.

## Two corrections to the review

The review said the ruler had no angle lock: it has one, Shift (across or up only). And a click on shapes lying over
each other selects the smallest first, then each larger one in turn - which is what `select` does too.

## Tests

| Test | What it holds |
|---|---|
| `find_shapesFindsTextsByWhatTheySay` (new) | `text` by a part, by a pattern, in any case; with a region; empty, and with `texts` false, refused |
| `find_placementsFindsWhereACellIsPlaced` (new) | INV's three placements - origin, rotation, mirror, bounds -; VIA's six copies, the fifth at (1, 1); one in a region; none in a cell that places it not, said; a cell that is none; `limit`; LEAF down a chain at (1.5, 1.5) through C2 > C1 > C0, at depth 2 and not 1; a cycle cut as the reader cuts it; an OASIS array counted to 100,000 |
| `show_layoutSelectsAndMeasures` (new) | The path then the rectangle at the same point; in `get_layout`'s tab; nothing there said; the user's own click read; `none`; a shape out of sight brought into view at the zoom shown; a ruler's ends snapped to the corners near them, its length, across, up and angle; one far from any shape where asked, on the grid; the user's ruler read; `clear`; what is not right refused |
| `show_layoutColoursTheLayersAndTheCanvas` (new) | Two layers coloured, `get_layout`'s colour; a bad colour or layer refused with none changed; a `.lyp` loaded, the palette again, one that is none refused; a `.lyp` named alone found beside a layout in a folder of its own; dark, no contrast, lines, said and in `get_layout`; back as they were; a file written again and read again, the cell kept |
| `export_imageWritesTheLayoutsView` (new) | Twice the view's pixels; at 1 the view's own picture within a percent, the layout drawn; an SVG and a PDF; a transparent PNG's canvas left out, a JPEG's note; EPS, PDF+LaTeX, `diagram` and grayscale refused |
| `theToolsAreListed` (`test_qucs_control`) | `find_placements` listed, every description within 2048, every argument described |

## Broken on purpose

`docs/feature_gaps/2026-11-02-layout-mcp-gaps/breaks.py` holds thirty-seven:

- **The selection:** show_layout's answer and get_layout's tab without it; a point taken as the view's pixels; a
  shape out of sight left there, or brought into view at another zoom; `none` clearing nothing.
- **The rulers:** not snapped; `clear` clearing nothing; the angle measured clockwise; the view's snap within no
  distance.
- **Where a cell is placed:** the turn read clockwise; a mirror not said; the cells placed not looked into; the depth
  not held to; an array's first copy alone; the region not held to; no cell taken as placing it.
- **Texts:** `text` finding every text and shape; a text matched in one case only; `text` with `texts` false taken.
- **How the layers look:** a layer's colour its fill alone; a colour that is none taken; a `.lyp` beside the layout
  not looked for; the palette not taken again; the theme, the contrast, the grid's style not set; `reload` reading
  nothing again; get_layout without a layer's colour.
- **The view written:** at the view's pixels whatever the scale; the picture without the layout; `transparent`
  ignored; a layout exported as a schematic; EPS, a schematic's `diagram` and grayscale taken.
- **The painting split in two:** the view not drawing what goes over the layout.

All thirty-seven are caught, on the first run. Two checks were made stronger as the breaks were written, before
they ran: the `.lyp` named alone is now beside a layout in a folder of its own (in the workspace's root, where the
tests' layouts are, a path from the workspace found it all the same), and EPS's refusal is held to its words (a
writer that cannot write EPS refused it too, for another reason).

Rebuilt after the breaks: the full ctest (153 tests in 313 s), the scenario script (97 checks) and the four lints
green, nothing written to the real cache. And through the built app's own `--mcp-server`, every folder isolated, a
drive of the new surface on the repository's heater layout: 17 of 17 - each layer's colour, a top cell placed
nowhere said so, a shape selected at a point, a ruler snapped to its corners with its length, a layer coloured, the
canvas dark and the grid crosses (in `get_layout` too), the view written as PNG at twice its pixels, SVG and PDF, and
all of it put back.

## Under the sanitizers

Built Debug with ASan and UBSan, as CI's Linux job is, each function alone:

| Function | Time |
|---|---|
| test_layout_doc `find_shapesFindsTextsByWhatTheySay` | 1 s (5 s with the process's start) |
| test_layout_doc `find_placementsFindsWhereACellIsPlaced` | 1 s, the 100,000 copies of the OASIS array among it |
| test_layout_doc `show_layoutSelectsAndMeasures` | 1 s |
| test_layout_doc `show_layoutColoursTheLayersAndTheCanvas` | 2 s |
| test_layout_doc `export_imageWritesTheLayoutsView` | 1 s |
| test_layout_doc `overlaysAreDrawnOverTheLayoutWithinTheirBudget` | 1 s |
| test_qucs_control `theToolsAreListed` | 1 s |

The layout suite whole took 20 s; no sanitizer said anything.

## Not done

- **Editing**: drawing boxes, polygons, paths and vias, moving and deleting, writing GDS/OASIS - the viewer's largest
  gap, and the one the RF 2.5D assessment's "every test layout came from scripts" points at. A round of its own.
- **Measuring more than a ruler**: a polygon's area and perimeter, a layer's area or density in a region. Claude can
  work them out from `find_shapes`' points; the user cannot from the tab.
- **More than one shape selected**, or everything on a layer; **net tracing** through vias; a **go-to coordinate**
  field, **saved views** and back and forward - the tab's own, not its tools'.
- **A cross-section from the open tab's ruler** by a tool: `fem_edit`'s import cuts one from the layout's file, which
  is the same model; the tab's menu item is reached by `set_ui`.
- **Rulers kept with the file**: they live while the tab does, as before.
