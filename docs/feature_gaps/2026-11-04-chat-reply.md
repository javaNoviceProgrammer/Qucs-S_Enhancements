# Reply, in the Claude Code dock's menu

The user, 4 November, with a picture of the dock's right-click menu on a selected line of an answer (Copy, a greyed Copy
Link Location, Select All): "Can you add a "Reply" menu item to the right click menu when a text is selected in the
claude chat. This is similar to how Claude desktop app has the "Reply" option when a text is selected in the chat."

## What it does

With text selected in a conversation, the right-click menu starts with **Reply**, set off from the usual items by a
line. With nothing selected it is not there, and neither is it for a selection of spaces alone, or of nothing but what
works the view (a prompt's Edit, the mark of a reply being written).

Choosing it puts the text in that conversation's composer as a Markdown quote and sends nothing:

```
> The selected text, a line to each line.

|
```

- **The cursor** is on a line of its own, a blank line under the quote, and the focus is in the composer. The blank
  line is there because, in Markdown, a line right under a quote is read as more of the quote.
- **What the composer already holds is kept.** The quote goes at its end, a blank line apart, wherever its cursor was
  and whatever was selected in it. A second Reply adds under the first. One undo takes one Reply back.
- **While a prompt is being edited**, the composer holds that prompt, and the quote goes under it. Cancelling the edit
  brings back what the composer held before, without the quote.
- **In tabs**, the quote goes to the composer of the conversation the menu was opened in. Each panel has its own.
- **When the turn is busy**, the quote waits in the composer like anything typed.

## How a selection reads

`QTextCursor::selectedText()` gives U+2029 between paragraphs, U+2028 for a `<br>`, U+FFFC for a formula or a picture,
U+FDD0 and U+FDD1 around a prompt's frame, nothing for a list's marks (they are drawn, not text), and a table's cells
run together. So `readSelection()` in `claudecodepanel.cpp` walks the selected blocks instead, in the document as it is
drawn:

| What is selected | What the quote holds |
|---|---|
| Paragraphs | A line each, a `>` line between them where they stand apart on the screen. Soft-broken source lines are one line, as drawn |
| A prompt's lines | As typed: no blank line unless one was typed |
| A list | Each item with the mark drawn: `•`, `◦`, and the number it has, not one from 1. Nested items indented |
| Code | Indented four spaces (the plain-text export's way), its blank lines and own indents kept |
| A quote in the reply | One level deeper: `> > text` |
| A table | Within one cell, its words. Across cells, the cells Qt chose, a row to a line with ` \| ` between. Through a table from outside it, all of it |
| Math | The TeX between its dollars, `$\epsilon_{eff}$`, `$$...$$` for display math, whether written with `$` or `\(` |
| A picture | Markdown's `![alt](file)`, so Claude can be pointed at it |
| A rule | `----` |
| Within one block | Its words alone, with no mark or indent |

Bold, italics and inline code come as their words. Zero-width spaces and U+FFFC never reach the quote, and a non-breaking
space reads as a plain one, as anything else in the composer does (its document's `toPlainText()` has it so). Nothing cuts
a long selection: 2.2 MB takes 34 ms to read at the right click and 163 ms to go into the composer. A formula now carries
what it was written as on its format (`kWrittenAs`), because its tool tip can hold more than its TeX.

What only works the view is marked on its format (`kWorksTheView`) and left out: a prompt's Edit, a tool's twisty, and
the ` ▍` of a reply being written. The rest of what the reader saw comes as it is, so a selection across entries holds
their captions ("You", "● Claude") and a turn's line ("Done in 4 s").

## A reply still being written

The menu was only half the work here. `render()` takes out everything from the first changed part and draws it again,
and a reply being written changes at each piece, so a selection in it collapsed within 40 ms. A selection reaching the
end of what stayed then grew with what was drawn after it, because a cursor at the place text goes in moves along with
that text. So `render()` now keeps the selection when what is drawn again reaches it, and chooses it again afterwards:

- by its positions, when the same text is still there;
- else at the nearest place the same text is found, because the words can move as more comes (a `**` closed, a formula
  closed);
- else not at all: nothing selected, rather than a selection of whatever fell there.

The mark of writing is cut off first, so a selection up to the end does not grow with the reply. The view does not move
for this, in either direction, and a view following the conversation's end goes on following it with the selection out
of sight. A cursor with no selection is kept too, unless it is at the very end: that is where a drag just pressed goes
from, and a piece arriving between the press and the first move used to start the selection at the end of the reply.

The quote is read when the menu opens, so a piece arriving while the menu is up changes nothing in it.

## Where else text can be selected

The conversation is one `QTextBrowser`, so every kind of entry goes through the same menu. The only other selectable text
in the panel is in the permission card: the subject (a label) and the details (a read-only box). Both now offer Reply as
well. A label's own menu cannot be added to, so its Copy and Select All are the panel's now, and they do what Qt's did
(`\n` between lines in what is copied). The question card's labels are not selectable, and the git bar's diff window is
not the conversation.

## Tests

In `test_claude_code`, all through `rightClick()`: a right click sent through the window as a mouse's is (it goes through
`QWidgetWindow` and the context-menu event, not to the widget directly), and a click on the item in the real `QMenu`. The
keys are checked once, Down and Return. The helper looks for the menu until it is up: its first form used a timer of 30 ms,
which on a busy machine went off before the click was dealt with (a break run showed it, with a menu left up for 60 s).

| Function | Holds |
|---|---|
| `replyIsOfferedWhereTextIsSelected` | Offered only with text selected; first, with a line under it; not for spaces |
| `replyQuotesTheSelectionInTheComposer` | The composer's exact text, the cursor, the focus; nothing sent (no turn, nothing in the fake program's log, the transcript the same); a second Reply; undo; what is typed after it; sent by Enter, all of it reaches the program |
| `replyKeepsWhatTheComposerHolds` | Drafts ending in nothing, one line, two, three; a cursor and a selection in the draft; a command being typed; a prompt being edited; the composer's height and cursor at 80 lines |
| `replyReadsParagraphsAndLines`, `replyReadsListsCodeAndTables`, `replyReadsMathAsItWasWritten`, `replyReadsThePromptsAndToolsToo` | The table above, exactly; every line of a whole conversation a quote's, and none of U+2028, U+2029, U+FFFC, U+FDD0, U+FDD1 |
| `replyOfALongSelectionIsWhole` | 226 kB, compared whole |
| `aReplyBeingWrittenCanBeQuoted` | A selection in a reply being written, across pieces; the mark never in it; a piece arriving and the selection let go of while the menu is up; words that moved; the nearest same words; backwards |
| `aSelectionStaysAsTheConversationIsDrawn` | A selection up to the end does not grow; drawn whole; backwards; words gone leave nothing selected; the view stays, up and down and sideways, and goes on following the end |
| `aDragSelectsWhatReplyQuotes` | A real drag; a drag across pieces, one arriving before the first move |
| `copyAndSelectAllStayBesideReply` | Copy, Select All and Copy Link Location from the same menu |
| `replyGoesToTheTabItWasChosenIn` | Each its own composer, also with another tab brought forward while the menu is up |
| `replyFromThePermissionCard` | Both places, Copy and Select All on the label, nothing answered |
| `replyAboveTheUsualEntriesBesideOpenInViewer` | Open in Viewer, a line, Reply, a line, the usual items; a reply's and a prompt's picture in the quote |
| `copyLinkLocationIsOfTheLinkUnderThePointer` | The link under the pointer, scrolled down, scrolled along, and right to left; none where there is none |
| `aReplyOpensAsItsMarkdownHasIt` | A first block of code, indented code, a heading, a quote, a rule, a list and an HTML list, a table, display math, a plain paragraph; being written, drawn in part as drawn whole |
| `theConversationsTextTakesTheKeys` (added to) | In the real main window with a schematic open and all of it chosen: the quote, the focus, and what is typed next going to the composer, the schematic untouched |

`test_claude_fuzz` now does it too. `fuzzThePanel` selects anywhere, quotes, and checks that the draft is kept and that
what was added is lines of a quote with none of the characters above or the mark of writing; it also checks that drawing
all again leaves what was selected. `fuzzTheDock`'s transcript-menu step now selects first half the time, so Reply is among
what it triggers.

`docs/feature_gaps/2026-11-04-chat-reply/breaks.py` undoes the work one piece at a time. It counts a break as caught only
when its test fails at the line meant for it (the failing line, as QtTest's `Loc` names it, must hold the break's needle).
All 71 were, in the end. The first run, of 70, left nine. Five were the script's own needles, wrong or naming a later line
of the same test. Three were tests that did not do what they said: a backward selection whose words never moved, a selection
up to an item's start that the "words alone" path hid, and a scrolled-up view whose selection was in a part not drawn again.
Those tests were changed. The ninth was a step in the panel that did nothing: it turned non-breaking spaces into plain ones,
which the composer's `toPlainText()` does anyway, so the step went. The second run, of 71, left two more needles, which were
corrected and those two run again. With the two fixes under "Seen in passing" it has 81, and a whole run left none.

## What was run

| What | Result |
|---|---|
| `test_claude_code`, whole, Release | Passes |
| The same with `QT_SCALE_FACTOR=2` (the 15 new functions) | Passes |
| `test_claude_code` and `test_claude_fuzz` under ASan and UBSan, as CI's Linux job builds them | Pass, 94 s and 15 s; with the fixes 183 s and 33 s, the machine busy, and fuzz seeds 2, 3, 4 at 600 steps with no finding |
| Full ctest in Release, the machine quiet (load average about 10) | 162 of 162, for the feature; for the fixes too, at a load of about 20 |
| The same under other sessions' load (load average 30 to 80) | 161 of 162; `test_git_integration` failed with a dialog left open across its functions and passed alone. An earlier run of the nearly final sources had the same with `test_file_browser`, and with `aReaderScrolledUpStaysPut`'s 2 px (written up as a load flake in `2026-11-03-multiphysics-3d-phase-b.md`), each passing alone |
| `test_claude_fuzz`, seeds 1 to 3 at 800 steps, 11 and 12 at 150 s a function | No finding of the quote's or the selection's, 288 and 266 Replies in the panel's fuzz. Seed 11's dock had one other finding: see "Seen in passing" |
| `fuzzTheDock` again, seed 11 twice and 21, 31, 41, at 150 to 170 s | No finding |
| The four lints | Green |

## Left out

- **The quote at the composer's cursor.** It always goes at the end. At the cursor, a Reply could split a line of a draft
  whose cursor had been left in the middle of it. That is the one behaviour someone may want otherwise.
- **The desktop app's floating Reply button** on a selection, and a quote shown as a chip above the composer. The request
  was the menu item, and a quote that is plain text can be edited like the rest of the draft.
- **A keyboard shortcut.** The keys are the menu's. A shortcut on keys the main window also binds fires neither (see
  Copy and Find in the PPTX viewer). The menu's `&Reply` underlines its letter like Copy's, but on macOS Qt keeps
  mnemonics off, for Copy and for this: the letter does not choose it there, the arrows and Return do.
