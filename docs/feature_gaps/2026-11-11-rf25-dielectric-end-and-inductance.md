# RF 2.5D: a dielectric that ends, and the inductance past the dense limit

The request: "Build F2 and F5 next", two of the open items of 8 November
([2026-11-08-claude-rf25-open-items.md](../bug_hunts/2026-11-08-claude-rf25-open-items.md)).

- **F5:** `rf25_inductance` past the dense limit.
- **F2:** a dielectric with a lateral end. Until now every layer ran on for ever across the plane.

## F5: the loop's inductance, compressed

Up to now `inductanceOf()` refused a loop of more than 3000 runs and cells: its saddle-point system, partial
inductances and Kirchhoff's law, is a dense matrix solved by a full-pivoting LU. Past that limit, or past RF 2.5D >
Settings…' dense limit where it is lower (`loopDenseLimit()`), it is now solved compressed, the way the full-wave
solve is past its own limit.

**L as H-matrices.** There are three kernels, as the full-wave solve holds its L: the runs with a part along x, along
y, and the vias. Each is scaled by the runs' cosines' parts, so that

    L_ab = (x_a x_b + y_a y_b + z_a z_b) K_ab

K is `alignedInductance()` in the plane and the vias' partial inductance between them. Neither kernel is nought between
two of its runs, as L is between runs at right angles. On an axis-aligned mesh every pair is parallel or at right
angles, so L is the dense solve's to the digit. Each low-rank block is held to a part in 1e6 of its size.

**The system by GMRES.** GMRES works on the whole saddle-point system [L Aᵀ; A 0]. It is preconditioned on the right
by the sparse LU of its near field: the near pairs' L, as the full-wave solve picks them, with all of A. L is scaled
first by its runs' mean self term. In henries it is about 1e-12 against A's ones, and a residual of the whole would
then have held L's rows to nothing. Unscaled, on a U of three strips, the near field alone met 1e-10 and L came out
2 % high.

**At direct current** the nodal solve G u = s is now a sparse LDLT for either path; at a compressed loop's size a dense
G is past what fits.

**A via's own cells** no longer make runs between themselves. A via's current runs from one metal to the other; joined
in the plane, a via meshed into more than one cell was an island joined to nothing. That held for the dense solve too,
which called such a loop "in pieces that nothing joins".

| A meander, 8 legs 20 µm wide | Runs | | Time | Memory | Dense would take | L at DC |
|---|---|---|---|---|---|---|
| 10 µm cells | 1328 | dense | 2.6 s | 124 MB | | 1.6554 nH |
| 5 µm cells | 6192 | compressed | 0.2 s | 137 MB | 722 MB | 1.6527 nH |
| 2.5 µm cells | 26528 | compressed | 3.7 s | 0.9 GB | 12.6 GB | 1.6500 nH |
| 1.25 µm cells | 109632 | compressed | 13 s | 4 GB | 211 GB | 1.6486 nH |

GMRES took 21 to 47 iterations to 1e-10. On a U of three strips, a plate whose current turns a corner, and two metals
joined by a via, the compressed and dense answers agree: L, L at direct current and R to a part in 1e6, and each run's
current to 1e-5 A.

## F2: a dielectric that ends

> Tested against the 2D engine the same day, and reworked: the faces' normal field, the panels' grading and
> coarsening, the walls' rows and a line's per metre. See
> [2026-11-11-rf25-dielectric-end-review.md](../bug_hunts/2026-11-11-rf25-dielectric-end-review.md). What follows is
> F2 as it was first committed.

**What the model takes.** A row of the layer map that sends a layer of the file to a dielectric or to the substrate
gives that layer its extent: it is there only inside those shapes. Beyond them its new **Beyond its shapes**
permittivity applies, 1 (air) by default. The processing keeps such shapes apart as the layer's extent
(`Processed::extents`) and does not process them as conductors. The tab outlines each extent in its layer's colour.
The Draw tool (F4) draws on a dielectric or the substrate too, which draws its extent, and so does `rf25_draw`.

**How it is solved** (`rf25/rf25_dielectric.*`), by the volume equivalence:

