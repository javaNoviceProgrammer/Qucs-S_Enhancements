# A data display of many curves zooms without choking

The user (2026-10-17): *"the dpl panels choke on plots that have many curves when zooming in and zooming out"*

## Reproduced

On a copy of their 10-well page (mqw_10well/mqw10_pg1.dpl), on the Mac's display (Retina). It has three diagrams:
- three graphs of wavefunctions, each 20 states of 2,000 points, auto colours, 1 wide;
- band edges, 3 wide.

Zoomed in 10 steps of 1.1 and out again:

| | A step's paint |
|---|---|
| Below 0.5 | 20 ms |
| From 0.54 up | 67-211 ms (median 118) |
| Held at 1.33 | 145 ms |

That is 5 to 8 frames a second: a wheel's or a pinch's events came faster than they were drawn.

## Why

Sampled: 3,415 of the 3,618 samples in the canvas's elements were in Qt's generic line stroker, under `Graph::drawLines`.

**Round caps.** A graph is a line for each pair of points, each with round caps. Qt's fast path draws lines a device pixel wide or less. Wider, it outlines every round cap as Bézier curves and then fills the outline. A 1-wide graph is wider than that from a zoom of 0.5 on a Retina screen. Held at 1.33:
- 145 ms with round caps;
- 23 ms with square caps;
- 12.7 ms with thin cosmetic lines.

**Thinning only on the way in.** A graph's points are thinned to a pixel apart for the scale they are drawn at. They were thinned again only when the scale grew past 1.5 times. On the way out from 2.2 to 0.6, each paint drew 3.7 times the lines it needed: 211 ms a step.

**Lines out of sight.** Lines off the canvas were outlined too.

## Now

- **Square caps where they look the same.** A solid line 3 device pixels wide or less gets square caps. There a square end differs from a round one by under half a pixel. Compared with round caps on the user's page at zooms of 1.2 and 1.45, 0.7 % of the inked pixels differ by more than 24 levels (at most 68). The difference is in the antialiasing at the joints between lines; magnified 8 times, the curves are the same.
- **A draft while zooming.** While a wheel or a pinch zooms, the canvas is drawn in draft: every solid line gets square caps, at its own width and colour. 120 ms after the last step, it is drawn in full with round caps (`Schematic::zooming()`, `qucs_s::lod::Draft`).
- **Thinned again on the way out** too, below the scale divided by 1.5.
- **Lines out of sight passed over** on the canvas: any line whose box misses the canvas and the painter's clip, a pen's width about. Prints and exports draw every line.
- **Dashed graphs unchanged.** They keep round caps: square ones would lengthen each dash.

The user's page after, drawn in full (no draft), is about 6 times quicker:

| | Before | After |
|---|---|---|
| A zoom step (median) | 118 ms | 19.7 ms |
| A zoom step (worst) | 211 ms | 31.5 ms |
| Held at 1.33 | 145 ms | 23.4 ms |

The test's page (20 curves of 2,000 points, offscreen): a full paint 7.3 ms, a pinch's draft frame 4.4.

## Tests

**test_canvas_drawing `aPlotOfManyCurvesZoomsSmoothly` (new):** a page of 20 wavefunctions of 2,000 points (1 wide, auto colours) and a band edge (3 wide).
- **Thinning:** the lines at 0.5 are the same before and after a zoom to 3.
- **Caps:** at 2 device pixels the 1-wide graph has no round caps; at 6 the 3-wide graph's are all round.
- **A pinch:** zooming, each step in draft, no round cap, every line drawn. 120 ms after, round again.
- **A trackpad's Ctrl-swipe** zooms the same way.
- **Culling:** zoomed to 4, the lines in sight alone (under half). A step of the band edge sits 1-5 pixels past the right edge, inside its half-width of 6. The canvas matches the scene held under View > Zoom In's rectangle, which is drawn on an image with no line passed over.

## Breaks

Ten changes broken on purpose (one of them also on the display), all caught:
- thin lines round-capped;
- no draft while zooming;
- a pinch not zooming;
- a wheel not zooming;
- never settled;
- not thinned on the way out;
- lines out of sight drawn;
- no margin for the pen, offscreen and on the display;
- the sight too narrow.

## Runs

- **test_canvas_drawing:** 31 of 31, offscreen and on the display.
- **The Release suite (offscreen):** 127 of 127.
- **Under the sanitizers,** no report:
  - test_canvas_drawing, test_graph_style, test_graph_autocolor, test_diagram_legend, test_diagram_theme, test_mouse_wheel, test_stacked_diagram, test_graphics_export, test_view_all.
- **The lints pass.**
