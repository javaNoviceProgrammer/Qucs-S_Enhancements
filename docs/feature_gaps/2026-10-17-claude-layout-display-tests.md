# Three layout tests that failed on the display

Three tests of test_layout_doc failed on the Mac's display (Cocoa, Retina, dark mode) and passed offscreen, as ctest runs them. They failed the same way at 2522d21, before this round (flagged in the update check of 48d5fa8):

- `theCanvasIsLightOrDarkWhateverTheTheme`: "Compared values are not the same", #ff171717 against #ff171717;
- `theLayersStandOutFromTheCanvas`: a layer swatch's corner #ffffffff, not the layer's #ff3474;
- `aGridIsBehindTheLayout`: the grid's dots read 255 and 255, none drawn.

## Why, and now

**The canvas's colour: the test.** The view's corner and the palette's Base were both plain RGB, both #171717, and still unequal as QColors. QColor's == compares its 16-bit components: a picture's pixel has 8 bits (#17 is 0x1717), and the platform's dark Base has finer ones. The canvas is drawn in 8 bits, so the test now compares the 8-bit values (`rgba()`). Offscreen Base is white, exact either way.

**The swatch's frame: the program.** On a Retina screen the swatch is a 32 by 24 pixmap at a ratio of 2. It was stroked, a rectangle through the pixels' centres. At a ratio of 2 its frame was the two outer device pixels all round, except its left corners' outer device pixels, which stayed the background:

    row  0  .FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
    row  1  FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
    row 22  FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
    row 23  .FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF

The test read the corner, white. Now the frame is filled: the swatch in the frame's colour, then the inside in the canvas's and the layer's fill (`LayoutDoc::swatchPixmap`). At a ratio of 1 the same pixels as before; at any ratio the outermost device pixels all the frame's, corners too. At 1.5 the frame is 2 device pixels left and above and 1 right and below, as Qt rounds the inside's edges.

**The grid's picture: the test.** `LayoutView::picture()` is the view's grab, at the screen's ratio: 1324 by 1240 for a view of 662 by 620. The test read it at the view's pixels, so it looked where no dot was. It now reads the device pixel at a view pixel's centre, at the picture's ratio. The grid's dots and lines are drawn through pixels' centres, (x + 0.5) · ratio.

The crosses' arms are read a pixel about, as the dots already were. At a ratio of 2, Qt's aliased lines fall a device pixel left of a whole x coordinate (a vertical arm at 703.0 in column 702) but not above a whole y. A single pixel can't be exact at both ratios. A weak arm is still told from a strong one: it reaches 2 pixels and not 3, a strong one 3 at least.

**Not changed:** `picture()` stays at the screen's ratio. It is Copy Picture's image too, sharp on a Retina screen.

## Tests

- **`aSwatchIsWholeAtAnyRatio` (new):** the swatch at ratios 1, 1.5 and 2, offscreen as on the display. Its outermost device pixels are the frame all round, at least as deep as the ratio's whole part. Inside it is the canvas. Filled solid, the fill is inside and the frame is whole at its corners. It needs no Retina screen: CI runs it.
- **`theCanvasIsLightOrDarkWhateverTheTheme`:** Base compared in 8 bits.
- **`aGridIsBehindTheLayout`:** its pixels read at the picture's ratio: the dots, the lines, the crosses, the scale bar, a filled shape.

## Breaks

Four changes broken on purpose, each run on the display and offscreen: eight runs, all caught.

- **The swatch stroked again:** the new test fails at a ratio of 2 (0,0 and 0,23 white), on both; on the display `theLayersStandOutFromTheCanvas` fails too.
- **The application's canvas its AlternateBase:** the canvas test.
- **The weak dots as strong:** the grid test (106 and 106).
- **The weak crosses' arms as long as the strong:** the grid test.

## Runs

- **test_layout_doc on the display:** 29 of 29 (25 of 28 before). Offscreen: 27, its 2 GPU tests skipped (no OpenGL there).
- **The Release suite (offscreen):** 127 of 127.
- **Under the sanitizers,** on the display and offscreen: test_layout_doc whole, no report.
- **The lints pass.**
