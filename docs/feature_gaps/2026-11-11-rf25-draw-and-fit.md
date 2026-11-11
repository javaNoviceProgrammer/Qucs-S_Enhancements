# RF 2.5D: shapes drawn on its tab, and its equivalent circuit fitted together on S

The request: "Build F4 and F6 next", two of the open items of 8 November
([2026-11-08-claude-rf25-open-items.md](../bug_hunts/2026-11-08-claude-rf25-open-items.md)).

- **F4:** draw shapes and vias in the tool. Until now the RF 2.5D view was read-only, so every shape had to come from a
  GDS made elsewhere.
- **F6:** fit the equivalent circuit jointly on S, after the per-branch fit.

## F4: drawing on the tab

The toolbar has a new **Draw** button, between Internal Port and Solve:

- **The button** takes up the last tool used again.
- **Its arrow** opens a menu with:
  - the tools: Rectangle, Polygon, Via;
  - the layer drawn on: any metal or via of the stack;
  - the via's size;
  - Delete the Selected Shape.

The port tools, the pen and Draw are one at a time. Escape puts any of them down.

**The tools:**

- **Rectangle:** drag it.
- **Polygon:** click each corner. A double click, Enter, or a click on the first corner closes it. Backspace takes the
  last corner back. The corners so far, and a line to the pointer, are drawn over the layout.
- **Via:** click where it goes. It is a square of the via size (1 µm until changed, and remembered), on the via chosen,
  else the stack's first via. A metal is refused.
- **Delete:** in Select mode, Delete (or Backspace) deletes the shape selected. Only the cell's own shapes can be
  deleted here, and only those on a layer the map sends to the stack. A shape of a placed cell is deleted in that cell.
  Return, Backspace and Delete are taken from the application's own shortcuts only while one of these is pending.

**Snapping.** A corner snaps as the ruler does: to a shape's corner or edge within a few pixels. Where there is none,
it snaps to the grid shown. The status line follows the pointer with the snapped point and the layer drawn on.

**The layer drawn on** is the one chosen in the Draw menu. Failing that, it is the metal or via chosen in the stack
view, then the topmost metal.

**Where it goes.** Each shape is written to the layout file at once, through the layout editor's own
`editLayout()`, which `edit_layout` uses too. It goes on the file's layer that the layer map sends to that stack
layer. Where the map has none, a row is added: the next free layer number, datatype 0. The file is then read again,
and the view stays where it was drawn on.

**A model that names no layout** begins one at its first shape: `<model>.gds` beside the model, with a cell TOP, a µm
unit and a nm grid (`newLayout()`). The model's Layout then names it. A model never saved is refused, because a
relative name needs a folder.

**Undo.** Each drawing is one step of Undo, holding the model and the file's bytes before and after. Undo writes the
file back, or removes a begun one, and puts back the model with its Layout and its map. A file over 64 MB is not kept;
its drawing says it cannot be undone.

The layout editor learned one more pick for a deletion, `"box_is"`: the one shape whose own box that is. That is how
the shape the view selected is deleted, and not a smaller one inside it.

**`rf25_draw`** does the same for Claude, all or none and one step of Undo. Its shapes are:

- `{"add": "rectangle", "box"}`
- `{"add": "polygon", "points"}`
- `{"add": "path", "points", "width", "ends"}`
- `{"add": "via", "at", "size"}`
- `{"delete": "shape", "at"}` or `{"delete": "shape", "box"}`

Each takes a `"layer"`: a metal or via of the stack, by name or tag. The answer gives:

- what was drawn, and the file;
- the map's rows added;
- whether the model's Layout now names a new file;
- the shapes the processing then leaves, by layer.

## F6: the circuit fitted together on S

Each branch of the pi used to be fitted alone, to its share of the solve's Y: Y11 + Y12, Y22 + Y12 and −Y12. That
weighs every frequency by the branch's own admittance, not by what the ports see. A circuit of the best branches can
then be well short of the best circuit: the review of 2 November found an rms of S of 0.022 where a joint fit reached
0.010.

`fitCircuit()` now goes on after the branch fits, with a Levenberg–Marquardt pass over every value of every branch at
once. It works on the values' logarithms, so they stay positive, and minimizes |S of the circuit − S of the solve|²
over every term and every frequency. The frequencies left out of the branch fits (no Y) count here too.

**Where it starts.** From each branch's own fit, and from the forms that fitted their share nearly as well (within
three times the best's misfit; three forms a branch at most). The combinations are ranked by how well they start, and
the branches' own plus the six best are fitted.

**What is kept.** The fewest values among those within a quarter of the best's rms, or a thousandth of S more, as a
branch's form is chosen. It is kept only where it lowers the rms of S by more than 2 %.

