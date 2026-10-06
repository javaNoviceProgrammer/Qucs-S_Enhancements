# Proposal: a GDS/OASIS layout viewer, and later an editor

*6 October 2026 - Qucs-S 26.1.6. A proposal; its phase 1, the viewer, was built the same
day ([Phase 1, as built](#phase-1-as-built) at the end). It writes up the answer of
2 October to "How much work does it take to create a built-in gds/oasis viewer and
editor into qucs? something similar to klayout?", and the look at embedding KLayout of
29 September, parked then. The facts below were checked in this tree, on this Mac and
on the libraries' own pages on 6 October 2026. Sizes and weeks are estimates.*

A layout is what a circuit becomes on a chip or a board: shapes on layers, in cells
that place other cells. GDSII is its old binary format and OASIS its compact
successor. KLayout is the free viewer and editor most people use for both. The
question is how much of that belongs in Qucs-S.

**In short:** a viewer is a modest project and a useful editor a large one.
Matching KLayout is not realistic: it is about two decades of work. Its design-rule
and layout-versus-schematic checks, its scripting and its parameterised cells would
each be a project of their own.

## What there is

**In Qucs-S**, a new kind of document has a path to follow, last taken by the ZIP
archive viewer and editor (`e00910f`, 27 September). It was about 2,100 lines
(`zipdoc.cpp/.h`, `zipfile.cpp/.h`) and 600 lines of tests
(`test_zip_doc.cpp`), in one long session. A layout document would plug in at the
same places:

| What | Where |
|---|---|
| Opened in a tab of its own by its suffix | `QucsApp::gotoPage` in `qucs.cpp` (as `.pdf`, sheets, `.zip`, Markdown), with an `isLayoutFile` beside `isArchiveFile` |
| Its kind, badge and colour in the File Browser | the table of kinds at the top of `filebrowser.cpp` |
| Followed as a link in a reply or a document | `opensItself` in `links.cpp` |
| Its own handling of copy, select all, find and autosave | `qucs_actions.cpp`, `autosave.cpp` (as `ZipDoc`'s) |
| Opened by another program | the File Browser's *Open with the System's Application* already does it |
| Claude's tools | `QucsControl`, with a schema, a summary and a read-only or additive flag each |

The schematic canvas draws itself: `Schematic` is a `Q3ScrollView`, not a
`QGraphicsScene`. A layout view would do the same: Qt's scene framework does not
scale to millions of shapes.

**Outside it:**

| | What it is | Licence | Notes |
|---|---|---|---|
| [gdstk](https://github.com/heitzmann/gdstk) | A C++ library that creates, reads and writes GDSII and OASIS | Boost Software License 1.0 | Builds with CMake. Needs zlib and Qhull. Usable from C++ without Python. Maintained (last push July 2026). |
| [Clipper2](https://github.com/AngusJohnson/Clipper2) | Boolean operations and offsets of polygons, in C++ | Boost Software License 1.0 | Only for the editor (phase 3). |
| [KLayout](https://www.klayout.de) | The viewer and editor | GPL v2 or later (its source headers), as Qucs-S | 0.30.8 is installed here, built on Qt 5.15.18. It builds with qmake, not CMake. Its core libraries for reading, drawing and editing are about 50 MB. The Python and Ruby bindings with their Qt wrappers are 83 MB more. Its GDS2 and OASIS readers are plugins of 1.1 MB. The app is 231 MB. |

GDSII alone is simple enough to read and write ourselves, in a few hundred lines.
OASIS is not: its compressed blocks, repetitions, modal variables and property
tables would be 3,000 to 5,000 lines of our own. That is why gdstk.

## Three ways

1. **Open in KLayout.** A File Browser and Content panel action that starts an
   installed KLayout on the file, found on the PATH or in its usual places. About a
   day. Nothing is drawn inside Qucs-S, and the user needs KLayout.
2. **A viewer of our own, on gdstk** (later an editor). In phases, below. It is
   Qucs-S's own document, so it has the tabs, the panels, undo and Claude's tools.
3. **KLayout's own view inside a Qucs-S tab**, linked against its libraries. It is
   the fastest way to get KLayout's drawing and editing, and the riskiest to keep:
   - its heavy qmake build, from source, against Qt 6, for all six release targets
     (the installed copy is Qt 5, so none of it can be reused);
   - about 100 MB more in each bundle;
   - its releases to follow, and its internal interfaces, which are not a stable
     library interface;
   - a Qucs-S build with it is under the GPL, as Qucs-S already is.

## The phases of way 2

| Phase | What you get | Size | Effort |
|---|---|---|---|
| **1. Viewer** | A `.gds`/`.oas` file in a tab. A cell tree and a layer list (colour, fill, shown or hidden). Pan and zoom, and how deep the cell hierarchy is drawn. A ruler. A click on a shape gives its layer, size and properties. Find a cell. Claude's tools to read the layout. | 4-8k lines | about a week |
| **2. Large layouts** | Smooth on multi-GB files of 10⁸ shapes and more: a spatial index for each cell, cells too small to see drawn as outlines, drawing in background threads with a cache of tiles, shapes stored compactly. | +5-10k | 2-4 weeks, mostly profiling |
| **3. Editor** | Draw boxes, polygons, paths and text. Select, move, copy, rotate and mirror; edit vertices; snap to the grid and to edges. Instances and arrays; new and flattened cells; layer operations and Boolean operations (Clipper2); undo. Saved back to GDS/OASIS with its properties kept. | +8-15k | 3-6 weeks |
| **4. KLayout's extras** | Design-rule and layout-versus-schematic checks, Python and Ruby macros, parameterised cells, net tracing, two layouts compared, LEF/DEF/DXF, technology files | months each | not worth copying |

## Recommended

**Way 1 and phase 1 of way 2.** *Open in KLayout* for heavy editing and design-rule
checks, and a viewer of our own on gdstk for looking at a layout where the circuit
is. Phase 3 only for a workflow that needs it, such as the one below. A
general-purpose IC layout editor is not where Qucs-S adds value.

### Phase 1 in detail

- **The document**, `LayoutDoc`: read-only, as `PdfDoc` is. gdstk reads the file in a
  background thread, with a progress bar and a Cancel, so a big file never blocks the
  window. It is read again when the file changes on disk, as a PDF is (`pdfdoc.cpp`'s
  file watcher).
- **The view**: drawn with `QPainter` over the visible part only. Each shape is drawn
  in its layer's colour and fill pattern, and the instances down to the depth chosen;
  deeper ones are drawn as their frames. Zooming is about the pointer; *Fit* shows all.
- **The panels beside it**: the cell tree (the top cells first, a cell's instance
  count beside it) and the layers, by layer and datatype, with their shape counts.
  Their colours come from a KLayout layer-properties file (`.lyp`, XML) when one lies
  beside the layout, else from a fixed palette.
- **Measuring and finding**: a ruler in the layout's own units (database units shown
  in µm), a shape's details on a click, and a cell found by name in the find bar under
  the pane (`findbar.h`), as a component is in a schematic.
- **Claude's tools**, read-only:
  - `get_layout` `{path}`: the database unit, the top cells, the cell tree, the layers
    with their shape counts, the bounding box.
  - `find_shapes` `{path, cell?, layer?, region? | at?, limit?}`: the shapes there,
    with their kind, layer, points and properties, at most `limit`, with how many
    there are.
  - `show_layout` `{path, cell?, region?, layers?}`: sets the view, which Claude's
    `screenshot` then shows.
- **Tests**: layouts written by gdstk in the test, read back cell for cell and shape
  for shape. One of each kind of OASIS record (compressed blocks, repetitions,
  properties). A layout of a million shapes opened and drawn under a time limit.
  The tools on a small layout.
- **Packaging**: gdstk and Qhull built in statically, from a pinned version, on all six
  release targets; zlib is on each of them already. The bundles grow by a few MB.

### The workflow that would justify phase 3

**A layout from an RF schematic.** Qucs-S's microstrip parts (`MLIN`, `MTEE`, `MCORN`,
`MCROSS`, `MSTEP`, `MCOUPLED`, `MGAP`, `MVIA`, ...) already have their widths, lengths and
substrate. Laid out by their connections, they are polygons on a metal layer. With
the substrate as layers, that is a GDS file an EM simulator (openEMS, Sonnet) or a
board house can read. That is a layout *generated*, not drawn. The editor then only
touches it up.

## Open questions

- **Qhull's licence**: it has a licence of its own, permissive. Its compatibility with
  the GPL bundles is to be checked before it is linked.
- **gdstk's OASIS reading**: its README states no limits. Real files (foundry test
  chips, KLayout's own samples) should be read before relying on it, compressed blocks
  and repetitions most of all.
- **Large files**: phase 1 holds the whole layout in memory, as gdstk reads it. Files
  of several GB need phase 2's storage, or a reader of our own that streams.
- **Where layer names come from**: GDS layers are numbers (layer, datatype). The names
  come from a `.lyp` or a technology file. Which ones we read decides how readable the
  layer list is.

## Not in scope

Design-rule and layout-versus-schematic checks, scripting, parameterised cells, net
tracing, comparing two layouts, LEF/DEF/DXF and technology files: *Open in KLayout*
covers them, with KLayout's own.

## Phase 1, as built

*6 October 2026.* The viewer of way 2, with way 1 in its menu.

- **The library**: gdstk 1.0.1, its C++ library only, in `third_party/gdstk`
  (`README.qucs.md` there), built statically with zlib on every platform.
- **The code**: `qucs/layout.h/.cpp` reads a layout into Qucs-S's own
  structures (cells of shapes, paths, texts and placements; repetitions kept
  as repetitions), draws it and searches it; `qucs/layoutdoc.h/.cpp` is the
  tab; `qucs/qucscontrol_layout.cpp` has Claude's three tools. About 4,200
  lines (the estimate was 4-8k), and 1,100 of tests (`test_layout_doc`).
- **As proposed**: the tab, read in the background with a progress bar and
  a Cancel, read again when its file changes; the cell tree and the layer
  list; colours from a `.lyp` beside the layout, else a palette; pan, zoom
  about the pointer, *Fit*, levels of hierarchy with frames below them; a
  ruler; a shape's details on a click; a cell found by name; `get_layout`,
  `find_shapes` and `show_layout`.
- **Otherwise than proposed**:
  - The find bar is the layout's own, under its toolbar, as a PDF's is: the
    schematic find bar (`findbar.h`) is a schematic's.
  - *Open in KLayout* (way 1) is in the tab's ⋯ menu, when KLayout is
    installed.
  - Copy, a zoom box dragged with the right button, Shift for a ruler
    across or up only, and printing what is in sight were added.

**The open questions, answered:**

- **Qhull's licence**: Qhull is not linked. gdstk used it for one function,
  a convex hull the viewer never calls; it is a monotone chain of our own
  instead.
- **gdstk's OASIS reading**: a file KLayout 0.30.8 wrote - compressed blocks,
  strict mode, its own repetitions, a property, a layer's name - is read as
  KLayout counts it, layer for layer; it is a test's fixture
  (`qucs/tests/data/klayout_sample.oas`). Damaged files were another matter:
  fuzzing gdstk's readers found 15 ways a cut or changed file crashed them
  (a null pointer written through, table numbers kept in pointers and freed,
  counts allocated as given, records read as types they were not). Each is
  fixed in our copy, and the test reads thousands of damaged files each run,
  under the sanitizers in CI.
- **Large files**: a million shapes read in 0.2 s and are drawn whole in
  31 ms (a corner in 2 ms) on an M-series Mac. Everything is held in
  memory, as said: files of several GB still need phase 2.
- **Layer names**: from the file (OASIS LAYERNAME records) and from a
  KLayout `.lyp`; a technology file (`.lyt`) is not read.