- **Markdown of the reply's own.** A quote is what was drawn: `**bold**` comes as its word, a bullet as `•` rather than
  `-`, and no backticks around inline code. The source of a part of a selection is not known.
- **Two code blocks one right after the other** read as one in a quote, with no line between. Nothing marks where one ends.
- **Bringing a tab forward.** If another tab comes forward while the menu is up (nothing in the dock does that by itself
  while a menu is open), the quote still goes to its own conversation's composer, but the focus has nowhere visible to go.
- **The welcome page.** Text selected in it is offered Reply too, rather than adding a condition. Harmless.

## Limits measured

A very long run with no place to break a line costs the composer's layout, in the square of its length, as it does the
transcript's own. For a Select All over one reply of a single unbroken run, Release, 440 px wide:

| Run | Drawing the reply | Menu | Into the composer |
|---|---|---|---|
| 16 kB | 51 ms | 1 ms | 26 ms |
| 64 kB | 498 ms | 2 ms | 275 ms |
| 256 kB | 6750 ms | 9 ms | 3593 ms |

The break characters that would avoid this are the ones a quote must not hold, so nothing is done about it. Ordinary
text is cheap: the 2.2 MB above is 30,000 paragraphs.

## Seen in passing

The user, of the first two: "Yes, fix them". They were fixed in the commit after the feature's.