- **Background and region.** The stack as it is equals a background stack, with the layer as it is beyond its shapes
  everywhere, plus a region, the layer's shapes. The region polarizes as (ε − ε_background)E. Within one permittivity
  that is a bound charge on the region's faces alone, σ = Δε E_inside·n, carried by the background's own layered
  Green's function. The region is taken within the conductors' box, grown by six heights of the stack.
- **Panels.** The region's faces are cut into panels:
  - its top and bottom into squares graded from the conductors and its edges outward, cut to it and kept convex;
  - where a metal lies on a face, or within two of its cells' sides of it, that metal's own cells;
  - its walls into strips along each edge, in rows up its thickness, closer toward the faces where a wall meets them.
  - A face on the ground plane has none: its charge and its image are one.
- **Each panel's charge** comes from the field just inside it, read off the potential on a second-order one-sided
  step:
  - A panel near the point uses its closed form for each of its 40 strongest images (Wilton, Rao, Glisson, Schaubert,
    Al-Bundak and Butler, 1984).
  - A wall's images are vertical rectangles like the wall itself. An image's distance moves with a charge's height at a
    slope of ±1, which the wall's two Gauss heights reveal.
  - A panel far from the point is its charge at its middle, read over a step sixteen times smaller than its distance.
- **What the conductors see** is a correction to their potential coefficients, ΔP = P_cb M⁻¹ T, static and symmetric:
  - **The quasi-static solve** adds it to P, its Green's functions built of the background.
  - **The full-wave solve** adds it to its retarded P, its Green's functions also of the background. It is dense up to
    5000 cells and held as its factors past them.
  - **A uniform line's per metre** cross-section is of the stack as it is. To it is added the end's own difference,
    ΔP(as drawn) − ΔP(drawn everywhere), summed along each strip. That difference fades along the line, as the region's
    whole correction does not.

### Checks

Each check below is against an answer the correction did not make.

**Against the stack's own Green's function** (a plate of 100 µm on 100 µm of 9.8 over a ground):

| The substrate | C |
|---|---|
| everywhere (the stack's Green's function) | 27.14 fF |
| as a region far wider than the box | 27.09 fF (0.2 %) |
| a mesa 200 / 60 / 20 / 5 / 0 µm round the plate | 27.13 / 25.04 / 19.73 / 15.77 / 14.72 fF |
| ending 30 µm beside the plate on one side | 25.79 fF |
| a mesa flush with the plate, εr 4 beyond it | 19.71 fF |
| air (the stack's Green's function) | 4.71 fF |

A 300 µm plate on a mesa cut flush with it reads 94.9 fF. Its parallel plate is 78.1 fF and its own fringing in air
12.4 fF.

Under an oxide between the plate and the substrate's face, so that the metal is not on the region's face, the region
far wider than the box is within 0.3 % at 1 µm of oxide and 1.2 % at 10 µm.

**A wall's potential**, its images taken whole, against the Green's function summed at 3600 points over it: within 5e-5,
at points inside its slab and on its interface.

**The two solvers.** An open stub, 1000 × 50 µm on alumina, its substrate ending 30 µm beside it:

| | Quasi-static | Full wave, at 0.5 GHz |
|---|---|---|
| the whole substrate | 128.30 fF | 128.32 fF |
| ending beside it | 115.13 fF | 115.45 fF |

**Against the 2D multiphysics engine.** The stub's line per metre goes from 122.68 to 110.37 pF/m, a ratio of 0.900. The
2D engine's cross-section of the same edge gives 125.95 and 111.61 pF/m at two mesh sizes, a ratio of 0.886. Its strip
is 0.5 µm thick, in a box 6 mm wide.

### Found and fixed on the way

Each of these broke the answer, and each is now caught by a test or a deliberate break:

- **A panel larger than the cells over it**, its field read at its middle, spread the field under one cell over the
  whole panel. A cell's bound charge came out 1.7 times its own, where the most it can be is 1 − 1/εr. A face now takes
  the metal's own cells as its panels there.
- **A square cut round a conductor's corner** was an L-shape whose middle lies under the conductor. Pieces are now kept
  convex.
