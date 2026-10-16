# Multiphysics > Settings…, and where else a GPU would help

The request (2026-10-16): "Add a "Settings..." menu item to "Multiphysics" menu. Then, make
this also available in that settings: Switching it off: right-click the view and untick
"Draw with the GPU (OpenGL)"; the setting is kept. QUCS_GPU=0 also turns it off. Where else
can we add GPU support and GPU acceleration? How about for the pdf viewer and markdown viewer
and other viewers that we have? In terms of multiphysics, what about the mesh creation? ...
what about the actual solver?"

## Settings…

| Asked | Now |
|---|---|
| A "Settings..." item in the Multiphysics menu | The last item, after a separator: **Settings…** opens *Multiphysics Settings*. It has no menu role: on macOS Qt otherwise takes an item named "Settings…" out of its menu and into the application menu as Preferences. |
| The GPU switch there too | A *Graphics view* group with **Draw with the GPU (OpenGL)**. It is the same setting as the view's right-click menu (`Multiphysics/gpu`, on unless set off). On OK it is kept and every open Graphics view is drawn so; on Cancel nothing changes. A text says what it does, that the view's own menu has it too, and that `QUCS_GPU=0` turns it off. |
| — | A line under it says what draws now: "The GPU draws the Graphics view.", or why the CPU does, whatever the box says: `QUCS_GPU=0` is set; the build has no OpenGL; or there is no OpenGL 3.3 here (a remote or offscreen display, or no such driver). |

Found on the way, only where there is a GPU: OK first walked `QApplication::allWidgets()` and
switched each view as it went. A view switched off deletes its GPU canvas, which is a widget
of that same list, so the loop then read a deleted object. On the real (Cocoa) platform that
was a crash (SIGSEGV), and under the sanitizers it is reported every time. The views are now
listed first and switched after.

### Tests

- **test_multiphysics_doc `theSettingsHaveTheGpu` (new):**
  - the item's text, role and place; opened from the menu;
  - the box shows the setting; Cancel keeps nothing; OK keeps it off (no view drawn by the GPU after); `apply()` turns it on again;
  - the line says "No OpenGL 3.3 here" offscreen and "QUCS_GPU=0 is set" with it set.
- **test_multiphysics_view `theSettingsDrawEveryViewSo` (new):** two open views, both switched off, then on again (by the GPU where there is one: checked on the Cocoa platform, "the GPU draws here"); the line's text in each case.

Nine changes broken on purpose:

- **7 caught at once, offscreen:** the item's role; the item gone; OK applying nothing; the setting not kept; the box not showing the setting; `QUCS_GPU=0` not said; no OpenGL not said.
- **1 caught on the Cocoa platform with its GPU:** the open views not switched.
- **1 caught only under the sanitizers on the Cocoa platform** (a SEGV in `apply()`): the views switched while being listed. In Release it crashed once and passed once, the freed widget's memory not always reused.

## Where else a GPU would help

Measured first:

- **Meshing.** The VCSEL laser example's mesh, 33,709 triangles, builds in 0.19 s; its solve takes 74 s. The photodiode example's mesh builds in 0.04 s. Meshing is under 0.3 % of the time, so a GPU mesher would gain nothing. Parallel Delaunay meshing on a GPU pays only at millions of points. If a mesh ever dominates, meshing the domains on CPU threads comes first.
- **The solver.** The time is in sparse direct factorizations: Eigen's SparseLU (13 places) and SimplicialLDLT/LLT (5), all single-threaded. Each Newton iteration of the VCSEL's walk, for example, is one factorization, about 0.2 s. GPU direct solvers (NVIDIA cuDSS, cuSOLVER) are CUDA-only. Apple's GPUs have no FP64 in Metal, which these systems need: metals, PMLs, carrier densities over 20 decades. Iterative solvers, which a GPU suits, converge badly on the semiconductor, the waveguide (PMLs, metals) and the eigenvalue problems. The sure win is a multithreaded sparse direct solver on the CPU. The reviewer's measurement, which arrived during this round (`2026-10-16-claude-fem-solver-gpu-vs-sparse-lu.md`): Accelerate's sparse LU factors a 681,054-unknown system in 0.17 s, against Eigen's 28.5 s. That is a review round of its own, next.

The viewers:

| Viewer | Where its time goes | A GPU? |
|---|---|---|
| Multiphysics Graphics view | Done (2026-10-15): 720,000 triangles at 6.8 ms a frame, against 81 on the CPU. | – |
| Layout (GDSII/OASIS) | Every pan or zoom draws every polygon again with QPainter into a cached image (`LayoutView::paintEvent`). | **The best candidate:** per-layer triangle buffers made once, a pan or zoom then two uniforms - as the Graphics view does. Worth it for layouts of millions of polygons; measure on one first. |
| Diagrams (plots) | Already thinned: a trace's segments are joined where they would not show apart at the scale drawn, and kept until the data changes or a zoom of 1.5× (`Graph::drawLines`). | Little left for a GPU. Only for traces of tens of millions of points; measure first. |
| PDF | PDFium rasterizes the pages on the CPU, tile by tile, cached; a tile not cached is made in the paint event. | No: the GPU cannot run PDFium. Making tiles on a worker thread, with a scaled preview meanwhile, is the gain. |
| Markdown, Claude's chat | Text layout and shaping (CPU); images scaled once to device pixels and cached; math typeset once. | No: nothing heavy is left to draw. |
| Images | One image scaled smoothly per frame. | Only for very large images (100 megapixels and more); a mipmap cache on the CPU would do as well. |
| Schematic | Hundreds to thousands of items. | No. |

On macOS OpenGL is deprecated (4.1 at most, kept working). A new GPU view would be better built
on Qt's RHI (QRhiWidget, Metal underneath) than on QOpenGLWidget.

## Not done

- A GPU solver: the reasons above. The CPU direct solver is the next round.
- The layout viewer on the GPU: proposed, not started.