**A symmetric two-port** keeps its two branches to the ground alike, in form and values. Symmetric means S11 and S22
within 1e-3 at every frequency.

**What it says.** A line before the last: "Its values fitted together to the solve's S[, and the forms that fit it
best together chosen]: 0.017 rms, where each branch fitted to its own share gave 0.029." Each branch's own misfit is
said of its values as they now are. `EquivalentCircuit` has `joint`, `formsChanged`, `branchwiseRms` and
`branchwiseWorst`. `rf25_circuit` answers `fitted`, and with a joint fit, the branches' own rms and most.

### What it did

Short lines between two 50 Ω ports, 41 points from 0.5 GHz:

| | Each branch on its own | Fitted together | Forms changed |
|---|---|---|---|
| 40 Ω, 15° at 30 GHz | rms 0.0036, most 0.012 | rms 0.0024, most 0.0070 | no |
| 70 Ω, 30° at 40 GHz, 10 fF pads | rms 0.029, most 0.15 | rms 0.017, most 0.075 | R+C to the ground |
| 30 Ω, 45° at 40 GHz, pads and 20 pH | rms 0.031, most 0.12 | rms 0.019, most 0.061 | no |
| 15 Ω, 0.3 mm, to 60 GHz (MOM-like) | rms 0.033, most 0.15 | rms 0.022, most 0.070 | no |

That is 35–45 % off the rms and about half off the most, in under 0.1 s each.

- A circuit the branches find exactly is found so still: the known pi of the tests, its forms and values to 1e-3, its S
  to 1e-6.
- A capacitor with a Q of thousands stays a C. Its R||C fits a little better but has more values, the same choice the
  branch fit makes.

## Where it is

- `rf25doc_draw.cpp` (new): the tools, the menu, snapping, the polygon's keys and clicks, `drawShapes()`,
  `deleteSelectedShape()`, and the Undo command.
- `rf25doc.*`:
  - the Draw button;
  - the view's picks sent to the tool in hand;
  - the tools one at a time;
  - the view kept after a drawing;
  - the polygon drawn over the overlays (`showDrawing()`);
  - what a drawing did, said till the pointer moves.
- `layoutedit.*`: `newLayout()`, and the `"box_is"` pick.
- `qucscontrol_rf25.cpp`, `qucscontrol.*`: `rf25_draw`, and `rf25_circuit`'s `fitted`.
- `rf25/rf25_circuit.*`: the joint fit.

## Tests

- `test_rf25_doc::aModelsShapesAreDrawnOnItsTab`:
  - a model with no layout drawn on from nothing: the file begun, named and mapped;
  - Undo and Redo of it;
  - a rectangle dragged, its corners snapped;
  - refusals: a substrate, a layer not there, no area, an unknown key, a via on a metal, nothing to delete — the file
    unchanged after each;
  - a polygon: Backspace, Enter, a double click, a click on its first corner;
  - a corner snapped to a shape's corner off the grid;
  - a deletion by box, and Delete of the shape selected;
  - the tools one at a time, and the view kept;
  - a via on the via chosen in the stack, with its own map row, undone;
  - and the strip drawn solving to the same S as the strip read from a GDS made elsewhere, within 1e-6.
- `test_qucs_control::anRf25ModelIsDrawnThroughItsTool`: `rf25_draw` from nothing, with a rectangle and a path; a
  deletion by point; `undo`; refusals.
- `test_qucs_control::anRf25ModelIsSolvedFullWaveThroughItsTool`: `rf25_circuit`'s `fitted`.
- `test_rf25_sparams::theCircuitIsFittedTogetherOnS`:
  - the 70 Ω line: rms down by more than a third, the most by more than a quarter, other forms chosen, the two ground
    branches alike;
  - the rms that of the circuit as it is;
  - the line said;
  - an exact pi found exactly.

[breaks.py](2026-11-11-rf25-draw-and-fit/breaks.py) undoes each piece on purpose, builds and runs its tests:
[results.txt](2026-11-11-rf25-draw-and-fit/results.txt). All 28 are caught.

One was not caught at first: the view fitted again after each drawing. The view was already fitted, and the polygon
drawn lay inside the layout's bounds, so a refit changed nothing. The test now sets a view of its own first, and that
break, run again, is caught.

## Left

- **No path tool on the tab.** A path is drawn by `rf25_draw` alone. Nor can a shape be moved or reshaped by dragging
  it: delete it and draw it again.
- **Drawing writes the cell solved alone.** A shape of a cell it places is deleted in the layout's own tab.
- **The joint fit keeps the pi.** A circuit of more sections, for a structure long beside the wavelength, is not
  fitted; the answer still says how far S is.
