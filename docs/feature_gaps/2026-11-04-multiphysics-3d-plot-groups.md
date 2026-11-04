# Multiphysics 3D: plot groups, 4 November

Phase C's second part, after heat in time, the sweep and derived values (2026-11-04-multiphysics-3d-time-and-derived):
the proposal's 3D plot group - "Surface", "Slice", "Isosurface", "Clip", "Arrow Volume only when asked", "Mesh" -
shown in the 3D tab's view.

## The kinds

A 3D model's Results take a **3D Plot Group** (`plotgroup3d`). A 2D one takes its 2D Plot Group, as before; each is
offered only where it belongs.

- **The group**: its study, point and time; when not given, the tab's Time and Point boxes choose them. It has a
  title and a **clip**: across x, y or z at a coordinate. What lies beyond is cut away, and the cut face is drawn
  as the group's first coloured plot is.
- **Surface** (the 2D kind, now of both): an expression on the mesh's faces, with its unit, colour table and range
  (automatic, or a minimum and maximum in the unit shown).
- **Slice**: the same on planes. Across x, y or z, either a number of planes evenly across the model (one
  through its middle) or planes at coordinates ("0.5, 1.5", "range(0, 0.5, 2)"). Or a plane through a point and a
  normal.
- **Isosurface**: where an expression has each of its levels. A number of levels evenly between its least and
  greatest, or levels at values in the unit shown. Each level has its colour.
- **Arrow Volume**: a vector field's arrows on a grid of nx × ny × nz, each in the middle of its cell, of
  proportional or normalized length, in one colour. It is offered, never made by default.
- **Mesh** (the 2D kind, now of both): the mesh's faces and edges, each face its domain's colour, or (quality)
  coloured from 0 to 1. A floor's quality is its triangle's 4√3 A/(a² + b² + c²); a wall's is the smaller of its
  height over its width or the reverse.

## How they are drawn

`fem3_results.{h,cpp}`:

