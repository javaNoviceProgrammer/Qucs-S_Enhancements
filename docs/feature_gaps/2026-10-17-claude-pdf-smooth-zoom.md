# The PDF viewer's zoom made smooth

The user (2026-10-17): *"focus on making zoom in and zoom out very smooth in the pdf viewer."*

## Why it was not

Measured on the Liu 2013 paper (copied), on the Mac's display (Retina), in a 1400 by 1000 window.

The pages are drawn in tiles of 512 by 512 device pixels, cached by page, scale and place. A zoom changes the scale, so no tile is cached at the new one. Each event of a pinch or of a Ctrl-scroll zoomed at once and repainted. That paint rendered up to 60 ms of tiles at the new scale in pdfium (about 1.7 ms a tile of text) and filled the rest with the page's 240-pixel preview, blurred. The next event threw all of it away.

| | Paints (median, worst) | Tiles rendered |
|---|---|---|
| A pinch in, 60 events | 37, 49 ms | 1,281, all thrown away |
| A pinch out, 60 events | 25, 48 ms | 1,122 |
| A step (the toolbar's +) | one paint of 30 ms, a jump | 20 |

## Now

- **A zoom under way renders no tile.** That covers a pinch, a trackpad's scroll with Ctrl, and a step's frames. Each part of the page is drawn from one source, the nearest first:
  1. the page's tiles at the nearest other scale, scaled;
  2. where it has none, the next scale's;
  3. where neither has any, the preview.

  A tile is the page's ink alone, transparent between the glyphs. Laid over each other, the text showed twice and the preview showed as a grey halo round every glyph.
- **The tiles of the zoom reached come after it.** They are rendered 100 ms after its last event, or at once when the fingers lift (the gesture's end). A paint renders at most 30 ms of them (it was 60); the rest come in the next paint. Till then the scaled tiles stand in for them.
- **A step is animated.** The toolbar's + and −, View > Zoom In and Out, a mouse wheel's notch with Ctrl, and a zoom chosen or typed in the box all go there in frames over 150 ms:
  - eased out, and in proportion: each frame the same factor more;
  - the point under the pointer (or the middle) kept, placed from it at each frame, so no rounding gathers;
  - a step before the end goes on from its end: two clicks of + are 1.25 × 1.25.
- **The tools' zoom is at once.** show_pdf, a fit, and View 1:1 zoom immediately, and end any zoom under way.
- **A pinch keeps the point under the fingers.** It is taken once a gesture, and again when the fingers move or the view is scrolled meanwhile, and each event is placed from it. Zoomed an event at a time from the scroll, each scroll rounded to a pixel, it drifted 2 pixels in 20 events.

The same paper after:

| | Frames (median, worst) | Tiles rendered meanwhile |
|---|---|---|
| A pinch in, 60 events | 4.4, 5.6 ms | 0 |
| A pinch out, 60 events | 4.9, 7.0 ms | 0 |
| A step, in or out (10 frames) | 4.8-4.9, 5.4-6.1 ms | 0 |
| Four steps in, clicked 40 ms apart | 5.0, 6.5 ms | 0 |

The pages are sharp about 100 ms after the last event.

## Tests

- **test_pdf_viewer `zoomIsSmooth` (new).**
  - A pinch of 30 events on the page's first line, scrolled after 10:
    - no tile rendered; tiles at another scale drawn;
    - the point under the fingers kept within 0.75 pixel (0.37-0.51 here), the scroll not undone;
    - its frame as white between the glyphs as the zoom's own (the grey at most 1.8 times; 1.1-1.3 here, the halo 2.3).
  - The fingers lifted: settled at once, then every tile the zoom's own.
  - The View menu's step:
    - the target said at once and the zoom shown unchanged;
    - three or more frames, no tile rendered by any;
    - the middle kept.
  - Two steps go on from the first's end.
  - A wheel's notch is a step; a trackpad's scroll is at once.
  - setZoom ends a step under way, at once.
- **`itIsZoomed`:** View > Zoom In's target at once, its zoom when the frames end.
- **The `shown()` helper** waits for a zoom's end before it compares frames.

## Breaks

Eleven changes broken on purpose, all caught by `zoomIsSmooth`:
- tiles rendered while zooming;
- the preview in place of the scaled tiles;
- the preview under the tiles (the halo);
- the point taken again at each event;
- a scroll during a pinch undone;
- the View menu's step at once;
- a step not from the last one's end;
- the pinch's end not settled;
- an immediate zoom left zooming;
- a wheel's notch at once;
- a trackpad's scroll in frames.

## Runs

- **test_pdf_viewer:** offscreen 25 of 25 and its GPU test skipped; on the display 26 of 26.
- **The Release suite (offscreen):** 127 of 127.
- **Under the sanitizers:** test_pdf_viewer whole, no report.
- **The lints pass.**
