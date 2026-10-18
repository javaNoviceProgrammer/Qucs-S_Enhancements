# Bug hunt: the RHI viewers and the Retina display

*18 October 2026, Qucs-S 27.0.1, `287bf23` (the Release build and the ASan/UBSan build of it). 06:49 to 07:49, the findings written up as they came. Paused some five minutes at 07:30 for the user's report on `fan1998_results.dpl` and the DMG they asked for. All fixed, the notes too, in the commit that adds this report: docs/feature_gaps/2026-10-18-claude-rhi-hunt-fixes.md (with E1, the Graphics view's GPU buffers deleted during a frame - a crash on the display -, found while checking the fixes).*

This hunt covers what changed since the last one:
- **The GPU viewers on Qt's RHI (`d01d425`, `287bf23`):** the layered canvas (layout viewer, Graphics view), the GPU viewport (PDF viewer), the Markdown preview's tiles, the shaders, the fallbacks.
- **The data display's lines on a Retina screen (`2afb8bf`)** and the zoom around it, which the user is using.
- **More broadly:** the other viewers' zoom anchors, the GPU settings, the whole suite at a pixel ratio of 2, and two fuzzers.

It ran in the foreground on macOS arm64 (Retina display, Metal), each run with its own HOME, settings and cache in a scratch folder. Methods:

1. **Reading the new code** for edge cases: invalidation, device pixels, fallbacks, settings paths.
2. **Probes**, throwaway test functions in six GUI test files. They are kept beside this report as patches (`*_probes.patch`), and the test files were restored. They ran on the display (ratio 2), offscreen, at `QT_SCALE_FACTOR` 0.75, 1.25, 1.5, 2 and 3, and the stress probes on the ASan build.
3. **The suite at a ratio of 2:** every non-solver entry offscreen with `QT_SCALE_FACTOR=2`, and the drawing and viewer entries again on the ASan build.
4. **Fuzzers on the ASan build:**
   - `scripts/ci/fuzz-sch.py`, seeds 18, 19 and 20, 370 mutants netlisted, rendered or both;
   - the 16 October hunt's `fuzz_qfem.py`, seeds 1818 to 1820, 105 mutants.

## Summary

| | Finding | Since |
|---|---|---|
| **A1** | The layout viewer on the GPU (Metal) draws every vertical outline a device pixel right of the CPU's: OpenGL's half-pixel nudge kept | `d01d425` |
| **A2** | The schematic and data display's zoom moves the point under the pointer at each wheel notch or pinch step: 2 points on average, up to 6.5 (13 device pixels) at zooms 2 to 10 | before (whole model units) |
| **B1** | The layered canvas's cost grows with the window: full screen, a small layout pans in 13.2 ms on the GPU against 6.7 on the CPU; the Graphics view takes some 30 ms a frame whatever its mesh | `d01d425` |
| **B2** | (reading) A PDF or Markdown GPU viewport that cannot render stays blank: no fallback to the CPU, unlike the layered canvases | `d01d425` |
| **B3** | The PDF viewer on the GPU softens pages after the first at most zooms, a device pixel lower than the CPU draws them | `d01d425` |
| **B4** | `QUCS_GPU=0` is ignored by the Graphics view's own menu, Settings… > Viewers and Multiphysics > Settings…: they put it on the GPU | `8d85227` and before |
| **B5** | The Markdown preview on the GPU: a selection dragged costs 6.9 ms a paint against the CPU's 0.15 (every tile in sight drawn again) | `d01d425` |
| **B6** | (reading) The shaders have no variant for OpenGL 2.1 or OpenGL ES 2.0, yet the GPU is offered there; Direct3D below feature level 11 likewise | `d01d425` |
| **B7** | The Markdown preview on the GPU at a fractional pixel ratio (150 %): its text softened when scrolled | `d01d425` |
| N1 | The graph's line joining works in the diagram's units: zoomed in, half a slope's lines are made vertical, reducing nothing (invisible under the pen) | before |
| N2 | A PDF's reload gives the search model a null document: a Qt warning each time | before |
| N3 | The Graphics view zooms on any wheel or scroll, whatever the modifiers and the Mouse wheel setting: a trackpad's two-finger scroll zooms it | before |
| N4 | A graph's points are thinned to a logical pixel: two device pixels on a Retina screen | before |
| N5 | (reading) On the CPU, the PDF's scaled parts leave the smooth image hint on for the rest of the paint | `d01d425` |
| N6 | (arithmetic) The Markdown preview's tiles on the GPU can hold some 230 MB, and as much again in textures, for a full-screen preview on a Retina screen | `d01d425` |
| T1 | Eleven test functions in six files read a grab at logical coordinates: they fail at a ratio of 2, so on a Retina display run | tests |
| T2 | `test_qucs_control aLargeSchematicIsArrangedInTime` failed CI's macOS job on `287bf23`: its 3,200-part arrange 8.6 times its 800-part one, against a limit of 8, other test files running beside it | the test |
| D1 | The RHI write-up and commit say the GPU matches the CPU "pixel for pixel" in the layout and PDF viewers (A1, B3), and the Viewers tab's tooltip says the preview's tiles are drawn again "only where the text changes" (B5) | `d01d425` |