1. **Copy Link Location was greyed on a link once the conversation was scrolled. Fixed.** `transcriptMenu()` handed
   `createStandardContextMenu()` a point in the viewport, and it wants one in the document. At a view scrolled by 2091 px
   the item was off with `anchorAt()` finding the link at that point, and where a link would have been were the view not
   scrolled it was on, and copied that other link. In the code since `dc49cc0`. The point is now moved on by how far the
   view is scrolled, down and along, as Qt's own context-menu event does (right to left, a scroll bar counts from its
   other end). The permission card's details have no links, so their menu now gets no point at all.
2. **A reply that opened with a code block, a heading or a quote drew its first block as plain text. Fixed.** The first
   block of a fragment takes the format of the block it goes into: the first line of such a code block was not shaded and
   wrapped, a heading lost its level, and a quote's first paragraph lost its indent and its bar (and a Reply read it as a
   paragraph). `renderMarkdown()` now gives that block its own format, keeping only how far it is under the caption. A plain
   first paragraph is left as it was, to the pixel. A list or a rule was not hit by this but had its own fault of the same
   place, an empty line over it: a list's first item, or a rule, comes in a block of its own after the one the reply starts
   in. That empty block is now taken out. Before a table it stays: it cannot be removed (it ends in the frame's start) and
   Qt does not draw it.
3. **One long fuzz run reported a line the dock wrote as not JSON. Not changed.** `fuzzTheDock`, seed 11, 150 s: a long MCP
   answer, of which the report keeps the first 200 characters. It did not come back in five more runs with the check made to
   say more. Nothing in this work writes to the program. The fake program is `cat`, stopped when its tab closes, and a long
   line stopped part-way would read so; that is a reading, not proved. CI's short pass has no texts that long.

## Not exercised

- **A real display.** All of the above ran offscreen. The window's route for the right click is the same as on a display,
  and the menu in the user's picture is Qt's own, not a native one, so the offscreen path is the same code. The icon comes
  from the platform theme (`mail-reply-sender`, which Qt 6.11's Apple icon engine maps to `arrowshape.turn.up.left`), so it
  shows only on the display and has not been seen; without it the item would simply have none.
- **CI**: nothing is pushed. No GCC or Windows build here.
