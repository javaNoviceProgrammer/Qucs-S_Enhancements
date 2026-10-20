# A process flow for the multiphysics FEM

The proposal: `~/Desktop/qucs-reviews/2026-10-15-claude-process-flow-proposal.md` - a Process Flow in the model, an ordered list of steps on a 2D cross-section whose result is the geometry, materials and doping the physics read. The user asked for its Stages 1 and 2 whole (Stage 3 is a set of projects of its own, left out as the proposal itself says).

This write-up grows with the work: Stage 1 first, then Stage 2's parts.

## Stage 1: shape steps and masks

### What it is

A geometry feature, **Process Flow** (`"process"`), holds its steps in order; when the build reaches it, it runs them and each region it leaves becomes an object of the geometry, named by the step that made it (its label, else its tag; an epitaxial stack's by its rows). The materials and the physics select them by those names, as they do a layout cross-section's rows.

- **Its window:** x from `x0` across `width`, or along a layout's cut (`file`, `cell`, `start`, `end`, `origin`), as the Layout Cross-Section takes it; the surface at y = 0 to start.
- **The steps** (`process/...`):

| Step | What it does |
|---|---|
| Substrate | A crystal across the window, `thickness` below y = 0; its `orientation` (100, 110, 111) is what a crystal-plane etch follows |
| Epitaxial Stack | Its `layers` [name, material, thickness] from the bottom, the top's surface at y = 0 |
| Deposit | `conformal` (its thickness from every surface: the solids offset, round), `planar` (filled to its thickness above the highest point) or `directional` (its thickness straight up on what the sky sees: nothing under an overhang); masked, a lift-off |
| Etch | A front from the surface into each material at its rate - 1 for those it etches (`materials`; empty: all but its stop and mask), 1/S for a `selectivity` S, `stopratio` for its stop, `maskratio` for the resist - for a `depth`, or until its `stop` shows in every opening, then its `overetch`. `anisotropic` (straight down), `isotropic` (round, under the mask's edges), `tapered` (`angle` from the surface: under 90° narrowing, over 90° a dovetail), `crystal` ({111} sidewalls: 54.74° on (100), upright on (110), a V where they meet; another plane's angle given) |
| Planarize (CMP) | Everything above a `height`, or above its stop's (`to`) highest point, cut away |
| Strip | A material removed wherever the air touches it; a buried piece stays (said) |
| Epitaxy | A crystal grown straight up on the exposed crystal - `selective` (on crystal alone: a substrate's, a stack's, epitaxy's) or not |
| Fill | A polymer or glass filling every open space to a `height` (or its `thickness` above the highest point) |

- **Masks:** a step's `mask` is a layout's `layer` along the cut (open where it is `drawn`, or where it is not: `polarity`) or typed `openings` ("1, 2; 4, 5"), each opening grown by its `bias`. A layer is "1/0", or a name the layout's `.lyp` gives (beside it, as the layout viewer finds it: one map for the viewer, the cross-sections and the masks). For an etch, a mask is a resist of its `resist` thickness over the surface where it is closed, for that step alone.
- **Robustness:** the regions are Clipper polygons on a 64th of the **process grid** (1 nm by default); after each step they are snapped to the grid, cleaned, and rid of slivers - a piece thinner than the **minimum** (1 nm) everywhere, or one the grid takes away - each dropped piece said in the step's warnings. An etch's front keeps only what it touches: a step of the front longer than a stop layer or a resist is thick does not go through it (a 2 nm buried oxide under a 500 nm etch is kept).

### The state after each step

- **In the window:** choosing a step in the Multiphysics panel shows the structure as that step leaves it ("Show After This Step"); any other node shows the whole geometry again. A step that makes a region has its domains highlighted.
- **Claude's tools:** `fem_build` with `until` a step's tag (or any geometry feature's) builds to it, shows it, and answers each step's report (what it did, its time, its warnings) and the regions then (name, material, area, where; crystalline marked). `fem_describe what=format` describes the flow and every step.

### With the layout viewer

- **A flow from a ruler:** the layout tab's menu, with a ruler drawn, has "Process Flow to Multiphysics" beside the cross-section: a new model of a flow along the ruler, a silicon substrate and its material to begin with.
- **The cut follows the ruler:** the flow's "Cut Along the Layout's Ruler" takes its layout tab's last ruler (or an open layout's, when it has none): its file, cell, start and end.
- **A step's mask over the layout:** "Show Its Mask in the Layout" shows that layer alone in the layout, the cut on it.
- **Cross-probing:** with the flow or a step chosen, a click in the cross-section shows that point in the layout - the cut drawn, the point marked across it, the shape there selected. A shape picked in the layout says, in its description, the open models' steps it is the mask of ("the mask of rib (heater_flow.qfem)").
- **Its materials:** "Make Its Materials" makes a material for each material its regions are of, from the library, on their regions by name - or adds them to a material of that library entry already there.

### How it was checked

`test_fem_process` (new):

| Check | Result |
|---|---|
| A 1 µm trench etched 0.5 µm through a mask, then 0.1 µm conformal oxide | the silicon's area 7.5 µm² to 1e-6, its corners on the mask's edges; the film 0.3 + π·0.01/2 + 0.1 + 0.08 µm² (flat tops, a quarter disc at each convex corner, the walls, the floor between them) to 1e-3 |
| The same, planar 0.2 µm | the trench filled, its top flat at 0.2 µm, 1.3 µm² |
| SOI's top silicon etched to its buried oxide, over-etched 20 % | the oxide untouched; at a twentieth of the rate, over-etched by half, it loses 2 × 0.3 × 0.5 × 0.05 µm² |
| A 2 nm buried oxide under a 0.7 µm etch, its steps 4 nm | not gone through: the handle untouched |
| An isotropic etch, 0.5 µm, through a 0.2 µm opening | a stadium, 0.2 × 0.5 + π·0.25/2 µm² to 3e-3; 0.5 µm under the mask at either side |
| A crystal-plane etch of (100) silicon through a 2 µm opening | a V of √2 µm² - its walls at 54.74° (to 0.1°, the proposal's target) |
| A tapered etch at 80° | the trapezoid's area to 3e-3 |
| A directional deposit over an undercut hard mask; one lifted off | metal on the cavity's floor under the opening, none under the overhang; lifted off: in the opening alone |
| Selective epitaxy in an oxide window, planarized to the oxide | grown in the window alone, crystalline; cut to the oxide's top |
| A strip of buried resist; of resist opened to the air | kept, said; stripped |
| A mask from a layout's layer along the cut (written with gdstk) | open where drawn, where not, biased by 0.1 µm: the areas exact |
| A film thinner than the grid; a flow without a substrate; an unknown stop; an etch of nothing | dropped and said; refused, each said |
| The regions as the geometry | objects named by their steps, three domains, a selection by name, built up to a step, the file read back as it was |

`test_multiphysics_doc`: `aLayoutsProcessFlow` - a flow from the ruler of the thermo-optic heater's layout, an SOI process whose rib etch's mask is the waveguide's layer by its `.lyp` name; the waveguide left where the layer is; the materials made (oxide new, the substrate's silicon extended to the core); the mask shown alone in the layout with the cut; a shape picked there naming the rib; the cut along another ruler; the state after the etch.