No crash, hang or sanitizer report in any probe, stress run or fuzzer. Of the findings, eight are this session's RHI move (`d01d425`); A2 is upstream Qucs-S's, and it may be part of what the user calls the data display's zoom bugs.

**The user's report during the hunt.** `fan1998_results.dpl` showed curves only in the canvas's top-left quarter. The app in `/Applications` was the d01d425 build, packaged before the Retina fix. On the fixed build, every graph of that page paints every line in sight at zooms 1, 1.5, 2 and 3, centred on each of its three diagrams. The 287bf23 DMG was rebuilt, checked and installed at the user's word (the old app moved to the Trash).

**Why the last hunt missed the Retina fault.** Its check of the culling (`huntCulledDiagrams`, 17 October) compared the canvas with the scene held under View > Zoom In's rectangle. Both pictures are rendered to an image, and an image is never culled, only a widget is. The same is true of `755bb00`'s own test. The fix's test (`aZoomedDiagramDrawsEveryLineInSight`) counts the widget's own paint.

## A1. The layout's vertical outlines a pixel right on Metal

The layout canvas's line shader moves lines half a device pixel right (`nudge` in `layoutviewgpu.cpp`). That was tuned on OpenGL, so that an edge on a pixel's boundary inks the pixel QPainter inks. On Metal it puts every vertical outline one device pixel right of the CPU's.

Probe P1 (8 layers, one square and one dot each, the pictures compared exactly):
- **with the nudge:** 1,684 pixels differ, whole columns at each square's left and right edges, the GPU's at x + 1; horizontal edges agree;
- **without it:** 245 differ (one edge and four dots);
- **at a ratio of 1** (`QT_SCALE_FACTOR=0.5`), the same: 974 of 8,628 inked pixels differ.

`theGpuDrawsAsTheCpu` passes because it lets a pixel's colour be found within a pixel of it, which is where this hides.

The probe's other purpose found nothing: 21, 22 and 40 layers take the canvas past its 64 uniform slots, and the slots grew as they should.

## A2. The zoom moves the point under the pointer

`Schematic::zoomAroundPoint` and `renderModel` work in whole model units. The point under the pointer is taken as a `QPoint` (`viewportToModel`), and the new view's top-left is rounded to a unit (`vpTopLeftOnModelPlane`). At zoom 10 a unit is 10 points, 20 device pixels on a Retina screen, so the content jumps by up to about a unit at each step.

Probe P7, on a data display, measured exactly from the scroll position and the scale: 40 steps, wheel notches and pinches of 0.02, from zoom 2 up to the limit of 10 and back to 1.2:
- the point moved on average 2.1 points a step, at most 6.45 (at 5.6);
- 27 of 40 steps moved it more than a pixel;
- wheel and pinch alike.

