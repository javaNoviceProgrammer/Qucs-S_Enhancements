# The GPU viewers on Qt's RHI

The user (2026-10-17), after asking why the viewers used OpenGL on macOS rather than Metal: *"Now let's fully migrate to Qt's RHI."*

The four viewers the GPU can draw now draw through Qt's RHI:
- the PDF viewer;
- the Markdown preview;
- the layout viewer;
- the multiphysics Graphics view.

The RHI uses Metal on macOS, Direct3D 11 on Windows and OpenGL elsewhere. Nothing in the program uses Qt's OpenGL modules any more. The GPU stays an opt-in, each viewer (Settings… > Viewers).

## The build

- **Qt 6.7 or later.** The canvases are `QRhiWidget`s. The RHI's headers come with Qt's GUI private module (`Qt6::GuiPrivate`), now linked in place of `Qt6::OpenGL` and `Qt6::OpenGLWidgets`. Without them the CPU draws every viewer, as before.
- **Shaders compiled beforehand.** They are written once, in Vulkan-style GLSL (`qucs/shaders/*.vert`, `*.frag`). `scripts/compile-shaders.sh` compiles each with Qt's `qsb` into a `.qsb`: GLSL ES 3.0, 1.50 and 3.30, HLSL 5.0, and MSL 1.2. The `.qsb` files are kept in the repository and in the program's resources, so a build needs no Qt Shader Tools.
- **A CI check.** `scripts/ci/check-shaders.py` (a new step of CI's lints) fails when a shader was changed without being compiled again, or when its `.qsb` is missing or not in `qucs.qrc`.

The shaders avoid what one backend lacks:
- no `gl_VertexID`: a corner number per vertex instead;
- no point sizes (Direct3D has none): a layout's dots are instanced quads;
- no constant vertex attributes: a uniform says when one colour and width serve a whole draw;
- `gl_FragCoord` read from the bottom or the top, as the backend has its framebuffer.

## How each viewer draws

**The layout viewer and the Graphics view: a layered canvas** (`rhicanvas.h`, `LayeredCanvas`). The view's own QPainter drawing — the grid, texts, selection, rulers, legend and title — goes to transparent images the canvas's size. A GPU step (a layout's layers, a mesh, a surface) ends one image and the painting goes on, its state kept, on the next. The images and steps are composed in their order, so what the view paints over a step stays over it, as with OpenGL's painter. The Graphics view draws with 4 samples a pixel, as before.

**The PDF viewer: a GPU viewport** (`Viewport2D`). The pages' painting goes through a small surface: the CPU's QPainter, or the GPU's frame of fills and images (`Canvas2D`). A tile becomes a texture, kept by its image's key. During a zoom, the tiles of other zooms are scaled, each cut to the part it alone draws.

**The Markdown preview: tiles** (`PreviewTiles`). The CPU draws the document as QTextEdit draws it, in rows 256 pixels high and the viewport's width; the GPU keeps them and scrolls them. A scroll draws only the rows it brings into sight. The rows are drawn again when what they show changes:
- the layout's update (an edit, an image or a formula typeset);
- the selection;
- the palette;
- the width, the horizontal scroll or the pixel ratio.

The quotes' bars are drawn over the tiles each frame. Since the glyphs are the CPU's own, the OpenGL engine's heavier glyphs are gone: the preview on the GPU is the CPU's, pixel for pixel.

## Found on the way

1. **A canvas made with its parent was never shown.** `QRhiWidget`'s constructor turns its RHI on only after `QWidget`'s constructor has parented it. Qt switches a window to composing with the RHI when a widget asking for it is parented into it, so made that way the window never switched. On screen the canvas's frames were not composed at all. Its grabs still worked, through a private QRhi of their own, so the tests did not see it. The canvases are now parented after construction.
2. **The area painted its GPU viewport with QPainter.** A `QRhiWidget` paints itself at once when shown or resized. Inside `setViewport`, before the viewport took its own paints, QTextEdit's and the PDF view's painting had that paint: "QPainter::begin: Paint device returned engine == 0". The first paint now waits until the viewport takes its paints; a viewport in no window yet paints when it is shown.
3. **A frame with no QRhi yet is not the end.** Before a window's first frame on the GPU there may be none, and Qt says so with `renderFailed`. That used to give the GPU up for good. Now the frame is tried again (40 times, 50 ms apart), and the CPU takes over only after that, or when no backend can be made at all.
4. **Textures of images let go.** A viewport kept each image's texture for up to 120 frames or 384 MB, though the image itself was gone. A selection dragged over the Markdown preview makes new tiles at each step, so dead textures piled up. A texture is now dropped at the next frame once its image is destroyed or changed: Qt's image cleanup hooks, as Qt's OpenGL engine did.
5. **An empty triangle a rectangle.** `QPolygonF(QRectF)` repeats its first point at the end, so a rectangle was fanned into three triangles, one of them empty. The closing point is dropped.

## Measured

On the Retina display, Metal. The OpenGL figures are from `2026-10-17-claude-gpu-viewers.md`.

| Viewer | What | CPU | OpenGL (before) | RHI (Metal) |
|---|---|---|---|---|
| Layout | The GPU's picture against the CPU's | | Edges differed (5 % allowed) | **0 pixels differ** within a pixel (the sample; the shapes in each pattern) - see the correction below |
| Layout | A million shapes, a pan or a zoom | 34 to 63 ms | 13 to 18 ms at most; buffers 35 ms | **6 ms at most; buffers 5 ms** |
| Layout | 8,000 curves of 96 points, a view | 45 ms | 6 to 8 ms; buffers 9 ms | **4 ms; buffers 4 ms** |
| Layout | A layer hidden, shown, first shown | 34 ms | 4, 4, 128 ms | 4, 4, 124 ms |
| Graphics view | Pixels more than 40 apart from the CPU's | | Under 3 % | Geometry and boundaries 0.00 %; mesh 1.26 %; surface 0.10 % |
| PDF | The GPU's picture against the CPU's | | Under 0.5 % allowed | **0 of 2,574,144**; mid-pinch 0 of 2,514,848 (the first page - see the correction below) |
| PDF | A scroll's paint (median) | 2.8 ms | 0.12 to 0.22 ms | 1.3 ms |
| Markdown | Inked pixels differing from the CPU's | | 8 to 20 % (heavier glyphs) | **0** (plain, selected, scrolled) |
| Markdown | A scroll's paint (median) | 2.2 ms | 1.27 to 1.42 ms | 1.2 ms |

**A PDF scroll's paint is slower than with OpenGL** (1.3 ms, against 0.12 to 0.22 ms), though still half the CPU's. A `QRhiWidget` draws each frame as an offscreen frame of the RHI, and Qt submits that frame and waits for the GPU to finish it (`QRhi::endOffscreenFrame`; on Metal `waitUntilCompleted`). That wait is about a millisecond a frame, whatever is drawn: the Markdown preview, all its tiles kept, paints in 1.2 to 1.4 ms. OpenGL's paint only queued its commands.

## Tests

- **test_layout_doc:**
  - `aMillionShapesAreReadAndDrawnInTime`: the window composes with the RHI once the canvas is in it (its surface no longer a raster one);
  - `theGpuDrawsAsTheCpu`: on Metal, alike within a pixel (a pixel in a thousand allowed); elsewhere as before.
- **test_multiphysics_view:** the canvas a `QRhiWidget`; on Metal, the geometry's picture the CPU's (under 0.1 %).
- **test_pdf_viewer:**
  - `theGpuCanvasClips` (new; no display needed, so it runs on CI): Canvas2D's clip cuts fills, polygons and images, each image's source in proportion; colours premultiplied;
  - `theGpuDrawsAsTheCpu`: no QPainter on the GPU's viewport; frames of the same view send no texture again; mid-pinch the GPU's frame is the CPU's.
- **test_markdown_doc:**
  - `theGpuPreviewDrawsWhatChanged` (new): a frame with nothing changed draws no tile; half a row's scroll draws one at most, back none; a selection, an edit and a new width draw those in sight again; tiles three screens away are let go, with their textures;
  - `theGpuDrawsAsTheCpu`: no QPainter on the GPU's viewport.
- **test_multiphysics_doc:** the settings' wording (no "OpenGL 3.3").

## Breaks

Thirteen changes broken on purpose, all caught:
- the canvas made with its parent (the window not switched);
- the viewport painted when shown, before it takes its paints;
- the selection not in what the tiles show;
- the tiles drawn at every frame;
- the far tiles kept;
- a layout pattern's rows read the wrong way up;
- the Graphics view's lines' rows read the wrong way up;
- an image not cut to the clip;
- a polygon not cut to the clip's right edge;
- the layout's lines not nudged half a pixel;
- the view's QPainter layers dropped;
- the PDF's tiles sent again at each frame;
- the textures of tiles let go kept.

The shader check fails on a shader changed after its compilation, and on a `.qsb` missing from `qucs.qrc`.

## Runs

- **The Release suite (offscreen):** 127 of 127.
- **On the display (Metal),** with no RHI or painting warning:
  - test_layout_doc 31 of 31;
  - test_pdf_viewer 29 of 29;
  - test_markdown_doc 19 of 19 (its fuzzer skipped, as unless asked);
  - test_multiphysics_view 5 of 5 (skipped: the no-GPU case and the benchmark);
  - test_multiphysics_doc 57 of 57.
- **Under the sanitizers,** no report:
  - offscreen: test_layout_doc, test_pdf_viewer, test_markdown_doc, test_multiphysics_view, test_multiphysics_doc, test_canvas_drawing, test_graphics_export;
  - on the display, the GPU's functions: the layout's six, the PDF's two, the Markdown preview's two, test_multiphysics_view whole.
- **The lints pass,** the shader check with them.

## Corrected, 18 October

The hunt of 18 October (`docs/bug_hunts/2026-10-18-rhi-and-retina.md`, D1) found two claims above too strong:
- **The layout viewer** was alike within a pixel, not pixel for pixel: the test found a pixel's colour within a pixel of it, and on Metal every vertical outline was a device pixel right of the CPU's (A1, OpenGL's half-pixel nudge kept).
- **The PDF viewer** was the CPU's on the first page only: later pages were drawn a device pixel lower at most zooms, softened (B3).

Both are fixed (`docs/feature_gaps/2026-10-18-claude-rhi-hunt-fixes.md`). The PDF is now the CPU's exactly on every page. The layout is the CPU's but for 1 to 5 % of its inked pixels, along diagonal edges, where the two rasterizers differ.
