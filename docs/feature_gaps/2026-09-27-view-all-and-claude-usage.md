# View All with diagrams, and what a Claude turn took

*27 September 2026 — Qucs-S 26.1.3.*

Two requests:

1. With plots on a schematic, *View → View All* did not work out the
   schematic's bounds correctly.
2. The Claude chat showed each turn's cost in dollars. The user wanted
   settings for the tokens of each prompt, the tokens of the whole
   conversation, the cost of each prompt and the cost of the conversation.
   By default only the tokens are shown and the cost is hidden.

## View All with diagrams

View All zooms to `Schematic::allBoundingRect()`, the union of every
element's bounds. The markers were the fault, and two more bounds were
wrong:

| What | Was | Now |
|---|---|---|
| A marker (`Marker::boundingRect`) | Its label and its point were in the diagram's own coordinates, from the diagram's lower left corner, and the point was upside down (y grows upwards on a diagram). So every marker added a box near the schematic's origin. A diagram at (1200, 1500) with one marker made View All show everything from (250, −360) on, and the schematic filled a third of the window. A selection rectangle drawn near the origin also selected markers far away. | Where `paint()` draws it, on the schematic: its label (with the frame drawn round it when it is selected) and the point it marks, with its indicator. |
| A diagram's title and the numbers of its axes | `Diagram::boundingRect` left out the title, although `Bounding()` and `getSelected()` have it. It also left out the numbers centred on the top and right edges of the frame. The margin View All keeps (40) and the one an image export keeps (30) mostly hid this, but not for a title wider than its diagram. | `boundingRect()` includes the title. The used area and the selection's bounds include all a diagram draws (`paintedRect`), so *Print*, *Print Selection*, *Zoom to Selection* and the image export have it too. |
| A Smith chart's drawing (`Diagram::paintedRect`) | Each arc of its grid counted as its whole circle, and a reactance circle is many times the size of the chart. `SmithChartTest.sch`'s chart of 232 × 217 counted as 482 × 965. `paintedRect` sizes the white card a diagram is drawn on in the dark theme, so that card was far larger than the chart. Once the used area takes `paintedRect` (the row above), View All would have shown these schematics small in the middle. | An arc counts as what is drawn of it: its two ends, and the points straight up, down, left and right of its centre that it passes. |

### Tests

`test_view_all`:

- A marker is bounded where it is drawn, and a selection rectangle takes
  it there, not by the origin.
- A title wider than its plot is in the used area and in a selection's
  bounds.
- The used area holds all a diagram draws, and nothing else.
- View All shows whole a plot with a title and one or two markers, and a
  narrow plot with a long title. No ink is within 3 pixels of an edge, and
  the drawing fills three quarters of the window one way or the other.
- The same holds for the examples: every one with a diagram other than a
  plot (Smith charts, tables, timing and truth diagrams, histograms, 3D
  plots) and every third with plots alone. `VIEW_ALL_EXAMPLES=1` runs all
  204. One example, whose Verilog-A model asks to be compiled as it opens,
  is skipped.

Each fix was taken out in turn, and the full sweep was run each time:

- Without the marker fix: the 5 examples with markers failed
  (`Amp_Two_Tone`, `rf_osci`, `sym_osci`, and the two Xyce S-parameter
  examples), and so did the plot tests.
- Without the arc fix: the 12 examples with Smith charts failed.
- Without the title fix: `aTitleIsInTheUsedArea` failed.

## What each Claude turn took

| Asked | Now |
|---|---|
| Tokens of each prompt | "46.5k tokens" on the line that ends the turn. The count covers every model call the turn made (the main loop, subagents, compaction) and every kind of token: input, output, and the context read from the cache and written to it. A tooltip gives each kind in full. On by default. |
| Tokens of the conversation | "318k tokens in all", the sum so far. It is left out on a turn where it equals the prompt's (the first turn), unless the prompt's tokens are hidden. On by default. |
| Cost of each prompt | "$0.046", as Claude Code estimates it. Off by default. |
| Cost of the conversation | "$0.196 in all". Off by default. |
| Where | *⋯ → Show Usage* in the dock: four switches that apply to every conversation. Each conversation redraws its lines as soon as a switch changes, and the switches are kept in the settings (`ClaudeCode/show…`, carried by *Export Settings*). |

The same choices apply to the conversation's exports (Markdown, text and
PDF; the header of an export gives the conversation's tokens and cost as
chosen) and to `/status` ("Used: …", "Cost: …").

### Where the numbers come from

Each `result` message Claude Code writes has `modelUsage`: tokens and cost
per model, as running totals since the program started. Claude Code's own
description calls this "the correct field for token/cost accounting". Its
`usage` covers the main loop alone, per turn. So a turn's tokens are what
the running totals of every model added since the previous result, just as
its cost is what `total_cost_usd` added. A result whose totals are zeroed
(a crash's) takes nothing away. From a program that has no `modelUsage`,
`usage` is taken as the turn's own. A new program counts from nothing
again, and the conversation's totals go on.

A turn's line keeps these numbers, not text, so it is written as the
settings say each time it is drawn, exported or brought back. A
conversation kept before this change holds its costs as text ("$0.021 ·
$0.150 in all"). They are read out of it when it is brought back, so they
are hidden and shown like the rest. A conversation brought back goes on
from its last turn's totals.

### Tests

`test_claude_code`:

- `theStreamBecomesSignals`: every model's tokens are summed, and the
  second result counts what it added.
- `aTurnsTokensAreCounted`: a zeroed result, `usage` alone, and totals
  continued and then forgotten.
- `aTurnWithTheProgram`: a new program counts from nothing.
- `theDockShowsTheConversation`: the tokens are shown and the cost is not
  at first, with a tooltip for each kind. Each switch changes the line.
- `usageIsShownAsChosen`: a conversation brought back, in the new format
  and the old one. The menu of one conversation changes both. Exports,
  `/status`, and what is kept.
