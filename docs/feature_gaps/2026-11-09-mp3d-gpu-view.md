# Multiphysics 3D's view drawn by the GPU (2026-11-09)

The user asked: "Did we add support for GPU rendering to multiphysics 3D similar to what we did for multiphysics 2D?" It had
none: `View3D` (`view3d.cpp`) was a rasterizer of its own on the CPU - triangles, lines and dots into a QImage with a depth
buffer of its own, every turn of the camera all of it again. Then "implement GPU support yourself in this session".

## What it is

- **`View3DCanvas`** (`view3dgpu.{h,cpp}`), a `gpu::LayeredCanvas` over the view, as the 2D Graphics view's `GpuCanvas`: Qt's
  RHI - Metal on macOS, Direct3D 11 on Windows, OpenGL elsewhere -, 4 samples a pixel. Each frame: the background (the pass's
  clear), the scene by the GPU, then the axes, the Cut's words and the legend by QPainter (`View3D::paintOverlay`, the CPU's
  own), composed over it.
- **Buffers made once a scene.** The triangles (32 bytes a vertex: position, the face's normal, its colour), the lines and the
  dots (an instance each) are uploaded when `setScene` gives a new one (a stamp), or the screen's pixels a point change (the
  widths and radii are in device pixels). A turn, a pan, a zoom or the Cut's slider is a uniform block of 176 bytes and a
  frame: nothing is made again (the test counts the builds).
- **As the CPU draws** (three shaders, `shaders/view3d_{tri,line,dot}.{vert,frag}`):
  - The orthographic camera of `View3D::camera()`, its depth the GPU's, cleared to 1, nearer smaller: triangles `Less` (the
    first drawn wins where two are as near, as on the CPU).
  - Each face lit by a light at the eye on both sides, 0.38 + 0.62 |n·toward| (a field's colours 0.72 + 0.28), its corners'
    colours blended where a field is shaded.
  - Lines a quad round each segment, their width in whole device pixels (at least one), square ends - the CPU's brush - and
    dots a disc of their radius. Both drawn toward the eye by the CPU's bias (2.5 pixels, and a dot its diameter more) and
    tested `LessOrEqual`, so an edge on a face shows and one behind it is hidden.
  - The Cut a half-space each fragment is tested against: the same value as the CPU's, a hair beyond (`View3D::cutValue`, now
    shared by both).
  - Hidden and Frame Only parts (882c3c98) are left out of the scene, or drawn as its lines, by the document: the GPU draws
    what the scene holds.
- **The CPU's still**: `picture()` (exports, Claude's screenshots of the view, the tests) and `idAt()` (a click's pick, the tree's
  selection) render on the CPU as before. Where the GPU is not wanted, not available, or its pipelines are not built
  (`canvasFailed`, the CPU from then on), the view draws itself as it did.
- **The setting**: a key of its own, `Viewers/multiphysics3dGpu` (`gpu::Viewer::Multiphysics3D`), the CPU's unless chosen, as
  every viewer's. Chosen in three places, each telling the open views (`gpu::Notifier`):
  - Settings… > Viewers: "Multiphysics 3D view".
  - Multiphysics 3D > Settings…: a new "3D view" group, "Draw with the GPU", its status (the backend, or what is in the way).
  - The view's own right-click menu: "Draw with the GPU" (and Fit), disabled where the GPU cannot draw.

  `QUCS_GPU=0` keeps the CPU, whatever is chosen.
- `gpu::PipelineSpec` gained `depth` and `depthOp` (`rhicanvas.h`); the 2D canvases' pipelines are as they were.

## Checked

New `tests/test_view3d.cpp`. A box of six coloured faces, a field of blended colours over its top on a 24 by 24 grid with its
mesh's lines, the box's edges 2 points wide, its corners' dots, and the axes and words over them:

| View | Pixels more than 40 apart in a channel | Of the smooth ones (faces, light, field) |
|---|---|---|
| Iso | 2.20 % | 0.000 % |
| Top | 2.97 % | 0.000 % |
| Front | 0.21 % | 0.000 % |
| Cut half away along x | 1.08 % | 0.000 % |
| Turned and zoomed 8 times | 3.16 % | 0.000 % |
| A coarser scene set | 1.18 % | 0.000 % |

On Metal. All the differences are along the edges and lines: the GPU's 4 samples a pixel against the CPU's none. The test
allows 4 % in all, and 0.2 % of the pixels where the CPU's picture is smooth. Also checked:

- **A deliberate break caught**: a field's colours lit as a plain face's gave 10 % of the smooth pixels apart.
- **Builds**: eight turns and zooms made no buffers again, while the frames went on; a new scene made them once.
- **Speed**: a scene of 320,012 triangles took 3.9 ms a frame on the GPU (read back each time) and 54.7 ms on the CPU.
- **The setting**: its key and name; the CPU without a GPU (the offscreen platform) whatever is asked; QUCS_GPU=0 keeping the
  CPU through the setting and the menu, the menu's item unchecked and disabled.

**Test runs.** Through ctest: test_view3d offscreen (the GPU's test skipped) and its GPU test on the display (Cocoa,
QT_QPA_PLATFORM=cocoa in a scratch ctest file). Offscreen, test_multiphysics_view, test_multiphysics3d_doc,
test_multiphysics_doc, test_layout_doc, test_qucs_control and test_gpu_probe; test_multiphysics_view's GPU tests on the display
(the shared pipeline helper changed). The four lints are green; the six shaders were compiled by `qsb` as
`scripts/compile-shaders.sh` does, the others' .qsb left byte for byte.

## Not done

- **Not looked at in the running app.** The GPU test shows the canvas over a real window on Cocoa, but not inside the
  program's Multiphysics 3D tab.
- **Metal only**: Direct3D 11 and OpenGL not run. The shaders compile for all of them, as the 2D view's.
- **Picking stays on the CPU**: a click renders the triangles' ids on the CPU, as before (some 50 ms for 300,000 triangles). An
  id buffer on the GPU would make it a read of one pixel.
- **Lines are not antialiased beyond the samples**: the 2D view's lines are; here a 1-point line is a quad of one device pixel,
  smoothed by the 4 samples alone.
