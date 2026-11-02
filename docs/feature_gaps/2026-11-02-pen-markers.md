# Pens on the layout and slide viewers

The user's requests of 2 November:
- "Can you add pen markers to the layout viewer and pptx viewers? This is to allow me select a pen and its color
  and then do drawings on an open gds layout or an open powerpoint pptx?"
- Then: "Can Claude MCP had direct view of what I draw with these pens? I want claude to easily see these (if
  possible, without screenshotting)." That is the second half of this note, from
  [What Claude reads of a drawing](#what-claude-reads-of-a-drawing).

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

- **Readings:** what the pen drew comes to Claude as a reading of each stroke, with no picture (next section).
- **Pictures:** `screenshot` of a slide and `read_pptx`'s pictures of an open deck show the drawings, so a slide
  marked up by the user can still be looked at as the user sees it. A layout's screenshot is the tab, drawings and
  all.
- **Arguments:** `pen` takes the user's pen up or puts it down, and `drawings: "clear"` takes away what it drew.
  Anything else in either is refused, and nothing is done: a `pen` that is not true or false by the tools' own
  check of each argument's type, a `drawings` other than `clear` by the tool.
- **`export_image`** writes slides from the file, open or not, and so without drawings.

## What Claude reads of a drawing

Each stroke is read the way a person reads a mark on a page: how it was drawn, and what it marks.

**How it was drawn:**
- a **dot**;
- a **ring**, which ends back near where it began, round a fair share of its box;
- a **line**, no point of it further from its chord than a tenth of the chord;
- an **arrow**. It may be drawn at one go: out to its tip, then back a little along a barb (more than the pen's
  wobble as it lifts). Or it may be a line with one or two short strokes drawn just after it at one of its ends:
  those read as its **arrowhead**, and the arrow points at that end, whichever way the line was drawn. A dot there
  is a pin, not a head;
- a **mark**: anything else, such as a tick, a cross or a scribble.

**What it marks, on a slide.** A slide is read as its parts, where they are drawn: each paragraph and each of its
words, a table's cells and their words, pictures (by their alt text, else their name), charts (by their title) and
shapes.
- A **ring** is round a paragraph whole, or round some of its words (three fifths of a word inside it), or round
  half of a part with no words.
- A **line** along a line of text underlines the words it runs under (just under their foot) or strikes through
  them (across their middle).
- A **highlighter**, or a **mark**, lies on the words and the parts it passes over.
- A **line or an arrow** that is neither runs *from* what its start is on and *points at* what its tip is on. If its
  tip is on nothing, it points at the first thing on ahead of it (a third of its length more); failing that, at the
  nearest thing within its reach.
- A **dot** is on the word, or the part, under it.

**What it marks, on a layout.** A layout is read as the shapes by layer, the texts and the cells placed. Only the
layers shown are read, to the depth drawn, and the texts only while they are shown.
- A **ring** holds what is half inside it or more (a text, by its origin), and the cells placed half inside it.
- A **highlighter** or a **mark** lies on the shapes within half its breadth.
- A **line or an arrow** runs from what lies under its start. It points at what lies under its tip, and at the
  placements the tip is in. If nothing is there, it looks just ahead, then at the nearest shape within its reach,
  so that a tip that stops short of what it means still finds it.

Each reading also has the stroke's number on its slide or cell, its tool (pen or highlighter), its colour, and its
box. Points from the slide's top left, or µm. With its points, it has them simplified to a few dozen at most
(Douglas and Peucker's), a line's two ends, a ring's few corners.

A slide's reading of an arrow:

```json
{"slide": 3, "stroke": 6, "drawn as": "arrow", "tool": "pen", "colour": "#e01b24", "box": [439, 117, 586, 201],
 "from": {"words": "frequency", "in paragraph": "The gain rises with frequency"},
 "points at": {"picture": "Bode plot"}}
```

The same in a sentence, as a prompt has it:

```
slide 3, stroke 6: a red pen arrow from "frequency" in the paragraph "The gain rises with frequency" pointing at the picture "Bode plot" (its box 439, 117 to 586, 201 points)
```

A layout's ring, in a sentence:

```
cell TOP, stroke 1: a blue pen ring around 1 shape(s) on 1/0, 1 shape(s) on 2/0, the text OUT, 1 placement(s) of INV (its box 9.15, -1.35 to 12.85, 2.35 µm)
```

### Where Claude gets it

- **The prompt itself.** A prompt sent from the Claude Code panel that names a layout or a presentation (by the
  document chip, or the document the conversation is pinned to) carries what is drawn on it, a sentence a stroke
  (the latest 30, the earlier counted). It does so once for each change: drawn on, the next prompt says so;
  unchanged, the prompts after say nothing more; all taken away, said once. A new conversation is told afresh, and
  is never told that a drawing it never heard of went. So the user draws, asks "what do you make of this?", and
  Claude already has it. The panel asks for a reading knowing the revision it last told, and the document is read
  again only when its drawing changed: a big layout's reading takes a while.
- **`read_drawings`** (new, read-only). It reads every open layout and presentation that is drawn on, or the one
  named by `path` (its file or its tab's title). Each stroke's reading comes with its points; `points: false` leaves
  them out. It gives the latest 200 a document, the earlier counted. It is refused for a document that is not open,
  or not a layout or a presentation. While anything is drawn on, it is among the tools Claude is shown at every
  turn.
- **`show_pptx`, `show_layout` and `read_pptx`'s tab** give the same readings without their points, the latest 40,
  the earlier counted, and whether the pen is taken up.

### How the reading is built

- **`markup.{h,cpp}`** reads a stroke the same way on both viewers:
  - `gestureOf()` and `gesturesOf()`: how it was drawn, and an arrowhead joined to its line. An arrow drawn at
    one go is looked for before a line: its small head lies within a tenth of its chord, so the line's test would
    take it first;
  - `simplified()`, `insideShare()` and `alongOf()`;
  - `readPage()`: a slide's strokes against its parts, the targets;
  - `readingOf()` and `reachOf()`, and `sentenceOf()` and `sentencesOf()` for prompts.

  A drawing's `revision()` moves on at each change, to a number no drawing has had. That is how the panel knows what
  it told.
- **`Presentation::parts()`** gives a slide's parts where they are drawn. It mirrors `paintElement()`:
  - groups in their children's space;
  - a turned or flipped shape's text, turned as it is drawn;
  - a table's cells, by `cellRects()`, which the table's drawing now shares;
  - a chart's title.

  `TextFrame::placed()` places a frame's paragraphs and words as `paint()` does: its anchor, its insets, vertical
  text, an unwrapped frame's alignment. A bullet that is part of the text is not one of its words. A shape or a
  picture keeps its alt text (`descr`), else its name.
- **`LayoutView::readDrawing()`** does one search of the layout for each question about a stroke, at most 200 000
  shapes a search. At a tip it asks in turn: what is under it, within half the stroke's breadth; then what is on
  ahead; then the nearest. Asking for everything within the stroke's reach at once took in a path beside the shape
  that was meant.
- **`QucsControl`** has `read_drawings` and `insertDrawings()`, and adds `read_drawings` to every turn while
  something is drawn on.
- **The panel:** `ClaudeCodePanel::setDrawingsProvider()`, through `ClaudeCodeTabs`. `QucsApp::drawingsOf()` reads
  a document for it.

## Tested

| Test | What it holds to |
|---|---|
| `aDrawingIsUndoneChangeByChange` (`test_pptx_doc`) | A stroke begun is the pen's breadth in the measures drawn on (4 pixels at 8 a measure: 0.5), a highlighter's its own (2.5) and let through (alpha some 0.4); a part's strokes and the parts; the eraser takes the latest within its reach and its own half width, no other part's; a stroke's box; adding, an eraser's drag and a clearing undone in turn, back to nothing |
| `thePenIsPickedFromItsMenuAndKept` | The menu's entries; picking takes the pen up; another button knows at once; the highlighter's breadth its own; Other Colour… as the chooser gives it, none chosen none changed; kept in the settings; a colour with the eraser in hand takes the pen; the eraser has no breadths; the icon in the pen's colour; undo and clearing as the drawing is |
| `aPenDrawsOnTheSlide` | A drag on slide 2 at 100 %: its points a pixel a point, on slide 2; on screen; in the slide's picture at twice the size, 3 points either side of the line and no more; none on slide 1; past the edge none in the gap; a double click two dots and no show; Ctrl+Z pressed undoes; the eraser and its undo; copied and printed with it; Escape puts the pen down; kept across a reload; the menu clears this slide's, then all |
| `aPenDrawsOnTheCellShown` (`test_layout_doc`) | A drag on TOP: its points in µm where the pixels were, the pen's 6 pixels in µm; 3 pixels either side; zoomed twice, where it was and twice as broad; panned with the layout; a hundredth of the zoom, still a pixel; not on INV, on TOP again; Ctrl+Z pressed; the middle button pans and draws nothing; the eraser and its undo; printed; Escape; kept across a reload; cleared from the menu |
| `strokesAreReadAsDrawn` (`test_pptx_doc`) | A dot; a ring; a line, straight or nearly; an arrow at one go, its tip where it turns back; a pen lifted with a wobble still a line; a tick a mark. A head drawn after its line joined to it, and pointing at its end when the line was drawn from its tip; a short stroke far from either end not; a dot at an end a pin. Simplified: a line to its ends, a square to its corners; a ring's share of a box. On a page of a paragraph, a picture and a chart: a ring round the paragraph, and one round two of its words with a tenth of their neighbours inside, not held; a line under two words, one through a word; a highlighter over two; an arrow from a word to the picture it stops short of; a dot on a word; a tick on the chart; a dot on nothing; the points; the sentences, "an orange" pen among them, the latest three with the six before counted |
| `slidePartsAreWhereTheyAreDrawn` | A title, a text box's two paragraphs, a picture by its alt text and one by its name, a table's two cells, a chart by its title, a turned text, a group's text, a bulleted paragraph, in that order. Each word's box has ink under it, and the words go in turn along their line; the turned word runs down the slide; none of a text's ink lies outside its words' boxes, nor are the boxes much broader than the ink; the bullet is not a word |
| `claudeSeesWhatThePenDrew` | Nothing drawn, `read_drawings` says so. `read_pptx`'s tab, stroke by stroke: its slide, number, gesture, tool, colour and box, no points; the ring round slide 2's title round its paragraph; a highlighter's yellow. `read_drawings`: the deck, its measures, each stroke's points, the same readings; `points: false`; refused for a file not open. In every turn while drawn on. `QucsApp::drawingsOf()`: the drawing's revision, a line a stroke; asked knowing that revision, nothing read. `screenshot` of slide 2 and `read_pptx`'s picture show it, slide 1's not. `show_pptx` takes the pen up and puts it down and clears; a wrong `pen` or `drawings` refused, nothing done. 245 strokes: the tab's latest 40, `read_drawings`' latest 200, each with the earlier counted; refused for a text document |
| `whatThePenDrewOnALayoutIsRead` (`test_layout_doc`) | On TOP: a ring round the first INV holds a shape on 1/0 and one on 2/0, the text OUT and the INV placed there; one round the VIA array its six squares on 3/0 and six VIAs; an arrow at one go from the triangle (5/0) points at the turned INV's body and that INV, which it stops short of; a highlighter lies on the four squares on 4/0 it passes; a dot on the mirrored INV's body is on it and in that INV; a line with its head drawn after it points at the first INV's body from above. 1/0 hidden: not read, the dot in the INV alone; the texts hidden; depth 0, the cells alone; the points; the sentence |
| `claudeSeesWhatThePenDrewOnALayout` | `show_layout`'s reading: its cell, number, gesture, box in µm, colour, no points; `read_drawings` with them; `QucsApp::drawingsOf()`, and nothing read when its revision is known; the pen up and down; cleared; a wrong `pen` or `drawings` refused |
| `whatThePenDrewGoesWithThePrompt` (`test_claude_code`) | Ten prompts to a fake Claude Code with a fake drawing: nothing drawn, nothing said; drawn, the reading; unchanged, nothing; changed, the new reading; taken away, said once, then nothing; the document chip off, neither the document nor its drawing, on again, both; a new conversation told afresh; one told nothing not told it went. Each prompt asks knowing the revision last told: after it was told, that one; after "taken away", none |

## Broken on purpose

`2026-11-02-pen-markers/breaks.py` holds seventy-two:

- **The pens (1 to 29):**
  - a stroke: its colour and breadth, the eraser's reach, undo;
  - the pen in hand and its button: kept, taken up, told to every button, the eraser left, the breadths, the icon;
  - the layout: the cells, the middle button, Ctrl+Z, a reload, a print, Escape;
  - the slides: which slide, its edge, a double click, its picture, a print, Ctrl+Z;
  - Claude's tools: the drawings said, a slide's picture, a wrong `pen`.
- **Reading a stroke (30 to 44):** a ring, a one-go arrow, a pen's wobble, a head drawn after its line, a dot at a
  line's end, an arrow drawn from its tip; some words said to be all, a word a twentieth inside a ring held,
  underline and strike swapped, an arrow pointing only at what its tip is on, a highlighter's words; a sentence's
  marks, the strokes left out uncounted, "a orange", the points not simplified.
- **A slide's parts (45 to 51):** a frame's anchor, a bullet as a word, a picture's alt text, a group's space, a
  table's cells, a turned shape, a chart's title.
- **A layout's reading (52 to 59):** the layers hidden, the texts hidden, the depth, everything within an arrow's
  reach at its tip, the cells at its tip, the cells in a ring, a highlighter at its points alone, a repeated
  shape's copies.
- **`read_drawings` and the prompts (60 to 72):** the points, a refusal, the two caps, every turn, the app's
  reading for a deck and for a layout, the panel's telling (the same twice, a drawing it never told of, a new
  conversation, the document chip off), a reading asked as if nothing were told, one read though nothing changed.

All seventy-two are caught, each at the assertion it was meant for (its log's failing line), none by a crash.

The first twenty-nine's first run let four through, and each showed something:
- **Every cell's strokes drawn on whichever is shown:** the layout's test looked for TOP's stroke on INV where it
  was out of sight. It frames the stroke's place now.
- **Ctrl+Z left to the window, on the layout and on the slides:** the views took Ctrl+Z ahead of the window, and
  nothing ever competed, because the window's Undo is off while a viewer is in front. The override went. In its
  place, breaking the slide view's Escape override is caught: the window takes Escape without it.
- **A `pen` of another kind acted on:** the tools check each argument's type before the tool's own code runs, so
  the tool's check of the same went too. In its place, breaking each tool's clearing is caught.

The reading's breaks were all caught at their first run. Writing them showed six the tests would have let through,
and the tests were made sharper first:
- **A dot at a line's end taken for its head:** no test had one. One does now; a dot is a pin.
- **A bullet read as a word:** no slide had a bullet in its text. One has.
- **A group's words where its children's own space puts them:** each word's box still had ink under it at twice
  the size. A text's words are now held to its ink's extent too.
- **A word a ring holds a twentieth of taken as held:** no word was partly inside a ring. Two neighbours are now, a
  tenth each.
- **The app's reading for a prompt:** it was a lambda no test reached. It is `QucsApp::drawingsOf()` now, tested
  with both viewers.
- **The answers' caps:** no test drew more than three strokes. One draws 245.

Rebuilt after the breaks:
- the full ctest;
- the four lints.

All green.

## Not done

- **Handwriting is not read.** Letters or figures written with the pen read as marks, by what they lie on.
- **What is inside a picture or a chart** is not known: an arrow at a curve in a plot points at the picture, and
  its points say where; a `screenshot` of that region shows it.
- **A ring is judged against boxes:** a word's, a part's, a shape's. A ring round a long diagonal shape may not
  hold its box.
- **Gestures are judged by their shape alone:** a hooked underline reads as an arrow, and a hasty ring round one
  word takes in a neighbour it holds three fifths of.
- **A layout's reading looks at 200 000 shapes at most** for each question about a stroke. On a flat cell larger
  than that, what lies beyond may be missed.
- **Drawings are not saved.** They live with the tab. Writing them into a .pptx as ink, or beside a layout, is a
  step further.
- **The slide show** shows the slides as the file has them, and has no pen of its own.
- **The thumbnails** show the slides without the drawings.
- **An RF 2.5D model's own layout** has no pen; `show_layout` refuses `pen` and `drawings` for it, as it refuses a
  layout tab's other arguments.
