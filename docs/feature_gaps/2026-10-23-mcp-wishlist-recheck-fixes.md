# The re-check of the wishlist fixes: its three open items

A re-check of the seven fixes (`~/Desktop/qucs-reviews/2026-10-23-claude-mcp-wishlist-fixes-check.md`,
against `a6eddae`) found six of them right and left three things open: one failure on real pages, one
oddity in the numbers, one nit. All three are done, and the first two were worse underneath than they
looked from outside.

| Item | Now |
|---|---|
| **1.** `read_pdf`'s `region` fell back to the whole page on an ordinary paper page: it paired the page's lines one-to-one with `QPdfSelection::bounds()`, and those are a box to a **text run**, not to a line - an italic word or a symbol splits one, so three lines came back as nine boxes | The boxes are grouped into lines first, by the line they sit on; where they still do not answer to the lines, the text **between the region's corners** is read, as a drag over it selects. The answer says which of the two it did |
| **2.** The meshes grew unevenly: ×2 gave 2.6 times the unknowns and ×4 gave 12.4 times more than ×2, where each halving should give about four | Each factor now divides the sizes the **model began with**, and the smallest element is divided with the rest. On `coax_rlgc.qfem` that is 2.89× then 3.13×, where it was 2.89× then 10.5× |
| **3.** A refusal inside `run_script` said the tool's name twice: "get_dataset: get_dataset: points is a whole number…" | Once |

## What each change is

**1, two ways to read a region** (`qucscontrol_design.cpp`). The check's diagnosis was exactly right, and
its first suggestion - `getSelection()` between the region's corners, the call the viewer's own drag
makes - is what the fallback now does. But it cannot be the only way: on a PDF written by hand, with no
widths for its font, `getSelection()` answers nothing at all while the boxes pair cleanly. So both are
here, in order:

1. the boxes grouped into lines (those whose vertical middles overlap by more than half a height are one
   box again), and where that gives one box to each line, the lines whose box meets the region;
2. else the text between the region's corners, whose ends come whole, so it may hold a little more than
   the region does;
3. else the whole page, said plainly.

On page 2 of the paper the check used, 201 boxes become 93 groups against 42 lines - grouping helps but
does not get there, because the runs are not in line order - so that page takes the second way and
returns the three lines the selection gave. The four-line probe PDF takes the first. The answer's
`"of the region"` says which, in each case.

**2, the sizes the model began with** (`qucscontrol_fem.cpp`). Measuring the uneven growth turned up a
worse bug than the one reported. `refineMesh()` restored the mesh node's own properties before each
refinement but took its **features** from the model as it then stood - which already held the previous
mesh's divided sizes. So the factors compounded: a Size feature of 0.05 went to 0.025 at ×2 and to
0.00625 at ×4, not 0.0125, and the last step was four times finer than asked rather than twice. Worse,
the finish put back only the node's properties, so a model was **left holding the finest mesh's local
sizes** after the call said it had put everything back.

The whole mesh node is now copied before the run, features and all, and that copy is what each
refinement divides and what the finish restores.

With the steps no longer compounding, the reported oddity itself remained: the smallest element is a
floor, and leaving it where it was holds the finer meshes back. It is divided with everything else now
(from whatever sets it - the preset's value or the node's own), which is what a convergence study wants:
one scaling of the whole size field. `coax_rlgc.qfem` at `[2, 4]` goes 81,324 → 234,636 → 735,531
unknowns, 2.89× then 3.13×, where before it was 2.89× then 10.5×.

**3, the tool's name once** (`qucscontrol_script.cpp`). A tool's refusal already begins with its own
name, and the script API added another. It adds one only where the text does not start with it.

## Tests

`test_mcp_wishlist`'s two FEM functions are extended; the suite is 17 functions.

| test | what it holds to |
|---|---|
| `aWeakRefinementShowsNothingAndAnEditIsChecked` | at `[2, 4]`, every size of the second mesh is exactly twice the third's - `hmax`, `hmin`, a Size feature's `hmax`, a Distribution's `count` - so the factors divide the model's own sizes and do not compound; `hmin` is the preset's divided, not the stale 4 mm the node still holds; and afterwards the Size feature is back at 0.05 and the Distribution at 12 |
| `aRegionKeepsThePdfsLinesAndOneLineEnding` | a four-line PDF whose first line is drawn in two goes takes the first way, grouping those two boxes into one line, and gives lines 1-2 and 3-4 for the two halves; a PDF Qt writes, with an italic word in a paragraph, cannot be paired box to line, and what comes back is of the page with the answer saying how it was read |

## Broken on purpose

`breaks.py` gains 7 breaks (R1-R3) and holds 81. Two older ones were re-aimed, since this round changed
the text they named.

Four of the new and older ones went uncaught on the first pass, and chasing them was worth more than the
breaks themselves:

- **A stale hidden field** could no longer be seen through `hmin`, because this round sets `hmin`
  explicitly every time. It is `growth` that bites now, and it bites hard: with the stale `1.01` applied,
  the refined mesh of the test's own fixture goes to 223,360 unknowns where it should go to 4,332. The
  test holds the growth between 2.2 and 10 times.
- **The boxes paired run by run** changed nothing, because every line of the probe PDF was one run. Its
  first line is now drawn in two goes, which is what a real page does with an italic word.
- **The tool named twice** had no assertion at all; there is one now.
- **Reading between two points** cannot be reached from a test at all: it answers nothing on a PDF
  written here, by hand or by Qt's own writer, and answers correctly on the real paper the re-check used
  (tried by hand). The break on it was dropped for one on the third way, which a generated PDF does
  reach. That the second way works rests on the re-check's own evidence and mine, not on the suite.

The 49 suites of the multiphysics side, the tools, the diagrams and the documents pass, with the two CI
lints.

## Not done

- **A region that is a column.** The second way reads in reading order between two points, so a narrow
  region over a table's column takes whole lines. The first way does hold to the rectangle, and the
  answer always says which way it took.
- **The second way under test.** Qt's reading between two points needs a font with real widths; PDFs
  generated in the suite do not get one, so only the first and third ways are exercised there.
- **Why the runs are not in line order.** Grouping gets 201 boxes down to 93 for 42 lines, so some runs
  of one line are not adjacent in what the module returns. Reading them in order of their position
  rather than their index might close that, and would make the precise way work on papers too.
