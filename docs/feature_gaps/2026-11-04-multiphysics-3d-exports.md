# Multiphysics 3D: VTU, Field on a Grid in 3D, functions of three arguments, 4 November

Phase C's third part, after its time and derived values (2026-11-04-multiphysics-3d-time-and-derived) and its plot
groups (2026-11-04-multiphysics-3d-plot-groups). The proposal's exports: "VTU: the VTK XML unstructured grid, with a
.pvd collection for times and sweeps. Tetrahedra and prisms; P2 as quadratic cells. Written by our own code (no VTK
library)" and "Field on a Grid: 3D grids (nx, ny, nz) as CSV and NPY"; and Interpolation of three arguments.

## VTU (ParaView)

A 3D model's Export takes **VTU (ParaView)** (`vtu`, its own; `fem3_export.{h,cpp}`):

- **Solutions**: one (its point and time) or each, as the other exports choose.
- **Expressions**, each named if wanted ("T = ht.T").
- **Cells**: wedges, or quadratic wedges. Automatic gives quadratic wedges where a field is of order 2.

What it writes (VTK's XML unstructured grid, ASCII):

- **The points**: the mesh's nodes in the geometry's unit. A quadratic wedge adds each edge's middle once, by its
  two nodes, mapped by the field's space (curved where its triangle is).
- **The cells**: VTK_WEDGE (13), or VTK_QUADRATIC_WEDGE (26): its six corners, then the middles of edges (0 1)
  (1 2) (2 0), (3 4) (4 5) (5 3) and (0 3) (1 4) (2 5), as VTK numbers them. Each wedge's floor is turned so its
  right-hand normal points away from its top, as VTK takes it. A plan triangle counter-clockwise from above is
  written (0, 2, 1).
- **Point data**: each expression in SI, at the corners as the tab colours them (each prism's value there,
  averaged), at the middles read in their prism. **Cell data**: `dom`, each prism's domain from 1.

Of several output times or a sweep's points, it writes a **PVD collection** with a VTU beside it for each
(model_tag_0000.vtu ...). Each VTU's timestep is its time, and a sweep's points are each a part of their own (at
their number where they have no time).

No VTK library or ParaView was on this machine, and installing one would have been a download. So each file is
held by its own structure: read back as XML, and each cell's orientation, point and value checked.

## Field on a Grid in 3D

Field on a Grid now takes 3D models: x and y from its two corners, z from **From z to z** (empty: the model's
extent) and **Points along z**, its frequency and mode the plane's alone.

- **CSV**: a row a point, x, y and z in the geometry's unit, each value SI, nan outside the domains.
- **NumPy**: shape ([solution,] expression, z, y, x), the points' x, y and z beside it as `_xy.npy`, shape (3, z,
  y, x). At the mesh's nodes: rows of x, y, z and each value.
- **Columns**: # comments, a blank line after each row of x and two after each plane of z (gnuplot's).

## The tab

An export's **Export** button (`mp3ExportButton`), or `runExport()`, writes it where its File says (relative to
the model's folder). With no File it goes beside the model as model_tag, or in the cache while the model has no
file. A grid's coordinates and a collection's parts go beside it, and what was written is said. Before a solve it is
refused, said.

## Functions of three arguments

The Interpolation function (Global Definitions) takes **Three: f(x, y, z) on a grid**: a table of x, y, z and f,
every x with every y and every z, its arguments' units split by commas.

- **Trilinear** between the grid's points, constant beyond them.
- **The expression language**: a third axis in its table and an op of three arguments (`Table3`).
- **Refused**: a table that is not a whole grid, with its counts said.
- **Both kinds of model** can use it, as the other functions are.

## Validated

| Check | What it holds |
|---|---|
| A VTU of a linear field | Order 1, T = 300 K + 1e4 K/m (x + 2y + 3z) held on two boxes: as many points as the mesh's nodes, T at each the field's there within 1e-8 K, six points a cell of type 13, every floor's normal away from its top, a domain each |
| Quadratic wedges | Order 2, the quadratic patch's T: 15 points a cell of type 26, each edge's middle its corners' middle within 1e-12 mm, T there the quadratic's within 1e-7 K |
| A collection | An insulated block in time at 0, 0.5 and 1 s: a PVD of three VTUs named by its base (timestep 0.5, file block_vtu1_0002.vtu), the last 294.15 K - its 1 K/s for 1 s |
| Field on a Grid in 3D | 7 × 5 × 3 points from (-0.5, -0.5) to (2.5, 1.5) mm, z from 0.25 to 750 µm: its z those given (0.25, 0.5, 0.75), the linear field exact inside, nan beyond; NumPy shape (1, 3, 5, 7), its _xy.npy (3, 3, 5, 7), 105 doubles; the columns' two blank lines after each of its 3 planes |
| Three arguments | x + 2y + 3z on a 2 × 3 × 2 grid: 2.85 at (0.5, 0.8, 0.25) mm to 1e-12, constant beyond, two arguments refused; a heat source of it over a 2 × 1 × 1 mm box, ht.P 7 W to 1e-9 (V (x̄ + 2ȳ + 3z̄)); a table with a point missing refused, said |
| The tab | A 3D model's Export offers VTU and Field on a Grid, not the plane's; the Export button writes model_tag.vtu beside the model, said; a NumPy grid its _xy.npy too; before a solve refused |

## Tests

| Test | What it holds |
|---|---|
| `test_fem3_solve` `exports`, `interpolationOfThree` (new) | The checks above |
| `test_multiphysics3d_doc` `exportsInTheTab` (new) | The tab's |
| The full ctest | 161 tests at eight at once: 158 passed. `test_markdown_doc`'s `longRunsRenderInLinearTime` and `test_qucs_control`'s `aLargeSchematicIsArrangedInTime`, time ratios, were over their bounds under the load and passed alone. `test_rf25_fullwave_2`'s `whatItCannotSolveIsSaid` is of the RF 2.5D dense LU's limit, which that session was changing in the checkout as it ran |

## Broken on purpose

`docs/feature_gaps/2026-11-04-multiphysics-3d-exports/breaks.py` holds 14:

- **VTU:**
  - a wedge's floor left counter-clockwise;
  - a quadratic wedge's middles at their prism's first corner;
  - their values read there;
  - their points in metres;
  - a collection's parts named without its base;
  - its times each part's number.
- **Field on a Grid:**
  - a grid's z range not read;
  - the columns without their blank lines after each plane of z;
  - a NumPy grid's shape without z.
- **Three arguments:**
  - the trilinear table's y and z swapped;
  - its third axis its cell's floor alone;
  - a table not a whole grid taken.
- **The tab:**
  - the Export button left out;
  - a 3D model's Export not offering VTU.

All 14 are caught at their first run. Rebuilt after the breaks. The four lints are green, and nothing was written
to the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan, each function alone: `exports` 2 s, `interpolationOfThree` 0.5 s,
`exportsInTheTab` 21 s (the chip heater solved once). No sanitizer said anything.

## What phase C still lacks

- **The `fem_*` tools on 3D models.**
- **Estimates and a limit** before a solve.
- **A user's anisotropic tensor, Newton's method, parallel assembly.**
- VTU's binary (appended) form: the ASCII form is some three times the size.