The layout viewer, the Graphics view (P8) and the image viewer (P12) keep their points exactly, to 0.6 px at most, all three in floating-point coordinates. The data display's zoom is the canvas's own (`Schematic`), shared with schematics.

## B1. The layered canvas's fixed cost grows with the window

Each frame of the layout viewer or Graphics view on the GPU does all of this, whatever changed:
- fills two transparent images the window's size;
- paints the view's QPainter layers on them;
- scans each for ink;
- uploads them.

The Graphics view also renders with 4 samples a pixel. OpenGL's painter drew into its framebuffer directly.

Probe P6, a pan's paint in a full-screen window (2022 by 1141 points, 4044 by 2282 device pixels):

| Content | CPU | GPU |
|---|---|---|
| The sample layout (a few shapes) | 6.7 ms median | 13.2 ms |
| 250,000 shapes | 56.1 ms | 15.2 ms |

Probe P13, the Graphics view full screen (2560 by 1321 points), a zoom step:

| Mesh | CPU | GPU |
|---|---|---|
| 100 by 100, its surface and contours | 38.1 ms | 30.4 ms |
| 300 by 300 | 58.2 ms | 33.7 ms |

So the GPU still wins on heavy content, but a light layout is twice as slow, and the Graphics view runs at about 30 frames a second at full screen whatever it shows. OpenGL's figures at these sizes were not measured.

## B2. A GPU viewport that cannot render stays blank

`Viewport2D` (the PDF viewer's and the Markdown preview's GPU viewport) gives up in two places:
- after 40 frames with no QRhi;
- when its pipelines are not made.

It then sets `a_failed` and draws nothing more. Nothing tells the view. `PageView` and `MarkdownDoc` keep the viewport, and the viewer is blank.