- **Isosurfaces** (`isosurfaces()`): each prism is split into three tetrahedra, and the expression's values at the
  mesh's nodes are taken as linear in each (marching tetrahedra). Each wall is split along its diagonal through its
  corner of the least global number, as Dompierre, Labbé, Vallet and Camarero give it ("How to subdivide pyramids,
  prisms and hexahedra into tetrahedra", 1999). The prism beside it splits their shared wall the same way, so the
  surfaces have no cracks between prisms.
- **A slice through any plane** (`slicePlane()`): the same tetrahedra, cut by the plane's signed distance. Each
  corner is read in its prism at its reference coordinates, so a field's value there is exact. Slices across x, y
  or z keep the exact `slice()` of phase C.
- **Arrows** (`arrows3()`): the grid's points found in the mesh, none where no domain defines the field.

**The tab** (`multiphysics3ddoc`):

- **Show**: the box lists each 3D Plot Group after Geometry, Mesh and Solution. A group's **Plot** button in the
  panel (`mp3PlotButton`), or `showPlotGroup()`, shows it.
- **Legend**: the first coloured plot's expression and unit, its range in the unit shown.
- **Problems**: an expression that does not read, or a unit not of its value, is said in `plotProblems()`.
- **A group removed** while shown: the solution is shown instead.
- **The tab's Cut**: it overrides a group's clip where it is set.

**The view's cut.** A face lying on the cut was speckled. Each pixel's interpolated coordinate fell either side of
the plane by its rounding, so a cut face's pixels came and went. The view now keeps a hair beyond the cut (1e-9 of
the model's extent) and draws such a face whole. The Solution view's cut face had the same speckles and is whole
now too.

**Defaults after the first solve.** A model without a 3D Plot Group gets two for each physics, added as one step
of Undo, its solution still fresh:

- **"Heat Transfer in Solids (ht)"**: its variable on the boundaries;
- **"Heat Transfer in Solids, slice (ht)"**: the same on a slice through the middle across x, alone.

The proposal had both in one group, but a surface is opaque: in the chip heater it hid the slice, the
isosurfaces and the arrows, all inside it. So the slice gets its own group, as the defaults of other tools do. Across
x, a slice through a centred device goes through its heater.

## Validated

| Check | What it holds |
|---|---|
| A flat isosurface | T of z alone (order 1, exact): at 304 K the plane z = 0.4 mm - in a layer, its tetrahedra cut across - its every corner within 1e-9 mm, its area the box's 2 mm² within 1e-9 |
| An oblique isosurface | T = 300 K + 1e4 K/m (x + 2y + 3z) at 320 and 340 K: every corner's T its level within 1e-9 K |
| A closed one | The distance from a box's middle at 0.3 mm: every edge of its triangles two triangles' - no cracks between the prisms |
| A slice through any plane | Through the box's middle of normal (1, 2, 3): every corner on its plane within 1e-9 mm, its value the field's there within 1e-9 K |
| Arrows | A uniform field (V = 1e3 V/m x): 16 arrows on 4 × 2 × 2, each (-1e3, 0, 0) V/m, the first in its cell's middle |
| The tab | The chip heater's two default groups, the Show box's five items, the Plot button, a surface's and a slice's triangles (the slice's every corner at the model's middle across x), the legend, an isosurface and arrows added, a clip across z set on the view with its cut face drawn, an expression that does not read said, the group removed |
| The view's cut | Two triangles on the cut plane: their every red pixel drawn, cut or not |

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_solve` `isosurfacesAndSlices` (new) | The isosurfaces, the slice through any plane and the arrows above |
| `test_multiphysics3d_doc` `plotGroupsInTheTab`, `aFaceOnTheCutIsWhole` (new) | The tab, its menus (3D Plot Group offered, not the plane's; Surface, Slice, Isosurface, Arrow Volume and Mesh in a group, not Contour) and the view's cut |
| The full ctest | 161 tests at eight at once: 160 passed. `test_build_commit` compared the build's commit with HEAD, which two RF 2.5D commits (9edbb9d, 47798db) moved while it ran |

## Broken on purpose

`docs/feature_gaps/2026-11-04-multiphysics-3d-plot-groups/breaks.py` holds 17:

- **Isosurfaces and slices:**
  - a prism split by its own corners, not the least global number (the sphere's edges left single: cracks);
  - a quadrilateral's two triangles overlapping;
  - a crossing at its edge's middle;
  - a slice's values read at its prism's first corner;
  - arrows' points in the geometry's unit, not metres;
  - arrows at their cells' corners.
- **The tab:**
  - no default plot groups;
  - the Show box without them;
  - a group's clip not set on the view;
  - its cut face not drawn;
  - the view's cut without its slack;
  - a slice's planes at their cells' lower sides;
  - an expression that does not read not said;
  - a group removed still shown;
  - the Plot button left out.
- **The schema:**
  - a 3D plot group not among the results' children;
  - one offering the plane's contours.

All 17 are caught. One needed a second look: **the view's cut without its slack** passed at first. The test's
plane was at z = 0.5 between bounds 0 and 1, where every sum is exact. Its plane is now at five odd heights between
odd bounds, its share of them computed as a group's clip computes it. Without the slack, 1 to 7 % of its pixels were
lost.

Rebuilt after the breaks. The four lints are green, and nothing was written to the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan, each function alone:

| Function | Time |
|---|---|
| test_fem3_solve `isosurfacesAndSlices` (four small solves) | 1.8 s |
| test_multiphysics3d_doc `plotGroupsInTheTab` (the chip heater solved once) | 23.3 s |
| test_multiphysics3d_doc `aFaceOnTheCutIsWhole` | 0.7 s |
| Whole: test_multiphysics3d_doc, 16 functions | 124.8 s |

No sanitizer said anything.

## What phase C still lacks

- **Exports**: VTU, Field on a Grid in 3D; Interpolation of three arguments.
- **The `fem_*` tools on 3D models.**
- **Estimates and a limit** before a solve.
- **A user's anisotropic tensor, Newton's method, parallel assembly.**
- Cut planes, lines and points as datasets of their own, and graphs of them (1D plot groups in 3D): the bridge's
  first pieces, phase D's.
- Transparency: a surface hides what is inside it. A clip shows the inside, as does a group without a surface.
