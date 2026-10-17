# The GPU in the PDF viewer, the Markdown preview and the layout viewer

The request (2026-10-17): *"Let's add GPU rendering and acceleration to the pdf viewer, markdown viewer, and gds layout viewer and add a setting option in qucs setting so that the user can manually choose CPU or GPU acceleration for these."*

An earlier note (2026-10-16-claude-multiphysics-settings.md) had named the layout viewer the best candidate and expected nothing from the other two. Measured now, the PDF viewer gains as well. The Markdown preview is drawn by the CPU unless chosen.

> **Corrected the same day** (2026-10-17-claude-markdown-dark-mode.md): the Markdown figures below were measured while the preview had wrong colours in a dark theme. That fault also let the CPU scroll by moving the picture. Measured again with the right colours, the GPU is a little quicker. The preview keeps the CPU by default for its glyphs, which Qt's OpenGL engine draws a little heavier than the platform does.

## The setting

**Settings… > Appearance > Drawing: the GPU (OpenGL) or the CPU.** One choice per viewer:

| Viewer | Kept as | Default |
|---|---|---|
| PDF viewer | `Rendering/pdf` (`gpu`, `cpu`) | GPU |
| Markdown preview | `Rendering/markdown` | **CPU** (its glyphs the platform's; see the correction above) |
| Layout viewer (GDS, OASIS) | `Rendering/layout` | GPU |
| Multiphysics Graphics view | `Multiphysics/gpu` (also in Multiphysics > Settings… and the view's menu) | GPU |

- **Applied at once:** on Apply or OK, every open viewer switches; no reopening.
- **A status line says whether the GPU can draw here.** If it cannot, it says why: `QUCS_GPU=0` set, a build without OpenGL, or no OpenGL 3.3 (an offscreen or remote display). In those cases the CPU draws, whatever is chosen.
- Each choice has a tooltip saying what the GPU does for that viewer.

## How each viewer draws on the GPU

- **The layout viewer** (`layoutviewgl.h`):
  - **One flatten, many frames.** The cell shown is flattened to its depth once into OpenGL buffers, per layer:
    - fills: a convex shape fanned, the rest triangulated by CDT, odd-even as QPainter fills them, so a GDSII hole's slit stays a hole;
    - outlines;
    - a dot for each shape under 1.5 pixels.
  - **What a pan or a zoom costs.** It is then a frame of the GPU's; the CPU draws nothing.
  - **What makes new buffers.** They are made again only when the layout, the cell, the depth, the layers shown or the texts change. A colour, a pattern or a theme needs none.
  - **Patterns are Qt's own** 8 × 8 brushes, as textures fixed to the device pixels, as QPainter draws them.
  - **QPainter still draws the overlays:** the grid, texts, cell names, the selection and the rulers.
  - **Past two million shapes the CPU draws the layout**, as before.
- **The PDF viewer:**
  - Its viewport is an OpenGL widget.
  - The pages are still rasterized by pdfium on the CPU, tile by tile.
  - The GPU keeps the tiles as textures and draws, scrolls and scales them.
  - A tile's pixel ratio is now set once, when it is made. Setting it at each paint made a copy, and so a new texture, every time.
- **The Markdown preview:**
  - Its viewport is an OpenGL widget.
  - It draws the document as `QTextEdit` does, through the document layout's own `draw`: scrolled, with its selections, in its palette. The quotes' bars are drawn over it (`drawPreview`).
  - Text comes from a glyph atlas; images are textures.

**Why the PDF viewer and the preview draw in `paintGL`.** Both draw through a function the viewer gives its OpenGL viewport, called from the viewport's `paintGL` with a painter on its framebuffer. They do not let the area paint the viewport itself. The reason is what a grab does (see "Found doing it").

## Measured

Measured on an M2 Ultra under macOS 26, on its own display, by the tests below; the ranges are over three runs. Times are the CPU's time in the paint; the GPU's own work runs alongside.

| Viewer | What | CPU | GPU |
|---|---|---|---|
| Layout | A million shapes, all in sight: a pan or a zoom | 33 to 63 ms | 13 to 18 ms at most; its buffers 35 ms, once |
| Layout | The same, zoomed into a corner | 3 ms | 4 to 6 ms |
| Layout | 8,000 rings and C's of 96 points, all in sight: a view | 45 to 75 ms | 6 to 8 ms; its buffers 9 ms, once |
| PDF | A scroll's paint (median of 20) | 1.8 to 3.0 ms | **0.12 to 0.22 ms** |
| Markdown | A scroll's paint (median), as first measured (wrong colours) | 0.25 to 0.30 ms | 1.38 to 1.41 ms |
| Markdown | The same, measured again with the right colours | 2.05 to 2.22 ms | **1.27 to 1.42 ms** |
| Markdown | All of it painted again, with the right colours | 1.7 to 2.0 ms | 1.5 to 1.7 ms |

**Zoomed into a corner of a large layout, the CPU is as quick.** It draws only the shapes in sight, while the GPU draws every shape from its buffers, those out of sight too. The GPU's gain is the whole layout in sight, and every pan or zoom without a new picture.

**Why the Markdown preview stays on the CPU.** As first measured, a CPU scroll moved the picture and painted only the lines coming in, about five times cheaper than the GPU's whole frame. That was the colour fault: the wrong fill let Qt move the picture. With the right colours, every scroll paints the whole view on either path, and the GPU is a little quicker. The CPU stays the default for its glyphs, the platform's own. The GPU is a choice.

**Alike, picture for picture** (the same view grabbed each way; "alike" means a pixel's colour within a pixel of it in the other picture):

| Picture | Differ |
|---|---|
| The layout test shapes (an L, a square with a slit hole, a bow tie, in every pattern) | 0 pixels |
| The sample layout | 2.84 % of the inked pixels, all in its text's glyphs (the GL engine's grey edges) |
| The curves | 4.9 % of the sampled pixels, at the curves' edges (triangles rasterized against QPainter's spans), checked by eye |
| A PDF page | 0 pixels |
| Markdown: headings, a table, code, math, quotes, an image | 0.28 % of the inked pixels on the wrong (dark) paper; 10.2 % on the right white paper, at the glyphs' edges; no 32 by 32 block's mean colour apart by more than 9 |
| Markdown, with a selection | 0.45 %; on the white paper 7.8 %, blocks within 9 |
| Markdown, scrolled 237 pixels down | 0 pixels; on the white paper 19.6 %, blocks within 9 |

## Found doing it

1. **Screenshots of a GPU canvas lost what QPainter drew.**
   - The cause: Qt's `QWidget::grab` (used by Claude's screenshot tool, `screenshot` with area `window` or `screen`) calls an OpenGL widget's `paintGL`. A `QPainter(this)` there is attached to the grab's raster painter.
   - The effect, in the layout canvas:
     - its background, grid and texts went into the grab's pixmap, under the framebuffer's picture;
     - the framebuffer showed the frames before it, never cleared: the curves test saw four copies of the layout, a pan apart.
   - **The multiphysics Graphics view's canvas (7cd3672) had the same fault.**
   - Both now paint with a painter on the framebuffer itself (`QOpenGLPaintDevice`), so a grab is the frame.
