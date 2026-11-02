# Long lines in the window's labels

The user's report of 2 November, with two pictures of the RF 2.5D tab: "when there is large line of text, it will
mess up the claude code panel and the entire window dimensions and forcefully resize them. This happened in the
layout viewer panel and other panels (RF 2.5D, ...). Maybe you can just wrap the text or use "..." if its length go
beyond a certain length and have tooltip for it?"

## What it was

A QLabel of one line asks its layout for the width of its whole text, and the layouts pass that up to the window.
The RF 2.5D tab's status line ran to some hundred and fifty characters ("4 layer(s), 2 of them metals or ground; the
stack is 152 µm high; 1 thing(s) missing; 3528 cell(s), 3528 to solve the charge on, a dense matrix of 95.0 MB;
solving", and its S results line). The main window's least width became more than the screen's, and Qt widened the
window to it. The Claude Code dock at its right went past the screen's edge, cut off.

The Claude Code panel had the same fault in a button. The composer's chip of the document in front, and its pin,
show a file's name, and a tool button asks for the whole of its text at a fixed width. The panel caps a name at the
setting's number of characters (`FileNameCap`), which still leaves it hundreds of pixels wide.

## What changed

**ElidedLabel** (`qucs/elidedlabel.{h,cpp}`) is a QLabel that asks for the width of four characters and its
margins. Where its text does not fit, it draws it cut, with an ellipsis, and its tool tip is the whole text, before
the label's own tool tip if it has one. Its size policy and preferred size stay a QLabel's, so where there is room
its text shows whole, and it gives way only when the room is short.

`text()` stays whole. The tests and the control's tools read labels by `text()`, and the tabs set them through
their `QLabel*` members. The text is cut when it is drawn, so every `setText` already in the code works as it did. A
label's rich text, cut, is drawn as its plain text, and its links still answer a click. A label with word wrap is
left as a QLabel is. The status bar's own `HintLabel` cuts its text into `setText` and keeps the whole in a member;
that would have changed what `text()` gives every reader, so it was not the way here.

**ElidedToolButton**, beside it, does the same for a tool button's text. It asks for its icon and four characters,
and cuts a name in the middle so that the name keeps its kind (`a_model_of_th….qrf25`). A tool button's width is
fixed, and a layout holds a fixed width to the whole text, whatever the least width says. So this one shrinks, and
grows no wider than its text. Its tool tip is its own, which names the file.

These give way now:

