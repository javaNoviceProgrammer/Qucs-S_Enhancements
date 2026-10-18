# Bug hunt: the day's zooms and viewers

*17 October 2026, Qucs-S 27.0.1, `755bb00` (the Release build and the ASan/UBSan build of it). 17:41 to 18:41, the findings written up as they came. All fixed, the notes too, in the commit that adds this report: docs/feature_gaps/2026-10-17-claude-hunt-fixes.md (with E1, an SVG naming macOS's system font, found while checking the fixes).*

This hunt covers what changed on 17 October:
- **The PDF viewer's smooth zoom (`7fbafb1`):** the scaled tiles while zooming, the animated steps, the pinch's anchor.
- **The data display's zoom (`755bb00`):** square caps on thin graph lines and in a zoom's draft, thinning on the way out, the lines out of sight passed over.
- **The Viewers tab and the CPU default (`8d85227`).**
- **The layout viewer's incremental layer buffers on the GPU (`8d85227`).**
- **The day's other changes, more briefly:** the sparse LU's re-analysis (`8d85227`), the abrupt heterojunctions (`d524e83`), the PDF text selection (`2522d21`), the File Browser's tabs (`f48688d`), and the tools through two fuzzers.

It ran in the foreground on macOS arm64 (Retina display), each run with its own HOME, settings, cache and trash in a scratch folder. Methods:

1. **Reading the new code** for edge cases: anchors, timers, cache keys, devices.
2. **Probes**, throwaway test functions in the three GUI test files, each behind an environment variable. They are copied beside this report (`*_probes.cpp`) and the test files were restored. They ran on the display and offscreen; the odd-PDF probe on the ASan build.
3. **Fuzzers on the ASan build:**
   - `scripts/ci/fuzz-sch.py` over two data-display seeds (a copy of the user's 10-well page, and `sweeps.sch` with Rect and Polar diagrams, dashed, auto-coloured and 4-wide graphs), and over all 252 examples in render mode;
   - the 16 October hunt's `fuzz_qfem.py` (80 multiphysics mutants, two seeds) and `fuzz_tools.py` (1,200 tool calls).
4. **Through the tools** (small MCP clients beside this report):
   - `console`'s reads and interrupts (`console_probe.py`);
   - the pn diode with two silicon nodes, plain, abrupt and graded (`hetero_probe.py`);
   - closing a model mid-solve (`close_probe.py`).
5. **An export:** the sweep page rendered to SVG from the command line, its strokes counted.

## Summary

| | Finding | Since |
|---|---|---|
| **F1** | A layout over the GPU's budget (2 million shapes), the GPU chosen: every frame flattens the cell again before the CPU draws it | `8d85227` |
| **A1** | A PDF zoom with the pointer off the page (the grey margin beside or between pages) is anchored on the current page's top-left corner, not the point under the pointer | before `7fbafb1` (zoomBy); the smooth zoom inherits it |
| **A2** | Vector exports (SVG, PDF, EPS) draw graph lines up to 3 wide with square caps | `755bb00` |
| B1 | The layout canvas keeps the CPU's vertex arrays of every layer added incrementally (a full build frees them) | `8d85227` |
| C1 | The `console` tool's `interrupt` with nothing running waits its whole `wait` (10 s, up to 600), then says the run "goes on" and to interrupt it | before |
| C2 | Selecting text on a page of many separate text runs costs time in proportion to them at every mouse move: 65 ms a step on 12,000 | `2522d21` |
| N1 | (latent) The layout canvas knows its layout by its address: a hidden tab reloaded twice could keep stale buffers | `80aeefd` |
| N2 | (minor) A PDF reloaded during a step's frames keeps the zoom shown mid-step, not the step's end | `7fbafb1` |
| N3 | (minor, by design) Vector exports thin a graph's points to a pixel at 96 dpi: detail lost when the export is zoomed | before |
| N4 | (minor, performance) Eigen's LU analyses each new pattern afresh, where Accelerate's keeps their union | `8d85227` |

No crash, hang or sanitizer report in any probe or fuzzer. F1 is a regression of this session's own change (`8d85227`), found by reading it again and confirmed by a probe.

## F1. A layout over the GPU's budget is flattened again at every frame

**Seen** (`huntOverBudget`, the display, the GPU chosen, the budget made 5 shapes with `QUCS_LAYOUT_GPU_BUDGET`): the sample layout panned 10 times gave 17 frames and 17 builds. Each frame made the canvas again, and then the CPU drew it (11 renders).

With the real budget, 2,000,000 shapes, a chip larger than that is flattened and tessellated up to 2 million shapes at every paint before the canvas gives up and the CPU draws. A whole rebuild took 394 ms for 8,000 curves; on such a chip it is seconds a frame, for every pan, zoom or layer toggle, as long as the GPU is chosen.

**Why.** `LayoutGl::sync` (`8d85227`) returns early, nothing changed, only inside `if (same && layout && !a_flat.tooLarge)`. A canvas over its budget skips that test and makes everything again. The code before it returned whenever the layout, cell, depth, texts and layers shown were the same, too large or not.

**Proposed fix.** `if (same && a_flat.tooLarge) return;` before the incremental path, with the layers shown in the key as before. A layer hidden could bring the cell under the budget, so try again when the layers shown change.

## A1. A PDF zoom off the page jumps to the page's corner

**Seen.** At Fit Page the page (499 by 706) sits in a view 872 wide, grey on both sides (`huntZoomInTheMargin`). A Ctrl+wheel notch was sent once with the pointer in the grey left of the page, and once on the page's left edge, both half way down (y 361):

| Pointer | Page after the notch |
|---|---|
| In the margin, x 93 | (137, 16), 598 by 846 |
| On the page, x 196 | (137, −62), 598 by 846 |

On the page, the point under the pointer stays: 361 + (8 − 361) × 1.197 = −61.6. In the margin, the zoom is placed so that the current page's top-left corner is at the pointer. Its scroll goes negative and is clamped, so the page's top stays at the margin, 78 pixels from where the point under the pointer would be.

In a long document scrolled to the middle of a page, at a zoom where the page is narrower than the view, the same notch puts that page's top at the pointer: a jump of up to a page. The same happens for a pinch, a trackpad's Ctrl-scroll and every step's anchor.

**Why.** `PageView::takeAnchor` (and `zoomBy` before it, whose code this is): `pageAt(anchor)` is −1 off a page, and the fallback is `a_currentPage` with the point (0, 0).

**Proposed fix.** Anchor off a page on the content itself: (scroll + anchor) / pixels per point, a point of the whole document's layout; or the nearest page's point clamped, kept outside its edge.

## A2. Vector exports draw thin graph lines with square caps

**Seen.** `qucs-s -p -i sweeps.sch -o sweeps.svg` gives 20 strokes with `stroke-linecap="square"`: the graphs 1, 2 and 3 wide, the auto-coloured curves each its own. Round caps remain only on the 4-wide graph and on what is not a graph. Before `755bb00` every graph line was round-capped.

A curve's joints show square corners and its ends square ends once the SVG or PDF is zoomed in a viewer or printed large. That is where a vector export is meant to stay sharp.

**Why.** `Graph::drawLines` squares solid lines 3 "device pixels" wide or less. The device pixels are the pen's width × the painter's scale × the device's pixel ratio. The SVG, PDF and EPS exports paint at scale 1 through a relay device of ratio 1, so every graph up to 3 wide is "3 pixels or less". The rule assumes a raster, where a pixel is the smallest thing shown.

**Proposed fix.** Apply the width rule only on raster devices: the canvas widget, `QImage`, `QPixmap`. A printer, the relay device and every vector device keep round caps; the draft is the canvas's already.

## B1. Incrementally added layers keep their CPU arrays

`LayoutGl::sync`'s full build uploads each layer's fills, outlines and dots and then frees the CPU's copies ("the GPU has them"). The incremental path, a layer shown for the first time, moves the new layer's arrays into `a_flat.layers[i]` and uploads them, but never frees them. Each such layer is held twice, on the CPU and the GPU, until the next full build: the cell, depth or texts changing, or a reload. On a large chip a layer's arrays are tens of MB (8 floats a triangle).

## C1. `console`'s interrupt with nothing running waits it out

**Seen** (`console_probe.py`, the Release build, through `qucs-s --mcp-server`):

| Call | Time | Answer |
|---|---|---|
| `{"kind": "python"}` | 0.0 s | `"running": false` |
| `{"kind": "python", "interrupt": true, "wait": 8}` | 8.1 s | `"finished": false`, "Still running after 8 s: it goes on in the console. 'interrupt': true stops it (Ctrl-C) ..." |
| The same for the terminal | 8.1 s | the same |

With the default `wait` that is 10 s, and up to 600 s with a larger one. The tool fuzzer's two console "hangs" were this, `wait` 570 and 99999, against its 90 s per call. Nothing was running, so the answer's note is wrong. It asks Claude to interrupt again, which waits again.

**Why.** `QucsControl::console` sends the interrupt and then waits for the prompt to come back in output printed after it. A console with nothing running prints nothing, and a program not started prints nothing either, so no prompt comes and the wait runs out.

**Proposed fix.** `interrupt` when `running()` is false: answer at once (nothing to stop, the last lines). After the Ctrl-C, take "not running" as settled too.

## C2. Selecting on a page of many text runs: each step in proportion to them

**Seen** (`scatter_pdf.py` writes the pages: single characters at random places, 4 to 8 points tall, each its own text object; `huntScatteredText`, the Release build offscreen). 40 selections between random points, then 40 only 5 points long:

| Page | Random points (median, worst) | 5 points long (median, worst) |
|---|---|---|
| 3,000 characters | 16.0, 32.4 ms | 15.8, 22.6 ms |
| 3,000, each with a space | 20.5, 44.6 ms | 20.2, 36.2 ms |
| 12,000 characters | 64.5, 109.2 ms | 64.5, 91.6 ms |

A short selection costs what a long one does, so the time is the search, not pdfium building the selection. A drag selects at every mouse move: on such a page (a drawing's labels, a map, a scanned page's OCR layer) it moves at 10 to 15 frames a second.

Before `2522d21` the first selection on a page took seconds (every character's box), so this is far better, but still in proportion to the page.

**Why.** For a point off the text, `PageView::charAt` looks through the page's runs (`pageText`, kept once a page) for the nearest one, its line first, at each call. The hit test from the page's first character to the point is pdfium's, over a range that grows with the character's place in the page.

**Proposed fix.** A grid of the runs' boxes per page, built with them, so the nearest runs are found in the cells about the point. Keep the last answer for a drag whose point stays in the same run.

## Minor notes

**N1. The layout canvas's key is an address.** `LayoutGl::sync` treats the layout as the same when `layout == a_madeOf`, a raw pointer, and `LayoutView::setLayout` does not reset it. If a tab is reloaded twice while hidden (nothing painted between), the last read could take the freed address of the one last drawn. Its cell, depth and texts are then compared as the same, and its stale buffers are drawn for the new file.

Not reproduced. `huntReloadWhileHidden` tried 6 times, rebuilding each time. The reloads alternate between two blocks of the reader's thread, and the one drawn is the newer of the two. The old key had the same pointer. A generation counter bumped by `setLayout` would close it.

**N2. A PDF reloaded mid-step.** `PdfDoc::reload` reads `a_view->zoom()`, the zoom shown, and restores it. During a step's 150 ms that is between the start and the end, so a file rewritten then (a watcher's reload) lands at an intermediate zoom. `huntReloadMidStep`: a step from 1 to 2, reloaded 40 ms in, stays at 1.339. `targetZoom()` would restore the step's end.

**N3. Vector exports thin at 96 dpi.** A graph's points are thinned to a pixel apart at the painter's scale. A vector export paints at scale 1 and keeps one point per pixel at 96 dpi: a dense curve exported to SVG loses its detail when zoomed. This is as before; it is noted beside A2, whose fix may want the same distinction.

**N4. Eigen's LU and a pattern that comes and goes.** Since `8d85227`, Eigen's backend (Linux, Windows, macOS before 26, and small models everywhere) analyses again whenever a matrix's pattern differs from the last one analysed. That is right where it once overflowed. Accelerate's backend instead keeps the union of the patterns seen and factorizes within it. Its comment counts an avalanche model's Newton at 2,264 pattern changes in 8,500 factorizations, each analysis some eight factorizations' time. On Eigen's backend each of those changes is now a fresh analysis. By reading, not timed; the union approach (explicit zeros for the entries absent) would serve Eigen too.

## Checked and found right

- **PDF, odd page boxes** (`oddpdf.py`, `huntOddPages`, ASan build): ten one-page PDFs whose MediaBox was zero, negative, flipped, 1e9, 1e30, nan, words, 0.01 points and 14,400 by 3. pdfium gives the impossible ones Letter (612 by 792). Each was fitted to width and page, zoomed smoothly and at once to 16 and to 0.1, its text selected and searched: no sanitizer report.
- **PDF, a pinch out over 80 pages** (`huntPinchOutOverManyPages`, display):
  - from Fit Width to 0.14: 50 frames, worst 4.4 ms, no tile rendered;
  - from 16, the only tiles at 16, out to 0.1: 40 frames, worst 3.9 ms (the coarse tile grids walked at up to 160 times are cheap).
- **The data display's culling against the uncut scene** (`huntCulledDiagrams`): a Rect and a Polar diagram, the graphs plain and selected (wider), zooms 3 and 7 at six centres each, offscreen and on the display. The canvas was compared with the scene held under View > Zoom In's rectangle, which is drawn on an image and not culled: no ink missing.

  One display view differed by 66 levels at one pixel of a dashed graph, the same to the eye. That is the held image's ratio-2 resampling at a dash's end; the dashed branch is not culled.
- **The layout canvas's incremental origin:** a layer flattened alone and the whole cell share the cell's bounds centre as their origin, so added layers are not offset.
- **The layout canvas, a hidden tab reloaded twice** (6 tries): rebuilt each time.
- **The Viewers tab's Apply:** only a changed box is written, and the open viewers are told through the notifier (what `theViewersSettings` checks).
- **The PDF's smooth zoom with the GPU chosen** (`huntGpuZoom`, display): a pinch's frame from scaled tiles, white between the glyphs as the zoom's own (0.9615 against 0.9671): no halo through OpenGL's clip regions.
- **A heterojunction of no step** (`hetero_probe.py`, Release, the pn diode at 0.5 and 0.7 V). Five variants: one silicon node; two nodes of the same library silicon (pside, nside), plain, abrupt and graded; and one node graded. All gave the same anode current: 17.54 nA and 28.12 µA.
- **The sparse LU's pattern check** reads the matrix by Eigen's iterator, so an uncompressed matrix's pattern is compared right too.
- **Printing** uses `QPrinter::HighResolution`: the painter's scale is large, so printed graphs keep round caps (A2 is the SVG, PDF and EPS exports').
- **The layout's CPU renderer** leaves out hidden layers' texts, as the GPU's now does.
- **The File Browser's "already in a tab"** compares canonical paths case-insensitively on macOS and Windows, where realpath keeps the case it is given (checked).
- **Closing a model mid-solve:** the multiphysics fuzzer's closes took 11.8 and 13.6 s on the ASan build after `fem_solve`'s own time limit (`mos_capacitor` and `induction_heating`, damaged). On the Release build the same files closed in 0.08 and 0.02 s with a solve still running, the originals in 0.16 and 0.02 s: the sanitizer's slowness between cancellation checks, not a wait.
- **F1's pattern elsewhere:** the multiphysics Graphics view's GPU canvas has no budget and no fallback to the CPU (the only budget there is the mesher's, 4 million vertices), so it never rebuilds at every frame that way. A layout canvas with no layout loaded yet rebuilds nothing (no flatten).
- **A1's pattern elsewhere:** the image viewer's zoom keeps the point under its anchor in the image's own coordinates (`shownAt`, `keepAt`), off the image too: no page fallback there. A1 is the PDF view's alone.
- **A2 covers EPS too:** `graphicsexport.cpp`'s EPS export paints at scale 1 through the same relay device as SVG and PDF.
- **`wait_for`'s edges** (the Release build, an MCP client): with nothing open, a file that does not exist, no simulation and no dialog, each wait ended at its 2 s with `happened: false` and the note to wait on. A timeout of 0 waits 1 s; an unknown event and a path not open (`../../etc/passwd`) are refused at once. The fuzzer's two `wait_for` "hangs" were its 90 s against waits asked of 450 and 100 s.
- **Fuzzing, ASan/UBSan:**
  - `fuzz-sch.py` over the two data-display seeds, netlisted and rendered, datasets 9 times in 10: 60 mutants (seed 1017, 120 runs), 120 (seed 1019, 240 runs) and 150 (seed 1020, 300 runs), no failure;
  - over the 252 examples, 100 mutants rendered: no failure;
  - `fuzz_qfem.py`, seeds 1017 and 1018, 40 mutants each (opened, built, solved, described, evaluated, closed): no finding;
  - `fuzz_tools.py`, seed 1017, 1,200 calls. Four "hangs":
    - two are `wait_for`, asked for 450 and 100 s against the fuzzer's 90 s per call: the fuzzer's own limit;
    - two are `console` interrupts, which is C1.

## Reproducing

- **A1:** paste `huntZoomInTheMargin` from `test_pdf_viewer_probes.cpp` into `test_pdf_viewer.cpp` and run `HUNT_PROBE=1 test_pdf_viewer aPdfOpensInATab huntZoomInTheMargin` on the display (offscreen the same).
- **A2:** `qucs-s -p -i sweeps.sch -o sweeps.svg`. `sweeps.sch` is beside this report; `python3 sweeps_dat.py sweeps.dat` writes its dataset next to it. Then `grep -o 'stroke-linecap="[a-z]*"' sweeps.svg | sort | uniq -c`.
- **F1:** `huntOverBudget` from `test_layout_doc_probes.cpp`, `HUNT_PROBE=1`, on the display.
- **B1:** by reading `LayoutGl::sync`'s two paths.
- **C1:** `python3 console_probe.py <qucs-s> <scratch>` (an MCP client; the scratch folder holds the server's HOME, settings, cache, trash and workspace).
- **C2:** `python3 scatter_pdf.py <folder>`, then `HUNT_SCATTER=<folder> test_pdf_viewer huntScatteredText`.
- **The rest:**
  - `HUNT_PROBE=1` for `huntReloadWhileHidden` (test_layout_doc, display, the GPU chosen inside) and `huntPinchOutOverManyPages`;
  - `HUNT_ODD=<the folder oddpdf.py writes>` for `huntOddPages`;
  - `HUNT_SEED=<sweeps.sch beside its dataset>` for `huntCulledDiagrams`;
  - the fuzzers as above, with seed 1017.