The layered canvases fall back: `canvasFailed` makes their views draw with the CPU. By reading; no failure could be forced here (`QT_WIDGETS_RHI_BACKEND=opengl` did not: the window follows the widget's Metal). B6 is one way it would happen.

## B3. Later PDF pages softened on the GPU

`PageView::relayout` places each page at the running sum of the heights above it, which is fractional at most zooms (only x is floored). The two paths draw a tile at that place differently:
- the CPU's `drawImage` at a point, without the smooth hint, snaps it to a device pixel;
- the GPU viewport draws the quad at the fractional place through a linear sampler, so the text comes out a little blurred and a device pixel lower.

Probe P9, page 3 of the test PDF. No tile was rendered or scaled in the GPU's frame: the same tiles as the CPU's.

| Zoom | Pixels apart | Mid-grey pixels, CPU / GPU |
|---|---|---|
| 1 | 0 | 3,321 / 3,321 |
| 1.37 | 7,099 | 4,219 / 4,518 |
| 1.53 | 893 | 4,783 / 4,877 |
| 2.11 | 12,719 | 5,342 / 8,448 (+58 %) |

The glyphs' tops are one device pixel lower on the GPU, column after column. `theGpuDrawsAsTheCpu` checks the first page at zoom 1 only.

## B4. `QUCS_GPU=0` and the Graphics view

The setting's text says `QUCS_GPU=0` "turns it off for all of them". The PDF viewer, the Markdown preview and the layout viewer follow `gpu::used()`, which checks it. A Graphics view is made on the CPU (its constructor checks the variable too), but three paths then put it on the GPU anyway:
- **its own menu's "Draw with the GPU":** `setGpu(true)` asks only `gpuAvailable()`, whether a backend exists, and the item is enabled;
- **Settings… > Viewers:** the notifier calls `setGpu(gpu::wanted(...))`, not `used()`, so every open Graphics view goes on the GPU;
- **Multiphysics > Settings…:** its OK calls `setGpu(on)` on every open view (`multiphysicssettings.cpp`).

Probe P10, `QUCS_GPU=0` on the display: made, on the CPU; its menu, on the GPU; the setting on, another view on the GPU.

Also, the menu's toggle writes the setting but tells no other open view (`setGpu` writes it directly; the Viewers tab uses `setWanted`, which notifies).

## B5. Selecting text in the Markdown preview on the GPU

`PreviewTiles::state()` holds the selection, so each change of it drops every tile, and the tiles in sight are drawn again, converted (ARGB32 to RGBA8888) and uploaded. QTextEdit on the CPU repaints only the band that changed.

Probe P11, a selection dragged 40 steps (1,771 characters) in a 1200 by 900 window:

| | Median paint | Worst |
|---|---|---|
| CPU | 0.15 ms | 0.23 ms |
| GPU | 6.94 ms | 8.55 ms |

Still within a 60 Hz frame here, but 46 times the CPU's. The Viewers tab's tooltip says the tiles are drawn again "only where the text changes" (D1).

## B6. No shader for OpenGL 2.1 or OpenGL ES 2.0

`qsb --dump` of the compiled shaders: SPIR-V 100, GLSL 150, GLSL 300 es, GLSL 330, HLSL 50 and MSL 12. `rhiAvailable()` only makes a QRhi: on Linux, the GLES2 backend on whatever OpenGL the driver gives.

On OpenGL 2.1 or ES 2.0 (old drivers, some virtual machines) a QRhi is made, `hindrance()` is empty, and the GPU is offered. But no variant fits; the shaders' `texelFetch` and flat varyings need GLSL 1.30 at least. The pipelines fail:
- the layout viewer and the Graphics view fall back to the CPU;
- the PDF viewer and the Markdown preview go blank (B2).

Direct3D 11 below feature level 11 cannot take HLSL 5.0 either. The OpenGL version asked for 3.3 and said "No OpenGL 3.3 here" instead. By reading and the dump; not reproduced here.

## B7. The Markdown preview at 150 %

A tile is placed at `row × 256 − scroll` points. At a ratio of 1.5, an odd scroll puts it half a device pixel off (237 → 355.5), and the linear sampler blurs it, as in B3.

`theGpuDrawsAsTheCpu` at `QT_SCALE_FACTOR=0.75` on the Retina display (a ratio of 1.5), scrolled to 237:
- 35,191 of 138,081 inked pixels differ even allowing a pixel's shift (25.5 %);
- 69,042 mid-grey pixels against 57,821 (+19 %): the text visibly softer.

The test passes: it compares 32 by 32 block means. At 1.5, the PDF viewer's first page (0 apart; 29 mid-pinch), the layout viewer (1) and the Graphics view hold.

## Minor notes

**N1. Line joining in the diagram's units.** `Graph::drawLines` joins consecutive lines whose x are within 0.25 of the diagram's units into a vertical run, once there are twice as many lines as the diagram is units wide. Zoomed in, a quarter unit spans several pixels.

Probe P2, a slope of 1,000 points on a 240-unit diagram: from zoom 2 up, 499 of 999 lines were made vertical (332 of 666 at 2). Nothing was reduced: 999 lines in, 999 out. Each joined line loses its 0.24 units of x. At zoom 8 the picture shows no gap, because the 1-unit pen covers it.

**N2. A PDF reload's warning.** `PdfDoc::reload` calls `a_search->setDocument(nullptr)`. Qt's search model then connects to a null document: "QObject::connect(QPdfDocument, QPdfSearchModel): invalid nullptr parameter", once per reload (13 in test_pdf_viewer's run).

**N3. The Graphics view's wheel.** `GraphicsView::wheelEvent` zooms on any vertical wheel or scroll, Shift and Ctrl included, and ignores Application Settings' Mouse wheel. A trackpad's two-finger scroll therefore zooms it. The schematic and data display, the layout viewer and the image viewer zoom with a plain wheel (by default) and scroll with Ctrl.

**N4. Thinning to a logical pixel.** A graph's points closer than a pixel in both x and y are passed over. The threshold uses `painter->transform()`, which has no pixel ratio, so on a Retina screen it is two device pixels. Sub-point wiggles are lost.

**N5. The smooth hint left on (CPU).** `paintScaled` used to `save()` and `restore()` around its smooth image hint. Through the new surface, `PainterSurface::drawImage(target, …)` sets the hint and leaves it on for the rest of the paint. Later pages' own tiles in that paint are then drawn smoothed at their fractional places: a frame of B3 on the CPU while tiles are still missing after a zoom. By reading.

**N6. The Markdown tiles' memory.** `PreviewTiles::draw` keeps rows up to three screens either side of those in sight: 7 times the rows in sight, which `theGpuPreviewDrawsWhatChanged` checks. Each holds an image the viewport's width and 256 points high, at the pixel ratio, and the GPU viewport holds a texture of each while the image lives. A full-screen preview on a Retina screen, 1,600 by 1,000 points, has about 5 rows in sight. A tile is 3,200 by 512 device pixels at 4 bytes, 6.6 MB. Up to 35 tiles kept is some 230 MB of images and as much in textures, for a scroll through a long document. By arithmetic from the code; not measured.

**T1. Tests that assume a ratio of 1.** With `QT_SCALE_FACTOR=2` offscreen, every non-solver suite entry passes except six files, eleven functions:
- `test_ink theCanvasOnDarkPaper`, `test_marker_colors aMarkerOnDarkPaperIsReadable`;
- `test_python_doc messagesAtLineEnds`;
- `test_python_tools cellsAndLinesRunInTheShell`, `breakpointsInTheMargin`, `theDebuggerStopsAndSteps`;
- `test_python_extras quickFixesOfALine`;
- `test_python_ide breakpointsWithConditions`, `foldsAndUnfolds`.

Each reads a grab's pixels at logical coordinates. Read at device pixels, `test_marker_colors` passes and `test_ink`'s card check passes (its wire check reads the same way). So these are tests, not the program, but they would fail on a run on a Retina display.

**T2. A timing ratio on CI's runner.** `aLargeSchematicIsArrangedInTime` arranges 800 and 3,200 parts and asks the larger to take under eight times the smaller's time (about four expected). CI's macOS job on `287bf23` (run 39120603433) had 160 and 1,377 ms, 8.6 times, on its 3-processor runner with three test files at a time. Here it passes three times in three, the whole function in 1.3 s. Every other entry passed there, the split VCSEL functions and the new ratio-2 entry among them. A ratio of two small timings on a shared runner: the check, not the arrange.

**D1. The write-ups.**
- `docs/feature_gaps/2026-10-17-claude-rhi-viewers.md` and `d01d425`'s message say the layout's and the PDF's pictures are the CPU's pixel for pixel. They are within a pixel, and not on later pages (A1, B3).
- The draft notes for v27.0.1 say the same; they are corrected before tagging.
- The Viewers tab's Markdown tooltip says tiles are drawn again only where the text changes (B5).

## Checked and found right

- **The data display's Retina fix (`2afb8bf`):** `aZoomedDiagramDrawsEveryLineInSight` and `aPlotOfManyCurvesZoomsSmoothly` pass offscreen at ratios 1.25, 1.5, 2 and 3, and under the sanitizers at 2 (5 s). The user's `fan1998_results.dpl`: every line in sight painted, as above.
- **The Markdown tiles' invalidation:** an edit, a re-render, `fitImages`, the style sheet (the theme), a typeset formula and the selection all reach the tiles' state, through the document's contents, its layout's `update`, the palette's cache key and the cursor. No path changes the picture with only a `viewport()->update()`.
- **The PDF selection's polygons on the GPU:** Qt makes each from a text run's rectangle, so fanning them is right (`Canvas2D::fillPolygon` assumes convex).
- **The Graphics view's GPU steps** are all outside its `save()`/`restore()` pairs (the layered canvas ends its painter at each step): no unbalanced save in any log.
- **The PDF viewer's caches** (tiles, character boxes, one-letter runs) are emptied by `setDocument`, which a reload calls with a null document first.
- **Eigen's LU and a remesh:** the pattern's union is only taken between matrices of the same size (`analyseEigen`).
- **Stress, on the display under the sanitizers:** no report, no failure.
  - P3: a layout tab opened, drawn and closed on the GPU 15 times, the GPU toggled 30 times, a resize to nothing;
  - P4: the Markdown preview's GPU switched while it scrolls and its text changes, its tab closed on the GPU, six rounds;
  - P5: Graphics views made and dropped on the GPU, resized to nothing, deleted while shown;
  - P14: the PDF viewer's GPU switched during zoom steps and pinches, reloaded mid-zoom, closed mid-zoom.
- **The layout canvas past 64 draws** (P1, 21 to 40 layers): its uniform slots grow; its pictures differ only as A1.
- **At 150 %** (the GPU tests at `QT_SCALE_FACTOR=0.75`): the PDF viewer, the layout viewer and the Graphics view as at 2 (B7 is the Markdown preview's).
- **The suite at a ratio of 2:** the drawing and viewer entries under the sanitizers, 16 of 18, the two failures T1's, no sanitizer report.
- **The Markdown preview scrolled sideways** (P15, a code block 2,667 pixels wider than the view, scrolled to 0, 333 and 1,001): the GPU's tiles the CPU's, 0 pixels apart.
- **The PDF's search highlights on the GPU** (P16, zooms 1, 1.37 and 2.11, the first page): 0 pixels apart, the highlights' pixels alike (3,037, 5,048, 10,540).
- **Fuzzing, ASan/UBSan:**
  - `fuzz-sch.py`, seed 18, 150 mutants of the 252 examples netlisted and rendered: 300 runs, no failure; seed 19, 100 rendered, and seed 20, 120 netlisted: no failure;
  - `fuzz_qfem.py`, seeds 1818 (20 mutants), 1819 (60) and 1820 (25), each opened, built, solved, described, evaluated and closed: no finding.

## Reproducing

The probes are patches against the test files at `287bf23`: `git apply docs/bug_hunts/2026-10-18-rhi-and-retina/<file>_probes.patch`, build the test, then run the function. The display runs need `QT_QPA_PLATFORM=cocoa`; the GPU functions skip offscreen.

| Finding | Probe | How |
|---|---|---|
| A1 | `test_layout_doc probeManyLayers` | `PROBE_LAYERS=8 QUCS_TEST_SHOTS=<dir>`, display; `QT_SCALE_FACTOR=0.5` for a ratio of 1 |
| A2 | `test_canvas_drawing probeZoomAnchor` | `PROBE_START=2 PROBE_STEPS=40 PROBE_PINCH=0.02` |
| B1 | `test_layout_doc probeBigWindowPan` | `PROBE_SHAPES=250000` for the large layout |
| B1 | `test_multiphysics_view probeGraphicsBigPan` | display |
| B3 | `test_pdf_viewer probeLaterPageSharp` | display, `QUCS_TEST_SHOTS=<dir>` |
| B4 | `test_multiphysics_view probeGpuEnvOff` | `QUCS_GPU=0`, display |
| B5 | `test_markdown_doc probeDragSelectTime` | display |
| B7 | `test_markdown_doc theGpuDrawsAsTheCpu` (unchanged) | `QT_SCALE_FACTOR=0.75`, display |
| N1 | `test_canvas_drawing probeSlopeJoined` | `PROBE_SHOTS=<dir>` |
| P3, P4, P5, P14 | `probeCanvasStress`, `probePreviewStress`, `probeGraphicsStress`, `probePdfStress` | the ASan build, display |
| P8, P12 | `probeLayoutAnchor`, `probeGraphicsAnchor`, `probeImageAnchor` | display |
| P15, P16 | `test_markdown_doc probeSidewaysScroll`, `test_pdf_viewer probeSearchHighlights` (the `*_probes2.patch` files) | display |
| The user's page | `test_canvas_drawing probeUserPage` | `QUCS_PROBE_DIR=<the page's folder> QUCS_PROBE_PAGE=fan1998_results.dpl` |
| T1 | — | `QT_SCALE_FACTOR=2 ctest -E '^test_fem_'` |
| B2, B6, N5 | — | by reading (`rhicanvas.cpp` `Viewport2D`; `qsb --dump`; `pdfdoc.cpp` `PainterSurface`) |