| Where | Label | What it shows |
|---|---|---|
| RF 2.5D tab | `rfStatus` | The status line of the pictures: the stack, the mesh, a solve's progress and results |
| | `rfLayoutLabel` | The layout file and what it holds, or why it could not be read |
| Layout viewer | `layoutCell` | The cell shown, by its name (a GDSII cell's name can be long) |
| | `layoutInfoText` | What is selected or measured; it was cut already, without an ellipsis |
| Multiphysics tab | `mpStatus` | The status line |
| Image tab | `imageFacts` | The picture's facts; it was cut already, without an ellipsis |
| Presentation tab | `pptxNotesTitle` | The slide's number and title |
| Archive tab | `zipStatus` | What the archive holds, packed and changed |
| XML tab | `xmlStatus` | Well formed, or the parser's error and where (rich text, a link) |
| Claude Code panel | `claudeState`, `claudeModel` | The session's state; the model and its tag, which a setting can make any text |
| | `claudeAttach`, `claudePin` (buttons) | The document in front, and the one the conversation is pinned to |
| Python panels | `pythonRunStatus`, `pythonPlotsTitle`, `pythonDataFilters` | A run's status; a figure's title; the filters shown |
| Git panels | `gitCommitHeader`, `gitHistoryStatus` | The repository's path and branch (cut in the middle, the branch kept); the history's status |

## How they were found

`test_long_lines` has an opt-in audit (`QUCS_LABEL_AUDIT=1`). It opens a file of each kind in the main window with the
Claude Code dock shown: an RF 2.5D model, a layout, a multiphysics model, a schematic, a text file, a sheet, a
picture, a PDF, an XML file and an archive. It says what the window asks for, then gives every visible label of one
line in turn a line of 2000 characters, and lists those that widened the window, with the first ancestor whose
least width took the line.

Before the change it listed the RF 2.5D and multiphysics status lines, the Claude Code panel's state and model, and
an image tab's label. The source was then read for every label member whose text the program sets and that had
neither word wrap, an ignored width nor a fixed one. There were thirty, and fourteen of them can hold a long line;
the rest are counts, positions and cell names such as "B12" or "3 of 12". The layout viewer's info line and the
image's facts, cut already at an ignored width, got the ellipsis and the tool tip too. After the change, of the 10
to 20 labels each kind of tab shows, the audit lists only labels whose text never changes or stays short: the RF
2.5D and multiphysics panels' start pages ("RF 2.5D", "Examples", "Recent models"), the toolbars' "Levels", "Show:"
and "of 1", the composer's "⏎ send · ⇧⏎ new line", and the image tab's selection and pointer (a size and a
position).

The chips were found by the window's own sizes. The Claude Code dock's least width went from 375 to 439 pixels as
the document in front went from `rc.sch` to `photodiode.qfem`; it is 367 now, whatever the name. Buttons whose text
the program sets were read for the same fault, and the two chips alone take a name; the others hold "PR #12", "Grid
10", "Send" and the like.

- **The audit measured itself at first, twice.** It put each label's text back and set the window's size in one
  step. The size came before the layout had run, so the window kept the long line's width, and the labels measured
  after a wide one seemed wide too. And it measured before the main window's own layout had run: a dock's least
  width and a tab's toolbar showed in the window only then, so the RF 2.5D layout line and the layout viewer's cell
  seemed to pass nothing on. The tests' `settle()` now runs the main window's layout as its layout request would,
  and each label is measured from the window as it is just then. Run so, the old build's RF 2.5D layout line, a
  plain QLabel again, asks the window 17 240 pixels.
- **The status bar was not one.** Its chips' row asks for no width (`ChipRow::minimumSizeHint`), hiding what does
  not fit, so the diagram's readout, which only ever grows while over a diagram, never reaches the window.
- **A dock never sized follows its contents' preferred width.** Qt's dock layout uses its docks' size hints until
  something sizes them: `restoreState`, `resizeDocks`, a separator dragged, a dock dropped (`fallbackToSizeHints`
  in qdockarealayout.cpp). Run alone, the test's Claude Code dock went from 366 to 692 pixels when a document of a
  long name came to the front, taking the room from the tabs: the chip's preferred width is its name's. The window
  restores its panels at every start (Restore Panels, on by default), so it is a first run's, and Qt's own. The
  tests size the dock as a restore does.

## What the window asks for

The window's least width, macOS's fonts, an RF 2.5D tab and both docks open: 1438 pixels, measured before the
chips gave way. The tabs ask 747 (the RF 2.5D bar's nine buttons with their words, "Zoom Extents" to "Solve"; a tab
widget asks for its widest page's, in front or not), the main dock 284 and the Claude Code dock 405, its composer's
row. The chip of the document in front gives way now, so the dock asks less. That is within a laptop screen of 1512
points, and nothing in it is a line of text that grows. Offscreen, where the tests run, the window asks 1509: 835,
295 and 367.

The packaged apps through MCP, offscreen, an RF 2.5D model of a 76-character name open (its layout file's name the
same, its status line "3 layer(s), 2 of them metals or ground; the stack is 102 µm high; nothing is missing; 4224
cell(s), 12512 unknowns; held compressed, some 238.6 MB"): the old build's window 1500 pixels wide, its layout line
whole across the tab's bar; this one's 1136, the layout line and the status cut with an ellipsis.

## What fixing it showed

The RF 2.5D tab's test of a port placed by hand failed once its window was no longer widened. A rectangle dragged
from 400, 10 µm to 520, 40 µm came out 25.8 µm tall, not 30, its middle where it should be. The layout view built a
dragged box with `QRect::normalized()`, which in Qt 6 moves a swapped corner in by a pixel, so a box dragged up or
to the left came out two pixels short. The test had passed because the RF 2.5D layout line, a long line itself, had
widened the test's window to 1341 pixels and its view to 886, where two pixels were less than the test's 4 µm; at
the window's own 1300 they were more. The box is now the one between the two points whichever way it is dragged,
and the layout test drags one each of the four ways, pixel for pixel.

## Tested

| Test | What it holds to |
|---|---|
| `aLongLineIsCutAndWholeInItsTip` (`test_long_lines`) | The label beside a button, with the RF status's margins, set through a `QLabel*`: its least width exactly the margins and four characters, the row's under its 320 px; the line cut with an ellipsis, a beginning of the text, within its room; drawn pixel for pixel as a QLabel draws that cut line; the whole text its tool tip, its own tool tip after it, and alone when the text fits; a short text's least width never more than its own, a two-letter one's its own; rich text cut as its plain text; with word wrap never cut, its least width a QLabel's, a word longer than the row too |
| `aLongNameIsCutInItsButton` | The button of a name beside a stretch and a button: its least width its icon and four characters, the row's under its 260 px; a long name cut in the middle, its beginning and `.qrf25` kept; drawn pixel for pixel as a QToolButton draws that cut name; a short name drawn whole, a two-letter one's least width its own |
| `aDocumentsLongNameLeavesTheWindowAsItWas` | A document of a 200-character name in front after one of five: the window's width and least width and the dock's width as they were; the chip showing the panel's capped name, cut further, the whole name in its tool tip; pinned, the pin the same |
| `aPickingModeSaysWhatIsPickedAndSelectsNothing` (`test_layout_doc`), added to | A box dragged each of the four ways: the box between the two points, to a hundredth of a pixel |
| `aTabsLongLineLeavesTheWindowAsItWas` | Each kind of tab above, the Claude Code dock beside it: every label that gives way, and the ten named ones whatever their class, given a line of 3000 characters - the window's width and least width and the dock's width as they were, the line cut with an ellipsis, `text()` whole; each named one seen |

## Broken on purpose

`docs/feature_gaps/2026-11-02-long-lines/breaks.py` holds twenty-nine:

- **The label's least width:** the whole text's, as a QLabel asks; four characters whatever the text; without the
  margins around the text.
- **Its cutting and drawing:** never cut; drawn whole; cut to the label's width rather than the room inside its
  margins; drawn from the label's edge.
- **Its tool tip:** without the whole text; without the label's own.
- **Rich text and word wrap:** rich text cut as its markup; cut with word wrap too; word wrap's least width four
  characters too.
- **The chips' button:** its least width its whole name; its width fixed, as a tool button's is; drawn whole; its
  name cut at the end, its kind lost.
- **The labels and chips:** each of the ten labels in the table that the window test opens, and the two chips,
  made plain again.
- **What fixing it showed:** a layout's box dragged up or to the left as `QRect::normalized()` leaves it.

All twenty-nine were caught. Of the twelve labels and chips made plain again, nine failed on the window's or the
dock's least width itself - the RF 2.5D status asked the window 16 453 pixels, the layout line 17 240, the layout
viewer's cell 16 836, the multiphysics and archive statuses some 16 450, the Claude Code state and model 17 070,
and the chips the dock 692 and 672 pixels, where it had 418. The info line and the image's facts, at an ignored width, and the XML status, whose bar
passes nothing on, failed on being drawn whole, not cut.

The pin chip, made plain, once got past the window's widths: its dock's least width was already 672 pixels, the
dock still 418, because the main window had not laid itself out yet when the test measured. That is the second
mistake of the audit above, and the tests now hold the dock's least width to its width as well. The box dragged
up or to the left was caught by the layout test and by the RF 2.5D tab's port placed by hand.

## Not done

- **The labels of the Python and git panels and the presentation's notes** give way as the others do, but no test
  opens them: their panels need a Python, a repository or a slide deck.
- **The start pages' headings** still ask for their whole text. It is a few words and never changes.
- **A first run's docks** follow their contents' preferred widths, as Qt lays docks out until they are sized. A long
  state or name in the Claude Code panel can widen the dock then, at the tabs' cost and not the window's, until the
  panels are restored at the next start or a separator is dragged. Turning that off would change how every dock
  first takes its width.
- **No limit on the window itself.** A tab's least width could be held below the screen's whatever its contents ask
  (a least width on the document tabs), but contents asked to be narrower than they can be would be drawn over each
  other. Every label found was fixed instead, and the audit is there to find the next.
- **Dialogs** were not audited. A dialog sizes itself, apart from the main window.
