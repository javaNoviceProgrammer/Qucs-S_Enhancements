# The PPTX viewer check of 22 October

The reviewer's check of d37ee17 and 519f625 (`2026-10-22-claude-pptx-viewer-check.md`) found six issues.
It used a python-pptx deck of seven slides and PowerPoint's own PDF of it as the reference. All six are
taken up below. Two of them did not reproduce here and are explained.

This round worked from copies of the reviewer's deck and its references (`ref/s-*.png`, `ref.pdf`).

| Feedback | Now |
|---|---|
| 1. Chart value axes have too many ticks and overlapping labels: 0 to 4 every 0.5 where PowerPoint has 0 to 6 every 2; the scatter's axis every 0.1 where PowerPoint has 0.5 | An axis's step is chosen as PowerPoint's own is. It is 1, 2 or 5 times a power of ten: the smallest that keeps to as many steps as the axis's length gives labels room for. Each step is 1.2 lines of the label font asked for (Calibri's, whatever stands in), and there are five at most. There is 5 % of room above the highest value, and the ends are whole steps. The review's deck now draws exactly PowerPoint's axes: columns and lines 0 to 6 by 2, the scatter 0 to 1 by 0.5 up and 0 to 1.5 by 0.5 across, no label over another. The chart's text sizes also follow PowerPoint where the chart says nothing (title 1.2 times its text, not 1.4; axis labels as large as its text, not 0.9), and its margins no longer grow with its text (some 5 points). |
| 2. The cloud preset drawn as an ellipse, and nothing says so | The cloud is drawn as `presetShapeDefinitions.xml` defines it: eleven arcs, and the eleven arcs inside it stroked alone. Its filled area is within 0.992 (overlap over union) of PowerPoint's in the reference; an ellipse is 0.888. Its text goes in the cloud's own text box, as PowerPoint places "Cloud" (x = 505 in both). The cloud callout is drawn too: the cloud and three bubbles out to its point. Drawing it found that every DrawingML arc was drawn wrong where its ellipse is not a circle: `arcTo`'s angles are true angles on the ellipse, and they were taken as its parameter's. The cloud's arcs close to 0.3 % read the first way, 5 % off the second. A custom geometry's arcs now use the same reading, and its paths marked `fill="none"` are stroked, not filled. |
| 3. The zoom not kept when the file is reloaded (275 % came back as "fit width") | Not reproduced here, by any reload: the tab's own file watcher, `show_pptx reload`, or the Claude panel's reload of a file its conversation wrote (`reloadChangedFiles`). It was tried offscreen and on the display, with the file written in place and written anew. Every reload kept the zoom. "Fit width" is what a new view starts with, so I could not find the path the review took. What was found and fixed: a reload ran the find bar's last search again (R1 below). The view is now kept explicitly. A presentation read anew starts at the first slide, as wide as the view; a reload saves the view (its fit or zoom, the slide, how far down it, how far across) and puts it back. A test checks the zoom and the region shown across both reloads. |
| 4. The theme font falls back to Helvetica without saying so; it looks bolder and wider, and lines can wrap otherwise | A font that is not installed is drawn by a metric-compatible one where there is one (Carlito for Calibri, Caladea for Cambria). Otherwise it is drawn by the installed font most like it, measured: PT Sans for Calibri, Aptos, Candara and Corbel; Charter for Cambria and Constantia; Palatino for Book Antiqua; Gill Sans for Gill Sans MT; and so on. That font is stretched so that it is as wide as the font asked for, in each style, and its lines are as far apart. Lines then break where PowerPoint breaks them. The table covers 25 Office fonts. 23 were measured on the fonts themselves, width and line height; Calibri Light and Aptos Display take Calibri's and Aptos's. Only the numbers are kept. The notes pane and `read_pptx` say it: "the font Calibri, not installed: drawn as PT Sans, as wide (lines break as in PowerPoint)". A font whose widths are not known is said with "(lines may break elsewhere)". On the review's deck the mixed line ("Bold red normal italic green") is now about 3 % short of PowerPoint's width; it was 7 % short. |
| 5. Shapes have no theme effects, such as the outer shadow of the default shape style | Outer shadows are drawn: a shape's own (`a:effectLst`), its placeholders', else the theme's effect style that its `effectRef` names. Each has its colour and alpha, blur, distance and direction, size and alignment, and turns with the shape or not. An empty effect list takes the shadow away. The shadow is the shape's silhouette (fill, line or picture), blurred at the device's resolution. Shadows under text, glow, reflections, soft edges and 3-D effects are not drawn. |
| 6. During a show the tab's "slide shown" moved (4 to 2); after it, it followed the show's slide | Not reproduced here, offscreen or on the display: the tab stayed at 4. The review offered two expectations, and the tab now meets the second: while a show plays, it shows the show's slide (each step of the show moves it), and it stays at the last one when the show ends. A resize of the window now keeps the view's place in the slide. Before, a fit put the slide's top at the view's top after every resize; at a fixed zoom the view did not move either way. |

## Found while testing

- **R1.** A reload ran the find bar's last search again and moved to its first hit, even with the bar
  closed (its text is kept). Found when the test of issue 3 came back on slide 3, the slide of the
  earlier search "measured loss", not slide 2. A reload now counts the bar's hits again, if the bar is
  shown, without moving the view.
- **R2.** The cloud callout's bubbles overlapped as their formulas place them, and the fill made holes
  where they met. They are now joined into the outline.
- **R3.** Qt applies a font's stretch squared on macOS (90 gives 0.81 as wide), but only to a font it
  stretches itself. A font with narrower widths of its own (the system font) takes the stretch once. The
  stretch is therefore measured on the stand-in itself. Measuring it on the default font first left PT
  Sans 5 % too narrow.

## Not done

- **Office's own fonts.** The reviewer suggested using them where Office is installed. They are inside
  PowerPoint's bundle, licensed for use while Office runs, not for other programs, so they are not
  loaded. Calibri's widths and line heights (a measurement) are all that is taken from them.
- **Other theme effects.** Shadows under text, inner shadows, glow, reflections, soft edges and 3-D
  bevels are not drawn and not named. Outer shadows are by far the most common, being the default
  shape style's.
- **The legend at the bottom.** In PowerPoint's own rendering of the column chart, the legend overlaps
  the category labels. Here it is drawn under them. The axis is the same either way.

## Tests

`test_pptx_doc` gains four functions:
- `chartAxesStepped`: the review's charts again, titled, with 18-point text and its frames. The column
  chart's tallest bar reaches two thirds of the way to its top gridline (6), with gridlines at 2, 4 and
  6. The scatter has three or four gridlines across and two or three up.
- `cloudsAndTextBoxes`:
  - the cloud's notch inside its ellipse is white, and its bump outside it is filled;
  - the callout's bubble is at its point;
  - text starts in the ellipse's and the cloud's own boxes;
  - nothing says the cloud is drawn otherwise.
- `shadowsDrawn`:
  - the theme's shadow under a shape, soft past its edge;
  - none where the shape's effect list is empty;
  - a shape's own blue shadow to its right, under the shape.
- `fontsStandIn`:
  - Calibri's width in regular and italic (42.83 and 42.66 ems at 20 points), within 3 %, on any
    platform (Calibri, Carlito or a stand-in);
  - its line pitch, 1.221 ems;
  - the stand-in said, and a font of unknown widths said so.

`chartAxesStepped` also checks the title's capitals at 1.2 times the top label's digit, a ratio that
holds in any font. `cloudsAndTextBoxes` also draws a custom geometry: a whole circle from one `arcTo`, a
full turn, and a square path that is not filled, which is stroked with its corners left white.

Existing tests changed:
- `claudeDrivesTheTab`:
  - the tab follows the show;
  - a reload keeps a region's zoom, the slide, and the region in sight.
- `opensInATab`:
  - the file watcher's reload keeps a zoom of 250 %, and a resize keeps the slide;
  - at fit width, the middle of a slide stays in sight across a resize.

The suites that use the code touched pass:
- `test_pptx_doc` (23, the two on request skipped; under AddressSanitizer and UndefinedBehaviorSanitizer
  too, with no report);
- `test_qucs_control` (209), `test_mcp_server` (18), `test_claude_code` (68, 1 skipped).

There were 32 breaks, each its own edit, built and run, then put back. 31 were caught. Five of those
were caught only once the tests were sharper:
- the title's size: the ratio check;
- an arc's sweep kept to its sign: the full circle;
- the resize: the place in the slide;
- the theme's effect styles: my edit first did not build;
- a path not filled being filled: added with the custom geometry.

One is not observable by these tests: labels spaced by the stand-in's line height rather than the font
asked for. On the review's charts the two differ by 6 %, and no step count changes. A test placed on
such a boundary would depend on each platform's stand-in.

The decks on this machine were read and drawn again: 270 decks, 4587 slides (the others are cloud
placeholders). In Release a slide draws in 2.3 ms on average (2.2 before shadows) and a deck reads in
57 ms. Under AddressSanitizer and UndefinedBehaviorSanitizer there was no report. The fonts stood in
most often:
- Calibri, drawn as PT Sans (2,755 slides);
- Cambria, as Charter (844);
- Cambria Math, as STIX Two Math (618);
- the corporate fonts of the work decks, Nokia Pure and Myriad Pro, as Helvetica Neue, said with
  "(lines may break elsewhere)".
