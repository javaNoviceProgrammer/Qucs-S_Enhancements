# Feature gaps: redrawing large schematics

*26 September 2026 — Qucs-S 26.1.3 (`9615ab2`).*

An RC ladder of 24,004 components (8,000 R, 8,000 C, 8,001 grounds, 202
wires) lagged in the editor. Every repaint took about 170 ms, however much
of the ladder was on show, so a selection rectangle, a drag or a wire
being drawn moved at about six frames a second. `9615ab2` fixed the worst
of it in three ways:

- **Culling:** only the elements that reach the painted area are drawn.
- **Hidden small text:** texts too small to read are left out of the
  canvas, but prints and exports keep them.
- **Held scene for gestures:** a gesture draws the rest of the schematic
  once, then paints only what moves at each step.

It also stopped the drag preview from planning its wire repairs over the
whole schematic at every step. This note records what is still slow or
missing, as a menu for further work.

Timings are from a Release build on an Apple Silicon Mac. They were taken
offscreen at 1× pixels in a 1281×809 canvas, on a copy of that ladder.

| Action | Before | After |
|---|---|---|
| Repaint, a corner at 1:1 | 166 ms | 5.7 ms |
| Repaint, the whole ladder zoomed out | 175 ms | 69 ms |
| A step of a selection rectangle (1:1 / zoomed out) | 173 ms | 4 / 8 ms |
| A step of dragging a resistor (1:1 / zoomed out) | 182 ms | 6 / 13 ms |
| A step with the wire tool, hovering (1:1 / zoomed out) | a full repaint | 5 / 8 ms |
| Clicking to deselect | 182 ms | 10 ms |
| Dropping a dragged resistor | 237 ms | 73 ms |
| Select all | 293 ms | 92 ms |
| Undo of the drag | 512 ms | 352 ms |
| Opening the file | about 450 ms | about 450 ms (unchanged) |

Effort: **S** days, **M** a week or two, **L** more. "Measured" means
timed or profiled. "Read" means found in the source, not timed.

## Still slow

1. **A whole repaint when zoomed out** (M, measured). A paint with the
   whole ladder in view takes 69 ms, so each zoom or scroll step at that
   zoom costs 69 ms. The time goes to about 24,000 symbols of about five
   antialiased strokes each, each stroke with its own pen, fitted to the
   paper by `ink::on()`. Options:
   - At a few pixels per symbol, draw a simplified shape, such as its
     outline or one line between its pins.
   - Batch the strokes of one pen into one `drawLines()` call.
   - Turn antialiasing off below some zoom.
   - Cache a symbol per type, rotation and zoom as a pixmap.
2. **Undo reloads the whole document** (L, measured). An undo takes
   352 ms on the ladder because it rebuilds the schematic from the saved
   text of its undo step (`Schematic::rebuild()`). Undo by commands,
   which reverse what an edit did, would cost in proportion to the edit.
   That touches every edit.
3. **Select all and drop** (S–M, measured, not profiled). Select all
   takes 92 ms and a drop after a drag 73 ms. The drop heals the whole
   schematic (`healAfterMousyMutation()`), as it should. Select all has
   not been looked into.
4. **A scroll step repaints the whole canvas** (S, read). The scroll
   view could blit the canvas and paint only the strip it uncovers: its
   `moveContents()` does that for small moves. But `Schematic::scrollUp()`
   and the others go through `renderModel()`, which ends with
   `viewport()->update()`. At 1:1 this is cheap now (about 6 ms). Zoomed
   out it is gap 1. Scrolling was not timed: the benchmark's wheel events
   did not reach the canvas.
5. **Culling looks at every element** (M, measured). Each paint makes one
   bounds check per component, wire and node. Culling a corner of 16,000
   resistors adds about 1 ms in Release and 17 ms in Debug with ASan (the
   tests allow for this). A spatial index would make a paint cost what is
   on show, not what exists. `conductor_index.h` already finds nodes and
   wires by place, and components would need one kept up to date.

## Not culled or not hidden

6. **Diagrams and paintings are always drawn** (S–M, read).
   `Diagram::boundingRect()` does not bound what a diagram draws, as its
   own comment says (axis texts lie outside it). Paintings were left alone
   too. A diagram with many points, off screen, is still drawn at every
   paint. It needs a true bound for each: its axes' texts, a text
   painting's font, an arrow's head.
7. **The DC bias display lays out every component at every paint** (S,
   read). While the bias is shown, `drawDcBiasPoints()` measures every
   component's text as an obstacle (`layoutBiasLabels()`,
   `Component::textSize()`), and it is not culled. On the ladder that is
   16,000 text measurements per paint. The layout could be kept until the
   next edit or bias run.
8. **Only component texts are hidden when small** (S, read). Wire and
   node labels and text paintings are still drawn when a pixel tall.
   They are usually few; the rule would be the same one
   (`qucs_s::lod::HiddenTexts`). The threshold is fixed at a 4-pixel
   line (`kSmallestLine`), with no setting.

## Gestures

9. **A dragged selection is drawn over everything** (S, cosmetic). While
   it moves, what is dragged is painted on top of the held scene, so it
   covers what it passes (a diagram, say) until the drop. Before, it was
   drawn in the usual order.
10. **A held scene is only as fresh as its key** (S, read). The held
    scene is drawn again when any of these change:
    - the view, size, pixel ratio or paper colour;
    - the element counts;
    - the selection;
    - `setChanged()`, a rebuild, or `reloadGraphs()`.

    A change that bypasses all of these would show only after the gesture
    ends, when the canvas is next painted as a whole. Examples: a
    property value set without `setChanged()`, or a font or grid colour
    changed in the settings during a drag. None is known in the code.
    In a hover mode, such as the wire tool, the gesture lasts until the
    next click.
11. **A held scene keeps a canvas-sized pixmap** (S, read). It is kept
    until the next whole paint of that schematic: up to about 16 MB for a
    full-screen Retina canvas. In a hover mode a tab left in the
    background keeps its pixmap.
12. **Each mouse move still walks the document a few times** (S,
    measured and read).
    - `sceneKey()` goes over every element for the selection's
      fingerprint, about 0.3 ms on the ladder.
    - `MMoveMoving2` builds `currentSelection()`.
    - The status bar relays its chips out at every move (10% of a drag
      step in a profile).
    - During a free move (`MMoveFree2`), `dropStaleElements()` builds a
      set of every element in the document per move. Read, not timed.

    A drag step on the ladder costs 6–13 ms with all of these. They are
    the floor.

## Tests

`qucs/tests/test_canvas_drawing` paints the examples' canvases in 64-pixel
tiles and small squares at three zooms, and compares each with a paint of
the whole. It covers every symbol of the symbol galleries, one example in
fifteen, and one with texts far from their symbols.
`CANVAS_ALL_EXAMPLES=1` checks all 252 examples, which takes minutes.

The same test also checks:

- that texts are left out of the canvas but kept in a print;
- that the property editor is placed over its text;
- that gesture steps match whole paints, including after an edit or a
  selection made from elsewhere.

Its timing checks compare fractions of a whole paint on the same machine,
not absolute times. Under Linux Debug with ASan (CI), the corner check has
about 1.5× room.

`qucs/tests/test_large_schematics` checks the drag preview's healing plan
against the whole schematic's over random drags.
