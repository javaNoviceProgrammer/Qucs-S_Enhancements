# RF 2.5D: the ports on the layout, and placed by hand

The user's request of 1 November, made during the recheck round: "add a feature so that the RF ports are shown on
the layout with their names so that it's easy to understand them. Would it be possible for the user to manually
select an edge of a shape and then it will be defined as an edge port or draw a rectangle in a metal and it will be
considered an inner port similar to how EMX™ handles ports?" It was done here, on the user's word ("Do it here"),
between the recheck's investigation and its fixes, which follow it.

## What it is

| Asked | Now |
|---|---|
| The RF ports shown on the layout with their names | **Drawn over the layout, each by its name**, in an orange that stands out on a light canvas and a dark one. An edge port's face is drawn as a bar along the whole edge it drives, with an arrow out of it and its name at the arrow's head. The face is found by the solve's own search (`faceEdge`, now shared with `faceCells`), so what is drawn is what is driven. A coplanar port's grounds are drawn beside it on its edge's line, thinner, as the solve finds them. An internal port is drawn as its rectangle (or a ring at its point) with its name. A port that is on no edge has its name at its point. The **Ports** button hides and shows them, remembered like the other overlays. A port added, moved or taken away is drawn at once. |
| Select an edge of a shape, and it is an edge port | **The Edge Port tool.** With it in hand, a click on (or within ten pixels of) an edge of a metal's processed shape puts an edge port on that edge. It goes at the edge's middle, facing out of it (±x or ±y along an axis, else "outwards"), with that metal as its conductor. The panel then shows its properties, and an undo takes it away. A second port on an edge one already drives is refused ("P1 drives that edge already"), since the solve refuses two on one face, and so is a click near no edge. Each is said in the status line. |
| Draw a rectangle in a metal, and it is an internal port | **The Internal Port tool.** A rectangle dragged over a metal puts an internal port there, from the ground to the topmost metal with a shape under the rectangle. It drives every cell of that metal the rectangle overlaps together, each by its share of the area, as an edge port's face is shared out. A click puts a port that drives the one cell under the point, as internal ports did before. The model's internal port gained **Width** and **Height**; zero keeps the old point port. The solve says which it drove: "P3 drives the 5 cell(s) of M1 under its 50 by 100 µm rectangle at 25, 50 µm against the ground." A rectangle over no cell of its metal is said, not solved. |

Escape, or the tool's button again, puts the tool down. The view's picking is generic. LayoutView gained two modes,
PickPoint (a click is `picked`, a drag still pans) and PickBox (a drag draws a box, `boxPicked`; a click is
`picked`), and overlays gained labels: an arrow of a fixed size and the name on a small card. The view knows nothing
of ports.

Over a line's end, the whole width and no deeper than its last cells, an internal port's rectangle drives what the
edge port there drives, and solves to the same S, to the last digit. A point port in the middle of that end is 0.0013
off in S11 at 20 GHz, and 0.0025 off in S21.

## What reproducing found

- **A new internal port beside edge ports P1 and P2 was named P1.** A node made was numbered among nodes of its own
  kind alone, so internal ports and edge ports counted apart. Two ports of one name are two of one name in a
  Touchstone file too. A node is now named one past the highest of its stem in use by any node: P3, and after P7, P8.

## Tested

| Test | What it holds to |
|---|---|
| `aPickingModeSaysWhatIsPickedAndSelectsNothing` (`test_layout_doc`) | PickPoint: a click picked at its point, nothing selected, a drag pans; PickBox: a drag picked as its box, to 0.05 µm, a click a point; Escape back to Select, the cursor an arrow again; a label's arrow and text in its colour by its point |
| `thePortsAreDrawnByTheirNames` (`test_rf25_doc`) | A coplanar line with a coplanar port, a ground-return port and an internal rectangle: three names at the faces' middles, pointing out of them, and at the rectangle's centre; the faces the signal's whole ends; the coplanar port's two grounds beside it on its line, and none for the other; the rectangle; the Ports button hides and shows them |
| `aPortIsPlacedByHandOnTheLayout` | Through the view, by mouse: a click on a strip's end puts an edge port at its middle, facing +x, on M1, which the panel shows; again on that edge, and near no edge, nothing, each said; undo and redo; drawn at once. A rectangle dragged puts an internal port of its size, within a few µm of the pixels; a click a point one; Escape puts the tool down; the solve drives the rectangle's cells |
| `anInternalPortOverARectangleDrivesItsCells` (`test_rf25_fullwave`) | Over a line's end, the edge ports' S to 1e-9; a point port more than 3e-4 off; said; over nothing of its metal, refused |
| `aNewNodeIsNamedPastTheHighestOfItsStem` (`test_rf25_model`) | An internal port after P1 and P2 is P3; after P7, P8; a metal M2 |

## Broken on purpose

`docs/feature_gaps/2026-11-01-rf25-ports-on-layout/breaks.py` holds sixteen:

- **The view:** an overlay's labels not written; a click in PickPoint selecting rather than picked; a drag in PickBox
  panning; Escape leaving a picking mode as it is.
- **Drawn:** an edge port's face found facing any way; a coplanar port's grounds not found; the ports drawn when
  hidden; a port put not drawn till the next preparing.
- **Placed:** an edge port put facing no way; a second put on an edge one drives; one put however far the click; an
  internal port's rectangle not written; the two tools in hand together.
- **The solve:** a rectangle's port driving the one cell under its point; a rectangle's cells driven alike rather
  than by their share of it.
- **Names:** a node named among its own kind alone.

Fifteen of the sixteen were caught at once. The one that was not let an edge port's face be found facing any way.
The test's ports sat in the middles of their edges, where only one edge is nearest and a port's facing decides
nothing; it decides only at a corner, where two edges meet. The test's ports now stand on the signal's corners, each
facing out of its end, and the break is caught: sixteen of sixteen.

The full ctest (149) and the scenario script (97 checks) were green, both lints too. Two tests of the tab's that
counted its overlays now leave the ports out of the count: the processed shapes' test turns them off, and a
port overlay with nothing in it is not drawn. Under the sanitizers the new functions take 0.3 to 5.9 s, with no
report.

## Not done

- **Moving or resizing a port by dragging it on the layout.** A port is moved through its properties; a new one is
  put with the tools.
- **An internal port between two metals by a rectangle.** The tool puts one from the ground. Its From can be set to
  another metal in the panel, and the solve then drives the rectangle's cells of the one against the other's.
- **A coplanar port by a click.** The Edge Port tool puts a ground-return port. Return set to "beside" in the panel
  makes it coplanar, and its grounds are then drawn.