2. **Switching the Markdown preview's viewport crashed** (SIGSEGV in `QTextDocumentLayoutPrivate::scaleToDevice`).
   - The cause: `QTextEdit` measures its layout by its viewport, and the layout kept a pointer to the deleted one.
   - Now the layout is pointed at the new viewport before the old one is deleted.
3. **An OpenGL viewport's grab was empty or stale.**
   - What Qt does:
     - `QAbstractScrollArea::setViewport` turns `autoFillBackground` on for an OpenGL viewport. A graphics view expects this, but it means the framebuffer is cleared at every painter's start, so the quotes' bars wiped the text under them.
     - A grab paints only `paintGL`, and `QOpenGLWidget`'s own `paintGL` clears.
   - What was seen: the PDF page and the Markdown text came out as an empty background in every grab, while the screen showed them right (checked with a capture of the screen).
   - Now the viewport's `paintGL` draws everything, as the layout canvas does: each paint and each grab.
4. **The existing test `pagesAreShownAndGoneThrough` grabbed before the first paint.** It now waits for the picture to settle. With the GL viewport, its grab is fresh.

## Tests

- **test_layout_doc:**
  - `theGpuDrawsAsTheCpu` checks the background pixel too: it is now painted in a grab.
  - `theGpuDrawsCurvesFaster` (new): the curves above.
    - The GPU at least twice as fast as the CPU.
    - Under a tenth of the sampled pixels differ.
    - One build of its buffers, none approximated.
  - `theDrawingSettings` (new), with a layout, a PDF, a Markdown preview and a Graphics view open:
    - the defaults;
    - the four choices shown;
    - the status line;
    - each choice switched and applied, kept and followed by each viewer;
    - shown again;
    - Cancel keeps nothing;
    - `QUCS_GPU=0` said and obeyed;
    - back to the defaults.
