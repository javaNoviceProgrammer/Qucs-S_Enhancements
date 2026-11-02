# Pens on the layout and slide viewers

The user's request of 2 November: "Can you add pen markers to the layout viewer and pptx viewers? This is to allow
me select a pen and its color and then do drawings on an open gds layout or an open powerpoint pptx?"

## What it does

Both viewers' toolbars have a **pen**, beside the ruler on a layout and beside the notes button on a presentation.
Pressed, a drag draws with the pen in hand. The arrow beside the button opens its menu:

- **Tool:** the pen, the highlighter (a broad stroke that what is under it shows through) or the eraser (which takes
  whole strokes away).
- **Colour:** red, orange, yellow, green, blue, purple, black, white, or Other Colour… for any.
- **Breadth:** fine, medium or bold (a pen's 2, 4 and 8 pixels; a highlighter's 10, 16 and 24).
- **Drawings:** undo the last change, clear the drawings on this cell or slide, clear all of them.

Picking from the menu takes the pen up. Escape puts it down, and Ctrl+Z undoes. The button's icon is the pen in
hand, in its colour. The pen in hand is every viewer's: picked in one tab, it is picked in all, and kept for the
next time (`Viewers/penTool`, `penColor`, `penWidth`, `highlighterWidth`).

A stroke is held in the measures of what it is drawn on, so it moves with what is under it:
- **On a layout:** µm, on the cell it was drawn on. It pans and zooms with the layout, shows with that cell and not
  with another, and is drawn as broad as the pen was at the zoom it was drawn at, like ink on paper (a pixel at the
  least, far out). The middle button pans, as it does in every mode.
- **On a slide:** points from the slide's top left, on that slide. It zooms and scrolls with it, goes on past the
  slide's edge as the pointer does, and is drawn no further than the edge. A double click with the pen up is two
  presses of it, not the slide show.

Drawings are kept when the file is read again, and are part of what each tab gives out. A layout prints with
them. A slide prints, copies and exports as a picture with them. They are not written to the file: they belong to
the tab, as long as it is open.

## How it is built

`qucs/markup.{h,cpp}` holds everything that is the same in both:
- the pen in hand (`Pen`, `currentPen()`, `setCurrentPen()`, and `Pens`, which tells every button it changed);
- a stroke (`Stroke`), and a document's strokes with undo (`Drawing`), each change undone in turn, an eraser's
  drag as one;
- `paint()`, for the screen, a print and a picture alike;
- `PenButton`, the toolbar's button and its menu, which is made once and kept as the pen and the drawing are,
  because Claude's `get_ui` reads a button's menu without opening it.

The name is `markup` because `ink.h` was already taken, by the colours a schematic is drawn in on dark paper.

`LayoutView` gains a mode, `Draw`, beside its ruler and picking modes, and paints the strokes of the cell shown in
`paintFrame`, which the CPU's drawing and the GPU's canvas share. `SlideView` takes the pen up and down itself, and
paints each slide's strokes clipped to the slide.

Escape and Ctrl+Z are the window's keys too:
- **Escape** ends a schematic's mode. While the pen is up, the slide view takes it on `ShortcutOverride`, as the
  layout view already did. Without that the window takes it, as a break showed.
- **Ctrl+Z** reaches both views as it is, because the window's Undo is off while a viewer is in front.

The tests press both keys, so they go through the window's shortcuts as yours do.

`Presentation::placed()` says where a slide drawn into a rectangle lands. Its rendering and the tab's print now
share it, so a print's strokes land on the slide.

## What Claude's tools see

