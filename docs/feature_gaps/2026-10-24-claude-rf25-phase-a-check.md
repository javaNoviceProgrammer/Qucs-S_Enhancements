# RF 2.5D: the phase A check of 24 October

The reviewer's live check of phase A (`~/Desktop/qucs-reviews/RF2p5D/2026-10-24-claude-rf25-phase-a-check.md`)
was made on the build of 24 Oct 12:00 - `8f9ec51` and `4de84d4` - before most of what landed that day. So
every item was reproduced again on `fba3a28` before anything was changed. Some were gone already; one was
worse than written; and mending it turned up two more at the stack's ends.

The reproductions drove the build tree's app through `qucs-s --mcp-server`, isolated as
`scripts/mcp-e2e-scenarios.py` isolates its own (`QUCS_SETTINGS_DIR`, `QUCS_NO_SHELL_ENV`, `HOME`,
`QUCS_TRASH_DIR`, `QUCS_CACHE_DIR`, `--workspace`), on copies of the reviewer's models.

## Bugs

| Feedback | Now |
|---|---|
| **B1** The ground plane is never under the stack: `stackOf()` gives a ground `z0 = z1 = z` at the running height, so every placement fails - GND first is "the substrate is not the first layer", after the cover "the cover is not the last", before the cover "whole" but at z = 305.1 µm | **Fixed, and it was worse.** The solver never read the ground's z - `layersOf()` went by its *place*: a ground before every slab was the plane below, one after any slab was refused ("a ground plane above the stack is not solved yet"). So the only order the solver accepted was the one the stack's own checks refused, and no model with a ground was both whole and solvable; `gnd_mid` said "nothing is missing" and was then refused by the solve. Now a ground is a plane at nought under everything wherever the tree has it: `stackOf()` puts it there, the order checks look past it (the substrate is the first of the *placed* layers, the cover the last), and `layersOf()` takes any ground as the plane below. `gnd_first`, `gnd_mid` and `gnd_last` are all whole, all solve, and give **the same 32 capacitances to the last digit**. `add()` puts a ground first and a substrate after it, and the Ground and Stack help say the same thing as each other and as the code. |
| ↳ found mending B1 | **A ground set to "Open (no plane)" was solved as a perfect plane.** The node has a kind - perfect, surface impedance, open - and nothing read it: `Layer` had no field for it. It does now (`Layer::variant`), and an open ground is no plane: a strip over it holds less than 0.8 of what it holds over a plane, which is the test. |
| ↳ found mending B1 | **A cover set to "A metal lid" was solved as open air**, the same way. A lid is not solved yet, so it is now *said* - "a metal lid over the stack is not solved yet" - rather than read as the air it is not. An open cover is still the air above. |
| **B2** A new model's stack view is empty ("No layer" beside a status line counting three) | **Fixed.** The constructor draws the starter model's stack; only an edit or a read did. |
| **B3** `get_state` and `open_document` say "schematic" | **Already fixed** by `64fa7c0`: both say "RF 2.5D model". |
| **B3** `save_document as=...qrf25` refuses | **Already fixed** by `64fa7c0`, for a model opened from a file and an untitled one alike. |
| **B3** `screenshot` with `region` refuses ("'diagram' and 'region' are of a schematic") | **Points the way now.** `region` is a schematic's coordinates; for an RF 2.5D model - and a layout - the refusal now says to frame it with `show_layout`'s `region` (in µm) and take `screenshot` without one. |
| **B3** `show_layout path=...qrf25` refuses ("could not be opened as a layout"), so the canvas can only be zoomed in steps about its middle | **Fixed.** `show_layout` takes an RF 2.5D model (named by its file, or the one in front) and frames its own view: `region`, `texts`, `grid`, and `processed` and `mesh` to draw the processed shapes and the cells over it - so an agent can frame a corner of a layout and look at its mesh. Its cell and layers are its Layout node's and its layer map's, so `cell`, `layers` and `depth` are refused for it, saying so. |

