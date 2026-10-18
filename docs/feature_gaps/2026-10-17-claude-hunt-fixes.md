# The hunt of 17 October, fixed

The user (2026-10-17), after the hunt's report (docs/bug_hunts/2026-10-17-zoom-and-viewers.md): *"let's resolve all of them and rebuild the dmg"*. All six findings and the four notes are fixed. One more was found while checking the fixes (E1).

## F1. A layout over the GPU's budget made again at every frame

**Was.** `LayoutGl::sync` (`8d85227`) returned early, nothing having changed, only for a canvas within its budget. One past it, a cell of more than 2 million shapes that the CPU then draws, was flattened again up to the budget at every frame: 17 builds in 17 frames.

**Now.** Past the budget it is made again only when the layers shown change: a layer hidden may bring the cell under. With a budget of 5 shapes, five pans make nothing, and a layer hidden makes it once.

## A1. A PDF zoom off the page jumped to the page's corner

**Was.** With the pointer in the grey beside or between pages, a zoom put the page in sight's top-left corner under the pointer. At Fit Page, a notch beside the middle page left that page's top at 347 pixels; keeping the point under the pointer gives −352.

**Now.** Off every page, the zoom is anchored on the nearest page's point, beyond its edge (`PageView::anchorAt`). This covers pinches, wheel notches, steps and the toolbar's buttons. The same notch keeps the point within 2 pixels.

## A2 and N3. A graph in a vector export as exact as asked

**Was.** Since `755bb00`, graph lines 3 device pixels wide or less got square caps. An SVG, PDF or EPS export paints at scale 1 through a relay of ratio 1, so every graph up to 3 wide was squared there; the sweep page's SVG had 20 square-capped strokes. Since before, an export's points were also thinned to a pixel of its own scale, though it is zoomed or printed large after (N3).

**Now.** A new switch, `qucs_s::lod::VectorOutput`, is set by the SVG, PDF and EPS exports and by printing. While it is on, a graph's lines keep round caps, are thinned to a tenth of a pixel, and are not merged into vertical runs. The canvas and the PNG and JPEG exports (rasters) are as before.
- The sweep page's SVG: no graph stroke squared. The 7 square caps left are the frame, the grid and the axes, as they always were.
- The test's graph of 2,000 points, 240 pixels wide: all 1,999 lines kept, where thinning to a pixel kept 1,662.

## B1. Incrementally added layers keep their CPU arrays

**Was.** A layer shown for the first time was flattened alone and uploaded, and its vertex arrays stayed on the CPU too. A whole build frees them.

**Now.** They are freed after the upload (`LayoutGl::heldFloats()`: none left).

## C1. `console`'s interrupt with nothing running

**Was.** An interrupt to a console whose program is not running printed nothing. The call waited its whole `wait` (10 s, up to 600) for a prompt, then said the run "goes on" and to interrupt it again.

**Now.** It is answered at once: "Nothing runs in the terminal console: nothing to stop.", with `running: false` and the last lines. A program that ends after its Ctrl-C ends the wait too.

## C2. Selecting PDF text on a page of many runs

**Was.** Each of Qt's calls for a character or a selection reads the page's text again: 3 ms a call on 3,000 runs, 12 on 12,000, whatever the range. A drag step made three to five calls; off the text, a search along the nearest runs made up to 51.

**Now.** Calls are asked only where needed:
- **No call for a known character.** A point in a character met before (its box kept), or nearest a one-character run met before (labels on a drawing, a map: the run is the character), makes none.
- **No call when nothing changed.** A drag step over the same characters keeps its selection.
- **No hit test out of reach.** A point beyond pdfium's reach of every run (16 points, about twice its tolerance) is not tried; the nearest runs are searched instead.
- **A capped search.** The search along runs stops at six hit tests.

