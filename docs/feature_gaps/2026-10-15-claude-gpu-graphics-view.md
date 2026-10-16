# The multiphysics Graphics view on the GPU

The request (2026-10-15): "full GPU support or something that makes graphics handling and
zoom in and out much smoother" for the .qfem Graphics view.

## What was slow

Every frame of a pan or a zoom was drawn again from nothing by QPainter's raster engine on
the CPU: the color surface rasterized pixel by pixel (barycentric interpolation over every
triangle, one thread), every mesh edge stroked with antialiasing, every contour segment,
the domains' paths filled. Measured on the test's square grid (1400 x 900 view, Retina 2x,
the surface, its mesh and 20 contours shown; ms per frame):

| triangles | surface only | + mesh | + contours |
|-----------|-------------:|-------:|-----------:|
| 20 000    | 26           | 37     | 42         |
| 180 000   | 27           | 72     | 80         |
| 720 000   | 46           | 165    | 181        |

At 180 000 triangles (the VCSEL bench's half mesh is 130 000) a zoom ran at 12 frames a
second; at 720 000, five.

## What it does now

**The GPU draws the heavy layers** (`multiphysicsviewgl.h`, `GlCanvas`, a QOpenGLWidget
over the view, OpenGL 3.3 core):

- the domains (their triangles, each vertex its domain's color; a hover or a chosen domain
  uploads the colors alone again);
- the color surface: each vertex its value scaled to the color range in doubles (a field of
  300 K ± 1 mK keeps its colors), the fragment's color the table's entry of the value
  interpolated - the CPU raster's rule, from a 256-texel table;
- the mesh's edges (each edge once), its quality colors, the boundaries, the contours and
  the rays: each segment an instance, a quad round it in pixels, its pixels covered by
  their distance from it - antialiased at any width, round ends for the boundaries, butt
  ends for the rest; a cosmetic width in device pixels as QPainter's cosmetic pens (found
  by the comparison test: at 2x the GPU's boundaries had come out twice as thick);
- each segment clipped to the viewport in the vertex shader, so a segment thousands of
  screens long, deep in, keeps its pixels' floats exact.

Buffers are made once a topology, mesh or plot (stamps the view counts) and then cost a pan
or a zoom nothing but two uniforms. Coordinates are floats from an origin - the content's
middle, moved to the view's center when, deep in and far from it, a float's step would
reach a pixel (2 million pixels away): a pixel 1e-9 of the model at 0.375 is drawn where
the CPU draws it.

**The text stays QPainter's** - the grid and its labels, the title, the legend, points,
arrows - drawn by the same code on the canvas's GL paint engine, the GPU's layers between
its native painting: one painting routine (`GraphicsView::paintFrame`) for both.

**Where:** where an OpenGL 3.3 core context can be made and every window's OpenGL is the
core profile (`GraphicsView::prepareGpu()` in `main()`, before the application: macOS shares
only alike contexts - without it Qt falls back to an unshared context, the case the
document tests showed). Not on the offscreen or minimal platforms (the suite, the MCP
server). A context that is not made, or shaders that do not build, hand the drawing back
to the CPU, said once. The view's context menu (right click) has *Draw with the GPU
(OpenGL)*, kept in the settings (`Multiphysics/gpu`); `QUCS_GPU=0` turns it off.

**Pictures stay the CPU's** (`picture()`: copy, export, Claude's fem picture), the same
as before, antialiased.

**The CPU's own drawing is faster too**, for the offscreen platform, machines without
OpenGL 3.3 and the GPU turned off: the surface's raster in bands of rows, one a core (its
bits once - `scanLine()` would detach the image in each thread); a large mesh's edges
(60 000 or more) unantialiased on screen, antialiased in a picture; the mesh's edges found
by sorting, not a set, and its path made when the CPU first draws it.

## Measured

The same grid, 1400 x 900 at 2x, the surface, mesh and contours shown, a zoom (ms a frame;
the GPU's including reading the whole frame back, 5 megapixels, which a frame on screen
does not):

| triangles | CPU before | CPU now | GPU |
|-----------|-----------:|--------:|----:|
| 20 000    | 42         | 20      | 4.8 |
| 180 000   | 80         | 35      | 5.8 |
| 720 000   | 181        | 81      | 6.8 |

## Tests

`test_multiphysics_view` (new):

- `theSurfaceRasterInBands` - a field linear in x, every sampled pixel of the square
  (9 000 and more) the color of its x within a step of the table, on every row, across
  every band's edge; again at another size.
- `theGpuWhenThereIsNone` - offscreen: asked for the GPU or not, the CPU draws.
- `theGpuDrawsAsTheCpu` - the GPU's frame (`grabFramebuffer`) against the CPU's picture:
  the geometry with a domain chosen, then a boundary chosen (its pixel the CPU's blue,
  #50a0ff), the mesh, a surface with contours and its mesh, another plot (its buffers made
  again), resized (the canvas the view's size) - under 3 % of the pixels more than 40 apart
  (0.16 %, 0.16 %, 1.57 %, 0.16 %, 0.34 %, 0.20 %: the letters' and the lines'
  antialiasing); a zoom the GPU's frames, no raster; deep in (a pixel 1e-9 of the model,
  its middle half a float's step from where the old origin would put it), the mesh's line
  at the CPU's pixel (532 of 1200); turned off, no canvas, a view made then the CPU's; on
  again, alike.
- `bench` (QUCS_VIEW_BENCH=1) - the tables above.

The GPU's tests need a window system's OpenGL: they skip on the offscreen platform the
suite and CI run on, and were run here under cocoa (QT_QPA_PLATFORM=cocoa; the display
asleep - the canvas draws into its own framebuffer, exposed or not). test_multiphysics_doc
passes under cocoa too (49 of 49), the canvas in the main window.

Twelve changes broken on purpose, all caught: a band's last row skipped, the raster's color
index, cosmetic widths scaled by the pixel ratio, no rebase of the origin, the domains'
colors not uploaded again, the plot's stamp not counted, the canvas not resized, the surface
rastered with the GPU on, the context's signal left connected (a crash at the canvas's
deletion - the first version's, found by the test), the canvas kept when turned off, the
surface's values not scaled, the boundaries' strokes not uploaded again. Two of them were
missed at first - the deep zoom's middle was by chance almost a float, and no test chose a
boundary - and the test made to see them.

## Choices and limits

- QOpenGLWidget, not QRhiWidget (Metal on macOS): its shaders are GLSL in the source - no
  Qt Shader Tools (qsb) at build time, none on CI's three platforms - and QPainter paints
  on it, so the grid, labels and legend are the same code. If Apple removes OpenGL, the
  canvas is the one class to port to QRhiWidget; the CPU path stays meanwhile.
- `GraphicsView::show()` is the view's own (what it shows: geometry, mesh, results) and
  hides QWidget::show(): a test shows the view with setVisible(true).
- Untried here: Linux and Windows drivers (OpenGL 3.3 core; Mesa's software one is
  enough). The GPU's tests run wherever a display is.
- During a gesture the CPU path still draws each frame in full; a cached frame scaled
  between full ones would make it smoother where there is no GPU.