## Smaller issues

| Feedback | Now |
|---|---|
| **N1** A bad expression is reported twice: "M2: thickness is not a number (...)" and "M2 has no thickness" | **Fixed.** The reader remembers which values would not read, and the range checks pass over them - thickness, permittivity, loss tangent, conductivity, roughness - so one mistake is one line and "1 thing missing", not two. A value that reads but is wrong (a thickness of nought) is still said. |
| **N2** The tree's two columns have no names (`get_ui` gives "1" and "2") | **Fixed.** "Node" and "Status" - the code's own name for the second. The header stays out of sight, as it was designed to. |
| **N3** The Save dialog for an untitled model starts in `~` | **Fixed, and not only for RF 2.5D.** With no project open and nothing opened or saved yet, *every* untitled document's Save started in the home folder, while the open dialogs already started in the workspace. It starts in the workspace now; the last folder used still wins, and an open project still does. |
| **N4** An unknown node key is dropped silently: `"props"` for `"properties"` loads every value at its default, with no warning | **Fixed.** What a file has that is not read is *said* - not read, since guessing what was meant is worse than saying - as the read's notes, which the tab shows under "Read with notes": `M1: "props" is not read - a node's values are under "properties", so they were left at their defaults`. A key no node has and a property no kind has (`"thicknes"`) are said too, the latter only of kinds this build knows. More than eight are said as eight "and N more". Checked for false notes against every real model to hand - the 26 of `rf25_check/`, both rounds', and the smoke tests' - and none of them reads with a note. |
| **N5** The node menu (add a layer, a ground, a port) could not be opened by a right-click through `send_input` on the dock | **Fixed, in the tool that is for it.** As suspected, a gap in the tool: `send_input`'s right-click sends plain mouse events, which never raise a widget's own menu, and `context_menu` - the tool that opens, lists and chooses from a menu - had no way to name a node of the tree. It has now: `{"node": "m1"}`, by tag or name, on the model named or the one in front. It matters more than it sounds, since no tool adds a node to an RF 2.5D model and the menu is where it is done: `context_menu` on "Stack" lists *Add > Dielectric, Metal, Via, Ground, Cover*, and choosing *Add > Ground* puts a ground at the bottom of the stack. |
| **N6** Possibly stale: right after a disk reload, `get_ui` once still listed a via's problem after it had been mended; a screenshot moments later did not | **Reproduced every time, and fixed.** Not stale data, a race: a tool call reads a file changed on disk *before* it answers, and the panel made its tree again at once but its form only on the next turn of the event loop - so the call was answered with the form of the file before. The first `get_ui` after a mend always listed what had been mended; the second never did, which is the "once" and the screenshot "moments later". A read comes from no field of the form, so the panel now makes the form again at once for a read; an edit is still left to the next turn, since it comes from one of the form's own fields, which must not be destroyed inside its own signal. |

## Tested

| Test | What it holds to |
|---|---|
| `aGroundIsUnderTheStackWhereverItIsPut` | A ground at five places, with a cover and without: the stack whole every time, the ground at nought, and **no other layer moved** for it; the order checks still seeing a substrate above an oxide and a cover below a metal, past the ground; and a ground's and a cover's own kind read for the solver |
| `aLayerAddedGoesWhereItBelongs` | A ground added after a cover going first, a dielectric going before the cover, a substrate going after a ground; and the Ground help no longer saying "last in the tree" |
| `aBadValueIsSaidOnce` | A bad thickness said once and not as "no thickness"; five bad values of three layers said once each; a thickness of nought still said; and the model's own list counting it once |
| `whatAFileHasThatIsNotReadIsSaid` | A blank model reading with nothing to say; `"props"`, a key no node has and a property no kind has each said; the values of `"props"` **not** read; twenty unknown keys said as eight "and 12 more" |
| `theGroundIsReadForWhatItIsNotWhereItIs` | A ground at five places always the plane below and the stack whole; open no plane, impedance and perfect a plane; a metal lid refused and an open cover not; **the same capacitance to the last bit** with the ground first or last; and a strip over an open ground holding less than 0.8 of what it holds over a plane |
| `aNewModelsStackIsDrawnAtOnce` | The stack view of a new model holding the starter's layers, name for name, before any edit |
| `theTreesColumnsAreNamed` | "Node" and "Status", and the header still hidden |
| `anUntitledModelIsSavedIntoTheWorkspace` | With no project and no folder used yet, the Save dialog opening in the workspace and not the home folder |
| `aFileReadAgainIsShownInTheFormAtOnce` | A model with a bad thickness, its form listing it; the file mended and read again, and the form **looked at before the event loop has turned** no longer listing it; an edit still marked as no read |
| `anRf25ModelIsFramedAndItsTreeMenuReached` | `show_layout` on a model: a region in µm held, the mesh drawn and then not, the one in front taken without a path, `cell`, `layers` and `depth` refused saying why; `screenshot`'s region pointing at `show_layout` and the tab itself taken; `context_menu` on the Stack by name and M1 by tag, *Add > Ground* chosen and the ground first in the stack, and a node that is not there and a schematic refused |

## Broken on purpose

`docs/feature_gaps/2026-10-24-claude-rf25-phase-a-check/breaks.py` holds thirty, one or more for each fix: the
ground at the running height, the order checks counting it, the solver reading it by place, an open ground as
a plane, a lid as air, the ground's kind unread, a ground added last and a substrate under it, the old help;
the starter stack undrawn; `show_layout` refusing a model, ignoring its region and its mesh, taking its cell;
`screenshot`'s old refusal; every check made of a value that did not read, and the reader forgetting; the
columns unnamed; Save in the home folder; the notes of a node's keys, of its properties, of its children, and
of the read itself dropped, and their cap; `context_menu`'s node opening nothing, the panel opening no menu,
a node found by tag alone; and the panel deferring a read, and the document not saying it was one.

## What the run taught

- **One break went uncaught at first**: `show_layout` ignoring a model's `region`. The test asked only that what
  was shown *held* the region asked for - and a view fitted to the whole 1200 µm wire holds a 200 µm window
  too. It now asks that the region shown is about the size and the place asked for.
- **The full ctest found an old test that held the ground in its old place.** `theTreeIsEditedInStepsOfUndo`
  asserted that a metal added after the ground still came before it - the ground at the end of the list, the
  very order the Ground's help and the Stack's were at odds over. It holds the ground first now, under the
  substrate, as the tree is in z from the bottom up. Run alone, the round's own tests had all passed; it took
  the whole suite to see it, which is why the whole suite is run.
- The full ctest, the scenario script (97 checks) and both lints were otherwise green.

The reviewer's phase B check of the same evening (`2026-10-24-claude-rf25-phase-b-check.md`) lists two of
these again - its F5 (one place for the ground) and F7 (a new model's empty stack view) - and both are done
here; the rest of it is a round of its own.

## Not done

- **A blank model has no ground.** "New Model" gives a substrate, an oxide and a metal, so its first solve is
  of a strip on a slab in free space - a capacitance to infinity, and a "Z0" with nothing to return through.
  That is a correct answer to the model as it stands, and outside this check; whether the starter model should
  carry a ground plane is a question for the phase B check, which has its own file.
- **A metal lid and a surface-impedance ground are not solved.** The lid is refused, the impedance taken as a
  perfect plane for the charge - as the RLGC write-up already says of the ground's own loss.
- **`context_menu` reaches the RF 2.5D tree, not the multiphysics one**, whose nodes `fem_edit` adds. The
  multiphysics panel's tree has the same unnamed columns and the same deferred form; neither was in this
  check, and neither is changed.
- **"For phase B"** - `rf25_*` tools to describe, make and edit a model, and an example model with its layout
  as the first S-parameter benchmark - is the reviewer's guidance rather than a fault, and is for the phase B
  check, which has its own file and covers it.