| | Before | Now |
|---|---|---|
| A drag of 150 steps a point apart, 3,000 labels | 881 ms | 343 ms (91 of Qt's calls; 101 steps under 1 ms) |
| The same, 12,000 labels | 3,244 ms | 2,536 ms |
| A press below a gap of a widely spaced line | 69 calls | 25 calls |

Each new character a drag reaches still costs a hit test, its box and the selection: three of Qt's calls. Qt's PDF module keeps no page text between calls, so on a page of 12,000 runs a drag over new characters stays at some 40 ms a character.

## N1. The layout canvas knew its layout by its address

**Was.** A layout read anew at the address of a freed one would have been taken for it, its buffers stale. This was latent and not reproduced.

**Now.** `LayoutView` counts the layouts set (`a_layoutGeneration`), and the canvas compares that count. The same layout set again is built again.

## N2. A PDF reloaded during a zoom step

**Was.** A reload during a step's frames kept the zoom shown mid-step: from 1 to 2, it stayed at 1.339.

**Now.** `PdfDoc::reload` first ends the step where it was going (`PageView::finishZoom`), with its place.

## N4. Eigen's LU and a pattern that comes and goes

**Was.** Since `8d85227`, Eigen's backend analysed again at each change of pattern: six analyses for the test's sequence of six.

**Now.** As Accelerate's backend does, it keeps the union of the patterns seen (P). A matrix within P is factorized as P, its other entries nought; one with an entry outside it makes the union and analyses that. The same sequence takes three analyses, on both backends. The solutions are checked against each matrix, to 1e-12.

## E1. An SVG export on a Mac's display named the system's font (found while checking A2)

`anSvgWithTextNamesAFontOthersHave` failed on the display, though not offscreen, where CI runs it: three groups named `.AppleSystemUIFont`, a font only macOS has. Text was already renamed to the reader's sans-serif. The painter's own font was not, nor the state's font relayed to it, and SVG groups name both. Now both are renamed: on the display the test passes, and the SVG names `sans-serif` alone.

## Tests

- **test_pdf_viewer:**
  - `aDragOverLabelsAsksLittle` (new, C2): 3,000 labels, the drag's calls under 150 and its selection the same as one asked afresh; then the widely spaced line, 26 calls at most;
  - `aZoomBesideAPageKeepsThePointer` (new, A1 and N2).
- **test_graphics_export:** `aGraphInAVectorExportIsExact` (new, A2 and N3).
- **test_layout_doc:**
  - `theGpuCanvasMakesWhatItMustOnly` (new, F1 and N1, on the display);
  - `aLayerShownOrHiddenAtOnce` (B1: no vertices left on the CPU).
- **test_qucs_control:** `consolesAreTypedIntoAskedEachTime` (C1: an interrupt with nothing running answered within 3 s).
- **test_fem_sparse:** `aPatternThatChanges` (N4: three analyses on both backends).

## Breaks

Twelve changes broken on purpose, all caught (the three layout canvas ones on the display, where its GPU tests run):
- C2:
  - one-letter runs not mapped (215 calls);
  - the search along runs uncapped (69 calls);
  - a step over the same characters asking again (212 calls);
- A1: off a page, the page in sight's corner again;
- N2: a reload mid-step not finished;
- A2: square caps in a vector export;
- N3: a vector export thinned to a pixel (1,662 lines);
- F1: past the budget, made again at every frame;
- B1: an added layer's arrays kept;
- N1: a layout known by its address;
- C1: an interrupt with nothing running waiting;
- N4: Eigen's LU analysed at each new pattern.

E1's two changes were each taken out in turn, checked on the display: either one gone, the test fails.

## Runs

- **The Release suite (offscreen):** 127 of 127.
- **On the display:** test_pdf_viewer 28 of 28, test_layout_doc 31 of 31 (its GPU tests), test_canvas_drawing 31 of 31, test_graphics_export 27 of 27.
- **Under the sanitizers (offscreen, Eigen's solvers),** no report:
  - test_pdf_viewer, test_layout_doc, test_canvas_drawing, test_graphics_export;
  - test_fem_sparse, test_fem_engine, test_fem_physics, test_fem_magnetics, test_fem_layout, test_fem_waves (35 of 35, 1,000 s), test_fem_semiconductor (37 of 37, 1,585 s: the Newtons whose patterns change, on the union now);
  - test_qucs_control's console functions.
- **The lints pass.**