- **test_pdf_viewer `theGpuDrawsAsTheCpu` (new):**
  - GPU by default;
  - the setting switches the open view;
  - the page alike pixel for pixel;
  - a scroll's paint timed each way.
- **test_markdown_doc `theGpuDrawsAsTheCpu` (new):**
  - CPU by default;
  - plain, selected and scrolled pictures alike;
  - the image drawn;
  - a document shorter than the view: the paper under it on the GPU, and on the CPU again after the GPU;
  - the screen's frame the grab's (below);
  - the scroll kept across a switch;
  - scroll and whole paints timed;
  - back to the default.
- **test_multiphysics_view `theGpuDrawsAsTheCpu`:** the view also grabbed as a window is grabbed, and grabbed again after three zooms with no paint of the window between.
- **What the screen shows is tested too.** `gpu::shownFrame` reads an OpenGL viewport's framebuffer back as the window last painted it, before any grab paints it again. The Markdown test compares that frame with the grab: the quotes' bars pixel for pixel, and the whole picture with no pixel apart.
- **The GPU comparisons need OpenGL 3.3.** They skip offscreen (CI's display) and run on a desktop.

Sixteen changes broken on purpose, all caught. Ten were caught at once; six only after the tests were tightened:

- **The canvases:**
  - the layout canvas painting with a painter on the widget;
  - the same for the multiphysics canvas (caught once the view was grabbed again after it moved);
  - the GL viewport's background not filled (caught once a document shorter than the view was added);
  - a viewport back from the GPU not filling its background (the same);
  - the GL viewport not taking its own paints (caught once the screen's frame was read before a grab);
  - the text layout's paint device not pointed at the new viewport (a SIGBUS).
- **The preview:**
  - without its selection;
  - not scrolled;
  - without the quotes' bars (caught once their pixels were compared exactly: in the test's dark display they are 6 levels from the paper);
  - the bars' filter left on the GPU viewport, so the bars are drawn twice on the screen (caught once the screen's frame was read).
- **The PDF viewer:** its pages drawn in an empty rectangle on the GPU.
- **The setting:**
  - the Markdown preview on the GPU by default;
  - the defaults wrong otherwise;
  - a choice not told to the open viewers;
  - Apply keeping no drawing choice;
  - `QUCS_GPU=0` not heeded.

## Runs

- **The Release suite (offscreen, as CI runs it):** 127 of 127, before the breaks and again after the rebuild.
- **The touched tests on the display** (layout, PDF, Markdown, multiphysics view and document): their new functions pass. Three things fail there that already fail at 5e4a02f (built from a worktree of it and run the same way):
  - four layout picture tests: `aLayoutOpensInATabWithItsCellsAndLayers`, `theCanvasIsLightOrDarkWhateverTheTheme`, `theLayersStandOutFromTheCanvas`, `aGridIsBehindTheLayout`;
  - two Markdown picture tests: `quotesHaveTheirBar` and `drawnWhereLaidOutInAWindow`;
  - the PDF `everyCommandLeavesItWhole` sometimes stops in File > Save As: macOS's own save panel, which the test's dialog closer cannot close. It ran to QtTest's 300 s limit once, and passed in other runs.

  All pass offscreen.
- **An older fault found on the display:** in macOS dark mode with the platform theme, the Markdown preview has black text on a near-black background. Its paper and ink are set as a palette, which the main window's style sheet resets; the text editor sets them as a style sheet for this reason. It is the CPU's picture too, so it was not fixed here. It was flagged as a task of its own, and fixed in the next commit (2026-10-17-claude-markdown-dark-mode.md), with both Markdown tests above.
- **Under the sanitizers** (offscreen and on the display): the new and changed functions, and the whole PDF and Markdown tests before their last changes. No report.
- **The lints pass.**

## Not done

- **pdfium still renders the PDF pages on the CPU.** The GPU draws the tiles.
- **The Markdown preview marks a link focused by the keyboard (Tab) as a selection,** not with the style's focus format, which `QTextEdit` reads from private state.
- **What reaches the screen is read from the viewport's framebuffer** (`shownFrame`), not from the display itself. The display was checked once, by a capture of the quotes.