- **`show_pptx` and `show_layout`:** the answer gives what the user drew, slide by slide or cell by cell: how many
  strokes, their box in the tool's own measures (points from the slide's top left; µm), their colours (a
  highlighter's said), and whether the pen is taken up. `read_pptx`'s `its tab` has the same.
- **Pictures:** `screenshot` of a slide and `read_pptx`'s pictures of an open deck show the drawings, so a slide
  marked up by the user can be read as the user sees it. A layout's screenshot is the tab, drawings and all.
- **Arguments:** `pen` takes the user's pen up or puts it down, and `drawings: "clear"` takes away what it drew.
  Anything else in either is refused, and nothing is done: a `pen` that is not true or false by the tools' own
  check of each argument's type, a `drawings` other than `clear` by the tool.
- **`export_image`** writes slides from the file, open or not, and so without drawings.

## Tested

| Test | What it holds to |
|---|---|
| `aDrawingIsUndoneChangeByChange` (`test_pptx_doc`) | A stroke begun is the pen's breadth in the measures drawn on (4 pixels at 8 a measure: 0.5), a highlighter's its own (2.5) and let through (alpha some 0.4); a part's strokes and the parts; the eraser takes the latest within its reach and its own half width, no other part's; a stroke's box; adding, an eraser's drag and a clearing undone in turn, back to nothing |
| `thePenIsPickedFromItsMenuAndKept` | The menu's entries; picking takes the pen up; another button knows at once; the highlighter's breadth its own; Other Colour… as the chooser gives it, none chosen none changed; kept in the settings; a colour with the eraser in hand takes the pen; the eraser has no breadths; the icon in the pen's colour; undo and clearing as the drawing is |
| `aPenDrawsOnTheSlide` | A drag on slide 2 at 100 %: its points a pixel a point, on slide 2; on screen; in the slide's picture at twice the size, 3 points either side of the line and no more; none on slide 1; past the edge none in the gap; a double click two dots and no show; Ctrl+Z pressed undoes; the eraser and its undo; copied and printed with it; Escape puts the pen down; kept across a reload; the menu clears this slide's, then all |
| `aPenDrawsOnTheCellShown` (`test_layout_doc`) | A drag on TOP: its points in µm where the pixels were, the pen's 6 pixels in µm; 3 pixels either side; zoomed twice, where it was and twice as broad; panned with the layout; a hundredth of the zoom, still a pixel; not on INV, on TOP again; Ctrl+Z pressed; the middle button pans and draws nothing; the eraser and its undo; printed; Escape; kept across a reload; cleared from the menu |
| `claudeSeesWhatThePenDrew` (`test_pptx_doc`) | `read_pptx`'s tab: each slide's strokes, box and colours, a highlighter's said; `screenshot` of slide 2 and `read_pptx`'s picture show it, slide 1's not; `show_pptx` takes the pen up and puts it down and clears; a wrong `pen` or `drawings` refused, nothing done |
| `claudeSeesWhatThePenDrewOnALayout` (`test_layout_doc`) | `show_layout`'s cell, strokes, box in µm, colours; the pen up and down; cleared; a wrong `pen` or `drawings` refused |

## Broken on purpose

`2026-11-02-pen-markers/breaks.py` holds twenty-nine, across:

- **A stroke:** its colour and breadth, the eraser's reach, undo.
- **The pen in hand and its button:** kept, taken up, told to every button, the eraser left, the breadths, the icon.
- **The layout:** the cells, the middle button, Ctrl+Z, a reload, a print, Escape.
- **The slides:** which slide, its edge, a double click, its picture, a print, Ctrl+Z.
- **Claude's tools:** the drawings said, a slide's picture, a wrong `pen`.

All twenty-nine are caught. A first run let four through, and each showed something:

- **Every cell's strokes drawn on whichever is shown:** the layout's test looked for TOP's stroke on INV where it
  was out of sight. It frames the stroke's place now.
- **Ctrl+Z left to the window, on the layout and on the slides:** the views took Ctrl+Z ahead of the window, and
  nothing ever competed, because the window's Undo is off while a viewer is in front. The override went. In its
  place, breaking the slide view's Escape override is caught: the window takes Escape without it.
- **A `pen` of another kind acted on:** the tools check each argument's type before the tool's own code runs, so
  the tool's check of the same went too. In its place, breaking each tool's clearing is caught.

Rebuilt after the breaks:
- the full ctest (151), before those last changes;
- `test_pptx_doc`, `test_layout_doc` and `test_qucs_control` after them;
- the four lints.

All green.

## Not done

- **Drawings are not saved.** They live with the tab. Writing them into a .pptx as ink, or beside a layout, is a
  step further.
- **The slide show** shows the slides as the file has them, and has no pen of its own.
- **The thumbnails** show the slides without the drawings.
- **An RF 2.5D model's own layout** has no pen; `show_layout` refuses `pen` and `drawings` for it, as it refuses a
  layout tab's other arguments.