- **The step for the field** was a twentieth of the smallest piece; a sliver from the cutting left it some nanometres.
  It is now tied to the grading.
- **`Green::potential(0)` is nought.** A panel straight above another, its middle read at the other's, coupled five
  times as strongly as to itself.
- **On an interface** the direct path and a face's reflection are one term, and the ground's image and its reflection
  another. Counting either again took a wall's potential 4 % low, and a plate on a mesa to 83 fF where the whole
  substrate is 27 fF. Images are now matched by distance.
- **A wall's first reflections whole and the rest as lines** were not enough under a thin layer: the train of images
  sits against the wall's edge. All the strong images are now whole.
- **A metal some nanometres off a face** is taken as on it: within 0.5 % of a cell's side, beside a step of tenths of a
  micrometre.
- **The other way round was tried and dropped.** Taking the layer everywhere, with the region beyond its shapes, read a
  plate on a mesa cut flush with it as fringing less than in air alone, 82 fF. Its panels' own field nearly cancels their
  charge: only a fifth is left at a high-contrast interface.

### What it does not do

- **The polarization is static.** A full-wave solve sees the layer's end in its charge, not in its current: true while
  the region is small beside the wavelength in it.
- **The background slab keeps the layer's loss tangent and conductivity.** The loss is the layer's everywhere in that
  slab, beyond its shapes too.
- **Accuracy where the metal is off the face.** With a metal lifted 10 µm above the region's face, the answers scatter
  by up to 1.5 % with how the panels fall. Each field step reads Green's functions built separately for each height,
  and those are not consistent to better than that over the step. On the face it is within 0.4 %.
- **A region is a layer's whole thickness,** within six heights of the conductors.
- **Not taken up elsewhere:**
  - the calibration strips of a de-embedded port;
  - the HFSS™ export, which writes the layer whole;
  - the loop inductance, which needs no dielectric.
- **A compressed full-wave solve** with a layer that ends fills each entry of P with a row of the correction's factors:
  slower.

## Where it is

- `rf25/rf25_inductance.*`:
  - the compressed loop: its kernels, near field, scale and GMRES;
  - `loopDenseLimit()`;
  - the sparse direct-current solve;
  - a via's own cells.
- `rf25/rf25_fast.*`: `nearPairs()`, moved here from the full-wave solve and shared.
- `rf25/rf25_dielectric.*` (new): the region, the panels, the bound charge and the correction.
- `rf25/rf25_model.*`: a dielectric's and the substrate's Beyond its shapes; `Layer::beyond`.
- `rf25/rf25_layout.*`: `Processed::extents`, `differenceOf()`.
- `rf25/rf25_static.*`, `rf25/rf25_fullwave.*`: the correction in each solve; `ExtractControl::extents`,
  `FullWaveControl::extents`.
- `rf25doc.cpp`, `rf25doc_draw.cpp`: the extents handed to each solve, outlined on the tab, and drawn on.
- `qucscontrol.cpp`: `rf25_draw`'s description.

## Tests

- `test_rf25_inductance::aLoopPastTheDenseLimitIsSolvedCompressed`: compressed against dense on three shapes, and the
  mesh once refused now solved.
- `test_rf25_dielectric` (new):
  - the closed form against quadrature;
  - a wall against its Green's function summed;
  - the region whole, the mesas, an edge, and εr 4 beyond;
  - the plate flush with its mesa;
  - under an oxide;
  - what needs no region.
- `test_rf25_doc::aSubstrateThatEndsIsSolvedByBothSolvers`: drawn on the tab, both solvers alike, the line per metre
  against the 2D engine, undone.
- `test_qucs_control::anRf25ModelIsDrawnThroughItsTool`: drawn by the tool, and solved.

[breaks.py](2026-11-11-rf25-dielectric-end-and-inductance/breaks.py) undoes each piece on purpose, builds and runs its
tests: [results.txt](2026-11-11-rf25-dielectric-end-and-inductance/results.txt). All 20 are caught.

One was not caught at first: the guard on a horizontal distance of nought. At eight rows, no point of the tests was
straight below a far wall panel. The wall test now reads one there, and the break is caught.
